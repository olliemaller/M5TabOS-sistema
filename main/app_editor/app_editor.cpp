// APP: Editor - os scripts Berry (os apps .be) escritos no proprio aparelho.
//
// Duas telas:
//   LISTA    os .be de /sdcard/apps/Berry, e "+ Novo" (nasce com um modelo).
//   EDITOR   o editor do sistema no MODO CODIGO (kernel/editor): fonte
//            monoespacada, numero de linha, cores do Berry, recuo mantido.
//            Verificar compila sem rodar e marca a linha do erro; Rodar
//            salva, verifica e abre o app; Salvar grava; voltar grava.
//   NO PC    o editor no navegador do PC (services/berry_web): o Tab5 mostra
//            o endereco e a senha; Desligar fecha a porta. O servidor roda de
//            fundo (sair do app nao o fecha; cai sozinho sem uso). Enquanto ligado,
//            o aparelho nao edita (dois editores no mesmo arquivo, um
//            gravaria por cima do outro).
//
// So interface. O cartao e pelo berry_scripts (portas do tab5_sd), no worker
// do sistema; a verificacao e do servico do Berry (berry_verificar). So
// existe na variante com o Berry (main/CMakeLists.txt).

#include "app.h"
#include "lista.hpp"
#include "editor.hpp"
#include "idioma.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "tab5_sd.hpp"
#include "tab5_arquivos.hpp"
#include "trabalho.hpp"
#include "berry_vm.hpp"
#include "berry_apps.hpp"
#include "berry_scripts.hpp"
#include "berry_web.hpp"
#include "tab5_web.hpp"
#include "tab5_wifi.hpp"
#include "painel_web.hpp"
#include "esp_heap_caps.h"
#include <atomic>
#include <stdio.h>
#include <string.h>

static const int TOPO_A = 96, BT_A = 64, MSG_A = 44, ITEM_A = 88;
#define APP_EDITOR_NOME "Editor"   // o dono do servidor web (o mesmo de berry_web_ligar)
// O botao "No PC" da lista, a esquerda do "+ Novo".
static const int BT_PC_L = 160;
#define BT_PC_X(a) ((a).largura - 216 - 16 - BT_PC_L)
static const int TXT_MAX = BERRY_SCRIPT_MAX;   // o editor guarda o inicio das linhas em int16
static const int ITENS_MAX = 64;

typedef enum { TELA_LISTA, TELA_EDITOR, TELA_PC } tela_t;
typedef enum { FEZ_NADA, FEZ_LISTA, FEZ_ABRIR, FEZ_SALVAR, FEZ_VERIFICAR, FEZ_RODAR } fez_t;

static tela_t            s_tela = TELA_LISTA;
static tab5_arq_item_t*  s_itens = nullptr;   // PSRAM, so com o app aberto
static int               s_n = 0;             // -1 = sem cartao
static lista_t           s_lista;
static char*             s_txt = nullptr;     // PSRAM, TXT_MAX
static char              s_arq[TAB5_ARQ_NOME_MAX];
static editor_t          s_ed;
static char              s_msg[160];
static bool              s_msg_erro = false;
static bool              s_forcar = true;

// ---------------------------------------------------------------------------
// O CARTAO E O BERRY, SO NO WORKER. Um trabalho por vez; o resultado volta
// por AVISO_TRABALHO com 's_fez' dizendo o que terminou.
// ---------------------------------------------------------------------------
static bool             s_ocupado = false;
static std::atomic<int> s_fez{FEZ_NADA};
static int              s_n_res = 0;
static bool             s_res_ok = false;
static int              s_res_linha = 0;
static char             s_res_msg[160];

static void trab_listar(void*)
{
  s_n_res = berry_scripts_listar(s_itens, ITENS_MAX);
  s_fez = FEZ_LISTA;
}

static void trab_abrir(void*)
{
  const berry_ler_t l = berry_script_ler(s_arq, s_txt, TXT_MAX);
  s_res_ok = l == BERRY_LER_OK;
  s_res_linha = l;   // o motivo, para o recado
  s_fez = FEZ_ABRIR;
}

