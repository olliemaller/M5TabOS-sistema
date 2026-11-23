// MUSIC PLAYER - the cover: the one in the ID3 tag, the folder's, the cache on the
// card and the dedicated task that handles the request. The internet search is in
// tab5_musica_rede.cpp. See tab5_musica_interno.hpp.
#include "tab5_musica_interno.hpp"
#include "ajustes.h"
#include "app.h"
#include "tab5_sd.hpp"
#include "tab5_volumes.hpp"
#include "tab5_wifi.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "musica";

// ---------------------------------------------------------------------------
// THE COVER (see tab5_musica.hpp)
// ---------------------------------------------------------------------------
// On its own task (stack in PSRAM, created on the first request and left
// waiting for the next): the internet search takes seconds and must not
// hold up either the sound or the interface. The sound loop only drops off the request.
static capa_ped_t          s_capa_ped = { };     // the last request (= the track playing)
static bool                s_capa_novo = false;  // there is a request the task has not picked up yet
static tab5_musica_capa_t  s_capa_estado = TAB5_MUSICA_CAPA_NENHUMA;
static char                s_capa_arq[200] = "";
static uint32_t            s_capa_versao = 0;
static TaskHandle_t        s_capa_tarefa = nullptr;

#define CAPA_PILHA       12288                // TLS + HTTP

tab5_musica_capa_t tab5_musica_capa(char* arquivo, size_t tam, uint32_t* versao)
{
  g_musica_trava.travar();
  const tab5_musica_capa_t e = s_capa_estado;
  if (arquivo && tam) { snprintf(arquivo, tam, "%s", e == TAB5_MUSICA_CAPA_PRONTA ? s_capa_arq : ""); }
  if (versao) { *versao = s_capa_versao; }
  g_musica_trava.destravar();
  return e;
}

// Reading the cover for the interface: through the volume service's gates
// (holds the disk and writes pending data first), the app does not open files.
uint8_t* tab5_musica_capa_ler(const char* arquivo, size_t* n)
{
  return tab5_volume_ler_tudo(arquivo, 3 * 1024 * 1024, n);
}

// JPEG that the screen's decoder (TJpgDec) opens: "baseline" only. A progressive
// cover (common inside MP3) would come out blank - better to look for another.
static bool jpeg_baseline(const uint8_t* b, size_t n)
{
  if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) { return false; }
  size_t i = 2;
  while (i + 4 <= n) {
    if (b[i] != 0xFF) { return false; }
    const uint8_t m = b[i + 1];
    if (m == 0xC0 || m == 0xC1) { return true; }
    if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { return false; }   // progressive, arithmetic...
    if (m == 0xD9 || m == 0xDA) { return false; }
    i += 2 + ((size_t)b[i + 2] << 8 | b[i + 3]);
  }
  return false;
}

static bool e_png(const uint8_t* b, size_t n) { return n > 8 && !memcmp(b, "\x89PNG", 4); }

// Writes the image to the cache: base + ".jpg"/".png". 'out' = the file.
static bool capa_gravar(const char* base, const uint8_t* img, size_t n, char* out, size_t tam)
{
  const bool png = e_png(img, n);
  if (!png && !jpeg_baseline(img, n)) { return false; }
  snprintf(out, tam, "%s.%s", base, png ? "png" : "jpg");
  FILE* f = fopen(out, "wb");
  if (!f) { return false; }
  const bool ok = fwrite(img, 1, n, f) == n;
  if (fclose(f) != 0 || !ok) { unlink(out); return false; }
  return true;
}

