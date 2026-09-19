// Fase 0 — reconocimiento de hardware.
//
// 1. Arranca el panel, saca por serial chip / flash / PSRAM / heap.
// 2. Calibra el tactil y persiste la calibracion en NVS.
// 3. Prueba las cuatro esquinas: hay que tocar los cuatro objetivos.
//
// Recalibrar: pulsar BOOT mas de 2 s con el sketch corriendo (la placa reinicia).
// OJO: NO vale mantener BOOT durante el reset. GPIO0 a LOW en el reset mete al
// ESP32 en modo descarga y el sketch ni arranca. La ventana alternativa es
// pulsarlo en el primer medio segundo DESPUES de soltar RESET.

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "config.h"

#if __has_include("secrets.h")
  #include "secrets.h"
  #define HAVE_SECRETS 1
#endif

static TFT_eSPI     tft = TFT_eSPI();
static Preferences  prefs;

// --- calibracion del tactil ---------------------------------------------------
#define NVS_NAMESPACE  "cad"
#define NVS_KEY_CAL    "touchcal"
#define CAL_MAGIC      0xCA11u   // sube esto si cambia el formato o la rotacion

struct TouchCal {
  uint16_t magic;
  uint16_t data[5];
};

static TouchCal cal;

// --- objetivos de las cuatro esquinas -----------------------------------------
#define TARGET_R 22
#define TARGET_M 26   // margen desde el borde

struct Target { int16_t x, y; bool hit; };
static Target targets[4];
static uint8_t targetsHit = 0;

// ------------------------------------------------------------------------------

static void logMem(const char *tag) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[mem] %-16s free=%7u B (%3u KB)  largest=%7u B (%3u KB)  minEver=%7u B%s\n",
                tag, freeHeap, freeHeap / 1024, largest, largest / 1024,
                ESP.getMinFreeHeap(),
                largest < HEAP_FLOOR_BYTES ? "  <-- largest POR DEBAJO DEL SUELO" : "");
}

static void logBoard() {
  Serial.println();
  Serial.println("=== claude-agent-display / fase 0 ===");
  Serial.printf("[hw ] chip=%s rev=%d cores=%d cpu=%u MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("[hw ] flash chip=%u B (%u MB)  flash speed=%u Hz\n",
                ESP.getFlashChipSize(), ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getFlashChipSpeed());
  Serial.printf("[hw ] sketch=%u B  libre para OTA=%u B\n",
                ESP.getSketchSize(), ESP.getFreeSketchSpace());
  Serial.printf("[hw ] sdk=%s  reset=%d\n", ESP.getSdkVersion(), (int)esp_reset_reason());
  Serial.printf("[hw ] psram=%u B (se espera 0)\n", ESP.getPsramSize());
  Serial.printf("[tft] driver=ST7796  %dx%d  MISO=%d MOSI=%d SCLK=%d CS=%d DC=%d BL=%d TOUCH_CS=%d\n",
                TFT_WIDTH, TFT_HEIGHT, TFT_MISO, TFT_MOSI, TFT_SCLK,
                TFT_CS, TFT_DC, TFT_BL, TOUCH_CS);
}

static bool calLoad() {
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  const size_t n = prefs.getBytes(NVS_KEY_CAL, &cal, sizeof(cal));
  prefs.end();

  if (n != sizeof(cal) || cal.magic != CAL_MAGIC) {
    Serial.printf("[cal] sin calibracion valida en NVS (leidos %u B)\n", (unsigned)n);
    return false;
  }
  Serial.printf("[cal] cargada de NVS: { %u, %u, %u, %u, %u }\n",
                cal.data[0], cal.data[1], cal.data[2], cal.data[3], cal.data[4]);
  return true;
}

static void calSave() {
  cal.magic = CAL_MAGIC;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  const size_t n = prefs.putBytes(NVS_KEY_CAL, &cal, sizeof(cal));
  prefs.end();
  Serial.printf("[cal] guardada en NVS (%u B): { %u, %u, %u, %u, %u }\n",
                (unsigned)n, cal.data[0], cal.data[1], cal.data[2], cal.data[3], cal.data[4]);
}

static void calErase() {
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  if (prefs.isKey(NVS_KEY_CAL)) {          // evita el "nvs_erase_key fail: NOT_FOUND"
    prefs.remove(NVS_KEY_CAL);
    Serial.println("[cal] borrada de NVS");
  } else {
    Serial.println("[cal] no habia nada que borrar en NVS");
  }
  prefs.end();
}

static void calRun() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Calibracion del tactil", 20, 20, 4);
  tft.drawString("Toca la punta de cada flecha con el stylus.", 20, 56, 2);
  tft.drawString("El tactil es resistivo: presiona, no roces.", 20, 74, 2);

  tft.calibrateTouch(cal.data, TFT_MAGENTA, TFT_BLACK, 15);
  calSave();
}

// --- WiFi: solo para medir cuanto heap se come (criterio de fase 0) -----------