static bool salvar(void) { return berry_script_gravar(s_arq, s_txt, strlen(s_txt)); }

static bool verificar(void)
{
  s_res_linha = 0;
  s_res_msg[0] = 0;
  return berry_verificar(s_txt, strlen(s_txt), s_res_msg, sizeof(s_res_msg), &s_res_linha);
}

static void trab_salvar(void*)    { s_res_ok = salvar(); s_fez = FEZ_SALVAR; }
static void trab_verificar(void*) { s_res_ok = verificar(); s_fez = FEZ_VERIFICAR; }
static void trab_rodar(void*)
{
  s_res_ok = salvar();
  if (!s_res_ok) { snprintf(s_res_msg, sizeof(s_res_msg), "%s", tr("não salvou (cartão?)")); s_fez = FEZ_RODAR; return; }
  s_res_ok = verificar();
  // Script novo ou nome trocado: a pasta e relida (o icone aparece na home).
  if (s_res_ok) { berry_apps_reler(); }
  s_fez = FEZ_RODAR;
}

static void pedir(trabalho_t fn)
{
  if (s_ocupado) { return; }
  if (sistema_trabalho(fn, nullptr)) { s_ocupado = true; sistema_pedir_redesenho(); }
}

static void mensagem(const char* t, bool erro)
{
  snprintf(s_msg, sizeof(s_msg), "%s", t);
  s_msg_erro = erro;
  s_forcar = true;
  sistema_pedir_redesenho();
}

static void trocar_tela(tela_t t)
{
  s_tela = t;
  s_forcar = true;
  sistema_pedir_redesenho_completo();
}

// ---------------------------------------------------------------------------
// LISTA
// ---------------------------------------------------------------------------
static int item_altura(int, void*) { return ITEM_A; }

static void item_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int larg, void*)
{
  if (i >= s_n || s_ocupado) { return; }
  d.fillRect(x, y, larg, ITEM_A, TEMA_FUNDO);
  tab5_fonte_corpo_negrito(d);
  tab5_texto(d, 3.0f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  char nome[TAB5_ARQ_NOME_MAX];
  snprintf(nome, sizeof(nome), "%s", s_itens[i].nome);
  tab5_cortar_com_reticencias(d, nome, sizeof(nome), larg - 200);
  d.drawString(nome, x + 24, y + ITEM_A / 2);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.2f);
  d.setTextDatum(middle_right);
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  char t[24];
  tab5_bytes_txt(t, sizeof(t), s_itens[i].bytes);
  d.drawString(t, x + larg - 24, y + ITEM_A / 2);
  d.drawFastHLine(x + 16, y + ITEM_A - 1, larg - 32, TEMA_LINHA);
  d.setTextDatum(top_left);
}

static void desenhar_lista(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (tudo) {
    tab5_tela_limpar(a.x, a.y, a.largura, TOPO_A, TEMA_FUNDO);
    tab5_fonte_corpo(d);
    tab5_texto(d, 2.6f);
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    char n[48];
    if (s_ocupado)     { snprintf(n, sizeof(n), "%s", tr("lendo...")); }
    else if (s_n < 0)  { snprintf(n, sizeof(n), "%s", tr("sem cartão")); }
    else               { snprintf(n, sizeof(n), tr("%d scripts em apps/Berry"), s_n); }
    tab5_cortar_com_reticencias(d, n, sizeof(n), BT_PC_X(a) - 40);
    d.drawString(n, a.x + 24, a.y + TOPO_A / 2);
    if (s_n >= 0 && !s_ocupado) {
      tab5_botao(d, a.x + a.largura - 216, a.y + (TOPO_A - BT_A) / 2, 200, BT_A, tr("+ Novo"), TEMA_SUCESSO);
      tab5_botao(d, a.x + BT_PC_X(a), a.y + (TOPO_A - BT_A) / 2, BT_PC_L, BT_A, tr("No PC"), TEMA_DESTAQUE);
    }
  }
  if (s_ocupado) { return; }
  const area_t janela = { a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A };
  lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, tudo);
  if (tudo && s_n == 0) {
    tab5_texto(d, 2.6f);
    d.setTextDatum(middle_center);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr("nenhum script ainda - toque em + Novo"), a.x + a.largura / 2, janela.y + janela.altura / 3);
    d.setTextDatum(top_left);
  }
}

