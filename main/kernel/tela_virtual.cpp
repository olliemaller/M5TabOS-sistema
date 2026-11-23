#include "tela_virtual.hpp"
#include <string.h>
#include "app.h"

#include "compositor.hpp"
#include "tab5_quadro.hpp"
#include "tab5_tela.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

// DIAGNOSTICS (console: tela rolar cpu|dma). true = scrolling shifts using the CPU
// inside the back page, as before DMA - to compare the two paths
// on the device without reflashing.
bool g_tela_rolar_cpu = false;

static const char *TAG = "tela_virtual";

static const int FB_L = 720;
static const int FB_A = 1280;

// ---------------------------------------------------------------------------
// THE RUBBISH BIN
// ---------------------------------------------------------------------------
// Drawing WITHOUT a panel (idle, clock change): there is no page to write to,
// and the old pointer points at a framebuffer that has already been freed. The pixel has to
// go somewhere - it goes here, and the log warns once. It is only allocated if
// this actually happens; with the system working properly, it never exists.
static void* s_lixeira = nullptr;

static void* lixeira(void)
{
  if (!s_lixeira) {
    s_lixeira = heap_caps_aligned_calloc(128, 1, (size_t)FB_L * FB_A * 2, MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "drawing with the panel off - sending it to the rubbish bin (%s)",
             s_lixeira ? "1.8 MB in PSRAM" : "NO MEMORY");
  }
  return s_lixeira;
}

// THE PANEL IS BACK: the bin goes. It only exists for an untimely drawing
// with the panel off (idle); kept around, it was 1.8 MB of PSRAM tied up until
// a restart because of a single stray drawing.
static void soltar_lixeira(void)
{
  if (!s_lixeira) { return; }
  heap_caps_free(s_lixeira);
  s_lixeira = nullptr;
  ESP_LOGI(TAG, "panel back - rubbish bin released (1.8 MB of PSRAM returned)");
}

// ---------------------------------------------------------------------------
// THE TRACKING PANEL
// ---------------------------------------------------------------------------
// An ordinary Panel_Sprite that, before each write, OPENS the frame and records the
// rectangle touched. These are ALL the doors through which M5GFX writes to a sprite
// (LGFX_Sprite.hpp: setWindow+writeBlock/writePixels, drawPixel, fillRect,
// writeImage[ARGB], copyRect). Text, line, circle, image - everything comes down
// to one of them.
//
// The coordinates arriving here are LOGICAL (already rotated). The
// recorded rectangle has to be PHYSICAL, because that is what the compositor copies -
// so the conversion is the SAME one Panel_Sprite does internally
// (LGFX_Sprite.cpp, writeFillRectPreclipped), repeated here.
struct PainelRastreado : public lgfx::Panel_Sprite
{
  // SWAPS ONLY THE POINTER. setBuffer() would redo the rotation and reset the
  // write window - and the swap happens in the MIDDLE of a write (between setWindow and
  // writeBlock, when the first write of the pass opens the frame).
  // SpriteBuffer::reset() with "preallocated" memory frees nothing.
  void apontar(void* buf)
  {
    if (buf != _img.get()) { _img.reset(buf); }
  }

  // Opens the frame (if not already open) and ensures the pixel goes to its
  // back page. Called BEFORE every write.
  void abrir(void)
  {
    void* tras = comp_abrir();
    apontar(tras ? tras : lixeira());
    if (tras && s_lixeira) { soltar_lixeira(); }   // already pointing at the page, not at the bin
  }

