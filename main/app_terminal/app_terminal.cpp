// APP: Terminal - the system commands on screen, with a keyboard.
//
// Interface only. The session (the lines, the queue, the current folder, the command
// running) belongs to the service (services/tab5_terminal), and the commands are the
// console's (console.cpp): closing the app does not stop what is running, and coming back shows
// everything where it was.
//
//   top       the output, scrolling (newest at the bottom, like every terminal)
//   bottom    the command line: the prompt with the current folder and what is typed
//
// Physical keyboard (ExtPort1): Enter runs, up/down arrows bring back what was
// typed before, Esc clears the line, Ctrl+C stops the command, Ctrl+L clears,
// Ctrl+V pastes. Without it: tapping the line opens the virtual one.
//
// ON THE PC: the button at the end of the command line opens the system panel
// (kernel/painel_web) - turning on browser access, the address and the password.
// Green = the Terminal is already open on the PC. The 'web' command shows the same.
//
// SSH IS A COMMAND: 'ssh user@host' opens the SSH session shared with
// the PC browser, and the app switches BY ITSELF to its screen (terminal_ssh.cpp)
// while it is up; 'exit' (or Encerrar - end) goes back to the commands, with the
// reason in the bar. The same rule applies on the browser's /terminal/ page.

#include "app.h"
#include "idioma.h"
#include "lista.hpp"
#include "teclado_virtual.hpp"
#include "tab5_terminal.hpp"
#include "transferencia.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "tab5_ssh.hpp"
#include "tab5_terminal_web.hpp"
#include "tab5_web.hpp"
#include "painel_web.hpp"
#include "terminal_ssh.hpp"
#include <stdio.h>
#include <string.h>

static const int MX    = 12;    // text margin
static const int BAR_A = 64;    // the command line
static const int PARAR_L = 120; // the "Parar" (stop) button on the line, with a command running
static const int PC_L  = 120;   // the "No PC" (on PC) button at the end of the line
static const int TOPO_PC = 64;  // the On PC screen: the "< Voltar" (back) strip
// The output background stays BLACK (and not TEMA_FUNDO), on purpose: it is a terminal,
// and the text colours (and the SSH session's ANSI ones) were made for a black background.
// That is why this app does NOT turn on .fundo_tema. Only the "frame" (bar, buttons) uses the theme.
static const uint16_t FUNDO_TERM = TFT_BLACK;

static lista_t   s_lista;
static tecvirt_t s_kb;
static char      s_txt[TERM_ENTRADA] = "";
static int       s_hist = -1;          // -1 = editing the new line; k = the k-th earlier one
static uint32_t  s_versao = 0xFFFFFFFF;
static bool      s_ocupado = false;
static bool      s_forcar = true;
static bool      s_ssh = false;        // the SSH screen in front (the session is up)
static bool      s_pc = false;         // the On PC screen in front (the web server panel)
static uint32_t  s_pc_assin = 0;       // what the panel showed (painel_web_assinatura)

#define DONO_WEB "Terminal"   // the web server part (the same as in tab5_terminal_web_ligar)

// The monospaced font grid, measured when drawing (changes on rotation).
static int s_cw = 14, s_lh = 26, s_cols = 48;

// From the top edge of the row to the FreeMono12pt7b baseline.
static const int BASE = 19;

static void fonte(lgfx::LovyanGFX& d) { d.setFont(&fonts::FreeMono12pt7b); d.setTextSize(1); }

// ---------------------------------------------------------------------------
// THE OUTPUT (kernel/lista: scrolls, clips and draws only what changed)
// ---------------------------------------------------------------------------
static int item_altura(int i, void*)
{
  const int n = tab5_terminal_tamanho(i);
  const int linhas = n <= 0 ? 1 : (n + s_cols - 1) / s_cols;
  return linhas * s_lh;
}

static void item_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int, void*)
{
  char t[TERM_LINHA_MAX + 1];
  term_tipo_t tipo = TERM_SAIDA;
  const int n = tab5_terminal_linha(i, t, sizeof(t), &tipo);
  if (n <= 0) { return; }
  fonte(d);
  d.setTextDatum(top_left);
  d.setTextColor(tipo == TERM_COMANDO ? (uint16_t)TFT_GREENYELLOW
               : tipo == TERM_AVISO   ? (uint16_t)TFT_ORANGE : d.color565(210, 210, 210));
  // Wraps every s_cols characters; accents go in the body font
  // (tab5_mono). The item height counts bytes: a long line with accents
  // may gain an empty row at the end - that is all.
  const char* p = t;
  for (int r = 0; *p; ++r) { p = tab5_mono(d, p, x + MX, y + r * s_lh + BASE, s_cw, s_cols); }
}

