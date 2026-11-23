#include "tab5_glifos.hpp"

#include <string.h>
#include <limits.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tela_virtual.hpp"

static const char *TAG = "glifos";

// ---------------------------------------------------------------------------
// THE CACHE
// ---------------------------------------------------------------------------
// Hash table (open addressing) + an arena just for the
// images, both in PSRAM and created on first use. Full (arena or 3/4
// of the table): start again from scratch. Simple on purpose - the set of letters
// in use is small (few fonts, few colour combinations), and starting again
// only costs redrawing each letter once.
//
// ONE TASK ONLY: only the UI draws text (see tab5_ui.cpp - the fonts already
// have that rule). No lock here.
static const uint32_t TAB_N       = 4096;          // power of 2
// 512 KB filled up on the first measurement (561 letters, ~900 B each): the UI uses
// more font and colour combinations than it seemed. 1 MB fits ~1,100.
static const uint32_t ARENA_BYTES = 1024 * 1024;
static const int      RASCUNHO_L  = 160;           // largest letter that fits
static const int      RASCUNHO_A  = 160;

typedef struct {
  const void* fonte;
  uint32_t    frente, fundo;   // rgb888, as the TextStyle provides
  uint32_t    ofs;             // where the image starts in the arena
  uint16_t    codigo;
  uint16_t    l, a;            // block size
  uint16_t    alt_linha;       // metrics->height when it was stored
  int16_t     esq;             // block start relative to the letter's x
  int16_t     dir;             // end of the background (filled_x = x + dir)
  uint16_t    avanco;          // how far the cursor moves
  bool        usada;
} entrada_t;

static entrada_t* s_tab   = nullptr;
static uint8_t*   s_arena = nullptr;
static uint32_t   s_uso   = 0;
static uint32_t   s_n     = 0;
static bool       s_sem_memoria = false;
static glifos_estat_t s_est = {};

static lgfx::LGFX_Sprite* s_rascunho = nullptr;

// TRANSPARENT TEXT (no background colour): the entry stores the letter's MASK -
// the grey of each pixel, as in the VLW file - and is valid for any colour.
// Key: frente = fundo = MASCARA (an rgb888 colour never exceeds 0xFFFFFF).
static const uint32_t MASCARA = 0xFFFFFFFFu;
// The largest block the transparent letter reads and writes at once, and the place
// where the blend is done (in PSRAM - the UI stack is 8 KB). Only one:
// only one task draws at a time (see above).
static const int MASC_MAX_PX = 64 * 64;
static lgfx::rgb565_t* s_mistura = nullptr;

static bool preparar(void)
{
  if (s_tab) { return true; }
  if (s_sem_memoria) { return false; }
  s_tab   = (entrada_t*)heap_caps_calloc(TAB_N, sizeof(entrada_t), MALLOC_CAP_SPIRAM);
  s_arena = (uint8_t*)heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM);
  s_mistura = (lgfx::rgb565_t*)heap_caps_malloc(MASC_MAX_PX * sizeof(lgfx::rgb565_t), MALLOC_CAP_SPIRAM);
  s_rascunho = new lgfx::LGFX_Sprite();
  if (s_rascunho) {
    // The SAME format as the panel pages: the stored image goes to the
    // screen with no conversion at all.
    s_rascunho->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    s_rascunho->setPsram(true);
  }
  if (!s_tab || !s_arena || !s_mistura || !s_rascunho || !s_rascunho->createSprite(RASCUNHO_L, RASCUNHO_A)) {
    ESP_LOGE(TAG, "no PSRAM for the glyph cache - text via the original path");
    heap_caps_free(s_tab);   s_tab = nullptr;
    heap_caps_free(s_arena); s_arena = nullptr;
    heap_caps_free(s_mistura); s_mistura = nullptr;
    delete s_rascunho;       s_rascunho = nullptr;
    s_sem_memoria = true;
    return false;
  }
  ESP_LOGI(TAG, "glyph cache up (%u KB of images)", (unsigned)(ARENA_BYTES / 1024));
  return true;
}

