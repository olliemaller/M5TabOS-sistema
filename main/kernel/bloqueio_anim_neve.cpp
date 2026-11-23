// Lock screen animation: Snow: flakes of three sizes and a wind that comes and goes.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 9. SNOW - flakes of three sizes (the small, distant ones fall slowly) and a
//    wind that comes and goes
// ---------------------------------------------------------------------------
namespace {
static const int N_FLOCO = 46;
struct Floco { float x, y, vy, fase, ox, oy; uint8_t tam; };
struct Neve  { Floco f[N_FLOCO]; float tv; };
}

static void floco_novo(Floco& f, bool espalhar)
{
  const int r = irand(10);
  f.tam  = (uint8_t)(r < 5 ? 1 : (r < 8 ? 2 : 3));   // many small, few large
  f.vy   = frand(30, 50) * f.tam;
  f.x    = frand(s_ax, s_ax + s_W);
  f.y    = espalhar ? frand(s_ay, s_ay + s_H) : s_ay - frand(4, 60);
  f.fase = frand(0, 6.2832f);
  f.ox = f.x; f.oy = f.y;
}

void neve_iniciar(void)
{
  Neve& s = bq_estado_como<Neve>();
  for (int i = 0; i < N_FLOCO; ++i) { floco_novo(s.f[i], true); }
  s.tv = frand(0, 20);
}

void neve_passo(float dt)
{
  Neve& s = bq_estado_como<Neve>();
  s.tv += dt;
  const float vento = 45.0f * sinf(s.tv * 0.23f) + 15.0f * sinf(s.tv * 0.71f);
  for (int i = 0; i < N_FLOCO; ++i) {
    Floco& f = s.f[i];
    const float ox = f.x, oy = f.y;
    f.fase += 1.7f * dt;
    f.x += (vento * f.tam / 3.0f + 18.0f * sinf(f.fase)) * dt;
    f.y += f.vy * dt;
    bool pulou = false;
    if (f.x < s_ax - 4)            { f.x += s_W + 8; pulou = true; }
    else if (f.x > s_ax + s_W + 4) { f.x -= s_W + 8; pulou = true; }
    if (f.y > s_ay + s_H + 4) {
      rec2(ox, oy, ox, oy, 4);
      floco_novo(f, false);
      continue;
    }
    if (pulou) { rec2(ox, oy, ox, oy, 4); rec2(f.x, f.y, f.x, f.y, 4); }
    else       { rec2(ox, oy, f.x, f.y, 4); }
  }
}

void neve_desenhar(lgfx::LovyanGFX& d)
{
  const Neve& s = bq_estado_como<Neve>();
  for (int i = 0; i < N_FLOCO; ++i) {
    const Floco& f = s.f[i];
    const int x = (int)f.x, y = (int)f.y;
    switch (f.tam) {
      case 1:
        d.fillRect(x, y, 2, 2, c565(150, 160, 185));
        break;
      case 2: {
        const uint16_t c = c565(200, 215, 240);
        d.fillRect(x - 2, y, 5, 1, c);
        d.fillRect(x, y - 2, 1, 5, c);
      } break;
      default: {
        const uint16_t c = c565(235, 245, 255);
        d.fillRect(x - 3, y, 7, 1, c);
        d.fillRect(x, y - 3, 1, 7, c);
        d.drawLine(x - 2, y - 2, x + 2, y + 2, c);
        d.drawLine(x - 2, y + 2, x + 2, y - 2, c);
      } break;
    }
  }
}

}  // namespace bq_anim
