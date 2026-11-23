// EDITOR - multi-line text, with a cursor, for those who write more than a
// message (the Notes app). Wraps by word and by '\n', scrolls with the finger,
// system virtual keyboard (and the physical one on ExtPort1), paste from the
// clipboard.
//
// THE TEXT BELONGS TO THE APP: it passes the buffer (PSRAM) and the size; the editor only works
// inside it. The line-start positions live in PSRAM and only exist while
// the editor is open.
//
// HOW TO USE (see app_notas/app_notas.cpp):
//   abrir:        editor_abrir(&s_ed, buf, cap);        (the text already in buf)
//   fechar:       editor_fechar(&s_ed);                 (also in ao_fechar)
//   desenhar:     editor_desenhar(&s_ed, d, retangulo, completo);
//   ao_tocar:     editor_tocar(&s_ed, x, y);
//   nivel:        editor_tocar_nivel(&s_ed, x, y, pressionado);
//   ao_teclar:    editor_teclar(&s_ed, texto, codigo, mod);
//
// CODE MODE (the Berry Editor): editor_modo_codigo() after opening -
// monospaced font, line number in the margin, Berry syntax colours,
// Enter keeps the indent, Tab = 2 spaces; editor_marcar_erro() paints the
// error line and moves the cursor to it (any change to the text clears the mark).
//
// Holding a finger on the text opens the menu right there: copy the WORD under the finger,
// the LINE (the paragraph up to the break), ALL, or Paste; Ctrl+V pastes.
#pragma once

#include "app.h"
#include "teclado_virtual.hpp"
#include "toque_longo.hpp"

typedef struct {
  char*     txt;          // the app's
  int       cap;          // buffer bytes, including the '\0'
  int       cursor;       // in bytes, always on a character boundary
  bool      mudou;        // the text changed since opening (the app decides whether to save)

  // Internal state - the app does not touch it.
  int16_t*  ini;          // start of each line; ini[n_lin] = end (PSRAM, cap + 1)
  int       n_lin, topo, larg, passo, linhas_vis;
  bool      requebrar, sujo;
  area_t    ret;          // the last rectangle (screen)
  int       alt_texto;    // the part of it that is text (the rest belongs to the keyboard)
  tecvirt_t kb;
  toque_longo_t lp;
  int       arr_y, arr_topo;
  bool      arrastando;
  bool      menu;         // the long-press menu is open, at (menu_x, menu_y) - screen
  int       menu_x, menu_y;
  int       menu_pos;     // the text byte under the finger when the menu opened
  bool      codigo;       // code mode (see above)
  int       calha;        // code mode: px of the number column (grows with the digits)
  int       linha_erro;   // line (1 = the first) marked as an error; 0 = none
  lgfx::LovyanGFX* disp;  // to measure text on touch (UI task only)
} editor_t;

bool editor_abrir(editor_t* e, char* buf, int cap);
void editor_fechar(editor_t* e);
void editor_desenhar(editor_t* e, lgfx::LovyanGFX& d, const area_t& r, bool completo);
void editor_tocar(editor_t* e, int x, int y);
void editor_tocar_nivel(editor_t* e, int x, int y, bool pressionado);
void editor_teclar(editor_t* e, const char* texto, uint8_t codigo, uint8_t mod);
void editor_colar(editor_t* e);
void editor_fechar_teclado(editor_t* e);
void editor_modo_codigo(editor_t* e);
void editor_marcar_erro(editor_t* e, int linha);
