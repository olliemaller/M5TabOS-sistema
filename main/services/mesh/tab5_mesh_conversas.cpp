// Mesh service - the CONVERSATIONS: the history on the card, the deleted ones, saving
// messages and the column order. Split out of tab5_mesh.cpp (the service); what the
// parts share lives in mesh/tab5_mesh_interno.hpp.

#include <string.h>
#include "idioma.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "notificacao.h"
#include <dirent.h>
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

static int      visivel_de(int interno);

// UNREAD COUNTS THAT SURVIVE THE SERVICE. Stopping the service (route change, Wi-Fi
// dropping in internet mode, Bluetooth dropping, C6L found on the Grove) clears
// s_conv[] - and the unread counts with it. Seen live: the bar notice VANISHED
// without anyone having opened Mesh, just because the service came up again via another
// route. This keeps each conversation's count until it is really READ;
// achar_ou_criar_conversa() returns the number when the conversation is reborn.
typedef struct { bool usada; bool canal; uint8_t idx; uint32_t no; uint16_t n; } nao_lida_guardada_t;
static nao_lida_guardada_t s_guardadas[MESH_MAX_CONVERSAS];

static nao_lida_guardada_t* guardada_de(bool canal, uint8_t idx, uint32_t no, bool criar)
{
  nao_lida_guardada_t* livre = nullptr;
  for (auto& g : s_guardadas) {
    if (!g.usada) { if (!livre) { livre = &g; } continue; }
    if (canal && g.canal && g.idx == idx) { return &g; }
    if (!canal && !g.canal && g.no == no) { return &g; }
  }
  if (!criar || !livre) { return nullptr; }
  *livre = { true, canal, idx, no, 0 };
  return livre;
}

// With the lock held (or with the service stopped). Snapshots the live conversations.
void guardar_nao_lidas(void)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    const mesh_conversa_t& c = s_conv[i];
    if (!c.usada) { continue; }
    nao_lida_guardada_t* g = guardada_de(c.canal, c.idx, c.no, c.nao_lidas > 0);
    if (!g) { continue; }
    if (c.nao_lidas > 0) { g->n = (uint16_t)(c.nao_lidas > 0xFFFF ? 0xFFFF : c.nao_lidas); }
    else                 { g->usada = false; }
  }
}

bool ha_nao_lidas_guardadas(void)
{
  for (const auto& g : s_guardadas) { if (g.usada && g.n) { return true; } }
  return false;
}

// Outcomes read from the card, ONCE per history load.
//
// The first version called a "look up this id in the file" function from INSIDE
// the loop that reads the messages. With 40 messages in the conversation that became 40
// opens and 40 full scans of envios.txt, on the card, with two
// files open at the same time and the user waiting for the conversation to appear.
// O(messages x outcomes) cost to answer a question that fits in 1KB
// of RAM.
//
// Now the file is read once into this table and the lookup is in memory.
#define MESH_DESFECHOS 64

typedef struct {
  uint32_t id;
  uint8_t  estado;
  uint8_t  erro;
  uint32_t quando;   // corrected timestamp ("H"); 0 = the message line's own applies
} desfecho_t;

