#include "compositor.hpp"

#include "tab5_tela.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <atomic>

static const char *TAG = "comp";

static const int FB_L = 720;
static const int FB_A = 1280;

// ---------------------------------------------------------------------------
// DIRTY AREAS
// ---------------------------------------------------------------------------
// Merging two areas is only worth it when the rectangle enclosing them is no larger than
// the two combined - otherwise we redraw pixels nobody asked for. It is the same
// rule as LVGL's (lv_refr.c, refr_join_area).
//
// 32 and not 16: a screen with scattered text (top bar, values in the middle,
// bottom bar) fills 16 easily, and what happens when it fills up is
// expensive - see estourou().
//
// 96 and not 32: ANIMATIONS WITH MANY SMALL OBJECTS. Particles, flakes,
// stars - each is a little box, and 32 filled up in the middle of a fireworks
// burst. Once full, the list merges distant boxes into fat rectangles, and the
// next page copies all the excess: the frame got heavy just as the screen
// filled, and the animation stuttered. Animators had to merge their own
// boxes to fit here - a concern that isn't an app's. It costs 1.5 KB more of internal
// RAM (three lists); the path of each write stays short
// (see s_ult_contem).
#define MAX_AREAS 96

typedef struct { tab5_ret_t r[MAX_AREAS]; int n; } areas_t;

static areas_t s_sujas;      // what will be drawn in the next frame
static areas_t s_anterior;   // what the previous frame drew (missing from the outgoing page)
static uint32_t s_estouros = 0;

static inline int area(const tab5_ret_t& r) { return r.l * r.a; }