static const char MODELO[] =
  "# nome: %s\n"
  "# cor: 0x07FF\n"
  "#\n"
  "# Code outside the functions runs once, when the app opens.\n"
  "# Functions: docs/BERRY_API.md\n"
  "\n"
  "var taps = 0\n"
  "\n"
  "def desenhar(tudo)\n"
  "  tela.texto(\"Hello!\", 40, 40, 40)\n"
  "  tela.texto(\"taps: \" + str(taps), 40, 120)\n"
  "end\n"
  "\n"
  "def tocar(x, y)\n"
  "  taps += 1\n"
  "  sistema.redesenhar()\n"
  "end\n";

static void abrir_editor(void)
{
  if (!editor_abrir(&s_ed, s_txt, TXT_MAX)) { mensagem(tr("sem memória"), true); return; }
  editor_modo_codigo(&s_ed);
  s_ed.cursor = 0;
  s_msg[0] = 0;
  trocar_tela(TELA_EDITOR);
}

static void script_novo(void)
{
  // O primeiro nome livre: novo_1.be, novo_2.be...
  for (int k = 1; k < 1000; ++k) {
    snprintf(s_arq, sizeof(s_arq), "novo_%d.be", k);
    bool existe = false;
    for (int i = 0; i < s_n && !existe; ++i) { existe = !strcasecmp(s_itens[i].nome, s_arq); }
    if (!existe) { break; }
  }
  char nome[32];
  snprintf(nome, sizeof(nome), "%s", s_arq);
  *strrchr(nome, '.') = 0;
  snprintf(s_txt, TXT_MAX, MODELO, nome);
  abrir_editor();
  s_ed.mudou = true;   // nasce para ser salvo
}

// ---------------------------------------------------------------------------
// NO PC - o editor no navegador (services/berry_web)
// ---------------------------------------------------------------------------
// "No PC" da lista: a tela do painel. Ligado por este app, ja esta no ar;
// desligado ou de outro app, o painel oferece o botao.
static bool s_pc_ligado = false;   // o servidor estava no ar para o Editor (tela do PC)
static bool web_meu(void)
{
  return tab5_web_aberto(APP_EDITOR_NOME);
}

static void ligar_pc(void)
{
  if (!web_meu() && tab5_wifi_estado() == TAB5_WIFI_CONECTADO && !berry_web_ligar()) {
    sistema_recado(tr("O servidor não ligou"));
  }
  s_pc_ligado = web_meu();
  trocar_tela(TELA_PC);
}

// A tela do PC: o titulo e o painel do sistema (kernel/painel_web), que
// mostra endereco, senha e o Desligar.
static void desenhar_pc(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (!tudo) { return; }
  tab5_tela_limpar(a.x, a.y, a.largura, TOPO_A, TEMA_FUNDO);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(tr("Editor no PC"), a.x + 24, a.y + TOPO_A / 2);
  d.setTextDatum(top_left);
  painel_web_desenhar(d, a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A, APP_EDITOR_NOME);
}

static void voltar_da_web(void)
{
  trocar_tela(TELA_LISTA);
  pedir(trab_listar);   // o PC pode ter criado ou mudado scripts
}

static void tocar_pc(int x, int y)
{
  // Desligar: a volta a lista vem pelo AVISO_WEB.
  const area_t a = sistema_area();
  painel_web_tocar(0, TOPO_A, a.largura, a.altura - TOPO_A, x, y, APP_EDITOR_NOME, berry_web_ligar);
}

static void tocar_lista(int x, int y)
{
  if (s_ocupado) { return; }
  const area_t a = sistema_area();
  if (y < TOPO_A) {
    if (s_n >= 0 && x >= a.largura - 216) { script_novo(); }
    else if (s_n >= 0 && x >= BT_PC_X(a) && x < BT_PC_X(a) + BT_PC_L) { ligar_pc(); }
    return;
  }
  const int i = lista_item_tocado(&s_lista, x, y, nullptr);
  if (i >= 0 && i < s_n) {
    snprintf(s_arq, sizeof(s_arq), "%s", s_itens[i].nome);
    pedir(trab_abrir);
  }
}