// LAST ONE WINS. The file only grows, so an id's most recent line is the
// current truth - a packet can go from SEM_RESPOSTA to ENTREGUE when an ACK
// arrives late. That is why we overwrite the entry instead of stopping at the first.
//
// Table full: discards the OLDEST (ring). The file is chronological, so
// what remains are the 64 most recent outcomes - and the history only shows the
// last few dozen messages anyway.
static int carregar_desfechos(desfecho_t* tab, int max)
{
  FILE* f = tab5_sd_abrir(MESH_PASTA, MESH_ARQ_ENVIOS, "r");
  if (!f) { return 0; }

  int n = 0, prox = 0;
  char linha[64];
  int voltas = 0;
  while (fgets(linha, sizeof(linha), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // do not hog the core
    unsigned long lid = 0, lq = 0; unsigned est = 0, err = 0;
    const bool eh_hora = (sscanf(linha, "H,%8lX,%lu", &lid, &lq) == 2);
    if (!eh_hora && sscanf(linha, "E,%8lX,%u,%u", &lid, &est, &err) != 3) { continue; }

    int alvo = -1;
    for (int i = 0; i < n; ++i) { if (tab[i].id == (uint32_t)lid) { alvo = i; break; } }
    if (alvo < 0) {
      if (n < max) { alvo = n++; }
      else         { alvo = prox; prox = (prox + 1) % max; }
      tab[alvo].estado = MESH_ENVIO_NA;
      tab[alvo].erro   = 0;
      tab[alvo].quando = 0;
    }
    tab[alvo].id = (uint32_t)lid;
    if (eh_hora) { tab[alvo].quando = (uint32_t)lq; continue; }
    tab[alvo].estado = (uint8_t)est;
    tab[alvo].erro   = (uint8_t)err;
  }
  fclose(f);
  return n;
}

static uint32_t buscar_carimbo(const desfecho_t* tab, int n, uint32_t id)
{
  if (id == 0) { return 0; }
  for (int i = 0; i < n; ++i) { if (tab[i].id == id) { return tab[i].quando; } }
  return 0;
}

static uint8_t buscar_desfecho(const desfecho_t* tab, int n, uint32_t id, uint8_t* erro)
{
  if (id == 0) { return MESH_ENVIO_NA; }
  for (int i = 0; i < n; ++i) {
    if (tab[i].id != id) { continue; }
    if (erro) { *erro = tab[i].erro; }
    return tab[i].estado;
  }
  return MESH_ENVIO_NA;
}

int tab5_mesh_num_msgs(void) { return s_n_msgs; }

// NEVER SATURATES - see the big note next to s_total_msgs. It exists ONLY for
// whoever needs to know "has a message arrived (or gone out) since I last
// looked" (direct comparison, not pintura_mudou_*): tab5_mesh_num_msgs()
// looks as if it would do for this but does not, it sticks at MESH_MAX_MSGS (8)
// forever.
uint32_t tab5_mesh_total_msgs(void) { return s_total_msgs; }

bool tab5_mesh_msg(int i, mesh_msg_t* saida)
{
  if (i < 0 || i >= s_n_msgs) { return false; }
  travar();
  *saida = s_msgs[i];
  destravar();
  // Derived in the copy, like the nodes' idade_s: the state changes all the time and
  // storing it in the message would make the copy go stale between reading and use.
  saida->envio = tab5_mesh_estado_envio(saida->id, &saida->erro);
  return true;
}

// ---------------------------------------------------------------------------
// DELETED CONVERSATIONS - THE MISSING FLAG.
// ---------------------------------------------------------------------------
// THE BUG: deleting a conversation (tab5_mesh_esquecer_conversa) only ever touched
// RAM, on purpose - "the card is not touched" was the rule, so the history would
// survive in case you started talking again. But tab5_mesh_carregar_do_
// cartao() REREADS THE WHOLE FOLDER every time it runs (opening the app, waking from
// idle), and the no-XXXXXXXX.txt file was still there - so the "deleted"
// conversation came back by itself on the next scan. It was not the message
// reappearing, it was the FOLDER being read afresh again without remembering the decision.
//
// THE FIX IS THE FLAG THAT WAS ASKED FOR: a small file, apagadas.txt, one
// line per hidden node number. tab5_mesh_carregar_do_cartao() stops
// recreating the conversation of anyone on this list - the history stays on the
// card (deleting only the RECORD, not the past, is still the rule), but
// now it is invisible on purpose, not by accident.
//
// A NODE LEAVES THE LIST BY ITSELF when you really start talking again - a deliberate
// tap on "conversar com este no" (tab5_mesh_conversa_com) or a new message
// arriving (guardar_msg). Rereading the card does NOT count as "talking
// again"; only new content counts. A CHANNEL leaves the same way - a tap on "conversar com
// este canal" (tab5_mesh_conversa_do_canal) or a new message on it.
//
// CHANNELS NOW COME IN HERE TOO - until then only a direct node could be
// deleted ("canal nao se apaga, e devolver false e mais honesto que fingir",
// said the old comment in tab5_mesh_esquecer_conversa). The request to
// offer "available channels" to reopen later needed exactly the
// same flag that already existed for nodes - it just had to learn to store WHICH
// KIND of key it is, since a channel index (0..7) and a node number (always
// a large value, derived from the MAC) must not be confused in the SAME field.
typedef struct { bool canal; uint32_t chave; } apagado_t;
#define MESH_ARQ_APAGADAS "apagadas.txt"

#define MESH_MAX_APAGADOS 64   // hidden conversations; nothing to do with the node table
static apagado_t s_apagados[MESH_MAX_APAGADOS];
static int       s_n_apagados = 0;
static bool      s_apagados_carregado = false;

// FILE FORMAT: one line per entry.
//   direct node: "%08lX"   (as it always was - BACKWARDS COMPATIBLE on purpose,
//               no apagadas.txt file already written needs to change)
//   channel:    "K%u"     ('K' is never a hexadecimal digit (0-9A-F), so it
//               can never be confused with a legitimate node number read from the
//               old format)
static void carregar_apagados(void)
{
  // No card does NOT count as loaded: the RAM list must not become the
  // file (desmarcar_apagada rewrites it). Try again on next use.
  if (!tab5_sd_montar()) { return; }
  s_apagados_carregado = true;   // with the card: one read per session, even if the file is missing
  // THE FILE IS THE TRUTH: whatever was marked before this read is already in it
  // (tab5_sd_abrir applies the journal and the pending items before opening).
  s_n_apagados = 0;
  FILE* f = tab5_sd_abrir(MESH_PASTA, MESH_ARQ_APAGADAS, "r");
  if (!f) { tab5_sd_desmontar(); return; }

  char linha[16];
  while (fgets(linha, sizeof(linha), f) && s_n_apagados < MESH_MAX_APAGADOS) {
    if (linha[0] == 'K') {
      const uint32_t idx = (uint32_t)strtoul(linha + 1, nullptr, 10);
      if (idx < MESH_MAX_CANAIS) {
        s_apagados[s_n_apagados].canal = true;
        s_apagados[s_n_apagados].chave = idx;
        ++s_n_apagados;
      }
      continue;
    }
    const uint32_t num = (uint32_t)strtoul(linha, nullptr, 16);
    if (num != 0) {
      s_apagados[s_n_apagados].canal = false;
      s_apagados[s_n_apagados].chave = num;
      ++s_n_apagados;
    }
  }
  fclose(f);
  tab5_sd_desmontar();
  ESP_LOGI(TAG, "card: %d deleted conversation(s) - kept hidden", s_n_apagados);
}

// NO LOCK: called from inside it.
bool esta_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].canal == canal && s_apagados[i].chave == chave) { return true; }
  }
  return false;
}

