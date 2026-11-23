// TAREFAS (TASKS) APP - the WINDOW for the system monitor (services/tab5_monitor).
//
// This app MEASURES NOTHING. The one measuring is the service, all the
// time, with the app open or closed: CPU per task, memory, peaks, and
// events fired by triggers. Here we only show what it has already stored -
// including what happened while nobody was looking.
//
// The screen is redrawn IN FULL once a second. With the compositor this does not
// flicker (everything goes to the virtual screen and only what changed reaches the panel), and the
// app does not need any "what changed" bookkeeping.

#include "sdkconfig.h"
#include "app.h"
#include "idioma.h"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_monitor.hpp"
#include "tab5_donos.hpp"
#include "tab5_web.hpp"
#include "lista.hpp"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "app.tarefas";

#include "esp_attr.h"
#include "esp_timer.h"
EXT_RAM_BSS_ATTR static mon_foto_t s_f;      // copy of the monitor snapshot (the screen reads from here)
EXT_RAM_BSS_ATTR static mon_foto_t s_nova;   // the last one the monitor delivered
static bool         s_forcar = true;         // next pass redraws everything

// THE TASK TABLE SCROLLS (kernel/lista): with many tasks it
// went under the events and vanished. Fixed-height row, read from the snapshot.
static lista_t s_lista;
static const int LIN = 34;

// GROUPED BY OWNER, like the "user / system" of
// task managers: first what belongs to an APP (the open one and the background ones),
// then the SERVICES, and finally the SYSTEM (kernel, drivers, IDF). The owner is
// declared by whoever created the task - hal/tab5_donos.hpp -, not guessed
// from the name. Each list row is either a group header or a task.
static const dono_tipo_t GRUPOS[] = { DONO_APP, DONO_FUNDO, DONO_SERVICO, DONO_SISTEMA };
static const char* const GRUPO_NOME[] = { N_("SISTEMA"), N_("SERVIÇOS"), N_("APP ABERTO"), N_("APPS EM FUNDO") };   // by dono_tipo_t
static int s_linhas[MON_MAX_TAREFAS + 4];   // >= 0: index in s_f.t; < 0: header (-1 - dono_tipo_t)
static int s_nlinhas = 0;

static uint16_t cor_grupo(int tipo)
{
  switch (tipo) {
    // CATEGORY colours (who owns the task), in the theme's tones.
    case DONO_APP:     return TEMA_APP_CIANO;
    case DONO_FUNDO:   return TEMA_APP_ROXO;
    case DONO_SERVICO: return TEMA_APP_AMARELO;
    default:           return TEMA_TEXTO_3;
  }
}

static void montar_linhas(void)
{
  s_nlinhas = 0;
  for (dono_tipo_t g : GRUPOS) {
    int n = 0;
    for (int i = 0; i < s_f.n; ++i) { if (s_f.t[i].tipo == g) { ++n; } }
    if (!n) { continue; }
    s_linhas[s_nlinhas++] = -1 - (int)g;
    for (int i = 0; i < s_f.n; ++i) { if (s_f.t[i].tipo == g) { s_linhas[s_nlinhas++] = i; } }
  }
}
static int  linha_altura(int, void*);
static void linha_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int, void*);
static float        s_cpu[MON_HIST_N];
static mon_evento_t s_ev[6];
static uint32_t     s_flash_usado = 0, s_flash_total = 0;
static bool         s_tem_flash = false;

static const float TAM_CORPO  = 2.6f * 1.5f;   // sizes requested in 13 and
static const float TAM_TITULO = 2.6f * 1.4f;

// ZERAR PICOS (reset peaks) button, in the footer (coordinates relative to the area).
static const int BT_L = 280, BT_A = 56;

static void ler_flash_programa(void)
{
  s_tem_flash = tab5_monitor_programa(&s_flash_usado, &s_flash_total);
}

static bool ao_abrir(void)
{
  ler_flash_programa();
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, linha_altura, linha_desenhar, nullptr);
  s_forcar = true;
  ESP_LOGI(TAG, "opened (the monitor was already measuring)");
  return true;
}

