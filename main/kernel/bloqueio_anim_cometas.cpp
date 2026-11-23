// Lock screen animation: Comets with tails, each on a Lissajous curve.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 10. COMETS - three comets with tails, each on a Lissajous curve
// ---------------------------------------------------------------------------
namespace {
static const int N_COMETA = 3;
static const int N_CAUDA  = 12;
struct Cometa  { float a, b, fase, vel, t, rx, ry; float ox[N_CAUDA], oy[N_CAUDA]; uint8_t h; };
struct Cometas { Cometa c[N_COMETA]; };
}

static const float CAUDA_PASSO = 0.035f;   // "time" between two points of the tail

static void cometa_pos(const Cometa& c, float t, float* x, float* y)
{
  *x = s_ax + s_W * 0.5f + c.rx * sinf(c.a * t + c.fase);
  *y = s_ay + s_H * 0.5f + c.ry * sinf(c.b * t);
}

static int cauda_raio(int k) { return k < 2 ? 6 - k : (k < 6 ? 3 : 2); }

void cometas_iniciar(void)
{
  static const uint8_t PARES[][2] = { { 3, 2 }, { 5, 4 }, { 4, 3 }, { 2, 3 }, { 5, 3 }, { 3, 4 } };
  for (int i = 0; i < N_COMETA; ++i) {
    Cometa& c = bq_estado_como<Cometas>().c[i];
    const int p = irand(6);
    c.a    = PARES[p][0];
    c.b    = PARES[p][1];
    c.fase = frand(0, 6.2832f);
    c.vel  = frand(0.22f, 0.34f);
    c.t    = frand(0, 60);
    c.rx   = s_W * 0.5f - 24;
    c.ry   = s_H * 0.5f - 24;
    c.h    = (uint8_t)(i * 85 + irand(30));
    for (int k = 0; k < N_CAUDA; ++k) { cometa_pos(c, c.t - k * CAUDA_PASSO, &c.ox[k], &c.oy[k]); }
  }
}

void cometas_passo(float dt)
{
  for (int i = 0; i < N_COMETA; ++i) {
    Cometa& c = bq_estado_como<Cometas>().c[i];
    c.t    += c.vel * dt;
    c.fase += 0.02f * dt;   // the curve changes slowly
    for (int k = 0; k < N_CAUDA; ++k) {
      float x, y;
      cometa_pos(c, c.t - k * CAUDA_PASSO, &x, &y);
      rec2(c.ox[k], c.oy[k], x, y, cauda_raio(k) + 2);
      c.ox[k] = x; c.oy[k] = y;
    }
  }
}

void cometas_desenhar(lgfx::LovyanGFX& d)
{
  for (int i = 0; i < N_COMETA; ++i) {
    const Cometa& c = bq_estado_como<Cometas>().c[i];
    for (int k = N_CAUDA - 1; k >= 0; --k) {   // the head last, on top
      const int x = (int)c.ox[k], y = (int)c.oy[k], r = cauda_raio(k);
      const uint16_t cor = k == 0 ? c565(255, 250, 235)
                                  : matiz((uint8_t)(c.h + k * 3), 250 - k * 17);
      if (r >= 3) { d.fillCircle(x, y, r, cor); }
      else        { d.fillRect(x - r, y - r, 2 * r, 2 * r, cor); }
    }
  }
}

}  // namespace bq_anim
