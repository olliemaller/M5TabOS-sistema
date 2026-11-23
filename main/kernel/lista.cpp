#include "lista.hpp"
#include "internal.hpp"   // diag_toque_ligado
#include "esp_log.h"
#include <algorithm>
#include "app.h"   // sistema_area - touch arrives in area coordinates

static const int BARRA_MARGEM = 8;   // rolagem_barra draws 4px to 8px from the edge
static const int BARRA_LARG   = 4;

void lista_definir(lista_t* l, uint8_t ancoragem, int n,
                   lista_altura_t altura, lista_desenhar_t desenhar, void* ctx)
{
  rolagem_definir(&l->rol, ROLAGEM_VERTICAL, ancoragem);
  l->n = n;
  l->altura = altura;
  l->desenhar = desenhar;
  l->ctx = ctx;
  l->topo = 0;
  l->total = 0;
  l->medida_ok = false;
  l->tudo = true;
  l->janela_ant = { 0, 0, 0, 0 };
  l->no_ar = false;
  l->sujo_de = 1; l->sujo_ate = 0;
}

static int somar(const lista_t* l)
{
  int t = l->topo;
  for (int i = 0; i < l->n; ++i) { t += l->altura(i, l->ctx); }
  return t;
}

void lista_itens_mudaram(lista_t* l, int n)
{
  const int antes = l->medida_ok ? l->total : -1;
  l->n = n;
  l->total = somar(l);
  l->medida_ok = true;
  l->tudo = true;

  // READING HISTORY WHEN SOMETHING NEW ARRIVES BELOW. In a list anchored at the
  // end, 'atual' counts from the bottom: if the content grows and 'atual' stays
  // the same, everything you are reading moves up with it. Adding the growth, what
  // is on screen stays put. Stuck to the end (atual == 0) stays stuck:
  // the new message appears.
  if (l->rol.ancoragem == ROLAGEM_FIM && antes >= 0 && l->rol.atual > 0 && l->total > antes) {
    l->rol.atual += l->total - antes;
    l->rol.no_visor = l->rol.atual;
  }
}

void lista_item_mudou(lista_t* l, int i)
{
  if (i < 0) { return; }
  if (l->sujo_de > l->sujo_ate) { l->sujo_de = l->sujo_ate = i; return; }
  if (i < l->sujo_de) { l->sujo_de = i; }
  if (i > l->sujo_ate) { l->sujo_ate = i; }
}

void lista_ao_inicio(lista_t* l)
{
  rolagem_zerar(&l->rol);
  l->tudo = true;
}

void lista_esconder(lista_t* l)
{
  l->no_ar = false;
  l->rol.ancora = -1;
  l->tudo = true;   // reappearing, the window was painted by another screen
}

// Where item 0 starts, in screen coordinates.
static int y_do_primeiro(const lista_t* l, const area_t& j)
{
  int y0;
  if (l->rol.ancoragem == ROLAGEM_FIM) {
    if (l->total <= j.altura) { y0 = j.y + j.altura - l->total; }   // few items: stuck to the bottom
    else                      { y0 = j.y - (l->total - j.altura - l->rol.atual); }
  } else {
    y0 = j.y - l->rol.atual;
  }
  return y0 + l->topo;
}

static int largura_item(const area_t& j) { return j.largura - BARRA_MARGEM - BARRA_LARG; }

static bool dentro_do_recorte(lgfx::LovyanGFX& d, const area_t& j)
{
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  return j.x >= cx && j.y >= cy && j.x + j.largura <= cx + cl && j.y + j.altura <= cy + ca;
}

// Draws the items that cross 'rec' (already clipped by the caller).
static void desenhar_faixa(lista_t* l, lgfx::LovyanGFX& d, const area_t& j, const area_t& faixa, uint16_t fundo)
{
  // INSIDE THE CLIP THAT IS PASSED IN. With the whole screen scrolling (landscape app), the
  // system clips the app to its area; replacing that clip with the strip's
  // let the list window - which scrolled with it - paint over the bars.
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  const int x0 = std::max<int>(faixa.x, cx), y0 = std::max<int>(faixa.y, cy);
  const int x1 = std::min<int>(faixa.x + faixa.largura, cx + cl);
  const int y1 = std::min<int>(faixa.y + faixa.altura, cy + ca);
  if (x1 <= x0 || y1 <= y0) { return; }
  const area_t rec = { x0, y0, x1 - x0, y1 - y0 };
  d.fillRect(rec.x, rec.y, rec.largura, rec.altura, fundo);
  d.setClipRect(rec.x, rec.y, rec.largura, rec.altura);

  const int fim = rec.y + rec.altura;
  int y = y_do_primeiro(l, j);
  for (int i = 0; i < l->n && y < fim; ++i) {
    const int h = l->altura(i, l->ctx);
    if (y + h > rec.y) {
      l->desenhar(d, i, j.x, y, largura_item(j), l->ctx);
      // THE CLIP BELONGS TO THE LIST, not the item. An M5GFX primitive that touches it
      // (drawWideLine swaps it for the line's box and then clears it) let
      // the following items loose over the header and footer - a trail at both
      // ends of the window on every scroll step.
      d.setClipRect(rec.x, rec.y, rec.largura, rec.altura);
    }
    y += h;
  }

  d.setClipRect(cx, cy, cl, ca);
}

