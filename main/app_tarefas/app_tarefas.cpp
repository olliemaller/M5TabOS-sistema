// APP TAREFAS - a JANELA do monitor do sistema (services/tab5_monitor).
//
// Este app NAO MEDE NADA. Quem mede e o servico, o tempo
// todo, com o app aberto ou fechado: CPU por tarefa, memoria, picos, e
// eventos disparados por gatilhos. Aqui so se mostra o que ele ja guardou -
// inclusive o que aconteceu enquanto ninguem olhava.
//
// A tela e redesenhada INTEIRA uma vez por segundo. Com o compositor isso nao
// pisca (tudo vai para a tela virtual e so o que mudou chega ao painel), e o
// app nao precisa de nenhuma contabilidade de "o que mudou".

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
EXT_RAM_BSS_ATTR static mon_foto_t s_f;      // copia da foto do monitor (a tela le daqui)
EXT_RAM_BSS_ATTR static mon_foto_t s_nova;   // a ultima que o monitor entregou
static bool         s_forcar = true;         // proxima passada redesenha tudo

// A TABELA DE TAREFAS ROLA (kernel/lista): com muitas tarefas ela
// passava por baixo dos eventos e sumia. Linha de altura fixa, lida da foto.
static lista_t s_lista;
static const int LIN = 34;

// AGRUPADA POR DONO, como o "usuario / sistema" dos
// gerenciadores de tarefa: primeiro o que e de APP (o aberto e os de fundo),
// depois os SERVICOS, e por fim o SISTEMA (kernel, drivers, IDF). O dono vem
// declarado por quem criou a tarefa - hal/tab5_donos.hpp -, nao adivinhado
// pelo nome. Cada linha da lista e um cabecalho de grupo ou uma tarefa.
static const dono_tipo_t GRUPOS[] = { DONO_APP, DONO_FUNDO, DONO_SERVICO, DONO_SISTEMA };
static const char* const GRUPO_NOME[] = { N_("SISTEMA"), N_("SERVIÇOS"), N_("APP ABERTO"), N_("APPS EM FUNDO") };   // por dono_tipo_t
static int s_linhas[MON_MAX_TAREFAS + 4];   // >= 0: indice em s_f.t; < 0: cabecalho (-1 - dono_tipo_t)
static int s_nlinhas = 0;

static uint16_t cor_grupo(int tipo)
{
  switch (tipo) {
    // Cores de CATEGORIA (quem e o dono da tarefa), nos tons do tema.
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

static const float TAM_CORPO  = 2.6f * 1.5f;   // tamanhos pedidos em 13 e
static const float TAM_TITULO = 2.6f * 1.4f;

// Botao ZERAR PICOS, no rodape (coordenadas relativas a area).
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
  ESP_LOGI(TAG, "aberto (o monitor ja estava medindo)");
  return true;
}

static void ao_fechar(void) { }

// SRAM de uso geral do P4 = 768 KB menos a parte que o cache L2 toma. Nunca
// menor que o heap medido (se a configuracao mudar de forma inesperada).
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

// "ha 3m", "ha 2h05" - quanto tempo atras, a partir do uptime.
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
// Monoespacada: 12 px por caractere em pe; deitada, 1,6x (~10 px), para as
// linhas de memoria (53 caracteres) caberem na coluna da direita.
static float s_mono = 2.0f;
static void mono(lgfx::LovyanGFX& d)    { d.setFont(&fonts::Font0); d.setTextSize(s_mono); }

// A geometria da tela, a mesma para o desenho e para o toque.
// x1/l1: a coluna da tabela; x2/l2: a de eventos, grafico e memoria. Em pe
// sao a mesma (uma embaixo da outra); deitada, lado a lado.
typedef struct { int y_rodape, y_prog, y_psram, y_bloco, y_ram, graf_y0, graf_y1, ev_y0, tab_y0, tab_y1, y_resumo, y_recursos, y_cab;
                 int x1, l1, x2, l2, tab_l; } geo_t;
static const int TAB_L = 720;   // a largura da tabela (as colunas sao fixas)
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
  g.y_resumo = a.y + 12;                 // quais apps estao abertos
  g.y_recursos = g.y_resumo + LIN + 2;   // o que segura a suspensao (kernel/recursos)
  g.y_cab    = g.y_recursos + LIN + 4;   // cabecalho da tabela
  g.tab_y0   = g.y_cab + LIN + 6;        // abaixo do cabecalho da tabela
  g.tab_y1   = g.ev_y0 - 10;
  g.x1 = g.x2 = a.x + 16;
  g.l1 = g.l2 = a.largura - 32;
  g.tab_l = a.largura;
  if (a.largura > a.altura && a.largura >= TAB_L + 400) {
    // DEITADA: a tabela a esquerda com a altura toda; eventos, grafico e
    // memoria (556 px) cabem na direita (584 px), sem rolar a tela.
    g.tab_y1 = a.y + a.altura - 8;
    g.tab_l  = TAB_L;
    g.l1     = TAB_L - 32;
    g.x2     = a.x + TAB_L;
    g.l2     = a.largura - TAB_L - 16;
  }
  return g;
}