  tab5_ret_t fisico(int x, int y, int w, int h) const
  {
    const uint_fast8_t r = _rotation;
    if (r) {
      if ((1u << r) & 0b10010110) { y = _height - (y + h); }
      if (r & 2)                  { x = _width  - (x + w); }
      if (r & 1) { std::swap(x, y); std::swap(w, h); }
    }
    return { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
  }

  // 'opaco': the write covers the whole rectangle with a solid colour (fillRect) -
  // whatever the back page had out of date there need not come from the front.
  void anotar(int x, int y, int w, int h, bool opaco = false)
  {
    if (w <= 0 || h <= 0) { return; }
    abrir();
    const tab5_ret_t f = fisico(x, y, w, h);
    // Shortcut: the same rectangle again (text is dozens of small fillRects
    // within one glyph) does not need to go to the list.
    // Only valid within the SAME round: once the compositor has consumed the
    // list, the same rectangle touched again is a new change.
    const uint32_t rodada = comp_rodada();
    if (rodada == _ult_rodada
     && f.x >= _ult_x && f.y >= _ult_y && f.x + f.l <= _ult_x + _ult_l && f.y + f.a <= _ult_y + _ult_a) { return; }
    // The back page may still be missing what the previous frame changed
    // here (lazy sync): it comes from the front BEFORE the pixel. The shortcut
    // above may skip this - the larger rectangle containing it has already been prepared.
    if (opaco) { comp_cobrir(f); } else { comp_preparar(f); }
    comp_invalidar(f);
    _ult_x = f.x; _ult_y = f.y; _ult_l = f.l; _ult_a = f.a; _ult_rodada = rodada;
  }
  void esquecer_atalho(void) { _ult_l = 0; _ult_a = 0; }

  void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override
  {
    Panel_Sprite::setWindow(xs, ys, xe, ye);
    _jan_marcada = false;
  }
  void writeBlock(uint32_t cor, uint32_t len) override
  {
    marcar_janela();
    Panel_Sprite::writeBlock(cor, len);
  }
  void writePixels(lgfx::pixelcopy_t* p, uint32_t len, bool dma) override
  {
    marcar_janela();
    Panel_Sprite::writePixels(p, len, dma);
  }
  void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t cor) override
  {
    anotar(x, y, 1, 1);
    Panel_Sprite::drawPixelPreclipped(x, y, cor);
  }
  // A LARGE RECTANGLE GOES TO THE PPA. Clearing the usable area (1.6 MB) cost
  // 20-25 ms of CPU on every app opening; the fill engine just
  // writes, without the CPU. A small rectangle (text, borders) stays on the CPU: there the
  // round trip to the PPA costs more than painting.
  static constexpr uint32_t PREENCHER_PPA_MIN = 16384;   // px (e.g. 720x23)