// Marks a conversation as deleted: RAM first (applies at once, even without a
// card), then writes the line - the extra line in the file, without rewriting
// everything, is the same trick as nos.txt.
static void marcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (esta_apagada(canal, chave)) { return; }
  if (s_n_apagados < MESH_MAX_APAGADOS) {
    s_apagados[s_n_apagados].canal = canal;
    s_apagados[s_n_apagados].chave = chave;
    ++s_n_apagados;
  }

  char linha[16];
  if (canal) { snprintf(linha, sizeof(linha), "K%u\n", (unsigned)chave); }
  else       { snprintf(linha, sizeof(linha), "%08lX\n", (unsigned long)chave); }
  tab5_sd_anexar(MESH_PASTA, MESH_ARQ_APAGADAS, linha);
}

// Takes a conversation off the deleted list - called when you really start
// talking again (see the big comment above). Rewrites the whole
// file: it is rare (only happens when renewing something you had deleted), and the
// list is small.
void desmarcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (!s_apagados_carregado) { return; }   // without the list from the card the file is not rewritten
  int achou = -1;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].canal == canal && s_apagados[i].chave == chave) { achou = i; break; }
  }
  if (achou < 0) { return; }   // it was not deleted - nothing to do, nothing to rewrite

  for (int i = achou; i < s_n_apagados - 1; ++i) { s_apagados[i] = s_apagados[i+1]; }
  --s_n_apagados;

  // Rewrites from scratch through the system door: replace first writes the
  // "deleted" lines that were still waiting to go in, and with the card
  // lent to the computer it goes to the journal.
  char txt[MESH_MAX_APAGADOS * 12 + 1];
  size_t k = 0;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].canal) { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "K%u\n", (unsigned)s_apagados[i].chave); }
    else                     { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "%08lX\n", (unsigned long)s_apagados[i].chave); }
  }
  tab5_sd_substituir(MESH_PASTA, MESH_ARQ_APAGADAS, txt, k);
}

// ---------------------------------------------------------------------------
// Conversations and history on the card
// ---------------------------------------------------------------------------
// A conversation's file name. Short and without accents on purpose: it is a
// FAT file name, and the card has already taught us that 8.3 bites (see the case of
// satelites.csv that would not open for lack of LFN).
static void arquivo_da_conversa(const mesh_conversa_t* c, char* out, size_t tam)
{
  if (c->canal) { snprintf(out, tam, "canal-%u.txt", (unsigned)c->idx); }
  else          { snprintf(out, tam, "no-%08lX.txt", (unsigned long)c->no); }
}

int achar_ou_criar_conversa(bool canal, uint8_t idx, uint32_t no)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    if (canal && s_conv[i].canal && s_conv[i].idx == idx) { return i; }
    if (!canal && !s_conv[i].canal && s_conv[i].no == no) { return i; }
  }
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada) { continue; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));
    s_conv_seq[i] = 0;
    ++s_conv_ver;   // the column order changed
    s_conv[i].usada = true;
    s_conv[i].canal = canal;
    s_conv[i].idx = idx;
    s_conv[i].no  = no;
    if (const nao_lida_guardada_t* g = guardada_de(canal, idx, no, false)) { s_conv[i].nao_lidas = g->n; }
    if (canal) {
      // The REAL name, if the radio has already told us (FromRadio field 10). The
      // label made up below is only what is left while the configuration has not arrived.
      if (idx < MESH_MAX_CANAIS && s_canal_nome[idx][0]) {
        snprintf(s_conv[i].nome, sizeof(s_conv[i].nome), "%s", s_canal_nome[idx]);
      } else {
        snprintf(s_conv[i].nome, sizeof(s_conv[i].nome),
                 idx == 0 ? "LongFast" : "Canal %u", (unsigned)idx);
      }
    } else {
      // Node name - short, long, and the id only as a last resort (nome_exibicao).
      nome_exibicao(no, s_conv[i].nome, sizeof(s_conv[i].nome));
    }
    return i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// SAVING MESSAGES
