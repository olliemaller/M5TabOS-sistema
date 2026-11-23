// SCROLLABLE LIST - the first standard component of the new base (docs/COMPOSITOR.md,
// stage 3b). Same idea as the LVGL list and that of any phone system:
//
//   AN ITEM IS MEASURED ONCE, WHEN IT CHANGES. THE LIST KEEPS THE RESULT.
//
// The app says only three things: how many items exist, the height of each (which
// it calculated and stored) and how to draw item i at position y. The list
// takes care of the rest - scrolling, limit, sum of heights, clipping, scroll
// bar, and the cheap scroll path: shifting what is already drawn on the
// virtual screen and drawing ONLY the newly exposed strip, and only with the items that
// cross it.
//
// Before this every list in the system reimplemented that core, and the Mesh one
// measured ALL messages on every scroll step (line wrapping, emoji,
// service mutex) just to discover heights that had not changed.
//
// Usage (EVERY scrollable list in the system follows this - see docs/ESCREVENDO_APPS.md):
//   static lista_t s_l;
//   // on open:   lista_definir(&s_l, ROLAGEM_INICIO, n_itens, altura_de, desenhar_item, ctx);
//   // items changed (added, removed, re-measured):  lista_itens_mudaram(&s_l, n);
//   // ONE item changed value (age, counter):  lista_item_mudou(&s_l, i);
//   // ao_tocar_nivel: if (lista_arrastar(&s_l, x, y, pressionado)) sistema_pedir_redesenho();
//   // ao_tocar:       const int i = lista_item_tocado(&s_l, x, y, &dy);   // -1 = nothing/drag
//   // desenhar:       lista_desenhar(&s_l, d, janela, fundo, tudo);
//
// TOUCH BELONGS TO THE LIST TOO. Before, each app decided "is the finger
// on the list?" and "which item is this?" with its own maths - the same geometry
// written twice (in drawing and in touch), with the scroll added by hand. That is
// exactly how the neighbouring item got opened and how a column hidden
// by the keyboard kept scrolling underneath it. Now the list uses the window of the
// LAST draw: what it did not draw, it does not react to.
//
// The bar column (8px to the right of the window) belongs to the list: do not draw items
// there.

#pragma once

#include "rolagem.hpp"

typedef int  (*lista_altura_t)(int i, void* ctx);
typedef void (*lista_desenhar_t)(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void* ctx);

typedef struct {
  rolagem_t        rol;
  int              n;
  lista_altura_t   altura;
  lista_desenhar_t desenhar;
  void*            ctx;

  int              topo;   // breathing room before item 0 (px) - scrolls with it

  // Internal state.
  int     total;         // topo + sum of heights (valid if medida_ok)
  bool    medida_ok;
  bool    tudo;          // the next pass redraws the whole window
  area_t  janela_ant;    // to notice that the window changed (keyboard opened)
  bool    no_ar;         // drawn on this screen (janela_ant is valid for touch)
  int     sujo_de, sujo_ate;   // items to redraw on their own; de > ate = none
} lista_t;

// Prepares the list. ROLAGEM_INICIO = read from the top; ROLAGEM_FIM = anchored at the
// end (messages: 0 = the most recent, stuck to the bottom).
void lista_definir(lista_t* l, uint8_t ancoragem, int n,
                   lista_altura_t altura, lista_desenhar_t desenhar, void* ctx);

// The items changed (added, removed, some re-measured). Redraws everything on the
// next pass. In a list anchored at the END, read mid-history,
// what GREW comes in below without dragging what you are reading.
void lista_itens_mudaram(lista_t* l, int n);

// An item changed VALUE without changing height (a node's age, unread
// counter). Only it is redrawn on the next pass - and only if visible.
void lista_item_mudou(lista_t* l, int i);

// Goes back to the origin (top, or the end in a list anchored at the end).
void lista_ao_inicio(lista_t* l);

// The screen that showed the list has gone (another page of the app): it stops reacting
// to touch until drawn again.
void lista_esconder(lista_t* l);

// Draws what is needed: the whole window if 'tudo' (or if something changed), only the
// strip the scroll exposed, or only the items marked with lista_item_mudou.
// Returns true if it painted anything.
bool lista_desenhar(lista_t* l, lgfx::LovyanGFX& d, const area_t& janela,
                    uint16_t fundo, bool tudo);

// TOUCH, in app AREA coordinates (the ones ao_tocar/ao_tocar_nivel
// receive). true = scrolled (call sistema_pedir_redesenho).
bool lista_arrastar(lista_t* l, int x, int y, bool pressionado);

// The item under the finger at the end of a touch; -1 = outside the list or in an empty spot;
// LISTA_ARRASTE = the touch was the END of a drag on this list (a drag is not a
// click - the app must not treat it as a touch on anything, not even a button). dy = where,
// within the item, the finger landed (0 = top of the item), for buttons inside the item.
#define LISTA_ARRASTE (-2)
int  lista_item_tocado(lista_t* l, int x, int y, int* dy);
// The item under (x, y) now, without the drag rule - for a long press, with the
// finger still on the screen. -1 = none.
int  lista_item_em(lista_t* l, int x, int y, int* dy);
