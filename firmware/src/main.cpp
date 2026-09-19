// claude-agent-display — fase 1: dashboard de solo lectura.
//
// Recalibrar el tactil: pulsar BOOT mas de 2 s (la placa reinicia).
// OJO: NO vale mantener BOOT durante el RESET. GPIO0 a LOW en el reset mete al
// ESP32 en modo descarga y el sketch ni arranca.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "agents.h"
#include "config.h"
#include "display.h"
#include "net.h"
#include "ui_agents.h"

static bool     uiDirty   = true;
static uint32_t lastSlow  = 0;
static uint32_t lastHeap  = 0;
static uint32_t bootDown  = 0;
static bool     lastBoot  = HIGH;

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
  Serial.println("=== claude-agent-display / fase 1 ===");
  Serial.printf("[hw ] chip=%s rev=%d  flash=%u MB  psram=%u B  sdk=%s  reset=%d\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize(),
                ESP.getSdkVersion(), (int)esp_reset_reason());
  Serial.printf("[hw ] sketch=%u B  libre=%u B\n", ESP.getSketchSize(), ESP.getFreeSketchSpace());
}

static void onNetChange() { uiDirty = true; }

/** El LED RGB repite el estado agregado: se ve desde lejos, sin leer nada. */
static void updateLed() {
  static AgentStatus last = ST_COUNT;
  static bool        phase = false;
  static uint32_t    nextPulse = 0;

  const AgentStatus agg = agentsAggregate();
  const uint32_t    now = millis();

  if (agg == ST_WAITING_PERMISSION) {
    if (now >= nextPulse) {                 // ambar pulsante: es lo unico que te reclama
      phase = !phase;
      nextPulse = now + 400;
      digitalWrite(PIN_LED_R, phase ? LED_ON : LED_OFF);
      digitalWrite(PIN_LED_G, phase ? LED_ON : LED_OFF);
      digitalWrite(PIN_LED_B, LED_OFF);
    }
    last = agg;
    return;
  }

  if (agg == last) return;
  last = agg;

  digitalWrite(PIN_LED_R, agg == ST_ERROR   ? LED_ON : LED_OFF);
  digitalWrite(PIN_LED_G, agg == ST_WORKING ? LED_ON : LED_OFF);
  digitalWrite(PIN_LED_B, LED_OFF);
}

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

  displayInit(bootHeld);
  logMem("tras LVGL");

  uiAgentsCreate();
  logMem("tras crear UI");

  netOnChange(onNetChange);
  netInit();

  randomSeed(esp_random());
  Serial.println("[ok ] setup completo");
}

void loop() {
  const uint32_t now = millis();

  netTick();

  if (uiDirty) {
    uiDirty = false;
    uiAgentsRefresh();
  }

  if (now - lastSlow >= 250) {      // cronometros, parpadeo, reloj
    lastSlow = now;
    uiAgentsTickSlow();
    updateLed();
  }

  // BOOT largo -> recalibrar el tactil
  const bool boot = digitalRead(PIN_BOOT);
  if (boot != lastBoot) {
    lastBoot = boot;
    if (boot == LOW) bootDown = now;
  }
  if (boot == LOW && bootDown && now - bootDown > 2000) {
    Serial.println("[cal] BOOT largo -> recalibrar");
    displayEraseCalibration();
    ESP.restart();
  }

  if (now - lastHeap >= HEAP_LOG_PERIOD_MS) {
    lastHeap = now;
    logMem("periodico");
  }

  const uint32_t nextMs = displayTick();
  delay(nextMs > 10 ? 5 : 1);       // LVGL dice cuando le hace falta la CPU
}
