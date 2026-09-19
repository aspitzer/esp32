#include "agents.h"
#include <string.h>

Agent agents[AGENTS_MAX] = {};

// Paleta del prototipo. Contraste alto a proposito: el panel es TN.
static const uint32_t COLORS[ST_COUNT] = {
  0x4DA6FF,  // idle
  0xB07CFF,  // thinking
  0x24D65F,  // working
  0xFFB020,  // waiting_permission
  0xFF4B4B,  // error
  0x5A5A5A,  // offline
};

static const char *NAMES[ST_COUNT] = {
  "idle", "thinking", "working", "waiting_permission", "error", "offline",
};

uint32_t agentColor(AgentStatus s) { return COLORS[s < ST_COUNT ? s : ST_IDLE]; }

const char *agentStatusName(AgentStatus s) { return NAMES[s < ST_COUNT ? s : ST_IDLE]; }

AgentStatus agentStatusFromName(const char *s) {
  if (!s) return ST_IDLE;
  for (uint8_t i = 0; i < ST_COUNT; i++) {
    if (strcmp(s, NAMES[i]) == 0) return (AgentStatus)i;
  }
  return ST_IDLE;
}

Agent *agentSlot(const char *id) {
  if (!id || !*id) return nullptr;

  for (auto &a : agents) {
    if (a.used && strncmp(a.id, id, AG_ID_LEN) == 0) return &a;
  }
  for (auto &a : agents) {
    if (!a.used) {
      memset(&a, 0, sizeof(a));
      a.used = true;
      strlcpy(a.id, id, AG_ID_LEN);
      return &a;
    }
  }

  // Lleno: se reutiliza el offline mas antiguo antes que rechazar el mensaje.
  Agent *victim = nullptr;
  for (auto &a : agents) {
    if (a.status != ST_OFFLINE) continue;
    if (!victim || a.rxMillis < victim->rxMillis) victim = &a;
  }
  if (!victim) return nullptr;

  memset(victim, 0, sizeof(*victim));
  victim->used = true;
  strlcpy(victim->id, id, AG_ID_LEN);
  return victim;
}

void agentRemove(const char *id) {
  for (auto &a : agents) {
    if (a.used && strncmp(a.id, id, AG_ID_LEN) == 0) memset(&a, 0, sizeof(a));
  }
}

AgentStatus agentsAggregate() {
  bool perm = false, err = false, work = false, think = false, idle = false;
  for (const auto &a : agents) {
    if (!a.used) continue;
    switch (a.status) {
      case ST_WAITING_PERMISSION: perm = true; break;
      case ST_ERROR:              err = true;  break;
      case ST_WORKING:            work = true; break;
      case ST_THINKING:           think = true; break;
      case ST_IDLE:               idle = true; break;
      default: break;
    }
  }
  if (perm)  return ST_WAITING_PERMISSION;   // lo unico que te reclama a ti
  if (err)   return ST_ERROR;
  if (work)  return ST_WORKING;
  if (think) return ST_THINKING;
  if (idle)  return ST_IDLE;
  return ST_OFFLINE;
}

/** Orden de interes: permiso, error, trabajo, pensando, reposo, offline. */
static uint8_t rank(AgentStatus s) {
  switch (s) {
    case ST_WAITING_PERMISSION: return 0;
    case ST_ERROR:              return 1;
    case ST_WORKING:            return 2;
    case ST_THINKING:           return 3;
    case ST_IDLE:               return 4;
    default:                    return 5;
  }
}

uint8_t agentsVisible(uint8_t *out, uint8_t max) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < AGENTS_MAX && n < AGENTS_MAX; i++) {
    if (agents[i].used) out[n++] = i;
  }

  // Insercion: n <= 6, no merece nada mas sofisticado.
  for (uint8_t i = 1; i < n; i++) {
    const uint8_t v = out[i];
    int8_t j = i - 1;
    while (j >= 0 && rank(agents[out[j]].status) > rank(agents[v].status)) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = v;
  }

  return n > max ? max : n;
}
