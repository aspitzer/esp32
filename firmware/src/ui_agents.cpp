#include "ui_agents.h"

#include <WiFi.h>
#include <lvgl.h>
#include <time.h>

#include "agents.h"
#include "avatar.h"
#include "config.h"
#include "net.h"
#include <string.h>

// Paleta. El panel es TN: negro puro contra color saturado, sin grises sutiles.
#define C_BG     lv_color_hex(0x000000)
#define C_PANEL  lv_color_hex(0x101010)
#define C_LINE   lv_color_hex(0x1F1F1F)
#define C_EDGE   lv_color_hex(0x2E2E2E)
#define C_TXT    lv_color_hex(0xFFFFFF)
#define C_DIM    lv_color_hex(0x9A9A9A)
#define C_SOFT   lv_color_hex(0xC8C8C8)
// Borde de los botones neutros. C_EDGE (#2E2E2E) vale para separar paneles,
// pero en el TN un boton con ese borde no se distingue del fondo.
#define C_BTN    lv_color_hex(0x7A7A7A)

#define ROW_H ((SCREEN_H - TOPBAR_H) / ROWS_VISIBLE)
#define TIMER_W 62

struct RowUi {
  lv_obj_t *root;
  lv_obj_t *bar;
  lv_obj_t *label;
  lv_obj_t *detail;
  lv_obj_t *timer;
};

static lv_obj_t *scr;
static lv_obj_t *lblMqtt, *lblRssi, *lblClock;
static lv_obj_t *lblMood, *lblCount;
static RowUi     rows[ROWS_VISIBLE];


// --- pantalla de detalle -----------------------------------------------------
// Se entra tocando una fila. El id se guarda, no el indice: el agente puede
// cambiar de slot entre repintados y acabarias mirando otro.
static lv_obj_t *detailScr;
static lv_obj_t *dLabel, *dStatus, *dTool, *dDetail, *dMeta, *dError;
static char      detailId[AG_ID_LEN] = {0};

// --- pantalla de acciones (fase 4) -------------------------------------------
//
// Mismo contrato que los permisos: confirmacion en dos pasos, toda tactil.
// Un roce suelto no puede lanzar nada, porque una accion sobre una sesion que
// no esta en tmux arranca un claude -p, o sea una sesion autonoma de verdad.
// Confirmar cae a la IZQUIERDA y cancelar a la derecha, en una banda distinta
// de la de los botones de accion: repetir un toque nunca ejecuta.
#define ACTION_COUNT 4

static lv_obj_t   *actionsScr, *lblActAgent, *lblActResult;
static lv_obj_t   *btnActBack, *btnActOk, *btnActCancel;
static lv_obj_t   *actBtn[ACTION_COUNT];
static lv_color_t  actColor[ACTION_COUNT];
static const char *actId[ACTION_COUNT];
static const char *actLabel[ACTION_COUNT];
static int8_t      actChosen  = -1;
static bool        actVisible = false;

bool uiActionsActive() { return actVisible; }

static const Agent *agentById(const char *id) {
  if (!id || !*id) return nullptr;
  for (const auto &a : agents) {
    if (a.used && strncmp(a.id, id, AG_ID_LEN) == 0) return &a;
  }
  return nullptr;
}

// --- utilidades --------------------------------------------------------------

static void noPad(lv_obj_t *o) {
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * lv_label_set_text invalida el area SIEMPRE, aunque el texto sea identico.
 * Con 15 etiquetas refrescadas 4 veces por segundo eso son 60 invalidaciones
 * por segundo de texto que no ha cambiado, y rasterizar texto es lo caro:
 * medido, se comia el framerate mientras el SPI estaba al 9%.
 */
static void setTextIfChanged(lv_obj_t *label, const char *text) {
  const char *cur = lv_label_get_text(label);
  if (cur && strcmp(cur, text) == 0) return;
  lv_label_set_text(label, text);
}

/** Igual para el color: cambiarlo invalida aunque sea el mismo. */
static void setColorIfChanged(lv_obj_t *o, lv_color_t c) {
  const lv_color_t cur = lv_obj_get_style_text_color(o, LV_PART_MAIN);
  if (cur.red == c.red && cur.green == c.green && cur.blue == c.blue) return;
  lv_obj_set_style_text_color(o, c, 0);
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, "");
  return l;
}

