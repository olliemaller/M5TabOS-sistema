// SHELL - the bar: battery icon, drawing the status bar (Wi-Fi, Bluetooth,
// ESP-NOW, short message, music chip), the clock, the home screen's bottom strip
// (temperature, GROVE/CARTAO services) and the navigation button. Split out of
// shell.cpp; what the parts share lives in shell_interno.hpp.

#include "tela_virtual.hpp"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_ble.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_tela.hpp"
#include "tab5_grove.hpp"
#include "tab5_sensores.hpp"
#include "notificacao.h"
#include "midia.h"
#include "tab5_mesh.hpp"
#include "tab5_gps.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_hora.hpp"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "shell_interno.hpp"

// ---------------------------------------------------------------------------
// Battery icon
// ---------------------------------------------------------------------------

// Three power states, not two. The earlier mistake was detecting "charging"
// from positive current alone: at 100% the charger stops, the current drops to
// zero and the bolt vanished, even with the cable plugged in.
//
// The discriminator is simple and reliable: the system draws more than 100mA.
// If that current is NOT coming out of the battery, it can only be coming from USB.
typedef enum {
  EN_BATERIA,      // discharging: clearly negative current
  EN_CARREGANDO,   // current flowing into the battery
  EN_TOMADA        // powered externally, battery not changing charge (full)
} estado_energia_t;

static estado_energia_t estado_energia(const tab5_medida_t& m)
{
  if (m.mA < -20.0f) { return EN_BATERIA;    }
  if (m.mA >  20.0f) { return EN_CARREGANDO; }
  return EN_TOMADA;
}

// Charging bolt, drawn with an outline so it is legible over both the full
// part and the empty part of the battery.
static void desenhar_raio(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillTriangle(cx + 2, cy - 10, cx - 7, cy + 2, cx + 1, cy + 2, cor);
  d.fillTriangle(cx - 2, cy + 10, cx + 7, cy - 2, cx - 1, cy - 2, cor);
}

// Plug, for when it is on mains with the battery already full - the same thing a
// phone shows when it has finished charging and you have not unplugged it.
//
// Tight dimensions on purpose: it draws INSIDE the battery, which is 30px tall
// with a 3px border on each side. That leaves 24px, and this plug takes 19.
static void desenhar_tomada(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillRect(cx - 5, cy - 9, 2, 5, cor);           // left pin
  d.fillRect(cx + 3, cy - 9, 2, 5, cor);           // right pin
  d.fillRoundRect(cx - 7, cy - 4, 14, 9, 2, cor);  // body
  d.fillRect(cx - 1, cy + 5, 2, 5, cor);           // cable
}