static tab5_ret_t envolver(const tab5_ret_t& a, const tab5_ret_t& b)
{
  const int x0 = a.x < b.x ? a.x : b.x;
  const int y0 = a.y < b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) > (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) > (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// How many UNREQUESTED pixels merging a and b forces us to copy.
static inline int desperdicio(const tab5_ret_t& a, const tab5_ret_t& b)
{
  return area(envolver(a, b)) - area(a) - area(b);
}

static bool contem(const tab5_ret_t& fora, const tab5_ret_t& dentro)
{
  return dentro.x >= fora.x && dentro.y >= fora.y
      && dentro.x + dentro.l <= fora.x + fora.l
      && dentro.y + dentro.a <= fora.y + fora.a;
}

static bool recortar_na_tela(tab5_ret_t& r)
{
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.l > FB_L ? FB_L : r.x + r.l;
  int y1 = r.y + r.a > FB_A ? FB_A : r.y + r.a;
  if (x1 <= x0 || y1 <= y0) { return false; }
  r = { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
  return true;
}

// The area that covered the last write. Almost every write falls inside the same
// area as the previous one (the lines of a circle, the glyphs of a text, the
// drawing on top of the box that was just erased): checking it first
// saves scanning the whole list, which is now long.
static int s_ult_contem = 0;

void comp_invalidar(const tab5_ret_t& entrada)
{
  tab5_ret_t r = entrada;
  if (!recortar_na_tela(r)) { return; }
  if (s_ult_contem < s_sujas.n && contem(s_sujas.r[s_ult_contem], r)) { return; }

  for (;;) {
    // Already covered: nothing to do.
    for (int i = 0; i < s_sujas.n; ++i) { if (contem(s_sujas.r[i], r)) { s_ult_contem = i; return; } }

    // Merge while it pays off. One merge can enable another, so repeat.
    bool fundiu = true;
    while (fundiu) {
      fundiu = false;
      for (int i = 0; i < s_sujas.n; ++i) {
        if (desperdicio(s_sujas.r[i], r) <= 0) {
          r = envolver(s_sujas.r[i], r);
          s_sujas.r[i] = s_sujas.r[--s_sujas.n];
          fundiu = true;
          break;
        }
      }
    }

    if (s_sujas.n < MAX_AREAS) { s_sujas.r[s_sujas.n++] = r; return; }

    // LIST FULL. The first version wrapped EVERYTHING in a single rectangle - and the
    // top bar plus a number down at the bottom became the whole screen (1.8 MB
    // of copying for a handful of text). Now the new area joins the NEIGHBOUR
    // that wastes least: it frees a slot without fattening what is far away. (The
    // previous version searched for the best PAIR among all of them - n^2 on every overflow,
    // too expensive with the long list; with 96 slots overflow became rare and the
    // difference in excess between the two approaches small.)
    ++s_estouros;
    int bi = 0;
    int melhor = 0x7fffffff;
    for (int i = 0; i < s_sujas.n; ++i) {
      const int dr = desperdicio(s_sujas.r[i], r);
      if (dr < melhor) { melhor = dr; bi = i; }
    }
    r = envolver(s_sujas.r[bi], r);
    s_sujas.r[bi] = s_sujas.r[--s_sujas.n];
    // Back to the top: the merged area may now cover or merge with others.
  }
}

// ---------------------------------------------------------------------------
// FRAME STATE
// ---------------------------------------------------------------------------
static bool     s_aberto   = false;   // the back page is synchronised and in use
static bool     s_ativo    = false;   // we know the current panel's pages
static bool     s_pendente = false;   // we requested a switch and haven't confirmed yet
static uint32_t s_geracao  = 0;       // which panel the pages we know belong to
static uint32_t s_geracao_vista = 0;  // last panel announced to the core (comp_painel_novo)
static void*    s_tras     = nullptr; // the back page of the open frame
static uint32_t s_epoca    = 0;       // g_tab5_tela_epoca when the frame opened
static uint32_t s_rodada   = 0;
static int64_t  s_ult_quadro_us = 0;  // diagnostics: when the last frame went out
static uint32_t s_ult_quadro_px = 0;  // and how many pixels it changed
static std::atomic<bool> s_pedido_tudo{false};

// LAZY SYNC. The areas of the previous frame that the back page
// does NOT have yet. They are not copied on opening: each comes from the front only when
// something is about to touch it (write or read - comp_preparar), and whatever is left,
// at presentation time. One that is entirely overwritten by a DMA copy
// (scrolling, surface) is never copied.
//
// Why: the first version synced EVERYTHING on opening. But the frame opens
// on the round's first write - and apps draw the header before the
// list. When scrolling arrived saying "I'll cover the whole window", the
// window had already been copied: measured, sync was 2/3 of all copying.
static areas_t s_pend;
static void*   s_frente_pg = nullptr;   // the front page of the open frame

static comp_estat_t s_est = {};       // the measurement (see comp_estat)
static uint64_t     s_us_dentro = 0;  // time spent in here (open + copies)
static uint64_t     s_marca_dentro = 0;

static bool intersecta(const tab5_ret_t& a, const tab5_ret_t& b)
{
  return a.x < b.x + b.l && b.x < a.x + a.l && a.y < b.y + b.a && b.y < a.y + a.a;
}

// Every compositor copy goes through here: it is where DMA time is counted.
static bool copiar(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy)
{
  const int64_t t0 = esp_timer_get_time();
  const bool ok = tab5_quadro_copiar_de(destino, r, origem, ox, oy);
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_copia += dt;
  s_us_dentro += dt;
  return ok;
}

static void esperar_troca_pendente(void)
{
  if (!s_pendente) { return; }
  const int64_t t0 = esp_timer_get_time();
  if (!tab5_quadro_esperar_troca(50)) { ESP_LOGW(TAG, "switch not confirmed within 50 ms"); }
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_espera += dt;
  s_us_dentro += dt;
  s_pendente = false;
}

// Discards the frame in progress: the panel vanished (idle) or was swapped.
// Whatever was drawn is lost on purpose - the core redraws everything
// when the new panel appears (comp_painel_novo).
static void descartar(void)
{
  s_aberto = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  s_sujas.n = 0;
  s_pend.n = 0;
  // New round: the tracker's shortcut ("this rectangle has already been noted and
  // prepared this round") must not carry over to the next frame - it
  // would skip comp_preparar, and the presentation sync would erase the drawing.
  ++s_rodada;
}

// PENDING AREAS ARE CLIPPED, not copied whole. Area merging joins
// neighbours (an app's header and the list window just below become ONE
// pending rectangle). Copying the whole pending area because the header was
// touched brought the window along - 1.6 MB that scrolling overwrites right afterwards.
// Now only the INTERSECTION comes (or is discarded), and the rest of the pending area stays
// pending, in up to 4 pieces.
static tab5_ret_t intersecao(const tab5_ret_t& a, const tab5_ret_t& b)
{
  const int x0 = a.x > b.x ? a.x : b.x;
  const int y0 = a.y > b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) < (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) < (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Removes pending area i and returns to the list what of it lies OUTSIDE 'r' (top,
// bottom, left, right strips). false = didn't fit in the list: nothing was
// touched, and the caller copies the whole pending area.
static bool recortar_pendente(int i, const tab5_ret_t& r)
{
  const tab5_ret_t p = s_pend.r[i];
  const tab5_ret_t c = intersecao(p, r);
  tab5_ret_t pedaco[4];
  int n = 0;
  if (c.y > p.y)                 { pedaco[n++] = { p.x, p.y, p.l, (int16_t)(c.y - p.y) }; }
  if (c.y + c.a < p.y + p.a)     { pedaco[n++] = { p.x, (int16_t)(c.y + c.a), p.l, (int16_t)(p.y + p.a - c.y - c.a) }; }
  if (c.x > p.x)                 { pedaco[n++] = { p.x, c.y, (int16_t)(c.x - p.x), c.a }; }
  if (c.x + c.l < p.x + p.l)     { pedaco[n++] = { (int16_t)(c.x + c.l), c.y, (int16_t)(p.x + p.l - c.x - c.l), c.a }; }
  if (s_pend.n - 1 + n > MAX_AREAS) { return false; }
  s_pend.r[i] = s_pend.r[--s_pend.n];
  for (int k = 0; k < n; ++k) { s_pend.r[s_pend.n++] = pedaco[k]; }
  return true;
}

static void copiar_pendente(const tab5_ret_t& a)
{
  copiar(s_tras, a, s_frente_pg, a.x, a.y);
  s_est.px_sinc += (uint32_t)area(a);
}

// Brings in pending area i whole and removes it from the list (swaps with the last - whoever
// iterates back to front loses nobody).
static void sincronizar(int i)
{
  const tab5_ret_t a = s_pend.r[i];
  s_pend.r[i] = s_pend.r[--s_pend.n];
  copiar_pendente(a);
}

// Iterates back to front: the pieces appended at the end lie OUTSIDE
// 'r' and don't need to be revisited.
void comp_preparar(const tab5_ret_t& r)
{
  if (!s_aberto || s_pend.n == 0) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    const tab5_ret_t c = intersecao(s_pend.r[i], r);
    if (recortar_pendente(i, r)) { copiar_pendente(c); }
    else                         { sincronizar(i); }
  }
}

void comp_cobrir(const tab5_ret_t& r)
{
  if (!s_aberto) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    if (contem(r, s_pend.r[i])) { s_pend.r[i] = s_pend.r[--s_pend.n]; continue; }
    if (!recortar_pendente(i, r)) { sincronizar(i); }   // no slot: comes whole (correct, just more expensive)
  }
}

void* comp_abrir(void)
{
  if (s_aberto) {
    // THE PATH OF EVERY WRITE (every letter-edge pixel passes here):
    // one comparison, nothing more. An open frame must not cross the
    // panel swap (idle, clock) - its page was freed along
    // with the old panel -, and the page epoch goes up precisely then.
    if (s_epoca == g_tab5_tela_epoca) { return s_tras; }
    descartar();
  }

  // No panel (idle): nothing to open.
  if (!tab5_tela_iniciar()) { return nullptr; }

  // NEW PANEL (idle, clock change, boot): the pages we
  // knew no longer exist, and the new ones contain nothing of ours - there is nothing
  // to sync. The core has already been (or will be) notified by
  // comp_painel_novo() and redraws the whole screen.
  if (!s_ativo || tab5_tela_geracao() != s_geracao) {
    s_geracao = tab5_tela_geracao();
    s_ativo = true;
    s_pendente = false;
    s_anterior.n = 0;
  }

  // 1. The back page is only free after the panel has switched.
  esperar_troca_pendente();
  void* tras   = tab5_tela_pagina(true);
  void* frente = tab5_tela_pagina(false);
  if (!tras || !frente) { return nullptr; }

  // 2. Sync, LAZY: what the previous frame drew stays
  //    pending, and comes from the front only when touched (see s_pend).
  s_pend = s_anterior;
  s_anterior.n = 0;

  s_tras = tras;
  s_frente_pg = frente;
  s_epoca = g_tab5_tela_epoca;
  s_aberto = true;
  return s_tras;
}

bool comp_desenhado_neste_quadro(const tab5_ret_t& src)
{
  for (int i = 0; i < s_sujas.n; ++i) { if (intersecta(s_sujas.r[i], src)) { return true; } }
  return false;
}

bool comp_copiar_da_frente(const tab5_ret_t& r, int ox, int oy)
{
  void* tras = comp_abrir();
  if (!tras) { return false; }
  if (!s_frente_pg || s_frente_pg == tras) { return false; }
  if (!copiar(tras, r, s_frente_pg, ox, oy)) { return false; }
  comp_cobrir(r);   // only AFTER it succeeds: if it failed, the pending areas remain
  s_est.px_rolar_dma += (uint32_t)area(r);
  return true;
}

bool comp_copiar_imagem(const void* imagem, const tab5_ret_t& r)
{
  void* tras = comp_abrir();
  if (!tras || !imagem) { return false; }
  if (!copiar(tras, r, imagem, r.x, r.y)) { return false; }
  comp_cobrir(r);
  comp_invalidar(r);
  return true;
}

bool comp_copiar_faixa(const void* faixa, int y0, int alt)
{
  void* tras = comp_abrir();
  if (!tras || !faixa || y0 < 0 || alt <= 0 || y0 + alt > (int)FB_A) { return false; }
  const tab5_ret_t r = { 0, (int16_t)y0, (int16_t)FB_L, (int16_t)alt };
  // The band is an image FB_L wide starting at its own line 0.
  if (!copiar(tras, r, faixa, 0, 0)) { return false; }
  comp_cobrir(r);
  comp_invalidar(r);
  return true;
}

void comp_invalidar_tudo(void) { s_pedido_tudo.store(true, std::memory_order_relaxed); }

bool comp_apresentar(void)
{
  if (s_pedido_tudo.exchange(false, std::memory_order_relaxed)) {
    comp_invalidar({ 0, 0, (int16_t)FB_L, (int16_t)FB_A });
  }
  if (s_sujas.n == 0) { return false; }

  // Marked with nothing drawn (just an invalidate request): opens here, so
  // the back page matches the front before going on air.
  if (!s_aberto && !comp_abrir()) { descartar(); return false; }

  // The panel vanished or was swapped AFTER opening: the page we
  // drew on no longer exists.
  if (!tab5_tela_iniciar() || tab5_tela_geracao() != s_geracao) { descartar(); return false; }

  // Whatever nobody touched is still pending: it comes now, otherwise the back page
  // would go on air with the frame from two switches ago at those points.
  while (s_pend.n > 0) { sincronizar(s_pend.n - 1); }

  int y_min = FB_A, y_max = 0;
  uint32_t px = 0;
  for (int k = 0; k < s_sujas.n; ++k) {
    const tab5_ret_t& a = s_sujas.r[k];
    if (a.y < y_min) { y_min = a.y; }
    if (a.y + a.a > y_max) { y_max = a.y + a.a; }
    px += (uint32_t)area(a);
  }

  // 3. The switch. The driver writes back from cache lines y_min..y_max of the back
  //    page (what the CPU wrote that is still only in cache) and changes the
  //    index. If it refuses, the frame stays open and tries again on the next
  //    round - closing here would make the sync erase what was drawn.
  if (!tab5_tela_pedir_troca(y_min, y_max)) { return false; }
  s_pendente = true;
  s_anterior = s_sujas;
  s_sujas.n = 0;
  s_aberto = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  ++s_rodada;

  s_ult_quadro_us = esp_timer_get_time();
  s_ult_quadro_px = px;
  ++s_est.quadros;
  s_est.px_quadro += px;
  if (px >= (uint32_t)FB_L * FB_A / 10 * 9) { ++s_est.cheios; }
  return true;
}

bool comp_painel_novo(void)
{
  if (!tab5_tela_iniciar()) { return false; }
  const uint32_t g = tab5_tela_geracao();
  if (g == s_geracao_vista) { return false; }
  s_geracao_vista = g;
  return true;
}

bool comp_tem_sujeira(void) { return s_sujas.n > 0 || s_pedido_tudo.load(std::memory_order_relaxed); }
void comp_esperar_troca(void) { esperar_troca_pendente(); }
uint32_t comp_rodada(void) { return s_rodada; }

void comp_ultimo_quadro(int64_t* quando_us, uint32_t* px) { if (quando_us) { *quando_us = s_ult_quadro_us; } if (px) { *px = s_ult_quadro_px; } }

void comp_marcar_desenho(void) { s_marca_dentro = s_us_dentro; }

uint32_t comp_contar_desenho(uint32_t us, bool completo)
{
  const uint64_t dentro = s_us_dentro - s_marca_dentro;
  const uint64_t cpu = (us > dentro) ? (us - dentro) : 0;
  s_est.us_desenho += cpu;
  ++s_est.n_desenho;
  if (completo) { s_est.us_desenho_completo += cpu; ++s_est.n_desenho_completo; }
  return (uint32_t)cpu;
}

void comp_contar_volta(uint32_t us)
{
  // The panel's MEASURED period (hal/tab5_quadro), not a fixed number: the
  // panel clock is a user setting (59 MHz = 20.6 ms, 80 MHz = 15.7).
  uint32_t periodo = 0;
  tab5_quadro_atrasos(nullptr, &periodo);
  if (periodo == 0) { periodo = 16667; }
  uint32_t f = us / periodo;          // 0 = fitted within 1 period
  if (f > COMP_FAIXAS - 1) { f = COMP_FAIXAS - 1; }
  s_est.us_volta += us;
  ++s_est.voltas[f];
}

// Read from another task (console, monitor) without a lock: 64-bit fields
// may come out torn at a rare instant. It is diagnostics - a skewed number in one
// window disappears in the next; a lock here would weigh on every frame.
void comp_estat(comp_estat_t* out)
{
  if (!out) { return; }
  *out = s_est;
  out->estouros = s_estouros;
  tab5_tela_rolagem_custo(&out->px_rolados, &out->us_rolar);
}
