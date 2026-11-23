// APP MUSICA - o tocador. A pessoa poe as musicas na pasta Music (ou Música...) do cartao
// (com subpastas, se quiser); o app lista, e tocar uma musica toca a pasta
// dela inteira a partir dali - ou uma playlist. Um PENDRIVE no USB-A aparece
// no topo da lista de Pastas: o pendrive inteiro, com as pastas dele, toca
// direto de la (sem copiar nada).
//
// Este app e SO A INTERFACE. Quem toca e o tocador do sistema
// (services/tab5_musica): achar a pasta, listar, a fila, aleatorio/repetir,
// as playlists (.m3u), a duracao de cada arquivo, a capa (etiqueta, pasta ou
// internet), decodificar, o alto-falante, o espectro - tudo la. Fechar o app
// nao para a musica; com ele fechado, a barra mostra o que toca e a central
// de avisos tem o miniplayer. O modulo Berry 'musica' usa o mesmo servico.
//
// DUAS TELAS:
//   TOCANDO    - a capa grande (ou o espectro, tocando nela), titulo, artista,
//                coracao (playlist Favoritas), a linha do tempo, aleatorio,
//                anterior, tocar, proxima, repetir, volume.
//   BIBLIOTECA - abas Pastas e Playlists; cada musica com a duracao a direita
//                e o botao de por numa playlist; o miniplayer embaixo.
//
// O QUE LE O CARTAO NAO RODA AQUI: listar, medir duracoes, abrir a capa e
// mexer nas playlists vao para o worker (kernel/trabalho.hpp) e voltam por
// AVISO_TRABALHO.

#include "app.h"
#include "idioma.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "lista.hpp"
#include "trabalho.hpp"
#include "teclado_virtual.hpp"
#include "tab5_teclado.hpp"
#include "ajustes.h"
#include "tab5_musica.hpp"
#include "tab5_volumes.hpp"
#include "tab5_hora.hpp"   // tab5_agora_ms

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>
#include <strings.h>

static const char* TAG = "app.musica";
#define APP_NOME "Música"   // o mesmo nome da parte de fundo do servico: o cartao da central abre este app

static const uint16_t LILAS = 0xAC7F;   // rgb(176,140,255) - so o ICONE do app (fica como esta; o resto da tela usa o tema)

// O destaque da tela, da lista e do miniplayer: a cor do tema do espectro.
// A REGRA E DA INTERFACE (o servico so guarda e publica a cor que receber):
// na ordem dos TEMAS, mais abaixo - arco-iris, "lilas" (agora o azul do
// tema do sistema), fogo, oceano, neon. As outras cores sao escolha do
// usuario no espectro e seguem como estao.
static uint16_t destaque(void)
{
  static const uint16_t COR[] = { 0xFBCB /* coral */, TEMA_DESTAQUE, 0xFCA5 /* laranja */,
                                  0x2E5E /* ciano */, 0xF9F9 /* rosa */ };
  return COR[ajustes_tema_espectro() % (sizeof(COR) / sizeof(COR[0]))];
}

// A tinta do que fica EM CIMA do destaque (texto e icone): branca no azul do
// tema; preta nas outras cores do espectro, que sao claras.
static uint16_t sobre_destaque(void)
{
  return (ajustes_tema_espectro() % 5) == 1 ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TFT_BLACK;   // 1 = o tema azul
}

static void tempo_txt(char* out, size_t n, uint32_t ms)
{
  const uint32_t s = ms / 1000;
  if (s >= 3600) { snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60)); }
  else           { snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }
}

// O que mostrar de um arquivo: "Artista - Titulo (Video).mp3" -> titulo
// "Titulo", sub "Artista". Quem limpa o nome e o servico (o mesmo que usa
// para o que toca; la a etiqueta ID3 vence).
static void nome_para_mostrar(const char* nome_cartao, char* tit, size_t tt, char* sub, size_t ts)
{
  sub[0] = '\0';
  tab5_musica_titulo_do_arquivo(nome_cartao, tit, tt, sub, ts);
}

// ---- icones (primitivas: a fonte nao tem esses simbolos) ---------------------
static void ic_tocar(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t c)
{
  d.fillTriangle(cx - r * 4 / 10, cy - r / 2, cx - r * 4 / 10, cy + r / 2, cx + r * 6 / 10, cy, c);
}
static void ic_pausa(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t c)
{
  const int w = r * 3 / 10, h = r;
  d.fillRoundRect(cx - w - r / 8, cy - h / 2, w, h, 3, c);
  d.fillRoundRect(cx + r / 8, cy - h / 2, w, h, 3, c);
}
static void ic_pular(lgfx::LovyanGFX& d, int cx, int cy, int s, bool frente, uint16_t c)
{
  const int dir = frente ? 1 : -1;
  d.fillTriangle(cx - dir * s, cy - s, cx - dir * s, cy + s, cx + dir * (s * 7 / 10), cy, c);
  d.fillRoundRect(frente ? cx + s * 7 / 10 : cx - s * 7 / 10 - 6, cy - s, 6, 2 * s, 2, c);
}
static void ic_aleatorio(lgfx::LovyanGFX& d, int cx, int cy, int s, uint16_t c)
{
  tab5_linha_grossa(d, cx - s, cy - s / 2, cx + s * 6 / 10, cy + s / 2, 2.2f, c);
  tab5_linha_grossa(d, cx - s, cy + s / 2, cx + s * 6 / 10, cy - s / 2, 2.2f, c);
  d.fillTriangle(cx + s, cy - s / 2, cx + s * 5 / 10, cy - s, cx + s * 5 / 10, cy, c);
  d.fillTriangle(cx + s, cy + s / 2, cx + s * 5 / 10, cy, cx + s * 5 / 10, cy + s, c);
}
static void ic_repetir(lgfx::LovyanGFX& d, int cx, int cy, int s, uint16_t c, uint16_t fundo, bool uma)
{
  d.drawRoundRect(cx - s, cy - s * 6 / 10, 2 * s, s * 12 / 10, s / 2, c);
  d.drawRoundRect(cx - s + 1, cy - s * 6 / 10 + 1, 2 * s - 2, s * 12 / 10 - 2, s / 2, c);
  d.drawRoundRect(cx - s + 2, cy - s * 6 / 10 + 2, 2 * s - 4, s * 12 / 10 - 4, s / 2, c);
  d.fillRect(cx - 4, cy - s * 6 / 10 - 2, 10, 7, fundo);   // o vao da seta
  d.fillTriangle(cx + 8, cy - s * 6 / 10 + 1, cx - 2, cy - s * 6 / 10 - 8, cx - 2, cy - s * 6 / 10 + 10, c);
  if (uma) {
    d.fillCircle(cx + s, cy - s * 6 / 10, 11, c);
    tab5_escrever(d, "1", cx + s, cy - s * 6 / 10 + 1, 18, true, fundo, c, middle_center);
  }
}
static void ic_coracao(lgfx::LovyanGFX& d, int cx, int cy, int s, uint16_t c, bool cheio, uint16_t fundo)
{
  const int r = s / 2;
  d.fillCircle(cx - r, cy - r / 2, r, c);
  d.fillCircle(cx + r, cy - r / 2, r, c);
  d.fillTriangle(cx - 2 * r + 1, cy - r / 4, cx + 2 * r - 1, cy - r / 4, cx, cy + s + 2, c);
  if (!cheio) {   // o contorno: o mesmo desenho, 3 px menor, na cor do fundo
    const int q = r - 3;
    d.fillCircle(cx - r, cy - r / 2, q, fundo);
    d.fillCircle(cx + r, cy - r / 2, q, fundo);
    d.fillTriangle(cx - 2 * r + 5, cy - r / 4 + 1, cx + 2 * r - 5, cy - r / 4 + 1, cx, cy + s - 3, fundo);
  }
}
static void ic_mais(lgfx::LovyanGFX& d, int cx, int cy, int s, uint16_t c)
{
  d.fillRoundRect(cx - s, cy - 2, 2 * s, 5, 2, c);
  d.fillRoundRect(cx - 2, cy - s, 5, 2 * s, 2, c);
}
static void ic_pasta(lgfx::LovyanGFX& d, int x, int cy, uint16_t c)
{
  d.fillRoundRect(x, cy - 16, 18, 9, 3, c);
  d.fillRoundRect(x, cy - 11, 40, 29, 4, c);
}
// Um pendrive de lado: o corpo e o conector.
static void ic_pendrive(lgfx::LovyanGFX& d, int cx, int cy, uint16_t c, uint16_t fundo)
{
  d.fillRoundRect(cx - 22, cy - 11, 30, 22, 5, c);
  d.fillRect(cx + 8, cy - 7, 14, 14, c);
  d.fillRect(cx + 12, cy - 4, 3, 3, fundo);
  d.fillRect(cx + 12, cy + 1, 3, 3, fundo);
}

static void ic_nota(lgfx::LovyanGFX& d, int cx, int cy, int s, uint16_t c)
{
  d.fillEllipse(cx - s / 3, cy + s / 2, s / 3 + 1, s / 4 + 1, c);
  d.fillRect(cx - s / 3 + s / 3 - 3, cy - s * 7 / 10, 5, s * 12 / 10, c);
  d.fillTriangle(cx - 1, cy - s * 7 / 10, cx + s / 2, cy - s * 3 / 10, cx - 1, cy - s / 5, c);
}
static void ic_lista(lgfx::LovyanGFX& d, int cx, int cy, int s, uint16_t c)
{
  for (int i = -1; i <= 1; ++i) { d.fillRoundRect(cx - s, cy + i * s * 6 / 10 - 2, 2 * s - (i == 1 ? s : 0), 5, 2, c); }
}
static void ic_volume(lgfx::LovyanGFX& d, int cx, int cy, int ondas, uint16_t c)
{
  d.fillRect(cx - 14, cy - 6, 8, 12, c);
  d.fillTriangle(cx - 8, cy, cx + 4, cy - 13, cx + 4, cy + 13, c);
  for (int k = 0; k < ondas; ++k) { d.drawArc(cx + 4, cy, 9 + k * 7, 7 + k * 7, -45, 45, c); }
}

// ---- estado ------------------------------------------------------------------
typedef enum { TELA_TOCANDO, TELA_BIBLIOTECA } tela_t;
typedef enum { ABA_PASTAS, ABA_PLAYLISTS } aba_t;
typedef enum { LIN_PASTA, LIN_MUSICA, LIN_PLAYLIST, LIN_NOVA, LIN_DISCO } lin_tipo_t;

typedef struct {
  uint8_t tipo;                           // lin_tipo_t
  int16_t n;                              // playlist: quantas musicas
  char    nome[TAB5_MUSICA_NOME_MAX];     // como no cartao
  char    caminho[TAB5_MUSICA_CAMINHO_MAX];   // musica e pasta: o caminho no servico (relativo ou "/usb/...")
} linha_t;

#define LIN_MAX (TAB5_MUSICA_LISTA_MAX + 2)

static tela_t   s_tela = TELA_BIBLIOTECA;
static aba_t    s_aba = ABA_PASTAS;
static char     s_raiz[160] = "";    // a pasta de musica (so para o nome)
static char     s_rel[160] = "";     // a pasta aberta, relativa a s_raiz ("" = a raiz); ou "/usb/..." (o pendrive)
static char     s_pl[TAB5_MUSICA_NOME_MAX] = "";   // a playlist aberta ("" = a lista delas)
static bool     s_forcar = true;
static lista_t  s_lista;

// As linhas: a interface desenha s_lin; o worker enche s_lin_tras e a
// interface troca as duas no aviso (kernel/trabalho.hpp, "o padrao do resultado").
static linha_t* s_lin = nullptr;
static linha_t* s_lin_tras = nullptr;
static int      s_n = 0;
static int      s_n_musicas = 0;
static bool     s_cheia = false;
static bool     s_sem_cartao = false;
static bool     s_carregando = false;
static uint32_t s_gen = 0;                       // muda a cada navegacao
static std::atomic<uint32_t> s_carga_pronta{0};  // a geracao que o worker terminou de carregar
static int      s_n_tras = 0, s_musicas_tras = 0;
// Linhas no topo que nao sao da pasta (o pendrive, na raiz): a musica da
// linha i e o item i - s_extra do que o servico lista.
static int      s_extra = 0, s_extra_tras = 0;
static bool     s_cheia_tras = false, s_sem_cartao_tras = false;
static int      s_tocando_linha = -1;

