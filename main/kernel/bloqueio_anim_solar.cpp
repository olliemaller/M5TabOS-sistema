// Lock screen animation: Solar system: a pulsing sun, six planets (one with a moon, one with a ring).
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 11. SOLAR SYSTEM - a pulsing sun, six planets (one with a moon, one with a ring)
// ---------------------------------------------------------------------------
namespace {
static const int N_PLANETA = 6;
struct Planeta { float orb, ang, w, ox, oy; int16_t r; uint16_t cor; };
struct Solar   { Planeta p[N_PLANETA]; float t, lua, lox, loy; int cx, cy; };
}

static const int SOL_R = 30;

void solar_iniciar(void)
{
  Solar& s = bq_estado_como<Solar>();
  s.cx = s_ax + s_W / 2;
  s.cy = s_ay + (int)(s_H * 0.6f);   // below the clock
  float rmax = fminf(s_W * 0.5f, s_H * 0.4f) - 22;
  if (rmax < 120) { rmax = 120; }
  static const float   ORB[N_PLANETA] = { 0.21f, 0.34f, 0.49f, 0.64f, 0.81f, 1.0f };
  static const int16_t RAD[N_PLANETA] = { 5, 8, 9, 6, 13, 10 };
  static const uint8_t COR[N_PLANETA][3] = {
    { 170, 160, 150 }, { 235, 180, 90 }, { 70, 140, 255 }, { 230, 90, 60 }, { 220, 190, 130 }, { 120, 220, 230 } };
  for (int i = 0; i < N_PLANETA; ++i) {
    Planeta& p = s.p[i];
    p.orb = rmax * ORB[i];
    if (p.orb < SOL_R + 16 + RAD[i]) { p.orb = SOL_R + 16 + RAD[i]; }
    p.ang = frand(0, 6.2832f);
    p.w   = 1.4f * powf(70.0f / p.orb, 1.5f);   // Kepler: further out is slower
    p.r   = RAD[i];
    p.cor = c565(COR[i][0], COR[i][1], COR[i][2]);
    p.ox  = s.cx + cosf(p.ang) * p.orb;
    p.oy  = s.cy + sinf(p.ang) * p.orb;
  }
  s.t = 0;
  s.lua = 0;
  s.lox = s.p[2].ox; s.loy = s.p[2].oy;
}

void solar_fundo(lgfx::LGFX_Sprite& sp, int W, int H)
{
  fundo_ceu(sp, W, H);
  const Solar& s = bq_estado_como<Solar>();
  for (int i = 0; i < N_PLANETA; ++i) {
    sp.drawCircle(s.cx - s_ax, s.cy - s_ay, (int)s.p[i].orb, c565(38, 40, 58));
  }
}

void solar_passo(float dt)
{
  Solar& s = bq_estado_como<Solar>();
  s.t += dt;
  // The sun pulses: its box (with the corona) is redone every frame.
  rec(s.cx - SOL_R - 7, s.cy - SOL_R - 7, 2 * SOL_R + 15, 2 * SOL_R + 15);
  for (int i = 0; i < N_PLANETA; ++i) {
    Planeta& p = s.p[i];
    p.ang += p.w * dt;
    if (p.ang > 6.2832f) { p.ang -= 6.2832f; }
    const float x = s.cx + cosf(p.ang) * p.orb, y = s.cy + sinf(p.ang) * p.orb;
    const int folga = p.r + (i == 4 ? 12 : 2);   // the ring extends beyond the planet
    rec2(p.ox, p.oy, x, y, folga);
    p.ox = x; p.oy = y;
  }
  // Earth's moon (planet 2).
  s.lua += 3.2f * dt;
  const float lx = s.p[2].ox + cosf(s.lua) * 18, ly = s.p[2].oy + sinf(s.lua) * 18;
  rec2(s.lox, s.loy, lx, ly, 5);
  s.lox = lx; s.loy = ly;
}

void solar_desenhar(lgfx::LovyanGFX& d)
{
  const Solar& s = bq_estado_como<Solar>();
  const int pulso = (int)(2.5f * sinf(s.t * 2.2f));
  d.fillCircle(s.cx, s.cy, SOL_R + 5 + pulso, c565(110, 40, 0));
  d.fillCircle(s.cx, s.cy, SOL_R + pulso, c565(255, 150, 30));
  d.fillCircle(s.cx, s.cy, SOL_R * 6 / 10, c565(255, 225, 120));
  for (int i = 0; i < N_PLANETA; ++i) {
    const Planeta& p = s.p[i];
    const int x = (int)p.ox, y = (int)p.oy;
    if (i == 4) { d.drawEllipse(x, y, p.r + 9, 4, c565(170, 150, 110)); }
    d.fillCircle(x, y, p.r, p.cor);
    if (i == 4) { d.drawFastHLine(x - p.r + 2, y, 2 * p.r - 4, c565(170, 150, 110)); }
  }
  d.fillCircle((int)s.lox, (int)s.loy, 3, c565(200, 200, 205));
}

}  // namespace bq_anim
