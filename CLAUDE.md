# claude-agent-display

Dispositivo de sobremesa (Freenove FNK0114-S, ESP32 + 320x480 táctil resistivo) que
muestra el estado de N agentes de Claude Code y permite aprobar/denegar sus permisos.

## Restricciones no negociables (salen del hardware)

1. **Sin PSRAM.** Prohibido el framebuffer completo (320x480x2 = 307 KB). LVGL con
   buffer parcial: dos buffers de 320x40 px (~51 KB) y DMA.
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
# firmware
cd firmware
pio run                      # compilar
pio run -t upload            # flashear
pio run -t monitor           # serial a 115200
pio run -t upload -t monitor # las dos cosas

# broker
cd broker
bun install
bun run src/index.ts         # servidor de hooks en 127.0.0.1:8787 + MQTT
bun run tools/mock-agent.ts  # 3 agentes falsos publicando eventos
```

El broker escucha los hooks **solo en 127.0.0.1**, con cabecera `X-Bus-Token`.
El puerto MQTT sí escucha en la LAN.

## Estado

**Fase 0 cerrada** (2026-09-19). Pines verificados en placa, panel y táctil
funcionando, calibración persistida en NVS, presupuesto de memoria medido con WiFi
levantado. Detalle en `PINOUT.md`.

Siguiente: fase 1, dashboard de solo lectura. Empezar por `broker/tools/mock-agent.ts`,
no por el firmware.

Ver el briefing para los criterios de aceptación de cada fase. No se pasa de fase
hasta cerrar la anterior.
