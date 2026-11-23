// Lock screen animation: Fireworks: a rocket rises, bursts into particles that fall and fade.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 2. FIREWORKS - a rocket rises, bursts into particles that fall and fade
// ---------------------------------------------------------------------------
namespace {
static const int N_FOGUETES = 3;
static const int N_PART     = 80;
struct Foguete { float x, y, vy, alvo, oy; bool on; };
// crep: when fading, releases a little shower of small sparks (crackle).
struct Part    { float x, y, vx, vy, vida, vida0, ox, oy; uint8_t h; bool on, crep; };
struct Fogos   { Foguete f[N_FOGUETES]; Part p[N_PART]; float espera; };
}

void fogos_iniciar(void)
{
  Fogos& s = bq_estado_como<Fogos>();
  memset(&s, 0, sizeof(s));
  // Starts with a rocket already halfway up: without this the screen stays empty for
  // about 1 s until the first one rises and bursts.
  Foguete& f = s.f[0];
  f.x = frand(s_ax + s_W * 0.25f, s_ax + s_W * 0.75f);
  f.y = f.oy = s_ay + s_H * 0.6f;
  f.vy = -520;
  f.alvo = s_ay + s_H * 0.3f;
  f.on = true;
  s.espera = 0.4f;
}

// Releases 'n' particles from (x, y). Ring: all at the same speed.
static int fogos_soltar(Fogos& s, float x, float y, uint8_t h, int n, float vmin, float vmax,
                        float vida_min, float vida_max, bool anel, bool crep)
{
  int k = 0;
  for (int i = 0; i < N_PART && k < n; ++i) {
    Part& p = s.p[i];
    if (p.on) { continue; }
    const float ang = 6.2832f * k / n + frand(-0.1f, 0.1f);
    const float vel = anel ? vmax : frand(vmin, vmax);
    p.x = p.ox = x;  p.y = p.oy = y;
    p.vx = cosf(ang) * vel;
    p.vy = sinf(ang) * vel;
    p.vida = p.vida0 = frand(vida_min, vida_max);
    p.h  = (irand(4) == 0) ? (uint8_t)(h + 40) : h;
    p.crep = crep;
    p.on = true;
    ++k;
  }
  return k;
}

// Layered burst: core + outer ring (+ crackling bursts).
static void fogos_explodir(Fogos& s, float x, float y)
{
  const uint8_t h = (uint8_t)irand(256);
  switch (irand(3)) {
    case 0:   // double ring
      fogos_soltar(s, x, y, h, 22, 0, 330.0f, 1.0f, 1.5f, true, false);
      fogos_soltar(s, x, y, (uint8_t)(h + 85), 14, 0, 190.0f, 0.9f, 1.3f, true, false);
      break;
    case 1:   // golden rain: keeps falling and crackling
      fogos_soltar(s, x, y, 40, 30, 100, 360, 1.3f, 2.0f, false, true);
      break;
    default:  // full sphere, some crackle
      fogos_soltar(s, x, y, h, 24, 80, 360, 1.0f, 1.6f, false, true);
      fogos_soltar(s, x, y, (uint8_t)(h + 30), 10, 40, 170, 0.8f, 1.2f, false, false);
      break;
  }
}

void fogos_passo(float dt)
{
  Fogos& s = bq_estado_como<Fogos>();
  s.espera -= dt;
  if (s.espera <= 0) {
    for (int i = 0; i < N_FOGUETES; ++i) {
      Foguete& f = s.f[i];
      if (f.on) { continue; }
      f.x = frand(s_ax + s_W * 0.15f, s_ax + s_W * 0.85f);
      f.y = f.oy = (float)(s_ay + s_H);
      f.vy = -frand(700, 950);
      f.alvo = s_ay + s_H * frand(0.15f, 0.55f);
      f.on = true;
      break;
    }
    s.espera = frand(0.3f, 1.0f);
  }
  for (int i = 0; i < N_FOGUETES; ++i) {
    Foguete& f = s.f[i];
    if (!f.on) { continue; }
    f.oy = f.y;
    f.y += f.vy * dt;
    f.vy += 250 * dt;   // loses power on the way up
    if (f.y <= f.alvo || f.vy > -120) {
      f.on = false;
      rec(f.x - 3, f.oy - 3, 7, 30);
      rec(f.x - 3, f.y - 3, 7, 30);
      fogos_explodir(s, f.x, f.y);
    } else {
      rec((int)f.x - 3, (int)f.y - 3, 7, (int)(f.oy - f.y) + 30);
    }
  }
  for (int i = 0; i < N_PART; ++i) {
    Part& p = s.p[i];
    if (!p.on) { continue; }
    p.ox = p.x; p.oy = p.y;
    p.vida -= dt;
    if (p.vida <= 0) {
      p.on = false;
      rec2(p.ox, p.oy, p.ox, p.oy, 3);
      if (p.crep) {   // crackle: small, short sparks
        fogos_soltar(s, p.x, p.y, p.h, 4, 30, 120, 0.3f, 0.6f, false, false);
      }
      continue;
    }
    p.vy += 320 * dt;
    const float k = 1.0f - 1.4f * dt;
    p.vx *= k; p.vy *= k;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    rec2(p.ox, p.oy, p.x, p.y, 3);
  }
}

void fogos_desenhar(lgfx::LovyanGFX& d)
{
  const Fogos& s = bq_estado_como<Fogos>();
  for (int i = 0; i < N_FOGUETES; ++i) {
    const Foguete& f = s.f[i];
    if (!f.on) { continue; }
    d.fillRect((int)f.x - 1, (int)f.y + 4, 3, 22, c565(255, 170, 70));
    d.fillRect((int)f.x - 2, (int)f.y - 2, 4, 6, c565(255, 245, 220));
  }
  for (int i = 0; i < N_PART; ++i) {
    const Part& p = s.p[i];
    if (!p.on) { continue; }
    const float f = p.vida / p.vida0;
    const int sz = f > 0.6f ? 4 : (f > 0.25f ? 3 : 2);
    const uint16_t cor = f > 0.85f ? c565(255, 250, 235) : matiz(p.h, (int)(255 * f * f));
    d.fillRect((int)p.x - sz / 2, (int)p.y - sz / 2, sz, sz, cor);
  }
}

}  // namespace bq_anim
