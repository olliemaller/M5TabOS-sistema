// ENERGIA (POWER) APP - the battery status is now two charts in the style
// of phones (the battery screen on Android and iOS):
//
//   1. The CHOSEN QUANTITY over time (by default the battery level), with a
//      filled area, an axis with round numbers and the CHARGING hours tinted
//      green behind the curve: charge and discharge read from the shape,
//      without having to read numbers. Period 30 MIN (RAM ring) or 24 H.
//   2. CONSUMPTION PER HOUR (mAh) over the last 24 h, as bars: screen on at
//      the top, idle at the bottom, and a green strip under the hours when it
//      was charging.
//
// In 24 h mode both charts share the SAME time axis (one column per clock
// hour, the last being the current hour): each bar sits under the stretch of
// the curve it refers to.
//
// At the top: the large percentage with the estimate ("dura ~X" / "cheia em
// ~X") and an INDEX of six cards with the current values. Tapping a card
// chooses the quantity for chart 1. Tapping chart 1 places a cursor with the
// time and value; tapping a bar shows the detail for that hour.
//
// 24 h data: yesterday's and today's files (services/registro, one sample
// per minute, with the real time), read by the WORKER, topped up with the
// 30 min RAM ring - what the log has not written yet (5 min batches).
//
// Redraw by REGION: top (hero + cards) every second; the two charts, with
// their titles, only when new data arrives or a selection changes.
// No tap asks for a full pass: each region clears its own rectangle.

#include "app.h"
#include "idioma.h"
#include "tab5_tela.hpp"
#include "pintura.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_hora.hpp"
#include "trabalho.hpp"
#include "registro.h"
#include "tab5_power.hpp"
#include "tab5_sensores.hpp"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef enum { SERIE_BATERIA = 0, SERIE_CORRENTE, SERIE_POTENCIA, SERIE_TENSAO, SERIE_TEMP } serie_t;
#define N_SERIES 5
static const char* SERIE_TITULO[N_SERIES] = { N_("Nível da bateria (%)"), N_("Corrente (mA)"), N_("Potência (W)"), N_("Tensão (V)"), N_("Temperatura P4 (°C)") };
static const char* SERIE_CARTAO[N_SERIES] = { N_("Bateria"), N_("Corrente"), N_("Potência"), N_("Tensão"), N_("Temp P4") };
// DATA colours (one per series), in the theme's equal-lightness tones.
static const uint16_t SERIE_COR[N_SERIES] = {
  TEMA_APP_AZUL,       // battery: blue, so it is not confused with the charging green
  TEMA_APP_LARANJA,
  TEMA_APP_LIMA,
  TEMA_APP_ROXO,
  TEMA_APP_VERMELHO,
};

#define COR_CARGA        TEMA_SUCESSO   // charging: the same green throughout the app
#define COR_CARGA_FUNDO  lgfx::color565( 14,  50,  26)   // tint for the charging hours behind the curve
#define COR_GASTO_TELA   TEMA_APP_LARANJA
#define COR_GASTO_REP    lgfx::color565(130,  75,  20)
#define COR_CARTAO       TEMA_SUPERFICIE
#define COR_CARTAO_SEL   TEMA_SUPERFICIE_ALTA
#define COR_EIXO         TEMA_TEXTO_3

static serie_t s_serie   = SERIE_BATERIA;
static bool    s_24h     = true;      // chart 1: true = 24 H (SD card + RAM); false = 30 MIN (RAM)
static int     s_cursor  = -1;        // cursor x in chart 1 (relative to the area); -1 = none
static int     s_hora_sel = -1;       // bar chosen in chart 2 (0..23); -1 = totals
static bool    s_graf_sujo = true;    // a tap changed what the charts show
static pintura_t s_p;

// ---------------------------------------------------------------------------
// 24 H: one slot per minute of the window, loaded by the worker
// ---------------------------------------------------------------------------
// The window starts on the hour 23 h ago: 24 whole-hour columns, the last
// being the current (partial) hour. Aligning to the clock hour is what gives
// chart 2's bars labels like "14h", "15h" instead of 60 min slices starting
// at some arbitrary minute.
#define MIN_DIA REGISTRO_MIN_DIA
typedef registro_dia_t dia_t;   // the day in minutes, as the log delivers it

// A source of points: n points, x from 0..1, the values, and the instant 't'
// (24 H: the slot, minutes since t0; 30 MIN: seconds before now).
typedef struct {
  int     n;
  float   x[MIN_DIA], v[MIN_DIA], ma[MIN_DIA], temp[MIN_DIA];
  int8_t  pct[MIN_DIA];
  uint8_t rep[MIN_DIA];        // 2 = idle; anything else = screen on (or unknown)
  bool    quebra[MIN_DIA];     // gap before this point: device was off
  int32_t t[MIN_DIA];
} pontos_t;

// One vertex per pixel column of chart 1 (see montar_vertices).
#define VERT_MAX 1280   // the landscape width (physical keyboard); portrait uses 720
typedef struct {
  int     n;
  int16_t px[VERT_MAX];
  float   val[VERT_MAX];
  bool    carga[VERT_MAX], quebra[VERT_MAX];
  int32_t t[VERT_MAX];
} vertices_t;

typedef struct { float tela, repouso, carga; int min_tela; bool tem; } hora_t;

// Everything only the UI uses, in one block in PSRAM allocated on open.
typedef struct {
  pontos_t     p24, p30;
  vertices_t   vt;
  tab5_ponto_t ram[TAB5_HIST_N];
  hora_t       horas[24];
  float        tot_gasto, tot_carga, tot_wh;
  int          tot_min_tela;
} memoria_t;
static memoria_t* s_m = nullptr;

static dia_t*        s_dia = nullptr;      // what the screen shows
static dia_t*        s_carga = nullptr;    // where the worker writes
static uint32_t      s_carga_t0 = 0;
static volatile bool s_carga_pronta = false;
static bool          s_carga_pedida = false;
static uint32_t      s_gravadas_lidas = UINT32_MAX;   // log already reflected in the 24 h
static uint32_t      s_dia_ver = 0;                    // goes up with each new load

// Start of the 24 h window for the current instant; 0 without the clock set.
static uint32_t janela_t0(void)
{
  const uint32_t agora = tab5_hora_epoch();
  if (!agora) { return 0; }
  struct tm lt;
  tab5_hora_local(agora, &lt);
  return agora - (uint32_t)(lt.tm_min * 60 + lt.tm_sec) - 23u * 3600u;
}