// Phone-style battery: rounded outline, terminal, proportional fill and the
// central symbol when charging or on mains.
static void desenhar_bateria(lgfx::LovyanGFX& d, int x, int y, int pct, estado_energia_t est)
{
  const int L = 58;              // body width
  const int A = 30;              // body height
  const int borda = 3;

  const int p = (pct < 0) ? 0 : (pct > 100 ? 100 : pct);

  // Green with the charger or a good charge; yellow below 50%; red below 20%.
  const uint16_t cor = (est != EN_BATERIA) ? (uint16_t)TEMA_SUCESSO
                     : (p < 20)            ? (uint16_t)TEMA_PERIGO
                     : (p < 50)            ? (uint16_t)TEMA_ALERTA
                                           : (uint16_t)TEMA_SUCESSO;

  // Body: interior cleared and then filled up to the charge fraction.
  // TEMA_FUNDO: this fillRect clears the icon area AND paints the EMPTY part of
  // the battery, which has to match the bar's colour.
  d.fillRect(x, y, L + 6, A, TEMA_FUNDO);                           // clears the whole area
  d.drawRoundRect(x, y, L, A, 6, TEMA_TEXTO_2);
  d.drawRoundRect(x + 1, y + 1, L - 2, A - 2, 5, TEMA_TEXTO_2);     // 2px frame
  d.fillRect(x + L + 1, y + A / 2 - 6, 5, 12, TEMA_TEXTO_2);        // positive terminal

  const int util = L - 2 * borda;
  const int cheio = (util * p) / 100;
  if (cheio > 0) {
    d.fillRoundRect(x + borda, y + borda, cheio, A - 2 * borda, 3, cor);
  }

  // Central symbol. Same technique in both cases: an outline in the background
  // colour offset by 1px and a light figure on top, so it is legible over both
  // the full part and the empty part of the battery.
  if (est == EN_CARREGANDO) {
    desenhar_raio(d, x + L / 2 + 1, y + A / 2 + 1, TEMA_FUNDO);
    desenhar_raio(d, x + L / 2,     y + A / 2,     TEMA_TEXTO);
  } else if (est == EN_TOMADA) {
    desenhar_tomada(d, x + L / 2 + 1, y + A / 2 + 1, TEMA_FUNDO);
    desenhar_tomada(d, x + L / 2,     y + A / 2,     TEMA_TEXTO);
  }

  // If the model cannot estimate (pct < 0), mark it with a small dash.
  if (pct < 0) {
    d.drawFastHLine(x + borda + 4, y + A / 2, util - 8, TEMA_TEXTO_2);
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// Wi-Fi icon: four concentric arcs, lit according to the signal.
//
// A symbol instead of text because the bar is read at a glance: shape and colour
// arrive before reading does. The network name is still available in the
// Settings app, which is where you go when you want detail.
//
// RSSI bands (dBm), the usual Wi-Fi ones:
//   >= -55 excellent (4) | >= -65 good (3) | >= -75 ok (2) | >= -85 weak (1)
// THE X GOES ALONGSIDE, NOT ON TOP.
//
// The previous version struck through the fan itself with two thick strokes.
// It was legible and it was hideous: the X fought the arcs for the same space
// and the symbol became a smudge. The good convention keeps the
// fan intact and puts the X ON ITS OWN in the top right-hand corner.
//
// For that the fan moves a little to the left and shrinks, freeing the corner.
// The whole icon now takes ~46px of width instead of ~40.
// Thermometer of ~14x31 (was 12x26, +20% along with the font
// change), drawn from the centre of the bulb. Bulb, stem and the coloured
// column inside it - the same drawing works for any colour, so the
// temperature paints the liquid and the glass always stays light.
//
// VECTOR ICON, IT DOES NOT FOLLOW THE FONT BY ITSELF - which is why the literals
// below are the originals * 1.2 (rounded), and not a run-time calculation: it
// is a fixed drawing, called from a single place, with no reason to recompute
// the scale every frame.
// THE TEMPERATURE COLOUR, continuous: cold blue -> green -> yellow -> orange ->
// red. Before there were three bands (blue up to 55, orange, red at 70) and the
// thermometer spent most of the day in the same blue - it told you nothing.
// The steps follow what this chip really does: ~40 idle, ~50 with the screen
// and the radio busy, 55+ working, 70+ hot.
static uint16_t cor_da_temp(float c)
{
  static const struct { float t; uint8_t r, g, b; } P[] = {
    // The same theme tones (INFO, SUCESSO, ALERTA, APP_LARANJA, PERIGO): the
    // gradient needs the RGB channels to interpolate, so the 24-bit values go
    // here and not the constants already reduced to 565.
    { 35.0f,  0x39, 0xB8, 0xE6 },   // blue    (TEMA_INFO)
    { 45.0f,  0x34, 0xC7, 0x7B },   // green   (TEMA_SUCESSO)
    { 55.0f,  0xF5, 0xB8, 0x3D },   // yellow  (TEMA_ALERTA)
    { 63.0f,  0xF2, 0x7B, 0x3D },   // orange  (TEMA_APP_LARANJA)
    { 70.0f,  0xFF, 0x5C, 0x5C },   // red     (TEMA_PERIGO)
  };
  const int n = sizeof(P) / sizeof(P[0]);
  int r = P[0].r, g = P[0].g, b = P[0].b;
  if (c >= P[n - 1].t) { r = P[n - 1].r; g = P[n - 1].g; b = P[n - 1].b; }
  else {
    for (int i = 0; i + 1 < n; ++i) {
      if (c >= P[i].t && c < P[i + 1].t) {
        const float k = (c - P[i].t) / (P[i + 1].t - P[i].t);
        r = P[i].r + (int)((P[i + 1].r - P[i].r) * k);
        g = P[i].g + (int)((P[i + 1].g - P[i].g) * k);
        b = P[i].b + (int)((P[i + 1].b - P[i].b) * k);
        break;
      }
    }
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// THE COLUMN RISES WITH THE TEMPERATURE (20 to 80 C, the sensor's range) and the
// liquid takes the colour from cor_da_temp(). No reading: empty glass, grey bulb.
// Occupies cy-20 to cy+12.
static void desenhar_termometro(lgfx::LovyanGFX& d, int cx, int cy, bool tem, float c,
                                uint16_t fundo)
{
  const uint16_t vidro   = TEMA_TEXTO_2;
  const uint16_t liquido = tem ? cor_da_temp(c) : (uint16_t)TEMA_TEXTO_3;

  // Stem: light outline with the core in the background colour, so the liquid
  // has somewhere to show without the glass disappearing.
  d.fillRoundRect(cx - 5, cy - 20, 10, 24, 5, vidro);
  d.fillRoundRect(cx - 2, cy - 18, 5, 22, 2, fundo);
  if (tem) {
    float f = (c - 20.0f) / 60.0f;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    const int h = 2 + (int)(f * 20.0f + 0.5f);        // 2..22 px of column
    d.fillRect(cx - 2, cy + 4 - h, 5, h, liquido);
  }

  d.fillCircle(cx, cy + 5, 7, vidro);
  d.fillCircle(cx, cy + 5, 5, liquido);               // bulb
}

static void desenhar_wifi(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor,
                          int barras, bool cortado)
{
  // Offset fan: its centre is no longer the centre of the icon.
  cx -= 7;

  // Drawn as arcs: the dot at the base plus three rings. fillArc covers 225
  // to 315 degrees, which is the 90-degree fan pointing up.
  // Dimmed in TEMA_TEXTO_3 (the theme's "disabled"): on the dark background the
  // "empty" fan draws no more attention than the full one.
  const uint16_t off = TEMA_TEXTO_3;
  d.fillCircle(cx, cy + 9, 3, barras >= 1 ? cor : off);
  for (int n = 1; n <= 3; ++n)
  {
    const int r = 5 + n * 5;
    d.fillArc(cx, cy + 9, r, r - 2, 225, 315,
              (barras >= n + 1) ? cor : off);
  }

  if (cortado)
  {
    // ON TOP of the fan's tip, eating a piece of it - as in the usual "no
    // connection" symbol. It is not decoration: the overlaid X says that fan is
    // interrupted, whereas an X alongside only said "there is an X here too".
    //
    // 3px thick. The previous version had 4 plus a black outline of 6, and
    // the whole thing ended up heavier than the very fan it was meant to negate.
    const int ex = cx + 16;
    const int ey = cy - 4;
    const int b  = 6;
    tab5_linha_grossa(d, ex - b, ey - b, ex + b, ey + b, 3, TEMA_PERIGO);
    tab5_linha_grossa(d, ex + b, ey - b, ex - b, ey + b, 3, TEMA_PERIGO);
  }
}

// ESP-NOW MODE. With the channel fixed, the Wi-Fi drops the router ON
// PURPOSE - the radio is up, talking over ESP-NOW. The grey fan with an X
// said "no connection", which is a lie: there is a connection, just a different
// one. In this mode the bar shows the ESP-NOW symbol (the same as the app icon:
// an origin dot and three outgoing waves) - cyan on air, grey with the radio stopped.
static void desenhar_espnow(lgfx::LovyanGFX& d, int cx, int cy, bool no_ar)
{
  const uint16_t cor = no_ar ? (uint16_t)TEMA_INFO : (uint16_t)TEMA_TEXTO_3;
  const int ox = cx - 9, oy = cy + 9;   // the origin dot, bottom left
  d.fillCircle(ox, oy, 3, cor);
  for (int n = 1; n <= 3; ++n) {
    const int r = 3 + n * 6;
    d.fillArc(ox, oy, r, r - 2, 270, 360, cor);
  }
}

// BLUETOOTH IN THE BAR: the rune, to the left of the Wi-Fi, only while
// Bluetooth is on. Colour: accent = connected to a node (Mesh BLE); text
// = on; dimmed grey = starting up; red = failed.
static int bt_visual(void)
{
  const tab5_ble_estado_t e = tab5_ble_estado();
  if (e == TAB5_BLE_DESLIGADO) { return 0; }
  if (e == TAB5_BLE_FALHOU)    { return 3; }
  if (e == TAB5_BLE_SUBINDO || e == TAB5_BLE_DESLIGANDO) { return 1; }
  return mesh_ble_conectado() ? 4 : 2;
}

static void desenhar_bt(lgfx::LovyanGFX& d, int cx, int cy, int v)
{
  const uint16_t cor = (v == 4) ? (uint16_t)TEMA_DESTAQUE : (v == 3) ? (uint16_t)TEMA_PERIGO
                     : (v == 2) ? (uint16_t)TEMA_TEXTO    : (uint16_t)TEMA_TEXTO_3;
  const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
  for (int k = 0; k <= 1; ++k) {
    d.drawLine(cx + k, t, cx + k, b, cor);
    d.drawLine(cx + k, t, cx + dx + k, t + q, cor);
    d.drawLine(cx + dx + k, t + q, cx - dx + k, b - q, cor);
    d.drawLine(cx + k, b, cx + dx + k, b - q, cor);
    d.drawLine(cx + dx + k, b - q, cx - dx + k, t + q, cor);
  }
}

// What the bar SHOWS of the radio: the Wi-Fi state, or ESP-NOW mode (100 on
// air, 101 stopped). A single number for drawing and for barra_mudou - the two
// have to agree, otherwise switching mode does not repaint.
static int radio_visual(void)
{
  if (tab5_espnow_canal_fixo()) { return tab5_espnow_pronto() ? 100 : 101; }
  return (int)tab5_wifi_estado();
}

// Last state drawn on the bar, so it is not repainted when nothing changed.
static int  s_barra_temp = -999;  // last WHOLE degree drawn (lock screen only)
static char s_barra_hora[8] = ""; // the time drawn in the corner (outside the lock screen)
static bool s_barra_bloq = false; // the corner was painted as the lock screen
static int  s_barra_pct  = -999;
static int  s_barra_wifi = -1;    // Wi-Fi state the bar was painted with
static int  s_barra_bt   = -1;    // Bluetooth state the bar was painted with
static uint32_t s_barra_notif = 0;   // notification version already drawn
static estado_energia_t s_barra_estado = EN_BATERIA;
static const char* s_barra_nome = nullptr;
static uint32_t s_barra_midia = 0;   // media version (kernel/midia.h) already drawn
static uint32_t s_barra_idioma = 0;  // language version (services/idioma.h) already drawn

// SHORT MESSAGE (sistema_recado, app.h): the phrase and until when it replaces
// the app name. s_barra_recado is the one drawn ("" = the name).
#define RECADO_MS 2000
static char     s_recado[32]      = "";
static uint32_t s_recado_ate      = 0;
static char     s_barra_recado[32] = "";

static const char* recado_agora(void)
{
  return (s_recado[0] && (int32_t)(s_recado_ate - tab5_agora_ms()) > 0) ? s_recado : "";
}

void sistema_recado(const char* texto)
{
  snprintf(s_recado, sizeof(s_recado), "%s", texto ? texto : "");
  s_recado_ate = tab5_agora_ms() + RECADO_MS;
  sistema_acordar_interface();
}

bool recado_pendente(void) { return strcmp(recado_agora(), s_barra_recado) != 0; }

// Smooth Roboto at the native height closest to 'px' (only the bar uses sizes
// the theme does not name: the mock-up asks for 21-26 px).
static void fonte_barra(lgfx::LovyanGFX& d, bool negrito, int px)
{
  d.setFont(tab5_fonte_suave(negrito, px));
  d.setTextSize(1);
}

// THE MUSIC CHIP, anchored on the right at 'dir'. In the theme's accent colour
// (soft background, strong accent), so it cannot be confused with any notice
// severity: a note (playing) or pause, and the title cut to whatever
// fits. Returns the width used (0 = did not fit).
static const int MIDIA_PAST_MAX = 320;
static int desenhar_pastilha_midia(lgfx::LovyanGFX& d, int dir, int max_l, const midia_info_t& mi)
{
  const int alt = ALTURA_BARRA - 24, cy = ALTURA_BARRA / 2;   // 40 px, like the notice pills
  const int ICONE = 22, PAD = 14;
  if (max_l < alt) { return 0; }
  char t[MIDIA_TITULO];
  snprintf(t, sizeof(t), "%s", mi.titulo);
  tab5_texto(d, 3);
  tab5_fonte_corpo_negrito(d);
  const int cabe_txt = max_l - (PAD + ICONE + 10 + PAD);
  if (cabe_txt >= 40) { tab5_cortar_com_reticencias(d, t, sizeof(t), cabe_txt); }
  else                { t[0] = '\0'; }
  const int larg = t[0] ? PAD + ICONE + 10 + (int)d.textWidth(t) + PAD : alt;
  const int x = dir - larg;
  d.fillRoundRect(x, (ALTURA_BARRA - alt) / 2, larg, alt, alt / 2, TEMA_DESTAQUE_SUAVE);
  const int ix = t[0] ? x + PAD : x + (alt - ICONE) / 2;
  if (mi.tocando) {
    // Quaver: head, stem and flag.
    d.fillEllipse(ix + 7, cy + 7, 7, 5, TEMA_DESTAQUE);
    d.fillRect(ix + 12, cy - 11, 3, 18, TEMA_DESTAQUE);
    d.fillTriangle(ix + 12, cy - 11, ix + 22, cy - 4, ix + 12, cy - 3, TEMA_DESTAQUE);
  } else {
    d.fillRect(ix + 4,  cy - 9, 5, 18, TEMA_DESTAQUE);
    d.fillRect(ix + 14, cy - 9, 5, 18, TEMA_DESTAQUE);
  }
  if (t[0]) {
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_DESTAQUE_SUAVE);
    d.drawString(t, ix + ICONE + 10, cy);
  }
  tab5_fonte_corpo(d);
  return larg;
}
// THE COLOUR OF EACH SEVERITY - a single one, so the bar, the centre and the
// lock screen speak the same language.
uint16_t cor_do_nivel(notif_nivel_t n)
{
  return (n >= NOTIF_URGENTE) ? (uint16_t)TEMA_PERIGO
       : (n == NOTIF_ATENCAO) ? (uint16_t)TEMA_ALERTA
       : (n == NOTIF_INFO)    ? (uint16_t)TEMA_INFO
                              : (uint16_t)TEMA_TEXTO_2;   // STATE: activity, not news
}

// completo=true paints the bar BACKGROUND before the elements. It is always
// needed when the screen has been cleared (opening, waking from idle, switching
// app), otherwise the framebuffer black is left and the bar seems to have gone -
// which is what happened when the fillRect was removed in the belief it was just the flash.
//
// completo=false is the periodic update: the elements overwrite themselves
// (text with a background colour, icons clearing their own rectangle) and
// painting the whole background here would be exactly the flash we removed.
void desenhar_barra_status(lgfx::LovyanGFX& d, bool completo)
{
  const int L = d.width();
  char buf[32];

  // Reads from the CACHE. The real read happens once a second in the task's
  // loop; going to I2C here again would be one more transaction per bar
  // redraw, with no new data.
  tab5_medida_t m = tab5_power_ultima();
  const estado_energia_t est = estado_energia(m);

  // IN BANDS. 1280x64 in landscape is ~164KB written in one go, and the bar is
  // repainted on every app switch - the same kind of burst that produced the
  // flicker, just smaller. tab5_tela_limpar breathes between bands.
  if (completo) {
    tab5_tela_limpar(0, 0, L, ALTURA_BARRA, TEMA_FUNDO);
    // The left, middle and right blocks clear their own rectangles just
    // below, all in TEMA_FUNDO and all inside this one. Once marked, they vanish.
    tab5_tela_marcar_limpo(0, 0, L, ALTURA_BARRA, TEMA_FUNDO);
  }

  // Outside 'completo', we do NOT clear the whole bar.
  //
  // The full-width fillRect was a visible flash on every repaint - subtle
  // while the middle of the bar was text, obvious once it became an icon with
  // arcs. Each element now fends for itself: text is written with a background
  // colour (which overwrites rather than clears) and the two icons clear only their own rectangle.
  //
  // The price of this: all text here MUST have a fixed width. An app name
  // shorter than the previous one would leave the old one's tail on screen.
  // ---- LEFT: the time, and the app right after it ---------------------
  //
  // The two together on purpose. They are the two things you read without
  // looking - what time it is and where I am - and the MIDDLE is left entirely
  // to notices, which is content that comes and goes. The time used to be in the
  // bottom strip and the P4 temperature here; they swapped places: the time is
  // what you check all the time, and the top corner is where every phone puts it.
  //
  // NOT ON THE LOCK SCREEN: it already has the big clock in the middle, and
  // repeating the time up here would be the same information twice. There the
  // corner keeps the P4 and the temperature, as before.
  //
  // ESQ_FIM is the boundary, and it exists because the middle block once ate
  // the app name: the two cleared rectangles that overlapped, and whichever
  // drew last won. Now each side knows where to stop.
  static const int ROT_X          = 16;
  static const int GAP_ROT_TERMO  = 14;  // space between "P4" and the thermometer
  static const int GAP_TERMO_TEMP = 15;  // space between the thermometer (+20%) and the number
  // THE APP NAME HUGS THE TIME, with a MAXIMUM width. It used to sit at a fixed
  // x (168) and run as far as the text went: a long name ("Configuracoes",
  // a Berry app) crossed the boundary and the notice chip drew on top of
  // it. Now the name starts right after the time, and anything beyond
  // APP_MAX_L becomes "..." - the rest of the bar belongs to notices and music.
  static const int GAP_ESQ_APP = 22;    // from the time (or the temperature) to the name
  static const int APP_MAX_L   = 190;   // the app name never goes past this
  static const int ESQ_MAX     = 420;   // the left corner never goes past this (what gets cleared)

  tab5_tela_limpar(0, 4, ESQ_MAX, ALTURA_BARRA - 8, TEMA_FUNDO);
  int esq_x = ROT_X;   // where whatever was drawn to the left of the name ended
  if (!s_bloqueado) {
    hora_agora(buf, sizeof(buf));
    fonte_barra(d, true, 26);   // Roboto medium/bold, like the mock-up's clock
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
    d.drawString(buf, ROT_X, ALTURA_BARRA / 2);
    esq_x = ROT_X + (int)d.textWidth(buf);
    tab5_fonte_corpo(d);
  } else {
    // Reading order: "P4", the thermometer, the number. The label says WHOSE the
    // measurement is, the symbol says WHAT it is, the number says HOW MUCH.
    float c = 0.0f;
    const bool tem = tab5_sensores_temp(&c);
    tab5_texto(d, 3.0f);   // +20%
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString("P4", ROT_X, ALTURA_BARRA / 2);
    // Measured, not fixed: the width of "P4" depends on the current font.
    const int termo_x = ROT_X + (int)d.textWidth("P4") + GAP_ROT_TERMO;
    desenhar_termometro(d, termo_x, ALTURA_BARRA / 2, tem, c, TEMA_FUNDO);
    if (tem) { snprintf(buf, sizeof(buf), "%.0f°C", (double)c); }
    else     { snprintf(buf, sizeof(buf), "--"); }
    d.drawString(buf, termo_x + GAP_TERMO_TEMP, ALTURA_BARRA / 2);
    esq_x = termo_x + GAP_TERMO_TEMP + (int)d.textWidth(buf);
  }

  // App name in WHITE and heavier than the rest: it is the answer to "where
  // am I", which is read before anything else on the bar. It is the only
  // TITLE that appears on every system screen, so it is the only text on the
  // bar that switches to Orbitron_Light (see tab5_fonte_titulo in
  // tab5_ui.hpp) - the rest of the bar (P4, Wi-Fi, battery) are short
  // readings, not a title, and stay in the body font.
  //
  // NATIVE SIZE (1) is the ideal base for a bitmap font like Orbitron -
  // scaling by a fraction drops/duplicates pixel rows irregularly and
  // thickens the stroke (that is why "CONFIG" on the bar looked like a
  // DIFFERENT font from the gallery sample in Settings). But 1.0 turned out
  // too big here in practice; it needed to come down about 20%,
  // hence 0.8 (still much cleaner than a fraction like 0.6, just a little
  // short of native).
  //
  // The offset double-draw bold that used to be here was a patch for
  // Font0 having no weight - Orbitron_Light is already a "light" font by
  // graphic design, so we do NOT draw twice: the second copy offset by
  // 1px would look blurred, not "bold", in a font with a thin, even
  // stroke like this one.
  // Native bold Roboto (see tab5_titulo in tab5_ui.hpp)
  // - it used to be FreeMonoBold at 1.04x, the bar's last fractional scale.
  tab5_titulo(d, 28);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  const char* recado = recado_agora();
  snprintf(s_barra_recado, sizeof(s_barra_recado), "%s", recado);
  const int app_x = esq_x + GAP_ESQ_APP;
  int ESQ_FIM;
  if (recado[0]) {
    // The SHORT MESSAGE is a pill (raised surface with a thin border) in place
    // of the name, not loose text: it stands out from the app name without needing its own colour.
    // The text shrinks so the whole pill fits in APP_MAX_L.
    const int PAD = 14, ALT = ALTURA_BARRA - 24;
    snprintf(buf, sizeof(buf), "%s", recado);
    tab5_cortar_com_reticencias(d, buf, sizeof(buf), APP_MAX_L - 2 * PAD);
    const int larg = (int)d.textWidth(buf) + 2 * PAD;
    const int py = (ALTURA_BARRA - ALT) / 2;
    d.fillRoundRect(app_x, py, larg, ALT, ALT / 2, TEMA_SUPERFICIE_ALTA);
    d.drawRoundRect(app_x, py, larg, ALT, ALT / 2, TEMA_LINHA);
    d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE_ALTA);
    d.drawString(buf, app_x + PAD, ALTURA_BARRA / 2);
    ESQ_FIM = app_x + larg;
  } else {
    snprintf(buf, sizeof(buf), "%s", s_app ? tr(s_app->nome) : tr("Início"));
    tab5_cortar_com_reticencias(d, buf, sizeof(buf), APP_MAX_L);
    d.drawString(buf, app_x, ALTURA_BARRA / 2);
    ESQ_FIM = app_x + (int)d.textWidth(buf);
  }
  tab5_fonte_corpo(d);

  // Wi-Fi state. Everything here comes from cache: tab5_wifi_* never goes to
  // the C6; what talks over SDIO is the service task, once every 5s.
  const tab5_wifi_estado_t ew = tab5_wifi_estado();

  uint16_t cor_wifi = TEMA_TEXTO_3;   // off
  int      barras   = 0;
  bool     cortado  = true;

  if (ew == TAB5_WIFI_CONECTADO)
  {
    cor_wifi = TEMA_TEXTO;
    cortado  = false;
    const int r = (int)tab5_wifi_rssi();
    // With no signal reading, we show the FULL icon instead of empty: we are
    // actually connected (we have an IP), and drawing zero bars would tell the
    // eye the connection is bad - swapping missing information for wrong information.
    barras = (r == TAB5_WIFI_RSSI_DESCONHECIDO) ? 4
           : (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
  }
  else if (ew == TAB5_WIFI_SUBINDO || ew == TAB5_WIFI_CONECTANDO)
  {
    // Yellow and WITHOUT the X: it is trying, it has not failed yet. Showing the
    // X here would make the screen say "no connection" during a connection in progress.
    cor_wifi = TEMA_ALERTA;
    barras   = 0;
    cortado  = false;
  }
  else if (ew == TAB5_WIFI_FALHOU || ew == TAB5_WIFI_SEM_CREDENCIAL
        || ew == TAB5_WIFI_C6_MUDO)
  {
    // C6_MUDO has to be HERE and in Settings: it is not off, it is BROKEN,
    // and the difference decides whether you go to Settings to turn it back on
    // (pointless) or reach for the programmer. Every new enum state needs
    // revisiting in both places.
    cor_wifi = TEMA_PERIGO;
  }

  // ---- RIGHT: notice, Wi-Fi, battery ---------------------------------
  //
  // All grouped in the same corner on purpose. The Wi-Fi used to sit in the
  // MIDDLE of the bar, far from the battery, and the two answer the same question -
  // "how is the device right now". Apart, the eye had to sweep the whole bar to join them.
  //
  // Positions run from right to left, anchored on the battery icon: that way
  // changing one width does not misalign the others.
  const int bx      = L - 16 - 64;        // battery icon
  const int pct_dir = bx - 12;            // right edge of the "100%"
  // 82: enough for the X's right arm (which reaches cx+22) not to touch the
  // percentage number, and no more than that - at 96 the icon floated
  // halfway along, away from the group it belongs to.
  const int wifi_cx = pct_dir - 82;       // centre of the Wi-Fi icon

  // THE MIDDLE BELONGS TO NOTICES ONLY, and it starts after ESQ_FIM. Before, the
  // notice rectangle was anchored on the Wi-Fi and grew 150px to the left, over
  // the app name - that is what was eating the name.
  const int bt_v     = bt_visual();
  const int bt_cx    = wifi_cx - 46;
  const int meio_ini = ESQ_FIM + 8;
  int meio_fim = bt_v ? (bt_cx - 18) : (wifi_cx - 30);

  notif_item_t lista[NOTIF_MAX_FONTES];
  int n_avisos = notif_listar(lista, NOTIF_MAX_FONTES);
  // NOW PLAYING (kernel/midia.h): its own chip, on the right of the middle, and
  // the notices in what is left - music playing does not hide a new message, nor
  // the other way round. The player's grey background-part chip is dropped from the
  // list: the music one already says it is running, with more information.
  midia_info_t mi;
  const bool tem_midia = midia_ler(&mi);
  if (tem_midia && mi.fonte) {
    int k = 0;
    for (int i = 0; i < n_avisos; ++i) {
      if (strcmp(lista[i].nome, mi.fonte) != 0) { lista[k++] = lista[i]; }
    }
    n_avisos = k;
  }
  tab5_tela_limpar(meio_ini, 4, meio_fim - meio_ini, ALTURA_BARRA - 8, TEMA_FUNDO);
  if (tem_midia) {
    const int vao = meio_fim - meio_ini;
    const int max_l = n_avisos ? (vao / 2 < MIDIA_PAST_MAX ? vao / 2 : MIDIA_PAST_MAX)
                               : (vao < MIDIA_PAST_MAX ? vao : MIDIA_PAST_MAX);
    const int l = desenhar_pastilha_midia(d, meio_fim, max_l, mi);
    meio_fim -= l + 12;
  }
  s_barra_midia = midia_versao();
  if (n_avisos > 0) {
    // NOTICES ON THE BAR ARE PILLS THAT SHARE THE MIDDLE: each app with
    // news gets a pill with the mini icon and the summary beside it ("Fulano:
    // oi" in Mesh), right to left (most severe first, the order from
    // notif_listar). Up to 3; with more, the first 3 and a "+N" beside them. The
    // space is split equally: a single notice can be wide, three share it;
    // with no room for text the pill shows only the icon. The full
    // text is still in the centre (tap the bar).
    const int MAX_P = 3, ALT = ALTURA_BARRA - 24, PY = (ALTURA_BARRA - ALT) / 2, GAP = 8;
    const int PONTO = 10, PAD = 14, PIL_MAX = 300, TXT_MIN = 48;   // mock-up measurements
    const int mostra = n_avisos < MAX_P ? n_avisos : MAX_P;
    int dir = meio_fim;
    if (n_avisos > mostra) {
      char mais[8];
      snprintf(mais, sizeof(mais), "+%d", n_avisos - mostra);
      fonte_barra(d, false, 22);
      const int lm = (int)d.textWidth(mais) + 2 * PAD;
      d.fillRoundRect(dir - lm, PY, lm, ALT, ALT / 2, TEMA_SUPERFICIE);
      d.setTextDatum(middle_center);
      d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
      d.drawString(mais, dir - lm / 2, ALTURA_BARRA / 2);
      dir -= lm + GAP;
    }
    int larg = (dir - meio_ini - (mostra - 1) * GAP) / (mostra > 0 ? mostra : 1);
    if (larg > PIL_MAX) { larg = PIL_MAX; }
    const bool com_txt = larg >= PAD + PONTO + 8 + TXT_MIN + PAD;
    if (!com_txt) { larg = ALT; }   // no text: just the dot, in a round pill
    fonte_barra(d, false, 22);
    for (int k = 0; k < mostra && dir - larg >= meio_ini; ++k) {
      const notif_item_t& a = lista[k];
      // Pill on the theme surface; from ATENCAO upwards it gets a 2 px border
      // in the level colour (the same as the dot), so serious ones jump out.
      const uint16_t cor_nivel = cor_do_nivel(a.nivel);
      const int x = dir - larg;
      d.fillRoundRect(x, PY, larg, ALT, ALT / 2, TEMA_SUPERFICIE);
      if (a.nivel >= NOTIF_ATENCAO) {
        d.drawRoundRect(x, PY, larg, ALT, ALT / 2, cor_nivel);
        d.drawRoundRect(x + 1, PY + 1, larg - 2, ALT - 2, ALT / 2 - 1, cor_nivel);
      }
      const int px = com_txt ? x + PAD + PONTO / 2 : x + larg / 2;
      d.fillCircle(px, ALTURA_BARRA / 2, PONTO / 2, cor_nivel);
      if (com_txt) {
        char t[NOTIF_RESUMO];
        snprintf(t, sizeof(t), "%s", a.resumo[0] ? a.resumo : tr(a.nome));
        const int tx = x + PAD + PONTO + 8;
        tab5_cortar_com_reticencias(d, t, sizeof(t), x + larg - PAD - tx);
        d.setTextDatum(middle_left);
        d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
        d.drawString(t, tx, ALTURA_BARRA / 2 + 1);
      }
      dir = x - GAP;
    }
    tab5_fonte_corpo(d);
  }

  // 56 wide, not 32: the icon grew to fit the X beside it. A short
  // rectangle would leave half an X on screen when the connection came back.
  tab5_tela_limpar(wifi_cx - 26, 4, 60, ALTURA_BARRA - 8, TEMA_FUNDO);
  if (bt_v) {
    tab5_tela_limpar(bt_cx - 14, 4, 28, ALTURA_BARRA - 8, TEMA_FUNDO);
    desenhar_bt(d, bt_cx, ALTURA_BARRA / 2, bt_v);
  }
  s_barra_bt = bt_v;

  // The centre maths, because a loose "-1" would become a magic number in two weeks:
  // the drawing puts the base dot at cy+9 and the biggest arc rises 20px from
  // there, so the icon spans (cy-11) to (cy+12) - centre at cy+0.5. For that
  // centre to land in the middle of the bar, cy = ALTURA_BARRA/2 - 1.
  // (Before it was (ALTURA_BARRA-18)/2, which left the icon 8px above centre.)
  const int rv = radio_visual();
  if (rv >= 100) { desenhar_espnow(d, wifi_cx, ALTURA_BARRA / 2 - 1, rv == 100); }
  else           { desenhar_wifi(d, wifi_cx, ALTURA_BARRA / 2 - 1, cor_wifi, barras, cortado); }

  // Battery on the right: just the icon and the percentage. Voltage, current and
  // power live in the Energy app - the bar is no place for diagnostic numbers.
  desenhar_bateria(d, bx, (ALTURA_BARRA - 30) / 2, m.pct, est);

  // ITS OWN SIZE, explicit - before it inherited whatever was left over from the
  // P4 or the notice (depending on whether there was a notice in the pass), a
  // fragile coupling nobody had chosen.
  fonte_barra(d, false, 24);
  d.setTextDatum(middle_right);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--%%"); }
  else           { snprintf(buf, sizeof(buf), "%3d%%", m.pct); }
  d.drawString(buf, pct_dir, ALTURA_BARRA / 2);

  { float c = 0.0f; s_barra_temp = tab5_sensores_temp(&c) ? (int)(c + 0.5f) : -999; }
  hora_agora(s_barra_hora, sizeof(s_barra_hora));
  s_barra_bloq   = s_bloqueado;
  s_barra_pct    = m.pct;
  s_barra_wifi   = rv;
  s_barra_notif  = notif_versao();
  s_barra_estado = est;
  s_barra_nome   = s_app ? s_app->nome : nullptr;
  s_barra_idioma = idioma_versao();
}

// True if anything visible on the bar has changed since it was last drawn.
bool barra_mudou(void)
{
  tab5_medida_t m = tab5_power_ultima();     // likewise: cache, not I2C
  const char* nome = s_app ? s_app->nome : nullptr;
  // WHOLE DEGREE, not the float: the decimal changes on almost every reading and
  // the bar would be repainted once a second forever - exactly the cost this
  // function exists to avoid.
  // The temperature only counts on the lock screen (the only place it appears
  // on this bar); elsewhere, what changes on its own is the time.
  float c = 0.0f;
  const int temp = tab5_sensores_temp(&c) ? (int)(c + 0.5f) : -999;
  char hora[8];
  hora_agora(hora, sizeof(hora));

  return (m.pct != s_barra_pct)
      || (s_bloqueado != s_barra_bloq)
      || (s_bloqueado ? (temp != s_barra_temp) : (strcmp(hora, s_barra_hora) != 0))
      || (estado_energia(m) != s_barra_estado)
      || (nome != s_barra_nome)
      // Without this line the Wi-Fi icon only changed when the BATTERY changed:
      // connecting or dropping repainted nothing, and the bar lied for minutes.
      || (radio_visual() != s_barra_wifi)
      || (bt_visual() != s_barra_bt)
      // A new notice has to appear straight away, not at the next 1% of battery.
      // The VERSION changes only when something really changed - a service that
      // republishes the same state does not make the bar repaint.
      || (notif_versao() != s_barra_notif)
      || (midia_versao() != s_barra_midia)
      // Changing the language changes the app name and "Início" without changing anything else.
      || (idioma_versao() != s_barra_idioma)
      || recado_pendente();
}

// Last services line drawn. It exists for the same reason as
// barra_mudou(): the strip is redrawn only on a FULL repaint, and its content
// changes on its own - the Grove swaps module, the card goes in, the radio goes quiet.
// Without comparing, the line would go stale on screen until someone opened and closed an app.
static char s_nav_txt[128] = "";
static char s_nav_temp[16] = "";  // the temperature the strip shows (with an app open)

// THE TOP BAR'S CLOCK (and the lock screen's). "--:--" while the clock has not been
// set (no valid RTC nor network) - better than a time from 1970.
void hora_agora(char* out, size_t n)
{
  const uint32_t t = tab5_hora_epoch();
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  tab5_hora_local(t, &lt);
  tab5_hora_hhmm(&lt, out, n);
}

// THE P4 TEMPERATURE IN THE BOTTOM STRIP (the time moved up to the top bar).
//
// The same column layout GROVE and CARTAO use - small caption on top,
// value below - with the thermometer on the left, spanning both lines:
//
//   |  P4 · avg 41°
//   o  47°C
//
// The caption says WHOSE the measurement is and carries the AVERAGE since boot
// (tab5_sensores_temp_media): a number on its own does not say whether 47 is a lot or
// normal for this device; next to the average, it does. The thermometer rises and changes
// colour with the reading (cor_da_temp), so you can read the state without reading the number.

// Key of what is drawn: only repaints when the WHOLE degree (or the average)
// changes - the decimal would change on almost every reading.
static void temp_chave(char* out, size_t n)
{
  float c = 0.0f, m = 0.0f;
  const int ci = tab5_sensores_temp(&c) ? (int)(c + 0.5f) : -999;
  const int mi = tab5_sensores_temp_media(&m) ? (int)(m + 0.5f) : -999;
  snprintf(out, n, "%d/%d", ci, mi);
}

static const int TEMP_TERMO_L = 26;   // thermometer (14) and the gap to the text

// Builds the two texts and returns the width of the whole block, for whoever wants
// to centre it. Leaves the font at body.
static int temp_textos(lgfx::LovyanGFX& d, char* rot, size_t nr, char* val, size_t nv,
                       bool* tem, float* c)
{
  float m = 0.0f;
  *tem = tab5_sensores_temp(c);
  if (tab5_sensores_temp_media(&m)) { snprintf(rot, nr, tr("P4 · méd %.0f°"), (double)m); }
  else                              { snprintf(rot, nr, "P4"); }
  if (*tem) { snprintf(val, nv, "%.0f°C", (double)*c); }
  else      { snprintf(val, nv, "--"); }
  tab5_fonte_corpo(d);
  tab5_texto(d, 1.95f);
  const int lr = (int)d.textWidth(rot);
  tab5_texto(d, 3.45f);
  const int lv = (int)d.textWidth(val);
  return TEMP_TERMO_L + (lr > lv ? lr : lv);
}

// Draws the block with its left edge at x0. Clears nothing: the caller has already
// cleared the strip (or its own part of it).
static void desenhar_temp_nav(lgfx::LovyanGFX& d, int x0)
{
  const int y = d.height() - ALTURA_NAV;
  char rot[32], val[16];
  bool tem; float c = 0.0f;
  temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);

  // The thermometer spans cy-20 to cy+12: cy = middle of the strip + 4 centres it.
  desenhar_termometro(d, x0 + 7, y + ALTURA_NAV / 2 + 4, tem, c, TEMA_SUPERFICIE);

  const int tx = x0 + TEMP_TERMO_L;
  d.setTextDatum(top_left);
  tab5_texto(d, 1.95f);   // the same pair of caption/value sizes as GROVE and CARTAO
  d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
  const int rot_y = y + 8;
  d.drawString(rot, tx, rot_y);
  const int alt_rot = d.fontHeight();
  tab5_texto(d, 3.45f);
  d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
  d.drawString(val, tx, rot_y + alt_rot + 4);
  tab5_fonte_corpo(d);
}

// With an app open the temperature sits to the LEFT of the pill - only it is
// repainted when the degree ticks over, never the pill.
static void desenhar_temp_nav_app(lgfx::LovyanGFX& d)
{
  const int y = d.height() - ALTURA_NAV;
  int l = d.width() / 2 - NAV_BTN_L / 2 - 24;   // up to near the pill, without touching it
  if (l > 240) { l = 240; }
  tab5_tela_limpar(16, y + 1, l, ALTURA_NAV - 1, TEMA_SUPERFICIE);
  desenhar_temp_nav(d, 24);
}

// Builds the two fields. ALL FROM CACHE: no probing, no card mounting,
// nothing that goes to I2C. This function runs on the periodic tick, and the
// drawing path is no place to go to the hardware.
static void montar_servicos(char* esq, size_t te, char* dir, size_t td)
{
  if (!ajustes_grove()) {
    snprintf(esq, te, "%s", tr("desligado"));
  } else if (tab5_mesh_servico_ativo()) {
    // With the service up, what matters is not "there is a C6L there" but whether it
    // is responding and how many nodes it reaches.
    snprintf(esq, te, tr("Meshtastic - %d nós, %s"),
             tab5_mesh_num_nos(), tr(tab5_mesh_no_ar() ? "no ar" : "mudo"));
  } else if (tab5_gps_servico_ativo()) {
    // Same idea as Mesh above: what matters is whether there is a real FIX, not just
    // "there is a GPS there". qualidade==0 is "receiving the module but no fix yet" -
    // happens in the first few seconds, or under heavy cloud.
    const nmea_posicao_t p = tab5_gps_posicao();
    if (p.valida && p.qualidade > 0) {
      snprintf(esq, te, tr("GPS - fix (%u sats)"), (unsigned)p.sats_usados);
    } else {
      snprintf(esq, te, "%s", tr("GPS - sem fix"));
    }
  } else {
    snprintf(esq, te, "%s", tr(grove_nome(tab5_grove_tipo())));
  }

  snprintf(dir, td, "%s", tr(tab5_sd_descricao()));
}

// TWO COLUMNS WITH CAPTIONS, not a run-on line.
//
// The run-on line overflowed the area in portrait - 90 characters do not fit in
// 720px - and even when it fitted it was a paragraph where a panel belonged. With a
// small caption on top and the value below, the eye finds the field without reading it all.
static void desenhar_servicos(lgfx::LovyanGFX& d, const char* esq, const char* dir)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  // THE LARGEST OF THOSE LEFT. 1280x95 is ~243KB in one
  // burst, and it is repainted on every change to the services line.
  tab5_tela_limpar(0, y + 1, L, ALTURA_NAV - 1, TEMA_SUPERFICIE);

  // Top rule and divider: that is what turns two loose texts into a panel. The
  // divider spans only the content height, without touching the edges - touching,
  // it would become a wall and cut the strip into two screens.
  // THREE COLUMNS: GROVE | P4 | CARTAO. The middle one is narrow and
  // holds only the temperature; the outer ones share the rest.
  const int meio_l = 210;
  const int lado_l = (L - meio_l) / 2;
  d.drawFastHLine(0, y, L, TEMA_LINHA);
  d.drawFastVLine(lado_l, y + 16, ALTURA_NAV - 32, TEMA_LINHA);
  d.drawFastVLine(lado_l + meio_l, y + 16, ALTURA_NAV - 32, TEMA_LINHA);

  {
    char rot[32], val[16];
    bool tem; float c = 0.0f;
    const int bl = temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);
    desenhar_temp_nav(d, L / 2 - bl / 2);
  }

  struct { const char* rot; const char* val; int cx; } col[2] = {
    { tr("GROVE"),  esq, lado_l / 2 },
    { tr("CARTÃO"), dir, L - lado_l / 2 },
  };

  for (int i = 0; i < 2; ++i) {
    d.setTextDatum(top_center);

    // LABEL AND VALUE POSITION ACTUALLY MEASURED (d.fontHeight()), no
    // longer fixed "y+22"/"y+44" (they cut off the bottom of the text): the old offsets were worked out by
    // eye for the size BEFORE the increase, and did not follow when the
    // label/value grew (+30%/+15%). Measuring the real height
    // of EACH size before positioning the next removes that
    // mismatch for good - the text grows, the reserved space grows with it.
    tab5_texto(d, 1.95f);  // +30%; +50%
    d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
    const int rot_y = y + 8;   // 72 px strip: label + value + breathing room fit with 8 on top
    d.drawString(col[i].rot, col[i].cx, rot_y);
    const int alt_rot = d.fontHeight();

    tab5_texto(d, 3.45f);  // +15%; +50%
    d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
    // Cut to what fits in HALF the screen, not to the buffer size: the text
    // is centred, so a long value would bleed out on both sides. Cuts
    // character by character, measuring for real (d.textWidth) - the
    // "12px per character" maths was for Font0 (fixed width) and does not hold for Font4
    // (proportional).
    char cabe[40];
    snprintf(cabe, sizeof(cabe), "%s", col[i].val);
    tab5_cortar_ao_caber(d, cabe, lado_l - 32);   // per character: "nós" has an accent
    d.drawString(cabe, col[i].cx, rot_y + alt_rot + 4);
  }
}