// ---------------------------------------------------------------------------
// GEOMETRY
// ---------------------------------------------------------------------------
static int kb_altura(const area_t& a) { return tecvirt_aberto(&s_kb) ? tecvirt_altura(a) : 0; }
static int bar_y(const area_t& a)     { return a.altura - kb_altura(a) - BAR_A; }   // relative to the area

// ---------------------------------------------------------------------------
// THE COMMAND LINE
// ---------------------------------------------------------------------------
static void desenhar_barra(lgfx::LovyanGFX& d, const area_t& a)
{
  const int y = a.y + bar_y(a);
  d.fillRect(a.x, y, a.largura, BAR_A, TEMA_SUPERFICIE);
  d.drawFastHLine(a.x, y, a.largura, TEMA_LINHA);
  fonte(d);
  d.setTextDatum(middle_left);
  char p[TERM_ENTRADA + 180];
  tab5_terminal_prompt(p, sizeof(p));
  bool oculto = false;
  if (tab5_terminal_lendo(&oculto) && oculto) {
    // The password a command asked for: asterisks only, one per character.
    for (const char* c = s_txt; *c; ++c) { if ((*c & 0xC0) != 0x80) { strlcat(p, "*", sizeof(p)); } }
  } else {
    strlcat(p, s_txt, sizeof(p));
  }
  strlcat(p, "_", sizeof(p));
  // Only the end, if it does not fit (what is typed always stays in view).
  const int livre = (a.largura - 3 * MX - PC_L - (s_ocupado ? PARAR_L + MX : 0)) / s_cw;
  const int n = (int)strlen(p);
  const char* mostra = n > livre && livre > 0 ? p + (n - livre) : p;
  while ((*mostra & 0xC0) == 0x80) { ++mostra; }   // do not start in the middle of an accent
  d.setTextColor(TEMA_TEXTO);
  tab5_mono(d, mostra, a.x + MX, y + BAR_A / 2 + BASE - s_lh / 2, s_cw, livre > 0 ? livre : 1);
  const bool no_pc = tab5_web_aberto(DONO_WEB);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.0f);
  tab5_botao(d, a.x + a.largura - MX - PC_L, y + 10, PC_L, BAR_A - 20, tr("No PC"),
             no_pc ? (uint16_t)TEMA_DESTAQUE : (uint16_t)TEMA_SUPERFICIE_ALTA, no_pc ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO);
  if (s_ocupado) {
    tab5_botao(d, a.x + a.largura - 2 * MX - PC_L - PARAR_L, y + 10, PARAR_L, BAR_A - 20, tr("Parar"),
               TEMA_PERIGO, TEMA_SOBRE_DESTAQUE);
  }
  fonte(d);
}

// ---------------------------------------------------------------------------
// THE ON PC SCREEN (the system panel, with "< Voltar" at the top)
// ---------------------------------------------------------------------------
static void pc_abrir(bool sim)
{
  s_pc = sim;
  if (sim) { tecvirt_fechar(&s_kb); }
  s_forcar = true;
  s_versao = 0xFFFFFFFF;
  sistema_pedir_redesenho_completo();
}

