// Lock screen animation: Balls bouncing off the walls and each other (elastic collision).
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 1. BALLS - bounce off the walls and each other (elastic collision)
// ---------------------------------------------------------------------------
namespace {
struct Bola { float x, y, vx, vy, r, ox, oy; uint16_t cor; };
struct Bolas { Bola b[7]; };
}

void bolas_iniciar(void)
{
  Bolas& s = bq_estado_como<Bolas>();
  for (int i = 0; i < 7; ++i) {
    Bola& b = s.b[i];
    b.r  = (float)(16 + irand(22));
    b.x  = frand(s_ax + b.r, s_ax + s_W - b.r);
    b.y  = frand(s_ay + b.r, s_ay + s_H - b.r);
    const float v = frand(240, 420), ang = frand(0, 6.2832f);
    b.vx = cosf(ang) * v;
    b.vy = sinf(ang) * v;
    b.cor = matiz((uint8_t)(i * 37 + irand(16)), 225);
    b.ox = b.x; b.oy = b.y;
  }
}

void bolas_passo(float dt)
{
  Bolas& s = bq_estado_como<Bolas>();
  for (int i = 0; i < 7; ++i) {
    Bola& b = s.b[i];
    b.ox = b.x; b.oy = b.y;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < s_ax + b.r)            { b.x = s_ax + b.r;            b.vx =  fabsf(b.vx); }
    else if (b.x > s_ax + s_W - b.r) { b.x = s_ax + s_W - b.r;      b.vx = -fabsf(b.vx); }
    if (b.y < s_ay + b.r)            { b.y = s_ay + b.r;            b.vy =  fabsf(b.vy); }
    else if (b.y > s_ay + s_H - b.r) { b.y = s_ay + s_H - b.r;      b.vy = -fabsf(b.vy); }
  }
  // Elastic collision; mass goes with area (r^2).
  for (int i = 0; i < 7; ++i) {
    for (int j = i + 1; j < 7; ++j) {
      Bola& a = s.b[i];
      Bola& c = s.b[j];
      const float dx = c.x - a.x, dy = c.y - a.y;
      const float d2 = dx * dx + dy * dy, rr = a.r + c.r;
      if (d2 >= rr * rr || d2 < 0.01f) { continue; }
      const float d = sqrtf(d2), nx = dx / d, ny = dy / d;
      const float ma = a.r * a.r, mc = c.r * c.r, tot = ma + mc;
      const float pen = rr - d;
      a.x -= nx * pen * mc / tot;  a.y -= ny * pen * mc / tot;
      c.x += nx * pen * ma / tot;  c.y += ny * pen * ma / tot;
      const float p = (a.vx - c.vx) * nx + (a.vy - c.vy) * ny;
      if (p > 0) {
        a.vx -= 2 * mc / tot * p * nx;  a.vy -= 2 * mc / tot * p * ny;
        c.vx += 2 * ma / tot * p * nx;  c.vy += 2 * ma / tot * p * ny;
      }
    }
  }
  for (int i = 0; i < 7; ++i) {
    const Bola& b = s.b[i];
    rec2(b.ox, b.oy, b.x, b.y, (int)b.r + 2);
  }
}

void bolas_desenhar(lgfx::LovyanGFX& d)
{
  const Bolas& s = bq_estado_como<Bolas>();
  for (int i = 0; i < 7; ++i) {
    const Bola& b = s.b[i];
    d.fillCircle((int)b.x, (int)b.y, (int)b.r, b.cor);
    const int hr = (int)fmaxf(2.0f, b.r * 0.22f);
    d.fillCircle((int)(b.x - b.r * 0.35f), (int)(b.y - b.r * 0.35f), hr, c565(235, 235, 235));
  }
}

}  // namespace bq_anim