// Only the items marked by lista_item_mudou, and only their visible part. The
// bar column is left out: an item never draws in it.
static bool desenhar_sujos(lista_t* l, lgfx::LovyanGFX& d, const area_t& j, uint16_t fundo)
{
  if (l->sujo_de > l->sujo_ate) { return false; }
  bool pintou = false;
  int y = y_do_primeiro(l, j);
  for (int i = 0; i < l->n && i <= l->sujo_ate && y < j.y + j.altura; ++i) {
    const int h = l->altura(i, l->ctx);
    if (i >= l->sujo_de) {
      int y0 = y, y1 = y + h;
      if (y0 < j.y) { y0 = j.y; }
      if (y1 > j.y + j.altura) { y1 = j.y + j.altura; }
      if (y1 > y0) {
        const area_t rec = { j.x, y0, largura_item(j), y1 - y0 };
        desenhar_faixa(l, d, j, rec, fundo);
        pintou = true;
      }
    }
    y += h;
  }
  l->sujo_de = 1; l->sujo_ate = 0;
  return pintou;
}

bool lista_desenhar(lista_t* l, lgfx::LovyanGFX& d, const area_t& j, uint16_t fundo, bool tudo)
{
  if (!l->medida_ok) { lista_itens_mudaram(l, l->n); }
  l->no_ar = true;

  // The window changed (the keyboard opened or closed, the screen rotated): everything again.
  if (j.x != l->janela_ant.x || j.y != l->janela_ant.y
   || j.largura != l->janela_ant.largura || j.altura != l->janela_ant.altura) {
    l->janela_ant = j;
    tudo = true;
  }

  // The limit depends on the total and the window - and may pull 'atual' back.
  if (rolagem_limite(&l->rol, l->total, j.altura)) { tudo = true; }

  tudo = tudo || l->tudo;
  if (tudo) {
    desenhar_faixa(l, d, j, j, fundo);
    rolagem_sincronizar(&l->rol);
    l->sujo_de = 1; l->sujo_ate = 0;   // the whole window has already covered the marked ones
  } else if (rolagem_pendente(&l->rol) && !dentro_do_recorte(d, j)) {
    // Moving the window's pixels would take along what is outside the clip (the
    // bars): draw again, clipped.
    desenhar_faixa(l, d, j, j, fundo);
    rolagem_sincronizar(&l->rol);
    tudo = true;
  } else if (rolagem_pendente(&l->rol)) {
    const rolagem_passada_t p = rolagem_deslocar(&l->rol, j, fundo);
    if (p.deslocou) { desenhar_faixa(l, d, j, p.exposta, fundo); }
    else            { desenhar_faixa(l, d, j, j, fundo); tudo = true; }
    desenhar_sujos(l, d, j, fundo);    // after the shift: new positions
  } else {
    return desenhar_sujos(l, d, j, fundo);   // values only; the bar has not changed
  }
  l->tudo = false;

  // THE BAR. The shift dragged its pixels along with the content:
  // the column is always rewritten in full - track + dot, or just background when
  // it should not appear (in a list anchored at the end, sitting at the end).
  const area_t col = { j.x + j.largura - BARRA_MARGEM, j.y, BARRA_LARG, j.altura };
  d.fillRect(col.x, col.y, col.largura, col.altura, fundo);
  rolagem_barra(d, &l->rol, j, fundo, BARRA_MARGEM);
  return true;
}

// ---------------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------------
// The stored window is in SCREEN coordinates; touch arrives in app AREA coordinates.
static bool na_janela(const lista_t* l, int x, int y, int* ty)
{
  if (!l->no_ar) { return false; }
  const area_t a = sistema_area();
  const int tx = x + a.x;
  *ty = y + a.y;
  const area_t& j = l->janela_ant;
  return tx >= j.x && tx < j.x + j.largura && *ty >= j.y && *ty < j.y + j.altura;
}

bool lista_arrastar(lista_t* l, int x, int y, bool pressionado)
{
  int ty = 0;
  const bool dentro = pressionado && na_janela(l, x, y, &ty) && l->rol.max > 0;
  return rolagem_arrastar(&l->rol, ty, dentro);
}

int lista_item_tocado(lista_t* l, int x, int y, int* dy)
{
  const int afastou = l->rol.afastamento;
  if (rolagem_foi_arraste(&l->rol)) {   // the end of a drag is not a click
    if (diag_toque_ligado()) { ESP_LOGI("lista", "touch rejected as a list drag (%d px)", afastou); }
    return LISTA_ARRASTE;
  }
  return lista_item_em(l, x, y, dy);
}

int lista_item_em(lista_t* l, int x, int y, int* dy)
{
  int ty = 0;
  if (!na_janela(l, x, y, &ty)) { return -1; }
  int yi = y_do_primeiro(l, l->janela_ant);
  for (int i = 0; i < l->n; ++i) {
    const int h = l->altura(i, l->ctx);
    if (ty >= yi && ty < yi + h) {
      if (dy) { *dy = ty - yi; }
      return i;
    }
    yi += h;
  }
  return -1;
}
