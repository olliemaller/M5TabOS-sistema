// Mesh service - the NODES: the table and its order, favourites, search, the name
// cache and the Tab5's NodeDB on the SD card. Split out of tab5_mesh.cpp (the service);
// what the parts share lives in mesh/tab5_mesh_interno.hpp.

#include "app.h"   // sistema_avisar
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "ajustes.h"
#include "tab5_mesh_ble.hpp"
#include "tab5_mesh_diario.hpp"
#include "mesh_pb.hpp"
#include "tab5_hora.hpp"
#include "nvs.h"
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

// How many seconds ago the node was heard; -1 = no way to tell. Two sources,
// in this order:
//   1. our own sighting - exact, depends on no clock at all;
//   2. the radio's last_heard against the time borrowed from it.
static int32_t idade_do_no(const mesh_no_t& no)
{
  const uint32_t ep = tab5_mesh_epoch();
  if (no.visto_local_ms != 0) { return (int32_t)((tab5_agora_ms() - no.visto_local_ms) / 1000); }
  if (no.visto_em >= EPOCH_MINIMO && ep > no.visto_em) { return (int32_t)(ep - no.visto_em); }
  return -1;
}

// THE TABLE ORDER, the same as the web client: our node first, then from the
// most recently heard to the oldest, and those with no known age at the end.
// It used to be the order of the vector's slots - a node that had just spoken could
// be at the end of the list, and the list "shuffled" on every eviction. NO LOCK.
static uint32_t s_prox_fixado = 0;
static int      fav_pos(uint32_t num);   // favourites, further down

// FIXED PLACE given ON ARRIVAL of the data (packet or NodeInfo with a radio SNR), and
// not when the screen asks: it is what protects the node from eviction (achar_no),
// and eviction happens with the screen closed too. NO LOCK.
void fixar_se_pelo_ar(mesh_no_t* no)
{
  if (no && !no->fixado && no->num != s_meu_num && tab5_mesh_no_pelo_ar(no)) {
    no->fixado = ++s_prox_fixado;
  }
}

// CACHED ORDER. With 256 slots, sorting on every tab5_mesh_no(i) - once per
// drawn row - would become millions of comparisons per second. The order only
// changes when the table changes (s_nos_ver) or when time passes (the age of the
// lower band): redone at most once per second, or immediately if something
// arrived. With the lock held.
uint32_t s_nos_ver = 0;
EXT_RAM_BSS_ATTR static int s_ordem[MESH_MAX_NOS];
static int      s_ordem_n = 0;
static uint32_t s_ordem_ver = UINT32_MAX;
static uint32_t s_ordem_ms = 0;
static uint32_t s_ordem_geracao = 0;   // +1 on every sort: only then does the search redo the filter

static int ordem_nos(void)
{
  if (s_ordem_ver == s_nos_ver && (tab5_agora_ms() - s_ordem_ms) < 1000) { return s_ordem_n; }
  // CHANGED, BUT HAS JUST SORTED: wait 250 ms. With dense traffic the
  // version changes on every packet, and the nodes page asks for one row at a time -
  // that was several sorts of 256 nodes per draw. A quarter-second delay
  // in the order can't be seen; the processing can.
  if (s_ordem_ver != UINT32_MAX && (tab5_agora_ms() - s_ordem_ms) < 250) { return s_ordem_n; }
  s_ordem_ver = s_nos_ver;
  s_ordem_ms  = tab5_agora_ms();

  // Three bands:
  //   0 - our node;
  //   F - the FAVOURITES (star), in the order they were marked;
  //   1 - those heard OVER THE AIR, FIXED: in the order they first appeared with
  //       an SNR - they don't move down when someone else speaks;
  //   2 - the rest (MQTT, or no SNR), from most recently heard to the
  //       oldest. Whoever gains an SNR moves up to band 1 and stays.
  EXT_RAM_BSS_ATTR static int64_t chave[MESH_MAX_NOS];
  int n = 0;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    mesh_no_t& no = s_nos[i];
    if (!no.usado) { continue; }
    fixar_se_pelo_ar(&no);
    int64_t k;
    const int fp = fav_pos(no.num);
    if (no.num == s_meu_num) { k = -1; }
    else if (fp >= 0)        { k = fp; }                         // favourites, in the order they were marked
    else if (no.fixado)      { k = ((int64_t)1 << 20) + no.fixado; }
    else {
      int64_t idade = idade_do_no(no);
      if (idade < 0) { idade = INT32_MAX; }
      k = ((int64_t)1 << 40) + idade;
    }
    int j = n++;
    while (j > 0 && k < chave[j - 1]) { s_ordem[j] = s_ordem[j - 1]; chave[j] = chave[j - 1]; --j; }
    s_ordem[j] = i; chave[j] = k;
  }
  s_ordem_n = n;
  ++s_ordem_geracao;
  return n;
}

bool tab5_mesh_no_pelo_ar(const mesh_no_t* no)
{
  return !no->via_mqtt && no->snr > -128.0f;
}

