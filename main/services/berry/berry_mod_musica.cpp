// Berry module 'musica' - the system player. See berry_apps.cpp (the engine) and
// berry_interno.hpp.

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ajustes.h"
#include "tab5_musica.hpp"
#include "tab5_usb.hpp"
#include "berry_interno.hpp"

// ---------------------------------------------------------------------------
// MODULE 'musica' - the system player (services/tab5_musica)
// ---------------------------------------------------------------------------
// The script is only the interface: the service finds the folder, lists, plays in the background,
// shows on the bar and in the mini player, has the X to stop, ends after being paused for
// 15 min and calculates the spectrum. Not one line of audio in Berry - nor should there be:
// decoding MP3 in a script would be ~100x too slow.

// pasta() - the name of the music folder ("Música"), or nil with no card.
static int mu_pasta(bvm* vm)
{
  char p[160];
  if (!tab5_musica_pasta(p, sizeof(p))) { be_return_nil(vm); }
  const char* b = strrchr(p, '/');
  char u[200];
  tab5_musica_nome_utf8(b ? b + 1 : p, u, sizeof(u));
  be_pushstring(vm, u);
  be_return(vm);
}

// pendrive() - the name of the USB-A flash drive, or nil with no flash drive. Its
// songs: lista("/usb"), tocar("/usb", i) - the whole flash drive is the "folder".
static int mu_pendrive(bvm* vm)
{
  if (!tab5_usb_presente()) { be_return_nil(vm); }
  char n[40];
  tab5_usb_nome(n, sizeof(n));
  be_pushstring(vm, n[0] ? n : "Pendrive");
  be_return(vm);
}

