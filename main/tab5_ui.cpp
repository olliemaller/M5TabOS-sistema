#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "idioma.h"
#include "fontes/fontes_suaves.hpp"
#include "tab5_glifos.hpp"
#include "esp_memory_utils.h"   // esp_ptr_external_ram
#include <string.h>

// ---------------------------------------------------------------------------
// SMOOTH FONTS - loaded ONCE, read from flash forever
// ---------------------------------------------------------------------------
// Each VLWfont keeps a pointer to its DataWrapper and reads each letter's drawing
// on demand (seek + read) - that is why the wrapper and the font object
// live in static storage, and changing size is just changing a pointer (nothing
// is allocated or parsed on each text). Each font's metrics table
// (~9 bytes per glyph) goes to PSRAM in loadFont.
//
// ONE TASK DRAWS: the wrapper has its own read position, and two tasks
// writing with the same font at the same time would scramble the read. Today
// only the UI draws text (the compositor is its) - whoever one day draws
// from another task needs its own copy.
#define FONTES_MAX 24
static FonteRapida         s_vlw[2][FONTES_MAX];   // VLW with glyph cache (tab5_glifos)
static lgfx::PointerWrapper s_dados[2][FONTES_MAX];
static int                 s_altura[2][FONTES_MAX];
static int                 s_n[2] = { 0, 0 };

void tab5_fontes_iniciar(void)
{
  const fonte_suave_t* tab[2] = { FONTES_CORPO, FONTES_NEGRITO };
  const int            n[2]   = { FONTES_CORPO_N, FONTES_NEGRITO_N };
  for (int f = 0; f < 2; ++f) {
    s_n[f] = 0;
    for (int i = 0; i < n[f] && s_n[f] < FONTES_MAX; ++i) {
      const int k = s_n[f];
      s_dados[f][k].set(tab[f][i].dados, tab[f][i].tamanho);
      if (s_vlw[f][k].loadFont(&s_dados[f][k])) {
        s_altura[f][k] = tab[f][i].altura;
        ++s_n[f];
      }
    }
  }
}

const lgfx::IFont* tab5_fonte_suave(bool negrito, int altura_px)
{
  const int f = negrito ? 1 : 0;
  if (s_n[f] == 0) {   // before tab5_fontes_iniciar (or out of memory): the old bitmap font
    return negrito ? (const lgfx::IFont*)&fonts::FreeSansBold12pt7b : (const lgfx::IFont*)&fonts::FreeSans12pt7b;
  }
  int melhor = 0;
  for (int i = 1; i < s_n[f]; ++i) {
    if (abs(s_altura[f][i] - altura_px) < abs(s_altura[f][melhor] - altura_px)) { melhor = i; }
  }
  return &s_vlw[f][melhor];
}

// Which family the font in use belongs to: 0 body, 1 bold, -1 other (title...).
static int familia_de(const lgfx::IFont* fonte)
{
  if (fonte == &fonts::FreeSans12pt7b)     { return 0; }
  if (fonte == &fonts::FreeSansBold12pt7b) { return 1; }
  for (int f = 0; f < 2; ++f) {
    for (int i = 0; i < s_n[f]; ++i) { if (fonte == &s_vlw[f][i]) { return f; } }
  }
  return -1;
}

void tab5_texto(lgfx::LovyanGFX& d, float tamanho)
{
  // A font that is neither body nor bold (a title or Font0 that someone
  // left behind): BODY text is what is wanted here - no call
  // asks for tab5_texto over the title (checked). Going back to the
  // body keeps the text the same regardless of what was drawn before.
  // (The 1st version called itself in this case: infinite loop in the status
  // bar, UI watchdog every 5 s.)
  const int f = familia_de(d.getFont());
  d.setFont(tab5_fonte_suave(f == 1, (int)(tamanho * 8.0f + 0.5f)));
  d.setTextSize(1);
}