// ---------------------------------------------------------------------------
// FAVOURITES - the star, the same as the node's and the web client's
// ---------------------------------------------------------------------------
// Meshtastic already has favourites: NodeInfo.is_favorite (field 10) in the dump, and
// AdminMessage set_favorite_node (39) / remove_favorite_node (40) to change them.
// Using its own, a star marked here shows up in the web client and on the
// node's screen, and the node also protects the favourite from eviction from its NodeDB.
//
// ALSO kept here (NVS), in the order they were marked - that is the order at the
// top of the table. Without the radio (internet mode) the local list stands alone; a
// change made like that stays PENDING and goes to the node on the next dump. With the
// radio, the dump is the truth: a star removed in the web client disappears here too.
#define MESH_NVS_FAVS  "favs"
uint32_t s_favs[MESH_MAX_FAVS];
int      s_n_favs = 0;
static bool     s_favs_lidos = false;
typedef struct { uint32_t num; bool quer; } fav_pend_t;
#define MESH_FAV_PEND 8
static fav_pend_t s_fav_pend[MESH_FAV_PEND];
static int        s_n_fav_pend = 0;

void recordar_favs(void)
{
  if (s_favs_lidos) { return; }
  s_favs_lidos = true;
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t n = sizeof(s_favs);
  if (nvs_get_blob(h, MESH_NVS_FAVS, s_favs, &n) == ESP_OK) { s_n_favs = (int)(n / sizeof(uint32_t)); }
  nvs_close(h);
}

static void salvar_favs(void)
{
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (s_n_favs) { nvs_set_blob(h, MESH_NVS_FAVS, s_favs, s_n_favs * sizeof(uint32_t)); }
  else          { nvs_erase_key(h, MESH_NVS_FAVS); }
  nvs_commit(h);
  nvs_close(h);
}

// Position in the favourites list; -1 = not one. NO LOCK.
static int fav_pos(uint32_t num)
{
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { if (s_favs[i] == num) { return i; } }
  return -1;
}

// Only the local list (and NVS, if it changed). NO LOCK.
static void fav_aplicar(uint32_t num, bool quer)
{
  const int pos = fav_pos(num);
  if (quer == (pos >= 0)) { return; }
  if (quer) {
    if (s_n_favs >= MESH_MAX_FAVS) { ESP_LOGW(TAG, "favourites full (%d)", MESH_MAX_FAVS); return; }
    s_favs[s_n_favs++] = num;
    achar_no(num);   // a favourite stays in the table even without having spoken this session
  } else {
    for (int i = pos; i < s_n_favs - 1; ++i) { s_favs[i] = s_favs[i + 1]; }
    --s_n_favs;
  }
  salvar_favs();
  ++s_nos_ver;
}

static int fav_pendente(uint32_t num)
{
  for (int i = 0; i < s_n_fav_pend; ++i) { if (s_fav_pend[i].num == num) { return i; } }
  return -1;
}

// The node is on the other end and has finished the dump: admin can be sent.
bool radio_aceita_admin(void)
{
  return s_servico_ativo && s_meu_num != 0 && s_config_ok_ms != 0
      && (s_via == AJUSTES_MESH_SERIAL || (s_via == AJUSTES_MESH_BLUETOOTH && mesh_ble_conectado()));
}

// ToRadio{ packet: MeshPacket{ to: us, decoded: Data{ portnum: ADMIN(6),
// payload: <AdminMessage> }, id } }. No 'from' (= 0): that is what the node
// recognises as LOCAL admin, from a connected client - accepted without a session
// key (AdminModule.cpp, branch mp.from == 0, from 2.5 to the current one).
// The service's only AdminMessage path: favourites and tab5_mesh_admin.
#define PORT_ADMIN 6
bool tab5_mesh_interno_admin(const uint8_t* adm, size_t n_adm)
{
  if (!adm || n_adm == 0 || n_adm > MESH_PAYLOAD_MAX + 33) { return false; }   // Data.payload: 233
  if (s_meu_num == 0) { return false; }   // without our number, 'to' would be 0 - and 0 is not the node
  const mesh_pb_envelope_t e = { s_meu_num, -1, PORT_ADMIN, adm, n_adm, esp_random() | 1u, 0, false };
  uint8_t quadro[MESH_PAYLOAD_MAX + 88];
  const size_t n = mesh_pb_to_radio(&e, quadro, sizeof(quadro));
  const bool ok = n && enlace_enviar(quadro, n);
  if (ok) { mesh_diario_nota_envio(MESH_ENV_ADMIN); }
  return ok;
}

bool tab5_mesh_interno_pode_admin(void) { return radio_aceita_admin(); }

static bool enviar_admin_favorito(uint32_t num, bool quer)
{
  // AdminMessage{ 39: set_favorite_node | 40: remove_favorite_node = num }
  uint8_t adm[12];
  pb_escritor_t w = pb_escritor(adm, sizeof(adm));
  pb_w_num(&w, quer ? 39 : 40, num);
  const bool ok = tab5_mesh_interno_admin(adm, pb_tamanho(&w));
  ESP_LOGI(TAG, "favourite %08lX -> %s on the node: %s", (unsigned long)num, quer ? "mark" : "unmark", ok ? "sent" : "failed");
  return ok;
}

// End of dump: what was changed without the radio goes now. With the lock.
void enviar_favs_pendentes(void)
{
  if (!radio_aceita_admin()) { return; }
  for (int i = 0; i < s_n_fav_pend; ++i) { enviar_admin_favorito(s_fav_pend[i].num, s_fav_pend[i].quer); }
  s_n_fav_pend = 0;
}

