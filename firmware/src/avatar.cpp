#include "avatar.h"

#include <Arduino.h>

#include "config.h"

/**
 * Avatar procedural: dos rectangulos redondeados y nada mas. Sin imagenes, sin
 * fuentes, sin buffers extra. Todo el movimiento sale de lv_anim, que trabaja
 * sobre los estilos ya reservados: el heap no se mueve mientras anima.
 *
 * El objetivo no es que sea mono. Es que el estado del sistema se lea de reojo,
 * desde el otro lado de la mesa, sin enfocar la vista y sin leer texto.
 */

#define EYE_W      46
#define EYE_H      58
#define EYE_GAP    36     // separacion desde el centro
#define EYE_Y     (-34)

static lv_obj_t *eyeL, *eyeR;
static lv_anim_t animL, animR;

static AgentStatus current   = ST_COUNT;   // ninguno: fuerza el primer set
static bool        blinking  = false;
static uint32_t    nextBlink = 0;
static int16_t     openH     = EYE_H;      // altura "abierto" del estado actual

// --- utilidades de animacion -------------------------------------------------

static void setH(void *obj, int32_t v) {
  lv_obj_set_height((lv_obj_t *)obj, v);
  lv_obj_set_style_radius((lv_obj_t *)obj, v < 16 ? v / 2 : EYE_W / 2, 0);
}

static void setDx(void *obj, int32_t v) {
  const bool left = (obj == eyeL);
  lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, (left ? -EYE_GAP : EYE_GAP) + v, EYE_Y);
}

static void stopAnims() {
  lv_anim_delete(eyeL, setH);
  lv_anim_delete(eyeR, setH);
  lv_anim_delete(eyeL, setDx);
  lv_anim_delete(eyeR, setDx);
}

static void startAnim(lv_anim_t *a, lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                      int32_t from, int32_t to, uint32_t ms, bool pingpong) {
  lv_anim_init(a);
  lv_anim_set_var(a, obj);
  lv_anim_set_exec_cb(a, cb);
  lv_anim_set_values(a, from, to);
  lv_anim_set_duration(a, ms);
  lv_anim_set_path_cb(a, lv_anim_path_ease_in_out);
  if (pingpong) {
    lv_anim_set_playback_duration(a, ms);
    lv_anim_set_repeat_count(a, LV_ANIM_REPEAT_INFINITE);
  }
  lv_anim_start(a);
}

// --- construccion ------------------------------------------------------------

static lv_obj_t *mkEye(lv_obj_t *parent, int16_t dx) {
  lv_obj_t *e = lv_obj_create(parent);
  lv_obj_set_style_pad_all(e, 0, 0);
  lv_obj_set_style_border_width(e, 0, 0);
  lv_obj_clear_flag(e, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(e, EYE_W, EYE_H);
  lv_obj_set_style_radius(e, EYE_W / 2, 0);
  lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
  lv_obj_align(e, LV_ALIGN_CENTER, dx, EYE_Y);
  return e;
}

void avatarCreate(lv_obj_t *parent) {
  eyeL = mkEye(parent, -EYE_GAP);
  eyeR = mkEye(parent, EYE_GAP);
  avatarSetState(ST_OFFLINE);
}

// --- estados -----------------------------------------------------------------

void avatarSetState(AgentStatus s) {
  if (s == current) return;
  current  = s;
  blinking = false;
  stopAnims();

  const lv_color_t col = lv_color_hex(agentColor(s));
  lv_obj_set_style_bg_color(eyeL, col, 0);
  lv_obj_set_style_bg_color(eyeR, col, 0);

  setDx(eyeL, 0);
  setDx(eyeR, 0);

  switch (s) {
    case ST_IDLE:
      // Abiertos y quietos. Solo parpadea, muy de tarde en tarde.
      openH = EYE_H;
      setH(eyeL, openH); setH(eyeR, openH);
      nextBlink = millis() + random(2500, 6000);
      break;

    case ST_THINKING:
      // Entornados y mirando de lado: la mirada perdida de quien esta pensando.
      openH = 34;
      setH(eyeL, openH); setH(eyeR, openH);
      startAnim(&animL, eyeL, setDx, -9, 9, 1400, true);
      startAnim(&animR, eyeR, setDx, -9, 9, 1400, true);
      nextBlink = millis() + random(3000, 7000);
      break;

    case ST_WORKING:
      // Abiertos y parpadeo rapido: hay actividad.
      openH = EYE_H;
      setH(eyeL, openH); setH(eyeR, openH);
      nextBlink = millis() + random(900, 2200);
      break;

    case ST_WAITING_PERMISSION:
      // Muy abiertos y SIN parpadear: te esta mirando fijamente. El latido
      // lento de tamano es lo que engancha la vista periferica.
      openH = 66;
      startAnim(&animL, eyeL, setH, 58, 70, 700, true);
      startAnim(&animR, eyeR, setH, 58, 70, 700, true);
      nextBlink = 0;
      break;

    case ST_ERROR:
      // Apretados y con temblor lateral. Se lee mal como "algo va mal", que es
      // justo lo que queremos.
      openH = 22;
      setH(eyeL, openH); setH(eyeR, openH);
      startAnim(&animL, eyeL, setDx, -3, 3, 110, true);
      startAnim(&animR, eyeR, setDx, 3, -3, 110, true);
      nextBlink = 0;
      break;

    default:  // ST_OFFLINE
      openH = 7;
      setH(eyeL, openH); setH(eyeR, openH);
      nextBlink = 0;
      break;
  }
}

void avatarTick() {
  if (nextBlink == 0) return;              // estados que no parpadean

  const uint32_t now = millis();
  if (now < nextBlink) return;

  blinking = !blinking;
  const int16_t h = blinking ? 6 : openH;
  setH(eyeL, h);
  setH(eyeR, h);

  // Un parpadeo dura poco; el intervalo hasta el siguiente es aleatorio para
  // que no parezca un metronomo.
  if (blinking) {
    nextBlink = now + 110;
  } else {
    switch (current) {
      case ST_WORKING:  nextBlink = now + random(900, 2200);  break;
      case ST_THINKING: nextBlink = now + random(3000, 7000); break;
      default:          nextBlink = now + random(2500, 6000); break;
    }
  }
}
