#include "avatar.h"

#include <Arduino.h>

#include "config.h"

/**
 * Cara procedural: dos ojos, dos cejas y una boca. Seis objetos LVGL, cero
 * assets, cero imagenes. Todo el movimiento sale de lv_anim sobre estilos ya
 * reservados, asi que el heap no se mueve mientras anima.
 *
 * Las cejas y la boca no son decoracion. Con solo ojos, "pensando" y
 * "esperandote" se parecian demasiado: el angulo de una ceja distingue dos
 * estados mucho mas rapido que un cambio de altura de parpado, y se lee desde
 * el otro lado de la mesa sin enfocar.
 */

#define EYE_W      46
#define EYE_H      58
#define EYE_GAP    36     // separacion desde el centro
#define EYE_Y     (-52)

#define BROW_W     40
#define BROW_H      8
// Separadas del barrido del ojo por el mismo motivo que la boca: si la caja
// del ojo al latir toca la de la ceja, LVGL repinta las dos.
#define BROW_Y   (-106)

// El arco tiene que quedar FUERA del area que barre el pulso del ojo. Si las
// cajas se tocan, LVGL vuelve a rasterizar el arco en cada frame del latido y
// eso solo costaba 4 fps. Ojo mas bajo con el pulso: -52+35 = -17; arco
// arriba: 34-34 = 0. Quedan 17 px de aire.
#define MOUTH_D    68     // diametro del arco
#define MOUTH_Y     34

static lv_obj_t *eyeL, *eyeR, *browL, *browR, *mouth, *mouthO;
static lv_anim_t animL, animR;

static AgentStatus current   = ST_COUNT;   // ninguno: fuerza el primer set
static bool        blinking  = false;
static uint32_t    nextBlink = 0;
static int16_t     openH     = EYE_H;

// --- animacion ---------------------------------------------------------------

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

// --- piezas ------------------------------------------------------------------

static lv_obj_t *mkBlob(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t r) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

/**
 * Las cejas giran sobre su centro. Grados en decimas: LVGL usa 1/10 de grado.
 * Positivo = horario, asi que en la ceja izquierda el extremo interno baja con
 * valores positivos y en la derecha con negativos. De ahi que casi siempre
 * vayan con el signo cambiado.
 */
