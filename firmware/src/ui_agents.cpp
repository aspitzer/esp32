#include "ui_agents.h"

#include <WiFi.h>
#include <lvgl.h>
#include <time.h>

#include "agents.h"
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

#define ROW_H ((SCREEN_H - TOPBAR_H) / ROWS_VISIBLE)

struct RowUi {
  lv_obj_t *root;
  lv_obj_t *bar;
  lv_obj_t *label;
  lv_obj_t *detail;
  lv_obj_t *timer;
};

static lv_obj_t *scr;
static lv_obj_t *lblMqtt, *lblRssi, *lblClock;
static lv_obj_t *eyeL, *eyeR, *lblMood, *lblCount;
static RowUi     rows[ROWS_VISIBLE];

static bool     eyesClosed   = false;
static uint32_t nextBlinkAt  = 0;

// --- pantalla de detalle -----------------------------------------------------
// Se entra tocando una fila. El id se guarda, no el indice: el agente puede
// cambiar de slot entre repintados y acabarias mirando otro.
static lv_obj_t *detailScr;
static lv_obj_t *dLabel, *dStatus, *dTool, *dDetail, *dMeta, *dError;
static char      detailId[AG_ID_LEN] = {0};

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

  eyeL = lv_obj_create(panel);
  noPad(eyeL);
  lv_obj_set_size(eyeL, 46, 58);
  lv_obj_set_style_radius(eyeL, 23, 0);
  lv_obj_set_style_bg_opa(eyeL, LV_OPA_COVER, 0);
  lv_obj_align(eyeL, LV_ALIGN_CENTER, -36, -34);

  eyeR = lv_obj_create(panel);
  noPad(eyeR);
  lv_obj_set_size(eyeR, 46, 58);
  lv_obj_set_style_radius(eyeR, 23, 0);
  lv_obj_set_style_bg_opa(eyeR, LV_OPA_COVER, 0);
  lv_obj_align(eyeR, LV_ALIGN_CENTER, 36, -34);

  lblMood = mkLabel(panel, &lv_font_montserrat_16, C_TXT);
  lv_obj_align(lblMood, LV_ALIGN_CENTER, 0, 34);

  lblCount = mkLabel(panel, &lv_font_montserrat_12, C_DIM);
  lv_obj_align(lblCount, LV_ALIGN_CENTER, 0, 58);
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

    r.label = mkLabel(r.root, &lv_font_montserrat_16, C_TXT);
    lv_obj_set_pos(r.label, 15, 9);
    lv_obj_set_width(r.label, listW - 15 - 62);
    lv_label_set_long_mode(r.label, LV_LABEL_LONG_DOT);

    r.detail = mkLabel(r.root, &lv_font_montserrat_12, C_SOFT);
    lv_obj_set_pos(r.detail, 15, 33);
    lv_obj_set_width(r.detail, listW - 15 - 62);
    lv_label_set_long_mode(r.detail, LV_LABEL_LONG_DOT);

    r.timer = mkLabel(r.root, &lv_font_montserrat_16, C_TXT);
    lv_obj_align(r.timer, LV_ALIGN_RIGHT_MID, -8, 0);

    // Toda la fila es el area tactil: 300x73, muy por encima del minimo de
    // 100x80 que exige el resistivo (el ancho compensa la altura).
    lv_obj_add_flag(r.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.root, rowClicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
  }
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

  dTool = mkLabel(detailScr, &lv_font_montserrat_14, C_DIM);
  lv_obj_align(dTool, LV_ALIGN_TOP_RIGHT, -14, 46);

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

  // Boton de volver: 452x76, a 14 px de los bordes. La calibracion medida da
  // hasta 12 px de error en las esquinas, asi que nada pegado al borde.
  lv_obj_t *back = lv_obj_create(detailScr);
  noPad(back);
  lv_obj_set_size(back, SCREEN_W - 28, 76);
  lv_obj_set_pos(back, 14, SCREEN_H - 76 - 14);
  lv_obj_set_style_bg_color(back, C_PANEL, 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(back, C_EDGE, 0);
  lv_obj_set_style_border_width(back, 3, 0);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, backClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *backTxt = mkLabel(back, &lv_font_montserrat_20, C_TXT);
  lv_label_set_text(backTxt, "VOLVER");
  lv_obj_center(backTxt);
}

static void refreshDetail() {
  const Agent *a = agentById(detailId);

  if (!a) {                       // la sesion termino mientras mirabas
    lv_label_set_text(dLabel, "agente desaparecido");
    lv_obj_set_style_text_color(dLabel, C_DIM, 0);
    lv_label_set_text(dStatus, "");
    lv_label_set_text(dTool, "");
    lv_label_set_text(dDetail, "La sesion ha terminado y el broker ya la ha purgado.");
    lv_label_set_text(dError, "");
    lv_label_set_text(dMeta, "");
    return;
  }

  const lv_color_t col = lv_color_hex(agentColor(a->status));

  lv_label_set_text(dLabel, a->label);
  lv_obj_set_style_text_color(dLabel, C_TXT, 0);

  lv_label_set_text(dStatus, agentStatusName(a->status));
  lv_obj_set_style_text_color(dStatus, col, 0);

  lv_label_set_text(dTool, a->tool[0] ? a->tool : "-");
  lv_label_set_text(dDetail, a->detail[0] ? a->detail : "(sin detalle)");

  if (a->lastError[0]) {
    static char e[AG_ERROR_LEN + 16];
    snprintf(e, sizeof(e), "ultimo error: %s", a->lastError);
    lv_label_set_text(dError, e);
  } else {
    lv_label_set_text(dError, "");
  }

  static char meta[64], el[10];
  fmtElapsed(el, sizeof(el), a->since);
  snprintf(meta, sizeof(meta), "id %s   en este estado %s", a->id, el);
  lv_label_set_text(dMeta, meta);
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

  uiAgentsRefresh();
}