// DEITADA (teclado fisico, kernel/orientacao) a area tem ~584 px. Antes a
// tela inteira rolava (a tabela, e depois eventos, grafico e memoria); agora
// sao duas colunas (ver geometria) e nada rola alem da tabela, como em pe.

static int linha_altura(int, void*) { return LIN; }

// O TEXTO DE UMA LINHA DA TABELA, exatamente como ela vai para a tela -
// usado para desenhar e para saber se a linha mudou desde a ultima amostra.
typedef struct {
  char nome[40], cpu[12], pico[12], nucleo[8], prio[8], pilha[12];
  uint16_t cor_nome, cor_pico, cor_grupo;
  bool cabecalho;
} linha_txt_t;

static void linha_texto(int li, linha_txt_t* t)
{
  memset(t, 0, sizeof(*t));   // inclusive o enchimento: a assinatura le os bytes crus
  const int i = s_linhas[li];
  if (i < 0) {   // cabecalho do grupo
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
  uint32_t h = 2166136261u;   // FNV-1a sobre o texto e as cores
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
  d.fillRect(x0 + 4, y + 4, 6, LIN - 8, t.cor_grupo);   // a marca do grupo
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
// CAMPOS - PINTAR SO O QUE MUDOU
// ---------------------------------------------------------------------------
// A janela repintava TUDO a cada amostra (1 Hz): apagar a area inteira e
// redesenhar tabela, eventos, grafico e memoria - ~70 ms de CPU e 1,6 MB de
// tela por segundo, para mudar meia duzia de numeros. Agora cada linha de
// texto e um CAMPO que guarda o que esta na tela e so e repintado (apaga a
// faixa dele e escreve) quando o texto ou a cor mudam. A tabela faz o mesmo
// por linha (lista_item_mudou). O que muda de fato a cada segundo - o
// grafico, os "ha Ns" - e so isso que vai para a tela.
typedef struct { char txt[96]; uint16_t cor; bool ok; } campo_t;

static campo_t  s_c_resumo, s_c_recursos, s_c_ev_tit, s_c_ev[EV_N], s_c_cpu_tit, s_c_cpu_val;
static campo_t  s_c_ram, s_c_bloco, s_c_psram, s_c_prog, s_c_rodape;
static uint32_t s_sig[MON_MAX_TAREFAS + 4];
static int      s_sig_n = -1;              // -1 = a tabela inteira de novo
static uint32_t s_graf_uptime = 0;         // amostra que o grafico mostra

static void esquecer_campos(void)
{
  campo_t* todos[] = { &s_c_resumo, &s_c_recursos, &s_c_ev_tit, &s_c_cpu_tit, &s_c_cpu_val,
                       &s_c_ram, &s_c_bloco, &s_c_psram, &s_c_prog, &s_c_rodape };
  for (campo_t* c : todos) { c->ok = false; }
  for (int k = 0; k < EV_N; ++k) { s_c_ev[k].ok = false; }
  s_sig_n = -1;
  s_graf_uptime = 0;
}

// 'l' x 'a' e a faixa do campo (apagada antes de escrever). datum
// top_right: x e a borda DIREITA do texto e da faixa.
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

  // A ESTRUTURA (titulos, linhas, moldura, botao) so quando o sistema acabou
  // de limpar a area - abrir, voltar do repouso. Nesse caso tudo o que os
  // campos achavam que estava na tela ja nao esta.
  const bool estrutura = sistema_area_foi_limpa() || s_forcar;
  if (estrutura) { esquecer_campos(); }

  // SO A ROLAGEM, entre duas amostras: desloca a tabela e desenha a faixa
  // exposta com a MESMA foto (a que ja esta na tela) - senao a faixa nova
  // nao bateria com os pixels deslocados. O resto da tela fica como esta.
  const bool tem = tab5_monitor_foto(&s_nova);
  // FOTO VELHA: o monitor amostra a 1 Hz; passando de 3 s, a tela diz que o
  // que mostra e antigo (e o rodape tem que ser repintado para dizer isso).
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
  const int xb = g.x2, lb = g.l2;   // eventos, grafico, memoria e rodape
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
    // Cabecalho da tabela.
    titulo(d); d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(tr("TAREFA"), x, g.y_cab);
    d.drawString(tr("CPU"),    x + 250, g.y_cab);
    d.drawString(tr("PICO"),   x + 345, g.y_cab);
    d.drawString(tr("NUC"),    x + 460, g.y_cab);
    d.drawString(tr("PRI"),    x + 535, g.y_cab);
    d.drawString(tr("PILHA"),  x + 600, g.y_cab);
    d.drawFastHLine(x, g.y_cab + lin, larg, TEMA_LINHA);
    // Moldura do grafico e separador da memoria.
    d.drawRect(xb, gy0, lb, gh, TEMA_LINHA);
    d.drawFastHLine(xb, y_ram - 8, lb, TEMA_LINHA);
    tab5_botao(d, a.x + a.largura - 16 - BT_L, y_rodape, BT_L, BT_A, tr("ZERAR PICOS"), TEMA_ALERTA,
               TEMA_FUNDO, TAB5_BOTAO_TEXTO_GRANDE);
  }

  // ---- quem esta aberto: o app na frente e as partes de fundo ------------
  {
    corpo(d);
    fundo_info_t fi[4];
    const int nf = sistema_fundo_listar(fi, 4);
    char resumo[96];
    int k = snprintf(resumo, sizeof(resumo), tr("EM FUNDO: "));
    // Cada app aberto no servidor web conta como rodando de fundo (no PC).
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

  // ---- o que segura a suspensao: os recursos em uso (kernel/recursos.h) ---
  // Responde "por que nao dorme": cada linha e um recurso e quem o declarou
  // (o app, o navegador, o Mesh). Nenhum = a tela apagada desce a 40 MHz.
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

  // ---- tabela de tarefas: so as linhas que mudaram ------------------------
  {
    const int n_antes = s_nlinhas;
    int antes[MON_MAX_TAREFAS + 4];
    memcpy(antes, s_linhas, sizeof(int) * (size_t)n_antes);
    montar_linhas();
    const bool mesma_forma = s_sig_n == s_nlinhas && n_antes == s_nlinhas
                          && memcmp(antes, s_linhas, sizeof(int) * (size_t)s_nlinhas) == 0;
    if (!mesma_forma) {
      // Entrou ou saiu tarefa, ou a ordem mudou: a lista inteira de novo.
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

  // ---- eventos (o que os gatilhos registraram) ----------------------------
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

  // ---- grafico de CPU (5 min) ---------------------------------------------
  {
    titulo(d);
    ha_quanto(h, sizeof(h), agora, s_f.cpu_pico_em);
    snprintf(buf, sizeof(buf), tr("CPU 5 min   pico %.0f%% %s"), (double)s_f.cpu_pico, h);
    campo(d, &s_c_cpu_tit, buf, TEMA_TEXTO, xb, graf_y0, lb - 120, 36);
    snprintf(buf, sizeof(buf), "%.0f%%", (double)s_f.cpu);
    campo(d, &s_c_cpu_val, buf, s_f.cpu > 80 ? TEMA_PERIGO : s_f.cpu > 50 ? TEMA_ALERTA : TEMA_SUCESSO,
          xb + lb, graf_y0, 120, 36, true);

    // O grafico muda a cada amostra (uma nova entra pela direita): o MIOLO e
    // repintado, uma vez por amostra; a moldura fica.
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
        // Alinhado a direita: o mais novo sempre na borda, o grafico enche da
        // direita para a esquerda nos primeiros 5 minutos.
        const int px = xb + 2 + ((MON_HIST_N - n + k) * (lb - 4)) / (MON_HIST_N - 1);
        const int py = gy0 + gh - 2 - (int)(s_cpu[k] * (float)(gh - 4) / 100.0f);
        // cor de DADO: a curva da CPU
        if (k > 0) { d.drawLine(px0, py0, px, py, TEMA_APP_LIMA); }
        px0 = px; py0 = py;
      }
    }
  }

  // ---- memoria: agora, total e o MENOR que ja sobrou, com quando ----------
  mono(d);
  const mon_mem_t& r = s_f.ram;
  // O total mostrado e a SRAM FISICA do P4 (768 KB menos o cache L2), nao so o
  // heap: o resto e codigo e dados fixos do sistema. A cor da folga continua
  // medida contra o heap, que e o que de fato pode ser alocado.
  mem_txt(m1, sizeof(m1), r.livre); mem_txt(m2, sizeof(m2), sram_fisica(r.total)); mem_txt(m3, sizeof(m3), r.min_livre);
  ha_quanto(h, sizeof(h), agora, r.min_livre_em);
  // COLUNAS FIXAS (Font0 e monoespacada): rotulo 6, descricao 12, valor 14 -
  // o "min" cai na mesma coluna nas tres linhas. Antes cada linha contava os
  // proprios espacos e o "min" do bloco saia 2 casas fora.
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

  // ---- rodape: uptime (o botao ZERAR PICOS e estrutura) -------------------
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

// Arrastar na tabela rola; o resto da tela nao rola.
static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  if (lista_item_tocado(&s_lista, x, y, nullptr) == LISTA_ARRASTE) { return; }   // fim de arraste nao e clique
  const area_t a = sistema_area();
  const int bx = a.largura - 16 - BT_L;
  const int by = a.altura - BT_A - 8;
  if (x >= bx && x < bx + BT_L && y >= by && y < by + BT_A) {
    tab5_monitor_zerar_picos();
    sistema_pedir_redesenho();
  }
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao do lado, como a marca do Mesh: sai limpo em qualquer
// tamanho, nao gasta flash e nao serrilha. A letra grande que estava aqui era
// a fonte embutida ampliada 6x - cada pixel dela virava um quadrado de 6.
static void icone_tarefas(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(22, 40, 26);
  const uint16_t barra = TFT_GREENYELLOW;

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  // Barras de altura diferente: o app e um monitor, e monitor se reconhece pelo
  // perfil irregular. Barras iguais leriam como "lista", que e outra coisa.
  const float alt[4] = { 0.30f, 0.52f, 0.22f, 0.42f };
  const int larg = lado / 9;
  const int vao  = lado / 22;
  const int base = y + (int)(0.76f * lado);
  int bx = x + (int)(0.20f * lado);

  for (int i = 0; i < 4; ++i) {
    const int h = (int)(alt[i] * lado);
    // A ultima em laranja: o app existe para achar quem esta gastando, e o
    // icone ja aponta para isso.
    d.fillRoundRect(bx, base - h, larg, h, larg / 3,
                    (i == 1) ? TFT_ORANGE : barra);
    bx += larg + vao;
  }

  // Linha de base: da chao as barras e fecha a leitura de grafico.
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
  .periodo_ms = 1000,   // o monitor amostra a 1Hz; a janela acompanha
  .icone = icone_tarefas,
  .fundo_tema = true,
};