// Duracoes: o worker mede de 16 em 16 (cada pedaco e um trabalho curto).
static std::atomic<bool> s_dur_pedaco{false};
static int      s_dur_prox = 0;        // a proxima linha a medir
static uint32_t s_dur_gen = 0;

// A capa: um sprite (PSRAM) que o worker pinta; a interface so le com s_capa_ok.
static M5Canvas* s_capa = nullptr;
static bool      s_capa_ok = false;
static uint32_t  s_capa_ver = 0xFFFFFFFF;
static char      s_capa_arq[200] = "";
static std::atomic<int> s_capa_feita{0};   // 1 = pintou, -1 = nao abriu
static bool      s_capa_rodando = false;   // o worker esta pintando o sprite
static uint16_t  s_tinta = 0;              // o fundo da tela Tocando (da cor da capa)
static uint16_t  s_tinta_tras = 0;
static bool      s_ver_espectro = false;   // tocar na capa troca capa <-> espectro

// Favoritas (o coracao) da musica que toca.
static char     s_fav_caminho[TAB5_MUSICA_CAMINHO_MAX] = "";
static bool     s_fav = false;
static std::atomic<int> s_fav_tras{-1};    // -1 = nada novo; 0/1 = resposta do worker

// Avisos rapidos ("Adicionada a Rock").
static char     s_aviso[96] = "";
static uint32_t s_aviso_ate = 0;
static char     s_aviso_tras[96] = "";
static std::atomic<bool> s_aviso_novo{false};
static std::atomic<bool> s_recarregar{false};   // o worker mudou playlists: recarregar a lista

// A folha "Adicionar a playlist" e a pagina "Nova playlist".
static bool     s_folha = false;
static char     s_folha_caminho[TAB5_MUSICA_CAMINHO_MAX] = "";
static tab5_musica_item_t* s_folha_pl = nullptr;   // as playlists, na folha
static int      s_folha_n = 0;
static bool     s_nova = false;
static char     s_nova_txt[48] = "";
static char     s_nova_por[TAB5_MUSICA_CAMINHO_MAX] = "";   // musica a por na playlist criada
static tecvirt_t s_kb;
static uint32_t s_apagar_ate = 0;          // "Apagar" tocado: confirma ate esta hora

static bool     s_restante = false;        // o tempo da direita mostra quanto falta
static uint32_t s_sig = 0, s_prog_sig = 0, s_mini_sig = 0;

// A pasta 'rel' para mostrar: "" = a pasta de musica do cartao; "/usb" = o
// nome do disco; "/usb/Rock/Anos 80" = "Pendrive / Rock / Anos 80" (com
// 'caminho_todo') ou so "Anos 80".
static void pasta_para_mostrar(const char* rel, bool caminho_todo, char* out, size_t tam)
{
  const char* r = strrchr(s_raiz, '/');
  if (!rel[0]) { snprintf(out, tam, "%s", r ? r + 1 : "Music"); return; }
  if (rel[0] == '/') {
    const char* resto = strchr(rel + 1, '/');   // depois de "/usb"
    if (!resto) { snprintf(out, tam, "%s", tr(tab5_volume_tipo(rel))); return; }
    if (!caminho_todo) { snprintf(out, tam, "%s", strrchr(rel, '/') + 1); return; }
    snprintf(out, tam, "%s", tr(tab5_volume_tipo(rel)));
    for (const char* q = resto; *q; ) {   // "/Rock/Anos 80" -> " / Rock / Anos 80"
      const char* fim = strchr(q + 1, '/');
      const size_t k = strlen(out);
      snprintf(out + k, tam - k, " / %.*s", (int)(fim ? fim - q - 1 : strlen(q + 1)), q + 1);
      q = fim ? fim : q + strlen(q);
    }
    return;
  }
  if (!caminho_todo) { const char* b = strrchr(rel, '/'); snprintf(out, tam, "%s", b ? b + 1 : rel); return; }
  snprintf(out, tam, "%s / %s", r ? r + 1 : "Music", rel);
}

// ---- geometria -----------------------------------------------------------------
static const int M = 24;
// TOCANDO
static const int TC_CAB_A = 80;
static const int TC_CAPA_Y = 88;
// ix/iw: a coluna do texto e dos controles (relativa a area). Em pe e a
// largura toda, abaixo do quadrado. DEITADO (teclado fisico encaixado,
// kernel/orientacao) sao duas colunas: o quadrado (capa ou espectro) a
// esquerda, do tamanho da altura, e o resto a direita.
typedef struct { int capa_x, capa_y, capa_l, ix, iw, tit_y, art_y, sub_y, prog_y, ctrl_y, vol_y; } geo_tc_t;
static geo_tc_t geo_tc(const area_t& a)
{
  geo_tc_t g;
  g.capa_y = TC_CAPA_Y;
  if (a.largura > a.altura) {
    g.capa_l = a.altura - TC_CAPA_Y - M;
    if (g.capa_l > 560) { g.capa_l = 560; }
    g.capa_x = M;
    g.ix     = g.capa_x + g.capa_l + M;
    g.iw     = a.largura - g.ix;
    g.tit_y  = TC_CAPA_Y + 50;
    g.art_y  = g.tit_y + 48;
    g.sub_y  = g.art_y + 38;
    g.prog_y = g.sub_y + 56;
    g.ctrl_y = g.prog_y + 116;
    g.vol_y  = g.ctrl_y + 110;
    return g;
  }
  g.capa_l = a.largura - 2 * 80;
  if (g.capa_l > 560) { g.capa_l = 560; }
  g.capa_x = (a.largura - g.capa_l) / 2;
  g.ix     = 0;
  g.iw     = a.largura;
  g.tit_y  = TC_CAPA_Y + g.capa_l + 44;
  g.art_y  = g.tit_y + 48;
  g.sub_y  = g.art_y + 38;
  g.prog_y = g.sub_y + 56;
  g.ctrl_y = g.prog_y + 116;
  g.vol_y  = g.ctrl_y + 110;
  return g;
}
// BIBLIOTECA
static const int BB_TIT_A = 84;
static const int BB_ABAS_Y = 88, BB_ABAS_A = 60;
static const int BB_SUB_Y = 160, BB_SUB_A = 72;
static const int BB_BOT_A = 84;           // os botoes da playlist aberta
static const int LIN_A = 96;
static const int MINI_A = 116;
static int bb_lista_y(void) { return BB_SUB_Y + BB_SUB_A + (s_aba == ABA_PLAYLISTS && s_pl[0] ? BB_BOT_A : 0) + 4; }

// ---- carregar a lista (worker) ----------------------------------------------------
typedef struct { uint32_t gen; uint8_t aba; char rel[160]; char pl[TAB5_MUSICA_NOME_MAX]; } carga_t;
// Um anel: navegar rapido enfileira cargas, e cada uma le o SEU argumento
// (a geracao dela diz se o resultado ainda serve).
static carga_t s_carga_arg[4];
static int     s_carga_vez = 0;

static void trab_carregar(void* arg)
{
  const carga_t* c = (const carga_t*)arg;
  linha_t* out = s_lin_tras;
  int n = 0, musicas = 0;
  bool cheia = false, sem = false;
  int extra = 0;
  if (c->aba == ABA_PASTAS) {
    // NA RAIZ, OS OUTROS DISCOS PRIMEIRO: o pendrive do USB-A, se houver.
    if (!c->rel[0]) {
      tab5_vol_t v[TAB5_VOL_N];
      const int nv = tab5_volumes_listar(v, TAB5_VOL_N);
      for (int i = 0; i < nv && n < LIN_MAX; ++i) {
        if (v[i].id == TAB5_VOL_CARTAO) { continue; }
        linha_t& l = out[n++];
        l.tipo = LIN_DISCO;
        l.n = 0;
        snprintf(l.nome, sizeof(l.nome), "%s", v[i].nome[0] ? v[i].nome : v[i].tipo);
        snprintf(l.caminho, sizeof(l.caminho), "%s", v[i].raiz);
        ++extra;
      }
    }
    tab5_musica_item_t* it = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * TAB5_MUSICA_LISTA_MAX, MALLOC_CAP_SPIRAM);
    if (it) {
      const int k = tab5_musica_listar(c->rel, it, TAB5_MUSICA_LISTA_MAX, &cheia);
      sem = k < 0 && !extra;   // sem cartao, mas com pendrive: a lista mostra o pendrive
      for (int i = 0; i < k && n < LIN_MAX; ++i) {
        linha_t& l = out[n++];
        l.tipo = it[i].pasta ? LIN_PASTA : LIN_MUSICA;
        l.n = 0;
        snprintf(l.nome, sizeof(l.nome), "%s", it[i].nome);
        if (c->rel[0]) { snprintf(l.caminho, sizeof(l.caminho), "%s/%s", c->rel, it[i].nome); }
        else           { snprintf(l.caminho, sizeof(l.caminho), "%s", it[i].nome); }
        if (!it[i].pasta) { ++musicas; }
      }
      heap_caps_free(it);
    }
  } else if (!c->pl[0]) {
    tab5_musica_item_t* it = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * 64, MALLOC_CAP_SPIRAM);
    tab5_musica_faixa_t* fx = (tab5_musica_faixa_t*)heap_caps_malloc(sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
    if (it && fx) {
      out[n].tipo = LIN_NOVA; out[n].n = 0; out[n].nome[0] = out[n].caminho[0] = '\0'; ++n;
      const int k = tab5_musica_playlists(it, 64);
      sem = k < 0;
      for (int i = 0; i < k && n < LIN_MAX; ++i) {
        if (sistema_trabalho_cancelado()) { break; }
        linha_t& l = out[n++];
        l.tipo = LIN_PLAYLIST;
        snprintf(l.nome, sizeof(l.nome), "%s", it[i].nome);
        l.caminho[0] = '\0';
        const int q = tab5_musica_playlist_itens(it[i].nome, fx, TAB5_MUSICA_FILA_MAX);
        l.n = (int16_t)(q < 0 ? 0 : q);
      }
    }
    heap_caps_free(it);
    heap_caps_free(fx);
  } else {
    tab5_musica_faixa_t* fx = (tab5_musica_faixa_t*)heap_caps_malloc(sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
    if (fx) {
      const int k = tab5_musica_playlist_itens(c->pl, fx, TAB5_MUSICA_FILA_MAX);
      sem = k < 0;
      for (int i = 0; i < k && n < LIN_MAX; ++i) {
        linha_t& l = out[n++];
        l.tipo = LIN_MUSICA;
        l.n = 0;
        const char* b = strrchr(fx[i].caminho, '/');
        snprintf(l.nome, sizeof(l.nome), "%s", b ? b + 1 : fx[i].caminho);
        snprintf(l.caminho, sizeof(l.caminho), "%s", fx[i].caminho);
        ++musicas;
      }
      heap_caps_free(fx);
    }
  }
  s_n_tras = n;
  s_extra_tras = extra;
  s_musicas_tras = musicas;
  s_cheia_tras = cheia;
  s_sem_cartao_tras = sem;
  s_carga_pronta = c->gen;
}

static void carregar(void)
{
  ++s_gen;
  carga_t* c = &s_carga_arg[s_carga_vez];
  s_carga_vez = (s_carga_vez + 1) % 4;
  c->gen = s_gen;
  c->aba = (uint8_t)s_aba;
  snprintf(c->rel, sizeof(c->rel), "%s", s_rel);
  snprintf(c->pl, sizeof(c->pl), "%s", s_pl);
  s_carregando = true;
  if (!sistema_trabalho(trab_carregar, c)) { s_carregando = false; }
}

// A navegacao mudou (pasta, aba, playlist): a lista atual some ja.
static void navegar(void)
{
  s_n = 0;
  s_tocando_linha = -1;
  lista_itens_mudaram(&s_lista, 0);
  lista_ao_inicio(&s_lista);
  carregar();
  s_forcar = true;
  sistema_pedir_redesenho_completo();
}