// The image inside the ID3v2 tag (APIC frame; the front cover if there is
// more than one). With the card mounted.
static bool capa_embutida(const char* abs, const char* base, char* out, size_t tam)
{
  const int fd = open(abs, O_RDONLY);
  if (fd < 0) { return false; }
  bool ok = false;
  uint8_t h[10];
  if (read(fd, h, 10) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 3 && h[3] <= 4 && !(h[5] & 0xC0)) {
    const uint32_t tag = syncsafe(h + 6);
    uint32_t pos = 10;
    while (!ok && pos + 10 <= tag + 10) {
      uint8_t q[10];
      lseek(fd, pos, SEEK_SET);
      if (read(fd, q, 10) != 10 || !q[0]) { break; }
      const uint32_t n = (h[3] == 4) ? syncsafe(q + 4) : be32(q + 4);
      if (!n || n > tag) { break; }
      // Compression/encryption/unsync on the frame = cannot be used raw. v2.4 with
      // "data length" (0x01) has 4 extra bytes in front.
      const bool cru = (h[3] == 3) ? !(q[9] & 0xC0) : !(q[9] & 0x0E);
      const size_t extra = (h[3] == 4 && (q[9] & 0x01)) ? 4 : 0;
      if (!memcmp(q, "APIC", 4) && cru && n > 16 + extra && n <= CAPA_MAX_BYTES) {
        uint8_t* d = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (d && (uint32_t)read(fd, d, n) == n) {
          const uint8_t enc = d[extra];
          size_t i = extra + 1;
          while (i < n && d[i]) { ++i; }   // MIME type (latin-1)
          ++i;
          ++i;                              // picture type (3 = front cover)
          if (enc == 1 || enc == 2) {       // description in UTF-16: ends in aligned 00 00
            while (i + 1 < n && (d[i] || d[i + 1])) { i += 2; }
            i += 2;
          } else {
            while (i < n && d[i]) { ++i; }
            ++i;
          }
          if (i < n) { ok = capa_gravar(base, d + i, n - i, out, tam); }
        }
        heap_caps_free(d);
      }
      pos += 10 + n;
    }
  }
  close(fd);
  return ok;
}

// cover.jpg, folder.jpg, front.jpg, capa.jpg, albumart*.jpg in the track's folder.
static bool capa_da_pasta(const char* abs, char* out, size_t tam)
{
  char pasta[160 + TAB5_MUSICA_CAMINHO_MAX + 2];
  snprintf(pasta, sizeof(pasta), "%s", abs);
  char* barra = strrchr(pasta, '/');
  if (!barra) { return false; }
  *barra = '\0';
  DIR* d = opendir(pasta);
  if (!d) { return false; }
  static const char* const NOMES[] = { "cover", "folder", "front", "capa", "albumart" };
  bool ok = false;
  struct dirent* e;
  while (!ok && (e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.' || e->d_type == DT_DIR) { continue; }
    if (!termina_com(e->d_name, ".jpg") && !termina_com(e->d_name, ".jpeg") && !termina_com(e->d_name, ".png")) { continue; }
    for (const char* nm : NOMES) {
      if (!strncasecmp(e->d_name, nm, strlen(nm))) {
        snprintf(out, tam, "%s/%s", pasta, e->d_name);
        ok = true;
        break;
      }
    }
  }
  closedir(d);
  return ok;
}

static tab5_musica_capa_t capa_resolver(const capa_ped_t* p, char* out, size_t tam)
{
  char raiz[160], base[200], abs[160 + TAB5_MUSICA_CAMINHO_MAX + 2];
  // The cache key: the album (artist + album) - tracks from the same album
  // share the cover; without an album, artist + title; with nothing, the file.
  uint32_t chave;
  if (p->album[0] && p->artista[0]) { chave = fnv(p->album, fnv(p->artista, fnv("d:"))); }
  else if (p->titulo[0])            { chave = fnv(p->titulo, fnv(p->artista, fnv("t:"))); }
  else                              { chave = fnv(p->caminho); }
  // THE CACHE LIVES ON THE CARD, the music may live on the USB drive: the cover of a
  // track on the USB drive is also kept (on the card), and without a card the
  // image in its folder still works.
  const bool sd = tab5_sd_montar();
  const bool abs_c = absoluto(p->caminho);
  const bool disco = abs_c ? tab5_volume_montar(p->caminho) : sd;
  if (!disco) { if (sd) { tab5_sd_desmontar(); } return TAB5_MUSICA_CAPA_NENHUMA; }
  if (sd) { musica_raiz_montado(raiz, sizeof(raiz)); }
  if (abs_c) { snprintf(abs, sizeof(abs), "%s", p->caminho); }
  else       { snprintf(abs, sizeof(abs), "%s/%s", raiz, p->caminho); }
  struct stat st;
  bool sem = false;
  bool ok = false;
  if (sd) {
    snprintf(base, sizeof(base), "%s/.capas", raiz);
    mkdir(base, 0775);
    snprintf(base, sizeof(base), "%s/.capas/%08lx", raiz, (unsigned long)chave);
    for (const char* ext : { ".jpg", ".png" }) {
      snprintf(out, tam, "%s%s", base, ext);
      if (stat(out, &st) == 0 && st.st_size > 0) { ok = true; break; }
    }
    if (!ok) { ok = capa_embutida(abs, base, out, tam); }
  }
  if (!ok) { ok = capa_da_pasta(abs, out, tam); }
  if (!ok && sd) {
    char marca[220];
    snprintf(marca, sizeof(marca), "%s.sem", base);
    sem = stat(marca, &st) == 0;
  }
  if (abs_c) { tab5_volume_desmontar(p->caminho); }
  if (sd) { tab5_sd_desmontar(); }
  if (ok) { return TAB5_MUSICA_CAPA_PRONTA; }
  if (!sd || sem || !ajustes_capa_internet() || tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return TAB5_MUSICA_CAPA_NENHUMA; }

  // THE INTERNET, with the card released (seconds of network do not hold the USB drive).
  uint8_t* img = nullptr;
  size_t n = 0;
  const net_t r = musica_capa_internet(p, &img, &n);
  if (r == NET_FALHOU) { heap_caps_free(img); return TAB5_MUSICA_CAPA_NENHUMA; }
  if (!tab5_sd_montar()) { heap_caps_free(img); return TAB5_MUSICA_CAPA_NENHUMA; }
  if (r == NET_ACHOU) {
    ok = capa_gravar(base, img, n, out, tam);
    if (ok) { ESP_LOGI(TAG, "cover from the internet: %s (%u bytes)", out, (unsigned)n); }
  }
  if (!ok) {
    // None (or one came that the screen cannot open): mark it, so as not to ask
    // again every time this track plays. Deleting .capas/ redoes the search.
    char marca[220];
    snprintf(marca, sizeof(marca), "%s.sem", base);
    FILE* f = fopen(marca, "w");
    if (f) { fclose(f); }
  }
  tab5_sd_desmontar();
  heap_caps_free(img);
  return ok ? TAB5_MUSICA_CAPA_PRONTA : TAB5_MUSICA_CAPA_NENHUMA;
}

static void capa_tarefa(void*)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      capa_ped_t p;
      g_musica_trava.travar();
      const bool tem = s_capa_novo;
      if (tem) { p = s_capa_ped; s_capa_novo = false; }
      g_musica_trava.destravar();
      if (!tem) { break; }
      char arq[200] = "";
      const tab5_musica_capa_t r = capa_resolver(&p, arq, sizeof(arq));
      g_musica_trava.travar();
      if (!s_capa_novo && !strcmp(s_capa_ped.caminho, p.caminho)) {   // still the one playing
        s_capa_estado = r;
        snprintf(s_capa_arq, sizeof(s_capa_arq), "%s", arq);
        ++s_capa_versao;
      }
      g_musica_trava.destravar();
      if (r == TAB5_MUSICA_CAPA_PRONTA) { sistema_acordar_interface(); }
    }
  }
}

