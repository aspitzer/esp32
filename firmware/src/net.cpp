#include "net.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>

#include "agents.h"
#include "config.h"
#include "secrets.h"

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static NetState state = NET_WIFI_DOWN;
static void (*changeCb)() = nullptr;

static uint32_t wifiNextTry = 0, wifiBackoff = WIFI_RETRY_MIN_MS;
static uint32_t mqttNextTry = 0, mqttBackoff = MQTT_RETRY_MIN_MS;
static uint8_t  mqttAttempt = 0;
static bool     ntpStarted  = false;

static char lwtTopic[64];

static PermReq  perm = {};
static void (*permCb)() = nullptr;
static char actionResult[64] = {0};

void netOnChange(void (*cb)()) { changeCb = cb; }
void netOnPermission(void (*cb)()) { permCb = cb; }

const PermReq *netPendingPerm() { return perm.active ? &perm : nullptr; }
void netDropPerm() { perm.active = false; }

const char *netLastActionResult() { return actionResult; }
void netClearActionResult() { actionResult[0] = '\0'; }
NetState netState() { return state; }
int8_t   netRssi() { return WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0; }
uint8_t  netMqttAttempt() { return mqttAttempt; }
const char *netIpMqtt() { return MQTT_HOST; }

uint32_t netNextRetryIn() {
  const uint32_t now = millis();
  const uint32_t t   = (state == NET_WIFI_DOWN) ? wifiNextTry : mqttNextTry;
  return t > now ? t - now : 0;
}

static void notifyChange() { if (changeCb) changeCb(); }

static void setState(NetState s) {
  if (s == state) return;
  state = s;
  notifyChange();
}

// --- mensajes entrantes ------------------------------------------------------

static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  if (strcmp(topic, "claude/perm/req") == 0) {
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, payload, len)) {
      Serial.println("[perm] JSON invalido");
      return;
    }
    memset(&perm, 0, sizeof(perm));
    strlcpy(perm.requestId, doc["requestId"] | "", PERM_ID_LEN);
    strlcpy(perm.agentId,   doc["agentId"]   | "", sizeof(perm.agentId));
    strlcpy(perm.tool,      doc["tool"]      | "", sizeof(perm.tool));
    strlcpy(perm.summary,   doc["summary"]   | "", PERM_SUMMARY_LEN);
    const char *r = doc["risk"] | "high";
    perm.risk      = !strcmp(r, "low") ? RISK_LOW : (!strcmp(r, "medium") ? RISK_MEDIUM : RISK_HIGH);
    perm.expiresAt = doc["expiresAt"] | 0;
    perm.rxMillis  = millis();
    perm.active    = perm.requestId[0] != '\0';
    Serial.printf("[perm] %s %s riesgo=%d: %s\n", perm.agentId, perm.tool, perm.risk, perm.summary);
    if (perm.active && permCb) permCb();
    return;
  }

  if (strcmp(topic, "claude/action/res") == 0) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload, len)) return;
    const bool ok = doc["ok"] | false;
    snprintf(actionResult, sizeof(actionResult), "%s %s - %s",
             ok ? "OK" : "FALLO", (const char *)(doc["via"] | "?"),
             (const char *)(doc["detail"] | ""));
    Serial.printf("[act ] %s\n", actionResult);
    notifyChange();
    return;
  }

  // claude/agents/<id>/state
  const char *p = strstr(topic, "claude/agents/");
  if (!p) return;
  p += strlen("claude/agents/");

  char id[AG_ID_LEN] = {0};
  for (uint8_t i = 0; i < AG_ID_LEN - 1 && p[i] && p[i] != '/'; i++) id[i] = p[i];
  if (!id[0]) return;

  // Payload vacio = retained borrado: el agente ya no existe.
  if (len == 0) {
    agentRemove(id);
    Serial.printf("[mqtt] %s borrado\n", id);
    notifyChange();
    return;
  }

  // Medido en el broker: 157 B como maximo. 384 es holgura de sobra y es fijo.
  StaticJsonDocument<384> doc;
  const DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("[mqtt] JSON invalido en %s: %s\n", topic, err.c_str());
    return;
  }

  Agent *a = agentSlot(id);
  if (!a) { Serial.printf("[mqtt] sin slot libre para %s\n", id); return; }

  {
    const char *lab = doc["label"] | id;
    if (strncmp(a->label, lab, AG_LABEL_LEN) != 0)
      Serial.printf("[lbl] %s -> \"%s\" (%u chars)\n", id, lab, (unsigned)strlen(lab));
    strlcpy(a->label, lab, AG_LABEL_LEN);
  }
  a->status = agentStatusFromName(doc["status"] | "idle");
  strlcpy(a->tool,      doc["tool"]      | "", AG_TOOL_LEN);
  strlcpy(a->detail,    doc["detail"]    | "", AG_DETAIL_LEN);
  strlcpy(a->raw,       doc["raw"]       | "", AG_DETAIL_LEN);
  strlcpy(a->lastError, doc["lastError"] | "", AG_ERROR_LEN);
  strlcpy(a->subType,   doc["subType"]   | "", AG_SUBTYPE_LEN);
  a->subN     = doc["subN"] | 0;
  a->since    = doc["since"] | 0;
  a->rxMillis = millis();

  notifyChange();
}

