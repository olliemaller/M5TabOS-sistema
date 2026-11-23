#include "tab5_tela.hpp"

#include "tab5_painel_dsi_interno.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "tela";

static esp_lcd_panel_handle_t s_handle = nullptr;   // panel we were prepared with
static void*     s_fb[2]     = { nullptr, nullptr };
static uint8_t** s_linhas[2] = { nullptr, nullptr };
static uint8_t** s_nosso     = nullptr;   // the array we ALLOCATED (the other one is M5GFX's)
static int       s_largura   = 0;
static int       s_altura    = 0;
static int       s_frente    = 0;         // which page the DSI is scanning
static bool      s_pronto    = false;
static uint32_t  s_geracao   = 0;
std::atomic<uint32_t> g_tab5_tela_epoca{0};

static lgfx::LovyanGFX* s_alvo = nullptr;
static inline lgfx::LovyanGFX& alvo(void) { return s_alvo ? *s_alvo : (lgfx::LovyanGFX&)M5.Display; }

void tab5_tela_definir_alvo(lgfx::LovyanGFX* d) { s_alvo = d; }
uint32_t tab5_tela_geracao(void) { return s_geracao; }

// ---------------------------------------------------------------------------
// THE PAGES
// ---------------------------------------------------------------------------
// M5GFX only knows fb0: its _lines_buffer is an array of line pointers
// into it. We build a twin for fb1 and swap which one M5GFX uses on
// every page switch - so M5.Display always points at the VISIBLE
// page. Nobody should draw on it (the target is the virtual screen), but if
// something does, it lands where it can be seen, and not on a page the compositor will
// overwrite without warning.
void tab5_tela_encerrar(void)
{
  auto* p = tab5_painel_dsi_interno();
  if (p && s_linhas[0]) {
    // ALWAYS returns M5GFX's ORIGINAL array. tab5_power's desmontar_tela()
    // frees whatever is in _lines_buffer; if it were OURS,
    // we would free the same block twice (the double free).
    p->trocar_lines(s_linhas[0]);
  }
  if (s_nosso) { heap_caps_free(s_nosso); s_nosso = nullptr; }
  tab5_quadro_encerrar();
  s_linhas[0] = s_linhas[1] = nullptr;
  s_fb[0] = s_fb[1] = nullptr;
  s_handle = nullptr;
  s_frente = 0;
  s_pronto = false;
  ++g_tab5_tela_epoca;
}

bool tab5_tela_iniciar(void)
{
  auto* p = tab5_painel_dsi_interno();
  if (p == nullptr) { return false; }

  esp_lcd_panel_handle_t h = p->pegar_handle();
  if (h == nullptr) { return false; }

  // Same panel and already prepared: nothing to do. A DIFFERENT panel = idle
  // (or a clock change) destroyed and rebuilt the DSI.
  if (s_pronto && h == s_handle) { return true; }
  if (s_pronto) { tab5_tela_encerrar(); }

  void* fb0 = nullptr;
  void* fb1 = nullptr;
  if (esp_lcd_dpi_panel_get_frame_buffer(h, 2, &fb0, &fb1) != ESP_OK
   || fb0 == nullptr || fb1 == nullptr) {
    ESP_LOGE(TAG, "the panel does not have two pages");
    return false;
  }

  uint8_t** doM5 = p->pegar_lines();
  if (doM5 == nullptr) { return false; }

  const int altura = p->altura();
  // Same formula as Panel_DSI::init - RGB565, line aligned to 4 bytes.
  const size_t passo = ((p->largura() * 16 >> 3) + 3) & ~3u;

  // Array of line POINTERS: only the CPU reads it (to find where to draw
  // line y); the DSI DMA reads the framebuffer, never this array. That is why it goes
  // to PSRAM - 5KB (1280 pointers) taken out of internal SRAM.
  uint8_t** meu = (uint8_t**)heap_caps_malloc(altura * sizeof(uint8_t*), MALLOC_CAP_SPIRAM);
  if (meu == nullptr) {
    ESP_LOGE(TAG, "out of memory for the line array (%d pointers)", altura);
    return false;
  }
  uint8_t* linha = (uint8_t*)fb1;
  for (int y = 0; y < altura; ++y) { meu[y] = linha; linha += passo; }

  s_handle    = h;
  s_fb[0]     = fb0;
  s_fb[1]     = fb1;
  s_largura   = p->largura();
  s_altura    = altura;
  s_linhas[0] = doM5;
  s_linhas[1] = meu;
  s_nosso     = meu;
  s_frente    = 0;
  s_pronto    = true;
  ++s_geracao;
  ++g_tab5_tela_epoca;

  ESP_LOGI(TAG, "display double-buffered (draws on one page, shows the other)");
  tab5_quadro_iniciar(h);
  return true;
}

