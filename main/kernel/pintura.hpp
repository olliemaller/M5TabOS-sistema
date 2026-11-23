// Paint by difference - the helper the apps were missing.
//
// -------------------------------------------------------------------------
// THE PROBLEM THIS SOLVES
// -------------------------------------------------------------------------
// M5GFX draws straight onto the visible framebuffer, so CLEARING TO REDRAW
// shows up as a flash. The rule is to write over the top, at a fixed width, and only
// when the value changed.
//
// But the rule alone was not enough: it was forgotten in five apps (Energia,
// Tarefas, Ajustes, Arquivos and Mesh), each fixed separately. And
// with it always came the same second mistake: forgetting that the SYSTEM clears the area
// (on opening, on returning from idle) and that from then on the app must repaint EVERYTHING,
// otherwise the screen is left blank - which is what happened with the Settings app.
//
// As long as this relies on the programmer's memory, it will keep failing. Here the
// two steps are built in: pintura_iniciar() checks the area clearing
// by itself, and on a full pass EVERY comparator returns "changed" - so the
// app repaints everything without having to remember anything.
//
// -------------------------------------------------------------------------
// HOW TO USE IT
// -------------------------------------------------------------------------
//   static pintura_t s_p;
//   static int s_bat; static char s_nome[24];
//
//   void desenhar(lgfx::LovyanGFX& d, const area_t& a)
//   {
//     pintura_iniciar(&s_p);                       // decides whether it is full
//     if (pintura_completa(&s_p)) { ...background, titles, frames... }
//
//     if (pintura_mudou_i(&s_p, &s_bat, bateria)) { ...draws the number... }
//     if (pintura_mudou_s(&s_p, s_nome, sizeof(s_nome), nome)) { ... }
//   }
//
//   // when changing page, selection, anything structural:
//   pintura_pedir(&s_p);  sistema_pedir_redesenho_completo();

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  bool completo;    // this pass repaints everything
  bool pedido;      // the app requested a total repaint
  bool ja_desenhou; // false until the first pass (which is always full)
} pintura_t;

// Call at the START of desenhar(). Decides whether the pass is full, by checking: whether
// it is the first, whether the app asked, or whether the system cleared the area.
void pintura_iniciar(pintura_t* p);

// The app requests a total repaint. Use it when changing page, selected item or
// anything that reorganises the screen - and also call
// sistema_pedir_redesenho_completo(), which is what schedules the drawing.
void pintura_pedir(pintura_t* p);

static inline bool pintura_completa(const pintura_t* p) { return p->completo; }

// ONLY FOR DRAWING DECISIONS. Do not use these comparators to decide I/O (rereading
// the card, opening a file, talking to hardware): they return true on every
// FULL pass, by design, and an I/O that triggers a full repaint goes into a loop -
// that is what made the Mesh app's virtual keyboard flicker by itself.
// For I/O decisions, compare by hand.
//
// Comparators WITH MEMORY. They store the value and return true when it changes -
// and ALWAYS true on a full pass, which is what makes the app repaint everything without
// having to remember to.
bool pintura_mudou_i(const pintura_t* p, int* mem, int agora);
bool pintura_mudou_s(const pintura_t* p, char* mem, size_t tam, const char* agora);