// --- conexion ----------------------------------------------------------------

static void mqttConnect() {
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "%s-%04X", DEVICE_ID, (uint16_t)(ESP.getEfuseMac() & 0xFFFF));

  // LWT retained: si la placa se va, el broker lo sabe y deja de enrutarle
  // permisos. Es lo que hace seguro el fallback del lado del Mac.
  const bool up = mqtt.connect(clientId, MQTT_USER, MQTT_PASS, lwtTopic, 1, true, "0");

  if (!up) {
    mqttAttempt++;
    mqttBackoff = min<uint32_t>(mqttBackoff * 2, MQTT_RETRY_MAX_MS);
    mqttNextTry = millis() + mqttBackoff;
    Serial.printf("[mqtt] fallo rc=%d (intento %u), reintento en %u ms\n",
                  mqtt.state(), mqttAttempt, mqttBackoff);
    notifyChange();
    return;
  }

  mqttAttempt = 0;
  mqttBackoff = MQTT_RETRY_MIN_MS;
  mqtt.publish(lwtTopic, "1", true);
  mqtt.subscribe("claude/agents/+/state", 1);
  mqtt.subscribe("claude/perm/req", 1);
  mqtt.subscribe("claude/action/res", 1);
  Serial.printf("[mqtt] conectado a %s:%d como %s\n", MQTT_HOST, MQTT_PORT, clientId);
  setState(NET_MQTT_UP);
}

void netInit() {
  snprintf(lwtTopic, sizeof(lwtTopic), "claude/device/%s/online", DEVICE_ID);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(true);           // ahorra ~20 mA y no afecta a la latencia de MQTT

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(MQTT_MAX_PACKET_SIZE);
  mqtt.setKeepAlive(MQTT_KEEPALIVE);

  wifiNextTry = 0;
}

void netAnswerPerm(bool allow) {
  if (!perm.active) return;

  StaticJsonDocument<192> doc;
  doc["requestId"] = perm.requestId;
  doc["decision"]  = allow ? "allow" : "deny";
  doc["source"]    = "device";

  char buf[192];
  const size_t n = serializeJson(doc, buf, sizeof(buf));
  const bool sent = mqtt.publish("claude/perm/res", (const uint8_t *)buf, n, false);

  Serial.printf("[perm] %s -> %s (%s)\n", perm.requestId, allow ? "allow" : "deny",
                sent ? "enviado" : "FALLO AL ENVIAR");
  perm.active = false;
  notifyChange();
}

void netSendAction(const char *agentId, const char *action) {
  StaticJsonDocument<160> doc;
  doc["agentId"] = agentId;
  doc["action"]  = action;
  doc["source"]  = "device";

  char buf[160];
  const size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish("claude/action/req", (const uint8_t *)buf, n, false);
  snprintf(actionResult, sizeof(actionResult), "enviando %s...", action);
  Serial.printf("[act ] %s -> %s\n", agentId, action);
}

void netTick() {
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (state != NET_WIFI_DOWN) {
      Serial.println("[wifi] caido");
      setState(NET_WIFI_DOWN);
    }
    if (now >= wifiNextTry) {
      Serial.printf("[wifi] conectando a \"%s\"\n", WIFI_SSID);
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifiBackoff = min<uint32_t>(wifiBackoff * 2, WIFI_RETRY_MAX_MS);
      wifiNextTry = now + wifiBackoff;
    }
    return;
  }

  if (state == NET_WIFI_DOWN) {
    wifiBackoff = WIFI_RETRY_MIN_MS;
    Serial.printf("[wifi] conectado  ip=%s  rssi=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    setState(NET_WIFI_UP);
  }

  if (!ntpStarted) {
    configTzTime(TZ_SPAIN, NTP_SERVER);
    ntpStarted = true;
  }

  if (!mqtt.connected()) {
    if (state == NET_MQTT_UP) {
      Serial.println("[mqtt] desconectado");
      setState(NET_WIFI_UP);
    }
    if (now >= mqttNextTry) mqttConnect();
    return;
  }

  mqtt.loop();
}