static void ao_fechar(void) { }

// General-purpose SRAM of the P4 = 768 KB minus the part the L2 cache takes. Never
// smaller than the measured heap (in case the configuration changes unexpectedly).
static uint32_t sram_fisica(uint32_t heap)
{
  const uint32_t t = 768u * 1024u - (uint32_t)CONFIG_CACHE_L2_CACHE_SIZE;
  return t > heap ? t : heap;
}

static void mem_txt(char* buf, size_t n, uint32_t b)
{
  if      (b >= 1024u * 1024u) { snprintf(buf, n, "%5.1fM", b / (1024.0f * 1024.0f)); }
  else if (b >= 1024u)         { snprintf(buf, n, "%5.1fK", b / 1024.0f); }
  else                         { snprintf(buf, n, "%5uB", (unsigned)b); }
}

// "ha 3m", "ha 2h05" (3m ago, 2h05 ago) - how long ago, from the uptime.
static void ha_quanto(char* buf, size_t n, uint32_t agora, uint32_t em)
{
  const uint32_t d = (agora >= em) ? agora - em : 0;
  if      (d < 60)    { snprintf(buf, n, "ha %us", (unsigned)d); }
  else if (d < 3600)  { snprintf(buf, n, tr("ha %um"), (unsigned)(d / 60)); }
  else                { snprintf(buf, n, "ha %uh%02u", (unsigned)(d / 3600), (unsigned)((d % 3600) / 60)); }
}

static uint16_t cor_folga(uint32_t livre, uint32_t total)
{
  if (total == 0) { return TEMA_TEXTO_3; }
  const int pct = (int)((uint64_t)livre * 100 / total);
  return (pct < 10) ? TEMA_PERIGO : (pct < 25) ? TEMA_ALERTA : TEMA_SUCESSO;
}

static void corpo(lgfx::LovyanGFX& d)   { tab5_fonte_corpo(d); tab5_texto(d, TAM_CORPO); }
static void titulo(lgfx::LovyanGFX& d)  { tab5_fonte_corpo(d); tab5_texto(d, TAM_TITULO); }
// Monospaced: 12 px per character in portrait; in landscape, 1.6x (~10 px), so the
// memory rows (53 characters) fit in the right-hand column.
static float s_mono = 2.0f;
static void mono(lgfx::LovyanGFX& d)    { d.setFont(&fonts::Font0); d.setTextSize(s_mono); }

// The screen geometry, the same for drawing and for touch.
// x1/l1: the table column; x2/l2: the events, graph and memory one. In portrait
// they are the same (one under the other); in landscape, side by side.
typedef struct { int y_rodape, y_prog, y_psram, y_bloco, y_ram, graf_y0, graf_y1, ev_y0, tab_y0, tab_y1, y_resumo, y_recursos, y_cab;
                 int x1, l1, x2, l2, tab_l; } geo_t;
static const int TAB_L = 720;   // the table width (the columns are fixed)
static const int EV_N = 5;
static geo_t geometria(const area_t& a)
{
  geo_t g;
  g.y_rodape = a.y + a.altura - BT_A - 8;
  g.y_prog   = g.y_rodape - 30;
  g.y_psram  = g.y_prog - 28;
  g.y_bloco  = g.y_psram - 28;
  g.y_ram    = g.y_bloco - 28;
  g.graf_y1  = g.y_ram - 14;
  g.graf_y0  = g.graf_y1 - 170;
  g.ev_y0    = g.graf_y0 - 14 - 40 - EV_N * 26;
  g.y_resumo = a.y + 12;                 // which apps are open
  g.y_recursos = g.y_resumo + LIN + 2;   // what holds off suspend (kernel/recursos)
  g.y_cab    = g.y_recursos + LIN + 4;   // table header
  g.tab_y0   = g.y_cab + LIN + 6;        // below the table header
  g.tab_y1   = g.ev_y0 - 10;
  g.x1 = g.x2 = a.x + 16;
  g.l1 = g.l2 = a.largura - 32;
  g.tab_l = a.largura;
  if (a.largura > a.altura && a.largura >= TAB_L + 400) {
    // LANDSCAPE: the table on the left at full height; events, graph and
    // memory (556 px) fit on the right (584 px), without scrolling the screen.
    g.tab_y1 = a.y + a.altura - 8;
    g.tab_l  = TAB_L;
    g.l1     = TAB_L - 32;
    g.x2     = a.x + TAB_L;
    g.l2     = a.largura - TAB_L - 16;
  }
  return g;
}

