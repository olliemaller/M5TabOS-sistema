// APP: IMU - live viewer of the BMI270 axes (accelerometer + gyroscope).
//
// WHAT IT IS FOR: no app in this system used the IMU yet. This is the
// playground - it shows the 6 axes (3 accelerometer, 3 gyroscope) live,
// to get a feel in the hand for what the chip measures before deciding what to
// do with it later (the postponed idea is auto-rotating the screen, like a phone).
//
// NO TASK OF ITS OWN, ON PURPOSE: unlike Mesh/GPS, nobody needs the IMU
// with the app CLOSED - it only matters while someone is looking at the
// screen and moving the device. desenhar() reads the sensor directly, on the
// UI task ITSELF, which is the only one allowed to talk to the internal I2C
// bus (the same one as touch and the INA226 - see the warning in
// tab5_power.hpp). A task and a queue here would be abstraction for no reason,
// in the same spirit as the Exemplo app: there is no background producer
// to feed.
//
// perfil.imu = true IS A PHASE 1 FLAG (see tab5_power.hpp): once this
// app exists in the table, the BMI270 is powered at boot for the WHOLE
// SESSION. M5Unified leaves it with the gyroscope on and no power saving; the
// hal/tab5_imu service cuts that when nobody reads the gyroscope (accelerometer
// only, in power-saving mode). This app reads the gyroscope: it asks on open and
// releases on close (tab5_imu_giroscopio).

#include "app.h"
#include "idioma.h"
#include "pintura.hpp"
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_imu.hpp"

static const char* TAG = "app.imu";

// OWN CONTRAST, NOT SIS_BORDA/SIS_LINHA: frame and crosshair were
// TOO FAINT against the background, SIS_LINHA is rgb(56,60,60), almost invisible).
// SIS_BORDA/SIS_LINHA are SYSTEM constants (app.h), used by several
// apps for deliberately subtle borders - changing both would change
// everyone. This app is an instrument dial (level + axis bars),
// where the grid NEEDS to show over the background - hence IMU_BORDA is local.
// Frame in the theme's disabled-text tone (lighter than TEMA_LINHA, which
// disappears against the background); the crosshair uses the same tone: it must
// show for the level to be read, and TEMA_LINHA vanished in the dark.
static const uint16_t IMU_BORDA = TEMA_TEXTO_3;

static bool ao_abrir(void)
{
  if (!tab5_imu_disponivel()) {
    // Does not refuse to open: desenhar() shows the notice on screen anyway.
    // This would only happen if perfil.imu had been left false by mistake -
    // see sistema_perfil_de_boot(), which ORs the field across ALL apps.
    ESP_LOGW(TAG, "IMU is not enabled - did perfil.imu end up false at boot?");
  }
  // The gyroscope turns on now (this app reads straight from the sensor every frame).
  tab5_imu_giroscopio(TAB5_GIRO_APP_IMU, true);
  tab5_imu_medir();
  ESP_LOGI(TAG, "opened");
  return true;
}

// LEVEL BACKGROUND, CACHED - see the big notice just below, next to
// desenhar_nivel(). Created on the first call, freed when the app closes.
static M5Canvas* s_nivel_fundo   = nullptr;
static int       s_nivel_fundo_r = -1;   // radius the background was drawn with

// AXIS BAR BACKGROUND, CACHED - see the big notice next to
// desenhar_barra(). Declared here (not next to the function, further down) for
// the same reason as the level: ao_fechar() must free both.
static M5Canvas* s_barra_fundo   = nullptr;
static int       s_barra_fundo_w = -1, s_barra_fundo_h = -1;

static void ao_fechar(void)
{
  tab5_sprite_soltar(&s_nivel_fundo);
  s_nivel_fundo_r = -1;
  tab5_sprite_soltar(&s_barra_fundo);
  s_barra_fundo_w = s_barra_fundo_h = -1;
  tab5_imu_giroscopio(TAB5_GIRO_APP_IMU, false);   // back to power saving on the next reading
  ESP_LOGI(TAG, "closed");
}