// ---- duracoes (worker, em pedacos) -------------------------------------------------
typedef struct { uint32_t gen; int n; char caminho[16][TAB5_MUSICA_CAMINHO_MAX]; } dur_arg_t;
static dur_arg_t* s_dur_arg = nullptr;   // PSRAM; so o worker le enquanto o pedaco roda
static bool       s_dur_rodando = false;  // ha um pedaco no worker: nao mexer em s_dur_arg

static void trab_duracoes(void* arg)
{
  const dur_arg_t* p = (const dur_arg_t*)arg;
  for (int i = 0; i < p->n && !sistema_trabalho_cancelado(); ++i) { tab5_musica_duracao(p->caminho[i]); }
  s_dur_pedaco = true;
}

// Pede o proximo pedaco de linhas sem duracao lembrada. Na interface.
static void duracoes_seguir(void)
{
  if (s_dur_rodando) { return; }   // o pedaco atual, ao terminar, chama de novo
  if (s_dur_gen != s_gen) { s_dur_gen = s_gen; s_dur_prox = 0; }
  if (!s_dur_arg) { s_dur_arg = (dur_arg_t*)heap_caps_malloc(sizeof(dur_arg_t), MALLOC_CAP_SPIRAM); }
  if (!s_dur_arg) { return; }
  int k = 0;
  uint32_t ms;
  while (s_dur_prox < s_n && k < 16) {
    const linha_t& l = s_lin[s_dur_prox++];
    if (l.tipo != LIN_MUSICA || tab5_musica_duracao_lembrada(l.caminho, &ms)) { continue; }
    snprintf(s_dur_arg->caminho[k++], TAB5_MUSICA_CAMINHO_MAX, "%s", l.caminho);
  }
  if (!k) { return; }
  s_dur_arg->n = k;
  s_dur_arg->gen = s_gen;
  s_dur_rodando = sistema_trabalho(trab_duracoes, s_dur_arg);
}

// ---- capa (worker) ----------------------------------------------------------------
typedef struct { char arq[200]; } capa_arg_t;
static capa_arg_t s_capa_arg;

