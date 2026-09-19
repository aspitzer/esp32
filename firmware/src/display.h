#pragma once
#include <lvgl.h>

/** Arranca panel, tactil y LVGL. Carga la calibracion de NVS o la ejecuta. */
void displayInit(bool forceCalibration);

/** Llamar en el loop. Devuelve los ms hasta el siguiente trabajo de LVGL. */
uint32_t displayTick();

void displayBacklight(bool on);

/** Frames completos desde la ultima llamada. Para medir fps de verdad. */
uint32_t displayFramesAndReset();

/** Diagnostico de render: trozos, pixeles y ms de SPI desde la ultima llamada. */
void displayRenderStats(uint32_t *chunks, uint32_t *pixels, uint32_t *spiMs);

/** Borra la calibracion de NVS. La siguiente llamada a displayInit recalibra. */
void displayEraseCalibration();