bool tab5_mesh_favorito(uint32_t num)
{
  travar();
  const bool f = fav_pos(num) >= 0;
  destravar();
  return f;
}

void tab5_mesh_definir_favorito(uint32_t num, bool quer)
{
  if (num == 0) { return; }
  travar();
  if (num != s_meu_num) {
    fav_aplicar(num, quer);
    // To the node: now, if it is there; otherwise it stays pending for the dump.
    int k = fav_pendente(num);
    if (radio_aceita_admin() && enviar_admin_favorito(num, quer)) {
      if (k >= 0) { s_fav_pend[k] = s_fav_pend[--s_n_fav_pend]; }
    } else {
      if (k < 0 && s_n_fav_pend < MESH_FAV_PEND) { k = s_n_fav_pend++; }
      if (k >= 0) { s_fav_pend[k] = fav_pend_t{ num, quer }; }
    }
  }
  destravar();
  sistema_avisar(AVISO_MESH_NOS);
}

// What the dump said about a node (field 10 absent = false, it's proto3). Our
// pending change wins: it is a change the node hasn't received yet.
void fav_do_radio(uint32_t num, bool fav)
{
  if (num == s_meu_num || s_via == AJUSTES_MESH_INTERNET || fav_pendente(num) >= 0) { return; }
  fav_aplicar(num, fav);
}

bool tab5_mesh_no(int idx, mesh_no_t* saida)
{
  bool ok = false;
  travar();
  const int n = ordem_nos();
  if (idx >= 0 && idx < n && s_nos[s_ordem[idx]].usado) {   // order from up to 250 ms ago
    *saida = s_nos[s_ordem[idx]];
    // DERIVED field, filled in only on the copy: without this it would age between
    // the read and the draw. With no source at all it stays -1, and the screen writes "?".
    saida->idade_s = idade_do_no(*saida);
    saida->favorito = fav_pos(saida->num) >= 0;
    ok = true;
  }
  destravar();
  return ok;
}

// ---------------------------------------------------------------------------
// SEARCHING THE TABLE (see tab5_mesh.hpp)
// ---------------------------------------------------------------------------
static const char* curto_conhecido(uint32_t num);

// Lower case and without accents, so "jose" finds "José" and "BR" finds "br". Only
// the two-byte Latin-1 (C3 xx), which is where Portuguese accents live; the
// rest of UTF-8 passes through as is (an emoji only finds the same emoji).
static void dobrar(const char* in, char* out, size_t tam)
{
  static const char LATIN[64 + 1] =
    "aaaaaaaceeeeiiii"   // C3 80..8F  (A-grave .. I-diaeresis)
    "dnooooo*ouuuuyps"   // C3 90..9F
    "aaaaaaaceeeeiiii"   // C3 A0..AF
    "dnooooo/ouuuuypy";  // C3 B0..BF
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 1 < tam) {
    if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { out[o++] = LATIN[p[1] - 0x80]; p += 2; continue; }
    out[o++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : (char)*p;
    ++p;
  }
  out[o] = 0;
}

// Does the node match the search (already folded)? Long name, short - the table's or,
// without them, the name cache's, which is what the row shows - and the id "!a1b2c3d4"
// (with or without the '!'). NO LOCK: called from inside it.
static bool no_bate(const mesh_no_t& no, const char* busca)
{
  char t[MESH_NOME_LONGO + 8];
  const char* nomes[2] = { no.longo[0] ? no.longo : longo_conhecido(no.num),
                           no.curto[0] ? no.curto : curto_conhecido(no.num) };
  for (const char* nome : nomes) {
    if (!nome) { continue; }
    dobrar(nome, t, sizeof(t));
    if (strstr(t, busca)) { return true; }
  }
  snprintf(t, sizeof(t), "!%08lx", (unsigned long)no.num);
  return strstr(t, busca) != nullptr;
}

// The filter is cached, like the order: the page asks for one row at a time, and
// filtering 256 nodes per drawn row would be the same waste. Redone
// when the search changes or the order was redone. With the lock held.
EXT_RAM_BSS_ATTR static int s_busca_ordem[MESH_MAX_NOS];
static int      s_busca_n = 0;
static char     s_busca_txt[MESH_BUSCA_MAX] = "";
static uint32_t s_busca_geracao = UINT32_MAX;

static int filtrar_nos(const char* busca)
{
  const int n = ordem_nos();
  char b[MESH_BUSCA_MAX];
  dobrar(busca ? busca : "", b, sizeof(b));
  if (s_busca_geracao == s_ordem_geracao && strcmp(b, s_busca_txt) == 0) { return s_busca_n; }
  s_busca_geracao = s_ordem_geracao;
  snprintf(s_busca_txt, sizeof(s_busca_txt), "%s", b);
  s_busca_n = 0;
  for (int k = 0; k < n; ++k) {
    if (!b[0] || no_bate(s_nos[s_ordem[k]], b)) { s_busca_ordem[s_busca_n++] = s_ordem[k]; }
  }
  return s_busca_n;
}

int tab5_mesh_num_nos_busca(const char* busca)
{
  travar();
  const int n = filtrar_nos(busca);
  destravar();
  return n;
}