// ---------------------------------------------------------------------------
// LEVEL (BUBBLE)
// ---------------------------------------------------------------------------
// The dot moves with the tilt read from the accelerometer at rest: 1g
// split between the axes as the device tips. Clamped at +-1g - beyond
// that the device is already on its side, and the dot only needs to touch the edge.
//
// this used to redraw the WHOLE box every frame (20Hz):
// it cleared everything and recomputed two circles + crosshair via the vector
// algorithm, just to move a dot. It was a CONSCIOUS trade-off (documented right
// here: "simpler than storing the previous position", "tens of KB per
// frame, nothing like the ~1.6MB that cause the blue flicker") - it was not
// wrong, it just had not been revisited yet in this session's power
// optimisation process.
//
// The trade-off now: the CIRCLE/CROSSHAIR (which never change, only their radius
// changes if the screen is resized) go into a small M5Canvas, drawn only
// when the radius changes. Each frame, instead of recalculating the two circles and
// the two lines by algorithm, it just does a pushSprite() of the canvas (a byte
// copy, no trigonometry nor Bresenham) and draws the little ball on top.
// THE NUMBER OF BYTES WRITTEN TO THE SCREEN STAYS THE SAME (the whole box is still
// "revealed" every frame, and that is how the old ball's trail stays
// correct without having to restore just the piece where it was) - the
// gain is CPU ONLY: a copy is always cheaper than recalculating circle and
// line from scratch, 20 times a second, while the app is open.
static void desenhar_nivel(lgfx::LovyanGFX& d, int cx, int cy, int r, float ax, float ay)
{
  const int lado = 2 * r + 12;

  if (!s_nivel_fundo) {
    // NO SPRITE, NO CACHE - back to the old path (redraw everything by
    // vector). Would only happen with no free PSRAM, which would already be a
    // bigger problem than this one.
    s_nivel_fundo = tab5_sprite_novo(&d, lado, lado);
  }

  if (!s_nivel_fundo) {
    const uint16_t linha = TEMA_TEXTO_3;   // see the IMU_BORDA notice at the top of the file
    d.fillRect(cx - r - 6, cy - r - 6, lado, lado, TEMA_FUNDO);
    d.drawCircle(cx, cy, r, IMU_BORDA);
    d.drawCircle(cx, cy, r / 2, linha);
    d.drawFastHLine(cx - r, cy, 2 * r, linha);
    d.drawFastVLine(cx, cy - r, 2 * r, linha);
  } else {
    if (s_nivel_fundo_r != r) {
      // ONLY WHEN THE RADIUS CHANGES (never, in practice - the screen does not resize
      // with the app open - but it is the same care as the rest of the system: do not
      // assume a fixed layout by luck).
      const uint16_t linha = TEMA_TEXTO_3;
      s_nivel_fundo->fillSprite(TEMA_FUNDO);
      s_nivel_fundo->drawCircle(lado / 2, lado / 2, r, IMU_BORDA);
      s_nivel_fundo->drawCircle(lado / 2, lado / 2, r / 2, linha);
      s_nivel_fundo->drawFastHLine(lado / 2 - r, lado / 2, 2 * r, linha);
      s_nivel_fundo->drawFastVLine(lado / 2, lado / 2 - r, 2 * r, linha);
      s_nivel_fundo_r = r;
    }
    s_nivel_fundo->pushSprite(&d, cx - lado / 2, cy - lado / 2);
  }

  float px = ax, py = ay;
  if (px >  1.0f) { px =  1.0f; }
  if (px < -1.0f) { px = -1.0f; }
  if (py >  1.0f) { py =  1.0f; }
  if (py < -1.0f) { py = -1.0f; }

  // Screen grows DOWNWARDS; tilting the top of the device "backwards" is
  // positive Y on the accelerometer, and should move the dot UP on the screen -
  // hence the flipped sign in py.
  const int dx = cx + (int)(px * (r - 16));
  const int dy = cy - (int)(py * (r - 16));
  d.fillCircle(dx, dy, 11, TEMA_APP_LIMA);
  d.drawCircle(dx, dy, 11, TEMA_FUNDO);
}