static void trab_capa(void* arg)
{
  const capa_arg_t* c = (const capa_arg_t*)arg;
  int r = -1;
  // A leitura e do servico da musica: o app so recebe a imagem inteira na PSRAM.
  size_t n = 0;
  uint8_t* b = s_capa ? tab5_musica_capa_ler(c->arq, &n) : nullptr;
  if (b) {
    const int L = s_capa->width();
    s_capa->fillScreen(TFT_BLACK);
    const bool png = n > 8 && b[0] == 0x89 && b[1] == 'P';
    // escala 0 = "caber": a imagem inteira no quadrado, centralizada.
    const bool ok = png ? s_capa->drawPng(b, n, 0, 0, L, L, 0, 0, 0.0f, 0.0f, middle_center)
                        : s_capa->drawJpg(b, n, 0, 0, L, L, 0, 0, 0.0f, 0.0f, middle_center);
    if (ok) {
      // A COR DO FUNDO sai da capa: a media de uma grade de pontos,
      // escurecida - a tela inteira fica "da cor do disco".
      uint32_t sr = 0, sg = 0, sb = 0, k = 0;
      for (int y = 8; y < L; y += 24) {
        for (int x = 8; x < L; x += 24) {
          const uint16_t p = s_capa->readPixel(x, y);
          sr += (p >> 11) & 31; sg += (p >> 5) & 63; sb += p & 31; ++k;
        }
      }
      if (k) {
        // Mais saturada que a media (a media de uma capa tende ao cinza), e escura.
        float r5 = sr / (float)k / 31.0f, g6 = sg / (float)k / 63.0f, b5 = sb / (float)k / 31.0f;
        const float m = (r5 + g6 + b5) / 3.0f;
        r5 = m + (r5 - m) * 1.8f; g6 = m + (g6 - m) * 1.8f; b5 = m + (b5 - m) * 1.8f;
        auto lim = [](float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
        const float esc = 0.32f;
        s_tinta_tras = (uint16_t)(((int)(lim(r5) * esc * 31) << 11) | ((int)(lim(g6) * esc * 63) << 5) | (int)(lim(b5) * esc * 31));
      }
      r = 1;
    }
  }
  heap_caps_free(b);
  s_capa_feita = r;
}

// ---- favoritas e playlists (worker) -------------------------------------------------
typedef struct { char caminho[TAB5_MUSICA_CAMINHO_MAX]; char pl[TAB5_MUSICA_NOME_MAX]; int op; } pl_arg_t;
enum { OP_FAV_TEM, OP_FAV_TROCAR, OP_POR, OP_TIRAR, OP_CRIAR, OP_APAGAR };
static pl_arg_t s_pl_arg[4];   // um anel: cada pedido tem o seu (o worker pode estar com o anterior)
static int      s_pl_vez = 0;

static void aviso_tras(const char* t) { snprintf(s_aviso_tras, sizeof(s_aviso_tras), "%s", t); s_aviso_novo = true; }

static void trab_pl(void* arg)
{
  const pl_arg_t* p = (const pl_arg_t*)arg;
  char nome[TAB5_MUSICA_NOME_MAX * 2], t[96];
  switch (p->op) {
    case OP_FAV_TEM:
      s_fav_tras = tab5_musica_playlist_tem(TAB5_MUSICA_FAVORITAS, p->caminho) ? 1 : 0;
      break;
    case OP_FAV_TROCAR: {
      const bool tem = tab5_musica_playlist_tem(TAB5_MUSICA_FAVORITAS, p->caminho);
      if (tem) { tab5_musica_playlist_tirar(TAB5_MUSICA_FAVORITAS, p->caminho); }
      else     { tab5_musica_playlist_por(TAB5_MUSICA_FAVORITAS, p->caminho); }
      s_fav_tras = tem ? 0 : 1;
      aviso_tras(tem ? tr("Tirada das Favoritas") : tr("Nas Favoritas"));
      s_recarregar = true;
      break;
    }
    case OP_POR:
      tab5_musica_nome_utf8(p->pl, nome, sizeof(nome));
      snprintf(t, sizeof(t), tab5_musica_playlist_por(p->pl, p->caminho) ? tr("Adicionada a %s") : tr("Já estava em %s"), nome);
      aviso_tras(t);
      if (!strcmp(p->pl, TAB5_MUSICA_FAVORITAS)) { s_fav_tras = -2; }   // o coracao pode ter mudado
      s_recarregar = true;
      break;
    case OP_TIRAR:
      tab5_musica_playlist_tirar(p->pl, p->caminho);
      s_recarregar = true;
      break;
    case OP_CRIAR: {
      char nc[TAB5_MUSICA_NOME_MAX];
      if (tab5_musica_playlist_criar(p->pl, nc, sizeof(nc))) {
        if (p->caminho[0]) { tab5_musica_playlist_por(nc, p->caminho); }
        snprintf(t, sizeof(t), tr("Playlist %s criada"), p->pl);
      } else {
        snprintf(t, sizeof(t), tr("Não deu para criar \"%s\""), p->pl);
      }
      aviso_tras(t);
      s_recarregar = true;
      break;
    }
    case OP_APAGAR:
      tab5_musica_playlist_apagar(p->pl);
      s_recarregar = true;
      break;
  }
}

static void pedir_pl(int op, const char* pl, const char* caminho)
{
  pl_arg_t* p = &s_pl_arg[s_pl_vez];
  s_pl_vez = (s_pl_vez + 1) % 4;
  p->op = op;
  snprintf(p->pl, sizeof(p->pl), "%s", pl ? pl : "");
  snprintf(p->caminho, sizeof(p->caminho), "%s", caminho ? caminho : "");
  sistema_trabalho(trab_pl, p);
}

// O worker terminou alguma coisa: pega os resultados (na interface).
static void ao_aviso(uint32_t av)
{
  if (av & AVISO_VOLUMES) {
    // UM PENDRIVE ENTROU OU SAIU. Dentro dele e ele saiu: volta a raiz. Na
    // raiz: recarrega (a linha dele aparece ou some).
    if (s_rel[0] == '/' && !tab5_volume_disponivel(s_rel)) { s_rel[0] = '\0'; }
    if (s_aba == ABA_PASTAS && (!s_rel[0] || s_rel[0] == '/')) { navegar(); }
  }
  if (!(av & AVISO_TRABALHO)) { return; }
  if (s_carregando && s_carga_pronta.load() == s_gen) {
    s_carregando = false;
    linha_t* t = s_lin; s_lin = s_lin_tras; s_lin_tras = t;
    s_n = s_n_tras;
    s_extra = s_extra_tras;
    s_n_musicas = s_musicas_tras;
    s_cheia = s_cheia_tras;
    s_sem_cartao = s_sem_cartao_tras;
    s_tocando_linha = -1;
    lista_itens_mudaram(&s_lista, s_n);
    s_forcar = true;
    s_dur_prox = 0;
    duracoes_seguir();
  }
  if (s_dur_pedaco.exchange(false)) {
    s_dur_rodando = false;
    if (!s_carregando) {
      if (s_dur_gen == s_gen) { lista_itens_mudaram(&s_lista, s_n); }
      duracoes_seguir();   // outra geracao: recomeca do zero da lista nova
    }
  }
  const int cf = s_capa_feita.exchange(0);
  if (cf) {
    s_capa_rodando = false;
    s_capa_ok = cf > 0;
    s_tinta = s_capa_ok ? s_tinta_tras : 0;
    s_forcar = true;
    if (s_tela == TELA_TOCANDO) { sistema_pedir_redesenho_completo(); }
  }
  const int fv = s_fav_tras.exchange(-1);
  if (fv == -2) { s_fav_caminho[0] = '\0'; }   // reconsultar
  else if (fv >= 0) { s_fav = fv; s_forcar = true; }
  if (s_aviso_novo.exchange(false)) {
    snprintf(s_aviso, sizeof(s_aviso), "%s", s_aviso_tras);
    s_aviso_ate = tab5_agora_ms() + 2200;
    s_forcar = true;
  }
  if (s_recarregar.exchange(false) && s_aba == ABA_PLAYLISTS && s_tela == TELA_BIBLIOTECA) { carregar(); }
  sistema_pedir_redesenho();
}

// ---- espectro -----------------------------------------------------------------------
// Barras em SEGMENTOS (os "LEDs" dos tocadores antigos), com um pico que fica
// um instante e cai devagar, DENTRO DO QUADRADO DA CAPA. Desenha so o que
// mudou: segmento que acendeu, segmento que apagou, o pico que andou.
static const int SEG_A = 12, SEG_V = 4, SEG_P = SEG_A + SEG_V;
static const int VIS_MAX_LINHAS = 36;
static int      s_vis_linhas = 30;
static uint8_t  s_vis_seg[TAB5_MUSICA_FAIXAS];
static int8_t   s_vis_pico_tela[TAB5_MUSICA_FAIXAS];
static float    s_vis_nivel[TAB5_MUSICA_FAIXAS];
static float    s_vis_pico[TAB5_MUSICA_FAIXAS];
static uint8_t  s_vis_pico_seg[TAB5_MUSICA_FAIXAS];
static bool     s_vis_vivo = false;

typedef struct { const char* nome; } tema_t;
static const tema_t TEMAS[] = { { N_("Arco-íris") }, { N_("Lilás") }, { N_("Fogo") }, { N_("Oceano") }, { N_("Neon") } };
static const int N_TEMAS = sizeof(TEMAS) / sizeof(TEMAS[0]);
static uint16_t s_vis_cor[TAB5_MUSICA_FAIXAS][VIS_MAX_LINHAS];
static uint16_t s_vis_cor_pico[TAB5_MUSICA_FAIXAS];
static uint32_t s_tema_nome_ate = 0;
static bool     s_vis_cores_ok = false;

static uint16_t hsv(lgfx::LovyanGFX& d, float h, float sat, float v)
{
  h = fmodf(h, 360.0f) / 60.0f;
  const int i = (int)h;
  const float f = h - i, p = v * (1 - sat), q = v * (1 - sat * f), t = v * (1 - sat * (1 - f));
  float r, g, b;
  switch (i) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return d.color565((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

static uint16_t degrade(lgfx::LovyanGFX& d, float t, const uint8_t a[3], const uint8_t b[3], const uint8_t c[3], float meio)
{
  const uint8_t* x; const uint8_t* y; float u;
  if (t < meio) { x = a; y = b; u = t / meio; } else { x = b; y = c; u = (t - meio) / (1 - meio); }
  return d.color565((uint8_t)(x[0] + (y[0] - x[0]) * u), (uint8_t)(x[1] + (y[1] - x[1]) * u),
                    (uint8_t)(x[2] + (y[2] - x[2]) * u));
}

static void vis_cores(lgfx::LovyanGFX& d)
{
  const int tema = ajustes_tema_espectro() % N_TEMAS;
  static const uint8_t LIL[3][3] = { {91,140,255}, {150,190,255}, {235,242,255} };
  static const uint8_t FOGO[3][3] = { {40,210,80}, {250,220,40}, {255,50,40} };
  static const uint8_t MAR[3][3]  = { {10,60,160}, {0,200,235}, {230,250,255} };
  static const uint8_t ROSA[3][3] = { {150,20,120}, {255,60,200}, {255,220,250} };
  static const uint8_t CIANO[3][3] = { {0,90,140}, {40,230,255}, {220,255,255} };
  for (int i = 0; i < TAB5_MUSICA_FAIXAS; ++i) {
    const float fi = (float)i / (TAB5_MUSICA_FAIXAS - 1);
    for (int k = 0; k < s_vis_linhas; ++k) {
      const float t = (float)k / (s_vis_linhas - 1);
      uint16_t c;
      switch (tema) {
        case 0:  c = hsv(d, fi * 290.0f, 0.85f - 0.35f * t * t, 0.65f + 0.35f * t); break;   // vermelho -> violeta
        case 1:  c = degrade(d, t, LIL[0], LIL[1], LIL[2], 0.7f); break;
        case 2:  c = degrade(d, t, FOGO[0], FOGO[1], FOGO[2], 0.6f); break;
        case 3:  c = degrade(d, t, MAR[0], MAR[1], MAR[2], 0.65f); break;
        default: c = (i & 1) ? degrade(d, t, CIANO[0], CIANO[1], CIANO[2], 0.7f)
                             : degrade(d, t, ROSA[0], ROSA[1], ROSA[2], 0.7f); break;
      }
      s_vis_cor[i][k] = c;
    }
    s_vis_cor_pico[i] = (tema == 0) ? hsv(d, fi * 290.0f, 0.35f, 1.0f) : (uint16_t)TFT_WHITE;
  }
  s_vis_cores_ok = true;
}

static uint16_t fundo_tc(void) { return s_tinta ? s_tinta : (uint16_t)TEMA_FUNDO; }   // sem capa: o fundo do tema
static uint16_t fundo_vis(void) { return TEMA_SUPERFICIE; }

static void vis_x(const area_t& a, const geo_tc_t& g, int i, int* x, int* l)
{
  const int w = g.capa_l - 40;
  const int passo = w / TAB5_MUSICA_FAIXAS;
  *l = passo - 5;
  *x = a.x + g.capa_x + 20 + i * passo + (w - passo * TAB5_MUSICA_FAIXAS) / 2;
}
static int vis_y_linha(const area_t& a, const geo_tc_t& g, int linha)
{
  return a.y + g.capa_y + g.capa_l - 20 - (linha + 1) * SEG_P + SEG_V;
}

static void vis_zerar_tela(void)
{
  memset(s_vis_seg, 0, sizeof(s_vis_seg));
  memset(s_vis_pico_tela, -1, sizeof(s_vis_pico_tela));
}

static void desenhar_espectro(lgfx::LovyanGFX& d, const area_t& a, const geo_tc_t& g)
{
  uint8_t f[TAB5_MUSICA_FAIXAS];
  if (!tab5_musica_espectro(f)) { memset(f, 0, sizeof(f)); }
  const uint16_t fundo = fundo_vis();
  bool vivo = false;
  for (int i = 0; i < TAB5_MUSICA_FAIXAS; ++i) {
    // Sobe na hora, desce suave (~1 linha por quadro): a batida "pula".
    const float alvo = f[i] * s_vis_linhas / 255.0f;
    float& n = s_vis_nivel[i];
    n = alvo > n ? alvo : (n - 1.3f > alvo ? n - 1.3f : alvo);
    if (n > s_vis_pico[i]) { s_vis_pico[i] = n; s_vis_pico_seg[i] = 12; }
    else if (s_vis_pico_seg[i]) { --s_vis_pico_seg[i]; }
    else { s_vis_pico[i] = s_vis_pico[i] > 0.4f ? s_vis_pico[i] - 0.4f : 0.0f; }

    int x, l;
    vis_x(a, g, i, &x, &l);
    int novo = (int)(n + 0.5f);
    if (novo > s_vis_linhas) { novo = s_vis_linhas; }
    int pico = (int)(s_vis_pico[i] + 0.5f) - 1;
    if (pico < novo || pico < 1) { pico = -1; }
    const int pt = s_vis_pico_tela[i];
    if (pt >= 0 && pt != pico && pt >= novo) { d.fillRect(x, vis_y_linha(a, g, pt), l, SEG_A, fundo); }
    for (int k = s_vis_seg[i]; k < novo; ++k) { d.fillRect(x, vis_y_linha(a, g, k), l, SEG_A, s_vis_cor[i][k]); }
    for (int k = novo; k < s_vis_seg[i]; ++k) {
      if (k != pico) { d.fillRect(x, vis_y_linha(a, g, k), l, SEG_A, fundo); }
    }
    s_vis_seg[i] = (uint8_t)novo;
    if (pico >= 0) {
      d.fillRect(x, vis_y_linha(a, g, pico), l, SEG_A - 4, fundo);
      d.fillRect(x, vis_y_linha(a, g, pico) + SEG_A - 4, l, 4, s_vis_cor_pico[i]);
    }
    s_vis_pico_tela[i] = (int8_t)pico;
    if (novo > 0 || pico >= 0) { vivo = true; }
  }
  s_vis_vivo = vivo;
}

static void trocar_tema(void)
{
  ajustes_definir_tema_espectro((uint8_t)((ajustes_tema_espectro() + 1) % N_TEMAS));
  s_tema_nome_ate = tab5_agora_ms() + 1500;
  s_vis_cores_ok = false;
  // O destaque muda junto: tela, lista, barra e miniplayer - redesenho completo.
  tab5_musica_definir_cor(destaque());
  s_forcar = true;
  lista_itens_mudaram(&s_lista, s_n);
  sistema_pedir_redesenho_completo();
}

// ---- a capa no quadrado ---------------------------------------------------------------
// Cantos redondos sem mascara: depois de pintar o quadrado, os quatro cantos
// recebem a cor do fundo por fora do arco.
static void cantos(lgfx::LovyanGFX& d, int x, int y, int l, int r, uint16_t fundo)
{
  for (int i = 0; i < r; ++i) {
    const float dy = r - i - 0.5f;
    const int fora = r - (int)(sqrtf((float)(r * r) - dy * dy) + 0.5f);
    if (fora <= 0) { continue; }
    d.drawFastHLine(x, y + i, fora, fundo);
    d.drawFastHLine(x + l - fora, y + i, fora, fundo);
    d.drawFastHLine(x, y + l - 1 - i, fora, fundo);
    d.drawFastHLine(x + l - fora, y + l - 1 - i, fora, fundo);
  }
}

static bool mostra_espectro(void) { return s_ver_espectro || !s_capa_ok; }

static void desenhar_quadrado(lgfx::LovyanGFX& d, const area_t& a, const geo_tc_t& g)
{
  const int x = a.x + g.capa_x, y = a.y + g.capa_y, l = g.capa_l;
  if (!mostra_espectro() && s_capa) {
    // O sprite nasce do tamanho do quadrado de quando abriu; girou, escala.
    if (s_capa->width() == l) { s_capa->pushSprite(&d, x, y); }
    else {
      const float z = (float)l / s_capa->width();
      s_capa->pushRotateZoomWithAA(&d, x + l / 2.0f, y + l / 2.0f, 0.0f, z, z);
    }
    cantos(d, x, y, l, TEMA_RAIO_G, fundo_tc());
    return;
  }
  d.fillRoundRect(x, y, l, l, TEMA_RAIO_G, fundo_vis());
  vis_zerar_tela();
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  if (!e.ativo || !e.tocando) {
    // Parado: uma nota grande no meio (o espectro volta quando tocar).
    ic_nota(d, x + l / 2, y + l / 2 - 10, l / 4, TEMA_TEXTO_3);
  }
}

// ---- TOCANDO -----------------------------------------------------------------------------
static void desenhar_progresso(lgfx::LovyanGFX& d, const area_t& a, const geo_tc_t& g, const tab5_musica_estado_t& e)
{
  const uint16_t bg = fundo_tc();
  const int lx = a.x + g.ix + M + 16, lw = g.iw - 2 * (M + 16), ly = a.y + g.prog_y;
  d.fillRect(lx - 14, ly - 14, lw + 28, 70, bg);
  const uint32_t dur = e.dur_ms, pos = (dur && e.pos_ms > dur) ? dur : e.pos_ms;
  const int feito = (e.ativo && dur) ? (int)((uint64_t)lw * pos / dur) : 0;
  d.fillRoundRect(lx, ly - 3, lw, 6, 3, TEMA_SUPERFICIE_ALTA);   // trilho
  if (feito > 0) { d.fillRoundRect(lx, ly - 3, feito, 6, 3, TEMA_TEXTO); }
  if (e.ativo && dur) { d.fillCircle(lx + feito, ly, 10, TEMA_TEXTO); }
  char tp[16], td[20];
  tempo_txt(tp, sizeof(tp), e.ativo ? pos : 0);
  if (e.ativo && dur) {
    if (s_restante) { char r[16]; tempo_txt(r, sizeof(r), dur - pos); snprintf(td, sizeof(td), "-%s", r); }
    else            { tempo_txt(td, sizeof(td), dur); }
  } else {
    snprintf(td, sizeof(td), "--:--");
  }
  tab5_escrever(d, tp, lx, ly + 18, 24, false, TEMA_TEXTO_2, bg, top_left);
  tab5_escrever(d, td, lx + lw, ly + 18, 24, false, TEMA_TEXTO_2, bg, top_right);
}

static void desenhar_tocando(lgfx::LovyanGFX& d, const area_t& a, const tab5_musica_estado_t& e, bool tudo)
{
  const geo_tc_t g = geo_tc(a);
  const uint16_t bg = fundo_tc();
  const uint16_t ac = destaque();
  const int WA = a.largura;              // o cabecalho: a largura toda
  const int W = g.iw, X = a.x + g.ix;   // o texto e os controles: a coluna deles
  if (tudo) {
    d.fillRect(a.x, a.y, a.largura, a.altura, bg);
    // Cabecalho: voltar (chevron), de onde toca, e o tema.
    const int cy = a.y + TC_CAB_A / 2;
    tab5_linha_grossa(d, a.x + 34, cy - 8, a.x + 48, cy + 6, 3.0f, TEMA_TEXTO);
    tab5_linha_grossa(d, a.x + 48, cy + 6, a.x + 62, cy - 8, 3.0f, TEMA_TEXTO);
    char de[64], nome[200];
    if (e.ativo && e.playlist[0]) {
      snprintf(de, sizeof(de), tr("TOCANDO DA PLAYLIST"));
      tab5_musica_nome_utf8(e.playlist, nome, sizeof(nome));
    } else {
      snprintf(de, sizeof(de), !e.ativo ? tr("NADA TOCANDO") : e.rel[0] == '/' ? tr("TOCANDO DO PENDRIVE") : tr("TOCANDO DA PASTA"));
      if (e.ativo) { pasta_para_mostrar(e.rel, false, nome, sizeof(nome)); }
      else         { nome[0] = '\0'; }
    }
    tab5_escrever(d, de, a.x + WA / 2, cy - 14, 18, false, TEMA_TEXTO_2, bg, middle_center);
    tab5_escrever(d, nome, a.x + WA / 2, cy + 14, 24, true, TEMA_TEXTO, bg, middle_center, WA - 220);
    // Tema: tres pontos coloridos.
    const int tx = a.x + WA - 52;
    d.fillCircle(tx - 12, cy + 6, 7, 0xFBCB);
    d.fillCircle(tx + 12, cy + 6, 7, 0x2E5E);
    d.fillCircle(tx, cy - 9, 7, ac);

    desenhar_quadrado(d, a, g);

    // Titulo, artista e a linha de baixo; o coracao a direita.
    char t[128];
    snprintf(t, sizeof(t), "%s", e.ativo && e.titulo[0] ? e.titulo : tr("Escolha uma música"));
    tab5_escrever(d, t, X + M + 16, a.y + g.tit_y, TEMA_LETRA_TITULO, true, TEMA_TEXTO, bg, middle_left, W - 2 * M - 120);
    if (e.ativo && e.artista[0]) { snprintf(t, sizeof(t), "%s", e.artista); }
    else if (e.ativo)            { snprintf(t, sizeof(t), tr("Artista desconhecido")); }
    else                         { snprintf(t, sizeof(t), tr("na Biblioteca")); }
    tab5_escrever(d, t, X + M + 16, a.y + g.art_y, 30, false, ac, bg, middle_left, W - 2 * M - 120);
    if (e.erro[0]) {
      tab5_escrever(d, e.erro, X + M + 16, a.y + g.sub_y, 22, false, TEMA_ALERTA, bg, middle_left, W - 2 * M - 32);
    } else if (e.ativo) {
      char info[24];
      if (e.kbps) { snprintf(info, sizeof(info), "%u kbps", (unsigned)e.kbps); }
      else        { snprintf(info, sizeof(info), "%.1f kHz", e.hz / 1000.0); }
      snprintf(t, sizeof(t), tr("%s%s%d de %d  ·  %s%s"), e.album, e.album[0] ? "  ·  " : "",
               e.idx + 1, e.n, info, e.tocando ? "" : tr("  ·  pausado"));
      tab5_escrever(d, t, X + M + 16, a.y + g.sub_y, 22, false, TEMA_TEXTO_2, bg, middle_left, W - 2 * M - 32);
    }
    if (e.ativo) { ic_coracao(d, X + W - M - 44, a.y + g.tit_y + 14, 22, s_fav ? ac : TEMA_TEXTO, s_fav, bg); }

    desenhar_progresso(d, a, g, e);

    // Aleatorio, anterior, tocar/pausar, proxima, repetir.
    const int by = a.y + g.ctrl_y, cx = X + W / 2;
    const uint16_t cb = e.ativo ? (uint16_t)TEMA_TEXTO : (uint16_t)TEMA_TEXTO_3;
    ic_aleatorio(d, cx - 270, by, 20, e.aleatorio ? ac : cb);
    if (e.aleatorio) { d.fillCircle(cx - 270, by + 34, 4, ac); }
    // Anterior/proxima em circulo de superficie; tocar/pausar em circulo de destaque.
    d.fillCircle(cx - 140, by, 40, TEMA_SUPERFICIE);
    ic_pular(d, cx - 140, by, 20, false, cb);
    d.fillCircle(cx, by, 58, TEMA_DESTAQUE);
    if (e.ativo && e.tocando) { ic_pausa(d, cx, by, 44, TEMA_SOBRE_DESTAQUE); } else { ic_tocar(d, cx + 4, by, 50, TEMA_SOBRE_DESTAQUE); }
    d.fillCircle(cx + 140, by, 40, TEMA_SUPERFICIE);
    ic_pular(d, cx + 140, by, 20, true, cb);
    ic_repetir(d, cx + 270, by, 22, e.repetir ? ac : cb, bg, e.repetir == TAB5_MUSICA_REPETIR_UMA);
    if (e.repetir) { d.fillCircle(cx + 270, by + 34, 4, ac); }

    // Por numa playlist, volume, parar.
    const int vy = a.y + g.vol_y;
    d.drawCircle(X + M + 30, vy, 26, TEMA_LINHA);
    ic_mais(d, X + M + 30, vy, 12, cb);
    const int vx0 = X + M + 112, vx1 = X + W - M - 112;
    ic_volume(d, vx0 - 30, vy, 0, TEMA_TEXTO_2);
    ic_volume(d, vx1 + 26, vy, 2, TEMA_TEXTO_2);
    const int vol = ajustes_volume();
    const int vf = (vx1 - vx0) * vol / AJUSTES_VOLUME_MAX;
    d.fillRoundRect(vx0, vy - 3, vx1 - vx0, 6, 3, TEMA_SUPERFICIE_ALTA);
    if (vf > 0) { d.fillRoundRect(vx0, vy - 3, vf, 6, 3, TEMA_TEXTO_2); }
    d.fillCircle(vx0 + vf, vy, 8, TEMA_TEXTO);
    d.fillRoundRect(X + W - M - 46, vy - 16, 32, 32, 6, cb);
  }
  if (mostra_espectro() && (e.ativo || s_vis_vivo)) { desenhar_espectro(d, a, g); }
  // O nome do tema, por 1,5 s depois de trocar - dentro do quadrado.
  if (s_tema_nome_ate) {
    const int x = a.x + g.capa_x, y = a.y + g.capa_y;
    if ((int32_t)(s_tema_nome_ate - tab5_agora_ms()) > 0) {
      tab5_escrever(d, tr(TEMAS[ajustes_tema_espectro() % N_TEMAS].nome), x + g.capa_l - 20, y + 20, 26, true, TEMA_TEXTO,
            mostra_espectro() ? fundo_vis() : TEMA_FUNDO, top_right);
    } else {
      s_tema_nome_ate = 0;
      desenhar_quadrado(d, a, g);
    }
  }
}

// ---- BIBLIOTECA ------------------------------------------------------------------------------
static int linha_altura(int, void*) { return LIN_A; }

static void barras_tocando(lgfx::LovyanGFX& d, int cx, int cy, uint16_t c)
{
  // Tres barrinhas que mexem com o relogio (a linha que toca).
  const uint32_t t = tab5_agora_ms() / 140;
  for (int k = 0; k < 3; ++k) {
    const int h = 8 + (int)((t * (k + 2) + k * 5) % 16);
    d.fillRect(cx - 14 + k * 11, cy + 14 - h, 7, h, c);
  }
}

static void linha_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= s_n) { return; }
  const linha_t& l = s_lin[i];
  const bool toca = (i == s_tocando_linha);
  const uint16_t ac = destaque();
  const int cy = y + LIN_A / 2;
  d.fillRect(x0, y, largura, LIN_A, TEMA_FUNDO);
  // O "quadradinho" da esquerda.
  const int qx = x0 + M, q = 64;
  char t[TAB5_MUSICA_NOME_MAX * 2], sub[TAB5_MUSICA_NOME_MAX * 2];
  sub[0] = '\0';
  int dir = largura - M;   // ate onde o texto vai
  switch (l.tipo) {
    case LIN_NOVA:
      d.fillRoundRect(qx, cy - q / 2, q, q, TEMA_RAIO_M, TEMA_SUPERFICIE_ALTA);
      ic_mais(d, qx + q / 2, cy, 16, ac);
      tab5_escrever(d, tr("Nova playlist"), qx + q + 20, cy, 30, true, ac, TEMA_FUNDO, middle_left);
      return;
    case LIN_PLAYLIST: {
      const bool fav = !strcmp(l.nome, TAB5_MUSICA_FAVORITAS);
      d.fillRoundRect(qx, cy - q / 2, q, q, TEMA_RAIO_M, fav ? ac : TEMA_SUPERFICIE_ALTA);
      if (fav) { ic_coracao(d, qx + q / 2, cy - 4, 16, TEMA_TEXTO, true, ac); }
      else     { ic_lista(d, qx + q / 2, cy, 16, ac); }
      tab5_musica_nome_utf8(l.nome, t, sizeof(t));
      snprintf(sub, sizeof(sub), tr("Playlist  ·  %d música%s"), l.n, l.n == 1 ? "" : "s");
      tab5_escrever(d, ">", x0 + largura - M, cy, 30, false, TEMA_TEXTO_3, TEMA_FUNDO, middle_right);
      dir -= 40;
      break;
    }
    case LIN_DISCO:
      d.fillRoundRect(qx, cy - q / 2, q, q, TEMA_RAIO_M, ac);
      ic_pendrive(d, qx + q / 2, cy, sobre_destaque(), ac);
      snprintf(t, sizeof(t), "%s", l.nome);
      snprintf(sub, sizeof(sub), tr("%s  ·  USB-A"), tr(tab5_volume_tipo(l.caminho)));
      tab5_escrever(d, ">", x0 + largura - M, cy, 30, false, TEMA_TEXTO_3, TEMA_FUNDO, middle_right);
      dir -= 40;
      break;
    case LIN_PASTA:
      ic_pasta(d, qx + 12, cy, TEMA_APP_AMARELO);   // a pasta amarela de sempre, no tom do tema
      tab5_musica_nome_utf8(l.nome, t, sizeof(t));
      snprintf(sub, sizeof(sub), tr("Pasta"));
      tab5_escrever(d, ">", x0 + largura - M, cy, 30, false, TEMA_TEXTO_3, TEMA_FUNDO, middle_right);
      dir -= 40;
      break;
    default: {
      d.fillRoundRect(qx, cy - q / 2, q, q, TEMA_RAIO_M, TEMA_SUPERFICIE_ALTA);
      if (toca) { barras_tocando(d, qx + q / 2, cy, ac); }
      else      { ic_nota(d, qx + q / 2, cy - 2, 26, TEMA_TEXTO_2); }
      nome_para_mostrar(l.nome, t, sizeof(t), sub, sizeof(sub));
      // O botao da direita: + (por numa playlist) ou - (tirar desta playlist).
      const int bx = x0 + largura - M - 22;
      if (s_pl[0]) { d.fillRoundRect(bx - 12, cy - 2, 24, 5, 2, TEMA_TEXTO_2); }
      else         { ic_mais(d, bx, cy, 12, TEMA_TEXTO_2); }
      // A DURACAO, na coluna da direita (como no Spotify).
      uint32_t ms;
      if (tab5_musica_duracao_lembrada(l.caminho, &ms) && ms) {
        char dt[16];
        tempo_txt(dt, sizeof(dt), ms);
        tab5_escrever(d, dt, bx - 40, cy, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_right);
      }
      dir = bx - 40 - 80;
      break;
    }
  }
  const int tx = qx + q + 20;
  if (sub[0]) {
    tab5_escrever(d, t, tx, cy - 16, 28, toca, toca ? ac : TEMA_TEXTO, TEMA_FUNDO, middle_left, dir - tx);
    tab5_escrever(d, sub, tx, cy + 18, 22, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_left, dir - tx);
  } else {
    tab5_escrever(d, t, tx, cy, 28, toca, toca ? ac : TEMA_TEXTO, TEMA_FUNDO, middle_left, dir - tx);
  }
}

static int linha_tocando(const tab5_musica_estado_t& e)
{
  if (!e.ativo || !e.caminho[0]) { return -1; }
  if (s_aba == ABA_PLAYLISTS && (!s_pl[0] || strcmp(e.playlist, s_pl) != 0)) { return -1; }
  for (int i = 0; i < s_n; ++i) {
    if (s_lin[i].tipo == LIN_MUSICA && !strcmp(s_lin[i].caminho, e.caminho)) { return i; }
  }
  return -1;
}

static void miniplayer(lgfx::LovyanGFX& d, const area_t& a, const tab5_musica_estado_t& e)
{
  const int y = a.y + a.altura - MINI_A, W = a.largura;
  d.fillRect(a.x, y, W, MINI_A, TEMA_FUNDO);
  const uint16_t bg = s_tinta ? s_tinta : TEMA_SUPERFICIE;
  d.fillRoundRect(a.x + 12, y + 8, W - 24, MINI_A - 16, TEMA_RAIO_G, bg);
  const int cy = y + MINI_A / 2;
  const int q = 72, qx = a.x + 28;
  if (s_capa_ok && s_capa) {
    const float z = (float)q / s_capa->width();
    s_capa->pushRotateZoomWithAA(&d, qx + q / 2.0f, cy, 0.0f, z, z);
  } else {
    d.fillRoundRect(qx, cy - q / 2, q, q, TEMA_RAIO_P, TEMA_SUPERFICIE_ALTA);
    ic_nota(d, qx + q / 2, cy - 2, 28, destaque());
  }
  const int tx = qx + q + 18, tl = W - (tx - a.x) - 170;
  tab5_escrever(d, e.titulo, tx, cy - 16, 28, true, TEMA_TEXTO, bg, middle_left, tl);
  tab5_escrever(d, e.artista[0] ? e.artista : tr("Tocando"), tx, cy + 18, 22, false, destaque(), bg, middle_left, tl);
  const int bp = a.x + W - 130, bn = a.x + W - 56;
  if (e.tocando) { ic_pausa(d, bp, cy, 40, TEMA_TEXTO); } else { ic_tocar(d, bp + 3, cy, 44, TEMA_TEXTO); }
  ic_pular(d, bn, cy, 15, true, TEMA_TEXTO);
  // A linha do tempo, fina, na base do cartao.
  const int lx = a.x + 30, lw = W - 60;
  const int f = e.dur_ms ? (int)((uint64_t)lw * (e.pos_ms > e.dur_ms ? e.dur_ms : e.pos_ms) / e.dur_ms) : 0;
  d.fillRect(lx, y + MINI_A - 13, lw, 3, TEMA_SUPERFICIE_ALTA);
  if (f > 0) { d.fillRect(lx, y + MINI_A - 13, f, 3, TEMA_TEXTO); }
}

static void desenhar_biblioteca(lgfx::LovyanGFX& d, const area_t& a, const tab5_musica_estado_t& e, bool tudo)
{
  const int W = a.largura;
  const uint16_t ac = destaque();
  if (tudo) {
    d.fillRect(a.x, a.y, W, bb_lista_y(), TEMA_FUNDO);
    tab5_escrever(d, tr("Biblioteca"), a.x + M, a.y + BB_TIT_A / 2 + 4, TEMA_LETRA_TITULO, true, TEMA_TEXTO, TEMA_FUNDO, middle_left);
    // As abas.
    const char* NOMES[2] = { tr("Pastas"), tr("Playlists") };
    int x = a.x + M;
    for (int i = 0; i < 2; ++i) {
      const bool sel = (int)s_aba == i;
      d.setFont(tab5_fonte_suave(true, 26));
      const int w = d.textWidth(NOMES[i]) + 48;
      if (sel) { d.fillRoundRect(x, a.y + BB_ABAS_Y + 6, w, BB_ABAS_A - 12, (BB_ABAS_A - 12) / 2, ac); }
      else     { d.drawRoundRect(x, a.y + BB_ABAS_Y + 6, w, BB_ABAS_A - 12, (BB_ABAS_A - 12) / 2, TEMA_LINHA); }
      tab5_escrever(d, NOMES[i], x + w / 2, a.y + BB_ABAS_Y + BB_ABAS_A / 2, 26, true, sel ? sobre_destaque() : TEMA_TEXTO,
            sel ? ac : TEMA_FUNDO, middle_center);
      x += w + 14;
    }
    // A linha de baixo das abas: a pasta, ou a playlist aberta.
    const int sy = a.y + BB_SUB_Y + BB_SUB_A / 2;
    char t[260], u[300];
    int tx = a.x + M;
    const bool volta = (s_aba == ABA_PASTAS && s_rel[0]) || (s_aba == ABA_PLAYLISTS && s_pl[0]);
    if (volta) {
      tab5_linha_grossa(d, tx + 14, sy - 12, tx + 2, sy, 3.0f, ac);
      tab5_linha_grossa(d, tx + 2, sy, tx + 14, sy + 12, 3.0f, ac);
      tx += 36;
    }
    if (s_aba == ABA_PASTAS) {
      pasta_para_mostrar(s_rel, true, t, sizeof(t));
      snprintf(u, sizeof(u), tr("%d música%s%s"), s_n_musicas, s_n_musicas == 1 ? "" : "s", s_cheia ? "+" : "");
    } else if (s_pl[0]) {
      snprintf(t, sizeof(t), "%s", s_pl);
      snprintf(u, sizeof(u), tr("%d música%s"), s_n_musicas, s_n_musicas == 1 ? "" : "s");
    } else {
      snprintf(t, sizeof(t), tr("Suas playlists"));
      snprintf(u, sizeof(u), "%d", s_n > 0 ? s_n - 1 : 0);
    }
    char tu[300];
    tab5_musica_nome_utf8(t, tu, sizeof(tu));
    tab5_escrever(d, u, a.x + W - M, sy, 22, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_right);
    tab5_escrever(d, tu, tx, sy, 28, true, TEMA_TEXTO, TEMA_FUNDO, middle_left, W - (tx - a.x) - M - 150);
    // Os botoes da playlist aberta: tocar, aleatorio, apagar.
    if (s_aba == ABA_PLAYLISTS && s_pl[0]) {
      const int by = a.y + BB_SUB_Y + BB_SUB_A + 8, bh = BB_BOT_A - 20;
      const int bw = (W - 2 * M - 2 * 14) / 3;
      int bx = a.x + M;
      d.fillRoundRect(bx, by, bw, bh, bh / 2, ac);
      ic_tocar(d, bx + 40, by + bh / 2, 30, sobre_destaque());
      tab5_escrever(d, tr("Tocar"), bx + bw / 2 + 16, by + bh / 2, 26, true, sobre_destaque(), ac, middle_center);
      bx += bw + 14;
      d.fillRoundRect(bx, by, bw, bh, bh / 2, TEMA_SUPERFICIE_ALTA);
      ic_aleatorio(d, bx + 38, by + bh / 2, 13, TEMA_TEXTO);
      tab5_escrever(d, tr("Aleatório"), bx + bw / 2 + 18, by + bh / 2, 26, true, TEMA_TEXTO, TEMA_SUPERFICIE_ALTA, middle_center);
      bx += bw + 14;
      const bool conf = (int32_t)(s_apagar_ate - tab5_agora_ms()) > 0;
      const uint16_t cf = conf ? (uint16_t)TEMA_PERIGO : (uint16_t)TEMA_SUPERFICIE_ALTA;
      d.fillRoundRect(bx, by, bw, bh, bh / 2, cf);
      tab5_escrever(d, conf ? tr("Confirmar?") : tr("Apagar"), bx + bw / 2, by + bh / 2, 26, true, TEMA_TEXTO, cf, middle_center);
    }
  }
  const int mini = e.ativo ? MINI_A : 0;
  const int ly = bb_lista_y();
  const area_t janela = { a.x, a.y + ly, a.largura, a.altura - ly - mini };
  if (s_n == 0) {
    if (tudo) {
      d.fillRect(janela.x, janela.y, janela.largura, janela.altura, TEMA_FUNDO);
      const int cx = a.x + W / 2, cy = janela.y + janela.altura / 3;
      if (s_carregando) {
        tab5_escrever(d, tr("Carregando..."), cx, cy, 28, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      } else if (s_sem_cartao && s_rel[0] == '/') {
        tab5_escrever(d, tr("O pendrive não está mais aqui"), cx, cy, 30, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      } else if (s_sem_cartao) {
        tab5_escrever(d, s_aba == ABA_PLAYLISTS && s_pl[0] ? tr("Esta playlist sumiu") : tr("Sem cartão SD"), cx, cy, 30, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      } else if (s_aba == ABA_PLAYLISTS) {
        tab5_escrever(d, tr("Playlist vazia"), cx, cy, 30, true, TEMA_TEXTO, TEMA_FUNDO, middle_center);
        tab5_escrever(d, tr("Toque no + de uma música em Pastas"), cx, cy + 50, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
        tab5_escrever(d, tr("ou no + da tela Tocando."), cx, cy + 84, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      } else {
        const bool pend = s_rel[0] == '/';
        tab5_escrever(d, s_rel[0] && !(pend && !strchr(s_rel + 1, '/')) ? tr("Pasta vazia")
                 : (pend ? tr("Nenhuma música neste pendrive") : tr("Nenhuma música ainda")),
              cx, cy, 30, true, TEMA_TEXTO, TEMA_FUNDO, middle_center);
        tab5_escrever(d, pend ? tr("O Tab5 toca arquivos MP3 e WAV") : tr("Copie arquivos MP3 ou WAV para a"), cx, cy + 50, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
        tab5_escrever(d, pend ? tr("de qualquer pasta do pendrive.") : tr("pasta Music (ou Música) do cartão,"), cx, cy + 84, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
        if (!pend) { tab5_escrever(d, tr("ou espete um pendrive no USB-A."), cx, cy + 118, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center); }
      }
    }
    lista_esconder(&s_lista);
  } else {
    // A linha que toca: as barrinhas mexem, entao ela e redesenhada sempre.
    if (s_tocando_linha >= 0 && e.tocando) { lista_item_mudou(&s_lista, s_tocando_linha); }
    lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, tudo);
  }
  if (mini) {
    const uint32_t sig = (uint32_t)(e.pos_ms / 1000) * 7919u + e.tocando + (uint32_t)e.idx * 31u + s_capa_ok * 2u + s_tinta;
    if (tudo || sig != s_mini_sig) { s_mini_sig = sig; miniplayer(d, a, e); }
  }
  // O aviso rapido, acima do miniplayer.
  if (s_aviso_ate) {
    if ((int32_t)(s_aviso_ate - tab5_agora_ms()) > 0) {
      const int y = a.y + a.altura - mini - 76;
      d.setFont(tab5_fonte_suave(true, 24));
      const int w = d.textWidth(s_aviso) + 56;
      d.fillRoundRect(a.x + (W - w) / 2, y, w, 56, 28, TEMA_SUPERFICIE_ALTA);
      tab5_escrever(d, s_aviso, a.x + W / 2, y + 28, 24, true, TEMA_TEXTO, TEMA_SUPERFICIE_ALTA, middle_center);
    } else {
      s_aviso_ate = 0;
      lista_itens_mudaram(&s_lista, s_n);   // o que estava embaixo do aviso volta
      s_forcar = true;
    }
  }
}

// ---- a folha "Adicionar a playlist" ---------------------------------------------------------
static const int FL_LIN = 88;
static int folha_y(const area_t& a) { const int h = FL_LIN * (s_folha_n + 1) + 120; return a.altura - (h > a.altura - 80 ? a.altura - 80 : h); }

static void abrir_folha(const char* caminho)
{
  if (!caminho || !caminho[0]) { return; }
  if (!s_folha_pl) { s_folha_pl = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * 32, MALLOC_CAP_SPIRAM); }
  if (!s_folha_pl) { return; }
  // Um opendir na pasta das playlists: rapido o bastante para a interface.
  s_folha_n = tab5_musica_playlists(s_folha_pl, 32);
  if (s_folha_n < 0) { s_folha_n = 0; }
  snprintf(s_folha_caminho, sizeof(s_folha_caminho), "%s", caminho);
  s_folha = true;
  sistema_pedir_redesenho_completo();
}

static void desenhar_folha(lgfx::LovyanGFX& d, const area_t& a)
{
  const int y0 = a.y + folha_y(a), W = a.largura;
  d.fillRect(a.x, a.y, W, y0 - a.y, TEMA_FUNDO);
  d.fillRoundRect(a.x, y0, W, a.y + a.altura - y0 + 30, TEMA_RAIO_G, TEMA_SUPERFICIE);
  d.fillRoundRect(a.x + W / 2 - 40, y0 + 14, 80, 6, 3, TEMA_LINHA);
  char t[TAB5_MUSICA_NOME_MAX * 2], tt[TAB5_MUSICA_NOME_MAX * 2], sub[8];
  const char* b = strrchr(s_folha_caminho, '/');
  nome_para_mostrar(b ? b + 1 : s_folha_caminho, t, sizeof(t), sub, sizeof(sub));
  snprintf(tt, sizeof(tt), tr("Adicionar \"%s\" a"), t);
  tab5_escrever(d, tt, a.x + M, y0 + 60, 28, true, TEMA_TEXTO, TEMA_SUPERFICIE, middle_left, W - 2 * M);
  int y = y0 + 100;
  d.fillRoundRect(a.x + M, y + 12, 64, 64, TEMA_RAIO_M, TEMA_SUPERFICIE_ALTA);
  ic_mais(d, a.x + M + 32, y + 44, 16, destaque());
  tab5_escrever(d, tr("Nova playlist"), a.x + M + 88, y + 44, 28, true, destaque(), TEMA_SUPERFICIE, middle_left);
  for (int i = 0; i < s_folha_n && y + 2 * FL_LIN < a.y + a.altura; ++i) {
    y += FL_LIN;
    const bool fav = !strcmp(s_folha_pl[i].nome, TAB5_MUSICA_FAVORITAS);
    d.fillRoundRect(a.x + M, y + 12, 64, 64, TEMA_RAIO_M, fav ? destaque() : TEMA_SUPERFICIE_ALTA);
    if (fav) { ic_coracao(d, a.x + M + 32, y + 40, 16, TEMA_TEXTO, true, destaque()); }
    else     { ic_lista(d, a.x + M + 32, y + 44, 16, destaque()); }
    tab5_musica_nome_utf8(s_folha_pl[i].nome, t, sizeof(t));
    tab5_escrever(d, t, a.x + M + 88, y + 44, 28, false, TEMA_TEXTO, TEMA_SUPERFICIE, middle_left, W - 2 * M - 100);
  }
}

static void tocar_folha(int x, int y)
{
  (void)x;
  const area_t a = sistema_area();
  const int y0 = folha_y(a);
  s_folha = false;
  sistema_pedir_redesenho_completo();
  if (y < y0) { return; }   // fora: fecha
  const int k = (y - (y0 + 100)) / FL_LIN;
  if (k == 0 && y >= y0 + 100) {
    s_nova = true;
    s_nova_txt[0] = '\0';
    snprintf(s_nova_por, sizeof(s_nova_por), "%s", s_folha_caminho);
    tecvirt_abrir(&s_kb, TECVIRT_TEXTO);
    return;
  }
  if (k >= 1 && k <= s_folha_n) { pedir_pl(OP_POR, s_folha_pl[k - 1].nome, s_folha_caminho); }
}

// ---- a pagina "Nova playlist" ------------------------------------------------------------
static void desenhar_nova(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  tecvirt_posicionar(&s_kb, a);
  if (tudo) {
    d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
    tab5_escrever(d, tr("Nova playlist"), a.x + M, a.y + 60, TEMA_LETRA_TITULO, true, TEMA_TEXTO, TEMA_FUNDO, middle_left);
    tab5_escrever(d, tr("ENVIAR cria; X cancela."), a.x + M, a.y + 230, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_left);
  }
  char campo[64];
  snprintf(campo, sizeof(campo), "%s_", s_nova_txt);
  d.fillRoundRect(a.x + M, a.y + 110, a.largura - 2 * M, 80, TEMA_RAIO_M, TEMA_SUPERFICIE);
  tab5_escrever(d, campo, a.x + M + 20, a.y + 150, 34, false, TEMA_TEXTO, TEMA_SUPERFICIE, middle_left, a.largura - 2 * M - 40);
  tecvirt_pintar(&s_kb, d, tudo);
}

static void nova_fechar(void)
{
  s_nova = false;
  tecvirt_fechar(&s_kb);
  sistema_pedir_redesenho_completo();
}

static void nova_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR) {
    if (s_nova_txt[0]) {
      pedir_pl(OP_CRIAR, s_nova_txt, s_nova_por);
      s_aba = ABA_PLAYLISTS;
      s_pl[0] = '\0';
      s_tela = TELA_BIBLIOTECA;
    }
    nova_fechar();
    return;
  }
  if (ac == TECVIRT_FECHAR) { nova_fechar(); return; }
  tecvirt_editar(s_nova_txt, sizeof(s_nova_txt), ac);
  sistema_pedir_redesenho();
}

// ---- desenhar -------------------------------------------------------------------------------
static uint32_t assinatura(const tab5_musica_estado_t& e)
{
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
  for (const char* p = e.caminho; *p; ++p) { mix((uint8_t)*p); }
  for (const char* p = e.titulo; *p; ++p) { mix((uint8_t)*p); }
  for (const char* p = e.erro; *p; ++p) { mix((uint8_t)*p); }
  mix(e.ativo); mix(e.tocando); mix(e.aleatorio); mix(e.repetir); mix(s_fav);
  mix(ajustes_volume()); mix((uint32_t)e.idx); mix((uint32_t)e.n); mix(e.kbps);
  return h;
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  bool tudo = sistema_area_foi_limpa() || s_forcar;
  s_forcar = false;
  // As linhas do espectro saem da altura do quadrado - que muda ao girar.
  int vl = (geo_tc(a).capa_l - 40) / SEG_P;
  if (vl > VIS_MAX_LINHAS) { vl = VIS_MAX_LINHAS; }
  if (!s_vis_cores_ok || vl != s_vis_linhas) { s_vis_linhas = vl; vis_cores(d); }

  tab5_musica_estado_t e;
  tab5_musica_estado(&e);

  // A CAPA mudou (outra musica, a busca terminou): abre no worker.
  char arq[200];
  uint32_t ver;
  const tab5_musica_capa_t ce = tab5_musica_capa(arq, sizeof(arq), &ver);
  if (ver != s_capa_ver && !s_capa_rodando) {   // pintando: espera o worker (o sprite e dele)
    s_capa_ver = ver;
    if (ce == TAB5_MUSICA_CAPA_PRONTA && e.ativo) {
      if (strcmp(arq, s_capa_arq) != 0 || !s_capa_ok) {
        if (!s_capa) { const int L = geo_tc(a).capa_l; s_capa = tab5_sprite_novo(&d, L, L); }
        if (s_capa) {
          s_capa_ok = false;   // a interface solta o sprite ate o worker terminar
          snprintf(s_capa_arq, sizeof(s_capa_arq), "%s", arq);
          snprintf(s_capa_arg.arq, sizeof(s_capa_arg.arq), "%s", arq);
          s_capa_rodando = sistema_trabalho(trab_capa, &s_capa_arg);
          if (!s_capa_rodando) { s_capa_arq[0] = '\0'; }
        }
      }
    } else if (ce == TAB5_MUSICA_CAPA_NENHUMA || ce == TAB5_MUSICA_CAPA_BUSCANDO) {
      // Buscando: segura a anterior so se for a mesma musica (nao e: a versao muda ao trocar).
      if (s_capa_ok || s_tinta) { s_capa_ok = false; s_tinta = 0; s_capa_arq[0] = '\0'; tudo = true; }
    }
  }
  // O CORACAO: a musica mudou, pergunta ao worker se ela esta nas Favoritas.
  if (e.ativo && strcmp(e.caminho, s_fav_caminho) != 0) {
    snprintf(s_fav_caminho, sizeof(s_fav_caminho), "%s", e.caminho);
    s_fav = false;
    pedir_pl(OP_FAV_TEM, nullptr, e.caminho);
  }

  if (s_nova) { desenhar_nova(d, a, tudo); d.setTextStyle(estilo); tab5_fonte_corpo(d); return; }
  // "Confirmar?" do apagar venceu: o botao volta a "Apagar".
  if (s_apagar_ate && (int32_t)(s_apagar_ate - tab5_agora_ms()) <= 0) { s_apagar_ate = 0; tudo = true; }

  const uint32_t sig = assinatura(e);
  if (sig != s_sig) { s_sig = sig; if (s_tela == TELA_TOCANDO) { tudo = true; } }
  if (s_folha) {
    if (tudo) { desenhar_folha(d, a); }
  } else if (s_tela == TELA_TOCANDO) {
    const uint32_t prog = (e.ativo ? e.pos_ms / 1000 + 1 : 0) * 100003u + e.dur_ms / 1000 + s_restante;
    desenhar_tocando(d, a, e, tudo);
    if (!tudo && prog != s_prog_sig) { desenhar_progresso(d, a, geo_tc(a), e); }
    s_prog_sig = prog;
  } else {
    const int lt = linha_tocando(e);
    if (lt != s_tocando_linha) {
      if (s_tocando_linha >= 0 && s_tocando_linha < s_n) { lista_item_mudou(&s_lista, s_tocando_linha); }
      s_tocando_linha = lt;
      if (lt >= 0) { lista_item_mudou(&s_lista, lt); }
    }
    desenhar_biblioteca(d, a, e, tudo);
  }
  d.setTextStyle(estilo);
  tab5_fonte_corpo(d);
}

// ---- toque ----------------------------------------------------------------------------------
static void tocar_tocando(int x, int y)
{
  const area_t a = sistema_area();
  const geo_tc_t g = geo_tc(a);
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  if (y < TC_CAB_A) {
    if (x < 120)                   { s_tela = TELA_BIBLIOTECA; s_forcar = true; sistema_pedir_redesenho_completo(); }
    else if (x > a.largura - 110)  { trocar_tema(); }
    return;
  }
  if (y >= g.capa_y && y < g.capa_y + g.capa_l && x >= g.capa_x && x < g.capa_x + g.capa_l) {
    if (s_capa_ok) { s_ver_espectro = !s_ver_espectro; s_forcar = true; }
    else           { trocar_tema(); }
    sistema_pedir_redesenho();
    return;
  }
  // Daqui para baixo, a coluna do texto e dos controles (deitado, a direita).
  if (x < g.ix) { return; }
  x -= g.ix;
  const int W = g.iw;
  if (abs(y - (g.tit_y + 14)) < 44 && x > W - M - 100 && e.ativo) {
    s_fav = !s_fav;   // na hora; o worker confirma
    pedir_pl(OP_FAV_TROCAR, nullptr, e.caminho);
    s_forcar = true;
    sistema_pedir_redesenho();
    return;
  }
  // O tempo da direita (embaixo da linha) troca duracao <-> quanto falta;
  // a faixa da linha, acima dele, busca.
  if (y > g.prog_y + 12 && y < g.prog_y + 60 && x > W - 170) {
    s_restante = !s_restante;
  } else if (y >= g.prog_y - 34 && y <= g.prog_y + 12 && e.ativo && e.dur_ms) {
    const int lx = M + 16, lw = W - 2 * (M + 16);
    int dx = x - lx;
    dx = dx < 0 ? 0 : (dx > lw ? lw : dx);
    tab5_musica_ir_para((uint32_t)((uint64_t)e.dur_ms * dx / lw));
  } else if (abs(y - g.ctrl_y) <= 66) {
    const int cx = W / 2;
    if      (abs(x - cx) <= 70) {
      if (e.ativo) { tab5_musica_tocar_pausar(); }
      else         { s_tela = TELA_BIBLIOTECA; s_forcar = true; sistema_pedir_redesenho_completo(); }
    }
    else if (abs(x - (cx - 270)) <= 60) { tab5_musica_definir_aleatorio(!e.aleatorio); }
    else if (abs(x - (cx - 140)) <= 60) { tab5_musica_anterior(); }
    else if (abs(x - (cx + 140)) <= 60) { tab5_musica_proxima(); }
    else if (abs(x - (cx + 270)) <= 60) {
      tab5_musica_definir_repetir((tab5_musica_repetir_t)((e.repetir + 1) % 3));
    }
  } else if (abs(y - g.vol_y) <= 44) {
    const int vx0 = M + 112, vx1 = W - M - 112;
    if (x < M + 70) { if (e.ativo) { abrir_folha(e.caminho); } return; }
    if (x > W - M - 70) { if (e.ativo) { tab5_musica_parar(); } return; }
    int v = ajustes_volume();
    if      (x < vx0) { v -= 16; }
    else if (x > vx1) { v += 16; }
    else              { v = (x - vx0) * AJUSTES_VOLUME_MAX / (vx1 - vx0); }
    if (v < AJUSTES_VOLUME_MIN) { v = AJUSTES_VOLUME_MIN; }
    if (v > AJUSTES_VOLUME_MAX) { v = AJUSTES_VOLUME_MAX; }
    ajustes_definir_volume((uint8_t)v);
  }
  sistema_pedir_redesenho();
}

static void tocar_biblioteca(int x, int y)
{
  const area_t a = sistema_area();
  const int W = a.largura;
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  // O miniplayer.
  if (e.ativo && y >= a.altura - MINI_A) {
    if      (x > W - 95)  { tab5_musica_proxima(); }
    else if (x > W - 175) { tab5_musica_tocar_pausar(); }
    else                  { s_tela = TELA_TOCANDO; s_forcar = true; sistema_pedir_redesenho_completo(); }
    sistema_pedir_redesenho();
    return;
  }
  int dy = 0;
  const int i = lista_item_tocado(&s_lista, x, y, &dy);
  if (i == LISTA_ARRASTE) { return; }
  if (i >= 0 && i < s_n) {
    const linha_t& l = s_lin[i];
    switch (l.tipo) {
      case LIN_NOVA:
        s_nova = true; s_nova_txt[0] = '\0'; s_nova_por[0] = '\0';
        tecvirt_abrir(&s_kb, TECVIRT_TEXTO);
        sistema_pedir_redesenho_completo();
        return;
      case LIN_PLAYLIST:
        snprintf(s_pl, sizeof(s_pl), "%s", l.nome);
        s_apagar_ate = 0;
        navegar();
        return;
      case LIN_PASTA:
      case LIN_DISCO:
        snprintf(s_rel, sizeof(s_rel), "%s", l.caminho);
        navegar();
        return;
      default:
        if (x > W - M - 60) {   // o botao da direita
          if (s_pl[0]) { pedir_pl(OP_TIRAR, s_pl, l.caminho); }
          else         { abrir_folha(l.caminho); }
          return;
        }
        {
          const bool ok = s_pl[0] ? tab5_musica_tocar_playlist(s_pl, i, APP_NOME)
                                  : tab5_musica_tocar(s_rel, i - s_extra, APP_NOME);
          if (!ok) { ESP_LOGW(TAG, "o tocador nao ligou"); }
          else     { s_tela = TELA_TOCANDO; s_forcar = true; sistema_pedir_redesenho_completo(); }
        }
        sistema_pedir_redesenho();
        return;
    }
  }
  // As abas.
  if (y >= BB_ABAS_Y && y < BB_ABAS_Y + BB_ABAS_A) {
    const aba_t nova = x < M + 160 ? ABA_PASTAS : ABA_PLAYLISTS;
    if (x < M + 360 && nova != s_aba) { s_aba = nova; s_pl[0] = '\0'; navegar(); }
    else if (x < M + 360 && nova == s_aba && s_aba == ABA_PLAYLISTS && s_pl[0]) { s_pl[0] = '\0'; navegar(); }
    return;
  }
  // A linha da pasta/playlist: volta.
  if (y >= BB_SUB_Y && y < BB_SUB_Y + BB_SUB_A) {
    if (s_aba == ABA_PASTAS && s_rel[0]) {
      // "/usb/Rock" -> "/usb" -> "" (a raiz, com o cartao e o pendrive).
      char* barra = strrchr(s_rel, '/');
      if (barra) { *barra = '\0'; } else { s_rel[0] = '\0'; }
      navegar();
    } else if (s_aba == ABA_PLAYLISTS && s_pl[0]) {
      s_pl[0] = '\0';
      navegar();
    }
    return;
  }
  // Os botoes da playlist aberta.
  if (s_aba == ABA_PLAYLISTS && s_pl[0] && y >= BB_SUB_Y + BB_SUB_A && y < BB_SUB_Y + BB_SUB_A + BB_BOT_A) {
    const int bw = (W - 2 * M - 2 * 14) / 3;
    const int k = (x - M) / (bw + 14);
    if (k == 0 || k == 1) {
      tab5_musica_definir_aleatorio(k == 1);
      if (tab5_musica_tocar_playlist(s_pl, -1, APP_NOME)) { s_tela = TELA_TOCANDO; s_forcar = true; sistema_pedir_redesenho_completo(); }
    } else if (k == 2) {
      if ((int32_t)(s_apagar_ate - tab5_agora_ms()) > 0) {
        pedir_pl(OP_APAGAR, s_pl, nullptr);
        s_apagar_ate = 0;
        s_pl[0] = '\0';
        navegar();
      } else {
        s_apagar_ate = tab5_agora_ms() + 3000;
        s_forcar = true;
      }
    }
    sistema_pedir_redesenho();
  }
}

static void ao_tocar(int x, int y)
{
  if (s_nova) {
    const area_t a = sistema_area();
    if (tecvirt_contem(&s_kb, a.x + x, a.y + y)) {
      const int ac = tecvirt_clique(&s_kb, a.x + x, a.y + y);
      if (ac) { nova_tecla(ac); }
    }
    return;
  }
  if (s_folha) { tocar_folha(x, y); return; }
  if (s_tela == TELA_TOCANDO) { tocar_tocando(x, y); }
  else                        { tocar_biblioteca(x, y); }
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (s_nova && tecvirt_aberto(&s_kb)) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb, acoes, 8);
    for (int i = 0; i < n && s_nova; ++i) { nova_tecla(acoes[i]); }
    return;
  }
  if (s_tela == TELA_BIBLIOTECA && !s_folha && lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_teclar(const char* txt, uint8_t codigo, uint8_t)
{
  if (!s_nova) { return; }
  if (codigo == TAB5_TECLA_ENTER)       { nova_tecla(TECVIRT_ENVIAR); }
  else if (codigo == TAB5_TECLA_ESC)    { nova_tecla(TECVIRT_FECHAR); }
  else if (codigo == TAB5_TECLA_APAGAR) { nova_tecla(TECVIRT_APAGAR); }
  else if (!codigo && txt && txt[0] && strlen(s_nova_txt) + strlen(txt) < sizeof(s_nova_txt)) {
    strcat(s_nova_txt, txt);
    sistema_pedir_redesenho();
  }
}

// ---- ciclo de vida --------------------------------------------------------------------------
static bool ao_abrir(void)
{
  if (!s_lin)      { s_lin      = (linha_t*)heap_caps_malloc(sizeof(linha_t) * LIN_MAX, MALLOC_CAP_SPIRAM); }
  if (!s_lin_tras) { s_lin_tras = (linha_t*)heap_caps_malloc(sizeof(linha_t) * LIN_MAX, MALLOC_CAP_SPIRAM); }
  if (!s_lin || !s_lin_tras) { return false; }
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  if (!tab5_musica_pasta(s_raiz, sizeof(s_raiz))) { s_raiz[0] = '\0'; }
  // Tocando: abre na tela Tocando, e a biblioteca no lugar de onde toca.
  s_tela = e.ativo ? TELA_TOCANDO : TELA_BIBLIOTECA;
  if (e.ativo && e.playlist[0]) { s_aba = ABA_PLAYLISTS; snprintf(s_pl, sizeof(s_pl), "%s", e.playlist); }
  else if (e.ativo)             { s_aba = ABA_PASTAS; snprintf(s_rel, sizeof(s_rel), "%s", e.rel); s_pl[0] = '\0'; }
  if (s_rel[0] == '/' && !tab5_volume_disponivel(s_rel)) { s_rel[0] = '\0'; }   // o pendrive saiu
  tab5_musica_definir_cor(destaque());
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, linha_altura, linha_desenhar, nullptr);
  s_n = 0;
  // A CAPA GUARDADA do fechamento anterior (ver ao_fechar) vale se for a
  // mesma: o desenhar compara o arquivo e nao manda o worker de novo.
  s_capa_ver = 0xFFFFFFFF;
  if (!s_capa || !s_capa_ok) { s_capa_ok = false; s_capa_arq[0] = '\0'; }
  s_tinta = s_capa_ok ? s_tinta_tras : 0;
  s_fav_caminho[0] = '\0';
  s_folha = s_nova = false;
  s_aviso_ate = 0;
  s_vis_cores_ok = false;
  memset(s_vis_nivel, 0, sizeof(s_vis_nivel));
  memset(s_vis_pico, 0, sizeof(s_vis_pico));
  s_vis_vivo = false;
  s_sig = 0;
  s_forcar = true;
  carregar();
  return true;
}

static void ao_fechar(void)
{
  // A musica continua (parte de fundo); a lista, a capa e o resto saem da
  // memoria. O worker ja terminou (o sistema espera antes de chamar isto).
  if (s_nova) { s_nova = false; tecvirt_fechar(&s_kb); }
  heap_caps_free(s_lin);      s_lin = nullptr;
  heap_caps_free(s_lin_tras); s_lin_tras = nullptr;
  heap_caps_free(s_dur_arg);  s_dur_arg = nullptr;
  heap_caps_free(s_folha_pl); s_folha_pl = nullptr;
  // A CAPA FICA enquanto a musica toca (~627 KB na PSRAM): reabrir o app nao
  // le nem decodifica o JPEG de novo. Medido 06/11/2026: abrir o app ja
  // aperta a CPU e o barramento (o redesenho inteiro), e a decodificacao da
  // capa no worker somava a isso a cada volta - o engasgo de sair e voltar.
  // Com a musica parada, solta (ninguem vai mostrar).
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  if (!e.ativo || !s_capa_ok) {
    tab5_sprite_soltar(&s_capa);
    s_capa_ok = false;
    s_capa_arq[0] = '\0';
  }
  s_capa_rodando = s_dur_rodando = false;
  s_n = 0;
  s_carregando = false;
}

// A TELA GIROU (teclado fisico encaixado ou retirado). O quadrado da capa
// muda de tamanho: o sprite renasce do tamanho novo (o worker repinta) em vez
// de esticar o antigo, e o espectro, a lista e o miniplayer recomecam na
// geometria nova. Pintando agora, o sprite e do worker: fica, e escala.
static void ao_girar(void)
{
  if (!s_capa_rodando) {
    tab5_sprite_soltar(&s_capa);
    s_capa_ok = false;
    s_capa_arq[0] = '\0';
    s_capa_ver = 0xFFFFFFFF;
  }
  vis_zerar_tela();
  s_vis_cores_ok = false;
  s_mini_sig = 0;
  lista_itens_mudaram(&s_lista, s_n);
  s_forcar = true;
}

// Tocando, a tela acompanha o espectro (~30 quadros/s) e o segundo; na
// biblioteca, as barrinhas da linha que toca e o miniplayer. Pausado: de
// meio em meio segundo. Parado: so quando tocam (ou o aviso sumir).
static uint32_t periodo_agora(void)
{
  if (s_nova) { return 0; }
  if (s_vis_vivo || s_tema_nome_ate) { return 33; }
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  if (s_aviso_ate || s_apagar_ate) { return 100; }
  if (e.ativo && e.tocando) { return (s_tela == TELA_TOCANDO && mostra_espectro()) ? 33 : 150; }
  if (s_carregando) { return 200; }
  return e.ativo ? 500 : 0;
}

static void icone_musica(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(52, 36, 86));
  const int s = lado;
  // Duas colcheias ligadas.
  const int h1x = x + s * 30 / 100, h2x = x + s * 66 / 100;
  const int hy1 = y + s * 70 / 100, hy2 = y + s * 62 / 100;
  const int rx = s / 10, ry = s / 13, haste = s / 22;
  d.fillEllipse(h1x, hy1, rx, ry, LILAS);
  d.fillEllipse(h2x, hy2, rx, ry, LILAS);
  const int t1 = y + s * 26 / 100, t2 = y + s * 18 / 100;
  d.fillRect(h1x + rx - haste, t1, haste, hy1 - t1, LILAS);
  d.fillRect(h2x + rx - haste, t2, haste, hy2 - t2, LILAS);
  for (int k = 0; k < s / 12; ++k) {
    d.drawLine(h1x + rx - haste, t1 + k, h2x + rx - 1, t2 + k, LILAS);
  }
}

extern const app_t APP_MUSICA = {
  .nome  = APP_NOME,
  .cor   = LILAS,
  .perfil = { .alto_falante = true },
  .usa_sd = true,
  // NAO exige: com o cartao no computador (modo pendrive) ainda toca o
  // pendrive do USB-A - o app mostra "Sem cartão SD" no lugar das pastas dele.
  .exige_sd = false,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_agora = periodo_agora,
  .avisos = AVISO_TRABALHO | AVISO_VOLUMES,
  .ao_aviso = ao_aviso,
  .icone = icone_musica,
  .ao_girar = ao_girar,
  .fundo_tema = true,   // o sistema limpa a area com o fundo do tema
};