static void desenhar_pc(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  const uint32_t assin = painel_web_assinatura();
  if (!tudo && assin == s_pc_assin) { return; }
  s_pc_assin = assin;
  tab5_tela_limpar(a.x, a.y, a.largura, TOPO_PC, TEMA_SUPERFICIE);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.0f);
  tab5_botao(d, a.x + 8, a.y + 8, 140, TOPO_PC - 16, tr("< Voltar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  painel_web_desenhar(d, a.x, a.y + TOPO_PC, a.largura, a.altura - TOPO_PC, DONO_WEB);
}

static void tocar_pc(int x, int y)
{
  const area_t a = sistema_area();
  if (y < TOPO_PC) { if (x < 160) { pc_abrir(false); } return; }
  painel_web_tocar(0, TOPO_PC, a.largura, a.altura - TOPO_PC, x, y, DONO_WEB, tab5_terminal_web_ligar);
}

static void editar(int acao)
{
  s_hist = -1;
  tecvirt_editar(s_txt, sizeof(s_txt), acao);
  sistema_pedir_redesenho();
}

static void rodar(void)
{
  tab5_terminal_enviar(s_txt);
  s_txt[0] = 0;
  s_hist = -1;
  lista_ao_inicio(&s_lista);   // the anchor is the END: back to the newest line
  sistema_pedir_redesenho();
}

static void historico(int passo)
{
  const int k = s_hist + passo;
  if (k < 0) { s_hist = -1; s_txt[0] = 0; }
  else if (tab5_terminal_historico(k, s_txt, sizeof(s_txt))) { s_hist = k; }
  sistema_pedir_redesenho();
}

static void colar(void)
{
  char t[TERM_ENTRADA];
  const size_t n = transf_colar(t, sizeof(t));
  for (size_t i = 0; i < n; ++i) { if (t[i] == '\n' || t[i] == '\r' || t[i] == '\t') { t[i] = ' '; } }
  strlcat(s_txt, t, sizeof(s_txt));
  s_hist = -1;
  sistema_pedir_redesenho();
}

static void acao_teclado(int ac)
{
  if (ac == TECVIRT_LINHA || ac == TECVIRT_ENVIAR) { rodar(); }   // COMMAND: the green key and Enter
  else if (ac == TECVIRT_FECHAR) { tecvirt_fechar(&s_kb); s_forcar = true; sistema_pedir_redesenho_completo(); }
  else                           { editar(ac); }
}

// ---------------------------------------------------------------------------
// THE SCREEN FOLLOWS THE SSH SESSION
// ---------------------------------------------------------------------------
// Up: its screen. Ended: the commands back, with the reason in the bar
// (the session is left FREE for the next 'ssh').
static void seguir_ssh(void)
{
  tab5_ssh_sessao_info_t i;
  tab5_ssh_sessao_info(&i);
  const bool ssh = i.estado >= TAB5_SSH_CONECTANDO && i.estado <= TAB5_SSH_ABERTA;
  if (ssh == s_ssh) { return; }
  if (ssh) {
    tecvirt_fechar(&s_kb);
    s_pc = false;   // the session coming up (here or on the PC) comes to the front
    ssh_tela_abrir();
  } else {
    ssh_tela_fechar();
    if (i.estado == TAB5_SSH_FIM) {
      char m[128];
      snprintf(m, sizeof(m), "ssh %s@%s: %s", i.usuario, i.host, i.msg[0] ? i.msg : tr("fim"));
      sistema_recado(m);
      tab5_ssh_sessao_esquecer_fim();
    }
  }
  s_ssh = ssh;
  s_forcar = true;
  s_versao = 0xFFFFFFFF;
  sistema_pedir_redesenho_completo();
}

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  seguir_ssh();
  const bool tudo = sistema_area_foi_limpa() || s_forcar;
  s_forcar = false;
  if (s_ssh) { ssh_tela_desenhar(d, a, tudo); return; }
  if (s_pc)  { desenhar_pc(d, a, tudo); return; }
  s_ocupado = tab5_terminal_ocupado();

  fonte(d);
  s_cw = d.textWidth("M");
  s_lh = d.fontHeight() + 2;
  if (s_cw <= 0) { s_cw = 14; }
  const int cols = (a.largura - 2 * MX - 8) / s_cw;   // 8: the scroll bar column
  if (cols != s_cols) { s_cols = cols > 8 ? cols : 8; s_versao = 0xFFFFFFFF; }

  const uint32_t v = tab5_terminal_versao();
  if (v != s_versao) { s_versao = v; lista_itens_mudaram(&s_lista, tab5_terminal_linhas()); }

  const area_t janela = { a.x, a.y, a.largura, bar_y(a) };
  lista_desenhar(&s_lista, d, janela, FUNDO_TERM, tudo);
  desenhar_barra(d, a);   // cheap: always (the typed text, the Parar button)

  tecvirt_posicionar(&s_kb, a);
  tecvirt_pintar(&s_kb, d, tudo);
  tab5_fonte_corpo(d);
}

static uint32_t periodo_agora(void)
{
  // Running: output arrives at any time (10 frames/s is enough to read).
  // Just stopped or text left over: one more frame (the Parar button goes).
  // Stopped: only when someone taps or types.
  const bool o = tab5_terminal_ocupado();
  return (o || o != s_ocupado || tab5_terminal_versao() != s_versao) ? 100 : 0;
}

