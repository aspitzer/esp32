#include "display.h"

#include <Preferences.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#include "config.h"

static TFT_eSPI    tft = TFT_eSPI();
static Preferences prefs;

// Buffer parcial, NO framebuffer: 320x480x2 = 307 KB y no hay PSRAM.
//
// UN solo buffer de 480x40 = 37,5 KB. El segundo buffer de LVGL solo sirve si
// el flush es asincrono; aqui pushPixels es bloqueante, asi que LVGL esperaria
// igual y el segundo buffer seria 37,5 KB tirados. Cuando pasemos a
// pushPixelsDMA habra que anadirlo.
//
// Va en el heap, no en .bss: la DRAM estatica del ESP32 no admite esto mas el
// pool de LVGL (desborda dram0_0_seg). Se reserva UNA vez en el arranque,
// antes de levantar WiFi, y no se libera nunca: cumple la regla de CLAUDE.md
// de no reservar en caliente.
// OJO: en LVGL 9 lv_color_t son 3 bytes (RGB888) aunque LV_COLOR_DEPTH sea 16 y
// el buffer de render sea RGB565. sizeof(lv_color_t) NO sirve para dimensionar:
// hay que contar bytes a mano.
#define BUF_LINES 40
#define BUF_BYTES (SCREEN_W * BUF_LINES * (LV_COLOR_DEPTH / 8))
static uint8_t *drawBuf = nullptr;

// --- calibracion del tactil, persistida en NVS -------------------------------
#define NVS_NAMESPACE "cad"
#define NVS_KEY_CAL   "touchcal"
#define CAL_MAGIC     0xCA11u

struct TouchCal {
  uint16_t magic;
  uint16_t data[5];
};
static TouchCal cal;

static bool calLoad() {
  prefs.begin(NVS_NAMESPACE, true);
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
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBytes(NVS_KEY_CAL, &cal, sizeof(cal));
  prefs.end();
  Serial.printf("[cal] guardada: { %u, %u, %u, %u, %u }\n",
                cal.data[0], cal.data[1], cal.data[2], cal.data[3], cal.data[4]);
}

void displayEraseCalibration() {
  prefs.begin(NVS_NAMESPACE, false);
  if (prefs.isKey(NVS_KEY_CAL)) prefs.remove(NVS_KEY_CAL);
  prefs.end();
}

static void calRun() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Calibracion del tactil", 20, 20, 4);
  tft.drawString("Toca la punta de cada flecha con el stylus.", 20, 56, 2);
  tft.calibrateTouch(cal.data, TFT_MAGENTA, TFT_BLACK, 15);
  calSave();
}

// --- enganche de LVGL --------------------------------------------------------

// Un refresco se parte en varios flushes (el buffer son 40 lineas de 320).
// Solo el ultimo cierra un frame: contar flushes daria un fps inflado.
static uint32_t frames = 0, chunks = 0, pixels = 0, spiUs = 0;

static void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  const uint32_t t0 = micros();

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixels(reinterpret_cast<uint16_t *>(px_map), w * h);
  tft.endWrite();

  spiUs  += micros() - t0;
  pixels += w * h;
  chunks++;
  if (lv_display_flush_is_last(disp)) frames++;
  lv_display_flush_ready(disp);
}

uint32_t displayFramesAndReset() {
  const uint32_t f = frames;
  frames = 0;
  return f;
}

void displayRenderStats(uint32_t *outChunks, uint32_t *outPixels, uint32_t *outSpiMs) {
  *outChunks = chunks;  *outPixels = pixels;  *outSpiMs = spiUs / 1000;
  chunks = 0; pixels = 0; spiUs = 0;
}

static void touchCb(lv_indev_t *, lv_indev_data_t *data) {
  uint16_t x, y;
  // getTouch hace una transaccion SPI en el mismo bus que el panel. LVGL solo
  // llama aqui entre flushes, nunca durante uno, asi que no hay colision.
  if (tft.getTouch(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static uint32_t tickCb() { return millis(); }

void displayBacklight(bool on) {
  digitalWrite(TFT_BL, on ? TFT_BACKLIGHT_ON : !TFT_BACKLIGHT_ON);
}

void displayInit(bool forceCalibration) {
  pinMode(TFT_BL, OUTPUT);
  displayBacklight(true);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.setSwapBytes(true);        // LVGL entrega RGB565 nativo; TFT_eSPI lo quiere al reves
  tft.fillScreen(TFT_BLACK);

  if (forceCalibration) displayEraseCalibration();
  if (forceCalibration || !calLoad()) calRun();
  tft.setTouch(cal.data);

  lv_init();
  lv_tick_set_cb(tickCb);

  const size_t bufBytes = BUF_BYTES;
  drawBuf = (uint8_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!drawBuf) {
    Serial.printf("[lvgl] FATAL: no hay %u B contiguos para el buffer\n", (unsigned)bufBytes);
    while (true) delay(1000);
  }

  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, drawBuf, nullptr, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchCb);

  Serial.printf("[lvgl] %dx%d  buffer parcial de %d lineas = %u B (%u KB) en heap DMA\n",
                SCREEN_W, SCREEN_H, BUF_LINES, (unsigned)bufBytes, (unsigned)(bufBytes / 1024));
}

uint32_t displayTick() { return lv_timer_handler(); }
