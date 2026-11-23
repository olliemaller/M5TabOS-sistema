// Touch scrolling - the SYSTEM's mechanism, and no longer a single app's.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// There were TWO scrolling mechanisms, with no relation at all
// between them: the core's generic one (kernel/core.cpp, whose only consumer was
// Settings) and the Mesh app's own - THREE almost identical copies (conversations,
// nodes, messages), each with its own drag/shift pair
// written by hand. The three Mesh copies never got the 80ms brake that
// the Settings app always had (see the big note in kernel/core.cpp about
// REPINTURA_MIN_MS): Mesh's touch fell into the only branch of the main loop
// without throttling, and redrew at ~50x/s instead of ~12.5x/s.
//
// Design criterion: do not fix only Mesh (it would be a patch for one app
// alone) - the SYSTEM needs ONE mechanism, which the kernel and
// any app use, and which serves as the basis for whatever will still need
// scrolling in future (home itself, paged like a phone screen, when it
// fills up with apps).
//
// -------------------------------------------------------------------------
// WHO USES IT
// -------------------------------------------------------------------------
// This is the ENGINE. It has two consumers, and only two:
//   - kernel/lista  - EVERY scrollable app list (Mesh, Files, Tasks).
//                     The app never calls rolagem_* directly: it uses lista_*.
//   - kernel/core   - scrolling of the whole area (app with altura_conteudo,
//                     the Settings app).
// An app that needs to scroll something that is not a list (a map, a horizontal
// page) is a sign that a COMPONENT is missing from the kernel, not that the app
// should build its own scrolling with these functions.
//
// -------------------------------------------------------------------------
// WHAT THIS DOES NOT DO
// -------------------------------------------------------------------------
// It knows nothing about pages, columns, an open keyboard or any other zone
// rule - that is the caller's (see 'dentro' in rolagem_arrastar). It does not
// decide FULL vs LIGHT - that remains each app's decision (TUDO,
// pintura_completa(), etc). It does not touch the framebuffer by itself outside
// rolagem_deslocar()/rolagem_barra() - the rest is just bookkeeping.

#pragma once

#include <stdint.h>
#include <M5Unified.h>
#include "app.h"   // area_t

// AXIS and ANCHORING are SEPARATE fields, not a single four-value
// enum - they are orthogonal questions. "Vertical or horizontal" decides WHICH
// shift function applies (tab5_tela_rolar copies whole lines: it is
// vertical by construction). "Start or end" decides the SIGN of the drag and the
// position of the little bar. Home's horizontal paging, one day, will need
// the first field without touching the second - packing the two together would force
// inventing a value like "HORIZONTAL_FIM" that nobody wants, just to make the enum
// complete.
#define ROLAGEM_VERTICAL    0
#define ROLAGEM_HORIZONTAL  1   // reserved - see the paging note at the end of this file

#define ROLAGEM_INICIO  0   // ordinary list, read from the top (the default - hence zero)
#define ROLAGEM_FIM     1   // anchored at the end (e.g. messages - 0 = the most recent)

// Minimum distance from the finger's down point to count as a drag (and
// not a click). The SAME number for the whole system: the kernel (click, in
// core.cpp) and every list (kernel/lista) decide "was it a touch or a drag" by
// it.
//
// 24 px (~2 mm at 294 DPI). It was 12, and the kernel started using
// 24 in the new click recogniser - with two limits, a touch that moved
// 13..24 px got through the kernel and the LIST rejected it as "end of drag":
// in the Files app, touch after touch with no effect at all and no log. Before, M5Unified's
// wasClicked() rejected everything above 8 px and the list limit
// never came into play. A single number stops that coming back.
#define ROLAGEM_ARRASTE_MIN 24

typedef struct {
  int atual;         // current offset, in pixels; 0 = at the anchoring origin
  int no_visor;      // how much of 'atual' the VISIBLE PAGE already reflects
  int max;           // limit; 0 = does not scroll. The caller writes it (rolagem_limite
                      // recalculates it by itself when the content may shrink)
  int ancora;        // finger coordinate in the previous cycle; -1 = no finger
  int descida;        // coordinate WHERE the finger went down - the drag reference
  int afastamento;    // largest distance from the down point so far (drag vs. click)
  uint8_t eixo;        // ROLAGEM_VERTICAL/HORIZONTAL
  uint8_t ancoragem;   // ROLAGEM_INICIO/FIM
  int passo_encaixe;   // 0 = free scrolling. Reserved for future paging
                       // (snapping 'atual' to a multiple when the finger lifts) -
                       // it has no function yet because no screen would feel it.
} rolagem_t;