void tab5_fonte_corpo(lgfx::LovyanGFX& d)          { d.setFont(TAB5_FONTE_CORPO); d.setTextSize(1); }
void tab5_fonte_corpo_negrito(lgfx::LovyanGFX& d)  { d.setFont(tab5_fonte_suave(true, 29)); d.setTextSize(1); }
void tab5_titulo(lgfx::LovyanGFX& d, int altura_px)   { d.setFont(tab5_fonte_suave(true, altura_px)); d.setTextSize(1); }

void tab5_botao(lgfx::LovyanGFX& d, int x, int y, int l, int a,
               const char* txt, uint16_t fundo,
               uint16_t tinta, float tamanho_texto)
{
  // Saves and restores the text style. Without this, the middle_center below LEAKS
  // to whoever draws next, and all following text is centred on x instead
  // of starting at it - which pushes long lines off the screen to the
  // left. The same gotcha already bit tab5_power_mostrar and, before this
  // function existed, four different apps independently.
  //
  // Copy BY VALUE: getTextStyle() returns a reference to the internal member.
  const lgfx::TextStyle estilo = d.getTextStyle();

  d.fillRoundRect(x, y, l, a, TEMA_RAIO_M, fundo);   // the central theme's corner
  d.setTextDatum(middle_center);
  d.setTextColor(tinta, fundo);
  // BOLD and SIZED TO THE BUTTON. The text used to be a
  // fixed size requested by the caller (2.0 or 4.0), and a tall button with a
  // 16 px letter looked "empty" and was hard to read. Now the letter takes ~55% of the button height and only shrinks if
  // it does not fit the width (with 12 px of breathing room on each side). The
  // caller's 'tamanho_texto' no longer has any effect: the button decides, and
  // all the system's buttons stay in the same proportion.
  (void)tamanho_texto;
  const int respiro = 12;
  int alt = (a * 55) / 100;
  for (;;) {
    d.setFont(tab5_fonte_suave(true, alt));
    d.setTextSize(1);
    if (alt <= 12 || (int)d.textWidth(txt) <= l - 2 * respiro) { break; }
    alt -= 2;
  }
  d.drawString(txt, x + l / 2, y + a / 2);
  tab5_fonte_corpo(d);

  d.setTextStyle(estilo);
}

void tab5_celula(lgfx::LovyanGFX& d, int cx, int y0,
                 const char* rotulo, const char* valor, uint16_t cor_valor,
                 const char* subtexto)
{
  const lgfx::TextStyle estilo = d.getTextStyle();   // see the warning in tab5_botao()

  d.setTextDatum(top_center);

  tab5_fonte_corpo(d);
  tab5_texto(d, 2);
  // The text background is the area's (sistema_cor_fundo): black in apps not
  // yet migrated, TEMA_FUNDO in those already on the theme.
  const uint16_t fundo = sistema_cor_fundo();
  d.setTextColor(TEMA_TEXTO_3, fundo);   // label: the dimmest
  d.drawString(rotulo, cx, y0);

  tab5_texto(d, 4);
  d.setTextColor(cor_valor, fundo);
  d.drawString(valor, cx, y0 + 22);

  if (subtexto && subtexto[0]) {
    tab5_texto(d, 2);
    d.setTextColor(TEMA_TEXTO_2, fundo);   // subtext: secondary text
    d.drawString(subtexto, cx, y0 + 66);
  }

  d.setTextStyle(estilo);
}

// M5GFX's draw_gradient_wedgeline (LGFXBase.cpp) line for line - same
// pixels, same scanning shortcut (each row stops when it leaves the line,
// and the next starts where the previous one entered) - with ONE difference: the box
// is clipped by the clip in force, instead of becoming the clip and being cleared
// at the end.
static float distancia_ao_segmento(float px, float py, float bx, float by)
{
  const float d = (px * bx + py * by) / (bx * bx + by * by);
  const float h = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
  const float dx = px - bx * h, dy = py - by * h;
  return sqrtf(dx * dx + dy * dy);
}