static void recomecar(void)
{
  memset(s_tab, 0, TAB_N * sizeof(entrada_t));
  s_uso = 0;
  s_n = 0;
  ++s_est.limpezas;
}

static inline uint32_t espalhar(const void* fonte, uint16_t c, uint32_t frente, uint32_t fundo)
{
  uint32_t h = (uint32_t)(uintptr_t)fonte * 2654435761u;
  h ^= (uint32_t)c * 0x9E3779B1u;
  h ^= frente * 0x85EBCA6Bu;
  h ^= fundo  * 0xC2B2AE35u;
  h ^= h >> 15;
  return h & (TAB_N - 1);
}

static entrada_t* procurar(const void* fonte, uint16_t c, uint32_t frente, uint32_t fundo, bool* nova)
{
  uint32_t i = espalhar(fonte, c, frente, fundo);
  for (uint32_t k = 0; k < TAB_N; ++k, i = (i + 1) & (TAB_N - 1)) {
    entrada_t* e = &s_tab[i];
    if (!e->usada) { *nova = true; return e; }
    if (e->fonte == fonte && e->codigo == c && e->frente == frente && e->fundo == fundo) {
      *nova = false;
      return e;
    }
  }
  return nullptr;
}

void tab5_glifos_estat(glifos_estat_t* out)
{
  if (!out) { return; }
  *out = s_est;
  out->entradas = s_n;
  out->bytes = s_uso;
}

// ---------------------------------------------------------------------------
// THE DRAWING
// ---------------------------------------------------------------------------
static const lgfx::LGFXBase* s_cv_gfx = nullptr;
static uint16_t*             s_cv_buf = nullptr;
static int                   s_cv_l = 0, s_cv_a = 0;

void tab5_glifos_canvas_direto(const lgfx::LGFXBase* gfx, uint16_t* buf, int largura, int altura)
{
  s_cv_gfx = buf ? gfx : nullptr;
  s_cv_buf = buf;
  s_cv_l = largura;
  s_cv_a = altura;
}