bool tab5_mesh_no_busca(const char* busca, int idx, mesh_no_t* saida)
{
  bool ok = false;
  travar();
  const int n = filtrar_nos(busca);
  if (idx >= 0 && idx < n && s_nos[s_busca_ordem[idx]].usado) {
    *saida = s_nos[s_busca_ordem[idx]];
    saida->idade_s = idade_do_no(*saida);
    saida->favorito = fav_pos(saida->num) >= 0;
    ok = true;
  }
  destravar();
  return ok;
}

// ---------------------------------------------------------------------------
// NODE NAME CACHE
// ---------------------------------------------------------------------------
// HOW MESHTASTIC IDENTIFIES A NODE. The identifier is the "num" - a uint32
// derived from the last 4 bytes of the radio's MAC. It does NOT change: it is what goes in
// field 1 of the MeshPacket and what a direct message is addressed by.
//
// The long and short names are LABELS the person chooses and can change
// whenever they like. Changing the name does NOT create another node - the num stays the same, and
// to the system it is the same contact with a new label. A truly new node
// is a new num, and then it really is another conversation.
//
// That is why the conversation file has always been named by NUM (no-A1B2C3D4.txt),
// and that was right from the start. What was missing was storing the LABEL: without
// the C6L plugged in, s_nos is empty and the conversation was born called "A1B2C3D4".
//
// This cache solves that. It lives in nos.txt, one line per sighting, and the
// reader keeps the LAST one for each num - so renaming is just appending a
// line, without rewriting any file.
//
// HISTORY, NOT "LIVE" - deliberately DECOUPLED from MESH_MAX_NOS (32,
// the size of the LIVE table in s_nos - see achar_no() and eviction of the
// oldest). A node history that only grows - MESH_MAX_NOS was too small for
// that AS WELL (evicting from the live table solves "who is active now",
// but not "who have I ever seen"). Each entry is only 4+24+1 bytes; 256
// of them are ~8KB - nothing for an ESP32-P4 with PSRAM.
//
// ON SEARCHING BEFORE APPENDING (the question): yes, it is needed
// - and it is a LINEAR search, with no index at all, exactly as it was with 32.
// It only runs when a NodeInfo arrives (not on every packet - NodeInfo is
// sporadic) and once per line when loading the file at boot. With 256
// entries that is at most 256 integer comparisons - microseconds,
// even in the worst case. A cleverer structure (hash, tree) doesn't pay off
// for a number this size; it would only pay off in the thousands.
#define MESH_ARQ_NOS "nos.txt"
// 1024: the name cache filled up with internet nodes and, once full, refused
// new names - including a radio neighbour's. ~53 KB in PSRAM.
#define MESH_MAX_CONHECIDOS 1024

// SHORT AND LONG, both - the node table came out entirely nameless. Previously only the short one persisted here
// (see the big notice next to lembrar_nome() further down).
typedef struct { uint32_t num; char curto[MESH_NOME_CURTO]; char longo[MESH_NOME_LONGO]; bool sujo; } no_conhecido_t;
EXT_RAM_BSS_ATTR static no_conhecido_t s_conhecidos[MESH_MAX_CONHECIDOS];
static int s_n_conhecidos = 0;

// THE FILE HAS ALREADY BEEN READ THIS SESSION. The guard used to be "s_n_conhecidos ==
// 0": if the SD card didn't mount at read time, the table stayed empty, the
// node's dump filled it, and the first time the card came back ALL the nodes
// went again to the end of the file - which only grew, with the same lines,
// on every such boot. Now nobody writes before reading (descarregar_nomes).
bool s_nomes_lidos = false;

// NO LOCK: called from inside it. Null (not an empty string) when there is no
// cache - "no entry" and "empty entry" are the same thing here, both
// ways of saying "nothing to show".
static const char* curto_conhecido(uint32_t num)
{
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (s_conhecidos[i].num == num) { return s_conhecidos[i].curto[0] ? s_conhecidos[i].curto : nullptr; }
  }
  return nullptr;
}

const char* longo_conhecido(uint32_t num)
{
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (s_conhecidos[i].num == num) { return s_conhecidos[i].longo[0] ? s_conhecidos[i].longo : nullptr; }
  }
  return nullptr;
}

// WHAT THE SYSTEM FONTS DRAW. The smooth fonts cover ASCII and the
// Latin-1 that Portuguese uses (2 bytes in UTF-8, lead byte 0xC2/0xC3); emoji and
// symbols (3-4 bytes) become a box. Copies only what is drawable, without spaces
// at the ends. "?" on its own is Meshtastic's placeholder for "nobody
// chose a short name" - counts as empty.
void nome_legivel(const char* in, char* out, size_t tam)
{
  if (!tam) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)(in ? in : "");
  while (*p && o + 1 < tam) {
    if (*p >= 0x20 && *p <= 0x7e) { out[o++] = (char)*p++; continue; }
    if ((*p == 0xC2 || *p == 0xC3) && (p[1] & 0xC0) == 0x80) {
      if (o + 2 >= tam) { break; }
      out[o++] = (char)p[0]; out[o++] = (char)p[1]; p += 2; continue;
    }
    // A sequence the font doesn't have: skip all of it.
    const int n = ((*p & 0xE0) == 0xC0) ? 2 : ((*p & 0xF0) == 0xE0) ? 3 : ((*p & 0xF8) == 0xF0) ? 4 : 1;
    for (int k = 0; k < n && *p; ++k) { ++p; }
  }
  out[o] = 0;
  size_t ini = 0;
  while (out[ini] == ' ') { ++ini; }
  if (ini) { memmove(out, out + ini, strlen(out + ini) + 1); }
  size_t n = strlen(out);
  while (n && out[n - 1] == ' ') { out[--n] = 0; }
  if (!strcmp(out, "?")) { out[0] = 0; }
}