/** "1:24" o "12:41"; por encima de una hora, "2h14". */
static void fmtElapsed(char *out, size_t n, uint32_t sinceEpoch) {
  const time_t nowEpoch = time(nullptr);
  if (sinceEpoch == 0 || nowEpoch < 1700000000 || (uint32_t)nowEpoch < sinceEpoch) {
    snprintf(out, n, "--:--");
    return;
  }
  uint32_t s = (uint32_t)nowEpoch - sinceEpoch;
  if (s >= 3600) snprintf(out, n, "%uh%02u", s / 3600, (s % 3600) / 60);
  else           snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

// --- construccion ------------------------------------------------------------

static void buildTopbar() {
  lv_obj_t *bar = lv_obj_create(scr);
  noPad(bar);
  lv_obj_set_size(bar, SCREEN_W, TOPBAR_H);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, C_PANEL, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(bar, C_EDGE, 0);
  lv_obj_set_style_border_width(bar, 1, 0);

  lv_obj_t *title = mkLabel(bar, &lv_font_montserrat_12, C_DIM);
  lv_label_set_text(title, "CLAUDE AGENTS");
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

  lblClock = mkLabel(bar, &lv_font_montserrat_14, C_TXT);
  lv_obj_align(lblClock, LV_ALIGN_RIGHT_MID, -10, 0);

  lblRssi = mkLabel(bar, &lv_font_montserrat_12, C_DIM);
  lv_obj_align(lblRssi, LV_ALIGN_RIGHT_MID, -62, 0);

  lblMqtt = mkLabel(bar, &lv_font_montserrat_12, C_DIM);
  lv_obj_align(lblMqtt, LV_ALIGN_RIGHT_MID, -130, 0);
}

static void buildAvatar() {
  lv_obj_t *panel = lv_obj_create(scr);
  noPad(panel);
  lv_obj_set_size(panel, AVATAR_W, SCREEN_H - TOPBAR_H);
  lv_obj_set_pos(panel, 0, TOPBAR_H);
  lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_color(panel, C_EDGE, 0);
  lv_obj_set_style_border_width(panel, 1, 0);

  avatarCreate(panel);

  // Debajo de la boca, que ocupa hasta y=+68.
  lblMood = mkLabel(panel, &lv_font_montserrat_16, C_TXT);
  lv_obj_align(lblMood, LV_ALIGN_CENTER, 0, 92);

  lblCount = mkLabel(panel, &lv_font_montserrat_12, C_DIM);
  lv_obj_align(lblCount, LV_ALIGN_CENTER, 0, 116);
}

static void refreshDetail();

static void rowClicked(lv_event_t *e) {
  const uint8_t slot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

  uint8_t idx[AGENTS_MAX];
  const uint8_t n = agentsVisible(idx, ROWS_VISIBLE);
  if (slot >= n) return;

  strlcpy(detailId, agents[idx[slot]].id, AG_ID_LEN);
  refreshDetail();
  lv_screen_load(detailScr);
  Serial.printf("[ui  ] detalle de %s\n", detailId);
}

static void backClicked(lv_event_t *) {
  detailId[0] = '\0';
  actVisible  = false;
  lv_screen_load(scr);
}

void uiAgentsShowList() {
  detailId[0] = '\0';
  actVisible  = false;
  actChosen   = -1;
  lv_screen_load(scr);
}

static void buildRows() {
  const lv_coord_t listW = SCREEN_W - AVATAR_W;

  for (uint8_t i = 0; i < ROWS_VISIBLE; i++) {
    RowUi &r = rows[i];

    r.root = lv_obj_create(scr);
    noPad(r.root);
    lv_obj_set_size(r.root, listW, ROW_H);
    lv_obj_set_pos(r.root, AVATAR_W, TOPBAR_H + i * ROW_H);
    if (i < ROWS_VISIBLE - 1) {
      lv_obj_set_style_border_side(r.root, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_color(r.root, C_LINE, 0);
      lv_obj_set_style_border_width(r.root, 1, 0);
    }

    r.bar = lv_obj_create(r.root);
    noPad(r.bar);
    lv_obj_set_size(r.bar, 6, ROW_H);
    lv_obj_set_pos(r.bar, 0, 0);
    lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);

    // El nombre manda y necesita sitio: ocupa la fila entera. El cronometro
    // baja a la segunda linea, donde compite con un texto secundario y no con
    // el titulo de la sesion. Antes ambos se peleaban por la misma linea y el
    // nombre se comia el hueco del reloj.
    // Tres lineas por fila: nombre arriba pegado, y debajo dos de detalle que
    // aprovechan el hueco que antes quedaba muerto. El cronometro vive en la
    // columna derecha, fuera del ancho del texto, asi que nunca se solapan.
    r.label = mkLabel(r.root, &lv_font_montserrat_16, C_TXT);
    lv_label_set_long_mode(r.label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(r.label, 15, 4);
    lv_obj_set_width(r.label, listW - 15 - 10);

    r.timer = mkLabel(r.root, &lv_font_montserrat_16, C_TXT);
    lv_obj_set_style_text_align(r.timer, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(r.timer, TIMER_W);
    lv_obj_set_pos(r.timer, listW - TIMER_W - 8, 28);

    r.detail = mkLabel(r.root, &lv_font_montserrat_12, C_SOFT);
    lv_label_set_long_mode(r.detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(r.detail, 15, 27);
    lv_obj_set_size(r.detail, listW - 15 - TIMER_W - 16, 40);

    // Toda la fila es el area tactil: 300x73, muy por encima del minimo de
    // 100x80 que exige el resistivo (el ancho compensa la altura).
    lv_obj_add_flag(r.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.root, rowClicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
  }
}

/**
 * Boton grande. El color del BORDE y el del TEXTO son parametros distintos a
 * proposito: VOLVER lleva borde gris oscuro, pero su texto tiene que ser
 * blanco. Cuando eran el mismo, el texto quedaba gris #2E2E2E sobre panel
 * #101010 y en el panel TN no se leia nada.
 */
static lv_obj_t *mkBigButton(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h, const char *text,
                             lv_color_t border, lv_color_t textColor,
                             lv_event_cb_t cb, void *user) {
  lv_obj_t *b = lv_obj_create(parent);
  noPad(b);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_color(b, C_PANEL, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(b, border, 0);
  lv_obj_set_style_border_width(b, 3, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);

  lv_obj_t *l = mkLabel(b, &lv_font_montserrat_20, textColor);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

static void showActions();
static void actionsClicked(lv_event_t *) { showActions(); }

/** Marca la seleccion. Sin esto no sabes que vas a lanzar al pulsar BOOT. */
static void paintActionChoice() {
  for (uint8_t i = 0; i < ACTION_COUNT; i++) {
    const bool on = (actChosen == (int8_t)i);
    lv_obj_set_style_bg_color(actBtn[i], on ? actColor[i] : C_PANEL, 0);
    lv_obj_set_style_border_width(actBtn[i], on ? 5 : 3, 0);
    lv_obj_t *l = lv_obj_get_child(actBtn[i], 0);
    if (l) lv_obj_set_style_text_color(l, on ? C_BG : actColor[i], 0);
  }

  const bool step1 = (actChosen < 0);

  if (step1) lv_obj_clear_flag(btnActBack, LV_OBJ_FLAG_HIDDEN);
  else       lv_obj_add_flag(btnActBack, LV_OBJ_FLAG_HIDDEN);
  for (lv_obj_t *o : { btnActOk, btnActCancel }) {
    if (step1) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }

  static char hint[64];
  if (step1) {
    setTextIfChanged(lblActResult, "toca una accion para elegirla");
    setColorIfChanged(lblActResult, C_DIM);
  } else {
    snprintf(hint, sizeof(hint), "confirma para lanzar %s", actLabel[actChosen]);
    setTextIfChanged(lblActResult, hint);
    setColorIfChanged(lblActResult, actColor[actChosen]);

    lv_obj_t *l = lv_obj_get_child(btnActOk, 0);
    if (l) {
      static char ok[32];
      snprintf(ok, sizeof(ok), "SI, %s", actLabel[actChosen]);
      lv_label_set_text(l, ok);
      lv_obj_set_style_text_color(l, C_BG, 0);
    }
    lv_obj_set_style_bg_color(btnActOk, actColor[actChosen], 0);
    lv_obj_set_style_border_color(btnActOk, actColor[actChosen], 0);
  }
}

static void actCancel(lv_event_t *) { actChosen = -1; paintActionChoice(); }
static void actGo(lv_event_t *)     { uiActionsConfirm(); }

/** El indice viaja como user_data; el id es una constante de flash. */
static void actionPicked(lv_event_t *e) {
  actChosen = (int8_t)(intptr_t)lv_event_get_user_data(e);
  paintActionChoice();
}

bool uiActionsConfirm() {
  if (!actVisible || actChosen < 0 || !detailId[0]) return false;

  netSendAction(detailId, actId[actChosen]);
  setTextIfChanged(lblActResult, "enviado, esperando al broker...");
  setColorIfChanged(lblActResult, C_DIM);
  actChosen = -1;
  paintActionChoice();
  return true;
}

static void actionsBack(lv_event_t *) {
  actVisible = false;
  actChosen  = -1;
  lv_screen_load(detailScr);
}

static void buildDetail() {
  detailScr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(detailScr, C_BG, 0);
  lv_obj_set_style_bg_opa(detailScr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(detailScr, 0, 0);
  lv_obj_set_style_border_width(detailScr, 0, 0);
  lv_obj_clear_flag(detailScr, LV_OBJ_FLAG_SCROLLABLE);

  dLabel = mkLabel(detailScr, &lv_font_montserrat_20, C_TXT);
  lv_obj_set_pos(dLabel, 14, 12);
  lv_obj_set_width(dLabel, SCREEN_W - 28);
  lv_label_set_long_mode(dLabel, LV_LABEL_LONG_DOT);

  dStatus = mkLabel(detailScr, &lv_font_montserrat_16, C_TXT);
  lv_obj_set_pos(dStatus, 14, 44);

  // Una herramienta de MCP puede llamarse
  // "mcp__plugin_playwright_playwright__browser_take_screenshot": alineada a
  // la derecha y sin ancho, se extendia por toda la pantalla y pisaba el
  // estado. Mitad de pantalla y recorte.
  dTool = mkLabel(detailScr, &lv_font_montserrat_14, C_DIM);
  lv_label_set_long_mode(dTool, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(dTool, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_width(dTool, 232);
  lv_obj_set_pos(dTool, SCREEN_W - 232 - 14, 46);

  // El comando completo, en varias lineas: es lo que vienes a leer.
  dDetail = mkLabel(detailScr, &lv_font_montserrat_14, C_SOFT);
  lv_obj_set_pos(dDetail, 14, 78);
  lv_obj_set_size(dDetail, SCREEN_W - 28, 86);
  lv_label_set_long_mode(dDetail, LV_LABEL_LONG_WRAP);

  dError = mkLabel(detailScr, &lv_font_montserrat_14, lv_color_hex(0xFF4B4B));
  lv_obj_set_pos(dError, 14, 168);
  lv_obj_set_width(dError, SCREEN_W - 28);
  lv_label_set_long_mode(dError, LV_LABEL_LONG_DOT);

  dMeta = mkLabel(detailScr, &lv_font_montserrat_12, C_DIM);
  lv_obj_set_pos(dMeta, 14, 194);
  lv_obj_set_width(dMeta, SCREEN_W - 28);

  // Dos botones de 222x76 a 14 px de los bordes. La calibracion medida da
  // hasta 12 px de error en las esquinas, asi que nada pegado al borde.
  mkBigButton(detailScr, 14,  SCREEN_H - 76 - 14, 222, 76, "ACCIONES",
              lv_color_hex(0x4DA6FF), lv_color_hex(0x4DA6FF), actionsClicked, nullptr);
  mkBigButton(detailScr, 244, SCREEN_H - 76 - 14, 222, 76, "VOLVER",
              C_BTN, C_TXT, backClicked, nullptr);
}

static void refreshDetail() {
  const Agent *a = agentById(detailId);

  if (!a) {                       // la sesion termino mientras mirabas
    setTextIfChanged(dLabel, "agente desaparecido");
    setColorIfChanged(dLabel, C_DIM);
    setTextIfChanged(dStatus, "");
    setTextIfChanged(dTool, "");
    setTextIfChanged(dDetail, "La sesion ha terminado y el broker ya la ha purgado.");
    setTextIfChanged(dError, "");
    setTextIfChanged(dMeta, "");
    return;
  }

  const AgentStatus st  = agentEffective(*a);
  const lv_color_t  col = lv_color_hex(agentColor(st));

  setTextIfChanged(dLabel, a->label);
  setColorIfChanged(dLabel, C_TXT);

  setTextIfChanged(dStatus, st == ST_STALLED ? "te espera" : agentStatusName(st));
  setColorIfChanged(dStatus, col);

  setTextIfChanged(dTool, a->tool[0] ? a->tool : "-");

  // Arriba lo explicativo y debajo el comando tal cual: la fila se lee de
  // reojo, pero al tocar vienes justamente a ver que se ha ejecutado.
  static char cuerpo[AG_DETAIL_LEN * 2 + 4];
  if (a->raw[0] && strcmp(a->raw, a->detail) != 0)
    snprintf(cuerpo, sizeof(cuerpo), "%s\n%s", a->detail, a->raw);
  else
    snprintf(cuerpo, sizeof(cuerpo), "%s", a->detail[0] ? a->detail : "(sin detalle)");
  setTextIfChanged(dDetail, cuerpo);

  if (a->lastError[0]) {
    static char e[AG_ERROR_LEN + 16];
    snprintf(e, sizeof(e), "ultimo error: %s", a->lastError);
    setTextIfChanged(dError, e);
  } else {
    setTextIfChanged(dError, "");
  }

  static char meta[96], el[10];
  fmtElapsed(el, sizeof(el), a->since);
  if (a->subN > 0)
    snprintf(meta, sizeof(meta), "id %s   %s   %u subagente%s activo%s",
             a->id, el, a->subN, a->subN == 1 ? "" : "s", a->subN == 1 ? "" : "s");
  else
    snprintf(meta, sizeof(meta), "id %s   en este estado %s", a->id, el);
  setTextIfChanged(dMeta, meta);
}

static void buildActions() {
  actionsScr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(actionsScr, C_BG, 0);
  lv_obj_set_style_bg_opa(actionsScr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(actionsScr, 0, 0);
  lv_obj_set_style_border_width(actionsScr, 0, 0);
  lv_obj_clear_flag(actionsScr, LV_OBJ_FLAG_SCROLLABLE);

  lblActAgent = mkLabel(actionsScr, &lv_font_montserrat_16, C_TXT);
  lv_obj_set_pos(lblActAgent, 14, 6);

  // 2x2 de 222x88: por encima del minimo de 100x80 del tactil resistivo.
  static const lv_coord_t X[ACTION_COUNT] = { 14, 244, 14, 244 };
  static const lv_coord_t Y[ACTION_COUNT] = { 34, 34, 130, 130 };
  actId[0]    = "continue"; actLabel[0] = "CONTINUA"; actColor[0] = lv_color_hex(0x24D65F);
  actId[1]    = "tests";    actLabel[1] = "TESTS";    actColor[1] = lv_color_hex(0x4DA6FF);
  actId[2]    = "status";   actLabel[2] = "ESTADO";   actColor[2] = lv_color_hex(0xB07CFF);
  actId[3]    = "interrupt";actLabel[3] = "PARAR";    actColor[3] = lv_color_hex(0xFF4B4B);

  for (uint8_t i = 0; i < ACTION_COUNT; i++) {
    actBtn[i] = mkBigButton(actionsScr, X[i], Y[i], 222, 88, actLabel[i],
                            actColor[i], actColor[i], actionPicked, (void *)(intptr_t)i);
  }

  lblActResult = mkLabel(actionsScr, &lv_font_montserrat_12, C_DIM);
  lv_obj_set_pos(lblActResult, 14, 224);
  lv_obj_set_width(lblActResult, SCREEN_W - 28);
  lv_label_set_long_mode(lblActResult, LV_LABEL_LONG_DOT);

  btnActBack = mkBigButton(actionsScr, 14, 246, SCREEN_W - 28, 64, "VOLVER",
                           C_BTN, C_TXT, actionsBack, nullptr);

  // Banda de confirmacion, en el sitio del VOLVER y lejos de los botones de
  // accion: para lanzar algo hay que tocar dos zonas distintas de la pantalla.
  btnActOk     = mkBigButton(actionsScr, 14,  246, 300, 64, "SI", C_BTN, C_BG,
                             actGo, nullptr);
  btnActCancel = mkBigButton(actionsScr, 318, 246, 148, 64, "NO", C_BTN, C_TXT,
                             actCancel, nullptr);
}

static void showActions() {
  const Agent *a = agentById(detailId);
  setTextIfChanged(lblActAgent, a ? a->label : detailId);
  netClearActionResult();
  actChosen  = -1;
  actVisible = true;
  paintActionChoice();
  lv_screen_load(actionsScr);
}

void uiAgentsCreate() {
  scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(scr, 0, 0);

  buildTopbar();
  buildAvatar();
  buildRows();
  buildDetail();
  buildActions();

  uiAgentsRefresh();
}

// --- actualizacion -----------------------------------------------------------

static void refreshTopbar() {
  static char buf[16];

  switch (netState()) {
    case NET_MQTT_UP:
      setTextIfChanged(lblMqtt, "MQTT");
      setColorIfChanged(lblMqtt, lv_color_hex(0x24D65F));
      break;
    case NET_WIFI_UP:
      snprintf(buf, sizeof(buf), "SIN MQTT");
      setTextIfChanged(lblMqtt, buf);
      setColorIfChanged(lblMqtt, lv_color_hex(0xFF4B4B));
      break;
    default:
      setTextIfChanged(lblMqtt, "SIN WIFI");
      setColorIfChanged(lblMqtt, lv_color_hex(0xFF4B4B));
      break;
  }

  if (netState() == NET_WIFI_DOWN) {
    setTextIfChanged(lblRssi, "");
  } else {
    snprintf(buf, sizeof(buf), "%d dBm", netRssi());
    setTextIfChanged(lblRssi, buf);
  }

  const time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    setTextIfChanged(lblClock, buf);
  } else {
    setTextIfChanged(lblClock, "--:--");
  }
}

static void refreshAvatar() {
  static const char *MOOD[ST_COUNT] = {
    "EN REPOSO", "PENSANDO", "TRABAJANDO", "PIDE PERMISO", "ERROR",
    "TE ESPERA", "SIN AGENTES",
  };

  const AgentStatus agg = agentsAggregate();
  const lv_color_t  col = lv_color_hex(agentColor(agg));

  avatarSetState(agg);

  setTextIfChanged(lblMood, MOOD[agg]);
  setColorIfChanged(lblMood, col);

  uint8_t total = 0, busy = 0;
  for (const auto &a : agents) {
    if (!a.used) continue;
    total++;
    const AgentStatus st = agentEffective(a);
    if (st == ST_WORKING || st == ST_THINKING) busy++;
  }

  static char buf[28];
  if (total == 0) snprintf(buf, sizeof(buf), "esperando eventos");
  else            snprintf(buf, sizeof(buf), "%u activos - %u en total", busy, total);
  setTextIfChanged(lblCount, buf);
}

static void refreshRows() {
  uint8_t idx[AGENTS_MAX];
  const uint8_t n = agentsVisible(idx, ROWS_VISIBLE);

  for (uint8_t i = 0; i < ROWS_VISIBLE; i++) {
    RowUi &r = rows[i];

    if (i >= n) {
      lv_obj_add_flag(r.root, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(r.root, LV_OBJ_FLAG_HIDDEN);

    const Agent      &a   = agents[idx[i]];
    const AgentStatus st  = agentEffective(a);
    const lv_color_t  col = lv_color_hex(agentColor(st));

    lv_obj_set_style_bg_color(r.bar, col, 0);
    setTextIfChanged(r.label, a.label);

    static char line[AG_TOOL_LEN + AG_DETAIL_LEN + 8];
    switch (st) {
      case ST_WAITING_PERMISSION:
        snprintf(line, sizeof(line), "ESPERA PERMISO - %s", a.tool);
        break;
      case ST_ERROR:
        snprintf(line, sizeof(line), "ERROR - %s", a.lastError[0] ? a.lastError : "?");
        break;
      case ST_OFFLINE:
        snprintf(line, sizeof(line), "SESION TERMINADA");
        break;
      case ST_IDLE:
        snprintf(line, sizeof(line), "EN REPOSO");
        break;
      case ST_STALLED:
        snprintf(line, sizeof(line), "TE ESPERA");
        break;
      default:
        // Si quien trabaja es un subagente, decirlo: la sesion padre puede
        // estar parada y aun asi la fila tiene que contar la verdad.
        if (a.subType[0] && a.detail[0])
          snprintf(line, sizeof(line), ">%s - %s", a.subType, a.detail);
        else if (a.subType[0])
          snprintf(line, sizeof(line), ">%s - %s", a.subType, a.tool);
        else if (a.detail[0])
          snprintf(line, sizeof(line), "%s - %s", a.tool, a.detail);
        else
          snprintf(line, sizeof(line), "%s", a.tool);
        break;
    }
    setTextIfChanged(r.detail, line);
    setColorIfChanged(r.detail, st == ST_WORKING ? C_SOFT : col);

    static char t[10];
    fmtElapsed(t, sizeof(t), a.since);
    setTextIfChanged(r.timer, t);
    setColorIfChanged(
        r.timer,
        (st == ST_ERROR || st == ST_WAITING_PERMISSION || st == ST_STALLED) ? col : C_TXT);
  }
}

void uiAgentsRefresh() {
  // Si estas mirando el detalle, se refresca eso: la lista no se ve.
  if (detailId[0]) {
    refreshDetail();
    return;
  }
  refreshTopbar();
  refreshAvatar();
  refreshRows();
}

void uiAgentsTickSlow() {
  avatarTick();

  // Resultado de la ultima accion, en cuanto lo publica el broker.
  const char *r = netLastActionResult();
  if (r && *r && actChosen < 0) {
    setTextIfChanged(lblActResult, r);
    setColorIfChanged(lblActResult, strncmp(r, "OK", 2) == 0 ? lv_color_hex(0x24D65F)
                                                             : lv_color_hex(0xFFB020));
  }

  uiAgentsRefresh();
}