void* tab5_tela_pagina(bool tras)
{
  if (!tab5_tela_iniciar()) { return nullptr; }
  return s_fb[tras ? 1 - s_frente : s_frente];
}

bool tab5_tela_pedir_troca(int y0, int y1)
{
  auto* p = tab5_painel_dsi_interno();
  if (!s_pronto || p == nullptr || y1 <= y0) { return false; }
  const int atras = 1 - s_frente;
  // The pointer is the framebuffer itself: the driver only changes the index, which takes
  // effect at the next end of frame.
  if (esp_lcd_panel_draw_bitmap(s_handle, 0, y0, s_largura, y1, s_fb[atras]) != ESP_OK) { return false; }
  tab5_quadro_armar_espera();
  s_frente = atras;
  p->trocar_lines(s_linhas[s_frente]);
  return true;
}

// ---------------------------------------------------------------------------
// REGION KNOWN TO BE CLEAR
// ---------------------------------------------------------------------------
// On a full repaint the system clears the work area and right afterwards the app
// asks for the same clear. The rule for skipping it lives HERE, not in each app:
// the app always asks, unconditionally, and is correct by default.
//
// Several regions (not just one) because a single pass clears separate areas with
// different colours - work area, bottom bar, top frame.
#define LIM_MAX 4
static struct { int x, y, l, a; uint16_t cor; } s_lim[LIM_MAX];
static int s_n_lim = 0;
static uint32_t s_lim_evitadas = 0;

void tab5_tela_marcar_limpo(int x, int y, int largura, int altura, uint16_t cor)
{
  if (s_n_lim >= LIM_MAX) { return; }
  s_lim[s_n_lim++] = { x, y, largura, altura, cor };
}

void tab5_tela_esquecer_limpo(void) { s_n_lim = 0; }
uint32_t tab5_tela_limpezas_evitadas(void) { return s_lim_evitadas; }

static bool ja_esta_limpo(int x, int y, int l, int a, uint16_t cor)
{
  for (int i = 0; i < s_n_lim; ++i) {
    if (cor != s_lim[i].cor) { continue; }
    if (x >= s_lim[i].x && y >= s_lim[i].y
     && (x + l) <= (s_lim[i].x + s_lim[i].l)
     && (y + a) <= (s_lim[i].y + s_lim[i].a)) { return true; }
  }
  return false;
}

void tab5_tela_limpar(int x, int y, int largura, int altura, uint16_t cor)
{
  if (largura <= 0 || altura <= 0) { return; }
  if (ja_esta_limpo(x, y, largura, altura, cor)) { ++s_lim_evitadas; return; }
  alvo().fillRect(x, y, largura, altura, cor);
}

// ---------------------------------------------------------------------------
// SCROLLING - reuse pixels instead of redrawing
// ---------------------------------------------------------------------------
// A single copyRect of the whole area: the virtual screen isn't scanned by anyone,
// so there are no bursts to space out and no beam to beat. Panel_Sprite copies line
// by line in the right order for overlapping source and destination.
static uint64_t s_rol_px = 0;
static uint64_t s_rol_us = 0;

bool tab5_tela_rolar(int x, int y, int largura, int altura, int delta, uint16_t cor)
{
  if (largura <= 0 || altura <= 0 || delta == 0) { return false; }
  const int ad = (delta < 0) ? -delta : delta;
  if (ad >= altura) { return false; }

  auto& d = alvo();
  const int h = altura - ad;
  const int64_t t0 = esp_timer_get_time();
  if (delta > 0) {
    d.copyRect(x, y, largura, h, x, y + delta);   // content moves up
    d.fillRect(x, y + h, largura, ad, cor);       // new strip at the bottom
  } else {
    d.copyRect(x, y + ad, largura, h, x, y);      // content moves down
    d.fillRect(x, y, largura, ad, cor);           // new strip at the top
  }
  s_rol_us += (uint64_t)(esp_timer_get_time() - t0);
  s_rol_px += (uint64_t)largura * h;
  return true;
}

void tab5_tela_rolagem_custo(uint64_t* px, uint64_t* us)
{
  if (px) { *px = s_rol_px; }
  if (us) { *us = s_rol_us; }
}
