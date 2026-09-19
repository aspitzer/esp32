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

void netOnChange(void (*cb)()) { changeCb = cb; }
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

  strlcpy(a->label, doc["label"] | id, AG_LABEL_LEN);
  a->status = agentStatusFromName(doc["status"] | "idle");
  strlcpy(a->tool,      doc["tool"]      | "", AG_TOOL_LEN);
  strlcpy(a->detail,    doc["detail"]    | "", AG_DETAIL_LEN);
  strlcpy(a->lastError, doc["lastError"] | "", AG_ERROR_LEN);
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