// What ONE pass of rolagem_deslocar() did - the caller uses it to clip
// (only the new strip needs redrawing).
typedef struct {
  int    delta;      // shift applied, in SCREEN space (what went
                      // to tab5_tela_rolar - already with the right sign for
                      // this list's anchoring)
  bool   deslocou;    // reused the pixels that were already right
  area_t exposta;     // the newly exposed strip; only valid when deslocou==true
} rolagem_passada_t;

// Prepares a scroll with explicit axis/anchoring. Only needs to be called
// for the NON-default case (ROLAGEM_FIM, or one day HORIZONTAL) - a
// zero-initialised rolagem_t (static, global) is already born VERTICAL/INICIO.
void rolagem_definir(rolagem_t* r, uint8_t eixo, uint8_t ancoragem);

// Returns 'atual' to the origin and releases the finger anchor - call when opening a
// new screen or changing the selected item. Does NOT touch 'no_visor': whoever
// zeroes it will also force a full pass (which synchronises by itself via
// rolagem_sincronizar), exactly as the kernel already did.
void rolagem_zerar(rolagem_t* r);

// "The visible page now reflects 'atual'." Call after any
// FULL pass (which redrew everything from scratch, so there is nothing left
// pending to shift).
static inline void rolagem_sincronizar(rolagem_t* r) { r->no_visor = r->atual; }

// True when the visible page does not yet reflect the current offset - the
// sign that a rolagem_deslocar() pass has work to do.
static inline bool rolagem_pendente(const rolagem_t* r) { return r->atual != r->no_visor; }

// ONE touch sample. 'coord' is the coordinate on THIS scroll's AXIS (y for
// vertical), already in the caller's space - this module knows nothing about zones,
// pages or an open keyboard, that is the responsibility of whoever computes
// 'dentro'. Returns true ONLY when 'atual' really changed (clamped to
// [0, r->max]) - a still touch or one outside the zone never returns true.
bool rolagem_arrastar(rolagem_t* r, int coord, bool dentro);

// "Was the last touch a DRAG, and not a click?" Compares the maximum distance
// (accumulated by rolagem_arrastar) against ROLAGEM_ARRASTE_MIN and ZEROES the
// distance - call once per finger release (e.g. inside the handling
// of wasClicked()), never more than once per touch.
bool rolagem_foi_arraste(rolagem_t* r);

// Recalculates 'max' from how much content exists (total) and how much fits
// in the visible window, and PULLS 'atual' back if it ended up past the new
// limit (the list shrank - an item was removed, a conversation changed).
// Returns true when it had to pull, so the caller can mark the area as
// dirty if need be.
bool rolagem_limite(rolagem_t* r, int total, int janela);

// Shifts the already-correct pixels inside 'janela' (tab5_tela_rolar) so
// that the visible page catches up with 'atual'. Synchronises 'no_visor' by itself,
// ALWAYS - whether it really moved (deslocou==true) or not (the caller will clear
// and redraw everything on its own, and then 'no_visor' has to keep up
// all the same, otherwise the next pass would think there was still something pending).
// Does NOT clear anything by itself when deslocou==false: the fallback is each
// caller's decision (the kernel falls back to a FULL repaint via the hidden
// page; Mesh clears the window immediately, on the visible page) - the two
// ways are legitimate and different, and should not be forced into one.
rolagem_passada_t rolagem_deslocar(rolagem_t* r, const area_t& janela, uint16_t cor);

// The little bar: track + dot, 4px wide at 'margem' px from the right
// edge of the window. What draws it is the list (the Settings app has no bar, by
// request).
void rolagem_barra(lgfx::LovyanGFX& d, const rolagem_t* r, const area_t& janela,
                   uint16_t cor_trilha, int margem = 8);


// -------------------------------------------------------------------------
// FUTURE PAGING (not implemented - the design merely does not prevent it)
// -------------------------------------------------------------------------
// When home needs horizontal pages: ROLAGEM_HORIZONTAL +
// ROLAGEM_INICIO (not a fifth anchoring value), 'passo_encaixe' > 0
// (the width of a page), and a new function - rolagem_soltar(r), called
// at the end of the drag - that snaps 'atual' to the nearest multiple of 'passo_encaixe'.
// rolagem_deslocar() would dispatch on 'eixo' (it would need a
// tab5_tela_rolar_h() in the HAL, since today's copies whole lines and is
// vertical by construction). There is no screen yet that would exercise that path, so
// there is nothing to test - which is why the function does not exist today, only the field.