// ---------------------------------------------------------------------------
// Mesh had its OWN saving task and queue ("mesh_grava";
// pressing send froze the screen while mounting the card). The idea was
// right and became the SYSTEM's: tab5_sd_anexar (hal/tab5_sd.hpp) copies the line and
// returns at once, and the "cartao" task writes in batches. The message goes as
// URGENT: written in milliseconds, not in the 5 s batch.
// Appends to the conversation's file. ONE-line format, with the text
// LAST so that a comma inside the message does not break reading:
//   M,<quando>,<de>,<snr>,<rssi>,<texto>
//
// NO MORE I/O HERE - see the big note above, next to laco_gravar().
// This runs with s_trava held (called from inside guardar_msg()) and
// sometimes on the UI task - it has to be fast unconditionally.
void gravar_msg(int ci, const mesh_msg_t* m)
{
  if (ci < 0) { return; }
  char arquivo[32];
  arquivo_da_conversa(&s_conv[ci], arquivo, sizeof(arquivo));
  // "M4" and not "M3": the record gained the hops - a new tag
  // instead of an extra field, and the reader knows what to expect before reading.
  // LINE BREAKS IN THE TEXT (the conversation has lines): the record is ONE
  // line, so the text's \n goes as 0x1F (unit separator, which
  // nobody types) and becomes \n again on reading (tab5_mesh_historico_de).
  char txt[sizeof(((mesh_msg_t*)0)->texto)];
  size_t k = 0;
  for (const char* c = m->texto; *c && k + 1 < sizeof(txt); ++c) { txt[k++] = *c == '\n' ? '\x1f' : *c == '\r' ? ' ' : *c; }
  txt[k] = 0;
  char linha[sizeof(((mesh_msg_t*)0)->texto) + 96];
  snprintf(linha, sizeof(linha), "M4,%lu,%08lX,%.1f,%ld,%d,%u,%08lX,%s\n",
           (unsigned long)m->quando, (unsigned long)m->de,
           m->snr, (long)m->rssi, (int)m->via_mqtt, (unsigned)m->saltos,
           (unsigned long)m->id, txt);
  if (!tab5_sd_anexar(MESH_PASTA, arquivo, linha, nullptr, true)) {
    ESP_LOGW(TAG, "writer full - message was NOT saved to the card");
  }
}

// THE CONVERSATIONS ON THE CARD, without needing the radio.
//
// The history was always saved, but the LIST of conversations only existed in
// memory - and it was only populated by messages arriving. Without the C6L plugged in, the
// app opened saying "no conversations" with the card full of them. The file was
// there; nobody was reading the index.
//
// The folder is the index itself: each conversation is a file, and its name carries
// the key (canal-3.txt, no-A1B2C3D4.txt). No separate catalogue needed, which
// would be one more state to fall out of sync with what it describes.
//
// It ALSO works with the radio on air: old conversations now appear in the column
// from opening, instead of only when someone speaks again. Renaming is
// safe - the channel name arrives later in the configuration dump and overwrites
// the made-up label, because the file is named by INDEX and not by name.
int tab5_mesh_carregar_do_cartao(void)
{
  // Without a radio, the number comes from NVS. That is what lets the file know which
  // messages are yours.
  if (s_meu_num == 0) { s_meu_num = recordar_meu_numero(); }

  // Names BEFORE conversations: it is the cache that names them when the radio is not
  // plugged in. In reverse order, every conversation would be born named by its number.
  if (!s_nomes_lidos) { carregar_nomes(); }

  if (!tab5_sd_montar()) { return 0; }

  char pasta[128];
  if (!tab5_sd_pasta(MESH_PASTA, pasta, sizeof(pasta))) {
    tab5_sd_desmontar();
    return 0;
  }

  DIR* dir = opendir(pasta);
  if (!dir) { tab5_sd_desmontar(); return 0; }

  int achadas = 0;
  struct dirent* e;
  travar();
  recordar_canais();
  while ((e = readdir(dir)) != NULL) {
    const char* n = e->d_name;
    if (strcmp(n, MESH_ARQ_ENVIOS) == 0) { continue; }   // outcomes, not a conversation

    if (strncmp(n, "canal-", 6) == 0) {
      const unsigned long idx = strtoul(n + 6, nullptr, 10);
      // !esta_apagada(true, idx) ADDED - it was missing here, and
      // it was the SECOND cause (independent of the radio config) of a deleted
      // channel coming back by itself: this scan runs when opening the app and knew
      // nothing about "deleted", it only checked whether the file existed on the
      // card. The same logic "no-" already used, it had just never been
      // extended to channels.
      // Channel DISABLED on the node (configuration known, see
      // recordar_canais): the file stays on the card, the conversation does not come back.
      const bool desligado = s_canais_conhecidos && idx < MESH_MAX_CANAIS && !s_canal_ativo[idx];
      if (idx < MESH_MAX_CANAIS && !desligado && !esta_apagada(true, (uint8_t)idx)
       && achar_ou_criar_conversa(true, (uint8_t)idx, 0) >= 0) { ++achadas; }
    }
    else if (strncmp(n, "no-", 3) == 0) {
      // Hexadecimal, as it was written. Zero is not a node - it is what strtoul
      // returns when the name is no good, so it also works as a sieve.
      const uint32_t num = (uint32_t)strtoul(n + 3, nullptr, 16);
      // Deleted on purpose: do not recreate just because the file still exists on the
      // card (see the big comment in carregar_apagados, above). The
      // history stays saved - it just does not reappear by itself.
      if (num != 0 && !esta_apagada(false, num) && achar_ou_criar_conversa(false, 0, num) >= 0) { ++achadas; }
    }
  }
  destravar();
  closedir(dir);
  tab5_sd_desmontar();

  ESP_LOGI(TAG, "card: %d conversation(s) in the file", achadas);
  return achadas;
}

