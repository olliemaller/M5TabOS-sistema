// BACKGROUND PART OF AN APP - work that carries on when the window closes
// .
//
// -------------------------------------------------------------------------
// THE PROBLEM
// -------------------------------------------------------------------------
// An app lives while it is on screen: ao_fechar() frees everything. But there is the app
// whose WORK is not the screen - recording the GPS track, sending the position over the
// internet, a long calculation. Trilha solved this by becoming a hand-made
// service (services/tab5_trilha, with its own task, notification and stop). The
// second app like that would have to repeat everything - and each would repeat it differently.
//
// -------------------------------------------------------------------------
// THE MODEL
// -------------------------------------------------------------------------
// The app splits into two halves:
//   - the WINDOW: the usual app (draws, takes touch, is born and dies with the screen);
//   - the BACKGROUND PART: a loop the KERNEL runs in a task of ITS OWN. The app only
//     describes it (fundo_def_t) and asks for it to be started/stopped. Closing the window does not
//     stop the background part.
//
// What the SYSTEM guarantees every background part, without the app writing anything:
//   - its OWN TASK, priority 1 (below the UI, the radios and the
//     worker) - the heaviest loop never stalls touch. The exception is whatever
//     has a real-time DEADLINE (sound): see fundo_def_t.prioridade;
//   - ALWAYS VISIBLE: while it runs, a NOTIF_ESTADO notification with its
//     name (grey pill in the bar). Nothing runs hidden from the user - a
//     short-job background part (aviso_ms) only appears if it runs past the deadline;
//   - POWER added: the declared profile enters the device's effective profile
//     (together with the on-screen app, or home) while it runs - WiFi requested
//     here stays up with any app on screen and when idle;
//   - CLEAN STOP: asking it to stop WAKES the loop (fundo_esperar returns
//     immediately) and the system waits for it to exit before calling ao_parar();
//   - WHO STOPS IT: the window, the console ("fundo parar <nome>") and THE USER, via the
//     card's X in the notification centre (tap the bar). See the contract below.
//
// -------------------------------------------------------------------------
// THE STOP CONTRACT - the user is in charge
// -------------------------------------------------------------------------
// Every background part appears in the notification centre, in the "Em segundo
// plano" (In the background) strip, with an X. The X is the SYSTEM's: no app writes anything to have it, and
// no app can refuse it. First tap: "Parar?" (Stop?); the second, within 3 s, stops it.
// Media (kernel/midia.h) gets the same X on the mini player, in place of the card.
//
// What the app guarantees in return:
//   - the loop exits soon after the request (fundo_esperar/fundo_ceder return
//     false; a long wait without them delays the stop);
//   - EVERYTHING that must survive is saved in ao_parar() (or before): it runs
//     on the UI after the loop has exited, always - including when it was the user
//     who stopped it;
//   - stopping midway corrupts nothing (file closed after each write, or
//     closed in ao_parar). The window may be closed and is not notified.
// And what the system guarantees: the request does not freeze the screen (the X stop requests
// and carries on; the kernel collects when the loop exits), and the pill shows
// "parando..." (stopping...) until then. Past 3 s without exiting: the slot is left "abandoned" and is
// cleaned up when the loop finally exits.
//
// A background part that can go IDLE (paused music) should end
// by itself after a while - the player exits after 5 min paused. Forgotten,
// it takes up one of the FUNDO_MAX slots and uses power without anyone seeing.
//
// -------------------------------------------------------------------------
// THE LOOP CONTRACT (body)
// -------------------------------------------------------------------------
//   static void meu_laco(fundo_t* f) {
//       while (fundo_esperar(f, 5000)) {       // sleeps 5 s; false = stop
//           ...work with BOUNDED waits...
//           fundo_estado(f, n, "42 envios");    // the pill text
//       }
//   }                                           // returned: the system cleans up
//
//   - NEVER wait forever (portMAX_DELAY): the stop request must be able
//     to get through. A real wait is fundo_esperar().
//   - A CALCULATION loop (waiting on nothing) calls fundo_ceder(f) on every turn:
//     without it the task takes the whole core, its IDLE never runs, the
//     watchdog fires and the device does not sleep. fundo_ceder only actually yields
//     every ~20 ms, so it can be called in a tight loop.
//   - The loop does NOT draw. What the window needs to show, the background part keeps in its
//     own (protected) state and the window reads a copy - same as a service.
//   - The background part's memory belongs to it: static or allocated in ao_iniciar() and
//     freed in ao_parar(). NEVER a window buffer - the window may close at
//     any moment.
//   - Returning from the loop by itself (work finished) is allowed: the system
//     notices and cleans up as with a requested stop.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_power.hpp"
#include "notificacao.h"