bool curto_utilizavel(const char* s)
{
  char t[MESH_NOME_CURTO * 2];
  nome_legivel(s, t, sizeof(t));
  return t[0] != 0;
}

// NO LOCK: called from inside it (lembrar_nome, achar_ou_criar_conversa).
// Live first (fresher), then the SD card cache.
void nome_exibicao(uint32_t num, char* out, size_t out_tam)
{
  const char* curto = nullptr;
  const char* longo = nullptr;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) {
      if (s_nos[i].curto[0]) { curto = s_nos[i].curto; }
      if (s_nos[i].longo[0]) { longo = s_nos[i].longo; }
      break;
    }
  }
  char t[MESH_NOME_LONGO];
  nome_legivel(curto ? curto : curto_conhecido(num), t, sizeof(t));
  if (!t[0]) { nome_legivel(longo ? longo : longo_conhecido(num), t, sizeof(t)); }
  if (t[0]) { snprintf(out, out_tam, "%s", t); return; }
  snprintf(out, out_tam, "!%08lx", (unsigned long)num);
}

void tab5_mesh_nome_exibicao(uint32_t num, char* out, size_t out_tam)
{
  travar();
  nome_exibicao(num, out, out_tam);
  destravar();
}

// See the full notice in tab5_mesh.hpp. Thin wrapper over curto_conhecido()
// (private to this file) - exists only to expose "cached, no hex" to
// callers who, unlike tab5_mesh_nome_exibicao(), do NOT want the hex fallback.
bool tab5_mesh_nome_cache(uint32_t num, char* out, size_t out_tam)
{
  const char* cache = curto_conhecido(num);
  if (!cache) { return false; }
  snprintf(out, out_tam, "%s", cache);
  return true;
}

// Sibling of tab5_mesh_nome_cache(), for the LONG name - see the big notice in the
// .hpp.
bool tab5_mesh_nome_longo_cache(uint32_t num, char* out, size_t out_tam)
{
  const char* cache = longo_conhecido(num);
  if (!cache) { return false; }
  snprintf(out, out_tam, "%s", cache);
  return true;
}

// Writes only when it CHANGES. node_info arrives on every introduction and every
// telemetry; always noting it down would fill the SD card with the same line.
//
// 'curto'/'longo' (short/long): each one NULL independently when this NodeInfo didn't
// bring (or brought something unusable in) that field - only updates what
// came, never erases the other one already in the cache.
void lembrar_nome(uint32_t num, const char* curto, const char* longo)
{
  const bool tem_curto = curto && curto[0];
  const bool tem_longo = longo && longo[0];
  if (num == 0 || (!tem_curto && !tem_longo)) { return; }

  int idx = -1;
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (s_conhecidos[i].num == num) { idx = i; break; }
  }
  if (idx < 0) {
    if (s_n_conhecidos >= MESH_MAX_CONHECIDOS) { return; }
    idx = s_n_conhecidos++;
    s_conhecidos[idx].num = num;
    s_conhecidos[idx].curto[0] = 0;
    s_conhecidos[idx].longo[0] = 0;
  }

  bool mudou = false;
  if (tem_curto && strcmp(s_conhecidos[idx].curto, curto) != 0) {
    snprintf(s_conhecidos[idx].curto, sizeof(s_conhecidos[idx].curto), "%s", curto);
    mudou = true;
  }
  if (tem_longo && strcmp(s_conhecidos[idx].longo, longo) != 0) {
    snprintf(s_conhecidos[idx].longo, sizeof(s_conhecidos[idx].longo), "%s", longo);
    mudou = true;
  }
  if (!mudou) { return; }

  // DOES NOT WRITE NOW - only marks. Writing here mounted and unmounted the SD card once
  // PER NODE, and the radio dumps the whole list at once on introduction:
  // at boot that came to eight mounts in one second, each followed by the SDMMC
  // LDO notice. Writing is worth a batch, not an event.
  s_conhecidos[idx].sujo = true;

  // A conversation that already exists switches to the new name, by the SAME rule as
  // display (short, long, id). ITS FILE is named by the num, so
  // renaming here doesn't detach the history.
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada && !s_conv[i].canal && s_conv[i].no == num) {
      nome_exibicao(num, s_conv[i].nome, sizeof(s_conv[i].nome));
    }
  }
}