// M5GFX's blend (VLWfont::drawChar, "alpha blend mode"), ink pixel by
// ink pixel, into RGB565 memory with stride 'passo'.
static void misturar_565(uint16_t* dst, int passo, const uint8_t* m, int m_passo, int w, int h, uint32_t cor888)
{
  const uint32_t fr = (cor888 >> 16) & 0xFF, fg = (cor888 >> 8) & 0xFF, fb = cor888 & 0xFF;
  for (int j = 0; j < h; ++j) {
    const uint8_t* linha = m + (size_t)j * m_passo;
    uint16_t* px = dst + (size_t)j * passo;
    for (int k = 0; k < w; ++k) {
      const uint32_t a = linha[k];
      if (!a) { continue; }
      const uint32_t p = 1 + a, q = 257 - p;
      const uint32_t c = px[k];
      const uint32_t r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
      const uint32_t r = (fr * p + ((r5 << 3) + (r5 >> 2)) * q) >> 8;
      const uint32_t g = (fg * p + ((g6 << 2) + (g6 >> 4)) * q) >> 8;
      const uint32_t b = (fb * p + ((b5 << 3) + (b5 >> 2)) * q) >> 8;
      px[k] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
}

// THE TRANSPARENT LETTER. The original (VLWfont::drawChar, "alpha blend mode")
// reads the screen and writes back ONE LINE AT A TIME: a 24 px letter is ~48
// trips to the canvas, and a 30-letter text exceeds a thousand - it was almost all the
// cost of a Berry app's desenhar() (tela.texto is transparent). Here the
// letter's whole block is read once, blended with the stored mask
// (the SAME sum as the original) and written once.
// Returns false = could not (stays with the original).
bool FonteRapida::letra_transparente(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c, uint16_t g,
                                     const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                                     size_t* avanco_out) const
{
  bool nova = false;
  entrada_t* e = procurar(this, c, MASCARA, MASCARA, &nova);
  if (!e) { return false; }
  if (nova) {
    uint32_t cab[6] = { 0 };
    _fontData->preRead();
    _fontData->seek(28 + g * 28);
    _fontData->read((uint8_t*)cab, 24);
    const int32_t h      = (int32_t)__builtin_bswap32(cab[0]);
    const int32_t w      = (int32_t)__builtin_bswap32(cab[1]);
    const int32_t avanco = (int32_t)__builtin_bswap32(cab[2]);
    const int32_t dY     = (int16_t)__builtin_bswap32(cab[3]);
    const int32_t xoff   = (int8_t)__builtin_bswap32(cab[4]);
    const int32_t yoff   = (int32_t)maxAscent - dY;
    const uint32_t bytes = (uint32_t)(w > 0 ? w : 0) * (uint32_t)(h > 0 ? h : 0);
    if (w <= 0 || h <= 0 || w * h > MASC_MAX_PX || yoff < -64 || yoff > 255) {
      _fontData->postRead();
      return false;
    }
    if (s_uso + bytes > ARENA_BYTES || s_n >= TAB_N * 3 / 4) {
      recomecar();
      bool n2 = false;
      e = procurar(this, c, MASCARA, MASCARA, &n2);
      if (!e) { _fontData->postRead(); return false; }
    }
    _fontData->seek(gBitmap[g]);
    _fontData->read(s_arena + s_uso, bytes);
    _fontData->postRead();
    *e = { };
    e->fonte = this; e->codigo = c;
    e->frente = MASCARA; e->fundo = MASCARA;
    e->ofs = s_uso; e->l = (uint16_t)w; e->a = (uint16_t)h;
    e->alt_linha = (uint16_t)metrics->height;
    e->esq = (int16_t)xoff; e->dir = (int16_t)yoff;   // in the mask: dir = vertical offset
    e->avanco = (uint16_t)avanco;
    e->usada = true;
    s_uso += bytes;
    ++s_n;
    ++s_est.faltas;
  } else {
    ++s_est.acertos;
  }

  // The ink block on screen, clipped by the canvas clip rectangle.
  const int32_t bx = x + e->esq;
  const int32_t by = y + metrics->y_offset + e->dir;
  int32_t cl, ct, cw, ch;
  gfx->getClipRect(&cl, &ct, &cw, &ch);
  const int32_t x0 = bx > cl ? bx : cl;
  const int32_t y0 = by > ct ? by : ct;
  const int32_t x1 = (bx + e->l) < (cl + cw) ? (bx + e->l) : (cl + cw);
  const int32_t y1 = (by + e->a) < (ct + ch) ? (by + e->a) : (ct + ch);
  *avanco_out = e->avanco;
  if (x0 >= x1 || y0 >= y1) { return true; }   // outside the clip: only move the cursor
  const int32_t rw = x1 - x0, rh = y1 - y0;

  // THE FAST PATH: straight into the back page of the system's drawing
  // object, only on the ink pixels (kernel/tela_virtual). The one below, via
  // M5GFX's readRect/pushImage, is for another canvas (an app sprite):
  // its read copies pixel by pixel through a generic routine, ~40 us/letter.
  {
    const uint8_t* m0 = s_arena + e->ofs + (size_t)(y0 - by) * e->l + (x0 - bx);
    if (tela_virtual_misturar_mascara(gfx, x0, y0, rw, rh, m0, e->l, style->fore_rgb888)) { return true; }
    // A registered canvas (layer): its memory, unrotated, stride = width.
    if (gfx == s_cv_gfx && x1 <= s_cv_l && y1 <= s_cv_a && x0 >= 0 && y0 >= 0) {
      misturar_565(s_cv_buf + (size_t)y0 * s_cv_l + x0, s_cv_l, m0, e->l, rw, rh, style->fore_rgb888);
      return true;
    }
  }

  // IN THE SCREEN FORMAT (RGB565), without going through RGB888: the read and the write
  // become a plain copy, and each ink pixel does the original's sum - the
  // background expanded to 8 bits the way M5GFX expands it (r5 << 3 | r5 >> 2), the
  // same blend, and the result truncated as it truncates. Same final pixel.
  lgfx::rgb565_t* const buf = s_mistura;
  gfx->readRect(x0, y0, rw, rh, buf);
  const int32_t fr = (style->fore_rgb888 >> 16) & 0xFF;
  const int32_t fg = (style->fore_rgb888 >> 8) & 0xFF;
  const int32_t fb = style->fore_rgb888 & 0xFF;
  const uint8_t* m = s_arena + e->ofs;
  for (int32_t yy = 0; yy < rh; ++yy) {
    const uint8_t* linha = m + (size_t)(y0 - by + yy) * e->l + (x0 - bx);
    lgfx::rgb565_t* px = buf + (size_t)yy * rw;
    for (int32_t xx = 0; xx < rw; ++xx) {
      const uint32_t a = linha[xx];
      if (!a) { continue; }
      const uint32_t p = 1 + a, q = 257 - p;
      const uint32_t r8 = px[xx].R8(), g8 = px[xx].G8(), b8 = px[xx].B8();
      const uint32_t r = (fr * p + r8 * q) >> 8;
      const uint32_t g = (fg * p + g8 * q) >> 8;
      const uint32_t b = (fb * p + b8 * q) >> 8;
      px[xx].raw = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  gfx->pushImage(x0, y0, rw, rh, buf);
  return true;
}

size_t FonteRapida::drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                             const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                             int32_t& filled_x) const
{
  // Whatever cannot be reproduced exactly takes the original path.
  // 16-bit destination: the stored image is RGB565 - on a 24-bit canvas
  // it would arrive with less colour precision than the original would draw.
  const bool com_fundo = style->back_rgb888 != style->fore_rgb888;
  const bool de_16_bits = ((int)gfx->getColorDepth() & 0xFF) == 16;
  uint16_t g = 0;
  // The REASON for each refusal is counted: that is what tells which rule is worth changing.
  uint32_t* motivo = nullptr;
  if      (c == 0x20)                                          { motivo = &s_est.rec_espaco; }
  else if (!de_16_bits || gfx->hasPalette())                   { motivo = &s_est.rec_destino; }
  else if (!com_fundo && !gfx->isReadable())                   { motivo = &s_est.rec_sem_fundo; }
  else if (style->size_x != 1.0f || style->size_y != 1.0f)     { motivo = &s_est.rec_escala; }
  else if (!getUnicodeIndex(c, &g) || !preparar())             { motivo = &s_est.rec_outro; }
  size_t avanco = 0;
  if (!motivo && !com_fundo) {
    if (letra_transparente(gfx, x, y, c, g, style, metrics, &avanco)) { return avanco; }
    motivo = &s_est.rec_outro;
  }
  if (motivo) {
    ++s_est.recusas;
    ++*motivo;
    return VLWfont::drawChar(gfx, x, y, c, style, metrics, filled_x);
  }

  bool nova = false;
  bool guardou = false;
  entrada_t* e = procurar(this, c, style->fore_rgb888, style->back_rgb888, &nova);
  if (e && !nova && e->alt_linha != (uint16_t)metrics->height) { e = nullptr; }   // different metrics: do not risk it

  if (e && nova) {
    // MISS: measures the letter from the file header (the same one
    // VLWfont::drawChar reads), works out the block it would write, and draws the
    // letter ONCE on a scratch sprite with M5GFX itself.
    uint32_t cab[6] = { 0 };
    _fontData->preRead();
    _fontData->seek(28 + g * 28);
    _fontData->read((uint8_t*)cab, 24);
    _fontData->postRead();
    const int32_t h      = (int32_t)__builtin_bswap32(cab[0]);
    const int32_t w      = (int32_t)__builtin_bswap32(cab[1]);
    const int32_t avanco = (int32_t)__builtin_bswap32(cab[2]);
    const int32_t dY     = (int16_t)__builtin_bswap32(cab[3]);
    const int32_t xoff   = (int8_t)__builtin_bswap32(cab[4]);
    const int32_t yoff   = (int32_t)maxAscent - dY;

    // The block: from the left of the ink (or from x, if it does not overhang) to the larger
    // of the end of the ink and the advance; from the top line to the larger of the
    // line height and the end of the ink. EVERY pixel in it is written by the
    // original (background or letter) - which is why the image replaces it entirely.
    const int32_t esq = xoff < 0 ? xoff : 0;
    const int32_t dir = (w + xoff) > avanco ? (w + xoff) : avanco;
    const int32_t l   = dir - esq;
    const int32_t a   = (yoff + h) > metrics->height ? (yoff + h) : metrics->height;
    const uint32_t bytes = (uint32_t)l * a * 2;

    // w == 0 (a letter with no ink that is not a space): the original does not paint the
    // letter's lines, and the block would have a hole. Stays with the original.
    if (w <= 0 || h < 0 || l <= 0 || a <= 0 || yoff < 0 || l > RASCUNHO_L || a > RASCUNHO_A) {
      e = nullptr;
    } else {
      if (s_uso + bytes > ARENA_BYTES || s_n >= TAB_N * 3 / 4) {
        recomecar();
        bool n2 = false;
        e = procurar(this, c, style->fore_rgb888, style->back_rgb888, &n2);
      }
      // On the scratch sprite: x = -esq puts the block at column 0; y = -y_offset puts the
      // top line at row 0 (drawChar adds y_offset). filled_x at its
      // minimum = the background starts at the block start, as in a standalone letter.
      int32_t f = INT32_MIN;
      s_rascunho->setClipRect(0, 0, l, a);
      VLWfont::drawChar(s_rascunho, -esq, -metrics->y_offset, c, style, metrics, f);
      const uint16_t* org = (const uint16_t*)s_rascunho->getBuffer();
      uint8_t* dst = s_arena + s_uso;
      for (int32_t r = 0; r < a; ++r) {
        memcpy(dst + (size_t)r * l * 2, org + (size_t)r * RASCUNHO_L, (size_t)l * 2);
      }
      *e = { };
      e->fonte = this; e->codigo = c;
      e->frente = style->fore_rgb888; e->fundo = style->back_rgb888;
      e->ofs = s_uso; e->l = (uint16_t)l; e->a = (uint16_t)a;
      e->alt_linha = (uint16_t)metrics->height;
      e->esq = (int16_t)esq; e->dir = (int16_t)dir; e->avanco = (uint16_t)avanco;
      e->usada = true;
      s_uso += bytes;
      ++s_n;
      guardou = true;
    }
  }

  // THE PREVIOUS LETTER OVERLAPS THIS ONE (its background went past this one's x): the
  // original does not repaint the background there, only the ink. The image would paint over it
  // - so this case stays with the original.
  if (!e || filled_x > x + e->esq) {
    ++s_est.recusas;
    ++*(e ? &s_est.rec_invade : &s_est.rec_outro);
    return VLWfont::drawChar(gfx, x, y, c, style, metrics, filled_x);
  }

  if (guardou) { ++s_est.faltas; } else { ++s_est.acertos; }
  gfx->pushImage(x + e->esq, y + metrics->y_offset, e->l, e->a,
                 (const lgfx::rgb565_t*)(s_arena + e->ofs));
  filled_x = x + e->dir;
  return e->avanco;
}
