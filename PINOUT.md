# PINOUT — Freenove FNK0114-S (4.0", 320x480, ST7796, táctil resistivo)

Placa: **E32R40T** (serigrafía del PCB) / referencia comercial **FNK0114-S**.
Módulo: **ESP32-WROOM-32E**, sin PSRAM.

**Verificado en la placa real (2026-09-19, serial a 115200):**
`ESP32-D0WD-V3 rev 3.1`, 2 cores, 240 MHz · flash **4 MB** (4194304 B) @ 80 MHz ·
**PSRAM = 0 B** · `rotation 1` → 480x320 · orden de color **RGB correcto**
(barras roja/verde/azul en orden, sin inversión ni desplazamiento) ·
BOOT en GPIO0 responde · CH340 `1A86:7523` → `/dev/cu.usbserial-31110`.

Táctil calibrado y persistido en NVS (`cad`/`touchcal`): `{ 315, 3592, 233, 3558, 7 }`
para `rotation 1`. Sobrevive a un reflasheo. Las cuatro esquinas responden con un
error máximo de **12 px** en los vértices, casi cero en el centro.

### Presupuesto de memoria medido en placa

| Momento | `free` | `largest_free_block` |
|---|---|---|
| Arranque (sin WiFi en el binario) | 342 KB | 111 KB |
| Arranque (binario con WiFi) | 302 KB | 107 KB |
| Tras `WiFi.mode(STA)` | 254 KB | 107 KB |
| **WiFi asociado** | **253 KB** | **107 KB** |

WiFi cuesta ~50 KB de heap, casi todo al levantar el driver, no al asociarse.
**`largest` no se mueve**: el stack de WiFi tira de regiones distintas y deja
intacto el bloque contiguo grande. El número que manda para una asignación única
es el `largest`, no el `free`.

Conexión en 799 ms, RSSI -51 dBm. Sin deriva de heap en 2+ min de bucle de render.

### Flash

Binario con WiFi: 790 KB. La partición de app por defecto (dos ranuras OTA) son
**1,25 MB de los 4 MB**, o sea ya al 60%. LVGL se comerá 200-300 KB más. Antes de
fase 3 hay que pasar a `huge_app.csv` (3 MB de app, sin OTA) o a un particionado
propio.

> **Regla**: los pines se leen de este fichero. No se copian de un "ESP32 CYD"
> genérico de internet ni se deducen. Si algo no está aquí, se verifica antes de usarlo.

## Fuentes

| # | Fuente | Qué aporta |
|---|---|---|
| F1 | `Freenove/Freenove_ESP32_Display` → `Libraries/FNK0114S_4.0inch_ST7796/TFT_eSPI_Setups_v1.4.zip` → `FNK0114S_4.0_320x480_ST7796.h` | Pines panel + táctil, driver, frecuencias SPI |
| F2 | Mismo repo → `Sketches/Sketch_02.1_RGB`, `03.1_Button_RGB`, `05.1_Battery_Voltage`, `06.1_SD_Test`, `07.1_Play_MP3_SD_by_DAC` | RGB, BOOT, batería, SD, audio |
| F3 | Mismo repo → `Datasheet/4.0inch_ST7796_V1.0/Schematic/4.0inch_ESP32-32E_E32R40T_E32N40T_Schematic.pdf` | Esquemático (extracción de texto; confirma netlist y chips) |
| F4 | Serigrafía del PCB y etiqueta (briefing del proyecto) | Headers externos |

Estado de cada pin:
- `DOCS` = aparece en código o esquemático oficial de Freenove **para esta variante**.
- `PLACA` = pendiente de comprobar en la placa real.

---

## Panel TFT — ST7796, SPI, bus **HSPI**

