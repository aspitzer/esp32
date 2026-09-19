#pragma once
#include <lvgl.h>

#include "agents.h"

/** Crea los ojos dentro de parent. Solo primitivas LVGL, cero assets. */
void avatarCreate(lv_obj_t *parent);

/** Cambia de estado. Idempotente: repetir el mismo estado no reinicia nada. */
void avatarSetState(AgentStatus s);

/** Parpadeo y microgestos. Llamar unas 4 veces por segundo. */
void avatarTick();