// --- actualizacion -----------------------------------------------------------

static void refreshTopbar() {
  static char buf[16];

  switch (netState()) {
    case NET_MQTT_UP:
      lv_label_set_text(lblMqtt, "MQTT");
      lv_obj_set_style_text_color(lblMqtt, lv_color_hex(0x24D65F), 0);
      break;
    case NET_WIFI_UP:
      snprintf(buf, sizeof(buf), "SIN MQTT");
      lv_label_set_text(lblMqtt, buf);
      lv_obj_set_style_text_color(lblMqtt, lv_color_hex(0xFF4B4B), 0);
      break;
    default:
      lv_label_set_text(lblMqtt, "SIN WIFI");
      lv_obj_set_style_text_color(lblMqtt, lv_color_hex(0xFF4B4B), 0);
      break;
  }

  if (netState() == NET_WIFI_DOWN) {
    lv_label_set_text(lblRssi, "");
  } else {
    snprintf(buf, sizeof(buf), "%d dBm", netRssi());
    lv_label_set_text(lblRssi, buf);
  }

  const time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(lblClock, buf);
  } else {
    lv_label_set_text(lblClock, "--:--");
  }
}

static void refreshAvatar() {
  static const char *MOOD[ST_COUNT] = {
    "EN REPOSO", "PENSANDO", "TRABAJANDO", "PIDE PERMISO", "ERROR", "SIN AGENTES",
  };

  const AgentStatus agg = agentsAggregate();
  const lv_color_t  col = lv_color_hex(agentColor(agg));

  lv_obj_set_style_bg_color(eyeL, col, 0);
  lv_obj_set_style_bg_color(eyeR, col, 0);

  // Ojos cerrados = dos lineas planas. Tambien es el estado offline.
  const bool flat = eyesClosed || agg == ST_OFFLINE;
  lv_obj_set_height(eyeL, flat ? 7 : 58);
  lv_obj_set_height(eyeR, flat ? 7 : 58);
  lv_obj_set_style_radius(eyeL, flat ? 4 : 23, 0);
  lv_obj_set_style_radius(eyeR, flat ? 4 : 23, 0);

  lv_label_set_text(lblMood, MOOD[agg]);
  lv_obj_set_style_text_color(lblMood, col, 0);

  uint8_t total = 0, busy = 0;
  for (const auto &a : agents) {
    if (!a.used) continue;
    total++;
    if (a.status == ST_WORKING || a.status == ST_THINKING) busy++;
  }

  static char buf[28];
  if (total == 0) snprintf(buf, sizeof(buf), "esperando eventos");
  else            snprintf(buf, sizeof(buf), "%u activos \xC2\xB7 %u en total", busy, total);
  lv_label_set_text(lblCount, buf);
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

    const Agent     &a   = agents[idx[i]];
    const lv_color_t col = lv_color_hex(agentColor(a.status));

    lv_obj_set_style_bg_color(r.bar, col, 0);
    lv_label_set_text(r.label, a.label);

    static char line[AG_TOOL_LEN + AG_DETAIL_LEN + 8];
    switch (a.status) {
      case ST_WAITING_PERMISSION:
        snprintf(line, sizeof(line), "ESPERA PERMISO \xC2\xB7 %s", a.tool);
        break;
      case ST_ERROR:
        snprintf(line, sizeof(line), "ERROR \xC2\xB7 %s", a.lastError[0] ? a.lastError : "?");
        break;
      case ST_OFFLINE:
        snprintf(line, sizeof(line), "SESION TERMINADA");
        break;
      case ST_IDLE:
        snprintf(line, sizeof(line), "EN REPOSO");
        break;
      default:
        if (a.detail[0]) snprintf(line, sizeof(line), "%s \xC2\xB7 %s", a.tool, a.detail);
        else             snprintf(line, sizeof(line), "%s", a.tool);
        break;
    }
    lv_label_set_text(r.detail, line);
    lv_obj_set_style_text_color(r.detail, a.status == ST_WORKING ? C_SOFT : col, 0);

    static char t[10];
    fmtElapsed(t, sizeof(t), a.since);
    lv_label_set_text(r.timer, t);
    lv_obj_set_style_text_color(
        r.timer, (a.status == ST_ERROR || a.status == ST_WAITING_PERMISSION) ? col : C_TXT, 0);
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
  const uint32_t now = millis();
  const AgentStatus agg = agentsAggregate();

  // Parpadeo con intervalo aleatorio. Esperando permiso no parpadea: mira fijo.
  if (agg != ST_WAITING_PERMISSION && agg != ST_OFFLINE) {
    if (now >= nextBlinkAt) {
      eyesClosed  = !eyesClosed;
      nextBlinkAt = now + (eyesClosed ? 120 : random(1800, 5200));
    }
  } else if (eyesClosed) {
    eyesClosed = false;
  }

  uiAgentsRefresh();
}
