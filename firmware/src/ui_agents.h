#pragma once

/** Construye la pantalla. Una vez, en el arranque. */
void uiAgentsCreate();

/** Repinta con el modelo actual. Idempotente, no reserva memoria. */
void uiAgentsRefresh();

/** Avanza cronometros y parpadeo. Llamar unas 4 veces por segundo. */
void uiAgentsTickSlow();

/** Vuelve a la lista desde cualquier otra pantalla. */
void uiAgentsShowList();
