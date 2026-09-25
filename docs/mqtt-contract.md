# Contrato MQTT — v3

Interfaz estable entre `broker/` y `firmware/`. **Cambiarla implica cambiar los dos
lados en el mismo commit y subir la versión de este documento.**

Broker MQTT: Mosquitto local, sin TLS, en la LAN. QoS 1 salvo donde se indique.

## 1. Estado de agente

**Topic:** `claude/agents/<agentId>/state`
**Sentido:** broker → placa
**Retained:** sí

```json
{
  "id": "sherpa-01",
  "label": "brain/api",
  "status": "working",
  "tool": "Bash",
  "detail": "pytest tests/",
  "since": 1758200000,
  "lastError": null,
  "subN": 1,
  "subType": "general-purpose"
}
```

| Campo | Tipo | Notas |
|---|---|---|
| `id` | string | igual que `<agentId>` del topic |
| `label` | string | nombre corto para la UI (≤ 24 chars) |
| `status` | enum | `idle`, `thinking`, `working`, `waiting_permission`, `error`, `offline` |
| `tool` | string \| null | herramienta en curso |
| `detail` | string \| null | qué está haciendo, en cristiano: "Editando net.cpp", "Lanzando los tests" |
| `raw` | string \| null | el comando literal, para la pantalla de detalle |
| `since` | number | epoch en segundos del último cambio de `status` |
| `lastError` | string \| null | p. ej. `rate_limit`, `overloaded` |
| `tmux` | boolean | la sesión es alcanzable por `tmux send-keys` |
| `subN` | number | subagentes vivos bajo esta sesión |
| `subType` | string \| null | tipo del subagente cuyo estado manda ahora |

**Una fila = una sesión.** Los subagentes no tienen topic propio: se agrupan
bajo su sesión y el broker publica el estado *fundido*. Gana el más
interesante de los dos lados, salvo `error` y `waiting_permission` del padre,
que mandan siempre porque te reclaman a ti.

**Todo el payload es ASCII.** Las fuentes Montserrat de LVGL no traen tildes,
ni ñ, ni `¿`, ni el punto medio: verificado leyendo el cmap del `.c` de la
fuente. El broker translitera antes de publicar, así la placa no tiene que
saber nada de esto.

`label` se resuelve por este orden:

1. **El nombre que le hayas puesto tú** con `/rename` o `--name`. Claude Code lo
   guarda en `<transcript sin .jsonl>/custom-title.json`. Gana siempre.
2. El `aiTitle` del transcript, que Claude Code genera solo con el tema.
3. El nombre de la carpeta, que no distingue dos sesiones en el mismo repo.

El fichero del nombre propio es diminuto y se relee cada 30 s; el transcript
puede pesar megas, así que solo cada 5 minutos y solo si no hay nombre propio.

Retained es clave: la placa reinicia y repinta al instante sin pedir nada.
Al terminar una sesión el broker publica `status: "offline"`, también retained.

## 2. Petición de permiso

**Topic:** `claude/perm/req`
**Sentido:** broker → placa
**Retained:** no

```json
{
  "requestId": "uuid",
  "agentId": "sherpa-01",
  "tool": "Bash",
  "summary": "git push --force origin main",
  "risk": "high",
  "expiresAt": 1758200090
}
```

`risk` ∈ `low | medium | high`. `expiresAt` es epoch en segundos; la placa pinta la
cuenta atrás con ese valor y descarta la petición al llegar a cero.

## 3. Respuesta de permiso

**Topic:** `claude/perm/res`
**Sentido:** placa → broker
**Retained:** no

```json
{ "requestId": "uuid", "decision": "allow", "source": "device" }
```

`decision` ∈ `allow | deny`. `source` ∈ `device | timeout | fallback`.
El broker ignora respuestas con un `requestId` que no tenga pendiente.

**El broker no enruta todo a la placa.** Solo las peticiones cuyo riesgo llega a
`PERM_MIN_RISK` (por defecto `high`). El resto se resuelve como `unspecified` al
instante y sigue el flujo normal de permisos en el portátil. Sin este filtro la
placa interrumpe por cada `Edit`, dejas de mirarla, y el proyecto ha fracasado.

## 3b. Acciones de vuelta (fase 4)

**Topic:** `claude/action/req` · **Sentido:** placa → broker · **Retained:** no

```json
{ "agentId": "sherpa-01", "action": "continue", "source": "device" }
```

`action` ∈ `go | continue | tests | status | interrupt`

La placa **solo publica aquí tras confirmar con BOOT**. Tocar un botón en la
pantalla de acciones únicamente selecciona: sin pulsación física no sale nada
al bus.

**El catálogo es cerrado.** El texto que se escribe en la sesión está en
`broker/src/actions.ts`, nunca viaja por MQTT, y todo se ejecuta con argv sin
shell. Un `action` desconocido se descarta en el broker. Un dispositivo
comprometido en la LAN solo puede disparar una de esas cuatro frases, no
comandos arbitrarios.

`tmux` viaja en el estado para que la placa pueda avisar **antes** de pulsar:
sin panel, la acción no escribe en la conversación viva, abre una sesión nueva
sin contexto. El broker cachea los paneles y los refresca cada 10 s, porque
esto se consulta en cada publicación de estado.

El broker elige el camino por agente:

| Situación | Camino | Efecto |
|---|---|---|
| La sesión corre en un panel de tmux con el mismo `cwd` | `tmux send-keys` | Escribe en la conversación viva |
| No hay panel | `claude -p` | Proceso nuevo, **sin el contexto** de la conversación |
| `interrupt` sin tmux | ninguno | Falla con detalle: no hay equivalente headless |

**Respuesta:** `claude/action/res` · broker → placa · no retained

```json
{ "agentId": "sherpa-01", "action": "continue", "ok": true, "via": "tmux", "detail": "brain:0.1" }
```

`via` ∈ `tmux | claude-p | none`

## 4. Presencia

- **LWT de la placa:** `claude/device/display-01/online` → `"0"`, retained.
- Al conectar, la placa publica `"1"` retained en ese mismo topic.
- El broker solo enruta permisos a la placa si está online. Si no, responde al hook
  con el fallback (200 y cuerpo vacío, "sin decisión").