// STARTING TO TALK TO SOMEONE WHO HAS NOT YET TALKED TO US.
//
// Until now a direct conversation was only born when a message ARRIVED
// (achar_ou_criar_conversa on the receive path). In other words: you could
// reply, but never start a conversation. This function opens the other direction, from
// the node list.
//
// Returns the VISIBLE index - the same one the conversation column uses - and not the
// internal one, because the caller is the screen and that is the one it knows how to work with. The
// two diverge as soon as a conversation is freed in the middle of the array.
int tab5_mesh_conversa_com(uint32_t no)
{
  // Number zero is not a node; it is the filler of whoever does not know. And a conversation with
  // your own node does not exist in the protocol - the radio does not deliver to itself.
  if (no == 0) { return -1; }

  travar();
  if (no == s_meu_num) { destravar(); return -1; }

  const int interno = achar_ou_criar_conversa(false, 0, no);
  const int visivel = (interno >= 0) ? visivel_de(interno) : -1;
  destravar();

  // A deliberate tap on "conversar com este no": it is talking again that
  // undoes an earlier delete (see the big comment above the flag).
  if (interno >= 0) { desmarcar_apagada(false, no); }
  return visivel;
}

// Same role as tab5_mesh_conversa_com(), for a CHANNEL - created
// alongside the option to delete a channel, so "available channels" can reopen one
// that was hidden (see the big note on the deleted flag, and
// tab5_mesh_num_canais_disponiveis()/tab5_mesh_canal_disponivel() at the end
// of this file).
int tab5_mesh_conversa_do_canal(uint8_t idx)
{
  if (idx >= MESH_MAX_CANAIS) { return -1; }

  travar();
  const int interno = achar_ou_criar_conversa(true, idx, 0);
  const int visivel = (interno >= 0) ? visivel_de(interno) : -1;
  destravar();

  // A deliberate tap on "conversar com este canal": undoes an earlier
  // delete, the same reasoning as conversa_com() for a node.
  if (interno >= 0) { desmarcar_apagada(true, idx); }
  return visivel;
}

// ---------------------------------------------------------------------------
// THE COLUMN ORDER
// ---------------------------------------------------------------------------
// Channels first, by index; then the direct ones, most recent on top. The
// s_conv array is a set of slots, not an order: a new conversation
// takes the first free slot, which may come BEFORE the open conversation. With the
// visible index being "the n-th used slot", that shifted the open conversation
// by one place - the app ended up drawing one conversation's history and sending
// to another. Now the order is explicit and changes to it are announced
// (s_conv_ver), so the app can find its own again by key.
//
// NO LOCK: called from inside it. Straight insertion: there are at most 12.
static bool vem_antes(int a, int b)
{
  const mesh_conversa_t& x = s_conv[a];
  const mesh_conversa_t& y = s_conv[b];
  if (x.canal != y.canal) { return x.canal; }
  if (x.canal)            { return x.idx < y.idx; }
  if (s_conv_seq[a] != s_conv_seq[b]) { return s_conv_seq[a] > s_conv_seq[b]; }
  return a < b;
}

static int ordem_conversas(int out[MESH_MAX_CONVERSAS])
{
  int n = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    int j = n++;
    while (j > 0 && vem_antes(i, out[j - 1])) { out[j] = out[j - 1]; --j; }
    out[j] = i;
  }
  return n;
}

