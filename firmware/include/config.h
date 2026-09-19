#pragma once
// Pines de la Freenove FNK0114-S (E32R40T). Fuente unica: ../../PINOUT.md
// Los pines del panel y del tactil NO estan aqui: los define platformio.ini
// via build_flags porque TFT_eSPI los necesita en tiempo de compilacion.

// --- LED RGB discreto, ANODO COMUN: LOW = encendido ---
#define PIN_LED_R 22
#define PIN_LED_G 16
#define PIN_LED_B 17
#define LED_ON  LOW
#define LED_OFF HIGH

// --- Boton BOOT: confirmacion fisica de permisos ---
#define PIN_BOOT 0   // INPUT_PULLUP, pulsado = LOW

// --- microSD, bus VSPI (compartido con el header SPI externo) ---
#define PIN_SD_CS   5
#define PIN_SD_SCK  18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23

// --- Audio ---
#define PIN_AUDIO_EN 4   // SHUTDOWN del SC8002B; polaridad PENDIENTE de comprobar

// --- Bateria ---
#define PIN_BAT_ADC 34   // input-only; divisor 1:2

// --- Pantalla, en apaisado (restriccion 8) ---
#define SCREEN_ROTATION 1
#define SCREEN_W        480
#define SCREEN_H        320

// Reparto de la pantalla
#define TOPBAR_H        26
#define AVATAR_W        180
#define ROWS_VISIBLE     4

// --- Red ---
#define WIFI_RETRY_MIN_MS   1000
#define WIFI_RETRY_MAX_MS  30000
#define MQTT_RETRY_MIN_MS   1000
#define MQTT_RETRY_MAX_MS  30000
#define NTP_SERVER        "pool.ntp.org"
#define TZ_SPAIN          "CET-1CEST,M3.5.0,M10.5.0/3"

// --- Presupuesto de memoria (CLAUDE.md) ---
#define HEAP_FLOOR_BYTES   (80 * 1024)
#define HEAP_LOG_PERIOD_MS 30000
