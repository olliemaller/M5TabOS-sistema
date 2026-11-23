// Lock screen animation: Bubbles rising and swaying.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 5. BUBBLES - rise while swaying
// ---------------------------------------------------------------------------
namespace {
struct Bolha  { float x0, y, r, vy, fase, fv, amp, ox, oy; uint8_t h; };
struct Bolhas { Bolha b[14]; };
}

static void bolha_nova(Bolha& b, bool espalhar)
{
  b.r    = frand(10, 40);
  b.amp  = frand(14, 30);
  b.x0   = frand(s_ax + b.amp + b.r, s_ax + s_W - b.amp - b.r);
  b.y    = espalhar ? frand(s_ay, s_ay + s_H) : (s_ay + s_H + b.r + frand(0, 200));
  b.vy   = frand(40, 110);
  b.fase = frand(0, 6.2832f);
  b.fv   = frand(1.0f, 2.2f);
  b.h    = (uint8_t)(110 + irand(60));   // from green to blue, with a touch of purple
  b.ox   = b.x0; b.oy = b.y;
}

void bolhas_iniciar(void)
{
  for (int i = 0; i < 14; ++i) { bolha_nova(bq_estado_como<Bolhas>().b[i], true); }
}

void bolhas_passo(float dt)
{
  for (int i = 0; i < 14; ++i) {
    Bolha& b = bq_estado_como<Bolhas>().b[i];
    const float ox = b.x0 + b.amp * sinf(b.fase), oy = b.y;
    b.fase += b.fv * dt;
    b.y    -= b.vy * dt;
    const float nx = b.x0 + b.amp * sinf(b.fase);
    rec2(ox, oy, nx, b.y, (int)b.r + 2);
    if (b.y + b.r < s_ay) { bolha_nova(b, false); }
  }
}

void bolhas_desenhar(lgfx::LovyanGFX& d)
{
  for (int i = 0; i < 14; ++i) {
    const Bolha& b = bq_estado_como<Bolhas>().b[i];
    const int x = (int)(b.x0 + b.amp * sinf(b.fase)), y = (int)b.y, r = (int)b.r;
    const uint16_t cor = matiz(b.h, 210);
    d.drawCircle(x, y, r, cor);
    d.drawCircle(x, y, r - 1, cor);
    d.fillCircle(x - (int)(b.r * 0.45f), y - (int)(b.r * 0.45f), (int)fmaxf(2.0f, b.r * 0.12f),
                 c565(235, 245, 255));
  }
}

}  // namespace bq_anim
