# claude-agent-display

Dispositivo de sobremesa que muestra en qué anda cada sesión de Claude Code y
permite aprobar sus permisos tocando la pantalla, sin volver al portátil.

Hardware: **Freenove FNK0114-S** (E32R40T) — ESP32-WROOM-32E, panel ST7796 de
4" a 320x480, táctil resistivo XPT2046. Sin PSRAM.

```
Claude Code (sesiones en el Mac)
   │  hooks type:"http"  →  POST JSON a 127.0.0.1:8787
   ▼
agent-bus (Bun + TypeScript)
   │  estado por agente · permisos pendientes · acciones de vuelta
   ▼
Mosquitto (LAN, con usuario y contraseña, sin TLS)
   ▲ │
   │ ▼
FNK0114-S (LVGL 9) — pinta, recoge toques, devuelve decisiones
```

## Arranque

```bash
brew install oven-sh/bun/bun mosquitto tmux      # tmux solo para la fase 4
cd broker && bun install
cp .env.example .env                              # y rellena MQTT_PASS / AGENT_BUS_TOKEN
mosquitto_passwd -c mosquitto/passwd agentbus     # la misma contraseña que en .env

./scripts/install-autostart.sh                    # mosquitto + broker al iniciar sesion
#   (o ./scripts/start.sh para arrancarlos solo esta vez)
cd firmware && pio run -t upload -t monitor       # placa
```

Las credenciales de la placa van en `firmware/include/secrets.h` (ignorado por
git; plantilla en `secrets.h.example`).

Para conectar tus sesiones reales, pega `hooks/settings.snippet.json` en
`~/.claude/settings.json`. Para los permisos, además
`hooks/settings.phase2.snippet.json`.

Sin agentes reales:

```bash
cd broker && bun run tools/mock-agent.ts --agents 4 --speed 2
```

## Qué hace cada pantalla

| Pantalla | Cómo se llega | Qué muestra |
|---|---|---|
| Lista | inicio | Avatar + hasta 4 agentes con color de estado, herramienta y cronómetro |
| Detalle | tocando una fila | Comando completo sin truncar, último error, id, tiempo en ese estado |
| Acciones | botón en el detalle | CONTINUA · TESTS · ESTADO · PARAR |
| Permiso | automática | Comando, riesgo, cuenta atrás. Se elige tocando, **se confirma con BOOT** |

## Las decisiones que no son obvias

**El toque nunca aprueba nada por sí solo.** El táctil resistivo se dispara con
el dedo plano. Una aprobación accidental aquí ejecuta un `rm -rf`. Tocar
selecciona; confirmar es siempre el botón físico BOOT.

**En modo `auto` la placa se enciende poco, y es lo correcto.** Con
`permissions.defaultMode: "auto"` el clasificador de Claude Code aprueba solo
lo inocuo y solo pregunta por lo destructivo o irreversible. El hook
`PermissionRequest` únicamente se dispara cuando iba a preguntar, así que el
conjunto que llega a la placa es casi el mismo que marca esta heurística. En
modo `default` se dispara mucho más.

**El broker solo molesta con lo de riesgo alto** (`PERM_MIN_RISK`, por defecto
`high`). Todo lo demás se resuelve al instante y sigue el flujo normal en el
portátil. Una placa que interrumpe por cada `Edit` es una placa que dejas de
mirar.

**El broker tiene que estar siempre vivo.** Con el hook de permisos puesto, si
el broker no responde cada evento intenta un POST que falla. Por eso Mosquitto
y el broker van como LaunchAgents con `KeepAlive`: arrancan al iniciar sesión y
launchd los resucita si mueren. Probado matándolos con `kill -9`.

**El hook de permisos nunca agota su timeout.** Si la placa está apagada
responde en 0,04 s; si nadie contesta, a los 90 s. Un hook colgado congela la
sesión de Claude Code, y eso es peor que no tener dispositivo.

**MQTT con usuario y contraseña.** Sin TLS, porque la placa no puede con una
handshake — pero quien pueda publicar en `claude/perm/res` aprueba permisos
arbitrarios, y el puerto 1883 sale a la LAN.

**El catálogo de acciones es cerrado.** El texto que se escribe en tu sesión
vive en `broker/src/actions.ts` y nunca viaja por MQTT. Todo se lanza con argv,
sin shell.

**Un reinicio del broker borra los retained que no reconoce.** Si no, la placa
pinta para siempre agentes de sesiones que ya murieron.

## Medido en la placa, no estimado

| | |
|---|---|
| Heap con WiFi, MQTT y UI | `free 179 KB` · **`largest 107 KB`** (suelo: 80 KB) |
| Framerate con el avatar animando | **23,4 fps** (mínimo: 20) |
| Payload de estado | 157 B máx → `StaticJsonDocument<256>` sobra |
| Flash | 1,20 MB de 3 MB (`huge_app.csv`, sin OTA) |
| Error del táctil en las esquinas | hasta 12 px → nada a menos de 14 px del borde |
| Fallback de permisos con la placa apagada | 0,04 s |

`largest_free_block` es la métrica que manda, no `free`: el heap del ESP32 está
partido en regiones no contiguas.

## Documentos

- [`CLAUDE.md`](CLAUDE.md) — restricciones, presupuesto de memoria, comandos
- [`PINOUT.md`](PINOUT.md) — pines verificados, con su fuente y su estado
- [`docs/mqtt-contract.md`](docs/mqtt-contract.md) — el contrato entre los dos lados
