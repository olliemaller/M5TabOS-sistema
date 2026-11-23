// Lock screen animation: Stars: a journey at light speed.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 4. STARS - a journey at light speed
// ---------------------------------------------------------------------------
namespace {
struct Estrela { float ca, sa, r, k; };
struct Estrelas { Estrela e[40]; };
}

static void estrela_nova(Estrela& e, bool espalhar)
{
  const float ang = frand(0, 6.2832f);
  e.ca = cosf(ang);
  e.sa = sinf(ang);
  e.r  = espalhar ? frand(4, 500) : frand(4, 40);
  e.k  = frand(0.6f, 1.4f);
}

void estrelas_iniciar(void)
{
  for (int i = 0; i < 40; ++i) { estrela_nova(bq_estado_como<Estrelas>().e[i], true); }
}

void estrelas_passo(float dt)
{
  const float cx = s_ax + s_W * 0.5f, cy = s_ay + s_H * 0.5f;
  for (int i = 0; i < 40; ++i) {
    Estrela& e = bq_estado_como<Estrelas>().e[i];
    const float r0 = e.r;
    e.r += (50.0f + r0 * 2.0f) * e.k * dt;
    const float hx = cx + e.ca * e.r, hy = cy + e.sa * e.r;
    const bool fora = hx < s_ax - 8 || hx > s_ax + s_W + 8 || hy < s_ay - 8 || hy > s_ay + s_H + 8;
    rec2(cx + e.ca * r0 * 0.88f, cy + e.sa * r0 * 0.88f, hx, hy, 3);
    if (fora) {
      estrela_nova(e, false);
      rec2(cx + e.ca * e.r * 0.88f, cy + e.sa * e.r * 0.88f, cx + e.ca * e.r, cy + e.sa * e.r, 3);
    }
  }
}

void estrelas_desenhar(lgfx::LovyanGFX& d)
{
  const float cx = s_ax + s_W * 0.5f, cy = s_ay + s_H * 0.5f;
  for (int i = 0; i < 40; ++i) {
    const Estrela& e = bq_estado_como<Estrelas>().e[i];
    const int x1 = (int)(cx + e.ca * e.r), y1 = (int)(cy + e.sa * e.r);
    const int x0 = (int)(cx + e.ca * e.r * 0.88f), y0 = (int)(cy + e.sa * e.r * 0.88f);
    const int v = (int)fminf(255.0f, 70.0f + e.r * 0.8f);
    const uint16_t cor = c565(v * 3 / 4, v * 7 / 8, v);
    d.drawLine(x0, y0, x1, y1, cor);
    if (e.r > 180) { d.drawLine(x0 + 1, y0, x1 + 1, y1, cor); }
    if (e.r > 320) { d.drawLine(x0, y0 + 1, x1, y1 + 1, cor); }
  }
}

}  // namespace bq_anim