// ---------------------------------------------------------------------------
// ZERO-CENTRED BAR
// ---------------------------------------------------------------------------
// 'frac' already comes in [-1, 1] (the value divided by the scale chosen by the
// caller). Fills from the centre towards the side the sign indicates - the same
// drawing as an audio balance meter, which is the right metaphor here:
// what matters is the DISTANCE from zero, in both directions.
//
// CACHED BACKGROUND, (s_barra_fundo, declared up top next to
// s_nivel_fundo) - found in the same sweep that prompted the level fix
// (desenhar_nivel(), see the big notice there): this function runs 6 times per
// frame at 20Hz (one per axis) and cleared + redrew border and centre
// line EVERY TIME, with all SIX calls using exactly the same w/h - the
// same static image, six times per frame, always recalculated from scratch
// by the rectangle/line algorithm. A single sprite, the size of ONE bar,
// serves all six (all identical) - only the fill (which really does change per
// axis) is still drawn straight to the screen.
static void desenhar_barra(lgfx::LovyanGFX& d, int x, int y, int w, int h,
                           float frac, uint16_t cor)
{
  const int cx = x + w / 2;

  if (s_barra_fundo && (s_barra_fundo_w != w || s_barra_fundo_h != h)) {
    tab5_sprite_soltar(&s_barra_fundo);
  }
  if (!s_barra_fundo) {
    // NO PSRAM FOR THE SPRITE (nullptr) - back to the old path, same
    // defence as the level.
    s_barra_fundo = tab5_sprite_novo(&d, w, h);
    if (s_barra_fundo) {
      s_barra_fundo->fillSprite(TEMA_FUNDO);
      s_barra_fundo->drawRect(0, 0, w, h, IMU_BORDA);
      s_barra_fundo->drawFastVLine(w / 2, 0, h, TEMA_LINHA);
      s_barra_fundo_w = w;
      s_barra_fundo_h = h;
    }
  }

  if (s_barra_fundo) {
    s_barra_fundo->pushSprite(&d, x, y);
  } else {
    d.fillRect(x, y, w, h, TEMA_FUNDO);
    d.drawRect(x, y, w, h, IMU_BORDA);
    d.drawFastVLine(cx, y, h, TEMA_LINHA);
  }

  if (frac >  1.0f) { frac =  1.0f; }
  if (frac < -1.0f) { frac = -1.0f; }
  const int px = (int)(frac * (w / 2 - 3));
  if (px >= 0) { d.fillRect(cx, y + 3, px, h - 6, cor); }
  else         { d.fillRect(cx + px, y + 3, -px, h - 6, cor); }
}

typedef struct {
  const char* rotulo;
  float       valor;
  float       escala;    // value that fills the whole bar (1.0 == full scale)
  uint16_t    cor;
  const char* unidade;
} eixo_t;

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  static pintura_t s_p;
  pintura_iniciar(&s_p);

  // The system tries again to bring up an IMU that failed at boot (tab5_imu_medir):
  // when it appears, the notice goes away in a full repaint.
  static bool s_sem_imu = false;
  if (!tab5_imu_disponivel()) {
    if (pintura_completa(&s_p) || !s_sem_imu) {
      s_sem_imu = true;
      tab5_texto(d, 2.3f);
      d.setTextDatum(middle_center);
      d.setTextColor(TEMA_PERIGO, TEMA_FUNDO);
      d.drawString(tr("IMU não respondeu"), a.x + a.largura / 2, a.y + a.altura / 2);
    }
    return;
  }
  if (s_sem_imu) {
    s_sem_imu = false;
    sistema_pedir_redesenho_completo();
    return;
  }

  // ONE reading per frame, on the UI task - same rule as the INA226 and
  // the keyboard (see the big notice in tab5_power.hpp about the internal
  // bus without a mutex).
  //
  // WHY NOT tab5_imu_ultima() (see hal/tab5_imu.hpp): that
  // cache exists for OTHER apps to ask "what is the device doing" without
  // touching I2C - but it is only fed 1x/second, fast enough for
  // power/temperature, too slow for a bubble level not to look
  // frozen. This app reads via tab5_imu_amostra(): it is the only place that needs
  // the fast reading, and it is already safe (same task).
  //
  // With no new sample this frame, the previous frame's values stay (as
  // M5Unified already did): zeroing would make the level flicker.
  static float s_a[3] = {0, 0, 0}, s_g[3] = {0, 0, 0};
  tab5_imu_amostra(s_a, s_g);
  const float ax = s_a[0], ay = s_a[1], az = s_a[2];
  const float gx = s_g[0], gy = s_g[1], gz = s_g[2];

  // LANDSCAPE (physical keyboard attached, kernel/orientacao): two columns - the
  // level on the left, the bars on the right. Stacked, the bars ran past the
  // height of the area (~584 px) and wrote over the navigation.
  const bool deitado = a.largura > a.altura;
  const int nivel_r  = 130;
  const int nivel_cx = deitado ? a.x + 40 + nivel_r : a.x + a.largura / 2;
  const int nivel_cy = a.y + 24 + nivel_r;
  // THE LEVEL BELONGS TO THE SCREEN, not the chip: in landscape (rotation 3, kernel/orientacao)
  // the screen's "to the right" is the chip's +Y and "up" is -X. Without
  // rotating the axes, the bubble moved 90 degrees off the side being tilted.
  // The bars below still show the chip's axes (they are measurements).
  desenhar_nivel(d, nivel_cx, nivel_cy, nivel_r, deitado ? ay : ax, deitado ? -ax : ay);

  if (pintura_completa(&s_p)) {
    tab5_texto(d, 2.3f);
    d.setTextDatum(top_center);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(tr("incline o aparelho"), nivel_cx, nivel_cy + nivel_r + 6);
  }

  // Accelerometer scale in g (1g = at rest, vertical) and gyroscope in
  // degrees/second. The numbers are a SCREEN ceiling, not the sensor's: going
  // past them only pins the bar to the edge, the numeric value stays exact.
  const eixo_t eixos[6] = {
    // DATA colours (X/Y/Z axes in red/green/blue, gyro in orange/yellow/purple),
    // softened to the theme's tones without losing the distinction between the six.
    { "AX", ax,   2.0f, TEMA_APP_VERMELHO, "g"   },
    { "AY", ay,   2.0f, TEMA_APP_VERDE,    "g"   },
    { "AZ", az,   2.0f, TEMA_APP_AZUL,     "g"   },
    { "GX", gx, 250.0f, TEMA_APP_LARANJA,  "dps" },
    { "GY", gy, 250.0f, TEMA_APP_AMARELO,  "dps" },
    { "GZ", gz, 250.0f, TEMA_APP_ROXO,     "dps" },
  };

  const int col_x      = deitado ? nivel_cx + nivel_r + 40 : a.x;   // the bars column
  const int y0         = deitado ? a.y + 40 : nivel_cy + nivel_r + 46;
  const int alt_linha  = 72;
  const int barra_x    = col_x + 100;
  const int barra_w    = a.x + a.largura - barra_x - 150;
  const int barra_h    = 36;

  for (int i = 0; i < 6; ++i) {
    const int cy = y0 + i * alt_linha;

    // Label: never changes, only drawn in the full pass.
    if (pintura_completa(&s_p)) {
      tab5_texto(d, 2.3f);
      d.setTextDatum(middle_left);
      d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
      d.drawString(eixos[i].rotulo, col_x + 16, cy + barra_h / 2);
    }

    desenhar_barra(d, barra_x, cy, barra_w, barra_h,
                   eixos[i].valor / eixos[i].escala, eixos[i].cor);

    char buf[24];
    snprintf(buf, sizeof(buf), "%+6.2f %s", (double)eixos[i].valor, eixos[i].unidade);
    tab5_texto(d, 2.3f);
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
    d.drawString(buf, barra_x + barra_w + 12, cy + barra_h / 2);
  }
}