// ---------------------------------------------------------------------------
// TOUCH AND KEYBOARD
// ---------------------------------------------------------------------------
static void ao_tocar(int x, int y)
{
  if (s_ssh) { ssh_tela_tocar(x, y); return; }
  if (s_pc)  { tocar_pc(x, y); return; }
  const area_t a = sistema_area();
  if (tecvirt_contem(&s_kb, a.x + x, a.y + y)) {
    const int ac = tecvirt_clique(&s_kb, a.x + x, a.y + y);
    if (ac) { acao_teclado(ac); }
    return;
  }
  const int by = bar_y(a);
  if (y >= by && y < by + BAR_A) {
    if (x >= a.largura - MX - PC_L) { pc_abrir(true); return; }
    if (s_ocupado && x >= a.largura - 2 * MX - PC_L - PARAR_L) { tab5_terminal_interromper(); return; }
    if (!tecvirt_aberto(&s_kb)) { tecvirt_abrir(&s_kb, TECVIRT_COMANDO); s_forcar = true; sistema_pedir_redesenho_completo(); }
    return;
  }
  lista_item_tocado(&s_lista, x, y, nullptr);   // closes the drag; the output has no button
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (s_ssh) { ssh_tela_tocar_nivel(x, y, pressionado); return; }
  if (s_pc)  { return; }
  if (tecvirt_aberto(&s_kb) && tecvirt_altura(sistema_area()) > 0) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb, acoes, 8);
    for (int i = 0; i < n; ++i) { acao_teclado(acoes[i]); }
  }
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_teclar(const char* txt, uint8_t codigo, uint8_t mod)
{
  if (s_ssh) { ssh_tela_teclar(txt, codigo, mod); return; }
  if (s_pc)  { if (codigo == TAB5_TECLA_ESC) { pc_abrir(false); } return; }
  if (mod & TAB5_TECLA_CTRL) {
    const char c = txt ? (char)(txt[0] | 0x20) : 0;   // lower case
    if (c == 'c') { tab5_terminal_interromper(); }
    else if (c == 'l') { tab5_terminal_limpar(); }
    else if (c == 'v') { colar(); }
    sistema_pedir_redesenho();
    return;
  }
  switch (codigo) {
    case TAB5_TECLA_ENTER:  rodar(); return;
    case TAB5_TECLA_APAGAR: editar(TECVIRT_APAGAR); return;
    case TAB5_TECLA_ESC:    s_txt[0] = 0; s_hist = -1; sistema_pedir_redesenho(); return;
    case TAB5_TECLA_CIMA:   historico(+1); return;
    case TAB5_TECLA_BAIXO:  historico(-1); return;
    default: break;
  }
  if (codigo || (mod & TAB5_TECLA_ALT) || !txt || !txt[0]) { return; }
  if (strlen(s_txt) + strlen(txt) < sizeof(s_txt)) {
    for (const char* p = txt; *p; ++p) { if ((unsigned char)*p < 0x20) { return; } }
    strlcat(s_txt, txt, sizeof(s_txt));
    s_hist = -1;
    sistema_pedir_redesenho();
  }
}

// ---------------------------------------------------------------------------
// LIFE CYCLE
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  if (!tab5_terminal_abrir()) { return false; }
  lista_definir(&s_lista, ROLAGEM_FIM, tab5_terminal_linhas(), item_altura, item_desenhar, nullptr);
  s_versao = 0xFFFFFFFF;
  s_hist = -1;
  s_forcar = true;
  // SSH session on air (opened here or on the PC): the app opens on it.
  s_ssh = false;
  s_pc = false;
  seguir_ssh();
  return true;
}

static void ao_aviso(uint32_t)
{
  seguir_ssh();   // the session came up or ended (here, on the PC or via 'ssh fechar')
  if (s_ssh) { ssh_tela_aviso(); } else { sistema_pedir_redesenho(); }
}

static void ao_fechar(void)
{
  // The session continues (the running command too, and SSH): only the screen goes.
  tecvirt_fechar(&s_kb);
  if (s_ssh) { ssh_tela_fechar(); }
}

// The grid changes width: so do the line heights.
static void ao_girar(void)
{
  s_versao = 0xFFFFFFFF;
  s_forcar = true;
}

static void icone_terminal(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(20, 24, 22));
  d.drawRoundRect(x, y, lado, lado, lado / 6, d.color565(60, 80, 64));
  const uint16_t c = TFT_GREENYELLOW;
  const int m = lado / 4, cy = y + lado / 2, t = lado / 14 + 1;
  // ">" and "_"
  for (int k = 0; k < t; ++k) {
    d.drawLine(x + m, cy - lado / 6 + k, x + m + lado / 6, cy + k, c);
    d.drawLine(x + m + lado / 6, cy + k, x + m, cy + lado / 6 + k, c);
  }
  d.fillRect(x + m + lado / 4, cy + lado / 6 - t + 1, lado / 4, t, c);
}

extern const app_t APP_TERMINAL = {
  .nome  = "Terminal",
  .cor   = TFT_GREENYELLOW,
  .perfil = { },
  .usa_sd = false,     // whoever needs the SD card is the command, and it mounts it (console)
  .exige_sd = false,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_agora = periodo_agora,
  .avisos = AVISO_SSH | AVISO_WEB,
  .ao_aviso = ao_aviso,
  .icone = icone_terminal,
  .ao_girar = ao_girar,
  .comandos = nullptr,
  .fora_da_home = false,
  .no_pc = tab5_terminal_web_ligar,
};
