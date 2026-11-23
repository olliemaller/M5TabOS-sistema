// MUSIC PLAYER - the music folder and playlists: finding the folder on the
// card, listing, playing a folder or a playlist (.m3u). See
// tab5_musica_interno.hpp.
#include "tab5_musica_interno.hpp"
#include "tab5_sd.hpp"
#include "tab5_volumes.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

static const char* TAG = "musica";

// ---------------------------------------------------------------------------
// THE MUSIC FOLDER (see tab5_musica.hpp)
// ---------------------------------------------------------------------------
// It used to be just "/Music": anyone who created "Música" on the computer saw nothing, and the app
// even created an empty "Music" next to it. The first five letters, without
// accents and ignoring case, have to be "music" ("Música", "MUSICAS", "Music").
// "m_sic" also counts: that is how the Ú appeared in the 8.3 short name ("M_SICA~1").
static bool nome_de_musica(const char* utf8)
{
  // U+00C0..U+00FF without accents, in lower case (what comes after 0xC3 in UTF-8).
  static const char DOBRA[] = "aaaaaaaceeeeiiiidnooooo_ouuuuyts" "aaaaaaaceeeeiiiidnooooo_ouuuuyty";
  char n[8] = "";
  int k = 0;
  for (const uint8_t* p = (const uint8_t*)utf8; *p && k < 5; ++p) {
    uint8_t c = *p;
    if (c == 0xC3 && p[1] >= 0x80 && p[1] < 0xC0) { c = (uint8_t)DOBRA[p[1] - 0x80]; ++p; }
    else if (c >= 0x80) { c = 'x'; while ((p[1] & 0xC0) == 0x80) { ++p; } }
    n[k++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
  }
  n[k] = '\0';
  return k == 5 && n[0] == 'm' && (n[1] == 'u' || n[1] == '_') && !strncmp(n + 2, "sic", 3);
}

static int contar_itens(const char* pasta, int max)
{
  DIR* d = opendir(pasta);
  if (!d) { return -1; }
  int n = 0;
  struct dirent* e;
  while (n < max && (e = readdir(d)) != nullptr) { if (e->d_name[0] != '.') { ++n; } }
  closedir(d);
  return n;
}

// With the card already mounted.
static void achar_pasta(char* out, size_t tam)
{
  char melhor[160] = "", vazia_music[160] = "";
  int melhor_n = -1;
  DIR* d = opendir(TAB5_SD_RAIZ);
  if (d) {
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
      if (e->d_type != DT_DIR || !nome_de_musica(e->d_name)) { continue; }
      char c[160];
      snprintf(c, sizeof(c), TAB5_SD_RAIZ "/%s", e->d_name);
      const int n = contar_itens(c, 2000);
      if (n == 0 && !strcmp(e->d_name, "Music")) { snprintf(vazia_music, sizeof(vazia_music), "%s", c); }
      if (n > melhor_n) { melhor_n = n; snprintf(melhor, sizeof(melhor), "%s", c); }
    }
    closedir(d);
  }
  if (!melhor[0]) {
    snprintf(melhor, sizeof(melhor), TAB5_SD_RAIZ "/Music");
    mkdir(melhor, 0775);   // first time: the folder is ready to receive music
  } else if (vazia_music[0] && strcmp(vazia_music, melhor) != 0) {
    if (rmdir(vazia_music) == 0) { ESP_LOGI(TAG, "deleted the empty %s (the music is in %s)", vazia_music, melhor); }
  }
  snprintf(out, tam, "%s", melhor);
}

bool tab5_musica_pasta(char* out, size_t tam)
{
  if (!tab5_sd_montar()) { return false; }
  achar_pasta(out, tam);
  tab5_sd_desmontar();
  return true;
}

static bool e_musica(const char* n)
{
  const char* p = strrchr(n, '.');
  return p && (!strcasecmp(p, ".mp3") || !strcasecmp(p, ".wav") || !strcasecmp(p, ".m4a"));
}