static int interno_de(int visivel)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = ordem_conversas(o);
  return (visivel >= 0 && visivel < n) ? o[visivel] : -1;
}

static int visivel_de(int interno)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = ordem_conversas(o);
  for (int k = 0; k < n; ++k) { if (o[k] == interno) { return k; } }
  return -1;
}

uint32_t tab5_mesh_conversas_versao(void) { return s_conv_ver; }

int tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no)
{
  int v = -1;
  travar();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada || s_conv[i].canal != canal) { continue; }
    if (canal ? (s_conv[i].idx == idx) : (s_conv[i].no == no)) { v = visivel_de(i); break; }
  }
  destravar();
  return v;
}

// DELETING A CONVERSATION - CHANNELS TOO.
//
// It arose from a side effect of "tap the node to talk to it": just
// TAPPING made the conversation exist, and it stayed there empty forever. Creating without
// offering a way to undo is a one-way street.
//
// CHANNELS COULD NOT BE DELETED - "devolver false e mais honesto que
// fingir" (returning false is more honest than pretending), said the old comment here, because there was no flag at all to
// stop the channel coming back by itself on the next message or the next
// config. Now there is (the same "apagados" that already worked for direct, extended
// to store channel indices as well - see the big note next to it) and the
// two places that recreated the conversation by themselves (the card scan and the
// config parser, see the notes there) already check that flag. All that is left is "talking
// again on purpose" - tab5_mesh_conversa_do_canal() takes care of that.
//
// THE CARD IS NOT TOUCHED, on purpose. What gets deleted is the in-memory record,
// not the history. If you start talking again, the conversation is reborn with what
// had already been said - which is what you expect from a list, not from a
// purge. An empty conversation, which is the case that motivated this, has nothing on the
// card anyway.
bool tab5_mesh_esquecer_conversa(int idx)
{
  bool ok = false, canal_apagado = false;
  uint32_t chave_apagada = 0;
  travar();
  const int i = interno_de(idx);
  if (i >= 0) {
    // Its unread ones leave the total by themselves: publicar_notif() recounts.
    // Kept before the memset wipes the fields themselves.
    canal_apagado = s_conv[i].canal;
    chave_apagada = canal_apagado ? s_conv[i].idx : s_conv[i].no;
    if (nao_lida_guardada_t* g = guardada_de(s_conv[i].canal, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));   // usada goes back to false
    s_conv_seq[i] = 0;
    ++s_conv_ver;
    ok = true;
  }
  if (ok) { publicar_notif(); }
  destravar();

  // Outside the lock: it is card I/O (fopen/fclose), it must not hold the mutex
  // that the rest of the service (receive, send) also contends for. Channel 0 is a valid
  // key (LongFast is channel 0), so only direct has "zero = invalid".
  if (ok && (canal_apagado || chave_apagada != 0)) {
    marcar_apagada(canal_apagado, chave_apagada);
  }
  return ok;
}

int tab5_mesh_num_conversas(void)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) { if (s_conv[i].usada) { ++n; } }
  destravar();
  return n;
}

bool tab5_mesh_conversa(int idx, mesh_conversa_t* saida)
{
  bool ok = false;
  travar();
  const int i = interno_de(idx);
  if (i >= 0) {
    *saida = s_conv[i];
    // The second line, derived now (see mesh_conversa_t.sub): the long
    // name of whoever the direct conversation is with, if it says something the
    // first line's name has not said yet.
    saida->sub[0] = 0;
    if (saida->canal) {
      snprintf(saida->sub, sizeof(saida->sub), saida->idx == 0 ? tr("canal primário") : tr("canal %u"), (unsigned)saida->idx);
    } else {
      const char* longo = nullptr;
      for (int k = 0; k < MESH_MAX_NOS; ++k) {
        if (s_nos[k].usado && s_nos[k].num == saida->no) { if (s_nos[k].longo[0]) { longo = s_nos[k].longo; } break; }
      }
      nome_legivel(longo ? longo : longo_conhecido(saida->no), saida->sub, sizeof(saida->sub));
      if (!strcmp(saida->sub, saida->nome)) { saida->sub[0] = 0; }
    }
    ok = true;
  }
  destravar();
  return ok;
}