// Flushes the pending names. ONE mount for all of them.
//
// Called by the mesh task, spaced out: the SD card is expensive to mount and a node name
// is not urgent information - if the device restarts before the flush, the
// next node_info teaches it again.
void descarregar_nomes(void)
{
  int pendentes = 0;
  for (int i = 0; i < s_n_conhecidos; ++i) { if (s_conhecidos[i].sujo) { ++pendentes; } }
  if (pendentes == 0) { return; }

  // Without having read the file, there's no telling whether the name is new: try reading
  // now; with no SD card, the pending ones wait for the next round.
  if (!s_nomes_lidos) { carregar_nomes(); }
  if (!s_nomes_lidos) { return; }
  pendentes = 0;
  for (int i = 0; i < s_n_conhecidos; ++i) { if (s_conhecidos[i].sujo) { ++pendentes; } }
  if (pendentes == 0) { return; }

  // THE FORMAT GAINED A FIELD: "hex,short,long" - it used to be just
  // "hex,short". carregar_nomes() (below) reads both formats: an
  // old line without the second comma just leaves the long one empty, breaking nothing.
  // Through the system writer: handing over is immediate, writing is batched.
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (!s_conhecidos[i].sujo) { continue; }
    char linha[96];
    snprintf(linha, sizeof(linha), "%08lX,%s,%s\n", (unsigned long)s_conhecidos[i].num,
             s_conhecidos[i].curto, s_conhecidos[i].longo);
    tab5_sd_anexar(MESH_PASTA, MESH_ARQ_NOS, linha);
    s_conhecidos[i].sujo = false;
  }
  ESP_LOGI(TAG, "%d node name(s) written", pendentes);
}

// Reads the cache from the SD card. The last line for each num wins.
//
// TWO FORMATS accepted - see the notice in descarregar_nomes(): "hex,short"
// (old) and "hex,short,long" (current). The line break is removed BEFORE
// looking for the second comma - it can only be at the real end,
// however many commas come before it.
void carregar_nomes(void)
{
  if (!tab5_sd_montar()) { return; }
  FILE* f = tab5_sd_abrir(MESH_PASTA, MESH_ARQ_NOS, "r");
  if (!f) { s_nomes_lidos = true; tab5_sd_desmontar(); return; }   // doesn't exist yet

  // Whoever was already in the table before the read came from the live node.
  EXT_RAM_BSS_ATTR static bool ao_vivo[MESH_MAX_CONHECIDOS];
  for (int i = 0; i < MESH_MAX_CONHECIDOS; ++i) { ao_vivo[i] = (i < s_n_conhecidos); }

  int linhas = 0;
  char linha[96];
  while (fgets(linha, sizeof(linha), f)) {
    char* resto = strchr(linha, ',');
    if (!resto) { continue; }
    *resto++ = 0;   // line = just the hex now
    ++linhas;

    char* fim = strpbrk(resto, "\r\n");
    if (fim) { *fim = 0; }   // resto = "curto" or "curto,longo" (short[,long]), no line break

    char* curto = resto;
    char* longo = strchr(resto, ',');
    if (longo) { *longo++ = 0; }   // new format: split the two fields

    if (!curto[0] && (!longo || !longo[0])) { continue; }

    const uint32_t num = (uint32_t)strtoul(linha, nullptr, 16);
    if (num == 0) { continue; }

    int idx = -1;
    for (int i = 0; i < s_n_conhecidos; ++i) {
      if (s_conhecidos[i].num == num) { idx = i; break; }
    }
    // The live node already taught the name before this read: it wins, and only
    // needs to go to the file if it differs from its LAST line there.
    if (idx >= 0 && ao_vivo[idx]) {
      s_conhecidos[idx].sujo = strcmp(s_conhecidos[idx].curto, curto) != 0
                            || strcmp(s_conhecidos[idx].longo, longo ? longo : "") != 0;
      continue;
    }
    if (idx < 0) {
      if (s_n_conhecidos >= MESH_MAX_CONHECIDOS) { continue; }
      idx = s_n_conhecidos++;
      s_conhecidos[idx].num = num;
      s_conhecidos[idx].curto[0] = 0;
      s_conhecidos[idx].longo[0] = 0;
      s_conhecidos[idx].sujo = false;
    }
    if (curto[0])         { snprintf(s_conhecidos[idx].curto, sizeof(s_conhecidos[idx].curto), "%s", curto); }
    if (longo && longo[0]) { snprintf(s_conhecidos[idx].longo, sizeof(s_conhecidos[idx].longo), "%s", longo); }
  }
  fclose(f);
  s_nomes_lidos = true;

  // COMPACT. The file is "append only" (renaming = a new line), so it
  // grows with lines the read above already discarded; and files written
  // before the s_nomes_lidos guard have the same node repeated dozens of times.
  // With too much slack, rewrite with one line per node - taking along what
  // was pending as well.
  if (linhas > s_n_conhecidos + 32) {
    // The whole new file in PSRAM and ONE swap through the system gateway
    // (tab5_sd_substituir) - never half a file.
    const size_t por = 8 + 3 + MESH_NOME_CURTO + MESH_NOME_LONGO;
    const size_t cap = (size_t)s_n_conhecidos * por + 1;
    char* txt = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (txt) {
      size_t k = 0;
      for (int i = 0; i < s_n_conhecidos && k < cap; ++i) {
        k += (size_t)snprintf(txt + k, cap - k, "%08lX,%s,%s\n", (unsigned long)s_conhecidos[i].num,
                              s_conhecidos[i].curto, s_conhecidos[i].longo);
      }
      if (k < cap && tab5_sd_substituir(MESH_PASTA, MESH_ARQ_NOS, txt, k)) {
        for (int i = 0; i < s_n_conhecidos; ++i) { s_conhecidos[i].sujo = false; }
        ESP_LOGI(TAG, "SD card: nos.txt compacted from %d to %d line(s)", linhas, s_n_conhecidos);
      }
      heap_caps_free(txt);
    }
  }
  tab5_sd_desmontar();
  ESP_LOGI(TAG, "SD card: %d known node(s)", s_n_conhecidos);
}

