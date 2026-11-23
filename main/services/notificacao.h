// NOTIFICATION - a system service, not an app patch.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// The status bar called tab5_mesh_nao_lidas() directly. It worked, and
// it was a debt: the SYSTEM knew a SERVICE by name. The second app that
// needed to announce something - a battery alarm, a finished download,
// a GPS that lost its fix - would have had to be stitched into the same place, with the same
// if. By the fifth, the status bar would be a list of special cases and touching
// it would mean touching five modules.
//
// Here the relationship is inverted: whoever has news ANNOUNCES it, and the system only asks
// "is there anything?" without knowing from whom. tab5_mesh disappears from sistema.cpp.
//
// -------------------------------------------------------------------------
// THE MODEL: ONE SOURCE, ONE STATE
// -------------------------------------------------------------------------
// Each service (or app) has ONE registered source and rewrites its state
// whenever it likes. Usage guide for publishers: docs/ESCREVENDO_APPS.md, section 8.2.
// It isn't an event queue, it is CURRENT STATE - "I have 3 messages, one of them
// direct". That is on purpose:
//
//   - a queue grows without limit when nobody consumes it; state doesn't;
//   - announcing "3 messages" is more useful than three "1 message" notices;
//   - the producer knows how to aggregate better than the displayer.
//
// -------------------------------------------------------------------------
// CONCURRENCY
// -------------------------------------------------------------------------
// notif_publicar() can be called from ANY task - that is the point, since whoever has
// news is the service and not the UI. Reading happens in the UI
// task. The module protects itself.
//
// What it does NOT do: draw. Notification doesn't know what a screen is.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define NOTIF_MAX_FONTES  12
#define NOTIF_RESUMO      32

// Severity. The bar paints by the WORST active source - red beats blue,
// because something addressed to you is not the same thing as passing traffic.
typedef enum {
  NOTIF_ESTADO = 0,   // ongoing activity ("recording track") - only shows if nothing else has anything to say
  NOTIF_INFO,         // something happening, nothing requires you
  NOTIF_ATENCAO,      // something addressed to you, or that will get in the way soon
  NOTIF_URGENTE,      // requires action: battery running out, SD card full
} notif_nivel_t;

// Registers a source and returns the identifier, or -1 if there is no slot.
// IDEMPOTENT BY NAME: registering again a name that already exists
// returns the SAME slot, WITH THE STATE IT HAD. (The slot used to come back
// zeroed - and Mesh, which registers again every time the service starts
// by another route, erased its own notice without anyone having read anything. Whoever
// wants to clear it publishes 0; registration doesn't decide that for them.) Mesh registers every time the service starts - and
// it starts again on every Grove probe. Before, each start took a
// new slot; by the 8th there were no slots left, s_notif became -1 and every message
// from then on arrived with no notice at all, until the device was restarted.
//
// 'nome' (name) must outlive the registration (literal or static): we keep the
// pointer, not a copy. It is the same contract as app_t.nome.
int notif_registrar(const char* nome);

// Publishes the source's CURRENT state. quantidade (count) 0 clears the notice.
// 'resumo' (summary) may be nullptr when there is no useful text.
void notif_publicar(int id, int quantidade, notif_nivel_t nivel, const char* resumo);

// WHICH APP OPENS when the notice is tapped in the notification centre (kernel/shell).
// Without an association, the app with the SAME NAME as the source applies - the case of the
// background parts (fundo.h), whose name is already the app's. 'app_nome' follows the
// contract of 'nome': literal or static. nullptr = the notice opens nothing.
void notif_associar_app(int id, const char* app_nome);

// Shortcut for "I have nothing more to say".
static inline void notif_limpar(int id) { notif_publicar(id, 0, NOTIF_INFO, nullptr); }

// Turns this source's SOUND ALERT on/off, and ONLY this one's
// . Does not affect
// count/level/summary: the source keeps publishing and appearing in the bar
// and in the active list exactly the same, it just stops counting for the beep (see
// notif_houve_novidade_sonora() below). Whoever stores the preference across
// restarts is the CALLER (see services/tab5_trilha.cpp and ajustes.h) - this module only
// keeps the state IN RAM, the same way it keeps count/level.
void notif_silenciar(int id, bool silenciar);

// ---- reading, for whoever draws -------------------------------------------

// Changes on every publication that alters something. The bar compares it with what it
// last drew - that is what avoids repainting needlessly and, with it, what ensures
// news appears immediately and not at the next 1% of battery.
uint32_t notif_versao(void);

typedef struct {
  const char*   nome;
  const char*   app;          // name of the app the notice opens (nullptr = none)
  int           quantidade;
  notif_nivel_t nivel;
  char          resumo[NOTIF_RESUMO];
} notif_item_t;

// ALL ACTIVE ONES, in the order the bar shows them: the highlight first
// (most severe; on a tie, the most recent) and the rest by the same rule. Returns
// how many it copied (up to 'max'). It is what the notification centre and the lock
// screen list - with more than one app notifying, a single highlight and a "+N"
// hid precisely what the user wanted to know: FROM WHOM.
int notif_listar(notif_item_t* saida, int max);

// ---- reading, for whoever plays sound -------------------------------------------

// True if any NON-MUTED source went up in count since the
// LAST call to this specific function - it is the ONLY question whoever plays
// the beep (kernel/core.cpp) needs to ask; it doesn't know, and needn't know,
// how many sources exist or their names. Always call at the SAME time
// step (today, every BARRA_MS) - that is what defines "since last time".
bool notif_houve_novidade_sonora(void);