#define FUNDO_MAX 4   // background parts running at the same time

typedef struct fundo_s fundo_t;
typedef void (*fundo_corpo_t)(fundo_t* f);

typedef struct {
  const char*   nome;       // literal: task name (<=15), of the notification and in the lists
  fundo_corpo_t corpo;
  uint32_t      pilha;      // bytes; 0 = 4096
  tab5_perfil_t perfil;     // what must be ON while it runs (added to the effective profile)
  // Optional. They run on the requester's task (the UI, in practice).
  bool (*ao_iniciar)(void); // prepares the state; false = refuses to start
  void (*ao_parar)(void);   // after the loop has exited: close and free
  bool (*com_som)(void);    // notification beep preference; nullptr = with sound
  // THE PILL ONLY AFTER RUNNING FOR aviso_ms (0 = immediately). For short,
  // frequent background work (a Terminal command): what finishes sooner
  // does not flash in the bar; what takes longer appears, with the X, like any background part.
  uint32_t aviso_ms;
  // TASK PRIORITY (0 = the default, 1). Only for something with a DEADLINE and
  // SHORT work per turn: the Music app's sound. At 1 it lost its turn to the camera
  // capture (3) and the UI (4), which together occupy both cores, and the
  // speaker ran dry. Above the UI there is only room for a loop that does a
  // fraction of its time and SLEEPS (decodes a block and waits on the queue) - a
  // calculation loop here stalls touch.
  uint32_t prioridade;
} fundo_def_t;

// ---- for the window (and anyone) -------------------------------------------
bool sistema_fundo_iniciar(const fundo_def_t* d);   // false: already running, no slot or refused
bool sistema_fundo_parar(const fundo_def_t* d);     // WAITS for the loop to exit (up to 3 s)
bool sistema_fundo_ativo(const fundo_def_t* d);
// START FROM ANY TASK, without waiting (the web server running a
// Terminal command): the UI starts it on the next turn. If the same background part is still
// in a slot (exiting), the request waits for it to exit and starts it again - which is what the
// loop that "exits when the queue runs dry" needs. Requesting again what was already
// requested is the same request. From the UI, it starts immediately if the slot is free.
// false = no slot for the request (FUNDO_MAX requests already waiting).
bool sistema_fundo_pedir(const fundo_def_t* d);

typedef struct {
  const char* nome;
  uint32_t    ha_s;                  // running for how many seconds
  char        resumo[NOTIF_RESUMO];  // the latest fundo_estado()
} fundo_info_t;
int  sistema_fundo_listar(fundo_info_t* out, int max);
// Requests the stop by NAME, from any task, WITHOUT WAITING (the centre's X and
// the console use this): the kernel really stops it in its own loop.
bool sistema_fundo_parar_nome(const char* nome);

// ---- for the loop ----------------------------------------------------------
bool fundo_esperar(fundo_t* f, uint32_t ms);   // sleeps up to ms; false = asked to stop
bool fundo_ceder(fundo_t* f);                  // calculation loop; false = asked to stop
bool fundo_rodando(const fundo_t* f);
void fundo_estado(fundo_t* f, int quantidade, const char* resumo);

// ---- for the kernel --------------------------------------------------------
void fundo_somar_perfil(tab5_perfil_t& p);   // called by sistema_impor_preferencias()
void fundo_manter(void);                     // in the UI loop: cleans up whoever ended on their own,
                                             // reapplies the profile when the set changed
