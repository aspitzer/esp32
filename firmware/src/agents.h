#pragma once
// Modelo de agentes en memoria. Todo de tamano fijo: ni un malloc en caliente.
// Refleja claude/agents/<id>/state — ver ../../docs/mqtt-contract.md

#include <Arduino.h>

#define AGENTS_MAX      6    // se pintan los 4 primeros; los otros esperan turno
#define AG_ID_LEN      20
#define AG_LABEL_LEN   34   // el broker recorta a 30; margen para el NUL y de sobra
#define AG_TOOL_LEN    18
#define AG_DETAIL_LEN  68
#define AG_ERROR_LEN   24
#define AG_SUBTYPE_LEN 20

/**
 * ST_STALLED no existe en el contrato MQTT: se deduce aqui. Un agente en
 * reposo mucho rato casi nunca esta "en reposo", esta esperandote a ti, y eso
 * tiene que verse distinto de una sesion que acaba de terminar su turno.
 */
enum AgentStatus : uint8_t {
  ST_IDLE = 0,
  ST_THINKING,
  ST_WORKING,
  ST_WAITING_PERMISSION,
  ST_ERROR,
  ST_STALLED,
  ST_OFFLINE,
  ST_COUNT,
};

/** Reposo a partir del cual se considera que la sesion te espera. */
#define STALLED_AFTER_S (10 * 60)

struct Agent {
  bool         used;
  char         id[AG_ID_LEN];
  char         label[AG_LABEL_LEN];
  AgentStatus  status;
  char         tool[AG_TOOL_LEN];
  char         detail[AG_DETAIL_LEN];   // explicativo: "Editando net.cpp"
  char         raw[AG_DETAIL_LEN];      // el comando literal, para el detalle
  uint32_t     since;                 // epoch en segundos, del broker
  char         lastError[AG_ERROR_LEN];
  // Subagentes agrupados bajo esta sesion. El broker ya funde su estado con
  // el del padre; esto es solo para poder decir quien esta trabajando.
  bool         tmux;                    // alcanzable por tmux: el boton escribe de verdad
  uint8_t      subN;
  char         subType[AG_SUBTYPE_LEN];
  uint32_t     rxMillis;              // cuando llego, para ordenar
};

extern Agent agents[AGENTS_MAX];

/** Colores por estado. Mismo codigo en la fila, el avatar y el LED RGB. */
uint32_t        agentColor(AgentStatus s);
const char     *agentStatusName(AgentStatus s);
AgentStatus     agentStatusFromName(const char *s);

/** Devuelve el slot del id, creandolo si hace falta. nullptr si no queda sitio. */
Agent *agentSlot(const char *id);
void   agentRemove(const char *id);

/** Estado real de un agente, con ST_STALLED ya aplicado. Usar SIEMPRE este. */
AgentStatus agentEffective(const Agent &a);

/** Estado agregado del sistema, para el avatar. Prioridad: permiso > error > espera. */
AgentStatus agentsAggregate();

/** Indices de los agentes a pintar, mas vivos primero. Devuelve cuantos. */
uint8_t agentsVisible(uint8_t *out, uint8_t max);
