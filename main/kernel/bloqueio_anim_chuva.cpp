// Lock screen animation: Digital rain: green streaks falling.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 3. DIGITAL RAIN - green streaks falling
// ---------------------------------------------------------------------------
namespace {
struct Coluna { float y, v; int16_t x, len; };
struct Chuva  { Coluna c[18]; };
}

static void chuva_nova(Coluna& c, bool no_topo)
{
  c.v   = frand(260, 620);
  c.len = (int16_t)frand(120, 320);
  c.y   = no_topo ? (s_ay - frand(0, 300)) : frand(s_ay, s_ay + s_H + c.len);
}

void chuva_iniciar(void)
{
  Chuva& s = bq_estado_como<Chuva>();
  for (int i = 0; i < 18; ++i) {
    s.c[i].x = (int16_t)(s_ax + (i * s_W) / 18 + 14);
    chuva_nova(s.c[i], false);
  }
}

void chuva_passo(float dt)
{
  Chuva& s = bq_estado_como<Chuva>();
  for (int i = 0; i < 18; ++i) {
    Coluna& c = s.c[i];
    rec(c.x - 1, (int)(c.y - c.len) - 2, 12, c.len + 14);
    c.y += c.v * dt;
    if (c.y - c.len > s_ay + s_H) { chuva_nova(c, true); }
    rec(c.x - 1, (int)(c.y - c.len) - 2, 12, c.len + 14);
  }
}

void chuva_desenhar(lgfx::LovyanGFX& d)
{
  const Chuva& s = bq_estado_como<Chuva>();
  for (int i = 0; i < 18; ++i) {
    const Coluna& c = s.c[i];
    const int n = c.len / 10;
    for (int k = 0; k < n; ++k) {
      if (k > 0 && ((i * 7 + k * 13) & 3) == 0) { continue; }   // gaps: looks like text
      const int yy = (int)c.y - k * 10 - 8;
      if (yy + 6 < s_ay || yy > s_ay + s_H) { continue; }
      const float f = 1.0f - (float)k / n;
      const int v = 40 + (int)(190 * f * f);
      d.fillRect(c.x, yy, 8, 6, k == 0 ? c565(210, 255, 220) : c565(v >> 4, v, v >> 3));
    }
  }
}

}  // namespace bq_anim
