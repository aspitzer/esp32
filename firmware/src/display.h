#pragma once
#include <lvgl.h>

/** Arranca panel, tactil y LVGL. Carga la calibracion de NVS o la ejecuta. */
void displayInit(bool forceCalibration);

/** Llamar en el loop. Devuelve los ms hasta el siguiente trabajo de LVGL. */
uint32_t displayTick();

void displayBacklight(bool on);

/** Borra la calibracion de NVS. La siguiente llamada a displayInit recalibra. */
void displayEraseCalibration();