  void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t cor) override
  {
    anotar(x, y, w, h, true);   // solid colour over the whole rectangle
    if ((uint32_t)w * h >= PREENCHER_PPA_MIN && _write_bits == 16
     && tab5_quadro_preencher(_img.get(), fisico(x, y, w, h), (uint16_t)cor)) { return; }
    Panel_Sprite::writeFillRectPreclipped(x, y, w, h, cor);
  }
  void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t* p, bool dma) override
  {
    anotar(x, y, w, h);
    Panel_Sprite::writeImage(x, y, w, h, p, dma);
  }
  void writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t* p) override
  {
    anotar(x, y, w, h);
    Panel_Sprite::writeImageARGB(x, y, w, h, p);
  }

  // SCROLLING VIA DMA. Shifting a window's content used to be a CPU memmove
  // within the same image (~1.6 MB per drag step). Now the FRONT
  // page - which has exactly what is on screen - is copied, shifted,
  // to the back one: different buffers, no overlap, and DMA2D does the
  // work ~1.8x faster than the CPU, which is left free.
  //
  // The condition: the SOURCE must not have been drawn in this frame (the front
  // does not have that drawing); if it has, it falls back to the CPU copyRect, which reads the
  // back one. And whatever the sync had pending inside the destination is never
  // copied: the DMA writes over it (comp_copiar_da_frente).
  void copyRect(uint_fast16_t dx, uint_fast16_t dy, uint_fast16_t w, uint_fast16_t h, uint_fast16_t sx, uint_fast16_t sy) override
  {
    if (w == 0 || h == 0) { return; }
    const tab5_ret_t dst = fisico(dx, dy, w, h);
    const tab5_ret_t src = fisico(sx, sy, w, h);
    abrir();
    if (!g_tela_rolar_cpu && !comp_desenhado_neste_quadro(src) && comp_copiar_da_frente(dst, src.x, src.y)) {
      anotar(dx, dy, w, h);
      return;
    }
    comp_preparar(src);          // the CPU is going to READ the source on the back page
    anotar(dx, dy, w, h);
    Panel_Sprite::copyRect(dx, dy, w, h, sx, sy);
  }

  // READING also needs the back page up to date (smooth text without background,
  // and anyone else who reads pixels back).
  void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, lgfx::pixelcopy_t* p) override
  {
    if (w && h) { comp_preparar(fisico(x, y, w, h)); }
    Panel_Sprite::readRect(x, y, w, h, dst, p);
  }

  // BLENDS A MASK (smooth transparent text) STRAIGHT ONTO THE BACK PAGE.
  // The rectangle arrives already clipped (drawing coordinates). Only the ink
  // pixels are read and written; the maths is M5GFX's (VLWfont::drawChar,
  // "alpha blend mode"), with the background expanded from RGB565 the way it expands it.
  // false = a format that is not the page's (stays on the ordinary path).
  bool misturar(int x, int y, int w, int h, const uint8_t* m, int passo, uint32_t cor888)
  {
    if (w <= 0 || h <= 0) { return true; }
    if (_write_bits != 16) { return false; }
    anotar(x, y, w, h);            // opens the frame and brings the back page up to date there
    // Address of (x, y) and how far it moves per column and per row - with
    // rotation, the same maths as Panel_Sprite's drawPixelPreclipped.
    auto indice = [this](int px, int py) -> int32_t {
      const uint_fast8_t r = _rotation;
      if (r) {
        if ((1u << r) & 0b10010110) { py = _height - (py + 1); }
        if (r & 2)                  { px = _width  - (px + 1); }
        if (r & 1) { std::swap(px, py); }
      }
      return px + py * (int32_t)_bitwidth;
    };
    const int32_t base = indice(x, y);
    const int32_t dx = indice(x + 1, y) - base;
    const int32_t dy = indice(x, y + 1) - base;
    uint16_t* const img = _img.img16();
    const uint32_t fr = (cor888 >> 16) & 0xFF, fg = (cor888 >> 8) & 0xFF, fb = cor888 & 0xFF;
    for (int j = 0; j < h; ++j) {
      const uint8_t* linha = m + (size_t)j * passo;
      int32_t i = base + j * dy;
      for (int k = 0; k < w; ++k, i += dx) {
        const uint32_t a = linha[k];
        if (!a) { continue; }
        const uint32_t p = 1 + a, q = 257 - p;
        const uint32_t c = img[i];
        const uint32_t r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        const uint32_t r = (fr * p + ((r5 << 3) + (r5 >> 2)) * q) >> 8;
        const uint32_t g = (fg * p + ((g6 << 2) + (g6 >> 4)) * q) >> 8;
        const uint32_t b = (fb * p + ((b5 << 3) + (b5 >> 2)) * q) >> 8;
        img[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      }
    }
    return true;
  }

  // PASTES AN RGB565 IMAGE (one layer) STRAIGHT ONTO THE BACK PAGE: row by
  // row with memcpy when the screen is not rotated, pixel by pixel when
  // it is. M5GFX's pushImage passed each pixel through a generic copy
  // routine (~70 ns/pixel: a 720x200 band took 11 ms).
  // 'passo' = pixels per row of the source. The rectangle arrives already clipped.
  bool colar(int x, int y, int w, int h, const uint16_t* src, int passo)
  {
    if (w <= 0 || h <= 0) { return true; }
    if (_write_bits != 16) { return false; }
    anotar(x, y, w, h, true);      // fully covered: nothing comes from the front first
    auto indice = [this](int px, int py) -> int32_t {
      const uint_fast8_t r = _rotation;
      if (r) {
        if ((1u << r) & 0b10010110) { py = _height - (py + 1); }
        if (r & 2)                  { px = _width  - (px + 1); }
        if (r & 1) { std::swap(px, py); }
      }
      return px + py * (int32_t)_bitwidth;
    };
    const int32_t base = indice(x, y);
    const int32_t dx = indice(x + 1, y) - base;
    const int32_t dy = indice(x, y + 1) - base;
    uint16_t* const img = _img.img16();
    for (int j = 0; j < h; ++j) {
      const uint16_t* o = src + (size_t)j * passo;
      uint16_t* d = img + base + j * dy;
      if (dx == 1) { memcpy(d, o, (size_t)w * 2); continue; }
      for (int k = 0; k < w; ++k, d += dx) { *d = o[k]; }
    }
    return true;
  }

private:
  // The whole window, once per setWindow - writeBlock/writePixels may come
  // in several calls for the same window.
  void marcar_janela(void)
  {
    if (_jan_marcada) { return; }
    anotar(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1);
    _jan_marcada = true;
  }
  bool _jan_marcada = false;
  int  _ult_x = 0, _ult_y = 0, _ult_l = 0, _ult_a = 0;
  uint32_t _ult_rodada = 0;
};

// The drawing object: an ordinary LovyanGFX whose panel is the tracker.
class TelaVirtual : public lgfx::LovyanGFX
{
public:
  PainelRastreado painel;

  TelaVirtual(void) { _panel = &painel; }

  // The size and format come from the panel page; the pointer itself changes
  // every frame (PainelRastreado::apontar).
  bool criar(void* pagina)
  {
    // The SAME format as the framebuffer (Panel_DSI forces rgb565_nonswapped).
    setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    painel.setBuffer(pagina, FB_L, FB_A, &_write_conv);
    setRotation(0);   // also sets up the clip to the screen size
    return true;
  }
};

static TelaVirtual* s_tv = nullptr;

bool tela_virtual_iniciar(void)
{
  if (s_tv) { return true; }
  if (!tab5_tela_iniciar()) {
    ESP_LOGE(TAG, "panel without pages - carrying on drawing directly on M5.Display");
    return false;
  }
  s_tv = new TelaVirtual();
  // Starts out pointing at the FRONT: read-only until the first write, which
  // opens the frame and switches to the back one.
  s_tv->criar(tab5_tela_pagina(false));
  // Emoji drawing is a PER-OBJECT screen hook (setEmojiCallback): the
  // physical display's does not apply here. Without copying it, all text drawn by the
  // compositor lost its emoji.
  s_tv->setEmojiCallback(M5.Display.getEmojiCallback());
  tab5_tela_definir_alvo(s_tv);
  ESP_LOGI(TAG, "drawing directly on the hidden page, with no intermediate copy");
  return true;
}

