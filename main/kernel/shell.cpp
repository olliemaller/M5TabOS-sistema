// The M5tabOS "shell": status bar, home screen (launcher), routing
// touch to the right app, and touch calibration.
//
// -------------------------------------------------------------------------
// STAGE 1 of docs/ARQUITETURA.md - the SHELL half (appearance)
// -------------------------------------------------------------------------
// Until now this lived inside kernel/sistema.cpp, together with the ENGINE (drawing
// schedule, app life cycle, power policy) - 2457 lines
// doing eight different things. Split in two, without changing a line of
// logic (the only exception is documented in kernel/internal.hpp, next to the
// area_util() -> sistema_area() swap): core.cpp is the engine, this file is
// the appearance.
//
// "shell" in the operating-system sense (Windows Shell, GNOME Shell): the
// visual layer around the apps. It has NO relation to console.cpp (which is a
// COMMAND-LINE shell, another concept, another file).
//
// ONE task alone owns the display and I2C, on purpose - see the identical
// note in core.cpp. Apps do not draw from their own tasks; each one's desenhar()
// function is called by the engine, which lives in the other file.
//
// -------------------------------------------------------------------------
// SPLIT BY TOPIC (only moved between files, no logic changed)
// -------------------------------------------------------------------------
// This file kept the touch geometry, the vertical correction, the
// touch diagnostics, the gestures, tratar_toque() (the
// touch routing). The rest went to kernel/shell_*.cpp; what the parts
// share is in kernel/shell_interno.hpp:
//   shell_barra.cpp     battery icon, status bar, time, home's bottom strip, navigation button
//   shell_home.cpp      home pages, icon order, move/remove icon, drawing home
//   shell_bloqueio.cpp  lock screen
//   shell_avisos.cpp    notification centre and mini player

#include "app.h"
#include "shell_interno.hpp"
#include "tela_virtual.hpp"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "shell";

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// VERTICAL TOUCH CORRECTION
// ---------------------------------------------------------------------------
// The capacitive panel returns the CENTROID of the contact area, and the finger does not
// touch the screen with the tip you see: it touches with the pad, a few millimetres
// lower. The eye aims at the visible edge, the sensor measures the middle of the patch - and the
// reading comes out systematically BELOW where you thought you touched.
//
// The symptom is always the same and was exactly what was observed: "I have to touch higher
// up to hit the button". It is not panel imprecision; it is the difference
// between where you aim and where you make contact.
//
// At this panel's 294 DPI, 10px is ~0.85mm - within the range operating
// systems apply for the same thing. It is not a blind guess: the console `toque` command
// measures the real offset, and this number exists to be tuned with
// that data, not with impressions.
//
// It sits in the SHELL and not in the apps on purpose: it is a property of the panel and the
// finger, the same for everyone. Each app correcting on its own would be eight
// different corrections for a single cause. The value (TOQUE_DY) lives in
// kernel/internal.hpp: level touch, in core.cpp, applies the same.

// Touch diagnostics, turned on by the console. Prints where the finger landed, raw and
// already converted - the only way to swap "seems imprecise" for a number.
static bool s_diag_toque = false;
bool diag_toque_ligado(void) { return s_diag_toque; }
void sistema_diag_toque(bool ligar)
{
  s_diag_toque = ligar;
  ESP_LOGI(TAG, "touch diagnostics %s (current correction: %d px)",
           ligar ? "ON" : "off", TOQUE_DY);
}

// Locked: drawn dimmed in the navigation bar, so the eye knows there is
// no point touching. See sistema_travar_navegacao() just below.
//
// NOT 'static' - core.cpp also writes false directly in fechar_app() (safety
// net: a closed app must not leave navigation locked for the
// next one). Declared extern in kernel/internal.hpp.
bool s_nav_travada = false;
void sistema_travar_navegacao(bool t)
{
  s_nav_travada = t;
  if (t && s_painel_avisos) { painel_avisos_abrir(false); }   // opened before the lock: close it
  s_sujo_layout = true;
}

// ---------------------------------------------------------------------------
// GESTURES
// ---------------------------------------------------------------------------
static const int GESTO_PAGINA_PX     = 120;   // minimum sideways drag to change page
static const int GESTO_DESBLOQUEIO_PX = 220;  // minimum upward swipe to unlock

