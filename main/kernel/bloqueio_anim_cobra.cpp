// Lock screen animation: a little snake that plays by itself (tribute to CreazySnake).
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 6. LITTLE SNAKE - plays by itself (tribute to CreazySnake)
// ---------------------------------------------------------------------------
namespace {
static const int CEL = 40;   // cell side, px
struct Cobra {
  uint8_t sx[64], sy[64];    // ring: [cab] is the head, [cab-1] the neck...
  uint8_t occ[800];          // gx*gy: 1 = body
  int     gx, gy, ox, oy;    // grid and origin on screen
  int     cab, len;
  int     fx, fy;            // food
  int     dx, dy;            // direction
  float   acum;
  bool    passou;            // took a step this frame
};
}

static inline int cobra_i(const Cobra& s, int k) { return (s.cab - k) & 63; }   // k-th segment from the head

static void cobra_comida(Cobra& s)
{
  for (int tent = 0; tent < 200; ++tent) {
    const int x = irand(s.gx), y = irand(s.gy);
    if (!s.occ[y * s.gx + x]) { s.fx = x; s.fy = y; rec(s.ox + x * CEL, s.oy + y * CEL, CEL, CEL); return; }
  }
  s.fx = s.fy = 0;
}

static void cobra_reiniciar(Cobra& s)
{
  // Erases what is still on screen, cell by cell (no clearing the area).
  if (s.len > 0) {
    for (int k = 0; k < s.len; ++k) {
      const int i = cobra_i(s, k);
      rec(s.ox + s.sx[i] * CEL, s.oy + s.sy[i] * CEL, CEL, CEL);
    }
    rec(s.ox + s.fx * CEL, s.oy + s.fy * CEL, CEL, CEL);
  }
  memset(s.occ, 0, sizeof(s.occ));
  s.len = 4;
  s.dx = 1; s.dy = 0;
  const int hx = s.gx / 2, hy = s.gy / 2;
  s.cab = 3;
  for (int k = 0; k < 4; ++k) {
    const int i = cobra_i(s, k);
    s.sx[i] = (uint8_t)(hx - k);
    s.sy[i] = (uint8_t)hy;
    s.occ[hy * s.gx + hx - k] = 1;
  }
  cobra_comida(s);
  s.acum = 0;
  s_tudo = true;
}

void cobra_iniciar(void)
{
  Cobra& s = bq_estado_como<Cobra>();
  memset(&s, 0, sizeof(s));
  s.gx = s_W / CEL;
  s.gy = s_H / CEL;
  if (s.gx * s.gy > (int)sizeof(s.occ)) { s.gy = (int)sizeof(s.occ) / s.gx; }
  s.ox = s_ax + (s_W - s.gx * CEL) / 2;
  s.oy = s_ay + (s_H - s.gy * CEL) / 2;
  cobra_reiniciar(s);
}

void cobra_passo(float dt)
{
  Cobra& s = bq_estado_como<Cobra>();
  s.passou = false;
  s.acum += dt;
  if (s.acum < 0.10f) { return; }
  s.acum -= 0.10f;
  if (s.acum > 0.10f) { s.acum = 0; }

  static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
  const int hx = s.sx[s.cab], hy = s.sy[s.cab];
  const int it = cobra_i(s, s.len - 1);
  const int tx = s.sx[it], ty = s.sy[it];
  int melhor = -1;
  float nota = 1e9f;
  for (int k = 0; k < 4; ++k) {
    if (DX[k] == -s.dx && DY[k] == -s.dy) { continue; }
    const int nx = hx + DX[k], ny = hy + DY[k];
    if (nx < 0 || ny < 0 || nx >= s.gx || ny >= s.gy) { continue; }
    const bool come = nx == s.fx && ny == s.fy;
    if (s.occ[ny * s.gx + nx] && (come || nx != tx || ny != ty)) { continue; }
    int livres = 0;
    for (int q = 0; q < 4; ++q) {
      const int ax = nx + DX[q], ay = ny + DY[q];
      if (ax >= 0 && ay >= 0 && ax < s.gx && ay < s.gy && !s.occ[ay * s.gx + ax]) { ++livres; }
    }
    float n = (float)(abs(nx - s.fx) + abs(ny - s.fy)) + frand(0, 0.9f);
    if (livres < 2)                          { n += 8; }
    if (DX[k] == s.dx && DY[k] == s.dy)      { n -= 0.4f; }
    if (n < nota) { nota = n; melhor = k; }
  }
  if (melhor < 0) { cobra_reiniciar(s); return; }   // cornered: start over

  const int nx = hx + DX[melhor], ny = hy + DY[melhor];
  s.dx = DX[melhor]; s.dy = DY[melhor];
  const bool come = nx == s.fx && ny == s.fy;
  rec(s.ox + hx * CEL, s.oy + hy * CEL, CEL, CEL);   // the neck changes colour
  if (!come) {
    s.occ[ty * s.gx + tx] = 0;
    rec(s.ox + tx * CEL, s.oy + ty * CEL, CEL, CEL);
  } else {
    ++s.len;
  }
  s.cab = (s.cab + 1) & 63;
  s.sx[s.cab] = (uint8_t)nx;
  s.sy[s.cab] = (uint8_t)ny;
  s.occ[ny * s.gx + nx] = 1;
  rec(s.ox + nx * CEL, s.oy + ny * CEL, CEL, CEL);
  s.passou = true;
  if (come) {
    if (s.len >= 50) { cobra_reiniciar(s); } else { cobra_comida(s); }
  }
}

static void cobra_celula(lgfx::LovyanGFX& d, const Cobra& s, int gx, int gy, uint16_t cor)
{
  d.fillRoundRect(s.ox + gx * CEL + 2, s.oy + gy * CEL + 2, CEL - 4, CEL - 4, 9, cor);
}

void cobra_desenhar(lgfx::LovyanGFX& d)
{
  Cobra& s = bq_estado_como<Cobra>();
  if (!s.passou && !s_tudo) { return; }
  d.fillCircle(s.ox + s.fx * CEL + CEL / 2, s.oy + s.fy * CEL + CEL / 2 + 1, 12, c565(255, 70, 70));
  d.fillRect(s.ox + s.fx * CEL + CEL / 2, s.oy + s.fy * CEL + 5, 3, 7, c565(120, 200, 80));
  // Body: from tail to neck, in stripes; only the last two if not everything.
  const int ate = s_tudo ? s.len : 2;
  for (int k = ate - 1; k >= 1; --k) {
    const int i = cobra_i(s, k);
    cobra_celula(d, s, s.sx[i], s.sy[i], (k & 1) ? c565(30, 170, 60) : c565(20, 125, 45));
  }
  const int hx = s.sx[s.cab], hy = s.sy[s.cab];
  cobra_celula(d, s, hx, hy, c565(110, 255, 120));
  const int cx = s.ox + hx * CEL + CEL / 2, cy = s.oy + hy * CEL + CEL / 2;
  for (int lado = -1; lado <= 1; lado += 2) {
    const int ex = cx + s.dx * 8 - s.dy * 8 * lado, ey = cy + s.dy * 8 + s.dx * 8 * lado;
    d.fillCircle(ex, ey, 4, TFT_WHITE);
    d.fillCircle(ex + s.dx, ey + s.dy, 2, TFT_BLACK);
  }
}

}  // namespace bq_anim