static int comparar_itens(const void* x, const void* y)
{
  const tab5_musica_item_t* a = (const tab5_musica_item_t*)x;
  const tab5_musica_item_t* b = (const tab5_musica_item_t*)y;
  if (a->pasta != b->pasta) { return a->pasta ? -1 : 1; }   // folders first
  return strcasecmp(a->nome, b->nome);
}

// The disk of 'rel' already mounted (the card, or the absolute path's one). 'abs'
// receives the path of the listed folder.
static int listar_montado(const char* rel, tab5_musica_item_t* out, int max, bool* cheia, char* abs, size_t abs_tam)
{
  if (absoluto(rel)) {
    snprintf(abs, abs_tam, "%s", rel);
  } else {
    char raiz[160];
    achar_pasta(raiz, sizeof(raiz));
    if (rel && rel[0]) { snprintf(abs, abs_tam, "%s/%s", raiz, rel); }
    else               { snprintf(abs, abs_tam, "%s", raiz); }
  }
  if (cheia) { *cheia = false; }
  DIR* d = opendir(abs);
  if (!d) { return -1; }
  int n = 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.') { continue; }   // hidden files and macOS junk (._*) - and the .capas cache
    const bool pasta = e->d_type == DT_DIR;
    if (pasta && !(rel && rel[0]) && !strcasecmp(e->d_name, TAB5_MUSICA_PLAYLISTS)) { continue; }
    // What Windows leaves on a USB drive: recycle bin and system folder.
    if (pasta && (e->d_name[0] == '$' || !strcasecmp(e->d_name, "System Volume Information"))) { continue; }
    if (!pasta && !e_musica(e->d_name)) { continue; }
    if (n >= max) { if (cheia) { *cheia = true; } break; }
    snprintf(out[n].nome, TAB5_MUSICA_NOME_MAX, "%s", e->d_name);
    out[n].pasta = pasta;
    ++n;
  }
  closedir(d);
  qsort(out, (size_t)n, sizeof(tab5_musica_item_t), comparar_itens);
  return n;
}

// The disk where 'rel' lives: the card (relative to the music folder) or the
// absolute path's one.
bool musica_montar_rel(const char* rel)    { return absoluto(rel) ? tab5_volume_montar(rel) : tab5_sd_montar(); }
void musica_desmontar_rel(const char* rel) { if (absoluto(rel)) { tab5_volume_desmontar(rel); } else { tab5_sd_desmontar(); } }

int tab5_musica_listar(const char* rel, tab5_musica_item_t* out, int max, bool* cheia)
{
  if (!musica_montar_rel(rel)) { return -1; }
  char abs[320];
  const int n = listar_montado(rel, out, max, cheia, abs, sizeof(abs));
  musica_desmontar_rel(rel);
  return n;
}

