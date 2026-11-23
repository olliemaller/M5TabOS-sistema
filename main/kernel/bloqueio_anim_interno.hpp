// What the bloqueio_anim files share: usable area, randomness, colours,
// list of touched boxes and each animation's entry functions. For them
// only: no other code includes this header (the public API is
// bloqueio_anim.hpp).
//
// bloqueio_anim was split by topic (only files changed, no logic):
//   bloqueio_anim.cpp            animation choice, timing, backgrounds, boxes, the frame and the wallpaper
//   bloqueio_anim_<nome>.cpp     one animation each: balls, fireworks, rain, stars, bubbles,
//                                snake, fireflies, pendulums, snow, comets, solar, logo
#pragma once

#include "bloqueio_anim.hpp"

#include <math.h>
#include <string.h>
#include <stdint.h>

#include "tab5_ui.hpp"
#include "tela_virtual.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// Area, randomness, colours (defined in bloqueio_anim.cpp)
// ---------------------------------------------------------------------------
extern int s_ax, s_ay, s_W, s_H;   // usable area (where the animation may touch)
extern uint32_t s_rng;

static inline uint32_t rnd(void)
{
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}
static inline float frand(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static inline int   irand(int n)            { return (int)(rnd() % (uint32_t)n); }

static inline uint16_t c565(int r, int g, int b)
{
  if (r > 255) { r = 255; } if (g > 255) { g = 255; } if (b > 255) { b = 255; }
  if (r < 0)   { r = 0; }   if (g < 0)   { g = 0; }   if (b < 0)   { b = 0; }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Hue 0..255 (red, yellow, green, cyan, blue, magenta) with brightness v.
uint16_t matiz(uint8_t h, int v);

// ---------------------------------------------------------------------------
// Boxes touched this frame, and background
// ---------------------------------------------------------------------------
extern bool s_tudo;   // draw everything (not just what changed)

void rec(int x, int y, int w, int h);                                  // records a box to erase
void rec2(float x0, float y0, float x1, float y1, int r);              // box covering two points
void fundo_ceu(lgfx::LGFX_Sprite& s, int W, int H);                    // fixed stars (solar background)

// ---------------------------------------------------------------------------
// State of the chosen animation
// ---------------------------------------------------------------------------
// The state of ALL animations shares these bytes: only the chosen one lives. Each
// animation sees the chunk as its own type (bq_estado_como) and the
// static_assert guarantees it fits.
static const size_t BQ_ESTADO_MAX = 4096;
alignas(16) extern uint8_t bq_estado[BQ_ESTADO_MAX];

template <typename T>
static inline T& bq_estado_como(void)
{
  static_assert(sizeof(T) <= BQ_ESTADO_MAX, "estado da animacao maior que bq_estado");
  return *reinterpret_cast<T*>(bq_estado);
}

// ---------------------------------------------------------------------------
// Entry functions of each animation (one per bloqueio_anim_<nome>.cpp file)
// ---------------------------------------------------------------------------
void bolas_iniciar(void);
void bolas_passo(float dt);
void bolas_desenhar(lgfx::LovyanGFX& d);

void fogos_iniciar(void);
void fogos_passo(float dt);
void fogos_desenhar(lgfx::LovyanGFX& d);

void chuva_iniciar(void);
void chuva_passo(float dt);
void chuva_desenhar(lgfx::LovyanGFX& d);

void estrelas_iniciar(void);
void estrelas_passo(float dt);
void estrelas_desenhar(lgfx::LovyanGFX& d);

void bolhas_iniciar(void);
void bolhas_passo(float dt);
void bolhas_desenhar(lgfx::LovyanGFX& d);

void cobra_iniciar(void);
void cobra_passo(float dt);
void cobra_desenhar(lgfx::LovyanGFX& d);

void lumes_iniciar(void);
void lumes_passo(float dt);
void lumes_desenhar(lgfx::LovyanGFX& d);

void pendulos_iniciar(void);
void pendulos_passo(float dt);
void pendulos_desenhar(lgfx::LovyanGFX& d);

void neve_iniciar(void);
void neve_passo(float dt);
void neve_desenhar(lgfx::LovyanGFX& d);

void cometas_iniciar(void);
void cometas_passo(float dt);
void cometas_desenhar(lgfx::LovyanGFX& d);

void solar_iniciar(void);
void solar_passo(float dt);
void solar_desenhar(lgfx::LovyanGFX& d);
void solar_fundo(lgfx::LGFX_Sprite& sp, int W, int H);   // its own background: the starry sky

void logo_iniciar(void);
void logo_passo(float dt);
void logo_desenhar(lgfx::LovyanGFX& d);
void logo_soltar(void);   // frees the painted text

}  // namespace bq_anim