// ---------------------------------------------------------------------------
// EDITOR
// ---------------------------------------------------------------------------
// Os botoes do alto: < Scripts a esquerda; Salvar, Rodar e Verificar a direita.
static const int BT_VOLTAR_L = 180, BT_L = 160;
static int bt_x(const area_t& a, int k) { return a.largura - 16 - (k + 1) * BT_L - k * 12; }   // 0 = o mais a direita

static void desenhar_editor(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (tudo) {
    tab5_tela_limpar(a.x, a.y, a.largura, TOPO_A + MSG_A, TEMA_FUNDO);
    const int by = a.y + (TOPO_A - BT_A) / 2;
    tab5_botao(d, a.x + 16, by, BT_VOLTAR_L, BT_A, tr("< Scripts"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    tab5_botao(d, a.x + bt_x(a, 0), by, BT_L, BT_A, tr("Salvar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    tab5_botao(d, a.x + bt_x(a, 1), by, BT_L, BT_A, tr("Rodar"), TEMA_SUCESSO);
    tab5_botao(d, a.x + bt_x(a, 2), by, BT_L, BT_A, tr("Verificar"), TEMA_DESTAQUE);
    // O nome do arquivo (e um ponto se ha mudanca sem salvar) e a mensagem.
    tab5_fonte_corpo(d);
    tab5_texto(d, 2.4f);
    d.setTextDatum(middle_left);
    char t[TAB5_ARQ_NOME_MAX + 8];
    snprintf(t, sizeof(t), "%s%s", s_arq, s_ed.mudou ? "  *" : "");
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    const int tx = a.x + 16 + BT_VOLTAR_L + 16, tl = a.x + bt_x(a, 2) - 16 - tx;
    if (tl > 40) { tab5_cortar_com_reticencias(d, t, sizeof(t), tl); d.drawString(t, tx, a.y + TOPO_A / 2); }
    char m[sizeof(s_msg)];
    snprintf(m, sizeof(m), "%s", s_ocupado ? tr("trabalhando...") : s_msg);
    d.setTextColor(s_msg_erro ? (uint16_t)TEMA_ALERTA : (uint16_t)TEMA_SUCESSO, TEMA_FUNDO);
    tab5_cortar_com_reticencias(d, m, sizeof(m), a.largura - 32);
    d.drawString(m, a.x + 16, a.y + TOPO_A + MSG_A / 2);
    d.drawFastHLine(a.x, a.y + TOPO_A + MSG_A - 1, a.largura, TEMA_LINHA);
    d.setTextDatum(top_left);
  }
  const area_t r = { a.x, a.y + TOPO_A + MSG_A, a.largura, a.altura - TOPO_A - MSG_A };
  editor_desenhar(&s_ed, d, r, tudo);
}

static void voltar_a_lista(void)
{
  editor_fechar(&s_ed);
  trocar_tela(TELA_LISTA);
  pedir(s_ed.mudou ? (trabalho_t)[](void*) { salvar(); trab_listar(nullptr); } : trab_listar);
}

static void tocar_editor(int x, int y)
{
  const area_t a = sistema_area();
  if (y < TOPO_A && !tecvirt_contem(&s_ed.kb, a.x + x, a.y + y)) {
    if (s_ocupado) { return; }
    if (x < 16 + BT_VOLTAR_L)                          { voltar_a_lista(); }
    else if (x >= bt_x(a, 0))                          { pedir(trab_salvar); }
    else if (x >= bt_x(a, 1) && x < bt_x(a, 1) + BT_L) { pedir(trab_rodar); }
    else if (x >= bt_x(a, 2) && x < bt_x(a, 2) + BT_L) { pedir(trab_verificar); }
    return;
  }
  if (y < TOPO_A + MSG_A) { return; }
  editor_tocar(&s_ed, x, y);
}

// O resultado da verificacao: a mensagem e a linha do erro marcada.
static void mostrar_verificacao(bool ok, const char* pronto)
{
  if (ok) { editor_marcar_erro(&s_ed, 0); mensagem(pronto, false); return; }
  char m[sizeof(s_msg)];
  if (s_res_linha > 0) { snprintf(m, sizeof(m), tr("linha %d: %s"), s_res_linha, s_res_msg); }
  else                 { snprintf(m, sizeof(m), "%s", s_res_msg); }
  mensagem(m, true);
  editor_marcar_erro(&s_ed, s_res_linha);
}

// ---------------------------------------------------------------------------
// SISTEMA
// ---------------------------------------------------------------------------
static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const bool tudo = sistema_area_foi_limpa() || s_forcar;
  s_forcar = false;
  if (s_tela == TELA_EDITOR)  { desenhar_editor(d, a, tudo); }
  else if (s_tela == TELA_PC) { desenhar_pc(d, a, tudo); }
  else                        { desenhar_lista(d, a, tudo); }
  tab5_fonte_corpo(d);
}

static void ao_aviso(uint32_t avisos)
{
  if ((avisos & AVISO_WEB) && s_tela == TELA_PC) {
    // Estava no ar para o Editor e caiu (Desligar, sem uso): de volta a lista -
    // o PC pode ter mudado scripts. Sem ter ligado (sem Wi-Fi), o painel fica.
    const bool meu = web_meu();
    if (s_pc_ligado && !meu) { s_pc_ligado = false; voltar_da_web(); return; }
    s_pc_ligado = meu;
    s_forcar = true;
    sistema_pedir_redesenho();
  }
  if (!(avisos & AVISO_TRABALHO) || !s_ocupado) { return; }
  s_ocupado = false;
  const int fez = s_fez.exchange(FEZ_NADA);
  if (fez == FEZ_LISTA) {
    s_n = s_n_res;
    lista_itens_mudaram(&s_lista, s_n > 0 ? s_n : 0);
  } else if (fez == FEZ_ABRIR) {
    if (s_res_ok) { abrir_editor(); return; }
    sistema_recado(s_res_linha == BERRY_LER_GRANDE ? tr("Grande demais para editar (mais de 32 KB)") : tr("Não abriu"));
  } else if (fez == FEZ_SALVAR) {
    if (s_res_ok) { s_ed.mudou = false; }
    mensagem(s_res_ok ? tr("salvo") : tr("não salvou (cartão?)"), !s_res_ok);
  } else if (fez == FEZ_VERIFICAR) {
    mostrar_verificacao(s_res_ok, tr("verificado: sem erros de sintaxe"));
  } else if (fez == FEZ_RODAR) {
    s_ed.mudou = false;
    if (!s_res_ok) { mostrar_verificacao(false, ""); return; }
    // Salvo e sem erro: abre o app. Cada abertura le o .be de novo - o que
    // roda e o que esta aqui. Script novo so ganha app depois da releitura,
    // que a tela inicial aplica.
    char nome[48];
    berry_script_nome_app(s_txt, s_arq, nome, sizeof(nome));
    if (!sistema_abrir_app(nome)) {
      mensagem(tr("salvo - o app novo aparece na tela inicial (toque nele para rodar)"), false);
    }
    return;
  }
  s_forcar = true;
  sistema_pedir_redesenho_completo();
}

static void ao_tocar(int x, int y)
{
  if (s_tela == TELA_EDITOR)  { tocar_editor(x, y); }
  else if (s_tela == TELA_PC) { tocar_pc(x, y); }
  else                        { tocar_lista(x, y); }
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (s_tela == TELA_EDITOR) { editor_tocar_nivel(&s_ed, x, y, pressionado); return; }
  if (s_tela == TELA_PC) { return; }
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  if (s_tela != TELA_EDITOR) { return; }
  // Atalhos de desktop: Ctrl+S salva, Ctrl+R roda; Esc volta.
  if ((mod & TAB5_TECLA_CTRL) && texto && !s_ocupado) {
    const char c = (char)(texto[0] | 0x20);
    if (c == 's') { pedir(trab_salvar); return; }
    if (c == 'r') { pedir(trab_rodar); return; }
  }
  if (codigo == TAB5_TECLA_ESC) { voltar_a_lista(); return; }
  editor_teclar(&s_ed, texto, codigo, mod);
}

static bool ao_abrir(void)
{
  s_itens = (tab5_arq_item_t*)heap_caps_calloc(ITENS_MAX, sizeof(tab5_arq_item_t), MALLOC_CAP_SPIRAM);
  s_txt = (char*)heap_caps_malloc(TXT_MAX, MALLOC_CAP_SPIRAM);
  if (!s_itens || !s_txt) { heap_caps_free(s_itens); heap_caps_free(s_txt); s_itens = nullptr; s_txt = nullptr; return false; }
  s_tela = TELA_LISTA;
  s_n = 0;
  s_ocupado = false;
  s_msg[0] = 0;
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, item_altura, item_desenhar, nullptr);
  // Reaberto com o servidor ligado (o aviso da barra, ou depois de um Rodar
  // do navegador): direto na tela do PC, onde esta o Desligar.
  if (web_meu()) { s_tela = TELA_PC; s_pc_ligado = true; return true; }
  pedir(trab_listar);
  return true;
}

// GRAVAR AO FECHAR: trabalho SOLTO com uma copia (nome + texto) - o app
// libera s_txt logo em seguida, antes de o cartao terminar.
typedef struct { char arq[TAB5_ARQ_NOME_MAX]; char txt[]; } copia_t;
static void trab_salvar_copia(void* a)
{
  copia_t* c = (copia_t*)a;
  if (tab5_sd_montar()) {
    tab5_sd_substituir(BERRY_PASTA, c->arq, c->txt, strlen(c->txt));
    tab5_sd_desmontar();
  }
  heap_caps_free(c);
}

static void ao_fechar(void)
{
  if (s_tela == TELA_EDITOR && s_ed.mudou) {
    const size_t n = strlen(s_txt);
    copia_t* c = (copia_t*)heap_caps_malloc(sizeof(copia_t) + n + 1, MALLOC_CAP_SPIRAM);
    if (c) {
      snprintf(c->arq, sizeof(c->arq), "%s", s_arq);
      memcpy(c->txt, s_txt, n + 1);
      if (!sistema_trabalho_solto(trab_salvar_copia, c)) { heap_caps_free(c); }
    }
  }
  if (s_tela == TELA_EDITOR) { editor_fechar(&s_ed); }
  s_tela = TELA_LISTA;
  heap_caps_free(s_itens); s_itens = nullptr;
  heap_caps_free(s_txt);   s_txt = nullptr;
}

static void ao_girar(void) { s_forcar = true; }

static void icone_editor(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(24, 30, 48));
  const int m = lado / 6, lh = lado / 9;
  // Linhas de codigo coloridas, com recuo, e o cursor.
  const uint16_t c[4] = { d.color565(100, 180, 255), d.color565(225, 225, 225), d.color565(230, 170, 100),
                          d.color565(100, 180, 255) };
  const int rec[4] = { 0, 1, 1, 0 }, comp[4] = { 50, 55, 40, 30 };
  for (int k = 0; k < 4; ++k) {
    const int ly = y + m + k * (lh + lado / 14) + lh;
    d.fillRoundRect(x + m + rec[k] * lado / 8, ly, (lado - 2 * m) * comp[k] / 70, lh, lh / 2, c[k]);
  }
  d.fillRect(x + lado - m - lado / 10, y + lado - m - lh * 2, lado / 22 + 2, lh * 2, TFT_GREENYELLOW);
}

extern const app_t APP_EDITOR = {
  .nome  = "Editor",
  .cor   = TFT_SKYBLUE,
  .perfil = { },
  .usa_sd = false,     // o worker monta o cartao quando precisa
  .exige_sd = false,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .avisos = AVISO_TRABALHO | AVISO_WEB,
  .ao_aviso = ao_aviso,
  .icone = icone_editor,
  .ao_girar = ao_girar,
  .comandos = nullptr,
  .fora_da_home = false,
  .no_pc = berry_web_ligar,
  .fundo_tema = true,   // o sistema limpa a area com TEMA_FUNDO
};
