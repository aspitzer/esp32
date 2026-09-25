#include "ui_permission.h"

#include <lvgl.h>
#include <time.h>

#include "config.h"
#include "net.h"
#include "ui_agents.h"

/**
 * Confirmacion en dos pasos, toda tactil.
 *
 * El tactil resistivo se dispara con el dedo plano y aqui un acierto casual
 * ejecuta un comando destructivo, asi que un solo toque no puede decidir. Pero
 * exigir un boton fisico en un aparato tactil es un mal aparato.
 *
 * La proteccion sale de la GEOMETRIA: el boton de confirmar aparece en el lado
 * IZQUIERDO, donde estaba DENEGAR, y cancelar ocupa el derecho, donde estaba
 * PERMITIR. Asi un doble toque en el mismo sitio nunca aprueba: repetir sobre
 * PERMITIR cancela, y repetir sobre DENEGAR confirma una denegacion, que es
 * inofensiva.
 *
 * BOOT sigue funcionando como atajo, pero ya no hace falta para nada.
 */
enum Choice : uint8_t { CH_NONE = 0, CH_DENY, CH_ALLOW };

static lv_obj_t *scr;
static lv_obj_t *header, *lblRisk, *lblAgent, *lblTool, *lblLeft;
static lv_obj_t *box, *lblCmd, *barBg, *barFill;
static lv_obj_t *btnDeny, *btnAllow, *lblDeny, *lblAllow, *lblHint;
static lv_obj_t *btnConfirm, *btnCancel, *lblConfirm, *lblCancel;

static Choice   choice     = CH_NONE;
static bool     active     = false;
static uint32_t totalSecs  = 90;

#define C_DENY   lv_color_hex(0xFF4B4B)
#define C_ALLOW  lv_color_hex(0x24D65F)
#define C_TXT    lv_color_hex(0xFFFFFF)
#define C_DIM    lv_color_hex(0x9A9A9A)
#define C_BLACK  lv_color_hex(0x000000)

static lv_color_t riskColor(PermRisk r) {
  switch (r) {
    case RISK_LOW:    return lv_color_hex(0x4DA6FF);
    case RISK_MEDIUM: return lv_color_hex(0xFFB020);
    default:          return C_DENY;
  }
}

static const char *riskName(PermRisk r) {
  switch (r) {
    case RISK_LOW:    return "PERMISO \xC2\xB7 RIESGO BAJO";
    case RISK_MEDIUM: return "PERMISO \xC2\xB7 RIESGO MEDIO";
    default:          return "PERMISO \xC2\xB7 RIESGO ALTO";
  }
}

bool uiPermissionActive() { return active; }