// A single place to publish, called from every point that touches the
// counters. Spreading publication across the change points would repeat the
// severity rule in five places - and forget it in one of them.
//
// NO LOCK: the caller is already inside it. Taking it again would lock the mutex
// against itself (it is not recursive).
void publicar_notif(void)
{
  // THE TOTAL IS DERIVED, NEVER KEPT SEPARATELY. Before there were three
  // counters adjusted by arithmetic in four places (+1 on arrival, -N on
  // read/delete, reset on opening the app) - and resetting the total on opening the app WITHOUT
  // resetting the conversations put them out of step: from then on, reading an old conversation
  // subtracted more than the total had, the floor at zero swallowed the difference and
  // a NEW message from another conversation vanished from the bar. Adding up the conversations
  // here, the bar and each conversation's dots are the same truth.
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    s_nao_lidas += s_conv[i].nao_lidas;
    if (!s_conv[i].canal) { s_nao_lidas_dir += s_conv[i].nao_lidas; }
  }
  if (s_notif < 0) { return; }
  char resumo[NOTIF_RESUMO];
  if (s_nao_lidas_dir > 0) {
    snprintf(resumo, sizeof(resumo), "%d direta%s", s_nao_lidas_dir,
             s_nao_lidas_dir > 1 ? "s" : "");
  } else {
    // WHICH CHANNEL, and not just "how many messages" - "1 on the channel" said nothing
    // when there is more than one channel in the list (LongFast, BR...). Knowing the NAME
    // of the only channel with news, the status bar already says where to look without
    // needing to open the app just to find out.
    //
    // ALWAYS CALLED WITH THE MUTEX ALREADY TAKEN by the caller (see the four
    // places that call publicar_notif) - that is why it reads s_conv[] directly, without
    // travar() again (it would unlock the same place twice - deadlock).
    const char* nome_unico = nullptr;
    int canais_com_novidade = 0;
    for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
      if (s_conv[i].usada && s_conv[i].canal && s_conv[i].nao_lidas > 0) {
        ++canais_com_novidade;
        nome_unico = s_conv[i].nome;
      }
    }
    if (canais_com_novidade == 1 && nome_unico && nome_unico[0]) {
      snprintf(resumo, sizeof(resumo), "%d em %s", s_nao_lidas, nome_unico);
    } else if (canais_com_novidade > 1) {
      snprintf(resumo, sizeof(resumo), "%d em %d canais", s_nao_lidas, canais_com_novidade);
    } else {
      // Safety net: s_nao_lidas>0 with no channel marked should not
      // happen, but without this the bar would go silent instead of generic.
      snprintf(resumo, sizeof(resumo), tr("%d no canal"), s_nao_lidas);
    }
  }
  // Direct is ATENCAO: it is addressed to you. Channel traffic is INFO - worth
  // knowing, requires nothing.
  notif_publicar(s_notif, s_nao_lidas,
                 s_nao_lidas_dir > 0 ? NOTIF_ATENCAO : NOTIF_INFO, resumo);
}

void tab5_mesh_marcar_conversa_lida(int idx)
{
  travar();
  const int i = interno_de(idx);
  // The total and the direct ones come out by themselves: publicar_notif() recounts.
  if (i >= 0) {
    s_conv[i].nao_lidas = 0;
    // Really read: the stored one goes too, so it does not come back on the next restart.
    if (nao_lida_guardada_t* g = guardada_de(s_conv[i].canal, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
  }
  publicar_notif();
  destravar();
}

// Reads the LAST 'max' messages from the file. Reads the whole file keeping a
// circular window instead of trying to seek to the end: lines are variable
// length, so there is no way to jump straight there - and the file is small.
int tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(conversa, &c)) { return 0; }
  return tab5_mesh_historico_de(c.canal, c.idx, c.no, saida, max);
}