void tratar_gesto(int dx, int dy, int y0)
{
  if (s_bloqueado) {
    if (dy < -GESTO_DESBLOQUEIO_PX && abs(dy) > 2 * abs(dx)) { desbloquear_tela(); }
    return;
  }
  if (s_painel_avisos) {
    if (abs(dy) > 2 * abs(dx) && abs(dy) > 40 && painel_avisos_rolar(y0 - TOQUE_DY, dy)) { return; }
    if (dy < -GESTO_PAGINA_PX && abs(dy) > 2 * abs(dx)) { painel_avisos_abrir(false); }
    return;
  }
  if (s_app) { return; }   // inside an app the drag is the app's
  if (abs(dx) > GESTO_PAGINA_PX && abs(dx) > 2 * abs(dy)) {
    home_mudar_pagina(dx < 0 ? +1 : -1);   // dragging left moves forward
  }
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------
void tratar_toque(int tx, int ty)
{
  // STILL MOUNTING THE SD for this app: swallow the touch. Neither the navigation ("INICIO")
  // nor the content really exist yet - ao_abrir() only runs when
  // concluir_abertura_do_sd() (core_apps.cpp) releases it. Lasts a few frames in practice.
  if (s_app_carregando) { return; }
  // Locked: only the swipe (tratar_gesto) unlocks; a simple touch does nothing.
  if (s_bloqueado) { return; }

  // sistema_area(), and not core.cpp's private area_util() function - the same
  // geometry, just through the public door (see the note in kernel/internal.hpp).
  area_t a = sistema_area();

  // Correction applied ONCE, on entry. From here down - bar, navigation,
  // home icons, apps - everyone already receives the corrected point, and none
  // of them needs to know the correction exists.
  const int ty_cru = ty;
  ty -= TOQUE_DY;

  // NOTIFICATION CENTRE: when open, the touch is all its own (including on the bar and the
  // navigation - any touch outside a line just closes it). When closed, tapping the
  // status bar opens it.
  if (s_painel_avisos) { painel_avisos_tocar(a, tx, ty); return; }
  // NAVIGATION LOCKED applies to the bar too: a line in the centre opens
  // another app, and that would close this one by a path the lock exists to
  // prevent (Pendrive, with the disk mounted on the computer).
  if (ty < ALTURA_BARRA) { if (!s_nav_travada) { painel_avisos_abrir(true); } return; }

  if (s_diag_toque) {
    // (the status bar has already returned above: only navigation or the area is left here)
    ESP_LOGI(TAG, "touch: raw (%d,%d) -> corrected (%d,%d) -> app (%d,%d)%s",
             tx, ty_cru, tx, ty, tx - a.x, ty - a.y + s_rol.atual,
             (ty >= M5.Display.height() - ALTURA_NAV) ? "  [navigation]" : "");
  }

  if (ty >= M5.Display.height() - ALTURA_NAV) {
    // ONLY THE BUTTON REGION closes the app now.
    //
    // UNTIL NOW, any touch on the whole navigation strip (96px
    // high, the FULL WIDTH of the screen) closed the app - only Y was checked
    // above, never X. In practice that never bothered anyone because there was
    // nothing else besides the button in that strip to touch by mistake - but it was
    // a touch area much bigger than the drawn pill, and it makes room to
    // put NON-touchable information beside it without that information
    // stealing the touch.
    //
    // The rectangle below is EXACTLY what desenhar_barra_nav() paints -
    // NAV_BTN_L/NAV_BTN_A/NAV_BTN_Y0, see the big note next to them.
    const int nav_y0 = M5.Display.height() - ALTURA_NAV;
    const int L = M5.Display.width();
    //
    // TOUCH AREA BIGGER THAN THE DRAWING. The pill shrank with the
    // strip (72px) and became tight for the finger - missing by 10px above or below
    // it did nothing. The touch now covers the WHOLE HEIGHT of the strip and
    // NAV_BTN_FOLGA extra on each side; the drawing stays the same. It does not go up
    // into the app area: what is above the strip belongs to the app.
    const bool dentro_do_botao =
        tx >= L / 2 - NAV_BTN_L / 2 - NAV_BTN_FOLGA && tx < L / 2 + NAV_BTN_L / 2 + NAV_BTN_FOLGA &&
        ty >= nav_y0;

    // NAVIGATION LOCKED: the strip becomes the app's (keyboard on screen). The
    // keyboard sits against the bar, and a finger that goes past the bottom row lands
    // here - delivered on the last line of the area, it counts as the bottom key instead of
    // vanishing. Same rule as level touch (core.cpp).
    if (s_nav_travada) {
      if (app_vivo() && s_app->ao_tocar) { s_app->ao_tocar(tx - a.x, a.altura - 1 + s_rol.atual); }
      return;
    }
    if (dentro_do_botao && s_app) { fechar_app(); }
    return;
  }

  if (s_app) {
    // SCROLL ADDED HERE, and only here. The app compares against the SAME constants
    // it used to draw, without knowing anything about scrolling.
    if (app_vivo() && s_app->ao_tocar) { s_app->ao_tocar(tx - a.x, ty - a.y + s_rol.atual); }
    return;
  }

  if (s_remover_app) { home_remover_tocar(tx, ty); return; }
  const int pos = home_pos_em(a, tx, ty, false);
  if (pos >= 0) { abrir_app(sistema_home_app(pos)); }
}