/** Alterna entre el paso de elegir y el de confirmar. */
static void paintChoice() {
  const bool step1 = (choice == CH_NONE);
  const bool deny  = (choice == CH_DENY);

  for (lv_obj_t *o : { btnDeny, btnAllow }) {
    if (step1) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  for (lv_obj_t *o : { btnConfirm, btnCancel }) {
    if (step1) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }

  if (step1) {
    lv_label_set_text(lblHint, "TOCA PARA ELEGIR");
    lv_obj_set_style_text_color(lblHint, C_DIM, 0);
    return;
  }

  const lv_color_t col = deny ? C_DENY : C_ALLOW;
  lv_label_set_text(lblConfirm, deny ? "SI, DENEGAR" : "SI, PERMITIR");
  lv_obj_set_style_bg_color(btnConfirm, col, 0);
  lv_obj_set_style_border_color(btnConfirm, col, 0);
  lv_obj_set_style_text_color(lblConfirm, C_BLACK, 0);

  lv_label_set_text(lblHint, deny ? "confirma para DENEGAR" : "confirma para PERMITIR");
  lv_obj_set_style_text_color(lblHint, col, 0);
}

static void pickDeny(lv_event_t *)  { choice = CH_DENY;  paintChoice(); }
static void pickAllow(lv_event_t *) { choice = CH_ALLOW; paintChoice(); }
static void cancelChoice(lv_event_t *) { choice = CH_NONE; paintChoice(); }
static void doConfirm(lv_event_t *)    { uiPermissionConfirm(); }

static lv_obj_t *mkButton(lv_coord_t x, lv_color_t color, lv_event_cb_t cb, lv_obj_t **outLabel,
                          const char *text, lv_coord_t w = 222) {
  lv_obj_t *b = lv_obj_create(scr);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_radius(b, 0, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(b, w, 84);
  lv_obj_set_pos(b, x, 200);
  lv_obj_set_style_bg_color(b, C_BLACK, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(b, color, 0);
  lv_obj_set_style_border_width(b, 3, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  *outLabel = l;
  return b;
}

void uiPermissionCreate() {
  scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, C_BLACK, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  header = lv_obj_create(scr);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(header, SCREEN_W, 40);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

  lblRisk = lv_label_create(header);
  lv_obj_set_style_text_font(lblRisk, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblRisk, C_BLACK, 0);
  lv_obj_align(lblRisk, LV_ALIGN_LEFT_MID, 12, 0);

  lblAgent = lv_label_create(header);
  lv_obj_set_style_text_font(lblAgent, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblAgent, C_BLACK, 0);
  lv_obj_align(lblAgent, LV_ALIGN_RIGHT_MID, -12, 0);

  lblTool = lv_label_create(scr);
  lv_obj_set_style_text_font(lblTool, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblTool, C_DIM, 0);
  lv_obj_set_pos(lblTool, 12, 48);

  lblLeft = lv_label_create(scr);
  lv_obj_set_style_text_font(lblLeft, &lv_font_montserrat_14, 0);
  lv_obj_align(lblLeft, LV_ALIGN_TOP_RIGHT, -12, 46);

  // El comando entero, legible. Es lo unico que importa de esta pantalla.
  box = lv_obj_create(scr);
  lv_obj_set_style_pad_all(box, 10, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(box, SCREEN_W - 24, 102);
  lv_obj_set_pos(box, 12, 68);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x141414), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 2, 0);

  lblCmd = lv_label_create(box);
  lv_obj_set_style_text_font(lblCmd, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblCmd, C_TXT, 0);
  lv_obj_set_width(lblCmd, SCREEN_W - 24 - 20);
  lv_label_set_long_mode(lblCmd, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(lblCmd, 0, 0);

  barBg = lv_obj_create(scr);
  lv_obj_set_style_pad_all(barBg, 0, 0);
  lv_obj_set_style_radius(barBg, 0, 0);
  lv_obj_set_style_border_width(barBg, 0, 0);
  lv_obj_clear_flag(barBg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(barBg, SCREEN_W - 24, 5);
  lv_obj_set_pos(barBg, 12, 180);
  lv_obj_set_style_bg_color(barBg, lv_color_hex(0x2E2E2E), 0);
  lv_obj_set_style_bg_opa(barBg, LV_OPA_COVER, 0);

  barFill = lv_obj_create(barBg);
  lv_obj_set_style_pad_all(barFill, 0, 0);
  lv_obj_set_style_radius(barFill, 0, 0);
  lv_obj_set_style_border_width(barFill, 0, 0);
  lv_obj_set_size(barFill, SCREEN_W - 24, 5);
  lv_obj_set_pos(barFill, 0, 0);
  lv_obj_set_style_bg_opa(barFill, LV_OPA_COVER, 0);

  btnDeny  = mkButton(12,  C_DENY,  pickDeny,  &lblDeny,  "DENEGAR");
  btnAllow = mkButton(246, C_ALLOW, pickAllow, &lblAllow, "PERMITIR");

  // Confirmar a la IZQUIERDA (donde estaba DENEGAR) y cancelar a la derecha
  // (donde estaba PERMITIR): repetir el toque en el mismo sitio jamas aprueba.
  btnConfirm = mkButton(12,  C_ALLOW, doConfirm,    &lblConfirm, "SI", 300);
  btnCancel  = mkButton(320, C_DIM,   cancelChoice, &lblCancel,  "CANCELAR", 148);

  lblHint = lv_label_create(scr);
  lv_obj_set_style_text_font(lblHint, &lv_font_montserrat_14, 0);
  lv_obj_align(lblHint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void uiPermissionShow() {
  const PermReq *p = netPendingPerm();
  if (!p) return;

  const lv_color_t col = riskColor(p->risk);

  lv_obj_set_style_bg_color(header, col, 0);
  lv_label_set_text(lblRisk, riskName(p->risk));
  lv_label_set_text(lblAgent, p->agentId);
  lv_label_set_text(lblTool, p->tool);
  lv_label_set_text(lblCmd, p->summary);
  lv_obj_set_style_border_color(box, col, 0);
  lv_obj_set_style_bg_color(barFill, col, 0);
  lv_obj_set_style_text_color(lblLeft, col, 0);

  const time_t now = time(nullptr);
  totalSecs = (p->expiresAt > (uint32_t)now && now > 1700000000) ? p->expiresAt - now : 90;

  choice = CH_NONE;
  paintChoice();
  active = true;
  lv_screen_load(scr);
  Serial.printf("[ui  ] modal de permiso: %s\n", p->summary);
}

void uiPermissionTick() {
  if (!active) return;

  const PermReq *p = netPendingPerm();
  if (!p) {                       // resuelta o caducada por otro camino
    active = false;
    uiAgentsShowList();
    return;
  }

  const time_t now = time(nullptr);
  uint32_t left;
  if (p->expiresAt && now > 1700000000) {
    left = (p->expiresAt > (uint32_t)now) ? p->expiresAt - now : 0;
  } else {
    // Sin hora valida se cuenta desde la llegada; el broker corta a los 90 s.
    const uint32_t elapsed = (millis() - p->rxMillis) / 1000;
    left = elapsed >= totalSecs ? 0 : totalSecs - elapsed;
  }

  static char buf[16];
  snprintf(buf, sizeof(buf), "%u s", left);
  lv_label_set_text(lblLeft, buf);

  const lv_coord_t full = SCREEN_W - 24;
  lv_coord_t w = totalSecs ? (lv_coord_t)((uint32_t)full * left / totalSecs) : 0;
  if (w < 0) w = 0;
  lv_obj_set_width(barFill, w);

  if (left == 0) {
    // No se responde nada: el broker ya habra caido a unspecified y la sesion
    // sigue por el flujo normal. Responder tarde solo confundiria.
    Serial.println("[perm] caducada en la placa, volviendo a la lista");
    netDropPerm();
    active = false;
    uiAgentsShowList();
  }
}

bool uiPermissionConfirm() {
  if (!active || choice == CH_NONE) return false;

  netAnswerPerm(choice == CH_ALLOW);
  active = false;
  choice = CH_NONE;
  uiAgentsShowList();
  return true;
}