static void trabalho_24h(void*)
{
  // Reading and parsing the files is the log service's job; only the finished day arrives here.
  registro_ler_dia(s_carga, s_carga_t0);
  s_carga_pronta = true;
}

static void pedir_24h(void)
{
  if (s_carga_pedida || !s_carga) { return; }
  s_carga_t0 = janela_t0();
  s_carga_pronta = false;
  s_carga_pedida = sistema_trabalho(trabalho_24h, nullptr);
}

static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carga_pronta) {
    s_carga_pronta = false;
    s_carga_pedida = false;
    dia_t* t = s_dia; s_dia = s_carga; s_carga = t;
    ++s_dia_ver;
    sistema_pedir_redesenho();
  }
}

static bool ao_abrir(void)
{
  if (!s_dia)   { s_dia   = (dia_t*)heap_caps_calloc(1, sizeof(dia_t), MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (dia_t*)heap_caps_calloc(1, sizeof(dia_t), MALLOC_CAP_SPIRAM); }
  if (!s_m)     { s_m     = (memoria_t*)heap_caps_calloc(1, sizeof(memoria_t), MALLOC_CAP_SPIRAM); }
  if (!s_dia || !s_carga || !s_m) { return false; }
  s_carga_pedida = false;
  s_carga_pronta = false;
  s_gravadas_lidas = UINT32_MAX;   // the 24 h are read on open
  s_cursor = -1;
  s_hora_sel = -1;
  s_graf_sujo = true;
  pintura_pedir(&s_p);
  return true;
}

static void ao_fechar(void)
{
  // (The system has already waited for the worker before calling this.)
  heap_caps_free(s_dia);   s_dia = nullptr;
  heap_caps_free(s_carga); s_carga = nullptr;
  heap_caps_free(s_m);     s_m = nullptr;
}

// ---------------------------------------------------------------------------
// GEOMETRY (relative to the area, 720 x 1144 in portrait; ends at ~1120) - the same for drawing and touch
// ---------------------------------------------------------------------------
static const int M = 16;                               // side margin
// THE AJUSTES (SETTINGS) HEADER: Energia lives inside it (the Energia row
// opens this app), so < VOLTAR and the title look like the pages there - and
// VOLTAR returns to Ajustes, not to the home screen.
static const int VOLTAR_L = 200, VOLTAR_A = 64;
static const int BARRA_A = 16 + VOLTAR_A + 12;
static const int HERO_Y = BARRA_A + 8, HERO_A = 76;    // large % + status
static const int CART_Y = HERO_Y + HERO_A + 8;         // index: 3 x 2 cards
static const int CART_A = 92, CART_V = 12;             // height and gap between cards
// ONE LINE under the cards: what is holding off idle right now (kernel/recursos.h) -
// the answer to "why is it draining with the screen off".
static const int REC_Y = CART_Y + 2 * CART_A + CART_V + 10, REC_A = 30;
static const int TOPO_A = REC_Y + REC_A;
static const int EIXO_L = 64;                          // column for the Y-axis numbers

static const int G1_Y = TOPO_A + 20;                   // title + 30 MIN | 24 H
static const int SEG_A = 48, SEG_L = 240;
static const int G1_PLOT_Y = G1_Y + SEG_A + 16, G1_A = 320;
static const int G1_FIM = G1_PLOT_Y + G1_A + 34;       // + time labels

static const int G2_Y = G1_FIM + 22;                   // title + legend
static const int G2_PLOT_Y = G2_Y + 48, G2_A = 216;
static const int G2_FAIXA = 8;                         // green charging strip under the bars
static const int G2_DET_Y = G2_PLOT_Y + G2_A + G2_FAIXA + 40;   // two lines of detail

// Where the content ends (the two detail lines under the bar chart).
static const int CONTEUDO_A = G2_DET_Y + 70;

// Portrait fits (1144 px); landscape (physical keyboard, ~584 px) the system scrolls.
static int altura_agora(const area_t& a) { return a.altura < CONTEUDO_A ? CONTEUDO_A : 0; }

static int cart_l(int larg) { return (larg - 2 * M - 2 * CART_V) / 3; }
static int seg_x(int larg)  { return larg - M - SEG_L; }
static int plot_x(void)     { return M + EIXO_L; }
static int plot_l(int larg) { return larg - 2 * M - EIXO_L; }

static const float TAM_BOTAO = 3.625f;     // = body 29 px native

// The system's smooth fonts, at native heights (no scaling).
static void fc(lgfx::LovyanGFX& d, int px) { d.setFont(tab5_fonte_suave(false, px)); d.setTextSize(1); }
static void fn(lgfx::LovyanGFX& d, int px) { d.setFont(tab5_fonte_suave(true, px));  d.setTextSize(1); }

// Darkened colour (f from 0..256) - the fill of the area under the curve.
static uint16_t escurecer(uint16_t c, int f)
{
  const int r = ((c >> 11) & 31) * f / 256, g = ((c >> 5) & 63) * f / 256, b = (c & 31) * f / 256;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t cor_nivel(int pct)
{
  return pct < 0 ? (uint16_t)TEMA_TEXTO_3 : pct < 20 ? (uint16_t)TEMA_PERIGO
       : pct < 50 ? (uint16_t)TEMA_ALERTA : (uint16_t)COR_CARGA;
}


// ---------------------------------------------------------------------------
// SERIES
// ---------------------------------------------------------------------------
// The BATTERY series is the SAME % the system shows (the estimator, saved
// with each sample) - not the voltage curve recalculated here, which would say
// something else right after a charge and put the chart at odds with the bar.
static float valor(const pontos_t* p, serie_t s, int i)
{
  switch (s) {
    case SERIE_TENSAO:   return p->v[i];
    case SERIE_CORRENTE: return p->ma[i];                                       // SIGNED
    case SERIE_POTENCIA: return p->v[i] * fabsf(p->ma[i]) / 1000.0f;            // W
    case SERIE_TEMP:     return p->temp[i];
    default:             return (float)p->pct[i];
  }
}
static bool valido(const pontos_t* p, serie_t s, int i)
{
  if (s == SERIE_TEMP)    { return p->temp[i] > -200.0f; }
  if (s == SERIE_BATERIA) { return p->pct[i] >= 0; }
  return p->v[i] >= 1.0f;
}
static const char* formato(serie_t s)
{
  return (s == SERIE_TENSAO)   ? "%.2f V"
       : (s == SERIE_CORRENTE) ? "%+.0f mA"
       : (s == SERIE_POTENCIA) ? "%.2f W"
       : (s == SERIE_TEMP)     ? "%.1f °C" : "%.0f %%";
}
static float faixa_minima(serie_t s)
{
  return (s == SERIE_TENSAO) ? 0.10f : (s == SERIE_CORRENTE) ? 40.0f
       : (s == SERIE_POTENCIA) ? 0.2f : 4.0f;
}

// ROUND-NUMBER SCALE. The old chart only showed the measured minimum and
// maximum (e.g. -183 and -97 mA) in the corner: no grid line had a value,
// and the curve could not be read. Here the limits are multiples of a
// 1/2/2.5/5 step, and each grid line has its number on the axis.
static void escala(float mn, float mx, float minima, float* lo, float* hi, float* passo)
{
  if (mx - mn < minima) { const float c = (mx + mn) / 2; mn = c - minima / 2; mx = c + minima / 2; }
  const float bruto = (mx - mn) / 3.0f;
  const float mag = powf(10.0f, floorf(log10f(bruto)));
  const float r = bruto / mag;
  *passo = mag * (r <= 1.0f ? 1.0f : r <= 2.0f ? 2.0f : r <= 2.5f ? 2.5f : r <= 5.0f ? 5.0f : 10.0f);
  *lo = floorf(mn / *passo + 1e-4f) * *passo;
  *hi = ceilf(mx / *passo - 1e-4f) * *passo;
  if (*hi <= *lo) { *hi = *lo + *passo; }
}

static void numero_eixo(char* out, size_t tam, float v, float passo)
{
  if (fabsf(v) < passo * 1e-3f) { v = 0; }   // no "-0"
  snprintf(out, tam, passo >= 1.0f ? "%.0f" : passo >= 0.1f ? "%.1f" : "%.2f", (double)v);
}

// ---------------------------------------------------------------------------
// BUILDING THE POINTS
// ---------------------------------------------------------------------------
static void montar_30(pontos_t* p)
{
  const int n = tab5_power_historico(s_m->ram);
  for (int i = 0; i < n; ++i) {
    // Right-aligned: "now" always at the edge, filling during the first 30 min.
    p->x[i] = (float)(TAB5_HIST_N - n + i) / (TAB5_HIST_N - 1);
    p->v[i] = s_m->ram[i].volts; p->ma[i] = s_m->ram[i].mA; p->temp[i] = s_m->ram[i].temp; p->pct[i] = (int8_t)s_m->ram[i].pct;
    p->rep[i] = 0;
    p->quebra[i] = (i == 0);
    p->t[i] = (n - 1 - i) * TAB5_HIST_SEG;
  }
  p->n = n;
}

static void montar_24h(pontos_t* p)
{
  p->n = 0;
  if (!s_dia || !s_dia->t0) { return; }
  int ultimo = -1000;
  for (int m = 0; m < MIN_DIA; ++m) {
    if (!s_dia->tem[m]) { continue; }
    const int k = p->n++;
    p->x[k] = (float)m / MIN_DIA;
    p->v[k] = s_dia->v[m]; p->ma[k] = s_dia->ma[m]; p->temp[k] = s_dia->temp[m]; p->pct[k] = s_dia->pct[m];
    p->rep[k] = s_dia->tem[m];
    p->quebra[k] = (m - ultimo) > 3;   // gap of more than 3 min: device was off
    p->t[k] = m;
    ultimo = m;
  }
  // WHAT THE SD CARD DOES NOT HAVE YET: the log writes in 5 min batches, so
  // top up with the RAM ring, from the minute after the last one written until
  // now. The ring does not store idle state; those few minutes count as
  // screen on (the app is open, after all).
  const uint32_t agora = tab5_hora_epoch();
  if (!agora) { return; }
  const int n = tab5_power_historico(s_m->ram);
  for (int i = 0; i < n; ++i) {
    const uint32_t ep = (uint32_t)agora - (uint32_t)((n - 1 - i) * TAB5_HIST_SEG);
    if (ep < s_dia->t0) { continue; }
    const int m = (int)((ep - s_dia->t0) / 60);
    if (m <= ultimo || m >= MIN_DIA || p->n >= MIN_DIA) { continue; }
    const int k = p->n++;
    p->x[k] = (float)m / MIN_DIA;
    p->v[k] = s_m->ram[i].volts; p->ma[k] = s_m->ram[i].mA; p->temp[k] = s_m->ram[i].temp; p->pct[k] = (int8_t)s_m->ram[i].pct;
    p->rep[k] = 0;
    p->quebra[k] = (m - ultimo) > 3;
    p->t[k] = m;
    ultimo = m;
  }
}

// Each 24 h point is worth one minute: mAh = mA / 60.
static void montar_horas(const pontos_t* p)
{
  memset(s_m->horas, 0, sizeof(s_m->horas));
  s_m->tot_gasto = s_m->tot_carga = s_m->tot_wh = 0;
  s_m->tot_min_tela = 0;
  for (int i = 0; i < p->n; ++i) {
    hora_t* h = &s_m->horas[p->t[i] / 60];
    h->tem = true;
    const bool tela = (p->rep[i] != 2);
    if (tela) { ++h->min_tela; ++s_m->tot_min_tela; }
    if (p->ma[i] < 0) {
      const float mah = -p->ma[i] / 60.0f;
      if (tela) { h->tela += mah; } else { h->repouso += mah; }
      s_m->tot_gasto += mah;
      s_m->tot_wh += p->v[i] * mah / 1000.0f;
    } else {
      h->carga += p->ma[i] / 60.0f;
      s_m->tot_carga += p->ma[i] / 60.0f;
    }
  }
}

// ONE VERTEX PER PIXEL COLUMN. In 24 h mode that is 1440 minutes over ~600 px:
// several points land in the same column, and their average becomes the
// vertex. This smooths the current's minute-to-minute noise (which turned
// into a zigzag blur) without a separate filter, and the drawing cost
// depends on the width, not on the number of points.
static void montar_vertices(const pontos_t* p, serie_t s, int gx, int gl)
{
  vertices_t* v = &s_m->vt;
  v->n = 0;
  int cnt = 0; float soma = 0; bool quebra_pend = true;
  for (int i = 0; i < p->n; ++i) {
    if (!valido(p, s, i)) { quebra_pend = true; continue; }
    const int px = gx + (int)(p->x[i] * (gl - 1) + 0.5f);
    if (p->quebra[i]) { quebra_pend = true; }
    if (cnt && px == v->px[v->n - 1] && !quebra_pend) {
      soma += valor(p, s, i); ++cnt;
      v->val[v->n - 1] = soma / cnt;
      v->carga[v->n - 1] |= (p->ma[i] > 20.0f);
      v->t[v->n - 1] = p->t[i];
      continue;
    }
    if (v->n >= VERT_MAX) { break; }
    const int k = v->n++;
    v->px[k] = (int16_t)px; v->val[k] = valor(p, s, i);
    v->carga[k] = (p->ma[i] > 20.0f);
    v->quebra[k] = quebra_pend;
    v->t[k] = p->t[i];
    soma = v->val[k]; cnt = 1; quebra_pend = false;
  }
}

// ---------------------------------------------------------------------------
// DRAWING - TOP (hero + index), every second
// ---------------------------------------------------------------------------
// Drawn battery: outline, fill by %, lightning bolt on top when charging.
static void icone_bateria(lgfx::LovyanGFX& d, int x, int y, int l, int a, int pct, bool carregando)
{
  const uint16_t cor = cor_nivel(pct);
  d.fillRoundRect(x, y, l, a, 7, TEMA_TEXTO_2);
  d.fillRoundRect(x + 3, y + 3, l - 6, a - 6, 5, TEMA_FUNDO);
  d.fillRoundRect(x + l + 2, y + a / 2 - 7, 5, 14, 2, TEMA_TEXTO_2);
  const int cheio = pct < 0 ? 0 : (l - 12) * (pct > 100 ? 100 : pct) / 100;
  if (cheio > 0) { d.fillRoundRect(x + 6, y + 6, cheio, a - 12, 3, cor); }
  if (carregando) {
    const int cx = x + l / 2, cy = y + a / 2;
    d.fillTriangle(cx + 4, cy - 14, cx - 9, cy + 2, cx + 1, cy + 2, TEMA_TEXTO);
    d.fillTriangle(cx - 4, cy + 14, cx + 9, cy - 2, cx - 1, cy - 2, TEMA_TEXTO);
  }
}

static void desenhar_topo(lgfx::LovyanGFX& d, const area_t& a)
{
  const tab5_medida_t m = tab5_power_ultima();
  tab5_tela_limpar(a.x, a.y, a.largura, TOPO_A, TEMA_FUNDO);
  char buf[64], dur[24];

  // HERO. The status comes from the instantaneous current (the same three-state
  // rule as the status bar); the ESTIMATE comes from the average of the last
  // 30 min - the one-second current jumps around too much to make a forecast.
  const int n = tab5_power_historico(s_m->ram);
  float media = m.mA;
  if (n >= 4) { media = 0; for (int i = 0; i < n; ++i) { media += s_m->ram[i].mA; } media /= n; }
  const bool carregando = m.mA > 20.0f, na_bateria = m.mA < -20.0f;

  tab5_botao(d, a.x + 16, a.y + 16, VOLTAR_L, VOLTAR_A, tr("< VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  tab5_titulo(d, 40);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(tr("Energia"), a.x + 16 + VOLTAR_L + 28, a.y + 16 + VOLTAR_A / 2);

  const int y = a.y + HERO_Y;
  fn(d, 56); d.setTextDatum(top_left);
  d.setTextColor(cor_nivel(m.pct), TEMA_FUNDO);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--"); } else { snprintf(buf, sizeof(buf), "%d%%", m.pct); }
  d.drawString(buf, a.x + M, y);
  const int fim_pct = a.x + M + d.textWidth(buf);
  icone_bateria(d, fim_pct + 18, y + (d.fontHeight() - 34) / 2, 68, 34, m.pct, carregando);
  // Tapping the hero chooses the BATTERY series: the strip on the left shows it is selected.
  if (s_serie == SERIE_BATERIA) { d.fillRoundRect(a.x + 4, y + 8, 5, HERO_A - 24, 2, SERIE_COR[SERIE_BATERIA]); }

  const char* estado; uint16_t cor_estado; buf[0] = 0;
  if (carregando) {
    estado = tr("Carregando"); cor_estado = COR_CARGA;
    if (media > 20.0f && m.pct >= 0 && m.pct < 100) {
      tab5_duracao((uint32_t)(((100 - m.pct) / 100.0f * TAB5_CAPACIDADE_MAH / media) * 3600.0f + 30.0f), dur, sizeof(dur));
      snprintf(buf, sizeof(buf), tr("cheia em ~%s"), dur);
    }
  } else if (na_bateria) {
    estado = tr("Na bateria"); cor_estado = TEMA_TEXTO;
    if (media < -20.0f && m.pct >= 0) {
      const float h = m.pct / 100.0f * TAB5_CAPACIDADE_MAH / -media;
      if (h < 100.0f) { tab5_duracao((uint32_t)((h) * 3600.0f + 30.0f), dur, sizeof(dur)); snprintf(buf, sizeof(buf), tr("dura ~%s neste ritmo"), dur); }
    }
  } else {
    estado = tr("Na tomada"); cor_estado = COR_CARGA;
    snprintf(buf, sizeof(buf), tr("carga completa"));
  }
  const int xr = a.x + a.largura - M;
  fn(d, 32); d.setTextDatum(top_right); d.setTextColor(cor_estado, TEMA_FUNDO);
  d.drawString(estado, xr, y + 2);
  fc(d, 24); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(buf, xr, y + 42);

  // INDEX: six cards. The first five are the series - tapping chooses chart
  // 1; the sixth is the 24 h consumption (chart 2's total).
  float temp = -273.0f; const bool tem_temp = tab5_sensores_temp(&temp);
  const int cl = cart_l(a.largura);
  for (int i = 0; i < 6; ++i) {
    const int cx = a.x + M + (i % 3) * (cl + CART_V), cy = a.y + CART_Y + (i / 3) * (CART_A + CART_V);
    const bool sel = (i < N_SERIES && (int)s_serie == i);
    const uint16_t fundo = sel ? COR_CARTAO_SEL : COR_CARTAO;
    d.fillRoundRect(cx, cy, cl, CART_A, TEMA_RAIO_M, fundo);
    if (sel) {
      d.drawRoundRect(cx, cy, cl, CART_A, TEMA_RAIO_M, SERIE_COR[i]);
      d.drawRoundRect(cx + 1, cy + 1, cl - 2, CART_A - 2, TEMA_RAIO_M - 1, SERIE_COR[i]);
    }
    uint16_t cor = (i < N_SERIES) ? SERIE_COR[i] : (uint16_t)TEMA_TEXTO;
    switch (i) {
      case SERIE_BATERIA:  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--"); } else { snprintf(buf, sizeof(buf), "%d %%", m.pct); } break;
      case SERIE_CORRENTE: snprintf(buf, sizeof(buf), "%+.0f mA", (double)m.mA); cor = m.mA > 20.0f ? (uint16_t)COR_CARGA : cor; break;
      case SERIE_POTENCIA: snprintf(buf, sizeof(buf), "%.2f W", (double)(m.mW / 1000.0f)); break;
      case SERIE_TENSAO:   snprintf(buf, sizeof(buf), "%.2f V", (double)m.volts); break;
      case SERIE_TEMP:     if (tem_temp) { snprintf(buf, sizeof(buf), "%.1f °C", (double)temp); } else { snprintf(buf, sizeof(buf), "--"); } break;
      default:             if (s_dia && s_dia->t0) { snprintf(buf, sizeof(buf), tr("%.0f mAh"), (double)s_m->tot_gasto); } else { snprintf(buf, sizeof(buf), "--"); } break;
    }
    fc(d, 21); d.setTextDatum(top_left);
    d.setTextColor(sel ? TEMA_TEXTO : TEMA_TEXTO_2, fundo);
    d.drawString(i < N_SERIES ? tr(SERIE_CARTAO[i]) : tr("Gasto 24 h"), cx + 14, cy + 10);
    fn(d, 36); d.setTextColor(cor, fundo);
    d.drawString(buf, cx + 14, cy + 40);
  }

  // WHAT IS HOLDING OFF IDLE: each resource in use and who declared it. None =
  // the screen off drops to 40 MHz with light sleep, the lowest possible drain.
  {
    recurso_uso_t u[RECURSOS_MAX];
    const int nr = sistema_recursos(u, RECURSOS_MAX);
    char rr[160];
    int k = snprintf(rr, sizeof(rr), "%s", nr ? tr("Segura o repouso: ") : tr("Nada segura o repouso: a tela apagada gasta o mínimo"));
    for (int j = 0; j < nr && k < (int)sizeof(rr); ++j) {
      k += snprintf(rr + k, sizeof(rr) - k, "%s%s (%s)", j ? ", " : "", tr(sistema_recurso_nome(u[j].r)), u[j].quem);
    }
    fc(d, 21); d.setTextDatum(top_left);
    d.setTextColor(nr ? TEMA_ALERTA : TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(rr, a.x + M, a.y + REC_Y);
  }
  d.setTextDatum(top_left);
}

// ---------------------------------------------------------------------------
// DRAWING - TIME AXIS (the same for both charts in 24 h mode)
// ---------------------------------------------------------------------------
static void eixo_tempo(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, bool h24, bool grade)
{
  const int divs = h24 ? 4 : 2;     // 24 H: every 6 h; 30 MIN: every 15 min
  if (grade) { for (int k = 1; k < divs; ++k) { d.drawFastVLine(gx + (gl * k) / divs, gy, ga, TAB5_GRAFICO_COR_GRADE); } }
  fc(d, 21); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  static const char* T[3] = { N_("-30 min"), N_("-15 min"), N_("agora") };
  char buf[12];
  for (int k = 0; k <= divs; ++k) {
    const char* txt = tr(T[k < 3 ? k : 2]);
    if (h24) {
      if (k == divs) { txt = tr("agora"); }
      else {
        struct tm lt; tab5_hora_local(s_dia->t0 + (uint32_t)k * 6u * 3600u, &lt);
        snprintf(buf, sizeof(buf), "%dh", lt.tm_hour); txt = buf;
      }
    }
    d.setTextDatum(k == 0 ? top_left : k == divs ? top_right : top_center);
    d.drawString(txt, gx + (gl * k) / divs, gy + ga + 8);
  }
  d.setTextDatum(top_left);
}

static void aviso_no_grafico(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, const char* txt)
{
  fc(d, 24); d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO); d.setTextDatum(middle_center);
  d.drawString(txt, gx + gl / 2, gy + ga / 2);
  d.setTextDatum(top_left);
}

static void desenhar_eixo_y(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, float lo, float hi, float passo, bool so_meio_cheio)
{
  char buf[16];
  fc(d, 21); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO); d.setTextDatum(middle_right);
  const int n = (int)((hi - lo) / passo + 0.5f);
  for (int k = 0; k <= n; ++k) {
    const float v = lo + k * passo;
    const int y = gy + ga - 1 - (int)((v - lo) * (ga - 1) / (hi - lo) + 0.5f);
    d.drawFastHLine(gx, y, gl, k == 0 ? COR_EIXO : TAB5_GRAFICO_COR_GRADE);
    if (so_meio_cheio && (k % 2)) { continue; }   // battery: labels 0/50/100, grid every 25
    numero_eixo(buf, sizeof(buf), v, passo);
    d.drawString(buf, gx - 10, y);
  }
  d.setTextDatum(top_left);
}

// ---------------------------------------------------------------------------
// DRAWING - CHART 1: the chosen quantity over time
// ---------------------------------------------------------------------------
static void desenhar_grafico1(lgfx::LovyanGFX& d, const area_t& a)
{
  const int gx = a.x + plot_x(), gl = plot_l(a.largura), gy = a.y + G1_PLOT_Y, ga = G1_A;
  tab5_tela_limpar(a.x, a.y + G1_Y, a.largura, G1_FIM - G1_Y, TEMA_FUNDO);
  char buf[48];

  // Header: what the chart is (with unit) and the period.
  fn(d, 28); d.setTextDatum(middle_left); d.setTextColor(SERIE_COR[s_serie], TEMA_FUNDO);
  d.drawString(tr(SERIE_TITULO[s_serie]), a.x + M, a.y + G1_Y + SEG_A / 2);
  const int sx = a.x + seg_x(a.largura), sy = a.y + G1_Y, meio = SEG_L / 2;
  tab5_botao(d, sx,        sy, meio, SEG_A, tr("30 MIN"), s_24h ? (uint16_t)TEMA_SUPERFICIE_ALTA : (uint16_t)TEMA_DESTAQUE,
             s_24h ? (uint16_t)TEMA_TEXTO_2 : (uint16_t)TEMA_SOBRE_DESTAQUE, TAM_BOTAO);
  tab5_botao(d, sx + meio, sy, meio, SEG_A, "24 H",   s_24h ? (uint16_t)TEMA_DESTAQUE : (uint16_t)TEMA_SUPERFICIE_ALTA,
             s_24h ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO_2, TAM_BOTAO);
  d.drawRoundRect(sx, sy, SEG_L, SEG_A, TEMA_RAIO_M, TEMA_LINHA);
  d.setTextDatum(top_left);

  if (s_24h && (!s_dia || !s_dia->t0)) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    aviso_no_grafico(d, gx, gl, gy, ga, s_carga_pedida ? tr("lendo o cartão...") : tr("24 H precisa do relógio (GPS, malha ou NTP)"));
    return;
  }
  const pontos_t* p = s_24h ? &s_m->p24 : &s_m->p30;
  eixo_tempo(d, gx, gl, gy, ga, s_24h, false);
  montar_vertices(p, s_serie, gx, gl);
  vertices_t* v = &s_m->vt;
  if (v->n < 2) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    aviso_no_grafico(d, gx, gl, gy, ga, s_24h ? tr("ainda sem pontos (1 por minuto)") : tr("juntando dados (1 ponto a cada 30 s)..."));
    return;
  }

  float lo, hi, passo;
  if (s_serie == SERIE_BATERIA) { lo = 0; hi = 100; passo = 25; }
  else {
    float mn = v->val[0], mx = v->val[0];
    for (int i = 1; i < v->n; ++i) { if (v->val[i] < mn) { mn = v->val[i]; } if (v->val[i] > mx) { mx = v->val[i]; } }
    // Current: zero is included in the scale - the zero line separates charge from discharge.
    if (s_serie == SERIE_CORRENTE) { if (mn > 0) { mn = 0; } if (mx < 0) { mx = 0; } }
    escala(mn, mx, faixa_minima(s_serie), &lo, &hi, &passo);
  }
  #define YV(val) (gy + ga - 1 - (int)((((val) < lo ? lo : (val) > hi ? hi : (val)) - lo) * (ga - 1) / (hi - lo) + 0.5f))

  // 1) The CHARGING hours tinted behind everything (for current, the sign
  //    itself already says so, with the green fill above zero).
  if (s_serie != SERIE_CORRENTE) {
    for (int i = 0; i < v->n; ++i) {
      if (!v->carga[i]) { continue; }
      const bool liga = (i + 1 < v->n && !v->quebra[i + 1]);
      const int x1 = liga ? v->px[i + 1] : v->px[i] + 2;
      d.fillRect(v->px[i], gy, x1 - v->px[i] + 1, ga, COR_CARGA_FUNDO);
    }
  }
  // 2) Grid with numbers and the time verticals.
  desenhar_eixo_y(d, gx, gl, gy, ga, lo, hi, passo, s_serie == SERIE_BATERIA);
  eixo_tempo(d, gx, gl, gy, ga, s_24h, true);

  // 3) AREA under the curve, column by column, interpolating between vertices.
  //    Current: fills to ZERO - green above (charging), orange below.
  const uint16_t cor = SERIE_COR[s_serie];
  const int y_base = (s_serie == SERIE_CORRENTE) ? YV(0.0f) : gy + ga - 2;
  const uint16_t fill = escurecer(cor, 72);
  const uint16_t fill_carga = escurecer(COR_CARGA, 80);
  for (int i = 1; i < v->n; ++i) {
    if (v->quebra[i]) { continue; }
    const int x0 = v->px[i - 1], x1 = v->px[i];
    const float y0 = (float)YV(v->val[i - 1]), y1 = (float)YV(v->val[i]);
    for (int x = x0; x <= x1; ++x) {
      const int y = (x1 == x0) ? (int)y1 : (int)(y0 + (y1 - y0) * (x - x0) / (x1 - x0) + 0.5f);
      if (y < y_base) { d.drawFastVLine(x, y, y_base - y, s_serie == SERIE_CORRENTE ? fill_carga : fill); }
      else if (y > y_base) { d.drawFastVLine(x, y_base + 1, y - y_base, fill); }
    }
  }
  if (s_serie == SERIE_CORRENTE) { d.drawFastHLine(gx, y_base, gl, TEMA_TEXTO_2); }

  // 4) The LINE on top (~3 px, anti-aliased edge). An isolated point becomes a dot.
  for (int i = 0; i < v->n; ++i) {
    const int y = YV(v->val[i]);
    if (i > 0 && !v->quebra[i]) { tab5_linha_grossa(d, v->px[i - 1], YV(v->val[i - 1]), v->px[i], y, 1.5f, cor); }
    else if (i + 1 >= v->n || v->quebra[i + 1]) { d.fillCircle(v->px[i], y, 2, cor); }
  }

  // 5) CURSOR: the vertex nearest the tap, with the time and value.
  if (s_cursor >= 0) {
    const int alvo = a.x + s_cursor;
    int k = 0;
    for (int i = 1; i < v->n; ++i) { if (abs(v->px[i] - alvo) < abs(v->px[k] - alvo)) { k = i; } }
    const int cx = v->px[k], cy = YV(v->val[k]);
    d.drawFastVLine(cx, gy, ga, TEMA_TEXTO);
    d.fillCircle(cx, cy, 7, TEMA_TEXTO);
    d.fillCircle(cx, cy, 4, cor);
    char val[24];
    snprintf(val, sizeof(val), formato(s_serie), (double)v->val[k]);
    if (s_24h) {
      struct tm lt; tab5_hora_local(s_dia->t0 + (uint32_t)v->t[k] * 60u, &lt);
      char hm[8];
      tab5_hora_hhmm(&lt, hm, sizeof(hm));
      snprintf(buf, sizeof(buf), "%s   %s", hm, val);
    } else {
      const int s = v->t[k];
      if (s < 60) { snprintf(buf, sizeof(buf), tr("agora   %s"), val); }
      else        { snprintf(buf, sizeof(buf), tr("há %d min   %s"), s / 60, val); }
    }
    fn(d, 24);
    const int bl = d.textWidth(buf) + 24, ba = 40;
    int bx = cx + 12;
    if (bx + bl > gx + gl) { bx = cx - 12 - bl; }
    if (bx < gx) { bx = gx; }
    d.fillRoundRect(bx, gy + 6, bl, ba, TEMA_RAIO_P, COR_CARTAO_SEL);
    d.drawRoundRect(bx, gy + 6, bl, ba, TEMA_RAIO_P, cor);
    d.setTextDatum(middle_left); d.setTextColor(TEMA_TEXTO, COR_CARTAO_SEL);
    d.drawString(buf, bx + 12, gy + 6 + ba / 2);
    d.setTextDatum(top_left);
  }
  #undef YV
}

// ---------------------------------------------------------------------------
// DRAWING - CHART 2: consumption per hour over 24 h
// ---------------------------------------------------------------------------
static void quadradinho(lgfx::LovyanGFX& d, int* xr, int ym, uint16_t cor, const char* txt)
{
  // Legend written right to left: returns in *xr where the next one ends.
  const int w = d.textWidth(txt);
  d.setTextDatum(middle_right); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(txt, *xr, ym);
  d.fillRoundRect(*xr - w - 22, ym - 8, 16, 16, 3, cor);
  *xr -= w + 22 + 20;
}

static void desenhar_grafico2(lgfx::LovyanGFX& d, const area_t& a)
{
  const int gx = a.x + plot_x(), gl = plot_l(a.largura), gy = a.y + G2_PLOT_Y, ga = G2_A;
  tab5_tela_limpar(a.x, a.y + G2_Y, a.largura, a.altura - G2_Y, TEMA_FUNDO);
  char buf[96], dur[24];

  fn(d, 28); d.setTextDatum(middle_left); d.setTextColor(COR_GASTO_TELA, TEMA_FUNDO);
  d.drawString(tr("Gasto por hora (mAh)"), a.x + M, a.y + G2_Y + 18);
  fc(d, 18);
  int xr = a.x + a.largura - M;
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_CARGA, tr("carga"));
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_GASTO_REP, tr("repouso"));
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_GASTO_TELA, tr("tela"));
  d.setTextDatum(top_left);

  if (!s_dia || !s_dia->t0) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    aviso_no_grafico(d, gx, gl, gy, ga, s_carga_pedida ? tr("lendo o cartão...") : tr("precisa do relógio (GPS, malha ou NTP)"));
    return;
  }

  float mx = 0;
  for (int h = 0; h < 24; ++h) { const float g = s_m->horas[h].tela + s_m->horas[h].repouso; if (g > mx) { mx = g; } }
  float lo, hi, passo;
  escala(0, mx < 20.0f ? 20.0f : mx, 20.0f, &lo, &hi, &passo);
  lo = 0;
  desenhar_eixo_y(d, gx, gl, gy, ga, lo, hi, passo, false);

  // Bars aligned with chart 1's hour columns (same gx/gl).
  for (int h = 0; h < 24; ++h) {
    const int c0 = gx + (gl * h) / 24, c1 = gx + (gl * (h + 1)) / 24;
    const int bx = c0 + 3, bl = c1 - c0 - 6;
    const hora_t* hr = &s_m->horas[h];
    if (h == s_hora_sel) { d.fillRect(c0 + 1, gy, c1 - c0 - 2, ga - 1, COR_CARTAO_SEL); }
    const int a_rep  = (int)(hr->repouso * (ga - 2) / hi + 0.5f);
    const int a_tela = (int)(hr->tela    * (ga - 2) / hi + 0.5f);
    const int base = gy + ga - 2;
    if (a_rep > 0)  { d.fillRect(bx, base - a_rep + 1, bl, a_rep, COR_GASTO_REP); }
    if (a_tela > 0) { d.fillRect(bx, base - a_rep - a_tela + 1, bl, a_tela, COR_GASTO_TELA); }
    if (hr->carga > 5.0f) { d.fillRoundRect(bx, gy + ga + 2, bl, G2_FAIXA - 2, 2, COR_CARGA); }
  }
  eixo_tempo(d, gx, gl, gy + G2_FAIXA, ga, true, false);

  // DETAIL: the tapped hour, or the 24 h totals.
  const int y1 = a.y + G2_DET_Y, y2 = y1 + 32;
  d.setTextDatum(top_left);
  if (s_hora_sel >= 0) {
    const hora_t* hr = &s_m->horas[s_hora_sel];
    struct tm lt; tab5_hora_local(s_dia->t0 + (uint32_t)s_hora_sel * 3600u, &lt);
    snprintf(buf, sizeof(buf), tr("%dh - %dh: gastou %.0f mAh · carregou %.0f mAh"),
             lt.tm_hour, (lt.tm_hour + 1) % 24, (double)(hr->tela + hr->repouso), (double)hr->carga);
    fn(d, 24); d.setTextColor(TEMA_TEXTO, TEMA_FUNDO); d.drawString(buf, a.x + M, y1);
    if (hr->tem) {
      snprintf(buf, sizeof(buf), tr("tela acesa %d min (%.0f mAh)   repouso %.0f mAh"),
               hr->min_tela, (double)hr->tela, (double)hr->repouso);
    } else {
      snprintf(buf, sizeof(buf), tr("sem registro nesta hora (aparelho desligado)"));
    }
    fc(d, 24); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO); d.drawString(buf, a.x + M, y2);
  } else {
    snprintf(buf, sizeof(buf), tr("24 h: gastou %.0f mAh (%.2f Wh) · carregou %.0f mAh"),
             (double)s_m->tot_gasto, (double)s_m->tot_wh, (double)s_m->tot_carga);
    fn(d, 24); d.setTextColor(TEMA_TEXTO, TEMA_FUNDO); d.drawString(buf, a.x + M, y1);
    tab5_duracao((uint32_t)((s_m->tot_min_tela / 60.0f) * 3600.0f + 30.0f), dur, sizeof(dur));
    uint32_t falhas; registro_estado(nullptr, nullptr, &falhas);
    if (falhas) { snprintf(buf, sizeof(buf), tr("tela acesa %s   registro: %lu falhas no cartão"), dur, (unsigned long)falhas); }
    else        { snprintf(buf, sizeof(buf), tr("tela acesa %s   toque numa barra para ver a hora"), dur); }
    fc(d, 24); d.setTextColor(falhas ? TEMA_ALERTA : TEMA_TEXTO_2, TEMA_FUNDO); d.drawString(buf, a.x + M, y2);
  }
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  pintura_iniciar(&s_p);
  const bool tudo = pintura_completa(&s_p);

  // 24 H: reloads when the log has written a new sample (every 5 min) or
  // when the hour has turned (the window moves one column).
  uint32_t grav; registro_estado(nullptr, &grav, nullptr);
  if (grav != s_gravadas_lidas || (s_dia && janela_t0() != s_dia->t0)) { s_gravadas_lidas = grav; pedir_24h(); }

  if (tudo) { tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TEMA_FUNDO); }

  // The charts only change when data arrives (ring every TAB5_HIST_SEG, SD card
  // on each load) or when a tap changed the selection.
  static int s_assin = -1;
  const int n = tab5_power_historico(s_m->ram);
  const int assin = (int)s_dia_ver * 7919 + n * 1000003 + (n ? (int)(s_m->ram[n - 1].mA * 10) : 0);
  if (tudo || s_graf_sujo || assin != s_assin) {
    s_assin = assin;
    s_graf_sujo = false;
    montar_24h(&s_m->p24);
    montar_horas(&s_m->p24);
    montar_30(&s_m->p30);
    desenhar_grafico1(d, a);
    desenhar_grafico2(d, a);
  }
  desenhar_topo(d, a);   // afterwards: the "Gasto 24 h" card reads the total montar_horas has just computed

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();
  const int cl = cart_l(a.largura);
  const int gx = plot_x(), gl = plot_l(a.largura);
  if (tab5_dentro(x, y, 16, 16, VOLTAR_L, VOLTAR_A)) { sistema_abrir_app("Ajustes"); return; }
  if (y >= HERO_Y && y < HERO_Y + HERO_A) {
    s_serie = SERIE_BATERIA; s_cursor = -1;
  }
  // An index card: chooses chart 1's series (the sixth is information only).
  else if (y >= CART_Y && y < REC_Y && x >= M) {   // the resources line is not a card
    const int col = (x - M) / (cl + CART_V), lin = (y - CART_Y) / (CART_A + CART_V);
    const int i = lin * 3 + (col > 2 ? 2 : col);
    if (i >= N_SERIES) { return; }
    s_serie = (serie_t)i; s_cursor = -1;
  }
  // The period control: left half 30 MIN, right half 24 H.
  else if (y >= G1_Y && y < G1_Y + SEG_A && x >= seg_x(a.largura)) {
    s_24h = (x >= seg_x(a.largura) + SEG_L / 2); s_cursor = -1;
    if (s_24h) { pedir_24h(); }
  }
  // Inside chart 1: places the cursor; tapping near it again removes it.
  else if (y >= G1_PLOT_Y && y < G1_PLOT_Y + G1_A && x >= gx - 8 && x < gx + gl + 8) {
    s_cursor = (s_cursor >= 0 && abs(x - s_cursor) < 16) ? -1 : x;
  }
  // A bar in chart 2 (with the charging strip and the labels below).
  else if (y >= G2_PLOT_Y && y < G2_PLOT_Y + G2_A + G2_FAIXA + 34 && x >= gx && x < gx + gl) {
    const int h = (x - gx) * 24 / gl;
    s_hora_sel = (h == s_hora_sel) ? -1 : h;
  }
  else { return; }
  s_graf_sujo = true;
  sistema_pedir_redesenho();
}