int tab5_mesh_historico_de(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c = { };
  c.canal = canal; c.idx = idx; c.no = no;
  if (!tab5_sd_montar()) { return 0; }

  char nome[32];
  arquivo_da_conversa(&c, nome, sizeof(nome));

  // Uses the service again: what knows how to tell "does not exist yet" from "went
  // wrong" is tab5_sd, not each app. A conversation with no history returns 0
  // messages with no alarm in the log.
  FILE* f = tab5_sd_abrir(MESH_PASTA, nome, "r");
  if (!f) { tab5_sd_desmontar(); return 0; }
  // ONLY THE END OF THE FILE. Only the last 'max' messages come out of here, and a line
  // is at most sizeof(linha) bytes: the last max * sizeof(linha) bytes
  // are enough. Before, it read the WHOLE file on every opening - LongFast, with
  // MQTT traffic, piled up megabytes and the conversation took seconds to
  // open. 16 KB buffer in PSRAM: stdio's default reads in tiny pieces.
  char* vbuf = (char*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
  if (vbuf) { setvbuf(f, vbuf, _IOFBF, 16 * 1024); }
  {
    const long cauda = (long)max * (long)(MESH_TEXTO + 80);
    long tam = -1;
    if (fseek(f, 0, SEEK_END) == 0) { tam = ftell(f); }
    if (tam > cauda && fseek(f, tam - cauda, SEEK_SET) == 0) {
      int ch;
      while ((ch = fgetc(f)) != EOF && ch != '\n') { }   // the line cut in the middle
    } else {
      fseek(f, 0, SEEK_SET);
    }
  }

  // Outcomes loaded ONCE, before the loop. See the table's comment.
  static desfecho_t desf[MESH_DESFECHOS];
  const int n_desf = carregar_desfechos(desf, MESH_DESFECHOS);

  int n = 0, primeiro = 0, voltas = 0;
  // IT MUST FIT THE PREFIX (~60 bytes: "M4,quando,de,snr,rssi,vmqtt,saltos,
  // id,") PLUS THE WHOLE TEXT (up to MESH_TEXTO-1=237 bytes, see the note there)
  // PLUS the line break - otherwise this function becomes the SAME kind of
  // truncation the live read had (MESH_TEXTO too small),
  // only when rereading from the card instead of on receipt.
  char linha[MESH_TEXTO + 80];
  while (fgets(linha, sizeof(linha), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // do not hog the core
    const bool fmt_m4 = (strncmp(linha, "M4,", 3) == 0);
    const bool fmt_m3 = !fmt_m4 && (strncmp(linha, "M3,", 3) == 0);
    const bool fmt_m2 = !fmt_m4 && !fmt_m3 && (strncmp(linha, "M2,", 3) == 0);
    if (!fmt_m4 && !fmt_m3 && !fmt_m2 && strncmp(linha, "M,", 2) != 0) { continue; }

    mesh_msg_t m; memset(&m, 0, sizeof(m));
    unsigned long quando = 0, de = 0, id = 0; float snr = 0; long rssi = 0;
    int vmqtt = 0;
    unsigned saltos = MESH_SALTOS_DESCONHECIDO;
    // %n returns where the text starts: everything from there to the end of the line is the message,
    // commas included.
    int pos = 0;
    if (fmt_m4) {
      if (sscanf(linha, "M4,%lu,%8lX,%f,%ld,%d,%u,%8lX,%n",
                 &quando, &de, &snr, &rssi, &vmqtt, &saltos, &id, &pos) < 7) { continue; }
    } else if (fmt_m3) {
      // M3 has via_mqtt but not hops (the field arrived just after, in the same batch
      // ) - it stays MESH_SALTOS_DESCONHECIDO, the sentinel for
      // "cannot be known", never a made-up number.
      if (sscanf(linha, "M3,%lu,%8lX,%f,%ld,%d,%8lX,%n",
                 &quando, &de, &snr, &rssi, &vmqtt, &id, &pos) < 6) { continue; }
    } else if (fmt_m2) {
      // M2 has neither via_mqtt nor hops - via_mqtt stays false, which is the
      // safest assumption: an old message very probably came over the
      // radio, and a wrong value here only swaps an icon, never loses
      // text or confuses delivery.
      if (sscanf(linha, "M2,%lu,%8lX,%f,%ld,%8lX,%n",
                 &quando, &de, &snr, &rssi, &id, &pos) < 5) { continue; }
    } else {
      // Old format, no id. The message appears the same; there is just no delivery
      // mark - and there really is none, because the state died with the restart.
      if (sscanf(linha, "M,%lu,%8lX,%f,%ld,%n",
                 &quando, &de, &snr, &rssi, &pos) < 4) { continue; }
    }
    m.quando = (uint32_t)quando; m.de = (uint32_t)de; m.snr = snr; m.rssi = (int32_t)rssi;
    m.via_mqtt = (vmqtt != 0);
    m.saltos = (uint8_t)saltos;
    m.id = (uint32_t)id;
    // RING FIRST, CARD AFTER. The ring has the live state, including the
    // ENVIANDO that never goes to the card; the card has the outcomes of all
    // the sends that have already left the ring or are from before the last restart.
    m.envio = tab5_mesh_estado_envio(m.id, &m.erro);
    if (m.envio == MESH_ENVIO_NA) { m.envio = buscar_desfecho(desf, n_desf, m.id, &m.erro); }
    const uint32_t corrigido = buscar_carimbo(desf, n_desf, m.id);
    if (corrigido) { m.quando = corrigido; }
    snprintf(m.texto, sizeof(m.texto), "%s", linha + pos);
    m.texto[strcspn(m.texto, "\r\n")] = 0;
    for (char* c = m.texto; *c; ++c) { if (*c == '\x1f') { *c = '\n'; } }   // the line break (see gravar_msg)

    saida[(primeiro + n) % max] = m;
    if (n < max) { ++n; } else { primeiro = (primeiro + 1) % max; }
  }
  fclose(f);
  heap_caps_free(vbuf);
  tab5_sd_desmontar();

  // Unrolls the window into chronological order.
  if (primeiro != 0 && n == max) {
    mesh_msg_t tmp[1];
    for (int i = 0; i < n; ++i) {
      // simple rotation: cheap for dozens of items
      tmp[0] = saida[0];
      for (int k = 0; k < n - 1; ++k) { saida[k] = saida[k+1]; }
      saida[n-1] = tmp[0];
      if (--primeiro == 0) { break; }
    }
  }
  return n;
}