bool tab5_musica_tocar(const char* rel, int idx, const char* app)
{
  if (idx < 0) { return false; }
  tab5_musica_item_t* itens = (tab5_musica_item_t*)heap_caps_malloc(
      sizeof(tab5_musica_item_t) * TAB5_MUSICA_LISTA_MAX, MALLOC_CAP_SPIRAM);
  caminho_t* fila = (caminho_t*)heap_caps_malloc(sizeof(caminho_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
  if (!itens || !fila) { heap_caps_free(itens); heap_caps_free(fila); return false; }
  bool ok = false;
  if (musica_montar_rel(rel)) {
    char abs[320], raiz[160] = "";
    const int n = listar_montado(rel, itens, TAB5_MUSICA_LISTA_MAX, nullptr, abs, sizeof(abs));
    if (!absoluto(rel)) { achar_pasta(raiz, sizeof(raiz)); }   // USB drive queue: paths are already absolute
    musica_desmontar_rel(rel);
    if (n > idx && !itens[idx].pasta) {
      // The queue: files only, in list order. The index becomes the queue index.
      int k = 0, inicio = -1;
      for (int j = 0; j < n && k < TAB5_MUSICA_FILA_MAX; ++j) {
        if (itens[j].pasta) { continue; }
        if (j == idx) { inicio = k; }
        if (rel && rel[0]) { snprintf(fila[k], sizeof(caminho_t), "%s/%s", rel, itens[j].nome); }
        else               { snprintf(fila[k], sizeof(caminho_t), "%s", itens[j].nome); }
        ++k;
      }
      ok = inicio >= 0 && musica_fila_tocar(fila, k, inicio, raiz, rel ? rel : "", nullptr, app);
    }
  }
  heap_caps_free(itens);
  heap_caps_free(fila);
  return ok;
}

// ---------------------------------------------------------------------------
// THE ROOT WITHOUT SCANNING AGAIN. achar_pasta() opens the card's root and counts
// what is in each music folder - expensive for something measuring the duration of 400
// files in a row. Remembered for a few seconds (with the card mounted).
// ---------------------------------------------------------------------------
static Trava s_trava_raiz;
static char    s_raiz_lembrada[160] = "";
static int64_t s_raiz_quando = 0;


void musica_raiz_montado(char* out, size_t tam)
{
  s_trava_raiz.travar();
  const int64_t agora = esp_timer_get_time();
  if (!s_raiz_lembrada[0] || agora - s_raiz_quando > 5000000) {
    achar_pasta(s_raiz_lembrada, sizeof(s_raiz_lembrada));
    s_raiz_quando = agora;
  }
  snprintf(out, tam, "%s", s_raiz_lembrada);
  s_trava_raiz.destravar();
}

// ---------------------------------------------------------------------------
// PLAYLISTS (see tab5_musica.hpp) - .m3u files in <musica>/Playlists.
// ---------------------------------------------------------------------------
// The name the person typed (UTF-8) -> a name FAT accepts: what
// FAT refuses (/ \ : * ? " < > | and control chars) becomes '_'; the rest goes as is.
static void nome_para_cartao(const char* utf8, char* out, size_t tam)
{
  size_t k = 0;
  for (const uint8_t* p = (const uint8_t*)utf8; *p && k + 1 < tam; ++p) {
    const uint8_t c = *p;
    out[k++] = (c < 0x20 || (c < 0x80 && strchr("/\\:*?\"<>|", (int)c))) ? '_' : (char)c;
  }
  out[k] = '\0';
  while (k && !musica_utf8_valido(out)) { out[--k] = '\0'; }   // the cut split a character
  while (k && (out[k - 1] == ' ' || out[k - 1] == '.')) { out[--k] = '\0'; }   // FAT strips these itself
}

static void pasta_playlists(const char* raiz, char* out, size_t tam)
{
  snprintf(out, tam, "%s/" TAB5_MUSICA_PLAYLISTS, raiz);
}

static void arquivo_playlist(const char* raiz, const char* nome, char* out, size_t tam)
{
  snprintf(out, tam, "%s/" TAB5_MUSICA_PLAYLISTS "/%s.m3u", raiz, nome);
}

// Reads the .m3u into 'out' (paths relative to the root). With the card mounted.
// "../X" lines are relative to the music folder (what we write); "/sdcard/..."
// inside it become relative; another disk ("/usb/...", the USB drive) stays
// absolute - the track plays when that USB drive is plugged in; the rest are
// relative to the Playlists folder (the format's default). A playlist written before
// FAT went UTF-8 has its names in CP437: converted here.
static int m3u_ler(const char* raiz, const char* nome, tab5_musica_faixa_t* out, int max)
{
  char arq[320];
  arquivo_playlist(raiz, nome, arq, sizeof(arq));
  FILE* f = fopen(arq, "r");
  if (!f) { return -1; }
  char linha[TAB5_MUSICA_CAMINHO_MAX + 64];
  int n = 0;
  const size_t nr = strlen(raiz);
  while (n < max && fgets(linha, sizeof(linha), f)) {
    size_t k = strlen(linha);
    while (k && (linha[k - 1] == '\n' || linha[k - 1] == '\r' || linha[k - 1] == ' ')) { linha[--k] = '\0'; }
    if (!k || linha[0] == '#') { continue; }
    for (char* q = linha; *q; ++q) { if (*q == '\\') { *q = '/'; } }
    char conv[TAB5_MUSICA_CAMINHO_MAX + 64];
    tab5_musica_nome_utf8(linha, conv, sizeof(conv));   // old CP437 -> UTF-8 (UTF-8 passes unchanged)
    const char* c = conv;
    char tmp[TAB5_MUSICA_CAMINHO_MAX + 64];
    if (!strncmp(c, "../", 3))                          { c += 3; }
    else if (!strncmp(c, raiz, nr) && c[nr] == '/')     { c += nr + 1; }
    else if (c[0] == '/' && tab5_volume_de(c) == TAB5_VOL_NENHUM) { continue; }   // not on any disk
    else if (c[0] == '/')                               { }   // another disk: stays absolute
    else { snprintf(tmp, sizeof(tmp), TAB5_MUSICA_PLAYLISTS "/%s", c); c = tmp; }
    snprintf(out[n].caminho, sizeof(out[n].caminho), "%s", c);
    ++n;
  }
  fclose(f);
  return n;
}

static bool m3u_gravar(const char* raiz, const char* nome, const tab5_musica_faixa_t* itens, int n)
{
  char arq[320];
  pasta_playlists(raiz, arq, sizeof(arq));
  mkdir(arq, 0775);
  arquivo_playlist(raiz, nome, arq, sizeof(arq));
  FILE* f = fopen(arq, "w");
  if (!f) { return false; }
  fputs("#EXTM3U\n", f);
  for (int i = 0; i < n; ++i) {
    fprintf(f, absoluto(itens[i].caminho) ? "%s\n" : "../%s\n", itens[i].caminho);
  }
  const bool ok = fclose(f) == 0;
  return ok;
}

static int comparar_playlists(const void* x, const void* y)
{
  const tab5_musica_item_t* a = (const tab5_musica_item_t*)x;
  const tab5_musica_item_t* b = (const tab5_musica_item_t*)y;
  const bool fa = !strcmp(a->nome, TAB5_MUSICA_FAVORITAS), fb = !strcmp(b->nome, TAB5_MUSICA_FAVORITAS);
  if (fa != fb) { return fa ? -1 : 1; }
  return strcasecmp(a->nome, b->nome);
}

int tab5_musica_playlists(tab5_musica_item_t* out, int max)
{
  if (!tab5_sd_montar()) { return -1; }
  char raiz[160], pasta[200];
  musica_raiz_montado(raiz, sizeof(raiz));
  pasta_playlists(raiz, pasta, sizeof(pasta));
  int n = 0;
  DIR* d = opendir(pasta);
  if (d) {
    struct dirent* e;
    while (n < max && (e = readdir(d)) != nullptr) {
      if (e->d_name[0] == '.' || e->d_type == DT_DIR || !termina_com(e->d_name, ".m3u")) { continue; }
      snprintf(out[n].nome, TAB5_MUSICA_NOME_MAX, "%s", e->d_name);
      out[n].nome[strlen(out[n].nome) - 4] = '\0';
      out[n].pasta = false;
      ++n;
    }
    closedir(d);
  }
  tab5_sd_desmontar();
  qsort(out, (size_t)n, sizeof(tab5_musica_item_t), comparar_playlists);
  return n;
}

bool tab5_musica_playlist_criar(const char* nome_utf8, char* nome_cartao, size_t tam)
{
  char nome[64];
  nome_para_cartao(nome_utf8 ? nome_utf8 : "", nome, sizeof(nome));
  if (nome_cartao && tam) { snprintf(nome_cartao, tam, "%s", nome); }
  if (!nome[0]) { return false; }
  if (!tab5_sd_montar()) { return false; }
  char raiz[160], arq[320];
  musica_raiz_montado(raiz, sizeof(raiz));
  arquivo_playlist(raiz, nome, arq, sizeof(arq));
  struct stat st;
  const bool ok = stat(arq, &st) != 0 && m3u_gravar(raiz, nome, nullptr, 0);
  tab5_sd_desmontar();
  return ok;
}

bool tab5_musica_playlist_apagar(const char* nome)
{
  if (!nome || !nome[0] || !tab5_sd_montar()) { return false; }
  char raiz[160], arq[320];
  musica_raiz_montado(raiz, sizeof(raiz));
  arquivo_playlist(raiz, nome, arq, sizeof(arq));
  const bool ok = unlink(arq) == 0;
  tab5_sd_desmontar();
  return ok;
}

int tab5_musica_playlist_itens(const char* nome, tab5_musica_faixa_t* out, int max)
{
  if (!nome || !nome[0] || !tab5_sd_montar()) { return -1; }
  char raiz[160];
  musica_raiz_montado(raiz, sizeof(raiz));
  const int n = m3u_ler(raiz, nome, out, max);
  tab5_sd_desmontar();
  return n;
}

// Reads, changes and writes back. 'por' = append at the end (false = remove). false = nothing changed.
static bool playlist_mudar(const char* nome, const char* caminho, bool por)
{
  if (!nome || !nome[0] || !caminho || !caminho[0]) { return false; }
  tab5_musica_faixa_t* it = (tab5_musica_faixa_t*)heap_caps_malloc(
      sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
  if (!it) { return false; }
  bool mudou = false;
  if (tab5_sd_montar()) {
    char raiz[160];
    musica_raiz_montado(raiz, sizeof(raiz));
    int n = m3u_ler(raiz, nome, it, TAB5_MUSICA_FILA_MAX);
    if (n < 0) { n = 0; }
    int achou = -1;
    for (int i = 0; i < n; ++i) { if (!strcmp(it[i].caminho, caminho)) { achou = i; break; } }
    if (por && achou < 0 && n < TAB5_MUSICA_FILA_MAX) {
      snprintf(it[n].caminho, sizeof(it[n].caminho), "%s", caminho);
      mudou = m3u_gravar(raiz, nome, it, n + 1);
    } else if (!por && achou >= 0) {
      memmove(&it[achou], &it[achou + 1], sizeof(tab5_musica_faixa_t) * (size_t)(n - achou - 1));
      mudou = m3u_gravar(raiz, nome, it, n - 1);
    }
    tab5_sd_desmontar();
  }
  heap_caps_free(it);
  return mudou;
}

bool tab5_musica_playlist_por(const char* nome, const char* caminho)   { return playlist_mudar(nome, caminho, true); }
bool tab5_musica_playlist_tirar(const char* nome, const char* caminho) { return playlist_mudar(nome, caminho, false); }

bool tab5_musica_playlist_tem(const char* nome, const char* caminho)
{
  if (!caminho || !caminho[0]) { return false; }
  tab5_musica_faixa_t* it = (tab5_musica_faixa_t*)heap_caps_malloc(
      sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
  if (!it) { return false; }
  const int n = tab5_musica_playlist_itens(nome, it, TAB5_MUSICA_FILA_MAX);
  bool tem = false;
  for (int i = 0; i < n && !tem; ++i) { tem = !strcmp(it[i].caminho, caminho); }
  heap_caps_free(it);
  return tem;
}

bool tab5_musica_tocar_playlist(const char* nome, int idx, const char* app)
{
  tab5_musica_faixa_t* it = (tab5_musica_faixa_t*)heap_caps_malloc(
      sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
  if (!it) { return false; }
  bool ok = false;
  if (tab5_sd_montar()) {
    char raiz[160];
    musica_raiz_montado(raiz, sizeof(raiz));
    const int n = m3u_ler(raiz, nome, it, TAB5_MUSICA_FILA_MAX);
    tab5_sd_desmontar();
    // tab5_musica_faixa_t is a caminho_t: the queue copies it directly.
    static_assert(sizeof(tab5_musica_faixa_t) == sizeof(caminho_t), "faixa = caminho");
    ok = n > 0 && musica_fila_tocar((const caminho_t*)it, n, idx, raiz, nullptr, nome, app);
  }
  heap_caps_free(it);
  return ok;
}