// Called by the main loop (core.cpp) on the same tick as the status bar -
// it only redraws if the TEXT changed. Comparing 90-byte strings every two
// seconds is cheaper than painting the strip for nothing. Only makes sense on the HOME
// screen (s_app==NULL): with an app open the strip shows the button, not services.
void atualizar_linha_servicos(void)
{
  if (s_bloqueado) { return; }   // the bottom strip is black while locked
  char temp[16];
  temp_chave(temp, sizeof(temp));
  if (s_app) {
    // With an app open only the temperature changes on its own in this strip.
    if (strcmp(temp, s_nav_temp) != 0) {
      snprintf(s_nav_temp, sizeof(s_nav_temp), "%s", temp);
      tela_virtual().startWrite();
      desenhar_temp_nav_app(tela_virtual());
      tela_virtual().endWrite();
    }
    return;
  }
  char e[64], v[64], junto[128];
  montar_servicos(e, sizeof(e), v, sizeof(v));
  snprintf(junto, sizeof(junto), "%s|%s|%s", e, v, temp);
  if (strcmp(junto, s_nav_txt) != 0) {
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s", junto);
    tela_virtual().startWrite();
    desenhar_servicos(tela_virtual(), e, v);
    tela_virtual().endWrite();
  }
  // The home screen's clock card goes on the same tick: it compares what it
  // shows itself (time, day, Wi-Fi, Mesh) and only repaints when something changed.
  home_cartao_atualizar();
}