// lista([sub]) - [{nome, titulo, pasta}] of the subfolder (folders first);
// nil = no card or the folder does not exist. 'titulo' = the name without the extension.
static int mu_lista(bvm* vm)
{
  tab5_musica_item_t* it = (tab5_musica_item_t*)heap_caps_malloc(
      sizeof(tab5_musica_item_t) * TAB5_MUSICA_LISTA_MAX, MALLOC_CAP_SPIRAM);
  if (!it) { be_return_nil(vm); }
  const int n = tab5_musica_listar(arg_txt(vm, 1), it, TAB5_MUSICA_LISTA_MAX, nullptr);
  if (n < 0) { heap_caps_free(it); be_return_nil(vm); }
  lista_nova(vm);
  for (int i = 0; i < n; ++i) {
    char u[TAB5_MUSICA_NOME_MAX * 2], t[TAB5_MUSICA_NOME_MAX];
    tab5_musica_nome_utf8(it[i].nome, u, sizeof(u));
    snprintf(t, sizeof(t), "%s", it[i].nome);
    char* ponto = it[i].pasta ? nullptr : strrchr(t, '.');
    if (ponto) { *ponto = '\0'; }
    char tu[TAB5_MUSICA_NOME_MAX * 2];
    tab5_musica_nome_utf8(t, tu, sizeof(tu));
    char cam[TAB5_MUSICA_CAMINHO_MAX];
    const char* sub = arg_txt(vm, 1);
    if (sub && sub[0]) { snprintf(cam, sizeof(cam), "%s/%s", sub, it[i].nome); }
    else               { snprintf(cam, sizeof(cam), "%s", it[i].nome); }
    mapa_novo(vm);
    m_txt(vm, "nome", u);
    m_txt(vm, "titulo", tu);
    m_bool(vm, "pasta", it[i].pasta);
    m_txt(vm, "caminho", cam);   // as on the card: what playlist_por() and duracao() receive
    fim(vm);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  fim(vm);
  heap_caps_free(it);
  be_return(vm);
}

// tocar(sub, i) - plays the subfolder starting from item i of lista(sub). true = done.
static int mu_tocar(bvm* vm)
{
  const char* dono = C->app ? C->app->nome : nullptr;
  be_pushbool(vm, tab5_musica_tocar(arg_txt(vm, 1), arg_int(vm, 2, 0), dono));
  be_return(vm);
}

static int mu_pausar(bvm* vm)   { tab5_musica_tocar_pausar(); be_return_nil(vm); }
static int mu_proxima(bvm* vm)  { tab5_musica_proxima(); be_return_nil(vm); }
static int mu_anterior(bvm* vm) { tab5_musica_anterior(); be_return_nil(vm); }
static int mu_parar(bvm* vm)    { tab5_musica_parar(); be_return_nil(vm); }
static int mu_ir_para(bvm* vm)  { tab5_musica_ir_para((uint32_t)arg_int(vm, 1, 0)); be_return_nil(vm); }

// estado() - {ativo, tocando, titulo, artista, pos, dur (ms), idx, n, pasta, erro, kbps, hz}.
static int mu_estado(bvm* vm)
{
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  mapa_novo(vm);
  m_bool(vm, "ativo", e.ativo);
  m_bool(vm, "tocando", e.ativo && e.tocando);
  m_txt(vm, "titulo", e.titulo);
  m_txt(vm, "artista", e.artista);
  m_int(vm, "pos", (int)e.pos_ms);
  m_int(vm, "dur", (int)e.dur_ms);
  m_int(vm, "idx", e.idx);
  m_int(vm, "n", e.n);
  m_txt(vm, "pasta", e.rel);
  m_txt(vm, "erro", e.erro);
  m_int(vm, "kbps", e.kbps);
  m_int(vm, "hz", (int)e.hz);
  m_txt(vm, "album", e.album);
  m_txt(vm, "caminho", e.caminho);
  m_txt(vm, "playlist", e.playlist);
  m_bool(vm, "aleatorio", e.aleatorio);
  m_int(vm, "repetir", e.repetir);
  fim(vm);
  be_return(vm);
}

// aleatorio([sim]) - shuffle the queue; returns the result.
static int mu_aleatorio(bvm* vm)
{
  if (be_top(vm) >= 1 && be_isbool(vm, 1)) { tab5_musica_definir_aleatorio(be_tobool(vm, 1)); }
  be_pushbool(vm, tab5_musica_aleatorio());
  be_return(vm);
}

// repetir([r]) - 0 no (stops at the end), 1 the whole queue, 2 the same song.
static int mu_repetir(bvm* vm)
{
  if (be_top(vm) >= 1 && be_isint(vm, 1)) {
    const int r = arg_int(vm, 1, 0);
    if (r >= 0 && r <= 2) { tab5_musica_definir_repetir((tab5_musica_repetir_t)r); }
  }
  be_pushint(vm, (bint)tab5_musica_repetir());
  be_return(vm);
}

// playlists() - [{nome, id}]: 'nome' for display, 'id' for the other
// functions (the name as on the card). nil = no card.
static int mu_playlists(bvm* vm)
{
  tab5_musica_item_t* it = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * 64, MALLOC_CAP_SPIRAM);
  if (!it) { be_return_nil(vm); }
  const int n = tab5_musica_playlists(it, 64);
  if (n < 0) { heap_caps_free(it); be_return_nil(vm); }
  lista_nova(vm);
  for (int i = 0; i < n; ++i) {
    char u[TAB5_MUSICA_NOME_MAX * 2];
    tab5_musica_nome_utf8(it[i].nome, u, sizeof(u));
    mapa_novo(vm);
    m_txt(vm, "nome", u);
    m_txt(vm, "id", it[i].nome);
    fim(vm);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  fim(vm);
  heap_caps_free(it);
  be_return(vm);
}

// playlist(id) - [{caminho, titulo}] in file order; nil = does not exist.
static int mu_playlist(bvm* vm)
{
  tab5_musica_faixa_t* fx = (tab5_musica_faixa_t*)heap_caps_malloc(sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
  if (!fx) { be_return_nil(vm); }
  const int n = tab5_musica_playlist_itens(arg_txt(vm, 1), fx, TAB5_MUSICA_FILA_MAX);
  if (n < 0) { heap_caps_free(fx); be_return_nil(vm); }
  lista_nova(vm);
  for (int i = 0; i < n; ++i) {
    char t[TAB5_MUSICA_NOME_MAX], tu[TAB5_MUSICA_NOME_MAX * 2];
    const char* b = strrchr(fx[i].caminho, '/');
    snprintf(t, sizeof(t), "%s", b ? b + 1 : fx[i].caminho);
    char* ponto = strrchr(t, '.');
    if (ponto) { *ponto = '\0'; }
    tab5_musica_nome_utf8(t, tu, sizeof(tu));
    mapa_novo(vm);
    m_txt(vm, "caminho", fx[i].caminho);
    m_txt(vm, "titulo", tu);
    fim(vm);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  fim(vm);
  heap_caps_free(fx);
  be_return(vm);
}

// playlist_criar(nome) - the new one's id (the name as it ended up on the card); nil = failed.
static int mu_playlist_criar(bvm* vm)
{
  char id[TAB5_MUSICA_NOME_MAX];
  if (!tab5_musica_playlist_criar(arg_txt(vm, 1), id, sizeof(id))) { be_return_nil(vm); }
  be_pushstring(vm, id);
  be_return(vm);
}

static int mu_playlist_apagar(bvm* vm) { be_pushbool(vm, tab5_musica_playlist_apagar(arg_txt(vm, 1))); be_return(vm); }
static int mu_playlist_por(bvm* vm)    { be_pushbool(vm, tab5_musica_playlist_por(arg_txt(vm, 1), arg_txt(vm, 2))); be_return(vm); }
static int mu_playlist_tirar(bvm* vm)  { be_pushbool(vm, tab5_musica_playlist_tirar(arg_txt(vm, 1), arg_txt(vm, 2))); be_return(vm); }

// playlist_tocar(id, [i]) - plays the playlist from track i (without i: from the start).
static int mu_playlist_tocar(bvm* vm)
{
  const char* dono = C->app ? C->app->nome : nullptr;
  be_pushbool(vm, tab5_musica_tocar_playlist(arg_txt(vm, 1), be_top(vm) >= 2 ? arg_int(vm, 2, -1) : -1, dono));
  be_return(vm);
}

// capa() - the cover art file of what is playing (JPEG or PNG), or nil.
static int mu_capa(bvm* vm)
{
  char arq[200];
  if (tab5_musica_capa(arq, sizeof(arq), nullptr) != TAB5_MUSICA_CAPA_PRONTA) { be_return_nil(vm); }
  be_pushstring(vm, arq);
  be_return(vm);
}

// duracao(caminho) - ms, without playing (reads the header); 0 = failed.
static int mu_duracao(bvm* vm) { be_pushint(vm, (bint)tab5_musica_duracao(arg_txt(vm, 1))); be_return(vm); }

// espectro() - the 32 bands (0..255, bass first) of what is coming out now; nil =
// nothing playing. Reading turns the calculation on; stopping reading (window closed) turns it off.
static int mu_espectro(bvm* vm)
{
  uint8_t f[TAB5_MUSICA_FAIXAS];
  if (!tab5_musica_espectro(f)) { be_return_nil(vm); }
  lista_nova(vm);
  for (int i = 0; i < TAB5_MUSICA_FAIXAS; ++i) { l_int(vm, f[i]); }
  fim(vm);
  be_return(vm);
}

// cor(c) - the player's accent colour (the bar chip and the mini player).
static int mu_cor(bvm* vm)
{
  if (be_top(vm) >= 1 && be_isnumber(vm, 1)) { tab5_musica_definir_cor((uint16_t)arg_int(vm, 1, 0)); }
  be_pushint(vm, (bint)ajustes_cor_tocador());
  be_return(vm);
}

const membro_t BERRY_MOD_MUSICA[] = {
  { "pasta", mu_pasta }, { "lista", mu_lista }, { "tocar", mu_tocar }, { "pausar", mu_pausar },
  { "proxima", mu_proxima }, { "anterior", mu_anterior }, { "parar", mu_parar },
  { "ir_para", mu_ir_para }, { "estado", mu_estado }, { "espectro", mu_espectro },
  { "volume", berry_volume }, { "cor", mu_cor },
  { "aleatorio", mu_aleatorio }, { "repetir", mu_repetir },
  { "playlists", mu_playlists }, { "playlist", mu_playlist }, { "playlist_criar", mu_playlist_criar },
  { "playlist_apagar", mu_playlist_apagar }, { "playlist_por", mu_playlist_por },
  { "playlist_tirar", mu_playlist_tirar }, { "playlist_tocar", mu_playlist_tocar },
  { "capa", mu_capa }, { "duracao", mu_duracao }, { "pendrive", mu_pendrive },
  { "FAIXAS", nullptr, TAB5_MUSICA_FAIXAS },
  { nullptr },
};