// ---------------------------------------------------------------------------

// Finds or creates the node's entry. The table is small and the search linear: with 32
// entries that is faster than any index, and there's no allocation.
mesh_no_t* achar_no(uint32_t num)
{
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) { return &s_nos[i]; }
  }
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (!s_nos[i].usado) {
      memset(&s_nos[i], 0, sizeof(s_nos[i]));
      s_nos[i].usado = true;
      s_nos[i].num = num;
      s_nos[i].snr = -128.0f;
      // HOPS STARTS UNKNOWN, not 0 (nodes were appearing with no name and no
      // hops). memset zeroes to 0, which is
      // EXACTLY the value for "direct neighbour" - a node only created by
      // achar_no() (seen in a packet, but no NodeInfo yet) would show "0
      // hops" as if confirmed, when really it's just "don't know
      // yet". Only ler_node_info() (NodeInfo, field 9) writes a real hop count;
      // until then, the sentinel avoids lying by omission.
      s_nos[i].saltos = MESH_SALTOS_DESCONHECIDO;
      ++s_nos_ver;
      return &s_nos[i];
    }
  }

  // TABLE FULL - this used to return nullptr and the NEW node was
  // ignored forever: whoever filled the slots first (not always the
  // most relevant nodes - just the first heard since boot) locked the
  // table, and nobody else got in, not even people chatting over MQTT who had
  // never appeared before. The table never swapped old nodes for new ones.
  //
  // THE WAY OUT: evict someone (criteria just below). That is safe because mesh_no_t is only a CACHE
  // of live metadata (name, battery, position) - the message history
  // lives on the SD card, indexed by the node's NUMBER, and survives intact when the node
  // leaves the table. If the evicted node speaks again, it reappears on the
  // next NodeInfo, like any "new" node.
  // WHO LEAVES, in three passes - the first that finds a candidate decides:
  //   0 - the INTERNET ones (via_mqtt) that aren't fixed: there are many, they come back on the next
  //       packet and aren't reachable without the internet;
  //   1 - any non-fixed one;
  //   2 - anyone (a table full of fixed ones - rare with 256).
  // Never ours. Within a pass, the OLDEST - by full age
  // (our sighting OR the radio's last_heard), not just by sighting:
  // a node from the dump has sighting 0 and was evicted first even if it had been
  // heard a minute ago, while the rest of the dump cycled through a single slot.
  int mais_velho = -1;
  int64_t maior = -1;
  for (int pass = 0; pass < 3 && mais_velho < 0; ++pass) {
    for (int i = 0; i < MESH_MAX_NOS; ++i) {
      const mesh_no_t& c = s_nos[i];
      if (c.num == s_meu_num || fav_pos(c.num) >= 0) { continue; }   // neither ours nor a favourite
      if (pass == 0 && (c.fixado || !c.via_mqtt)) { continue; }
      if (pass == 1 && c.fixado) { continue; }
      int64_t idade = idade_do_no(c);
      if (idade < 0) { idade = INT64_MAX / 2; }   // never heard: the oldest of all
      if (idade > maior) { maior = idade; mais_velho = i; }
    }
  }
  if (mais_velho < 0) { mais_velho = 0; }
  // NO LOG HERE ON PURPOSE (it cluttered the log). This is normal steady state (the table STAYS full in a mesh with
  // more than 32 nodes, and each eviction is expected, not an error) - only the COUNT
  // is kept, to look up when needed instead of appearing on its own
  // all the time. See tab5_mesh_despejos() and the "mesh" console command.
  ++s_despejos;
  ++s_nos_ver;
  memset(&s_nos[mais_velho], 0, sizeof(s_nos[mais_velho]));
  s_nos[mais_velho].usado = true;
  s_nos[mais_velho].num = num;
  s_nos[mais_velho].snr = -128.0f;
  s_nos[mais_velho].saltos = MESH_SALTOS_DESCONHECIDO;   // see the notice in the branch above
  return &s_nos[mais_velho];
}

