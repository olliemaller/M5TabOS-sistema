// SCREEN ORIENTATION - see orientacao.hpp.
#include "orientacao.hpp"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tela_virtual.hpp"
#include "tab5_imu.hpp"
#include "tab5_teclado.hpp"
#include "tab5_hora.hpp"   // tab5_agora_ms - the same clock as the UI loop
#include "esp_log.h"

static const char* TAG = "orientacao";

// The two M5GFX rotations that exist in the system. 3 and not 1: it is the one that puts
// ExtPort1 - where the keyboard docks - on the bottom edge, next to whoever
// is typing. With 1 the screen was upside down relative to the keyboard.
static const uint8_t ROT_EM_PE   = 0;
static const uint8_t ROT_DEITADO = 3;

// ---------------------------------------------------------------------------
// THE DECISION
// ---------------------------------------------------------------------------
// Only the SCREEN PLANE matters (ax, ay). The IMU's y axis runs along the long
// side of the panel (the same frame of reference as the IMU app's level), so:
//
//   gravity almost all in y    -> portrait
//   gravity almost all in x    -> landscape
//
// THE SIGN DOES NOT COUNT, on purpose. The two states are one per axis, and the
// opposite side of each (upside down, or landscape with the keyboard on top)
// falls into the same state - the screen does not flip to the other side, it just stays. That is
// what we want, and it also avoids needing to know the polarity of each BMI270 axis
// on this device: getting a sign wrong here would silently invert the whole
// rule.
//
// TWO DEAD ZONES, where nothing changes with the app OPEN:
//   - device on the table: gravity almost all in z, the screen plane with
//     less than ~0.4 g (tilted less than ~24 degrees). Putting the device down must
//     not rotate the screen. It was 0.5 g (30 degrees): holding the device reclined
//     in the hand, portrait, the reading stayed below that and an app opened in landscape
//     never corrected itself.
//   - on the diagonal: one axis must exceed the other by PLANO_RAZAO (tangent
//     of 60 degrees). Between 30 and 60 degrees of rotation, it stays as it is - the hysteresis
//     that stops the screen jumping from one state to the other at the boundary.
static const float PLANO_MIN_G2 = 0.16f;   // (0,4 g)^2
static const float PLANO_RAZAO  = 1.73f;

// ON OPENING THERE IS NO PREVIOUS STATE TO PROTECT: the hysteresis exists so as not to
// jump back and forth, and on opening there is no "other side" yet. The
// dominant axis wins, with a small margin (tangent of ~50 degrees), and a smaller
// plane (0.3 g, ~17 degrees of tilt). Outside that - on the table, or
// exactly on the diagonal - it opens in the position the screen is already in (home is
// always portrait), never on a fixed guess.
static const float ABRIR_MIN_G2 = 0.09f;   // (0,3 g)^2
static const float ABRIR_RAZAO  = 1.2f;

// TIMING. One sample every 200 ms (5 Hz) and the pose must repeat in 3
// consecutive ones: ~0.6 s still in the new posture before rotating. A sway of the hand
// does not rotate; turning the device on purpose rotates promptly. After rotating, 1.5 s
// of grace: the full pass takes a frame, and switching again in the middle
// of the gesture would be worse than waiting.
static const uint32_t AMOSTRA_MS   = 200;
static const int      SEGUIDAS     = 3;
static const uint32_t CARENCIA_MS  = 1500;

static orientacao_t s_modo      = ORI_EM_PE;   // that of the open app
static uint32_t     s_ultima_ms = 0;
static uint32_t     s_girou_ms  = 0;
static int          s_cand      = -1;          // the rotation asking to come through
static int          s_seguidas  = 0;

// PHYSICAL KEYBOARD DOCKED = EVERYTHING LANDSCAPE: home and every app, whatever
// the declared orientation - except ORI_FIXO_EM_PE. It docks on ExtPort1, which rotation 3 puts
// at the bottom, next to whoever is typing; in portrait it would hang off the side. Removing the
// keyboard returns each one to what it declared (the IMU keeps deciding for those who follow it).
static bool         s_teclado   = false;       // the keyboard state already applied to the screen