lgfx::LovyanGFX& tela_virtual(void)
{
  if (s_tv) { return *s_tv; }
  return M5.Display;
}

void tela_virtual_seguir_rotacao(void)
{
  if (!s_tv) { return; }
  const uint8_t r = M5.Display.getRotation();
  if (s_tv->getRotation() != r) {
    s_tv->setRotation(r);
    s_tv->painel.esquecer_atalho();
  }
}

void tela_virtual_cobrir(int x, int y, int w, int h)
{
  if (s_tv) { s_tv->painel.anotar(x, y, w, h, true); }
}

bool tela_virtual_misturar_mascara(lgfx::LGFXBase* gfx, int x, int y, int w, int h,
                                   const uint8_t* mascara, int passo, uint32_t cor888)
{
  if (!s_tv || gfx != s_tv) { return false; }
  return s_tv->painel.misturar(x, y, w, h, mascara, passo, cor888);
}

bool tela_virtual_colar(lgfx::LGFXBase* gfx, int x, int y, int w, int h, const uint16_t* src, int passo)
{
  if (!s_tv || gfx != s_tv) { return false; }
  return s_tv->painel.colar(x, y, w, h, src, passo);
}

bool tela_virtual_colar_faixa(const uint16_t* faixa, int y0, int alt)
{
  if (!s_tv || s_tv->getRotation() != 0) { return false; }
  if (!comp_copiar_faixa(faixa, y0, alt)) { return false; }
  // The copy opened the frame: the tracker now points at the back
  // page straight away (the same as in sistema_superficie_mudou).
  s_tv->painel.abrir();
  return true;
}

void tela_virtual_invalidar_tudo(void)
{
  comp_invalidar_tudo();
}

// ---------------------------------------------------------------------------
// DMA SURFACE (kernel/app.h)
// ---------------------------------------------------------------------------
// A 720x1280 image of ITS OWN, allocated on first request and returned when the
// app closes: the app's DMA (the camera's PPA) writes into it at its own pace, and on
// every desenhar() the changed area goes to the back page via DMA2D
// (sistema_superficie_mudou). Before, it was the virtual screen itself - which always
// existed, for everyone; now only those who use it pay, and only while they use it.
static void* s_superficie = nullptr;

bool sistema_superficie(const area_t& r, superficie_t* sup)
{
  if (!s_tv || !sup || s_tv->getRotation() != 0) { return false; }
  if (r.x < 0 || r.y < 0 || r.largura <= 0 || r.altura <= 0
   || r.x + r.largura > FB_L || r.y + r.altura > FB_A) { return false; }
  // FULL WIDTH AND ALIGNED ROWS. The writer's PPA INVALIDATES the
  // cache of whole destination rows (ppa_srm.c, "out_ext_window"),
  // rounded to the 128 B cache line. A row is 1440 B; 8
  // rows = 11520 B = 90 x 128: with edges at multiples of 8, no cache
  // line is shared with a neighbour.
  if (r.x != 0 || r.largura != FB_L || (r.y % 8) != 0 || (r.altura % 8) != 0) {
    ESP_LOGE(TAG, "surface %dx%d at (%d,%d) refused: needs full width and y/height multiples of 8",
             r.largura, r.altura, r.x, r.y);
    return false;
  }
  if (!s_superficie) {
    s_superficie = heap_caps_aligned_calloc(128, 1, (size_t)FB_L * FB_A * 2, MALLOC_CAP_SPIRAM);
    if (!s_superficie) {
      ESP_LOGE(TAG, "no PSRAM for the DMA surface (1.8 MB)");
      return false;
    }
  }
  sup->buf   = s_superficie;
  sup->bytes = (size_t)FB_L * FB_A * 2;
  sup->pic_l = FB_L;
  sup->pic_a = FB_A;
  sup->x     = r.x;
  sup->y     = r.y;
  return true;
}

void sistema_superficie_mudou(const area_t& r)
{
  if (!s_superficie) { return; }
  const tab5_ret_t f = { (int16_t)r.x, (int16_t)r.y, (int16_t)r.largura, (int16_t)r.altura };
  comp_copiar_imagem(s_superficie, f);
  // The copy opened the frame: the tracker now points at the back
  // page straight away, not only on the next write.
  if (s_tv) { s_tv->painel.abrir(); }
}

void tela_virtual_soltar_superficie(void)
{
  if (!s_superficie) { return; }
  heap_caps_free(s_superficie);
  s_superficie = nullptr;
  ESP_LOGI(TAG, "intermediate screen released (1.8 MB of PSRAM returned)");
}