// In LANDSCAPE (physical keyboard, kernel/orientacao) the area is ~584 px. Previously the
// whole screen scrolled (the table, then events, graph and memory); now
// there are two columns (see geometry) and nothing scrolls except the table, as in portrait.

static int linha_altura(int, void*) { return LIN; }

// THE TEXT OF A TABLE ROW, exactly as it goes to the screen -
// used to draw and to know whether the row changed since the last sample.
typedef struct {
  char nome[40], cpu[12], pico[12], nucleo[8], prio[8], pilha[12];
  uint16_t cor_nome, cor_pico, cor_grupo;
  bool cabecalho;
} linha_txt_t;

static void linha_texto(int li, linha_txt_t* t)
{
  memset(t, 0, sizeof(*t));   // including the padding: the signature reads the raw bytes
  const int i = s_linhas[li];
  if (i < 0) {   // group header
    const int tipo = -1 - i;
    int n = 0;
    for (int k = 0; k < s_f.n; ++k) { if (s_f.t[k].tipo == tipo) { ++n; } }
    t->cabecalho = true;
    t->cor_grupo = cor_grupo(tipo);
    snprintf(t->nome, sizeof(t->nome), "%s (%d)", tr(GRUPO_NOME[tipo]), n);
    return;
  }
  const mon_tarefa_t& m = s_f.t[i];
  t->cor_grupo = cor_grupo(m.tipo);
  t->cor_nome  = m.pilha_livre < 512 ? TEMA_PERIGO : TEMA_TEXTO;
  t->cor_pico  = m.pct_pico > 60 ? TEMA_ALERTA : TEMA_TEXTO_2;
  snprintf(t->nome, sizeof(t->nome), "%-14.14s", m.nome);
  snprintf(t->cpu,  sizeof(t->cpu),  "%5.1f", (double)m.pct);
  snprintf(t->pico, sizeof(t->pico), "%5.1f", (double)m.pct_pico);
  if (m.core == tskNO_AFFINITY) { snprintf(t->nucleo, sizeof(t->nucleo), " --"); }
  else                          { snprintf(t->nucleo, sizeof(t->nucleo), "%3d", (int)m.core + 1); }
  snprintf(t->prio, sizeof(t->prio), "%3u", (unsigned)m.prioridade);
  mem_txt(t->pilha, sizeof(t->pilha), m.pilha_livre);
}

static uint32_t linha_assinatura(int li)
{
  linha_txt_t t;
  linha_texto(li, &t);
  uint32_t h = 2166136261u;   // FNV-1a over the text and the colours
  const uint8_t* p = (const uint8_t*)&t;
  for (size_t k = 0; k < sizeof(t); ++k) { h = (h ^ p[k]) * 16777619u; }
  return h;
}