void musica_capa_pedir(const char* caminho, const char* titulo, const char* artista, const char* album)
{
  g_musica_trava.travar();
  if (!strcmp(s_capa_ped.caminho, caminho) && s_capa_estado != TAB5_MUSICA_CAPA_NENHUMA) {
    g_musica_trava.destravar();   // the same track again (repeat one): already has it
    return;
  }
  snprintf(s_capa_ped.caminho, sizeof(s_capa_ped.caminho), "%s", caminho);
  snprintf(s_capa_ped.titulo, sizeof(s_capa_ped.titulo), "%s", titulo);
  snprintf(s_capa_ped.artista, sizeof(s_capa_ped.artista), "%s", artista);
  snprintf(s_capa_ped.album, sizeof(s_capa_ped.album), "%s", album);
  s_capa_novo = true;
  s_capa_estado = TAB5_MUSICA_CAPA_BUSCANDO;
  s_capa_arq[0] = '\0';
  ++s_capa_versao;
  TaskHandle_t t = s_capa_tarefa;
  g_musica_trava.destravar();
  if (!t) {
    if (xTaskCreatePinnedToCoreWithCaps(capa_tarefa, "capa", CAPA_PILHA, nullptr, 1, &t,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
      ESP_LOGW(TAG, "cover: the task did not start");
      g_musica_trava.travar();
      s_capa_estado = TAB5_MUSICA_CAPA_NENHUMA;
      ++s_capa_versao;
      g_musica_trava.destravar();
      return;
    }
    s_capa_tarefa = t;
  }
  xTaskNotifyGive(t);
}
