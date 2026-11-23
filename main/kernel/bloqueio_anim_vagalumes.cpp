// Lock screen animation: Fireflies glowing and fading as they wander.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 7. FIREFLIES - glow and fade, wandering
// ---------------------------------------------------------------------------
namespace {
struct Lume  { float x, y, ang, vel, fase, fv, ox, oy; uint8_t h; };
struct Lumes { Lume l[16]; };
}

void lumes_iniciar(void)
{
  for (int i = 0; i < 16; ++i) {
    Lume& l = bq_estado_como<Lumes>().l[i];
    l.x = frand(s_ax + 20, s_ax + s_W - 20);
    l.y = frand(s_ay + 20, s_ay + s_H - 20);
    l.ang = frand(0, 6.2832f);
    l.vel = frand(30, 75);
    l.fase = frand(0, 6.2832f);
    l.fv = frand(1.2f, 2.8f);
    l.h = (uint8_t)(38 + irand(40));   // yellow to lime green
    l.ox = l.x; l.oy = l.y;
  }
}

void lumes_passo(float dt)
{
  for (int i = 0; i < 16; ++i) {
    Lume& l = bq_estado_como<Lumes>().l[i];
    l.ox = l.x; l.oy = l.y;
    l.ang += frand(-4, 4) * dt;
    l.x += cosf(l.ang) * l.vel * dt;
    l.y += sinf(l.ang) * l.vel * dt;
    if (l.x < s_ax + 12)           { l.x = s_ax + 12;           l.ang = 3.1416f - l.ang; }
    else if (l.x > s_ax + s_W - 12) { l.x = s_ax + s_W - 12;     l.ang = 3.1416f - l.ang; }
    if (l.y < s_ay + 12)           { l.y = s_ay + 12;           l.ang = -l.ang; }
    else if (l.y > s_ay + s_H - 12) { l.y = s_ay + s_H - 12;     l.ang = -l.ang; }
    l.fase += l.fv * dt;
    rec2(l.ox, l.oy, l.x, l.y, 10);
  }
}

void lumes_desenhar(lgfx::LovyanGFX& d)
{
  for (int i = 0; i < 16; ++i) {
    const Lume& l = bq_estado_como<Lumes>().l[i];
    float b = 0.5f + 0.5f * sinf(l.fase);
    b *= b;
    const int v = (int)(255 * b);
    if (v < 10) { continue; }
    d.fillCircle((int)l.x, (int)l.y, 8, matiz(l.h, v * 18 / 100));
    d.fillCircle((int)l.x, (int)l.y, 5, matiz(l.h, v * 45 / 100));
    d.fillCircle((int)l.x, (int)l.y, 2, c565(v, v, v * 6 / 10));
  }
}

}  // namespace bq_anim
