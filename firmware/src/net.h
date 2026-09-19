#pragma once
#include <Arduino.h>

#define PERM_ID_LEN      40
#define PERM_SUMMARY_LEN 96

enum PermRisk : uint8_t { RISK_LOW = 0, RISK_MEDIUM, RISK_HIGH };

struct PermReq {
  bool     active;
  char     requestId[PERM_ID_LEN];
  char     agentId[24];
  char     tool[18];
  char     summary[PERM_SUMMARY_LEN];
  PermRisk risk;
  uint32_t expiresAt;      // epoch en segundos
  uint32_t rxMillis;
};

enum NetState : uint8_t {
  NET_WIFI_DOWN = 0,
  NET_WIFI_UP,          // asociado, pero sin broker
  NET_MQTT_UP,
};

void     netInit();
void     netTick();
NetState netState();

int8_t   netRssi();
uint8_t  netMqttAttempt();      // intentos seguidos fallidos
uint32_t netNextRetryIn();      // ms que faltan para el siguiente intento
const char *netIpMqtt();

/** Se llama cuando cambia el estado de algun agente o de la red. */
void netOnChange(void (*cb)());

// --- permisos (fase 2) -------------------------------------------------------

/** Peticion pendiente, o nullptr. Solo hay una a la vez a proposito. */
const PermReq *netPendingPerm();

/** Responde y cierra la peticion. Publica en claude/perm/res. */
void netAnswerPerm(bool allow);

/** Descarta la peticion sin responder (caducada). */
void netDropPerm();

/** Se llama cuando llega una peticion nueva. */
void netOnPermission(void (*cb)());

// --- acciones (fase 4) -------------------------------------------------------

/** Publica en claude/action/req. El catalogo lo valida el broker. */
void netSendAction(const char *agentId, const char *action);

/** Ultimo resultado recibido, para pintarlo. Vacio si no hay. */
const char *netLastActionResult();
void        netClearActionResult();