// ---------------------------------------------------------------------------
// HOME SCREEN ICON
// ---------------------------------------------------------------------------
// Three axes, the usual 3D tool convention: red X to the right, green Y
// upwards, blue Z as a dotted circle (the axis that "comes out of the
// screen" cannot become a flat arrow).
static void icone_imu(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(32, 24, 40);
  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  const int cx = x + lado / 2;
  const int cy = y + lado / 2;
  const int r  = (int)(0.34f * lado);

  for (int ang = 0; ang < 360; ang += 30) {
    const float rad = (float)ang * 3.14159265f / 180.0f;
    const int px = cx + (int)(r * 0.55f * cosf(rad));
    const int py = cy + (int)(r * 0.55f * sinf(rad));
    d.fillCircle(px, py, lado / 60, TFT_SKYBLUE);
  }

  tab5_linha_grossa(d, cx, cy, cx + r, cy, lado / 30.0f, TFT_RED);
  d.fillTriangle(cx + r, cy - lado / 22, cx + r, cy + lado / 22, cx + r + lado / 14, cy, TFT_RED);

  tab5_linha_grossa(d, cx, cy, cx, cy - r, lado / 30.0f, TFT_GREEN);
  d.fillTriangle(cx - lado / 22, cy - r, cx + lado / 22, cy - r, cx, cy - r - lado / 14, TFT_GREEN);

  d.fillCircle(cx, cy, lado / 26, TFT_WHITE);
}

extern const app_t APP_IMU = {
  .nome  = "IMU",
  .cor   = TFT_PURPLE,
  // The IMU only. It is PHASE 1 (see the big notice at the top of the file): once
  // registered, the BMI270 stays on for the whole boot, not only while this
  // app is in the foreground.
  .perfil = { .imu = true },
  .usa_sd = false,
  .orientacao = ORI_EM_PE,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = nullptr,
  .ao_tocar_nivel = nullptr,
  .ao_teclar = nullptr,
  // 50ms = 20Hz. Smooth enough to follow a hand gesture without using
  // more I2C than needed - the same reasoning as TECLADO_MS in core.cpp.
  .periodo_ms = 50,
  .altura_conteudo = 0,
  .icone = icone_imu,
  .fundo_tema = true,
};