// ---------------------------------------------------------------------------
// HOME SCREEN ICON
// ---------------------------------------------------------------------------
// Primitives and fractions of the side, like the Mesh logo: it comes out clean
// at any size, uses no flash and has no jagged edges. The big letter that was
// here was the built-in font scaled 6x - each of its pixels became a 6x6 square.
static void icone_energia(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo  = d.color565(255, 150, 40);
  const uint16_t escuro = d.color565(55, 30, 0);

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  #define IX(f) (x + (int)((f) * lado))
  #define IY(f) (y + (int)((f) * lado))

  // Battery lying on its side with the terminal on the right. Thick outline
  // drawn as two rectangles (filled and hollow), which is cheaper than four
  // lines and leaves no open corner.
  d.fillRoundRect(IX(0.14f), IY(0.32f), (int)(0.62f * lado), (int)(0.36f * lado),
                  lado / 16, escuro);
  d.fillRoundRect(IX(0.14f) + lado / 22, IY(0.32f) + lado / 22,
                  (int)(0.62f * lado) - lado / 11, (int)(0.36f * lado) - lado / 11,
                  lado / 20, fundo);
  d.fillRoundRect(IX(0.78f), IY(0.42f), (int)(0.07f * lado), (int)(0.16f * lado),
                  lado / 40, escuro);

  // Lightning bolt inside: that is what separates "battery" from "consumption".
  // The app measures current, not just charge.
  d.fillTriangle(IX(0.48f), IY(0.36f), IX(0.30f), IY(0.53f), IX(0.44f), IY(0.53f), escuro);
  d.fillTriangle(IX(0.40f), IY(0.64f), IX(0.58f), IY(0.47f), IX(0.44f), IY(0.47f), escuro);

  #undef IX
  #undef IY
}

extern const app_t APP_ENERGIA = {
  .nome  = "Energia",
  .cor   = TFT_ORANGE,
  // Profile: only the brightness. Everything else comes from the tab5_perfil_t
  // defaults, which are 'off' - so there is no need to list what is not used.
  .perfil = { },
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .periodo_ms = 1000,     // the top follows the kernel's 1 Hz measurement
  .avisos = AVISO_TRABALHO,
  .ao_aviso = ao_aviso,
  .altura_agora = altura_agora,
  .icone = icone_energia,
  // Lives inside Ajustes (the "Energia" row opens this app): no icon on the home screen.
  .fora_da_home = true,
  .fundo_tema = true,
};
