#pragma once
#include <stdbool.h>

void uiPermissionCreate();

/** Carga la pantalla modal con la peticion pendiente. */
void uiPermissionShow();

/** true mientras la modal esta en pantalla. */
bool uiPermissionActive();

/** Cuenta atras y caducidad. Llamar unas 4 veces por segundo. */
void uiPermissionTick();

/**
 * Pulsacion corta de BOOT: confirma lo seleccionado.
 * Devuelve true si habia seleccion y se ha enviado.
 */
bool uiPermissionConfirm();
