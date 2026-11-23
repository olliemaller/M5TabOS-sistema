#include "tab5_gif.hpp"
#include "tab5_gif_nucleo.hpp"
#include "tab5_imagem.hpp"
#include "tab5_volumes.hpp"

#include "esp_heap_caps.h"
#include <string.h>

#define GIF_ARQUIVO_MAX (16 * 1024 * 1024)

struct tab5_gif {
  uint8_t*      arquivo;     // the whole GIF
  gif_nucleo_t* nucleo;
  uint16_t*     linha;       // one scaled-down row (only if the image shrinks)
  int           linha_cap;
  bool          fundo_pintado;
};

static void* alocar_psram(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

tab5_gif_t* tab5_gif_abrir(const char* caminho)
{
  size_t n = 0;
  uint8_t* arquivo = tab5_volume_ler_tudo(caminho, GIF_ARQUIVO_MAX, &n);
  if (!arquivo) { return nullptr; }
  tab5_gif_t* g = (tab5_gif_t*)heap_caps_calloc(1, sizeof(tab5_gif_t), MALLOC_CAP_SPIRAM);
  if (g) {
    g->arquivo = arquivo;
    g->nucleo = gif_n_abrir(arquivo, n, alocar_psram, heap_caps_free);
  }
  if (!g || !g->nucleo) {
    heap_caps_free(arquivo);
    heap_caps_free(g);
    return nullptr;
  }
  return g;
}

int  tab5_gif_largura(const tab5_gif_t* g)        { return gif_n_largura(g->nucleo); }
int  tab5_gif_altura(const tab5_gif_t* g)         { return gif_n_altura(g->nucleo); }
int  tab5_gif_quadros_vistos(const tab5_gif_t* g) { return gif_n_quadros_vistos(g->nucleo); }
bool tab5_gif_animado(const tab5_gif_t* g)        { return gif_n_animado(g->nucleo); }

int tab5_gif_proximo(tab5_gif_t* g, lgfx::LovyanGFX* destino)
{
  const int atraso = gif_n_proximo(g->nucleo);
  if (atraso < 0) { return -1; }

  const int W = gif_n_largura(g->nucleo), H = gif_n_altura(g->nucleo);
  int l, a;
  tab5_imagem_caber(W, H, destino->width(), destino->height(), &l, &a);
  const int px = (destino->width() - l) / 2, py = (destino->height() - a) / 2;
  if (!g->fundo_pintado) { destino->fillScreen(TFT_BLACK); g->fundo_pintado = true; }

  if (l < W && l > g->linha_cap) {   // row for the scaling down
    heap_caps_free(g->linha);
    g->linha = (uint16_t*)heap_caps_malloc((size_t)l * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    g->linha_cap = g->linha ? l : 0;
    if (!g->linha) { return -1; }
  }
  // The logical screen is natural RGB565: pushImage only understands it with byte swapping.
  const bool troca_ant = destino->getSwapBytes();
  destino->setSwapBytes(true);
  const uint16_t* tela = gif_n_tela(g->nucleo);
  for (int y = 0; y < a; ++y) {
    const uint16_t* origem = tela + (size_t)((int64_t)y * H / a) * W;
    if (l == W) {
      destino->pushImage(px, py + y, l, 1, origem);
    } else {   // nearest neighbour
      for (int x = 0; x < l; ++x) { g->linha[x] = origem[(int64_t)x * W / l]; }
      destino->pushImage(px, py + y, l, 1, g->linha);
    }
  }
  destino->setSwapBytes(troca_ant);
  return atraso;
}

void tab5_gif_fechar(tab5_gif_t* g)
{
  if (!g) { return; }
  gif_n_fechar(g->nucleo);
  heap_caps_free(g->linha);
  heap_caps_free(g->arquivo);
  heap_caps_free(g);
}