static void linha_desenhar(lgfx::LovyanGFX& d, int li, int x0, int y, int, void*)
{
  if (li >= s_nlinhas) { return; }
  const int x = x0 + 16;
  linha_txt_t t;
  linha_texto(li, &t);
  corpo(d);
  d.setTextDatum(top_left);
  if (t.cabecalho) {
    d.setTextColor(t.cor_grupo, TEMA_FUNDO);
    d.drawString(t.nome, x, y + 2);
    d.drawFastHLine(x + (int)d.textWidth(t.nome) + 12, y + LIN / 2, 400, t.cor_grupo);
    return;
  }
  d.fillRect(x0 + 4, y + 4, 6, LIN - 8, t.cor_grupo);   // the group mark
  d.setTextColor(t.cor_nome, TEMA_FUNDO);
  d.drawString(t.nome, x, y);
  d.drawString(t.cpu,  x + 240, y);
  d.setTextColor(t.cor_pico, TEMA_FUNDO);
  d.drawString(t.pico, x + 340, y);
  d.setTextColor(t.cor_nome, TEMA_FUNDO);
  d.drawString(t.nucleo, x + 465, y);
  d.drawString(t.prio,   x + 540, y);
  d.drawString(t.pilha,  x + 590, y);
}

// ---------------------------------------------------------------------------
// FIELDS - PAINT ONLY WHAT CHANGED
// ---------------------------------------------------------------------------
// The window repainted EVERYTHING on every sample (1 Hz): clearing the whole area and
// redrawing table, events, graph and memory - ~70 ms of CPU and 1.6 MB of
// screen per second, to change half a dozen numbers. Now each line of
// text is a FIELD that stores what is on screen and is only repainted (clears its
// strip and writes) when the text or the colour changes. The table does the same
// per row (lista_item_mudou). What actually changes every second - the
// graph, the "N s ago" - is all that goes to the screen.
typedef struct { char txt[96]; uint16_t cor; bool ok; } campo_t;

static campo_t  s_c_resumo, s_c_recursos, s_c_ev_tit, s_c_ev[EV_N], s_c_cpu_tit, s_c_cpu_val;
static campo_t  s_c_ram, s_c_bloco, s_c_psram, s_c_prog, s_c_rodape;
static uint32_t s_sig[MON_MAX_TAREFAS + 4];
static int      s_sig_n = -1;              // -1 = the whole table again
static uint32_t s_graf_uptime = 0;         // sample the graph is showing

static void esquecer_campos(void)
{
  campo_t* todos[] = { &s_c_resumo, &s_c_recursos, &s_c_ev_tit, &s_c_cpu_tit, &s_c_cpu_val,
                       &s_c_ram, &s_c_bloco, &s_c_psram, &s_c_prog, &s_c_rodape };
  for (campo_t* c : todos) { c->ok = false; }
  for (int k = 0; k < EV_N; ++k) { s_c_ev[k].ok = false; }
  s_sig_n = -1;
  s_graf_uptime = 0;
}

