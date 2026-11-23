#include "notificacao.h"
#include "tab5_mutex.hpp"
#include "app.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "notif";

typedef struct {
  const char*   nome;
  const char*   app;                    // see notif_associar_app()
  int           quantidade;
  notif_nivel_t nivel;
  char          resumo[NOTIF_RESUMO];
  bool          usada;
  bool          silenciada;             // see notif_silenciar()
  int           quantidade_no_ultimo_beep;  // see notif_houve_novidade_sonora()
  uint32_t      mudou_em;               // s_versao of the last change (tie-breaker for the highlight)
} fonte_t;

static fonte_t          s_fontes[NOTIF_MAX_FONTES];
static int              s_n = 0;
static uint32_t         s_versao = 0;
static Trava s_trava;

static inline void travar(void)    { s_trava.travar(); }
static inline void destravar(void) { s_trava.destravar(); }

int notif_registrar(const char* nome)
{
  if (!nome) { nome = "?"; }
  travar();
  int id = -1;
  // Same name = same source: the slot comes back, zeroed (see the .hpp).
  for (int i = 0; i < s_n; ++i) {
    if (s_fontes[i].usada && strcmp(s_fontes[i].nome, nome) == 0) { id = i; break; }
  }
  if (id >= 0) {
    // THE SLOT COMES BACK WITH ITS STATE (see the .h). Zeroing here was the Mesh
    // notice vanishing every time the service started again by another route.
    destravar();
    return id;
  }
  if (s_n < NOTIF_MAX_FONTES) {
    id = s_n++;
    memset(&s_fontes[id], 0, sizeof(s_fontes[id]));
    s_fontes[id].nome  = nome;
    s_fontes[id].app   = nome;   // default: the app of the same name (notif_associar_app)
    s_fontes[id].usada = true;
  }
  destravar();

  if (id < 0) { ESP_LOGE(TAG, "no slot for '%s' (limit %d)", nome, NOTIF_MAX_FONTES); }
  else        { ESP_LOGI(TAG, "'%s' may send notifications (slot %d)", nome, id); }
  return id;
}

void notif_publicar(int id, int quantidade, notif_nivel_t nivel, const char* resumo)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  if (quantidade < 0) { quantidade = 0; }

  bool mudou = false;
  travar();
  fonte_t* f = &s_fontes[id];
  if (f->usada) {
    // The version only changes when something REALLY changed. Without this comparison, a
    // service that republishes the same state every second would make the status
    // bar repaint every second - the flicker back again, by a new route.
    mudou = (f->quantidade != quantidade)
         || (f->nivel != nivel)
         || (strncmp(f->resumo, resumo ? resumo : "", NOTIF_RESUMO) != 0);
    if (mudou) {
      f->quantidade = quantidade;
      f->nivel      = nivel;
      snprintf(f->resumo, sizeof(f->resumo), "%s", resumo ? resumo : "");
      ++s_versao;
      f->mudou_em = s_versao;
    }
  }
  destravar();

  // FIRST ACTIVE PRODUCER OF THE EVENT QUEUE - Stage 2 of docs/ARQUITETURA.md
  // . "mudou" (changed) is checked OUTSIDE the locked section on purpose:
  // sistema_acordar_interface() only puts an event on a different queue and never
  // blocks, but there is no reason to hold this module's mutex lent
  // to another (avoiding any lock ordering between the two, even though today
  // neither of them could cause a problem). Mesh and Trilha (the only two
  // services that call notif_publicar() today) get this for free, without
  // needing to know this queue exists.
  if (mudou) { sistema_acordar_interface(); }
}

void notif_silenciar(int id, bool silenciar)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  travar();
  s_fontes[id].silenciada = silenciar;
  destravar();
}

void notif_associar_app(int id, const char* app_nome)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  travar();
  if (s_fontes[id].app != app_nome) { s_fontes[id].app = app_nome; ++s_versao; }
  destravar();
}

uint32_t notif_versao(void)
{
  travar();
  const uint32_t v = s_versao;
  destravar();
  return v;
}

bool notif_houve_novidade_sonora(void)
{
  travar();
  bool tocar = false;
  for (int i = 0; i < s_n; ++i) {
    fonte_t* f = &s_fontes[i];
    if (!f->usada) { continue; }
    // ALWAYS compares against the last value seen by THIS function - not the
    // aggregated total (that lived in kernel/core.cpp, and there was no
    // way to tell which source had changed). Updates the "last seen"
    // of every source in use, muted or not: that way, if the user turns the
    // sound back on for a source that went up while muted, it does NOT count as
    // retroactive news - only what goes up FROM HERE ON plays.
    if (f->quantidade > f->quantidade_no_ultimo_beep && !f->silenciada) {
      tocar = true;
    }
    f->quantidade_no_ultimo_beep = f->quantidade;
  }
  destravar();
  return tocar;
}

// Does 'a' come before 'b' in the bar? Most severe first; on a tie, the most recent.
static bool antes(const fonte_t* a, const fonte_t* b)
{
  if (a->nivel != b->nivel) { return a->nivel > b->nivel; }
  return a->mudou_em > b->mudou_em;
}

static void copiar(notif_item_t* saida, const fonte_t* f)
{
  saida->nome       = f->nome;
  saida->app        = f->app;
  saida->quantidade = f->quantidade;
  saida->nivel      = f->nivel;
  snprintf(saida->resumo, sizeof(saida->resumo), "%s", f->resumo);
}

int notif_listar(notif_item_t* saida, int max)
{
  if (!saida || max <= 0) { return 0; }
  int ordem[NOTIF_MAX_FONTES];
  int n = 0;
  travar();
  for (int k = 0; k < s_n; ++k) {
    const fonte_t* f = &s_fontes[k];
    if (!f->usada || f->quantidade <= 0) { continue; }
    // Sorted insertion: eight sources at most, not worth a qsort.
    int j = n++;
    while (j > 0 && antes(f, &s_fontes[ordem[j - 1]])) { ordem[j] = ordem[j - 1]; --j; }
    ordem[j] = k;
  }
  if (n > max) { n = max; }
  for (int i = 0; i < n; ++i) { copiar(&saida[i], &s_fontes[ordem[i]]); }
  destravar();
  return n;
}