// ---------------------------------------------------------------------------
// THE TAB5'S NODEDB, ON THE SD CARD
// ---------------------------------------------------------------------------
// The radio keeps at most MAX_NUM_NODES (100 on the C6L up to 2.7, 120 on 2.8):
// a constant in its firmware. The table here has 256 slots and also learns
// from live traffic - and, kept on the SD card, it survives a restart: the Tab5
// gets to know more nodes than the radio can remember.
//
// Binary with a header (magic, version, record size): if mesh_no_t
// changes shape in a new version, the old file is ignored instead of read
// wrongly. Written every 10 min if the table changed, and when the service stops.
//
// On load, what is "from this session" comes back zeroed: local sighting, SNR and the
// fixed place at the top. A node heard yesterday is not "on air" today; it returns to the
// top when it is heard again.
#define MESH_ARQ_NODEDB "nodedb.bin"
typedef struct { char magia[4]; uint16_t versao; uint16_t tam; uint32_t n; } nodedb_cab_t;
// THE COPY ONLY EXISTS WHILE WRITING OR READING (31 KB of PSRAM): it used to be a fixed
// array, used twice per boot and every 10 minutes.
static mesh_no_t* nodedb_copia(void)
{
  return (mesh_no_t*)heap_caps_malloc(sizeof(mesh_no_t) * MESH_MAX_NOS, MALLOC_CAP_SPIRAM);
}
uint32_t s_nodedb_ver_salva = 0;
// The SD card's NodeDB was read on this start of the service (or, with the card
// present, didn't exist). Only then may the table replace it - a table that
// started empty for lack of an SD card would erase the stored nodes.
static bool s_nodedb_lida = false;

void salvar_nodedb(void)
{
  mesh_no_t* s_nodedb_copia = nodedb_copia();
  if (!s_nodedb_copia) { return; }
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num != 0) { s_nodedb_copia[n++] = s_nos[i]; }
  }
  const uint32_t ver = s_nos_ver;
  destravar();
  if (n == 0 || !s_nodedb_lida) { heap_caps_free(s_nodedb_copia); return; }
  // Header and table in a single block, and ONE swap through the system gateway: a
  // restart midway leaves the old NodeDB whole, never half - and with the
  // SD card lent to the computer it goes to the journal.
  const size_t tam = sizeof(nodedb_cab_t) + sizeof(mesh_no_t) * (size_t)n;
  uint8_t* bloco = (uint8_t*)heap_caps_malloc(tam, MALLOC_CAP_SPIRAM);
  if (bloco) {
    const nodedb_cab_t cab = { { 'M', 'N', 'D', 'B' }, 1, (uint16_t)sizeof(mesh_no_t), (uint32_t)n };
    memcpy(bloco, &cab, sizeof(cab));
    memcpy(bloco + sizeof(cab), s_nodedb_copia, sizeof(mesh_no_t) * (size_t)n);
    if (tab5_sd_substituir(MESH_PASTA, MESH_ARQ_NODEDB, bloco, tam)) {
      s_nodedb_ver_salva = ver;
      ESP_LOGI(TAG, "SD card: %d node(s) in the Tab5 NodeDB", n);
    }
    heap_caps_free(bloco);
  }
  heap_caps_free(s_nodedb_copia);
}

// With the table empty (right after the memset in iniciar). Reads the SD card WITHOUT the
// lock (slow I/O) into the copy, and only the handover to the table is locked - the
// screen may be reading the table at the same time.
void carregar_nodedb(void)
{
  s_nodedb_lida = false;
  if (!tab5_sd_montar()) { return; }
  s_nodedb_lida = true;   // with the SD card: either it reads, or the file doesn't exist (the first save creates it)
  FILE* f = tab5_sd_abrir(MESH_PASTA, MESH_ARQ_NODEDB, "rb");
  if (!f) { tab5_sd_desmontar(); return; }
  mesh_no_t* s_nodedb_copia = nodedb_copia();
  if (!s_nodedb_copia) { fclose(f); tab5_sd_desmontar(); return; }
  nodedb_cab_t cab;
  int lidos = 0;
  if (fread(&cab, sizeof(cab), 1, f) == 1 && !memcmp(cab.magia, "MNDB", 4)
   && cab.versao == 1 && cab.tam == sizeof(mesh_no_t)) {
    const int total = (cab.n < MESH_MAX_NOS) ? (int)cab.n : MESH_MAX_NOS;
    lidos = (int)fread(s_nodedb_copia, sizeof(mesh_no_t), (size_t)total, f);
  } else {
    ESP_LOGW(TAG, "SD card: nodedb.bin from another version - ignored");
  }
  fclose(f);
  tab5_sd_desmontar();

  int n = 0;
  travar();
  for (int i = 0; i < lidos && n < MESH_MAX_NOS; ++i) {
    mesh_no_t no = s_nodedb_copia[i];
    if (no.num == 0) { continue; }
    // Those from the American network (removed) were marked here: they don't come back.
    if (no.rede_antiga != 0) { continue; }
    no.usado = true;
    no.visto_local_ms = 0;
    no.idade_s = -1;
    no.fixado = 0;
    no.snr = -128.0f;
    no.favorito = false;
    s_nos[n++] = no;
  }
  ++s_nos_ver;
  s_nodedb_ver_salva = s_nos_ver;
  destravar();
  heap_caps_free(s_nodedb_copia);
  ESP_LOGI(TAG, "SD card: %d node(s) remembered from the Tab5 NodeDB", n);
}

// See the full notice in tab5_mesh.hpp. READ ONLY - never creates an entry
// (unlike achar_no): asking "what is this node's link" must not have the
// side effect of inventing a node in the table.
bool tab5_mesh_no_enlace(uint32_t num, bool* via_mqtt, uint8_t* saltos)
{
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) {
      if (via_mqtt) { *via_mqtt = s_nos[i].via_mqtt; }
      if (saltos)   { *saltos   = s_nos[i].saltos;   }
      return true;
    }
  }
  return false;
}