| Señal | GPIO | Estado | Fuente |
|---|---|---|---|
| `TFT_MISO` | **12** | DOCS | F1, F3 (net `LCD_MISO` → pin 14 = IO12) |
| `TFT_MOSI` | **13** | DOCS | F1, F3 (`LCD_MOSI`) |
| `TFT_SCLK` | **14** | DOCS | F1, F3 (`LCD_SCK` → pin 13 = IO14) |
| `TFT_CS` | **15** | DOCS | F1, F3 (`LCD_CS`) |
| `TFT_DC` (RS) | **2** | DOCS | F1, F3 (`LCD_RS`) |
| `TFT_RST` | **-1** (atado al RESET de la placa) | DOCS | F1 |
| `TFT_BL` (backlight) | **27** | DOCS | F1 (`#define TFT_BL 27`, `TFT_BACKLIGHT_ON HIGH`) |

- Driver: `ST7796_DRIVER`. Resolución nativa 320(W) x 480(H); apaisado = `rotation 1` o `3`.
- `SPI_FREQUENCY 80000000`, `SPI_READ_FREQUENCY 20000000`.
- `USE_HSPI_PORT` — el panel **no** va en VSPI.
- Backlight activo a **HIGH**. En el esquemático el net `LCD_BL` ataca un MOSFET (Q4 BSS138) hacia `LEDK`.

## Táctil — XPT2046 (resistivo, 1 punto)

| Señal | GPIO | Estado | Fuente |
|---|---|---|---|
| `TOUCH_CS` (T_CS / `RTP_CS`) | **33** | DOCS | F1 (`#define TOUCH_CS 33`) |
| T_CLK / T_DIN / T_DO | **comparte 14 / 13 / 12 con el panel** | DOCS | F1 (TFT_eSPI gestiona el táctil en el mismo bus HSPI) |
| `RTP_IRQ` (PENIRQ) | **sin conectar al ESP32** (aparentemente) | PLACA | F3 — el net existe en el bloque del XPT2046 pero no se ha podido confirmar su destino |

- `SPI_TOUCH_FREQUENCY 2500000`.
- **Importante**: en las variantes de 2.4"/2.8" de Freenove el táctil va en un bus
  bit-bang aparte (39/32/33/25) con la librería `TFT_Touch`. **En la de 4.0" NO**:
  `TOUCH_CS 33` está activo en el setup y el sketch de calibración usa
  `tft.calibrateTouch()` de TFT_eSPI. Esto encaja con que IO25/IO32/IO35/IO39
  estén libres y sacados a headers en esta placa. No mezclar los dos esquemas.
- Calibración de ejemplo del repo (3.5"/4.0", `rotation 1`): `{286, 3534, 283, 3600, 6}`.
  Es un valor **de referencia**, hay que recalibrar y persistir en NVS.

## microSD — SPI, bus **VSPI**

| Señal | GPIO | Estado | Fuente |
|---|---|---|---|
| `SD_CS` | **5** | DOCS | F2 (`SD_VSPI_SS 5`) |
| `SD_SCK` | **18** | DOCS | F2 |
| `SD_MISO` | **19** | DOCS | F2 |
| `SD_MOSI` | **23** | DOCS | F2 |

El bus VSPI es el mismo que sale al header SPI externo (nets `SPI_CLK`, `SPI_MISO`,
`SPI_MOSI`, `SPI_CS`): SD y periférico externo comparten bus, distinto CS.

## LED RGB — discreto (LED2, MHP5050RGBDT), **activo a LOW**

| Color | GPIO | Estado | Fuente |
|---|---|---|---|
| Rojo | **22** | DOCS | F2 |
| Verde | **16** | DOCS | F2 |
| Azul | **17** | DOCS | F2 |

No es WS2812. Ánodo común a 3V3 con R14/R15/R16 = 1K, cátodos a los GPIO:
`digitalWrite(pin, LOW)` enciende. Usar `ledcWrite` invertido para PWM.

## Botones

| Botón | GPIO | Estado | Fuente |
|---|---|---|---|
| BOOT (KEY2) | **0** | DOCS | F2 (`#define KEY_PIN 0`, `INPUT_PULLUP`) |
| RESET (KEY1) | pin EN del módulo | DOCS | F3 |

BOOT es el botón físico de confirmación de permisos (restricción 4 del proyecto).