// WHAT THE STRIP'S BUTTON DOES, and the drawing follows. Today it has only one job
// - close the app and return to the home screen - but the symbol comes from here, not
// from text baked into the drawing: the day the button does something else (an app
// with levels going back one level, for example), change this function and the touch
// handling, and the drawing follows by itself.
typedef enum {
  NAV_INICIO,   // circle: the home screen (Android's "home")
  NAV_VOLTAR,   // triangle pointing left: one step back
} nav_acao_t;

static nav_acao_t nav_acao(void)
{
  return NAV_INICIO;
}

// The navigation symbols, in a 2 px stroke. The little house is the mock-up's
// (24x24 viewBox at 26 px); "back" is still the triangle.
static const int NAV_ICONE_L = 26;   // side length of the house drawing

static void desenhar_icone_nav(lgfx::LovyanGFX& d, int cx, int cy, nav_acao_t acao,
                               uint16_t tinta, uint16_t fundo)
{
  if (acao == NAV_VOLTAR) {
    d.fillTriangle(cx - 13, cy, cx + 10, cy - 14, cx + 10, cy + 14, tinta);
    d.fillTriangle(cx - 7, cy, cx + 7, cy - 9, cx + 7, cy + 9, fundo);
    return;
  }
  // Little house: roof, walls, floor and door (mock-up points, scaled 26/24).
  const float k = NAV_ICONE_L / 24.0f;
  const float ox = cx - NAV_ICONE_L / 2.0f, oy = cy - NAV_ICONE_L / 2.0f;
  auto traco = [&](float x0, float y0, float x1, float y1) {
    tab5_linha_grossa(d, ox + x0 * k, oy + y0 * k, ox + x1 * k, oy + y1 * k, 1.0f, tinta);
  };
  traco(3, 11, 12, 4);  traco(12, 4, 21, 11);     // roof
  traco(4, 11, 4, 21);  traco(20, 11, 20, 21);    // walls
  traco(4, 21, 9, 21);  traco(15, 21, 20, 21);    // floor, open at the door
  traco(9, 21, 9, 15);  traco(15, 21, 15, 15);    // door jambs
  traco(9, 15, 15, 15);                       // door lintel
}