static void setBrows(int16_t degL, int16_t degR, int16_t dy, bool visible) {
  if (!visible) {
    lv_obj_add_flag(browL, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(browR, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(browL, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(browR, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_transform_rotation(browL, degL * 10, 0);
  lv_obj_set_style_transform_rotation(browR, degR * 10, 0);
  lv_obj_align(browL, LV_ALIGN_CENTER, -EYE_GAP, BROW_Y + dy);
  lv_obj_align(browR, LV_ALIGN_CENTER,  EYE_GAP, BROW_Y + dy);
}

/**
 * Boca. El arco cubre casi todo: sonrisa, mueca y linea recta son el mismo
 * objeto con otros angulos. La "o" de sorpresa es un circulo aparte porque un
 * arco cerrado no queda igual de limpio.
 *
 * Angulos de LVGL: 0 grados a las 3 en punto y crecen en sentido horario, asi
 * que abajo son 90. Sonrisa = arco alrededor de 90; mueca = alrededor de 270.
 */
static void setMouth(uint16_t start, uint16_t end, bool round, bool visible) {
  if (visible) {
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_bg_angles(mouth, start, end);
  } else {
    lv_obj_add_flag(mouth, LV_OBJ_FLAG_HIDDEN);
  }
  if (round) lv_obj_clear_flag(mouthO, LV_OBJ_FLAG_HIDDEN);
  else       lv_obj_add_flag(mouthO, LV_OBJ_FLAG_HIDDEN);
}

void avatarCreate(lv_obj_t *parent) {
  eyeL = mkBlob(parent, EYE_W, EYE_H, EYE_W / 2);
  eyeR = mkBlob(parent, EYE_W, EYE_H, EYE_W / 2);
  lv_obj_align(eyeL, LV_ALIGN_CENTER, -EYE_GAP, EYE_Y);
  lv_obj_align(eyeR, LV_ALIGN_CENTER,  EYE_GAP, EYE_Y);

  browL = mkBlob(parent, BROW_W, BROW_H, BROW_H / 2);
  browR = mkBlob(parent, BROW_W, BROW_H, BROW_H / 2);
  for (lv_obj_t *b : { browL, browR }) {
    lv_obj_set_style_transform_pivot_x(b, BROW_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, BROW_H / 2, 0);
  }

  mouth = lv_arc_create(parent);
  lv_obj_remove_style(mouth, nullptr, LV_PART_KNOB);
  lv_obj_remove_style(mouth, nullptr, LV_PART_INDICATOR);
  lv_obj_clear_flag(mouth, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(mouth, MOUTH_D, MOUTH_D);
  lv_obj_set_style_arc_width(mouth, 7, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(mouth, true, LV_PART_MAIN);
  lv_obj_align(mouth, LV_ALIGN_CENTER, 0, MOUTH_Y);

  mouthO = mkBlob(parent, 28, 28, 14);
  lv_obj_align(mouthO, LV_ALIGN_CENTER, 0, MOUTH_Y + 8);

  avatarSetState(ST_OFFLINE);
}

// --- estados -----------------------------------------------------------------

void avatarSetState(AgentStatus s) {
  if (s == current) return;
  current  = s;
  blinking = false;
  stopAnims();

  const lv_color_t col = lv_color_hex(agentColor(s));
  for (lv_obj_t *o : { eyeL, eyeR, browL, browR, mouthO })
    lv_obj_set_style_bg_color(o, col, 0);
  lv_obj_set_style_arc_color(mouth, col, LV_PART_MAIN);

  setDx(eyeL, 0);
  setDx(eyeR, 0);

  switch (s) {
    case ST_IDLE:
      // Tranquilo. Cejas rectas, boca discreta, parpadeo espaciado.
      openH = EYE_H;
      setH(eyeL, openH); setH(eyeR, openH);
      setBrows(0, 0, 0, true);
      setMouth(72, 108, false, true);
      nextBlink = millis() + random(2500, 6000);
      break;

    case ST_THINKING:
      // Una ceja levantada y la mirada vagando: la cara de estar dandole vueltas.
      openH = 34;
      setH(eyeL, openH); setH(eyeR, openH);
      setBrows(-14, -4, -4, true);
      setMouth(60, 96, false, true);
      startAnim(&animL, eyeL, setDx, -9, 9, 1400, true);
      startAnim(&animR, eyeR, setDx, -9, 9, 1400, true);
      nextBlink = millis() + random(3000, 7000);
      break;

    case ST_WORKING:
      // Contento y despierto: cejas altas y sonrisa. Parpadeo rapido.
      openH = EYE_H;
      setH(eyeL, openH); setH(eyeR, openH);
      setBrows(0, 0, -6, true);
      setMouth(25, 155, false, true);
      nextBlink = millis() + random(900, 2200);
      break;

    case ST_WAITING_PERMISSION:
      // Sorpresa: ojos muy abiertos SIN parpadear, cejas arriba y boca en "o".
      // El latido lento es lo que engancha la vision periferica.
      openH = 66;
      setBrows(0, 0, -14, true);
      setMouth(0, 0, true, false);
      // 58..68 en vez de 58..70: doce pixeles de barrido costaban frames y el
      // latido se lee igual de bien con diez.
      startAnim(&animL, eyeL, setH, 58, 68, 700, true);
      startAnim(&animR, eyeR, setH, 58, 68, 700, true);
      nextBlink = 0;
      break;

    case ST_STALLED:
      // Aburrido de esperarte: parpados a media asta, cejas caidas por fuera,
      // boca plana y una deriva muy lenta de la mirada.
      openH = 26;
      setH(eyeL, openH); setH(eyeR, openH);
      setBrows(-12, 12, 6, true);
      setMouth(80, 100, false, true);
      startAnim(&animL, eyeL, setDx, -5, 5, 3200, true);
      startAnim(&animR, eyeR, setDx, -5, 5, 3200, true);
      nextBlink = millis() + random(4000, 9000);
      break;

    case ST_ERROR:
      // Enfadado: cejas caidas hacia dentro, mueca, y temblor lateral.
      openH = 22;
      setH(eyeL, openH); setH(eyeR, openH);
      setBrows(16, -16, 4, true);
      setMouth(205, 335, false, true);
      startAnim(&animL, eyeL, setDx, -3, 3, 110, true);
      startAnim(&animR, eyeR, setDx, 3, -3, 110, true);
      nextBlink = 0;
      break;

    default:  // ST_OFFLINE — dormido: dos rayas y nada mas
      openH = 7;
      setH(eyeL, openH); setH(eyeR, openH);
      setBrows(0, 0, 0, false);
      setMouth(0, 0, false, false);
      nextBlink = 0;
      break;
  }
}

void avatarTick() {
  if (nextBlink == 0) return;              // estados que no parpadean

  const uint32_t now = millis();
  if (now < nextBlink) return;

  blinking = !blinking;
  setH(eyeL, blinking ? 6 : openH);
  setH(eyeR, blinking ? 6 : openH);

  // Intervalo aleatorio para que no parezca un metronomo.
  if (blinking) {
    nextBlink = now + 110;
  } else {
    switch (current) {
      case ST_WORKING:  nextBlink = now + random(900, 2200);  break;
      case ST_THINKING: nextBlink = now + random(3000, 7000); break;
      case ST_STALLED:  nextBlink = now + random(4000, 9000); break;
      default:          nextBlink = now + random(2500, 6000); break;
    }
  }
}