// The rotation for whoever is on screen without a keyboard: home is ORI_EM_PE; whoever follows
// the IMU stays as it is until the watcher decides.
static uint8_t rotacao_sem_teclado(void)
{
  // (ORI_FIXO_EM_PE falls through to the portrait at the end.)
  if (s_modo == ORI_DEITADO) { return ROT_DEITADO; }
  if (s_modo == ORI_SEGUE_IMU) { return M5.Display.getRotation(); }
  return ROT_EM_PE;
}

// The rotation the CURRENT posture asks for, or -1 (dead zone, IMU without a reading).
static int rotacao_pedida(tab5_imu_dados_t* m)
{
  // Physical keyboard docked: landscape, always. In portrait it would hang
  // off the side, and rotating on the lap while typing would be the worst defect.
  if (tab5_teclado_presente()) { return ROT_DEITADO; }
  if (!tab5_imu_ultima(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < PLANO_MIN_G2) { return -1; }
  if (ay > ax * PLANO_RAZAO) { return ROT_EM_PE; }
  if (ax > ay * PLANO_RAZAO) { return ROT_DEITADO; }
  return -1;
}

// A READING THAT CAN BE TRUSTED, for opening. The IMU cache is
// updated 1x/s and may be stale; and the first reading after the chip
// has been quiet may come back zeroed (M5Unified only updates the values when there is
// new data, and in the BMI270's power-saving mode new data takes a while). A zeroed
// reading fell into the dead zone, and the screen opened landscape with the device portrait.
// Here: up to 6 readings, 10 ms apart, until the gravity magnitude makes
// sense (0.6 to 1.4 g - still, gravity only). ~60 ms in the worst case, once
// per app opening.
static bool ler_postura_fresca(tab5_imu_dados_t* m)
{
  for (int k = 0; k < 6; ++k) {
    tab5_imu_medir();
    if (tab5_imu_ultima(m)) {
      const float g2 = m->ax * m->ax + m->ay * m->ay + m->az * m->az;
      if (g2 > 0.36f && g2 < 1.96f) { return true; }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

// The rotation for OPENING (see ABRIR_RAZAO), or -1 to stay as the screen is.
static int rotacao_para_abrir(tab5_imu_dados_t* m)
{
  if (tab5_teclado_presente()) { return ROT_DEITADO; }
  if (!ler_postura_fresca(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < ABRIR_MIN_G2) { return -1; }
  if (ay > ax * ABRIR_RAZAO) { return ROT_EM_PE; }
  if (ax > ay * ABRIR_RAZAO) { return ROT_DEITADO; }
  return -1;
}

static void aplicar(uint8_t rot)
{
  if (M5.Display.getRotation() == rot) { return; }
  M5.Display.setRotation(rot);
  tela_virtual_seguir_rotacao();
}

void orientacao_ao_abrir(const app_t* app)
{
  s_modo     = app->orientacao;
  s_cand     = -1;
  s_seguidas = 0;
  s_ultima_ms = s_girou_ms = tab5_agora_ms();

  uint8_t rot = ROT_EM_PE;
  tab5_imu_dados_t m = { };
  int p = -1;
  s_teclado = tab5_teclado_presente();
  if (s_teclado && s_modo != ORI_FIXO_EM_PE) {
    rot = ROT_DEITADO;
  } else if (s_modo == ORI_DEITADO) {
    rot = ROT_DEITADO;
  } else if (s_modo == ORI_SEGUE_IMU) {
    // The current posture, measured now (see ler_postura_fresca). Without a clear
    // decision, open as the screen already is - whoever opens an app from home is looking
    // at it in portrait. Before, the default was landscape, and that was what always appeared
    // when the reading failed.
    p = rotacao_para_abrir(&m);
    const uint8_t agora = M5.Display.getRotation();
    rot = (p >= 0) ? (uint8_t)p : (agora == ROT_DEITADO ? ROT_DEITADO : ROT_EM_PE);
  }
  aplicar(rot);
  if (s_modo == ORI_SEGUE_IMU) {
    // Gravity alongside, as with rotation: confirms in the log why it opened like that.
    ESP_LOGI(TAG, "'%s' opens %s, following the device (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s", app->nome,
             rot == ROT_EM_PE ? "portrait" : "landscape", M5.Display.width(), M5.Display.height(),
             (double)m.ax, (double)m.ay, (double)m.az,
             tab5_teclado_presente() ? "  (keyboard docked)" : p < 0 ? "  (no decision: the screen's position)" : "");
  } else {
    ESP_LOGI(TAG, "'%s' opens %s (%dx%d)%s", app->nome, rot == ROT_EM_PE ? "portrait" : "landscape",
             M5.Display.width(), M5.Display.height(),
             s_modo == ORI_FIXO_EM_PE ? "  (fixed)" : s_teclado ? "  (keyboard docked)" : "");
  }
}

void orientacao_ao_fechar(void)
{
  s_modo = ORI_EM_PE;
  s_teclado = tab5_teclado_presente();
  const uint8_t rot = s_teclado ? ROT_DEITADO : ROT_EM_PE;
  if (M5.Display.getRotation() != rot) {
    aplicar(rot);
    ESP_LOGI(TAG, "screen back %s (%dx%d)", rot == ROT_EM_PE ? "portrait" : "landscape (keyboard docked)",
             M5.Display.width(), M5.Display.height());
  }
}

uint32_t orientacao_prazo_ms(uint32_t agora)
{
  if (s_modo != ORI_SEGUE_IMU) { return UINT32_MAX; }
  const uint32_t dec = agora - s_ultima_ms;
  return (dec < AMOSTRA_MS) ? (AMOSTRA_MS - dec) : 0;
}

bool orientacao_vigiar(uint32_t agora, bool pode_girar)
{
  // The keyboard was docked or removed: rotate right away (no samples or grace - it is not a
  // sway of the hand), as soon as possible (no finger on the screen, nothing in front).
  const bool teclado = tab5_teclado_presente();
  if (teclado != s_teclado && pode_girar) {
    s_teclado = teclado;
    s_cand = -1;
    s_seguidas = 0;
    s_girou_ms = agora;
    const uint8_t rot = (teclado && s_modo != ORI_FIXO_EM_PE) ? ROT_DEITADO : rotacao_sem_teclado();
    if (rot != M5.Display.getRotation()) {
      aplicar(rot);
      ESP_LOGI(TAG, "keyboard %s: screen %s (%dx%d)", teclado ? "docked" : "removed",
               rot == ROT_EM_PE ? "portrait" : "landscape", M5.Display.width(), M5.Display.height());
      return true;
    }
  }
  if (s_modo != ORI_SEGUE_IMU) { return false; }
  if ((agora - s_ultima_ms) < AMOSTRA_MS) { return false; }
  s_ultima_ms = agora;

  tab5_imu_medir();
  tab5_imu_dados_t m = { };
  const int p = rotacao_pedida(&m);
  if (p < 0 || p == (int)M5.Display.getRotation()) {
    s_cand = -1;
    s_seguidas = 0;
    return false;
  }
  if (p == s_cand) { ++s_seguidas; }
  else             { s_cand = p; s_seguidas = 1; }

  if (s_seguidas < SEGUIDAS || !pode_girar || (agora - s_girou_ms) < CARENCIA_MS) { return false; }

  aplicar((uint8_t)p);
  s_girou_ms = agora;
  s_cand = -1;
  s_seguidas = 0;
  // Gravity alongside: it is what confirms in the log, without guessing, that the axis and the
  // threshold are right on this device.
  ESP_LOGI(TAG, "rotated to %s (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s",
           p == ROT_EM_PE ? "portrait" : "landscape", M5.Display.width(), M5.Display.height(),
           (double)m.ax, (double)m.ay, (double)m.az, tab5_teclado_presente() ? "  (keyboard docked)" : "");
  return true;
}
