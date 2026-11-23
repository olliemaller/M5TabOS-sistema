// MUSIC PLAYER - duration without decoding: reads only the header (Xing/VBRI of
// MP3, WAV, MP4 moov) and remembers the result. See tab5_musica_interno.hpp.
#include "tab5_musica_interno.hpp"

#include "esp_heap_caps.h"
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "medf_mp4.h"
#include "tab5_midia.hpp"

// ---------------------------------------------------------------------------
// DURATION WITHOUT DECODING (see tab5_musica.hpp)
// ---------------------------------------------------------------------------
typedef struct { uint32_t h, ms; } dur_t;
#define DUR_MAX 1024                       // power of 2
static dur_t* s_dur = nullptr;             // PSRAM, created on first use; h = 0 = free slot

static uint32_t hash_caminho(const char* c) { const uint32_t h = fnv(c); return h ? h : 1; }

bool tab5_musica_duracao_lembrada(const char* caminho, uint32_t* ms)
{
  if (!s_dur || !caminho) { return false; }
  const uint32_t h = hash_caminho(caminho);
  bool achou = false;
  g_musica_trava.travar();
  for (uint32_t i = 0, k = h & (DUR_MAX - 1); i < 16; ++i, k = (k + 1) & (DUR_MAX - 1)) {
    if (s_dur[k].h == h) { *ms = s_dur[k].ms; achou = true; break; }
    if (!s_dur[k].h) { break; }
  }
  g_musica_trava.destravar();
  return achou;
}

static void dur_lembrar(const char* caminho, uint32_t ms)
{
  g_musica_trava.travar();
  if (!s_dur) { s_dur = (dur_t*)heap_caps_calloc(DUR_MAX, sizeof(dur_t), MALLOC_CAP_SPIRAM); }
  if (s_dur) {
    const uint32_t h = hash_caminho(caminho);
    uint32_t k = h & (DUR_MAX - 1), vaga = k;   // full nearby: overwrite the first one
    for (uint32_t i = 0; i < 16; ++i, k = (k + 1) & (DUR_MAX - 1)) {
      if (s_dur[k].h == h || !s_dur[k].h) { vaga = k; break; }
    }
    s_dur[vaga].h = h;
    s_dur[vaga].ms = ms;
  }
  g_musica_trava.destravar();
}

// MP3 frame header: true = valid. Fills in what the duration needs.
typedef struct { int versao; int camada; uint32_t kbps, hz, por_quadro, bytes; bool mono; } quadro_mp3_t;

static bool quadro_mp3(const uint8_t* b, quadro_mp3_t* q)
{
  static const uint16_t KBPS_V1[3][16] = {
    { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
    { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } };
  static const uint16_t KBPS_V2[2][16] = {
    { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } };
  static const uint32_t TAXAS[3] = { 44100, 48000, 32000 };
  if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) { return false; }
  const int ver = (b[1] >> 3) & 3;      // 0 = 2.5, 1 = reserved, 2 = 2, 3 = 1
  const int lay = 4 - ((b[1] >> 1) & 3); // 1..3 (4 = reserved)
  const int bi = b[2] >> 4, si = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
  if (ver == 1 || lay == 4 || bi == 0 || bi == 15 || si == 3) { return false; }
  q->versao = ver == 3 ? 1 : (ver == 2 ? 2 : 25);
  q->camada = lay;
  q->kbps = ver == 3 ? KBPS_V1[lay - 1][bi] : KBPS_V2[lay == 1 ? 0 : 1][bi];
  q->hz = TAXAS[si] >> (ver == 3 ? 0 : (ver == 2 ? 1 : 2));
  q->mono = ((b[3] >> 6) & 3) == 3;
  q->por_quadro = lay == 1 ? 384 : (lay == 3 && ver != 3 ? 576 : 1152);
  if (lay == 1) { q->bytes = (12 * q->kbps * 1000 / q->hz + pad) * 4; }
  else          { q->bytes = q->por_quadro / 8 * q->kbps * 1000 / q->hz + pad; }
  return q->bytes >= 21;
}

