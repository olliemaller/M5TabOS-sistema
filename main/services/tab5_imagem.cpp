#include "tab5_imagem.hpp"
#include "tab5_gif.hpp"
#include "tab5_volumes.hpp"

#include "esp_heap_caps.h"
#include <string.h>
#include <strings.h>

typedef enum { FMT_NENHUM, FMT_JPEG, FMT_PNG, FMT_BMP, FMT_QOI, FMT_GIF } formato_t;

typedef struct {
  formato_t formato;
  int       largura, altura;     // real (height always positive)
  bool      suportado;           // a decoder for this image exists (progressive JPEG: no)
  bool      bmp16;               // 16-bit BMP: its own path
  bool      de_cima;             // BMP: rows stored top to bottom
  uint32_t  bmp_offset;          // BMP: where the pixels start
} cabecalho_t;

// Sides above this aren't a real image (and overflow calculations). A colour
// BMP also keeps a row on the worker's stack (4 bytes per pixel).
#define LADO_MAX      65535
#define BMP_LADO_MAX  4096

static inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// ---------------------------------------------------------------------------
// HEADER (only what is needed to know the size and the decoding path)
// ---------------------------------------------------------------------------
static bool ler_exato(tab5_leitor_t* r, void* dst, size_t n)
{
  return tab5_leitor_ler(r, dst, n) == (int)n;
}