void tab5_linha_grossa(lgfx::LovyanGFX& d, float ax, float ay, float bx, float by,
                       float r, uint16_t cor)
{
  if (r < 0.0f) { return; }
  if (fabsf(ax - bx) < 0.01f && fabsf(ay - by) < 0.01f) { bx += 0.01f; }   // avoids division by zero
  const float baixo = 1.0f / 32.0f, alto = 1.0f - baixo;

  int32_t x0 = (int32_t)floorf(fminf(ax - r, bx - r));
  int32_t x1 = (int32_t)ceilf(fmaxf(ax + r, bx + r));
  int32_t y0 = (int32_t)floorf(fminf(ay - r, by - r));
  int32_t y1 = (int32_t)ceilf(fmaxf(ay + r, by + r));
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  if (x0 < cx) { x0 = cx; }
  if (y0 < cy) { y0 = cy; }
  if (x1 > cx + cl - 1) { x1 = cx + cl - 1; }
  if (y1 > cy + ca - 1) { y1 = cy + ca - 1; }
  if (x0 > x1 || y0 > y1) { return; }

  int32_t ys = (int32_t)ay;
  if ((ax - r) > (bx - r)) { ys = (int32_t)by; }
  if (ys < y0) { ys = y0; }
  if (ys > y1 + 1) { ys = y1 + 1; }
  const float ar = r + 0.5f;
  const float bax = bx - ax, bay = by - ay;

  d.startWrite();
  for (int pass = 0; pass < 2; ++pass) {
    // 1st pass: from ys downwards; 2nd: from ys-1 upwards.
    int32_t xs = x0;
    const int32_t passo = pass ? -1 : 1;
    for (int32_t yp = pass ? ys - 1 : ys; pass ? (yp >= y0) : (yp <= y1); yp += passo) {
      bool dentro = false;
      float alfa = 1.0f;
      for (int32_t xp = xs; xp <= x1; ++xp) {
        if (dentro && alfa <= baixo) { break; }
        alfa = ar - distancia_ao_segmento(xp - ax, yp - ay, bax, bay);
        if (alfa <= baixo) { continue; }
        if (!dentro) { dentro = true; xs = xp; }
        if (alfa > alto) { d.drawPixel(xp, yp, cor); }
        else             { d.fillRectAlpha(xp, yp, 1, 1, (uint8_t)(alfa * 255.0f), cor); }
      }
    }
  }
  d.endWrite();
}

// ---------------------------------------------------------------------------
// SPRITE IN PSRAM
// ---------------------------------------------------------------------------
M5Canvas* tab5_sprite_novo(lgfx::LovyanGFX* pai, int largura, int altura)
{
  if (largura <= 0 || altura <= 0) { return nullptr; }
  M5Canvas* c = new M5Canvas(pai);
  c->setPsram(true);
  c->setColorDepth(16);
  // createSprite() may have fallen back to internal memory (M5GFX's fallback when PSRAM
  // refuses): check WHERE the buffer ended up, not just whether it did.
  if (!c->createSprite(largura, altura) || !esp_ptr_external_ram(c->getBuffer())) {
    c->deleteSprite();
    delete c;
    return nullptr;
  }
  c->setFont(TAB5_FONTE_CORPO);
  return c;
}

void tab5_sprite_soltar(M5Canvas** sprite)
{
  if (!sprite || !*sprite) { return; }
  (*sprite)->deleteSprite();
  delete *sprite;
  *sprite = nullptr;
}

