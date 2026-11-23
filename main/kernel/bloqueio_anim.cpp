// Lock screen animations. Contract and rules in bloqueio_anim.hpp.
//
// Each animation has three functions (and an optional fourth):
//   iniciar()  - draws a random initial state;
//   passo(dt)  - moves everything and records with rec() the box each object
//                occupied and the one it will occupy (those boxes will be erased);
//   desenhar() - paints all the objects, after the erase;
//   fundo()    - (optional) paints its own fixed background; without it, the background is a
//                randomly chosen abstract drawing.
// The frame (bq_anim_quadro) ties the three together and keeps the box list so the
// shell knows where the text needs repainting.

#include "bloqueio_anim_interno.hpp"

#include "esp_random.h"
#include "esp_timer.h"

using namespace bq_anim;

namespace bq_anim {

// ---------------------------------------------------------------------------
// Area, randomness, colours
// ---------------------------------------------------------------------------
int s_ax, s_ay, s_W, s_H;   // usable area (where the animation may touch)

uint32_t s_rng = 2463534242u;

alignas(16) uint8_t bq_estado[BQ_ESTADO_MAX];   // state of the chosen animation

// Hue 0..255 (red, yellow, green, cyan, blue, magenta) with brightness v.
uint16_t matiz(uint8_t h, int v)
{
  const int reg = h / 43;
  const int sub = (h - reg * 43) * 6;   // 0..252
  int r, g, b;
  switch (reg) {
    case 0:  r = 255;       g = sub;       b = 0;         break;
    case 1:  r = 255 - sub; g = 255;       b = 0;         break;
    case 2:  r = 0;         g = 255;       b = sub;       break;
    case 3:  r = 0;         g = 255 - sub; b = 255;       break;
    case 4:  r = sub;       g = 0;         b = 255;       break;
    default: r = 255;       g = 0;         b = 255 - sub; break;
  }
  return c565(r * v / 255, g * v / 255, b * v / 255);
}

// ---------------------------------------------------------------------------
// List of boxes touched this frame
// ---------------------------------------------------------------------------
// Boxes that OVERLAP become one (erasing the same pixel twice is
// waste, and the rain, for example, produces two nearly identical ones per column). The
// rest go as they are: the compositor copes with many small areas per frame
// (compositor.cpp, MAX_AREAS). MAX_RET is just the list size; filling it is
// rare, and then the new one joins the pair that wastes least.
static const int MAX_RET = 160;
static bq_ret_t s_ret[MAX_RET];
static int      s_n_ret = 0;
bool            s_tudo  = false;   // draw everything (not just what changed)

static inline int area_de(const bq_ret_t& r) { return r.w * r.h; }

static bq_ret_t envolver(const bq_ret_t& a, const bq_ret_t& b)
{
  const int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
  const int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  const int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Pixels NOT requested that merging a and b forces to be erased (negative = they
// overlap: merging comes free).
static inline int desperdicio(const bq_ret_t& a, const bq_ret_t& b)
{
  return area_de(envolver(a, b)) - area_de(a) - area_de(b);
}

void rec(int x, int y, int w, int h)
{
  int x1 = x + w, y1 = y + h;
  if (x < s_ax)           { x = s_ax; }
  if (y < s_ay)           { y = s_ay; }
  if (x1 > s_ax + s_W)    { x1 = s_ax + s_W; }
  if (y1 > s_ay + s_H)    { y1 = s_ay + s_H; }
  if (x1 <= x || y1 <= y) { return; }
  bq_ret_t r = { (int16_t)x, (int16_t)y, (int16_t)(x1 - x), (int16_t)(y1 - y) };

  // Merge with whatever already overlaps (free) - one merge may open up another.
  for (bool juntou = true; juntou; ) {
    juntou = false;
    for (int i = 0; i < s_n_ret; ++i) {
      if (desperdicio(s_ret[i], r) <= 0) {
        r = envolver(s_ret[i], r);
        s_ret[i] = s_ret[--s_n_ret];
        juntou = true;
        break;
      }
    }
  }
  if (s_n_ret < MAX_RET) { s_ret[s_n_ret++] = r; return; }

  // Full: merge the pair (the new one counts as a candidate) that wastes least.
  int bi = 0, bj = -1, melhor = 0x7fffffff;
  for (int i = 0; i < s_n_ret; ++i) {
    const int dr = desperdicio(s_ret[i], r);
    if (dr < melhor) { melhor = dr; bi = i; bj = -1; }
    for (int j = i + 1; j < s_n_ret; ++j) {
      const int d = desperdicio(s_ret[i], s_ret[j]);
      if (d < melhor) { melhor = d; bi = i; bj = j; }
    }
  }
  if (bj < 0) {
    s_ret[bi] = envolver(s_ret[bi], r);
  } else {
    s_ret[bi] = envolver(s_ret[bi], s_ret[bj]);
    s_ret[bj] = r;
  }
  // A merged box erases an object that didn't move: whoever only draws what
  // changed (the little snake) needs to draw everything.
  s_tudo = true;
}

// Box covering two points, with margin 'r' around them.
void rec2(float x0, float y0, float x1, float y1, int r)
{
  const int xa = (int)floorf(fminf(x0, x1)) - r;
  const int ya = (int)floorf(fminf(y0, y1)) - r;
  rec(xa, ya, (int)ceilf(fabsf(x1 - x0)) + 2 * r + 1, (int)ceilf(fabsf(y1 - y0)) + 2 * r + 1);
}

// ---------------------------------------------------------------------------
// BACKGROUND
// ---------------------------------------------------------------------------
// A fixed drawing behind the animation: thin dark lines on black. It
// is painted ONCE per lock into an image the size of the area (PSRAM,
// ~1.6 MB, only while locked) and "erasing" each box becomes
// copying that piece of the image instead of painting black - the same amount of
// writing as before, so the background costs nothing per frame. Without memory,
// the usual black remains.
static lgfx::LGFX_Sprite* s_fundo = nullptr;
static bool s_fundo_falhou = false;   // during this lock: don't retry on every frame
static bool s_fundo_pintar = false;   // the image is new: it goes to the screen whole

static void fundo_soltar(void)
{
  delete s_fundo;
  s_fundo = nullptr;
}

// Point on the image border, 't' from 0 to 1 going all the way round.
static void ponto_borda(int W, int H, float t, float* x, float* y)
{
  t -= floorf(t);
  const float p = 2.0f * (W + H), d = t * p;
  if (d < W)              { *x = d;                  *y = 0; }
  else if (d < W + H)     { *x = (float)W;           *y = d - W; }
  else if (d < 2 * W + H) { *x = (float)W - (d - W - H); *y = (float)H; }
  else                    { *x = 0;                  *y = (float)H - (d - 2 * W - H); }
}

// THREADS: "string art" - straight lines between two rails draw curves that don't
// exist.
static void fundo_fios(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int env = 3 + irand(3);
  const uint8_t h0 = (uint8_t)irand(256);
  for (int e = 0; e < env; ++e) {
    float a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y;
    const float ta = frand(0, 1), tb = frand(0, 1);
    ponto_borda(W, H, ta, &a0x, &a0y);
    ponto_borda(W, H, ta + frand(0.15f, 0.3f), &a1x, &a1y);
    ponto_borda(W, H, tb, &b0x, &b0y);
    ponto_borda(W, H, tb + frand(0.15f, 0.3f), &b1x, &b1y);
    const int n = 44;
    const uint8_t h = (uint8_t)(h0 + e * 30);
    for (int i = 0; i <= n; ++i) {
      const float f = (float)i / n;
      const int v = 34 + (int)(40 * sinf(f * 3.1416f));
      s.drawLine((int)(a0x + (a1x - a0x) * f), (int)(a0y + (a1y - a0y) * f),
                 (int)(b1x + (b0x - b1x) * f), (int)(b1y + (b0y - b1y) * f), matiz((uint8_t)(h + i), v));
    }
  }
}

// WAVES: contour lines flowing from one side to the other.
static void fundo_ondas(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int n = 30;
  const float k1 = frand(0.004f, 0.009f), k2 = frand(0.011f, 0.02f);
  const float p1 = frand(0, 6.28f), p2 = frand(0, 6.28f);
  const float a1 = frand(24, 50), a2 = frand(8, 20);
  const uint8_t h0 = (uint8_t)(120 + irand(80));
  for (int i = 0; i < n; ++i) {
    const float y0 = (i + 0.5f) * H / n;
    const uint16_t cor = matiz((uint8_t)(h0 + i * 2), 40 + (i % 3) * 12);
    int px = 0, py = 0;
    for (int x = 0; x <= W + 12; x += 12) {
      const int y = (int)(y0 + a1 * sinf(x * k1 + i * 0.28f + p1) + a2 * sinf(x * k2 - i * 0.17f + p2));
      if (x > 0) { s.drawLine(px, py, x, y, cor); }
      px = x; py = y;
    }
  }
}

// WEB: points linked to their nearest neighbours.
static void fundo_teia(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int n = 38;
  int16_t px[n], py[n];
  for (int i = 0; i < n; ++i) { px[i] = (int16_t)irand(W); py[i] = (int16_t)irand(H); }
  const uint8_t h = (uint8_t)irand(256);
  for (int i = 0; i < n; ++i) {
    int viz[3] = { -1, -1, -1 };
    int dist[3] = { 0x7fffffff, 0x7fffffff, 0x7fffffff };
    for (int j = 0; j < n; ++j) {
      if (j == i) { continue; }
      const int dx = px[j] - px[i], dy = py[j] - py[i], d = dx * dx + dy * dy;
      for (int k = 0; k < 3; ++k) {
        if (d < dist[k]) {
          for (int m = 2; m > k; --m) { dist[m] = dist[m - 1]; viz[m] = viz[m - 1]; }
          dist[k] = d; viz[k] = j;
          break;
        }
      }
    }
    for (int k = 0; k < 3; ++k) {
      if (viz[k] >= 0) { s.drawLine(px[i], py[i], px[viz[k]], py[viz[k]], matiz((uint8_t)(h + k * 12), 44)); }
    }
  }
  for (int i = 0; i < n; ++i) { s.fillCircle(px[i], py[i], 2, matiz(h, 110)); }
}

// RAYS: a fan coming from a point off-screen, with rings around it (moire).
static void fundo_raios(lgfx::LGFX_Sprite& s, int W, int H)
{
  float fx, fy;
  ponto_borda(W, H, frand(0, 1), &fx, &fy);
  fx += (fx < W / 2 ? -60.0f : 60.0f);
  fy += (fy < H / 2 ? -60.0f : 60.0f);
  const float R = sqrtf((float)W * W + (float)H * H) + 80;
  const uint8_t h = (uint8_t)irand(256);
  const int n = 72;
  for (int i = 0; i < n; ++i) {
    const float ang = 6.2832f * i / n;
    s.drawLine((int)fx, (int)fy, (int)(fx + cosf(ang) * R), (int)(fy + sinf(ang) * R),
               matiz((uint8_t)(h + (i & 1) * 20), (i & 1) ? 30 : 48));
  }
  for (int r = 50; r < R; r += 46) { s.drawCircle((int)fx, (int)fy, r, matiz((uint8_t)(h + 128), 34)); }
}

// CIRCLES: rings from two or three centres crossing (interference).
static void fundo_circulos(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int n = 2 + irand(2);
  const uint8_t h = (uint8_t)irand(256);
  const int R = (int)sqrtf((float)W * W + (float)H * H);
  for (int c = 0; c < n; ++c) {
    const int cx = irand(W), cy = irand(H);
    const uint16_t cor = matiz((uint8_t)(h + c * 70), 40);
    for (int r = 14; r < R; r += 22) { s.drawCircle(cx, cy, r, cor); }
  }
}

// Fixed stars, very faint (the solar system background).
void fundo_ceu(lgfx::LGFX_Sprite& s, int W, int H)
{
  for (int i = 0; i < 160; ++i) {
    const int v = 40 + irand(90);
    s.drawPixel(irand(W), irand(H), c565(v, v, v + 20));
  }
}

// Pushes the whole image to the screen, in bands (one pushSprite per band: the
// same rule of not dumping 1.6 MB at once - see tab5_tela_limpar).
static void fundo_pintar_tudo(lgfx::LovyanGFX& d)
{
  const int FAIXA = 128;
  for (int y = 0; y < s_H; y += FAIXA) {
    const int h = (y + FAIXA <= s_H) ? FAIXA : s_H - y;
    tela_virtual_cobrir(s_ax, s_ay + y, s_W, h);
    d.setClipRect(s_ax, s_ay + y, s_W, h);
    s_fundo->pushSprite(&d, s_ax, s_ay);
  }
  d.clearClipRect();
}

// Erases a box: restores its background (or black, without a background).
static void apagar(lgfx::LovyanGFX& d, const bq_ret_t& r)
{
  if (!s_fundo) {
    d.fillRect(r.x, r.y, r.w, r.h, TFT_BLACK);
    return;
  }
  // Cover first: the whole box will be written, so the compositor doesn't
  // need to bring its previous frame over from the other page.
  tela_virtual_cobrir(r.x, r.y, r.w, r.h);
  d.setClipRect(r.x, r.y, r.w, r.h);
  s_fundo->pushSprite(&d, s_ax, s_ay);
}

}  // namespace bq_anim

// ===========================================================================
// THE FRAME
// ===========================================================================
typedef struct {
  void (*iniciar)(void);
  void (*passo)(float dt);
  void (*desenhar)(lgfx::LovyanGFX& d);
  void (*fundo)(lgfx::LGFX_Sprite& s, int W, int H);   // nullptr = random abstract
} anim_t;

static const anim_t ANIMS[] = {
  { bolas_iniciar,    bolas_passo,    bolas_desenhar,    nullptr     },
  { fogos_iniciar,    fogos_passo,    fogos_desenhar,    nullptr     },
  { chuva_iniciar,    chuva_passo,    chuva_desenhar,    nullptr     },
  { estrelas_iniciar, estrelas_passo, estrelas_desenhar, nullptr     },
  { bolhas_iniciar,   bolhas_passo,   bolhas_desenhar,   nullptr     },
  { cobra_iniciar,    cobra_passo,    cobra_desenhar,    nullptr     },
  { lumes_iniciar,    lumes_passo,    lumes_desenhar,    nullptr     },
  { pendulos_iniciar, pendulos_passo, pendulos_desenhar, nullptr     },
  { neve_iniciar,     neve_passo,     neve_desenhar,     nullptr     },
  { cometas_iniciar,  cometas_passo,  cometas_desenhar,  nullptr     },
  { solar_iniciar,    solar_passo,    solar_desenhar,    solar_fundo },
  { logo_iniciar,     logo_passo,     logo_desenhar,     nullptr     },
};
static const int N_ANIMS = (int)(sizeof(ANIMS) / sizeof(ANIMS[0]));

static void (* const FUNDOS[])(lgfx::LGFX_Sprite&, int, int) = {
  fundo_fios, fundo_ondas, fundo_teia, fundo_raios, fundo_circulos,
};

static const int N_FUNDOS = (int)(sizeof(FUNDOS) / sizeof(FUNDOS[0]));

static int  s_atual = 0;
static int  s_fundo_atual = 0;
static bool s_pronta = false;   // the chosen one has already been started

// dt comes from the system frame pace (kernel/ritmo.hpp): the time between the
// scans in which two frames go on air. Clamped here: after a
// pause (wake-up), the animation continues where it was instead of jumping.
static float passo_dt(void)
{
  float dt = sistema_quadro_dt();
  if (dt < 0.005f) { dt = 0.005f; }
  if (dt > 0.10f)  { dt = 0.10f; }
  return dt;
}

void bq_anim_sortear(void)
{
  s_rng ^= esp_random();
  if (s_rng == 0) { s_rng = 2463534242u; }
  int nova = irand(N_ANIMS - 1);
  if (nova >= s_atual) { ++nova; }   // never the same one twice in a row
  int fundo = irand(N_FUNDOS - 1);
  if (fundo >= s_fundo_atual) { ++fundo; }
  s_atual       = nova;
  s_fundo_atual = fundo;
  s_pronta      = false;
  s_fundo_falhou = false;
  fundo_soltar();   // a new drawing on each lock
  if (ANIMS[s_atual].iniciar != logo_iniciar) { logo_soltar(); }
}

void bq_anim_soltar(void)
{
  fundo_soltar();
  logo_soltar();
  s_pronta = false;   // the logo (if it's that one) needs its image back
}

// Creates and paints the background image. false = no memory (black stays).
static bool fundo_criar(void)
{
  auto* sp = new lgfx::LGFX_Sprite();
  if (!sp) { return false; }
  // The SAME format as the panel pages: the copy becomes a pure memcpy.
  sp->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  sp->setPsram(true);
  if (!sp->createSprite(s_W, s_H)) { delete sp; return false; }
  sp->fillScreen(TFT_BLACK);
  if (ANIMS[s_atual].fundo) { ANIMS[s_atual].fundo(*sp, s_W, s_H); }
  else                      { FUNDOS[s_fundo_atual](*sp, s_W, s_H); }
  s_fundo = sp;
  return true;
}

void bq_anim_iniciar(const area_t& a)
{
  const bool mudou_area = a.x != s_ax || a.y != s_ay || a.largura != s_W || a.altura != s_H;
  s_ax = a.x; s_ay = a.y; s_W = a.largura; s_H = a.altura;
  s_n_ret  = 0;
  if (!s_pronta || mudou_area) {
    ANIMS[s_atual].iniciar();
    s_pronta = true;
    fundo_soltar();
  }
  if (!s_fundo && !s_fundo_falhou) { s_fundo_falhou = !fundo_criar(); }
  s_fundo_pintar = s_fundo != nullptr;
  s_tudo = true;   // the screen was cleared: everything needs painting
}

void bq_anim_quadro(lgfx::LovyanGFX& d, const bq_ret_t* extras, int n_extras,
                    const bq_ret_t* caixas, int n_caixas, bq_ret_t* clips)
{
  const float dt = passo_dt();

  s_n_ret = 0;
  for (int i = 0; i < n_extras; ++i) { rec(extras[i].x, extras[i].y, extras[i].w, extras[i].h); }
  if (n_extras > 0) { s_tudo = true; }

  ANIMS[s_atual].passo(dt);

  // WHERE THE TEXT NEEDS REPAINTING: for each text box, the box that
  // covers everything the animation touched inside it. That WHOLE box goes into the
  // erase list. The text is anti-aliased (translucent edge): painted twice
  // on the same pixel it gets bolder every frame. Erasing the whole box
  // first, each pixel receives the text only once.
  //
  // In passes: merging boxes (rec) can grow one of them over
  // another text. Each pass redoes each text's clip with EVERYTHING that will be
  // erased inside it, and the clip itself goes into the list - until it stops changing.
  for (int j = 0; j < n_caixas; ++j) { clips[j] = { 0, 0, 0, 0 }; }
  for (int passada = 0; passada < 4; ++passada) {
    bool mudou = false;
    for (int j = 0; j < n_caixas; ++j) {
      const bq_ret_t& cx = caixas[j];
      bq_ret_t env = { 0, 0, 0, 0 };
      for (int i = 0; i < s_n_ret; ++i) {
        const bq_ret_t& r = s_ret[i];
        const int ix0 = r.x > cx.x ? r.x : cx.x, iy0 = r.y > cx.y ? r.y : cx.y;
        const int ix1 = r.x + r.w < cx.x + cx.w ? r.x + r.w : cx.x + cx.w;
        const int iy1 = r.y + r.h < cx.y + cx.h ? r.y + r.h : cx.y + cx.h;
        if (ix1 <= ix0 || iy1 <= iy0) { continue; }
        const bq_ret_t c = { (int16_t)ix0, (int16_t)iy0, (int16_t)(ix1 - ix0), (int16_t)(iy1 - iy0) };
        env = env.w > 0 ? envolver(env, c) : c;
      }
      if (env.w <= 0 || memcmp(&env, &clips[j], sizeof(env)) == 0) { continue; }
      clips[j] = env;
      rec(env.x, env.y, env.w, env.h);
      s_tudo = true;   // what was in that box was erased: everything is redrawn
      mudou = true;
    }
    if (!mudou) { break; }
  }

  if (s_fundo_pintar) {
    fundo_pintar_tudo(d);
    s_fundo_pintar = false;
  }
  for (int i = 0; i < s_n_ret; ++i) { apagar(d, s_ret[i]); }
  d.setClipRect(s_ax, s_ay, s_W, s_H);   // never touches the bars
  ANIMS[s_atual].desenhar(d);
  d.clearClipRect();
  s_tudo = false;
}

// ---------------------------------------------------------------------------
// THE SAME DRAWING AS WALLPAPER (home screen). Declared in the .hpp. The image
// only exists during the call: painted, copied to the screen and freed - the
// home screen is only drawn whole (opening, changing page, returning from an app),
// so there's no reason to hold 1.6 MB all the time. The same seed makes the
// drawing come out the same every time.
// THE CACHED WALLPAPER. Generating the procedural drawing into a sprite the
// size of the area (~1.6 MB) on every full pass of the home screen - every app
// close, every wake-up - was the same work, with the same result (same
// seed), repeated in the heaviest transition of the system. Now the finished sprite
// stays while style/seed/area are the same - ALSO with an
// app open and when idle (06/11/2026: the kernel used to release it at those two moments, and
// returning to the home screen redid the whole drawing: the home screen's full pass
// measured 145 ms against 17-38 ms for apps). It is ~1.6 MB of PSRAM, with 30 MB
// free; PSRAM keeps its content in light sleep.
static lgfx::LGFX_Sprite* s_parede = nullptr;
static int      s_parede_estilo = -1;
static uint32_t s_parede_semente = 0;
static int      s_parede_l = 0, s_parede_a = 0;

void bq_parede_soltar(void)
{
  delete s_parede;
  s_parede = nullptr;
  s_parede_estilo = -1;
}

bool bq_fundo_parede(lgfx::LovyanGFX& d, const area_t& a, int estilo, uint32_t semente)
{
  if (estilo < 0 || estilo >= N_FUNDOS) { return false; }
  if (a.largura <= 0 || a.altura <= 0) { return false; }
  const bool mesma = s_parede && s_parede_estilo == estilo && s_parede_semente == semente
                  && s_parede_l == a.largura && s_parede_a == a.altura;
  if (!mesma) {
    bq_parede_soltar();
    auto* sp = new lgfx::LGFX_Sprite();
    if (!sp) { return false; }
    sp->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    sp->setPsram(true);
    if (!sp->createSprite(a.largura, a.altura)) { delete sp; return false; }
    sp->fillScreen(TFT_BLACK);
    // The seed belongs to the wallpaper, not the animation: the common generator is
    // borrowed and returned as it was.
    const uint32_t rng = s_rng;
    s_rng = semente ? semente : 2463534242u;
    FUNDOS[estilo](*sp, a.largura, a.altura);
    s_rng = rng;
    s_parede = sp;
    s_parede_estilo = estilo; s_parede_semente = semente;
    s_parede_l = a.largura; s_parede_a = a.altura;
  }

  // IN BANDS, like every large write (see tab5_tela_limpar): the whole image
  // at once drains the panel FIFO. And covered: the compositor doesn't
  // need to bring over from the front page what will be overwritten.
  const int FAIXA = 128;
  for (int y = 0; y < a.altura; y += FAIXA) {
    const int h = (y + FAIXA <= a.altura) ? FAIXA : a.altura - y;
    tela_virtual_cobrir(a.x, a.y + y, a.largura, h);
    d.setClipRect(a.x, a.y + y, a.largura, h);
    s_parede->pushSprite(&d, a.x, a.y);
  }
  d.clearClipRect();
  return true;
}