// JPEG: scans the markers up to the SOF (frame). Markers without a size (RSTn, SOI)
// pass straight through; the others are skipped by their size. Stopping at SOS/EOI = no SOF.
static bool cabecalho_jpeg(tab5_leitor_t* r, cabecalho_t* c)
{
  if (!tab5_leitor_ir(r, 2)) { return false; }
  for (int guarda = 0; guarda < 1024; ++guarda) {
    uint8_t b = 0, m = 0;
    do { if (!ler_exato(r, &b, 1)) { return false; } } while (b != 0xFF);
    do { if (!ler_exato(r, &m, 1)) { return false; } } while (m == 0xFF);
    if (m == 0x00 || m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { continue; }
    if (m == 0xD9 || m == 0xDA) { return false; }
    uint8_t t[2];
    if (!ler_exato(r, t, 2)) { return false; }
    const uint32_t tam = (uint32_t)(t[0] << 8) | t[1];
    if (tam < 2) { return false; }
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {   // SOF
      uint8_t s[5];   // precision, height (2), width (2)
      if (!ler_exato(r, s, 5)) { return false; }
      c->altura  = (int)((s[1] << 8) | s[2]);
      c->largura = (int)((s[3] << 8) | s[4]);
      c->suportado = (m == 0xC0 || m == 0xC1);   // sequential; C2 and others (progressive...) no
      return true;
    }
    if (!tab5_leitor_ir(r, tab5_leitor_pos(r) + tam - 2)) { return false; }
  }
  return false;
}

static bool cabecalho(tab5_leitor_t* r, cabecalho_t* c)
{
  memset(c, 0, sizeof(*c));
  uint8_t h[32];
  const int n = tab5_leitor_ler(r, h, sizeof(h));
  if (n < 12) { return false; }
  c->suportado = true;
  if (h[0] == 0xFF && h[1] == 0xD8) {
    c->formato = FMT_JPEG;
    if (!cabecalho_jpeg(r, c)) { return false; }
  } else if (n >= 24 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G' && !memcmp(h + 12, "IHDR", 4)) {
    c->formato = FMT_PNG;
    c->largura = (int)be32(h + 16);
    c->altura  = (int)be32(h + 20);
  } else if (h[0] == 'B' && h[1] == 'M' && n >= 30) {
    c->formato = FMT_BMP;
    c->bmp_offset = le32(h + 10);
    if (le32(h + 14) == 12) {            // old header (OS/2): 16-bit sides
      c->largura = (int)le16(h + 18);
      c->altura  = (int)le16(h + 20);
    } else {
      c->largura = (int)le32(h + 18);
      const int32_t alt = (int32_t)le32(h + 22);   // negative = top to bottom
      c->de_cima = alt < 0;
      c->altura = alt < 0 ? -alt : alt;
      c->bmp16 = le16(h + 28) == 16;
    }
    if (c->largura > BMP_LADO_MAX) { c->suportado = false; }
  } else if (n >= 14 && !memcmp(h, "qoif", 4)) {
    c->formato = FMT_QOI;
    c->largura = (int)be32(h + 4);
    c->altura  = (int)be32(h + 8);
  } else if (!memcmp(h, "GIF87a", 6) || !memcmp(h, "GIF89a", 6)) {
    c->formato = FMT_GIF;
    c->largura = (int)le16(h + 6);
    c->altura  = (int)le16(h + 8);
  } else {
    return false;
  }
  return c->largura > 0 && c->altura > 0 && c->largura <= LADO_MAX && c->altura <= LADO_MAX;
}

// ---------------------------------------------------------------------------
// SCALING
// ---------------------------------------------------------------------------
void tab5_imagem_caber(int l, int a, int L, int A, int* saida_l, int* saida_a)
{
  if (l <= L && a <= A) { *saida_l = l; *saida_a = a; return; }   // fits: original size
  if ((int64_t)L * a <= (int64_t)A * l) {   // width governs
    *saida_l = L;
    *saida_a = (int)((int64_t)a * L / l);
  } else {                                  // height governs
    *saida_a = A;
    *saida_l = (int)((int64_t)l * A / a);
  }
  if (*saida_l < 1) { *saida_l = 1; }
  if (*saida_a < 1) { *saida_a = 1; }
}

// ---------------------------------------------------------------------------
// THE READER AS SEEN BY M5GFX
// ---------------------------------------------------------------------------
// M5GFX's decoders (JPEG, PNG, BMP, QOI) ask for bytes through a
// DataWrapper. This one hands over those from the fast reader, bit by bit, and it is how
// cancellation arrives: once cancelled, the read "ends" and the decoder gives up.
class FonteLeitor : public lgfx::DataWrapper {
public:
  FonteLeitor(tab5_leitor_t* r, bool (*cancelado)(void)) : _r(r), _cancelado(cancelado) {}

  int read(uint8_t* buf, uint32_t len) override {
    if (_cancelado && _cancelado()) { return 0; }
    const int n = tab5_leitor_ler(_r, buf, len);
    return n < 0 ? 0 : n;
  }
  void skip(int32_t desloc) override {
    const int64_t alvo = (int64_t)tab5_leitor_pos(_r) + desloc;
    if (alvo >= 0) { tab5_leitor_ir(_r, (uint64_t)alvo); }
  }
  bool seek(uint32_t pos) override { return tab5_leitor_ir(_r, pos); }
  void close(void) override {}   // whoever opened the reader closes it
  int32_t tell(void) override { return (int32_t)tab5_leitor_pos(_r); }

private:
  tab5_leitor_t* _r;
  bool (*_cancelado)(void);
};

// ---------------------------------------------------------------------------
// 16-BIT BMP (what Camera writes)
// ---------------------------------------------------------------------------
// M5GFX's decoder reads BMP16 with the wrong colours; here the image comes
// row by row, and when it has to shrink each output pixel is the 2x2 average
// of the source. Only one row (or two) in memory, never the whole photo.

static inline uint16_t media4(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
  const uint32_t r  = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2) >> 2;
  const uint32_t g  = (((a >> 5) & 0x3F) + ((b >> 5) & 0x3F) + ((c >> 5) & 0x3F) + ((d >> 5) & 0x3F) + 2) >> 2;
  const uint32_t bl = ((a & 0x1F) + (b & 0x1F) + (c & 0x1F) + (d & 0x1F) + 2) >> 2;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static bool desenhar_bmp16(tab5_leitor_t* r, const cabecalho_t& c, lgfx::LovyanGFX* d, int px, int py,
                           int ol, int oa, bool (*cancelado)(void))
{
  const uint32_t W = (uint32_t)c.largura, H = (uint32_t)c.altura;
  const bool reduz = ((uint32_t)ol < W || (uint32_t)oa < H);
  uint16_t* atual = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_SPIRAM);
  uint16_t* baixo = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_SPIRAM);
  uint16_t* saida = (uint16_t*)heap_caps_malloc((size_t)ol * 2, MALLOC_CAP_SPIRAM);
  bool ok = atual && baixo && saida && tab5_leitor_ir(r, c.bmp_offset);

  const size_t quer = (size_t)W * 2;
  const size_t pulo = ((W * 2 + 3) / 4) * 4 - quer;   // BMP rows aligned to 4 bytes
  // The file generally stores the last row first (bottom to top).
  int o = c.de_cima ? 0 : oa - 1;
  const int passo = c.de_cima ? 1 : -1;
  // The file's RGB565 is the natural value; pushImage wants it byte-swapped
  // (destination state: restored at the end).
  const bool troca_ant = d->getSwapBytes();
  d->setSwapBytes(true);

  for (uint32_t k = 0; ok && k < H && o >= 0 && o < oa; ++k) {
    const uint32_t origem = c.de_cima ? k : H - 1 - k;
    uint16_t* t = baixo; baixo = atual; atual = t;
    ok = ler_exato(r, atual, quer) && (pulo == 0 || tab5_leitor_ler(r, nullptr, pulo) == (int)pulo);
    if (!ok) { break; }
    // Output rows that come from this source row. The other row of the average is
    // the one read just before (the one below, in a bottom-to-top BMP).
    while (o >= 0 && o < oa && ((uint32_t)o * H) / (uint32_t)oa == origem) {
      const uint16_t* vizinha = k > 0 ? baixo : atual;
      const uint16_t* linha = atual;
      if (reduz) {
        for (int x = 0; x < ol; ++x) {
          const uint32_t sx  = ((uint32_t)x * W) / (uint32_t)ol;
          const uint32_t sx2 = (sx + 1 < W) ? sx + 1 : sx;
          saida[x] = media4(atual[sx], atual[sx2], vizinha[sx], vizinha[sx2]);
        }
        linha = saida;
      }
      d->pushImage(px, py + o, ol, 1, linha);
      o += passo;
    }
    if ((k & 31) == 0 && cancelado && cancelado()) { ok = false; }
  }
  d->setSwapBytes(troca_ant);
  const bool completo = ok && (o < 0 || o >= oa);
  heap_caps_free(atual);
  heap_caps_free(baixo);
  heap_caps_free(saida);
  return completo;
}

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
static bool desenhar_gif(const char* caminho, lgfx::LovyanGFX* destino)
{
  tab5_gif_t* g = tab5_gif_abrir(caminho);
  if (!g) { return false; }
  const bool ok = tab5_gif_proximo(g, destino) >= 0;
  tab5_gif_fechar(g);
  return ok;
}