static uint32_t medir_mp3(int fd, long tam, uint8_t* buf, size_t cap)
{
  uint8_t h[10];
  long ini = 0;
  if (read(fd, h, 10) == 10 && !memcmp(h, "ID3", 3)) { ini = 10 + (long)syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0); }
  long fim = tam;
  if (tam >= 128) {
    uint8_t t[3];
    lseek(fd, tam - 128, SEEK_SET);
    if (read(fd, t, 3) == 3 && !memcmp(t, "TAG", 3)) { fim -= 128; }
  }
  lseek(fd, ini, SEEK_SET);
  const ssize_t n = read(fd, buf, cap);
  if (n < 64) { return 0; }
  for (ssize_t i = 0; i + 4 <= n; ++i) {
    quadro_mp3_t q;
    if (!quadro_mp3(buf + i, &q)) { continue; }
    // A false sync in the middle of junk: the next frame has to match.
    if (i + (ssize_t)q.bytes + 4 <= n) {
      quadro_mp3_t q2;
      if (!quadro_mp3(buf + i + q.bytes, &q2) || q2.hz != q.hz || q2.camada != q.camada) { continue; }
    }
    const uint8_t* f = buf + i;
    const size_t resto = (size_t)(n - i);
    const size_t o = 4 + (q.versao == 1 ? (q.mono ? 17 : 32) : (q.mono ? 9 : 17));
    if (resto >= o + 12 && (!memcmp(f + o, "Xing", 4) || !memcmp(f + o, "Info", 4)) && (be32(f + o + 4) & 1)) {
      return (uint32_t)((uint64_t)be32(f + o + 8) * q.por_quadro * 1000u / q.hz);
    }
    if (resto >= 36 + 18 && !memcmp(f + 36, "VBRI", 4)) {
      return (uint32_t)((uint64_t)be32(f + 36 + 14) * q.por_quadro * 1000u / q.hz);
    }
    const long bytes = fim - (ini + (long)i);
    return bytes > 0 ? (uint32_t)((uint64_t)bytes * 8u / q.kbps) : 0;   // kbps = bits per ms
  }
  return 0;
}

static uint32_t medir_wav(int fd)
{
  uint8_t h[12];
  if (read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { return 0; }
  uint32_t por_s = 0;
  for (int k = 0; k < 32; ++k) {
    uint8_t c[8];
    if (read(fd, c, 8) != 8) { return 0; }
    const uint32_t n = le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t f[16];
      if (n < 16 || read(fd, f, 16) != 16) { return 0; }
      por_s = le32(f + 8);
      lseek(fd, (off_t)(n - 16 + (n & 1)), SEEK_CUR);
    } else if (!memcmp(c, "data", 4)) {
      return por_s ? (uint32_t)((uint64_t)n * 1000u / por_s) : 0;
    } else {
      lseek(fd, (off_t)(n + (n & 1)), SEEK_CUR);
    }
  }
  return 0;
}

// The duration of an MP4/M4A: the header (moov) says. 0 = could not.
static uint32_t medir_mp4(const char* abs)
{
  tab5_midia_preparar();
  medf_arena_t a;
  if (!tab5_midia_arena(&a, MP4_ARENA)) { return 0; }
  a.direct = true;   // header only: no read-ahead
  uint32_t ms = 0;
  medf_mp4_demux_t* d = medf_mp4_demux_open(abs, &a, false, nullptr);
  if (d) {
    const int64_t us = medf_mp4_demux_info(d)->duration_us;
    ms = us > 0 ? (uint32_t)(us / 1000) : 0;
    medf_mp4_demux_close(d);
  }
  tab5_midia_arena_soltar(&a);
  return ms;
}

uint32_t tab5_musica_duracao(const char* caminho)
{
  uint32_t ms = 0;
  if (!caminho || !caminho[0]) { return 0; }
  if (tab5_musica_duracao_lembrada(caminho, &ms)) { return ms; }
  const size_t CAP = 4096;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  if (!buf) { return 0; }
  if (musica_montar_rel(caminho)) {
    char raiz[160], abs[160 + TAB5_MUSICA_CAMINHO_MAX + 2];
    if (absoluto(caminho)) { snprintf(abs, sizeof(abs), "%s", caminho); }
    else { musica_raiz_montado(raiz, sizeof(raiz)); snprintf(abs, sizeof(abs), "%s/%s", raiz, caminho); }
    const int fd = e_mp4(caminho) ? -1 : open(abs, O_RDONLY);
    if (e_mp4(caminho)) {
      ms = medir_mp4(abs);
      dur_lembrar(caminho, ms);
    } else if (fd >= 0) {
      const long tam = (long)lseek(fd, 0, SEEK_END);
      lseek(fd, 0, SEEK_SET);
      ms = termina_com(caminho, ".wav") ? medir_wav(fd) : medir_mp3(fd, tam, buf, CAP);
      close(fd);
      dur_lembrar(caminho, ms);   // 0 too: do not retry a file that cannot be measured
    }
    musica_desmontar_rel(caminho);
  }
  heap_caps_free(buf);
  return ms;
}