static void wifiProbe() {
#ifndef HAVE_SECRETS
  Serial.println("[net] sin secrets.h -> no se mide el heap con WiFi");
#else
  if (strcmp(WIFI_SSID, "PON-AQUI-TU-SSID") == 0) {
    Serial.println("[net] secrets.h sin rellenar -> no se mide el heap con WiFi");
    return;
  }

  logMem("antes de WiFi");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  logMem("WiFi.mode(STA)");

  Serial.printf("[net] conectando a \"%s\" ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[net] NO conecta (status=%d) tras %u ms\n",
                  (int)WiFi.status(), (unsigned)(millis() - t0));
    logMem("WiFi fallido");
    return;
  }

  Serial.printf("[net] conectado en %u ms  ip=%s  rssi=%d dBm  canal=%d  mac=%s\n",
                (unsigned)(millis() - t0), WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel(), WiFi.macAddress().c_str());
  logMem("WiFi conectado");
#endif
}

// ------------------------------------------------------------------------------

static void targetsInit() {
  const int16_t w = tft.width(), h = tft.height();
  targets[0] = { (int16_t)TARGET_M,          (int16_t)TARGET_M,          false };
  targets[1] = { (int16_t)(w - TARGET_M),    (int16_t)TARGET_M,          false };
  targets[2] = { (int16_t)TARGET_M,          (int16_t)(h - TARGET_M),    false };
  targets[3] = { (int16_t)(w - TARGET_M),    (int16_t)(h - TARGET_M),    false };
  targetsHit = 0;
}

static void drawTarget(const Target &t) {
  const uint16_t c = t.hit ? TFT_GREEN : TFT_DARKGREY;
  tft.fillCircle(t.x, t.y, TARGET_R, c);
  tft.fillCircle(t.x, t.y, TARGET_R / 2, TFT_BLACK);
}

static void drawTestScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Toca los cuatro circulos", tft.width() / 2, 92, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("BOOT 2 s = recalibrar", tft.width() / 2, 210, 2);
  for (uint8_t i = 0; i < 4; i++) drawTarget(targets[i]);
}

static void drawReadout(uint16_t x, uint16_t y) {
  static char buf[40];
  snprintf(buf, sizeof(buf), "x=%4u  y=%4u  %u/4", x, y, targetsHit);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(buf, tft.width() / 2, 140, 4);
}

static void handleTouch(uint16_t x, uint16_t y) {
  static uint32_t lastLog = 0;
  const uint32_t now = millis();
  if (now - lastLog > 150) {           // no inundar el serial
    lastLog = now;
    Serial.printf("[tch] x=%u y=%u\n", x, y);
  }

  drawReadout(x, y);
  tft.fillCircle(x, y, 2, TFT_CYAN);

  for (uint8_t i = 0; i < 4; i++) {
    if (targets[i].hit) continue;
    const int32_t dx = (int32_t)x - targets[i].x;
    const int32_t dy = (int32_t)y - targets[i].y;
    if (dx * dx + dy * dy <= (int32_t)TARGET_R * TARGET_R) {
      targets[i].hit = true;
      targetsHit++;
      drawTarget(targets[i]);
      Serial.printf("[tch] esquina %u OK (%u/4)\n", i, targetsHit);

      if (targetsHit == 4) {
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString("CALIBRACION OK", tft.width() / 2, 172, 4);
        Serial.println("[cal] las cuatro esquinas responden. Fase 0 cerrada.");
      }
    }
  }
}

// ------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  const bool bootHeld = (digitalRead(PIN_BOOT) == LOW);

  logBoard();
  logMem("arranque");

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  digitalWrite(PIN_LED_R, LED_OFF);
  digitalWrite(PIN_LED_G, LED_OFF);
  digitalWrite(PIN_LED_B, LED_OFF);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setRotation(1);                  // apaisado, 480x320
  logMem("tras tft.init");
  Serial.printf("[tft] rotation=1 -> %dx%d\n", tft.width(), tft.height());

  if (bootHeld) {
    Serial.println("[cal] BOOT pulsado justo tras el arranque -> recalibrar");
    calErase();
  }

  if (bootHeld || !calLoad()) {
    calRun();
  }
  tft.setTouch(cal.data);
  logMem("tras calibrar");

  targetsInit();
  drawTestScreen();
  logMem("tras pintar");

  wifiProbe();
  logMem("fin de setup");

  digitalWrite(PIN_LED_G, LED_ON);
  Serial.println("[ok ] setup completo. Toca las cuatro esquinas.");
}

void loop() {
  static uint32_t lastLog   = 0;
  static uint32_t bootDown  = 0;
  static bool     lastBoot  = HIGH;

  uint16_t tx, ty;
  if (tft.getTouch(&tx, &ty)) handleTouch(tx, ty);

  const bool boot = digitalRead(PIN_BOOT);
  if (boot != lastBoot) {
    lastBoot = boot;
    if (boot == LOW) {
      bootDown = millis();
      digitalWrite(PIN_LED_B, LED_ON);
    } else {
      digitalWrite(PIN_LED_B, LED_OFF);
      Serial.printf("[key] BOOT soltado tras %u ms\n", (unsigned)(millis() - bootDown));
    }
  }
  if (boot == LOW && bootDown && millis() - bootDown > 2000) {
    Serial.println("[cal] BOOT largo -> recalibrar");
    calErase();
    ESP.restart();
  }

  const uint32_t now = millis();
  if (now - lastLog >= HEAP_LOG_PERIOD_MS) {
    lastLog = now;
    logMem("periodico");
  }
}