// 'l' x 'a' is the field's strip (cleared before writing). datum
// top_right: x is the RIGHT edge of the text and the strip.
static void campo(lgfx::LovyanGFX& d, campo_t* c, const char* txt, uint16_t cor,
                  int x, int y, int l, int a, bool direita = false)
{
  if (c->ok && c->cor == cor && strcmp(c->txt, txt) == 0) { return; }
  snprintf(c->txt, sizeof(c->txt), "%s", txt);
  c->cor = cor;
  c->ok = true;
  d.fillRect(direita ? x - l : x, y, l, a, TEMA_FUNDO);
  d.setTextDatum(direita ? top_right : top_left);
  d.setTextColor(cor, TEMA_FUNDO);
  d.drawString(txt, x, y);
  d.setTextDatum(top_left);
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  const geo_t g = geometria(a);
  const area_t janela = { a.x, g.tab_y0, g.tab_l, g.tab_y1 - g.tab_y0 };
  s_mono = g.x2 != g.x1 ? 1.6f : 2.0f;

  // THE STRUCTURE (titles, lines, frame, button) only when the system has just
  // cleared the area - opening, coming back from idle. In that case everything the
  // fields thought was on screen no longer is.
  const bool estrutura = sistema_area_foi_limpa() || s_forcar;
  if (estrutura) { esquecer_campos(); }

  // ONLY THE SCROLLING, between two samples: shifts the table and draws the exposed
  // strip with the SAME snapshot (the one already on screen) - otherwise the new strip
  // would not match the shifted pixels. The rest of the screen stays as it is.
  const bool tem = tab5_monitor_foto(&s_nova);
  // OLD SNAPSHOT: the monitor samples at 1 Hz; past 3 s, the screen says what
  // it shows is old (and the footer has to be repainted to say so).
  const uint32_t agora_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
  const uint32_t atraso = (tem && agora_s > s_nova.uptime_s) ? agora_s - s_nova.uptime_s : 0;
  if (tem && !estrutura && s_nova.uptime_s == s_f.uptime_s && atraso <= 3) {
    lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, false);
    d.setTextStyle(estilo);
    return;
  }
  s_forcar = false;
  d.setTextDatum(top_left);

  const int x = g.x1, larg = g.l1, lin = LIN;
  const int xb = g.x2, lb = g.l2;   // events, graph, memory and footer
  char buf[96], m1[12], m2[12], m3[12], h[16];

  if (!tem) {
    corpo(d);
    campo(d, &s_c_resumo, tr("monitor ainda sem a primeira amostra..."), TEMA_TEXTO_2, x, g.y_resumo, larg, lin);
    d.setTextStyle(estilo);
    return;
  }
  s_f = s_nova;
  const uint32_t agora = s_f.uptime_s;

  const int y_rodape = g.y_rodape, y_prog = g.y_prog, y_psram = g.y_psram;
  const int y_bloco = g.y_bloco, y_ram = g.y_ram, graf_y0 = g.graf_y0, graf_y1 = g.graf_y1;
  const int ev_y0 = g.ev_y0;
  const int gy0 = graf_y0 + 40, gh = graf_y1 - gy0;

  if (estrutura) {
    // Table header.
    titulo(d); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(tr("TAREFA"), x, g.y_cab);
    d.drawString(tr("CPU"),    x + 250, g.y_cab);
    d.drawString(tr("PICO"),   x + 345, g.y_cab);
    d.drawString(tr("NUC"),    x + 460, g.y_cab);
    d.drawString(tr("PRI"),    x + 535, g.y_cab);
    d.drawString(tr("PILHA"),  x + 600, g.y_cab);
    d.drawFastHLine(x, g.y_cab + lin, larg, TEMA_LINHA);
    // Graph frame and memory separator.
    d.drawRect(xb, gy0, lb, gh, TEMA_LINHA);
    d.drawFastHLine(xb, y_ram - 8, lb, TEMA_LINHA);
    tab5_botao(d, a.x + a.largura - 16 - BT_L, y_rodape, BT_L, BT_A, tr("ZERAR PICOS"), TEMA_ALERTA,
               TEMA_FUNDO, TAB5_BOTAO_TEXTO_GRANDE);
  }

  // ---- who is open: the app in front and the background parts -------------
  {
    corpo(d);
    fundo_info_t fi[4];
    const int nf = sistema_fundo_listar(fi, 4);
    char resumo[96];
    int k = snprintf(resumo, sizeof(resumo), tr("EM FUNDO: "));
    // Each app open on the web server counts as running in the background (on the PC).
    const char* web[WEB_PARTES_MAX];
    const int nw = tab5_web_abertos(web, WEB_PARTES_MAX);
    if (nf == 0 && nw == 0) { snprintf(resumo + k, sizeof(resumo) - k, tr("nenhum app")); }
    for (int j = 0; j < nf && k < (int)sizeof(resumo); ++j) {
      k += snprintf(resumo + k, sizeof(resumo) - k, "%s%s", j ? ", " : "", fi[j].nome);
    }
    for (int j = 0; j < nw && k < (int)sizeof(resumo); ++j) {
      k += snprintf(resumo + k, sizeof(resumo) - k, tr("%s%s (no PC)"), (nf || j) ? ", " : "", tr(web[j]));
    }
    campo(d, &s_c_resumo, resumo, (nf || nw) ? TEMA_APP_ROXO : TEMA_TEXTO_3, x, g.y_resumo, larg, lin);
  }

  // ---- what holds off suspend: the resources in use (kernel/recursos.h) ---
  // Answers "why does it not sleep": each row is a resource and who declared it
  // (the app, the browser, Mesh). None = the screen off drops to 40 MHz.
  {
    corpo(d);
    recurso_uso_t u[RECURSOS_MAX];
    const int nr = sistema_recursos(u, RECURSOS_MAX);
    char rr[96];
    int k = snprintf(rr, sizeof(rr), tr("EM USO: "));
    if (nr == 0) { snprintf(rr + k, sizeof(rr) - k, tr("nenhum recurso")); }
    for (int j = 0; j < nr && k < (int)sizeof(rr); ++j) {
      k += snprintf(rr + k, sizeof(rr) - k, "%s%s (%s)", j ? ", " : "", tr(sistema_recurso_nome(u[j].r)), u[j].quem);
    }
    campo(d, &s_c_recursos, rr, nr ? TEMA_ALERTA : TEMA_TEXTO_3, x, g.y_recursos, larg, lin);
  }

  // ---- task table: only the rows that changed -----------------------------
  {
    const int n_antes = s_nlinhas;
    int antes[MON_MAX_TAREFAS + 4];
    memcpy(antes, s_linhas, sizeof(int) * (size_t)n_antes);
    montar_linhas();
    const bool mesma_forma = s_sig_n == s_nlinhas && n_antes == s_nlinhas
                          && memcmp(antes, s_linhas, sizeof(int) * (size_t)s_nlinhas) == 0;
    if (!mesma_forma) {
      // A task came or went, or the order changed: the whole list again.
      lista_itens_mudaram(&s_lista, s_nlinhas);
      for (int li = 0; li < s_nlinhas; ++li) { s_sig[li] = linha_assinatura(li); }
      s_sig_n = s_nlinhas;
      lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, true);
    } else {
      for (int li = 0; li < s_nlinhas; ++li) {
        const uint32_t sg = linha_assinatura(li);
        if (sg != s_sig[li]) { s_sig[li] = sg; lista_item_mudou(&s_lista, li); }
      }
      lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, estrutura);
    }
  }

  // ---- events (what the triggers recorded) --------------------------------
  titulo(d);
  snprintf(buf, sizeof(buf), tr("EVENTOS  (%lu desde o boot)"), (unsigned long)tab5_monitor_total_eventos());
  campo(d, &s_c_ev_tit, buf, TEMA_TEXTO, xb, ev_y0, lb, 36);
  mono(d);
  const int nev = tab5_monitor_eventos(s_ev, EV_N);
  for (int k = 0; k < EV_N; ++k) {
    const int ey = ev_y0 + 40 + k * 26;
    uint16_t cor = TEMA_TEXTO_2;
    if (k >= nev) {
      buf[0] = '\0';
      if (k == 0) { snprintf(buf, sizeof(buf), tr("nenhum - tudo dentro dos limites")); cor = TEMA_TEXTO_3; }
    } else {
      ha_quanto(h, sizeof(h), agora, s_ev[k].uptime_s);
      snprintf(buf, sizeof(buf), "%-8s %.*s", h, (int)(lb / (6.0f * s_mono)) - 9, s_ev[k].texto);
      cor = (k == 0) ? TEMA_ALERTA : TEMA_TEXTO_2;
    }
    campo(d, &s_c_ev[k], buf, cor, xb, ey, lb, 26);
  }

  // ---- CPU graph (5 min) --------------------------------------------------
  {
    titulo(d);
    ha_quanto(h, sizeof(h), agora, s_f.cpu_pico_em);
    snprintf(buf, sizeof(buf), tr("CPU 5 min   pico %.0f%% %s"), (double)s_f.cpu_pico, h);
    campo(d, &s_c_cpu_tit, buf, TEMA_TEXTO, xb, graf_y0, lb - 120, 36);
    snprintf(buf, sizeof(buf), "%.0f%%", (double)s_f.cpu);
    campo(d, &s_c_cpu_val, buf, s_f.cpu > 80 ? TEMA_PERIGO : s_f.cpu > 50 ? TEMA_ALERTA : TEMA_SUCESSO,
          xb + lb, graf_y0, 120, 36, true);

    // The graph changes on every sample (a new one enters from the right): the CORE is
    // repainted, once per sample; the frame stays.
    if (s_graf_uptime != agora) {
      s_graf_uptime = agora;
      d.fillRect(xb + 1, gy0 + 1, lb - 2, gh - 2, TEMA_FUNDO);
      for (int k = 1; k < 4; ++k) {
        d.drawFastHLine(xb + 1, gy0 + (gh * k) / 4, lb - 2, TAB5_GRAFICO_COR_GRADE);
        d.drawFastVLine(xb + (lb * k) / 4, gy0 + 1, gh - 2, TAB5_GRAFICO_COR_GRADE);
      }
      const int n = tab5_monitor_historico(s_cpu, nullptr, nullptr, MON_HIST_N);
      int px0 = 0, py0 = 0;
      for (int k = 0; k < n; ++k) {
        // Right-aligned: the newest always at the edge, the graph fills from
        // right to left in the first 5 minutes.
        const int px = xb + 2 + ((MON_HIST_N - n + k) * (lb - 4)) / (MON_HIST_N - 1);
        const int py = gy0 + gh - 2 - (int)(s_cpu[k] * (float)(gh - 4) / 100.0f);
        // DATA colour: the CPU curve
        if (k > 0) { d.drawLine(px0, py0, px, py, TEMA_APP_LIMA); }
        px0 = px; py0 = py;
      }
    }
  }

  // ---- memory: now, total and the LOWEST ever left, with when -------------
  mono(d);
  const mon_mem_t& r = s_f.ram;
  // The total shown is the P4's PHYSICAL SRAM (768 KB minus the L2 cache), not just the
  // heap: the rest is code and fixed system data. The headroom colour is still
  // measured against the heap, which is what can actually be allocated.
  mem_txt(m1, sizeof(m1), r.livre); mem_txt(m2, sizeof(m2), sram_fisica(r.total)); mem_txt(m3, sizeof(m3), r.min_livre);
  ha_quanto(h, sizeof(h), agora, r.min_livre_em);
  // FIXED COLUMNS (Font0 is monospaced): label 6, description 12, value 14 -
  // the "min" falls in the same column on all three rows. Previously each row counted its
  // own spaces and the block's "min" came out 2 places off.
  char v[24];
  snprintf(v, sizeof(v), "%s/%s", m1, m2);
  snprintf(buf, sizeof(buf), tr("%-6s%-12s%-14s min %s %s"), tr("RAM"), tr("livre"), v, m3, h);
  campo(d, &s_c_ram, buf, cor_folga(r.livre, r.total), xb, y_ram, lb, 24);
  mem_txt(m1, sizeof(m1), r.maior_bloco); mem_txt(m3, sizeof(m3), r.min_bloco);
  ha_quanto(h, sizeof(h), agora, r.min_bloco_em);
  snprintf(buf, sizeof(buf), tr("%-6s%-12s%-14s min %s %s"), "", tr("maior bloco"), m1, m3, h);
  campo(d, &s_c_bloco, buf, r.maior_bloco < 16 * 1024 ? TEMA_PERIGO : TEMA_TEXTO_2, xb, y_bloco, lb, 24);
  const mon_mem_t& p = s_f.psram;
  mem_txt(m1, sizeof(m1), p.livre); mem_txt(m2, sizeof(m2), p.total); mem_txt(m3, sizeof(m3), p.min_livre);
  ha_quanto(h, sizeof(h), agora, p.min_livre_em);
  snprintf(v, sizeof(v), "%s/%s", m1, m2);
  snprintf(buf, sizeof(buf), tr("%-6s%-12s%-14s min %s %s"), tr("PSRAM"), tr("livre"), v, m3, h);
  campo(d, &s_c_psram, buf, cor_folga(p.livre, p.total), xb, y_psram, lb, 24);
  if (s_tem_flash) {
    mem_txt(m1, sizeof(m1), s_flash_usado); mem_txt(m2, sizeof(m2), s_flash_total);
    snprintf(v, sizeof(v), "%s/%s", m1, m2);
    snprintf(buf, sizeof(buf), "%-6s%-12s%-14s", tr("PROG"), tr("usado"), v);
    campo(d, &s_c_prog, buf, cor_folga(s_flash_total - s_flash_usado, s_flash_total), xb, y_prog, lb, 24);
  }

  // ---- footer: uptime (the ZERAR PICOS button is structure) ---------------
  const uint32_t up = agora;
  if      (up < 3600)  { snprintf(h, sizeof(h), "%um%02us", (unsigned)(up / 60), (unsigned)(up % 60)); }
  else if (up < 86400) { snprintf(h, sizeof(h), "%uh%02um", (unsigned)(up / 3600), (unsigned)((up % 3600) / 60)); }
  else                 { snprintf(h, sizeof(h), "%ud%02uh", (unsigned)(up / 86400), (unsigned)((up % 86400) / 3600)); }
  corpo(d);
  if (atraso > 3) {
    snprintf(buf, sizeof(buf), tr("monitor sem amostra há %us"), (unsigned)atraso);
    campo(d, &s_c_rodape, buf, TEMA_ALERTA, xb, y_rodape + 10, lb - BT_L - 16, lin);
  } else {
    snprintf(buf, sizeof(buf), tr("%d tarefas | no ar %s"), s_f.n, h);
    campo(d, &s_c_rodape, buf, TEMA_TEXTO_2, xb, y_rodape + 10, lb - BT_L - 16, lin);
  }

  d.setTextStyle(estilo);
}

