#pragma once
#include <Arduino.h>

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
