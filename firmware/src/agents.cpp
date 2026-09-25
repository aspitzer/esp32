#include "agents.h"
#include <Arduino.h>
#include <string.h>
#include <time.h>

Agent agents[AGENTS_MAX] = {};

// Paleta del prototipo. Contraste alto a proposito: el panel es TN.
static const uint32_t COLORS[ST_COUNT] = {
  0x4DA6FF,  // idle
  0xB07CFF,  // thinking
  0x24D65F,  // working
  0xFFB020,  // waiting_permission
  0xFF4B4B,  // error
  0x19E0E8,  // stalled: cian, lejos del azul de idle en un panel TN
  0x5A5A5A,  // offline
};

static const char *NAMES[ST_COUNT] = {
  "idle", "thinking", "working", "waiting_permission", "error", "stalled", "offline",
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

AgentStatus agentEffective(const Agent &a) {
  // Sin `awaiting` esta quieta, no esperandote: son cosas distintas.
  if (a.status != ST_IDLE || !a.awaiting || a.since == 0) return a.status;

  const time_t now = time(nullptr);
  if (now < 1700000000) return a.status;          // sin hora NTP no se puede juzgar
  if ((uint32_t)now < a.since) return a.status;

  return ((uint32_t)now - a.since >= STALLED_AFTER_S) ? ST_STALLED : ST_IDLE;
}

AgentStatus agentsAggregate() {
  bool perm = false, err = false, stall = false, work = false, think = false, idle = false;
  for (const auto &a : agents) {
    if (!a.used) continue;
    switch (agentEffective(a)) {
      case ST_WAITING_PERMISSION: perm = true;  break;
      case ST_ERROR:              err = true;   break;
      case ST_STALLED:            stall = true; break;
      case ST_WORKING:            work = true;  break;
      case ST_THINKING:           think = true; break;
      case ST_IDLE:               idle = true;  break;
      default: break;
    }
  }
  if (perm)  return ST_WAITING_PERMISSION;   // lo unico que te reclama ya
  if (err)   return ST_ERROR;
  if (stall) return ST_STALLED;              // te esta esperando desde hace rato
  if (work)  return ST_WORKING;
  if (think) return ST_THINKING;
  if (idle)  return ST_IDLE;
  return ST_OFFLINE;
}

/**
 * Orden de interes. Lo que esta PASANDO ahora va arriba: una sesion trabajando
 * o rota es informacion viva, y una que lleva rato esperandote puede aguantar
 * unas lineas mas abajo. Antes "te espera" adelantaba a las que trabajaban y
 * la pantalla enseñaba lo quieto por encima de lo vivo.
 */
static uint8_t rank(AgentStatus s) {
  switch (s) {
    case ST_WAITING_PERMISSION: return 0;   // te bloquea AHORA
    case ST_ERROR:              return 1;
    case ST_WORKING:            return 2;
    case ST_THINKING:           return 3;
    case ST_STALLED:            return 4;
    case ST_IDLE:               return 5;
    default:                    return 6;
  }
}

/**
 * Un agente pasa de working a idle y vuelve varias veces por minuto. Reordenar
 * en cada cambio hace que las filas salten sin parar y la pantalla no se puede
 * leer. Solucion: el orden solo se recalcula cuando cambia QUIEN esta, o cuando
 * han pasado REORDER_QUIET_MS desde la ultima reordenacion.
 *
 * Excepcion: waiting_permission entra de inmediato. Es lo unico que te reclama
 * y no puede esperar a que se calme la pantalla.
 */
#define REORDER_QUIET_MS 6000

static uint8_t  orderCache[AGENTS_MAX];
static uint8_t  orderLen      = 0;
static uint32_t orderAt       = 0;
static uint8_t  orderTopRank  = 0xFF;

static void sortByRank(uint8_t *arr, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {
    const uint8_t v = arr[i];
    int8_t j = i - 1;
    while (j >= 0 && rank(agentEffective(agents[arr[j]])) > rank(agentEffective(agents[v]))) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = v;
  }
}

uint8_t agentsVisible(uint8_t *out, uint8_t max) {
  uint8_t live[AGENTS_MAX], n = 0;
  for (uint8_t i = 0; i < AGENTS_MAX; i++) {
    if (agents[i].used) live[n++] = i;
  }

  // ¿Sigue valiendo el orden anterior? Vale si estan los mismos agentes.
  bool sameSet = (n == orderLen);
  if (sameSet) {
    for (uint8_t i = 0; i < n && sameSet; i++) {
      bool found = false;
      for (uint8_t j = 0; j < orderLen; j++) {
        if (orderCache[j] == live[i]) { found = true; break; }
      }
      sameSet = found;
    }
  }

  uint8_t topRank = 0xFF;
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t r = rank(agentEffective(agents[live[i]]));
    if (r < topRank) topRank = r;
  }
  const bool permissionAppeared = (topRank == 0 && orderTopRank != 0);

  const uint32_t now = millis();
  if (!sameSet || permissionAppeared || now - orderAt >= REORDER_QUIET_MS) {
    sortByRank(live, n);
    memcpy(orderCache, live, n);
    orderLen     = n;
    orderAt      = now;
    orderTopRank = topRank;
  }

  memcpy(out, orderCache, orderLen);
  return orderLen > max ? max : orderLen;
}