// Dragging on the table scrolls; the rest of the screen does not scroll.
static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  if (lista_item_tocado(&s_lista, x, y, nullptr) == LISTA_ARRASTE) { return; }   // end of a drag is not a click
  const area_t a = sistema_area();
  const int bx = a.largura - 16 - BT_L;
  const int by = a.altura - BT_A - 8;
  if (x >= bx && x < bx + BT_L && y >= by && y < by + BT_A) {
    tab5_monitor_zerar_picos();
    sistema_pedir_redesenho();
  }
}

// ---------------------------------------------------------------------------
// HOME SCREEN ICON
// ---------------------------------------------------------------------------
// Primitives and fractions of the side, like the Mesh logo: comes out clean at any
// size, uses no flash and has no jaggies. The large letter that was here was
// the built-in font enlarged 6x - each pixel of it became a 6x6 square.
static void icone_tarefas(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(22, 40, 26);
  const uint16_t barra = TFT_GREENYELLOW;

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  // Bars of different heights: the app is a monitor, and a monitor is recognised by its
  // irregular profile. Equal bars would read as "list", which is something else.
  const float alt[4] = { 0.30f, 0.52f, 0.22f, 0.42f };
  const int larg = lado / 9;
  const int vao  = lado / 22;
  const int base = y + (int)(0.76f * lado);
  int bx = x + (int)(0.20f * lado);

  for (int i = 0; i < 4; ++i) {
    const int h = (int)(alt[i] * lado);
  // The last one in orange: the app exists to find who is using up resources, and the
  // icon already points to that.
    d.fillRoundRect(bx, base - h, larg, h, larg / 3,
                    (i == 1) ? TFT_ORANGE : barra);
    bx += larg + vao;
  }

  // Baseline: gives the bars a floor and completes the reading as a graph.
  d.fillRect(x + (int)(0.16f * lado), base + lado / 40,
             (int)(0.68f * lado), lado / 40, d.color565(90, 130, 95));
}

extern const app_t APP_TAREFAS = {
  .nome  = "Tarefas",
  .cor   = TFT_GREENYELLOW,
  .perfil = { },
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .periodo_ms = 1000,   // the monitor samples at 1Hz; the window follows
  .icone = icone_tarefas,
  .fundo_tema = true,
};