void desenhar_barra_nav(lgfx::LovyanGFX& d)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  tab5_tela_limpar(0, y, L, ALTURA_NAV, TEMA_SUPERFICIE);
  // MARK. desenhar_servicos just below asks for the same clear - and asks
  // unconditionally, as it should. Without this mark the strip was cleared TWICE on the home
  // screen: 276KB where 138 would do, in the heaviest transition there is.
  tab5_tela_marcar_limpo(0, y, L, ALTURA_NAV, TEMA_SUPERFICIE);
  d.drawFastHLine(0, y, L, TEMA_LINHA);

  // ON THE HOME SCREEN, THE STRIP SAYS WHAT IS TURNED ON IN THE DEVICE.
  //
  // With no app open there is no "INICIO" to draw and the strip sat empty. The
  // TOP bar already answers "how is the device" - battery, temperature,
  // Wi-Fi. What it does not answer is "what is turned on in it", and that is the
  // question that cost time in recent sessions: the Grove swapped module and
  // nobody said, the card said "no card" with a card inside.
  //
  // All of this comes from CACHE - no probing, no card mounting, nothing
  // that goes to I2C. The strip is drawn along with the rest of the home screen, and the
  // drawing path is no place to go to the hardware.
  if (!s_app) {
    char e[64], v[64], temp[16];
    montar_servicos(e, sizeof(e), v, sizeof(v));
    temp_chave(temp, sizeof(temp));
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s|%s|%s", e, v, temp);
    desenhar_servicos(d, e, v);
    return;
  }

  if (s_app) {
    // Locked: drawn dimmed (everything in TEMA_TEXTO_3, without the raised pill), so
    // the eye knows there is no point tapping.
    const uint16_t fundo = s_nav_travada ? (uint16_t)TEMA_SUPERFICIE : (uint16_t)TEMA_SUPERFICIE_ALTA;
    const uint16_t tinta = s_nav_travada ? (uint16_t)TEMA_TEXTO_3    : (uint16_t)TEMA_TEXTO;
    // Fully round pill (radius = height/2). The height is the touch
    // rectangle's (NAV_BTN_A): the drawing must not go beyond the area that responds.
    const int bx = L / 2 - NAV_BTN_L / 2, by = y + NAV_BTN_Y0;
    d.fillRoundRect(bx, by, NAV_BTN_L, NAV_BTN_A, NAV_BTN_A / 2, fundo);
    // Little house + "Início" together, the pair centred in the pill.
    const int GAP_ICONE = 10;
    d.setFont(tab5_fonte_suave(true, 26));
    d.setTextSize(1);
    const char* rotulo = tr("Início");
    const int conj = NAV_ICONE_L + GAP_ICONE + (int)d.textWidth(rotulo);
    const int cy = by + NAV_BTN_A / 2;
    const int x0 = L / 2 - conj / 2;
    desenhar_icone_nav(d, x0 + NAV_ICONE_L / 2, cy, nav_acao(), tinta, fundo);
    d.setTextDatum(middle_left);
    d.setTextColor(tinta, fundo);
    d.drawString(rotulo, x0 + NAV_ICONE_L + GAP_ICONE, cy);
    tab5_fonte_corpo(d);
    temp_chave(s_nav_temp, sizeof(s_nav_temp));
    desenhar_temp_nav_app(d);
  }
}
