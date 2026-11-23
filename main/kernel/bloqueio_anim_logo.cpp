// Lock screen animation: the M5tabOS logo bouncing off the edges and changing colour on every hit.
// (part of bloqueio_anim; what is common to all of them is in bloqueio_anim_interno.hpp).

#include "bloqueio_anim_interno.hpp"

namespace bq_anim {

// ---------------------------------------------------------------------------
// 12. LOGO - "M5tabOS" bouncing off the edges and changing colour on every hit
//     (a tribute to the DVD screensaver; hitting the corner is rare)
// ---------------------------------------------------------------------------
namespace {
struct Logo { float x, y, vx, vy, ox, oy; int w, h; uint8_t cor; bool repintar; uint8_t cantos; };
}
static lgfx::LGFX_Sprite* s_logo = nullptr;   // the text, painted only when the colour changes

static void logo_pintar(Logo& s)
{
  if (!s_logo) { return; }
  const uint16_t cor = matiz(s.cor, 255);
  s_logo->fillScreen(TFT_BLACK);
  s_logo->drawRoundRect(0, 0, s.w, s.h, 12, cor);
  s_logo->drawRoundRect(1, 1, s.w - 2, s.h - 2, 11, cor);
  tab5_fonte_corpo(*s_logo);
  tab5_texto(*s_logo, 5.0f);
  s_logo->setTextDatum(middle_center);
  s_logo->setTextColor(cor, TFT_BLACK);
  s_logo->drawString("M5tabOS", s.w / 2, s.h / 2);
}

void logo_iniciar(void)
{
  Logo& s = bq_estado_como<Logo>();
  s.w = 230; s.h = 76;
  if (!s_logo) {
    s_logo = new lgfx::LGFX_Sprite();
    s_logo->setColorDepth(16);
    s_logo->setPsram(true);
    if (!s_logo->createSprite(s.w, s.h)) { delete s_logo; s_logo = nullptr; }
  }
  s.x = frand(s_ax, s_ax + s_W - s.w);
  s.y = frand(s_ay, s_ay + s_H - s.h);
  const float v = 170;
  s.vx = (irand(2) ? v : -v) * frand(0.8f, 1.1f);
  s.vy = (irand(2) ? v : -v) * frand(0.8f, 1.1f);
  s.ox = s.x; s.oy = s.y;
  s.cor = (uint8_t)irand(256);
  s.cantos = 0;
  logo_pintar(s);
}

void logo_passo(float dt)
{
  Logo& s = bq_estado_como<Logo>();
  s.ox = s.x; s.oy = s.y;
  s.x += s.vx * dt;
  s.y += s.vy * dt;
  int bateu = 0;
  if (s.x < s_ax)                { s.x = (float)s_ax;              s.vx =  fabsf(s.vx); ++bateu; }
  else if (s.x > s_ax + s_W - s.w) { s.x = (float)(s_ax + s_W - s.w); s.vx = -fabsf(s.vx); ++bateu; }
  if (s.y < s_ay)                { s.y = (float)s_ay;              s.vy =  fabsf(s.vy); ++bateu; }
  else if (s.y > s_ay + s_H - s.h) { s.y = (float)(s_ay + s_H - s.h); s.vy = -fabsf(s.vy); ++bateu; }
  if (bateu) {
    s.cor = (uint8_t)(s.cor + 45 + irand(40));
    s.repintar = true;
    if (bateu == 2) { ++s.cantos; }   // the corner! (kept only in memory, as a trophy)
  }
  const int x0 = (int)fminf(s.ox, s.x) - 1, y0 = (int)fminf(s.oy, s.y) - 1;
  rec(x0, y0, s.w + (int)ceilf(fabsf(s.x - s.ox)) + 3, s.h + (int)ceilf(fabsf(s.y - s.oy)) + 3);
}

void logo_desenhar(lgfx::LovyanGFX& d)
{
  Logo& s = bq_estado_como<Logo>();
  if (s.repintar) { logo_pintar(s); s.repintar = false; }
  if (s_logo) {
    s_logo->pushSprite(&d, (int)s.x, (int)s.y, (uint16_t)0);   // black = transparent
  } else {
    d.drawRoundRect((int)s.x, (int)s.y, s.w, s.h, 12, matiz(s.cor, 255));
  }
}

// Frees the painted text (when choosing another animation or leaving the lock screen).
void logo_soltar(void)
{
  delete s_logo;
  s_logo = nullptr;
}

}  // namespace bq_anim
