// What the shell files share: the geometry used by more than one of them, the
// state (declared here, defined in the file indicated) and the internal
// functions that cross files. For them only: no app includes this header.
// The core <-> shell bridge is still kernel/internal.hpp (included by
// this one); apps talk to the system only through app.h.
//
// The shell was split by subject (it only changed files, no logic):
//   shell.cpp           touch geometry, vertical correction, diagnostics, gestures, tratar_toque
//   shell_barra.cpp     battery icon, status bar, clock, home screen bottom strip, navigation button
//   shell_home.cpp      home screen pages, icon order, move/remove icon, home screen drawing
//   shell_bloqueio.cpp  lock screen
//   shell_avisos.cpp    notification centre and mini player
#pragma once

#include "internal.hpp"
#include "tab5_tema.hpp"

// ---------------------------------------------------------------------------
// Geometry used by more than one file
// ---------------------------------------------------------------------------
// GEOMETRY OF THE STRIP BUTTON (the pill in the navigation strip) - IN ONE
// PLACE, because desenhar_barra_nav() PAINTS into this rectangle
// and tratar_toque() now NEEDS to know the same rectangle so it only closes the
// app when the touch lands inside it (before, any touch on the whole
// 96px strip - the FULL WIDTH of the screen - closed the app; only Y was
// checked). Two diverging constants here would reopen that hole.
static const int NAV_BTN_L  = 160;               // pill width (was 180, along with the lower strip)
static const int NAV_BTN_A  = ALTURA_NAV - 32;   // pill height
static const int NAV_BTN_FOLGA = 48;            // touch beyond the pill, on each side (see tratar_toque)
static const int NAV_BTN_Y0 = 16;                // offset from the top of the strip to the top of the pill

static const int ICONE_LADO   = TEMA_ICONE_APP;
// Grid columns: 4 in portrait; in landscape (the docked physical keyboard turns
// the home screen sideways, kernel/orientacao) 8 fit with 2 rows across the 1280 px.
static inline int icone_cols(const area_t& a) { return a.largura > a.altura ? 8 : 4; }


// ---------------------------------------------------------------------------
// State used by more than one file (each defined in the file indicated)
// ---------------------------------------------------------------------------
// shell_home.cpp:
extern const app_t* s_remover_app;   // waiting for the confirmation's "Remover"

// ---------------------------------------------------------------------------
// Functions that cross files
// ---------------------------------------------------------------------------
// shell_barra.cpp:
uint16_t cor_do_nivel(notif_nivel_t n);   // the colour of each severity: bar, centre and lock screen
void hora_agora(char* out, size_t n);     // the top bar's time (and the lock screen's)

// shell_home.cpp:
bool home_mudar_pagina(int delta);
// Repaints only the home screen's clock card when the minute (or the Wi-Fi/Mesh)
// changed. The main loop calls it on the same tick as the status bar; without the home
// screen visible it does nothing.
void home_cartao_atualizar(void);
int home_pos_em(const area_t& a, int tx, int ty, bool folga);
void home_remover_tocar(int tx, int ty);

// shell_avisos.cpp:
bool painel_avisos_rolar(int y0, int dy);
void painel_avisos_tocar(const area_t& a, int tx, int ty);
