#pragma once
// Modelo de agentes en memoria. Todo de tamano fijo: ni un malloc en caliente.
// Refleja claude/agents/<id>/state — ver ../../docs/mqtt-contract.md

#include <Arduino.h>

#define AGENTS_MAX      6    // se pintan los 4 primeros; los otros esperan turno
#define AG_ID_LEN      20
#define AG_LABEL_LEN   28
#define AG_TOOL_LEN    18
#define AG_DETAIL_LEN  68
#define AG_ERROR_LEN   24

enum AgentStatus : uint8_t {
  ST_IDLE = 0,
  ST_THINKING,
  ST_WORKING,
  ST_WAITING_PERMISSION,
  ST_ERROR,
  ST_OFFLINE,
  ST_COUNT,
};

struct Agent {
  bool         used;
  char         id[AG_ID_LEN];
  char         label[AG_LABEL_LEN];
  AgentStatus  status;
  char         tool[AG_TOOL_LEN];
  char         detail[AG_DETAIL_LEN];
  uint32_t     since;                 // epoch en segundos, del broker
  char         lastError[AG_ERROR_LEN];
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

/** Estado agregado del sistema, para el avatar. Prioridad: permiso > error > trabajo. */
AgentStatus agentsAggregate();

/** Indices de los agentes a pintar, mas vivos primero. Devuelve cuantos. */
uint8_t agentsVisible(uint8_t *out, uint8_t max);