## Audio — amplificador SC8002B + DAC interno

| Señal | GPIO | Estado | Fuente |
|---|---|---|---|
| `AUDIO_EN` (SHUTDOWN del ampli) | **4** | DOCS | F2 (`#define AUDIO_EN 4`) |
| `AUDIO_IN` (salida DAC) | **26** (DAC2) — probable | PLACA | F3 + F2 (`AudioOutputI2S(0, 1)` = modo DAC interno) |

`AudioOutputI2S(0, 1)` pone el ESP32 en modo DAC interno, que saca señal por
**GPIO25 (DAC1) y GPIO26 (DAC2)** a la vez. Solo uno llega al ampli.
Los sketches oficiales hacen `digitalWrite(AUDIO_EN, LOW)` en el `setup()`.

## Batería

| Señal | GPIO | Estado | Fuente |
|---|---|---|---|
| `BAT_ADC` | **34** (input-only) | DOCS | F2, F3 (net `BAT_ADC` → pin 6 = IO34) |

Divisor 1:2 → `analogReadMilliVolts(34) * 2.0 / 1000` = voltios. Cargador TP4054.

## Headers externos (serigrafía)

| Header | Señal | GPIO | Estado |
|---|---|---|---|
| SPI | MOSI / MISO / SCK / CS | 23 / 19 / 18 / 21 | DOCS (F4 + F3, nets `SPI_*`) |
| I2C | SDA / SCL | 32 / 25 | PLACA (F4; nets `IIC_SDA`/`IIC_SCL` existen en F3, asignación a GPIO no confirmada por extracción) |
| Extra | — | 35, 39 | DOCS (F3: nets `IO35`, `IO39` sueltos desde pines 7 y 5) |

## Conflictos y avisos

1. **IO25 = SCL del header I2C y DAC1 del ESP32.** Si se usa audio en modo DAC
   interno, el DAC escribe en GPIO25 aunque el ampli cuelgue de GPIO26. No usar
   audio e I2C a la vez sin comprobarlo en la placa.
2. **IO35 e IO39 son input-only** y sin pull-up interno. Nunca como salida ni como
   botón sin resistencia externa.
3. **IO12 es strapping pin (MTDI)** del ESP32 y aquí es `TFT_MISO`. Si el panel tira
   de esa línea en el arranque puede cambiar el voltaje del flash. Con el eFuse de
   fábrica de los WROOM-32E no suele dar problema, pero si hay arranques erráticos,
   sospechar de aquí primero.
4. **IO0 (BOOT)** no puede estar a LOW en el reset o la placa entra en modo descarga.
5. Panel y táctil comparten HSPI: toda transacción del táctil interrumpe el DMA del
   panel. LVGL debe leer el táctil entre flushes, no durante.

## Pendiente de comprobar en la placa real

- [ ] `RTP_IRQ` (PENIRQ del XPT2046): ¿conectado a algún GPIO o al aire?
- [ ] `AUDIO_IN`: ¿GPIO26 o GPIO25?
- [ ] Header I2C: confirmar SDA=32 / SCL=25 con multímetro.
- [ ] Polaridad real de `AUDIO_EN` (LOW = enable o = shutdown).
- [x] ~~Tamaño de flash real~~ → 4 MB, confirmado por serial.
- [x] ~~Orientación y orden de color del panel~~ → `rotation 1` da 480x320, RGB correcto.
- [x] ~~Valores de calibración del táctil propios, en las cuatro esquinas~~ → hechos
      y persistidos en NVS.

## Notas de flasheo

- `upload_speed 921600` **falla a través de un dock USB-C** (HP Elite Dock G4):
  `A fatal error occurred: Unable to verify flash chip connection (Invalid head of
  packet (0xE0): Possible serial noise or corruption)`. Con **460800** va bien.
- Si un intento de flasheo se corta a medias, el proceso `esptool` se queda agarrado
  al puerto: `[Errno 35] Could not exclusively lock port`. Localizarlo con
  `lsof /dev/cu.usbserial-*` y matarlo antes de reintentar.