// ---------------------------------------------------------------------------
// CUT TO FIT (see tab5_ui.hpp)
// ---------------------------------------------------------------------------
static inline bool continuacao_utf8(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

// Longest prefix of 't' (on a character boundary) that measures <= lim. 'n' is the
// length of 't'; the whole of 't' does NOT fit (the caller has already measured).
static size_t maior_prefixo(lgfx::LovyanGFX& d, char* t, size_t n, int lim)
{
  size_t lo = 0, hi = n;   // the prefix of length lo fits; the one of length hi does not
  while (hi - lo > 1) {
    size_t meio = (lo + hi) / 2;
    while (meio > lo && continuacao_utf8(t[meio])) { --meio; }
    if (meio == lo) {      // no character start before the middle: look after it
      meio = (lo + hi) / 2;
      while (meio < hi && continuacao_utf8(t[meio])) { ++meio; }
      if (meio >= hi) { break; }   // a single character between lo and hi, and it does not fit
    }
    const char guardado = t[meio];
    t[meio] = 0;
    const bool cabe = (int)d.textWidth(t) <= lim;
    t[meio] = guardado;
    if (cabe) { lo = meio; } else { hi = meio; }
  }
  return lo;
}

size_t tab5_cortar_ao_caber(lgfx::LovyanGFX& d, char* t, int max_l)
{
  if (!t) { return 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  const size_t k = maior_prefixo(d, t, n, max_l);
  t[k] = 0;
  return k;
}

size_t tab5_cortar_com_reticencias(lgfx::LovyanGFX& d, char* t, size_t tam, int max_l)
{
  if (!t || tam < 4) { return t ? strlen(t) : 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  size_t k = maior_prefixo(d, t, n, max_l - (int)d.textWidth("..."));
  if (k + 4 > tam) {   // no room for the three dots: step back by character
    k = tam - 4;
    while (k > 0 && continuacao_utf8(t[k])) { --k; }
  }
  memcpy(t + k, "...", 4);
  return k + 3;
}

void tab5_duracao(uint32_t s, char* out, size_t n)
{
  if (s >= 3600)    { snprintf(out, n, tr("%lu h %02lu min"), (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60)); }
  else if (s >= 60) { snprintf(out, n, tr("%lu min"), (unsigned long)(s / 60)); }
  else              { snprintf(out, n, tr("%lu s"), (unsigned long)s); }
}

void tab5_escrever(lgfx::LovyanGFX& d, const char* t, int x, int y, int px, bool negrito,
                   uint16_t cor, uint16_t fundo, uint8_t datum, int max_l)
{
  char b[256];
  snprintf(b, sizeof(b), "%s", t ? t : "");
  d.setFont(tab5_fonte_suave(negrito, px));
  d.setTextSize(1);
  if (max_l > 0) { tab5_cortar_com_reticencias(d, b, sizeof(b), max_l); }
  d.setTextDatum(datum);
  d.setTextColor(cor, fundo);
  d.drawString(b, x, y);
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
}

void tab5_bytes_txt(char* out, size_t n, uint64_t b)
{
  if      (b >= 1024ULL * 1024 * 1024) { snprintf(out, n, "%.1f GB", b / (1024.0 * 1024 * 1024)); }
  else if (b >= 1024ULL * 1024)        { snprintf(out, n, "%.1f MB", b / (1024.0 * 1024)); }
  else if (b >= 1024ULL)               { snprintf(out, n, "%.1f KB", b / 1024.0); }
  else                                 { snprintf(out, n, "%llu B", (unsigned long long)b); }
}

const char* tab5_mono(lgfx::LovyanGFX& d, const char* t, int x, int y_base, int cw, int cols)
{
  char run[129];
  int  n = 0, col0 = 0, col = 0;
  d.setFont(&fonts::FreeMono12pt7b);
  d.setTextSize(1);
  d.setTextDatum(baseline_left);
  auto soltar = [&]() {
    if (n) { run[n] = 0; d.drawString(run, x + col0 * cw, y_base); n = 0; }
  };
  while (*t && col < cols) {
    const uint8_t c = (uint8_t)*t;
    if (c < 0x80) {
      if (!n) { col0 = col; }
      run[n++] = (char)c;
      if (n == (int)sizeof(run) - 1) { soltar(); }
      ++t; ++col;
      continue;
    }
    soltar();
    int k = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    char g[5];
    int m = 0;
    for (; m < k && t[m]; ++m) { g[m] = t[m]; }
    g[m] = 0;
    t += m;
    tab5_fonte_corpo(d);
    tab5_texto(d, 2.2f);
    d.setTextDatum(baseline_center);
    d.drawString(g, x + col * cw + cw / 2, y_base);
    d.setFont(&fonts::FreeMono12pt7b);
    d.setTextSize(1);
    d.setTextDatum(baseline_left);
    ++col;
  }
  soltar();
  return t;
}
