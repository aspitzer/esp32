# Contrato MQTT — v1

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
  "lastError": null
}
```

| Campo | Tipo | Notas |
|---|---|---|
| `id` | string | igual que `<agentId>` del topic |
| `label` | string | nombre corto para la UI (≤ 24 chars) |
| `status` | enum | `idle`, `thinking`, `working`, `waiting_permission`, `error`, `offline` |
| `tool` | string \| null | herramienta en curso |
| `detail` | string \| null | primera línea del comando o fichero, truncado a 64 chars |
| `since` | number | epoch en segundos del último cambio de `status` |
| `lastError` | string \| null | p. ej. `rate_limit`, `overloaded` |

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

## 4. Presencia

- **LWT de la placa:** `claude/device/display-01/online` → `"0"`, retained.
- Al conectar, la placa publica `"1"` retained en ese mismo topic.
- El broker solo enruta permisos a la placa si está online. Si no, responde al hook
  con el fallback (200 y cuerpo vacío, "sin decisión").
