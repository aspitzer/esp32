# claude-agent-display

Dispositivo de sobremesa (Freenove FNK0114-S, ESP32 + 320x480 táctil resistivo) que
muestra el estado de N agentes de Claude Code y permite aprobar/denegar sus permisos.

## Restricciones no negociables (salen del hardware)

1. **Sin PSRAM.** Prohibido el framebuffer completo (320x480x2 = 307 KB). LVGL con
   buffer parcial. Implementado: **un** buffer de 480x40 px (37,5 KB) reservado
   con `heap_caps_malloc(MALLOC_CAP_DMA)` una vez en el arranque. El segundo
   buffer solo sirve con flush asincrono; con `pushPixels` bloqueante LVGL
   esperaria igual. Anadirlo cuando se pase a `pushPixelsDMA`.
2. **Sin TLS en la placa.** MQTT plano en la LAN. Todo el cifrado lo hace el broker.
3. **Táctil resistivo de un punto.** Sin gestos ni swipes. Calibración persistida en
   NVS. Áreas táctiles mínimo 100x80 px.
4. **Acciones destructivas: confirmar con botón físico BOOT (GPIO0).** Se selecciona
   en pantalla, se confirma con BOOT. Nunca aprobar un permiso solo con un toque.
5. **IO35 e IO39 son input-only** y sin pull-up interno. No usarlos como salida ni
   como botón sin resistencia externa.
6. **IO25 sirve de SCL en el header I2C y de DAC1 del ESP32.** Verificar en el
   esquemático antes de usar audio e I2C a la vez.
7. **Panel TN**: ángulos de visión pobres. Contraste alto, nada de grises sutiles.
8. **Orientación apaisada (480x320).** Avatar a la izquierda (~180 px), lista de
   agentes a la derecha.

## Presupuesto de memoria

Obligatorio instrumentarlo desde el primer sketch: log de `ESP.getFreeHeap()` y
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` en el arranque, tras conectar
WiFi, y cada 30 s.

**Objetivo: >80 KB de heap libre en régimen.** Si baja de ahí, parar y optimizar
antes de añadir nada.

Lo que manda es **`largest_free_block`, no `free`**: el heap del ESP32 está partido
en regiones no contiguas y una asignación única no puede cruzarlas.

Medido en placa (fase 0), con WiFi asociado: `free 253 KB`, **`largest 107 KB`**.
WiFi cuesta ~50 KB de `free` y **cero** de `largest`. Ese 107 KB es el techo real
para los buffers de LVGL (que necesitan 2 x 25,6 KB).

Medido en placa con la UI de fase 1 corriendo y MQTT conectado:
`free 180 KB`, **`largest 107 KB`**. Muy por encima del suelo.

Reparto: buffer de dibujo 37,5 KB en heap DMA, pool propio de LVGL 24 KB en
DRAM estatica (`LV_MEM_SIZE`). 40 KB de pool **no caben**: desbordan
`dram0_0_seg` junto al resto de estaticos.

**Trampa de LVGL 9**: `lv_color_t` ocupa **3 bytes** (RGB888) aunque
`LV_COLOR_DEPTH` sea 16 y el buffer de render sea RGB565. `sizeof(lv_color_t)`
no sirve para dimensionar buffers; hay que contar bytes a mano.

Reglas derivadas:
- Nada de `String` concatenado en bucles.
- `ArduinoJson` con documentos de tamaño fijo (`StaticJsonDocument`).
- Sin `new` ni `malloc` dentro del render loop.
- Fuentes y assets en flash (`PROGMEM`) o en SD.

## Pines

**Se leen de [`PINOUT.md`](PINOUT.md). No se inventan ni se copian de un "ESP32 CYD"
genérico de internet** — hay muchas variantes incompatibles con los mismos nombres.
Cada pin ahí está marcado como `DOCS` (verificado en fuente oficial de Freenove) o
`PLACA` (pendiente de comprobar en hardware). Si una fuente contradice a otra, gana
lo que compile y funcione en la placa; y se anota.

## Contrato MQTT (resumen)

Detalle en [`docs/mqtt-contract.md`](docs/mqtt-contract.md). **Es una interfaz
estable: si cambia, cambian broker y firmware a la vez, en el mismo commit.**

| Topic | Sentido | Retained | Payload |
|---|---|---|---|
| `claude/agents/<agentId>/state` | broker → placa | sí | `{id,label,status,tool,detail,since,lastError}` |
| `claude/perm/req` | broker → placa | no | `{requestId,agentId,tool,summary,risk,expiresAt}` |
| `claude/perm/res` | placa → broker | no | `{requestId,decision,source}` |
| `claude/device/display-01/online` | placa → broker (LWT) | sí | `"1"` / `"0"` |

`status` ∈ `idle | thinking | working | waiting_permission | error | offline`
`decision` ∈ `allow | deny`

## Comandos

```bash
# mosquitto (usuario/contrasena en broker/.env, sin TLS)
cd broker && mosquitto -c mosquitto/mosquitto.conf -v

# firmware
cd firmware
pio run                      # compilar
pio run -t upload            # flashear
pio run -t monitor           # serial a 115200
pio run -t upload -t monitor # las dos cosas

# broker
cd broker
bun install
set -a && . ./.env && set +a       # credenciales MQTT + token de hooks
bun run src/index.ts               # hooks en 127.0.0.1:8787 + publicacion MQTT
bun run tools/mock-agent.ts        # 3 agentes falsos
bun run tools/mock-agent.ts --agents 4 --speed 5
curl -s localhost:8787/health | jq # estado del bus
```

`broker/.env` no se commitea. Se genera con `.env.example` de plantilla; la
contrasena de MQTT tiene que coincidir con `broker/mosquitto/passwd`
(`mosquitto_passwd -c mosquitto/passwd agentbus`).

El broker escucha los hooks **solo en 127.0.0.1**, con cabecera `X-Bus-Token`.
El puerto MQTT sí escucha en la LAN.

## Estado

**Fase 0 cerrada** (2026-09-19). Pines verificados en placa, panel y táctil
funcionando, calibración persistida en NVS, presupuesto de memoria medido con WiFi
levantado. Detalle en `PINOUT.md`.

**Fase 1 casi cerrada.** Broker y firmware hablando: la placa se conecta a MQTT,
publica su LWT y recibe los estados retained. Prototipo de las pantallas en
https://claude.ai/artifact/P6bkHKWNkznEBRaPDUdYBh

Particionado cambiado a `huge_app.csv`: con LVGL el binario son 1,16 MB y no
cabia en la ranura de 1,25 MB del esquema con OTA. Se pierde OTA, que no se usa.

Decisiones tomadas en fase 1, no obvias:
- **MQTT con usuario y contrasena.** Quien pueda publicar en `claude/perm/res`
  aprueba permisos arbitrarios. La restriccion 2 prohibe TLS en la placa, no
  autenticacion.
- **Al arrancar, el broker borra los retained de agentes que no conoce.** Un
  reinicio implica que esas sesiones ya no existen; si no, la placa pinta
  fantasmas trabajando para siempre.
- **La salida de `PermissionRequest` va anidada en `hookSpecificOutput`** y usa
  `verdict: "unspecified"` para "sin decision". Verificado en la documentacion,
  no deducido.
- Payload de estado medido: **157 bytes maximo**. El firmware puede usar un
  `StaticJsonDocument<256>`.

Ver el briefing para los criterios de aceptación de cada fase. No se pasa de fase
hasta cerrar la anterior.