static bool desenhar_pelo_m5gfx(tab5_leitor_t* r, const cabecalho_t& c, lgfx::LovyanGFX* d,
                                int L, int A, bool (*cancelado)(void))
{
  // A scale that fits, without enlarging. Passed explicitly (rather than 0 = "fit") because
  // M5GFX's 0 enlarges a small image.
  float escala = 1.0f;
  if (c.largura > L) { escala = (float)L / (float)c.largura; }
  if (c.altura > A && (float)A / (float)c.altura < escala) { escala = (float)A / (float)c.altura; }

  FonteLeitor fonte(r, cancelado);
  if (!tab5_leitor_ir(r, 0)) { return false; }
  // With 'escala' given, the JPEG already comes out reduced by 1/2, 1/4 or 1/8 in the
  // decoder itself (fast) and the rest by the affine; centred by the datum.
  switch (c.formato) {
    case FMT_JPEG: return d->drawJpg(&fonte, 0, 0, L, A, 0, 0, escala, escala, lgfx::middle_center);
    case FMT_PNG:  return d->drawPng(&fonte, 0, 0, L, A, 0, 0, escala, escala, lgfx::middle_center);
    case FMT_BMP:  return d->drawBmp(&fonte, 0, 0, L, A, 0, 0, escala, escala, lgfx::middle_center);
    case FMT_QOI:  return d->drawQoi(&fonte, 0, 0, L, A, 0, 0, escala, escala, lgfx::middle_center);
    default:       return false;
  }
}

bool tab5_imagem_desenhar(const char* caminho, lgfx::LovyanGFX* destino, bool (*cancelado)(void))
{
  const int L = destino->width(), A = destino->height();
  if (L <= 0 || A <= 0) { return false; }
  tab5_leitor_t* r = tab5_leitor_abrir(caminho);
  if (!r) { return false; }
  cabecalho_t c;
  const bool conhecida = cabecalho(r, &c) && c.suportado;
  bool ok = false;
  if (conhecida && c.formato == FMT_GIF) {
    tab5_leitor_fechar(r);   // GIF reads the whole file and mounts the disk on its own
    r = nullptr;
    destino->fillScreen(TFT_BLACK);
    ok = desenhar_gif(caminho, destino);
  } else if (conhecida) {
    destino->fillScreen(TFT_BLACK);
    if (c.formato == FMT_BMP && c.bmp16) {
      int ol, oa;
      tab5_imagem_caber(c.largura, c.altura, L, A, &ol, &oa);
      ok = desenhar_bmp16(r, c, destino, (L - ol) / 2, (A - oa) / 2, ol, oa, cancelado);
    } else {
      ok = desenhar_pelo_m5gfx(r, c, destino, L, A, cancelado);
    }
  }
  if (r) { tab5_leitor_fechar(r); }
  return ok && !(cancelado && cancelado());
}

bool tab5_imagem_tamanho(const char* caminho, int* largura, int* altura)
{
  tab5_leitor_t* r = tab5_leitor_abrir(caminho);
  if (!r) { return false; }
  cabecalho_t c;
  const bool ok = cabecalho(r, &c);
  tab5_leitor_fechar(r);
  if (ok) { *largura = c.largura; *altura = c.altura; }
  return ok;
}

bool tab5_imagem_suportada(const char* nome)
{
  static const char* const EXT[] = { ".jpg", ".jpeg", ".png", ".bmp", ".qoi", ".gif", nullptr };
  const char* ponto = strrchr(nome, '.');
  if (!ponto || ponto == nome) { return false; }
  for (const char* const* e = EXT; *e; ++e) {
    if (!strcasecmp(ponto, *e)) { return true; }
  }
  return false;
}
