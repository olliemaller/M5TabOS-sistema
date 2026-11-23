// The Terminal's SSH screen - see terminal_ssh.hpp.
//
//   top       the session (user@server), "Encerrar" (end) and "No PC" (on PC)
//   middle    depending on the state: the new server's fingerprint, the
//             password - or the terminal GRID (colours, cursor), with the row
//             of extra keys (Esc, Tab, Ctrl, arrows...) as in Termius
//   bottom    the virtual keyboard (the green key is the new-line one: Enter)
//
// The SIZE is that of whoever typed last (as in tmux): when typing here,
// the session takes the columns and rows that fit this screen.
#include "terminal_ssh.hpp"
#include "teclado_virtual.hpp"
#include "painel_web.hpp"
#include "idioma.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "tab5_hora.hpp"
#include "tab5_ssh.hpp"
#include "tab5_terminal_web.hpp"
#include "tab5_web.hpp"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DONO_WEB "Terminal"   // the web server part (the same as in tab5_terminal_web_ligar)

static const int TOPO_A = 56;    // the status line
static const int TECLAS_A = 56;  // the row of extra keys
static const int MX = 8;
static const int MAX_COLS = 240, MAX_LINHAS = 80;
// The grid and the connection screens sit on BLACK, not TEMA_FUNDO: the session's ANSI
// colours (server data) assume a black background, and "default background"
// arrives from the service as 0x0000 (no constant). That is why the app does not turn on .fundo_tema.
static const uint16_t FUNDO_TERM = TFT_BLACK;

static tecvirt_t s_kb;
static area_t    s_a;                    // the tab (screen) area, from the last drawing
static bool      s_pc = false;           // the "No PC" screen (web server panel)
static char      s_senha[128];
static bool      s_ctrl = false;         // the row's Ctrl: applies to the next key
static uint32_t  s_encerrar_ms = 0;      // "Encerrar?" waiting for the second tap
static uint32_t  s_versao = 0xFFFFFFFF;  // state already drawn
static int       s_estado_kb = -1;       // which state the keyboard was set up for
// The grid: font measurements and what each row showed (only what changed is repainted).
static int       s_cw = 14, s_lh = 24;
static uint32_t  s_hash[MAX_LINHAS];
static tab5_ssh_celula_t s_cel[MAX_COLS];

static void fonte(lgfx::LovyanGFX& d) { d.setFont(&fonts::FreeMono12pt7b); d.setTextSize(1); }

// ---------------------------------------------------------------------------
// GEOMETRY (relative to the tab area)
// ---------------------------------------------------------------------------
static int kb_a(void)       { return tecvirt_aberto(&s_kb) ? tecvirt_altura(s_a) : 0; }
static bool com_teclas(void) { return !tab5_teclado_presente(); }   // the physical one has Esc, Tab and arrows
static int teclas_y(void)   { return s_a.altura - kb_a() - TECLAS_A; }
static int grade_a(void)    { return s_a.altura - TOPO_A - kb_a() - (com_teclas() ? TECLAS_A : 0); }
static int grade_cols(void) { return (s_a.largura - 2 * MX) / s_cw; }
static int grade_lin(void)  { return grade_a() / s_lh; }

// The extra keys: label and what they send (nullptr = Ctrl, which is a state).
static const struct { const char* rot; const char* seq; } TECLAS[] = {
  { "Esc", "\x1b" }, { "Tab", "\t" }, { "Ctrl", nullptr }, { "\xE2\x86\x90", "\x1b[D" },   // ←
  { "\xE2\x86\x91", "\x1b[A" }, { "\xE2\x86\x93", "\x1b[B" }, { "\xE2\x86\x92", "\x1b[C" },   // ↑ ↓ →
  { "|", "|" }, { "~", "~" }, { "/", "/" }, { "-", "-" },
};
static const int N_TECLAS = sizeof(TECLAS) / sizeof(TECLAS[0]);

// ---------------------------------------------------------------------------
// SENDING WHAT IS TYPED
// ---------------------------------------------------------------------------
// Typing here makes this screen the owner of the size (as in tmux).
static void reclamar_tamanho(void)
{
  const int c = grade_cols(), l = grade_lin();
  if (c > 0 && l > 0) { tab5_ssh_sessao_tamanho(c, l); }
}

static void mandar(const char* d, size_t n)
{
  if (!n) { return; }
  reclamar_tamanho();
  if (s_ctrl && n == 1) {
    char c = d[0];
    if (c >= 'a' && c <= 'z') { c = (char)(c - 32); }
    if (c >= '@' && c <= '_') { const char k = (char)(c - 64); s_ctrl = false; sistema_pedir_redesenho(); tab5_ssh_sessao_escrever(&k, 1); return; }
  }
  if (s_ctrl) { s_ctrl = false; sistema_pedir_redesenho(); }
  tab5_ssh_sessao_escrever(d, n);
}

// THE FIELD TYPE in each state: the password is SENHA, the open terminal is
// COMANDO (the green key is Enter).
static uint8_t tipo_do_estado(tab5_ssh_estado_t e)
{
  return e == TAB5_SSH_SENHA ? TECVIRT_SENHA : TECVIRT_COMANDO;
}

// The keyboard follows the state (the type changes while it is open); it opens by itself when the
// connection asks for the password and closes while it connects.
static void ajustar_teclado(tab5_ssh_estado_t e)
{
  if ((int)e == s_estado_kb) { return; }
  s_estado_kb = (int)e;
  if (e == TAB5_SSH_SENHA && !tecvirt_aberto(&s_kb)) { s_senha[0] = 0; tecvirt_abrir(&s_kb, TECVIRT_SENHA); }
  if (e == TAB5_SSH_CONFIRMAR || e == TAB5_SSH_CONECTANDO) { tecvirt_fechar(&s_kb); }
  if (tecvirt_aberto(&s_kb)) { tecvirt_tipo(&s_kb, tipo_do_estado(e)); }
}

static void acao_teclado(int ac)
{
  tab5_ssh_sessao_info_t i;
  tab5_ssh_sessao_info(&i);
  if (ac == TECVIRT_FECHAR) { tecvirt_fechar(&s_kb); sistema_pedir_redesenho_completo(); return; }
  if (i.estado == TAB5_SSH_ABERTA) {
    if (ac == TECVIRT_APAGAR) { mandar("\x7f", 1); return; }
    if (ac == TECVIRT_LINHA || ac == TECVIRT_ENVIAR) { mandar("\r", 1); return; }
    if (ac > 0) { char u[4]; mandar(u, (size_t)tecvirt_utf8(ac, u)); }
    return;
  }
  if (i.estado == TAB5_SSH_SENHA) {
    if (ac == TECVIRT_ENVIAR || ac == TECVIRT_LINHA) {
      tab5_ssh_sessao_senha(s_senha);
      memset(s_senha, 0, sizeof(s_senha));
      tecvirt_fechar(&s_kb);
      s_estado_kb = -1;
    } else {
      tecvirt_editar(s_senha, sizeof(s_senha), ac);
    }
    sistema_pedir_redesenho_completo();
  }
}

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
static uint32_t hash_linha(int n, int cur_col)
{
  uint32_t h = 2166136261u;
  for (int c = 0; c < n; ++c) {
    const tab5_ssh_celula_t& x = s_cel[c];
    h = (h ^ x.cp) * 16777619u;
    h = (h ^ ((uint32_t)x.fg << 16 | x.bg)) * 16777619u;
  }
  return (h ^ (uint32_t)(cur_col + 1)) * 16777619u;
}

// A grid row: the background, the cells' background colours, the text in
// runs of the same colour, and the cursor.
static void desenhar_linha(lgfx::LovyanGFX& d, int l, int n, int x0, int y, int cur_col)
{
  d.fillRect(s_a.x, y, s_a.largura, s_lh, FUNDO_TERM);
  for (int c = 0; c < n; ++c) {
    if (s_cel[c].bg) { d.fillRect(x0 + c * s_cw, y, s_cw, s_lh, s_cel[c].bg); }
  }
  if (cur_col >= 0 && cur_col < n) { d.fillRect(x0 + cur_col * s_cw, y, s_cw, s_lh, 0xD69A); }   // cursor: the same grey as the default text (terminal data)
  char t[MAX_COLS * 4 + 1];
  for (int c = 0; c < n; ) {
    const uint16_t fg = (c == cur_col) ? (uint16_t)FUNDO_TERM : s_cel[c].fg;
    int k = 0, ini = c;
    while (c < n && ((c == cur_col) ? (uint16_t)FUNDO_TERM : s_cel[c].fg) == fg && (c == ini || (c != cur_col && ini != cur_col))) {
      const uint32_t cp = s_cel[c].cp ? s_cel[c].cp : ' ';
      k += tecvirt_utf8((int)cp, t + k);
      ++c;
    }
    t[k] = 0;
    d.setTextColor(fg);
    fonte(d);
    tab5_mono(d, t, x0 + ini * s_cw, y + s_lh - 6, s_cw, c - ini);
  }
  (void)l;
}

static void desenhar_grade(lgfx::LovyanGFX& d, const tab5_ssh_sessao_info_t& i, bool tudo)
{
  const int gy = s_a.y + TOPO_A, x0 = s_a.x + MX;
  const int lin = grade_lin() < i.linhas ? grade_lin() : i.linhas;
  const int cols_tela = grade_cols();
  const int n = (i.cols < cols_tela ? i.cols : cols_tela) < MAX_COLS ? (i.cols < cols_tela ? i.cols : cols_tela) : MAX_COLS;
  int cl, cc;
  bool vis;
  tab5_ssh_sessao_cursor(&cl, &cc, &vis);
  if (tudo) {
    tab5_tela_limpar(s_a.x, gy, s_a.largura, grade_a(), FUNDO_TERM);
    memset(s_hash, 0, sizeof(s_hash));
  }
  for (int l = 0; l < lin && l < MAX_LINHAS; ++l) {
    if (!tab5_ssh_sessao_celulas(l, s_cel, n)) { break; }
    const int cur = (vis && l == cl) ? cc : -1;
    const uint32_t h = hash_linha(n, cur);
    if (!tudo && h == s_hash[l]) { continue; }
    s_hash[l] = h;
    desenhar_linha(d, l, n, x0, gy + l * s_lh, cur);
  }
  // The session is larger than the screen (the PC owns the size): notify in the footer.
  if (tudo && (i.cols > cols_tela || i.linhas > grade_lin())) {
    tab5_fonte_corpo(d);
    tab5_texto(d, 1.8f);
    d.setTextDatum(bottom_right);
    d.setTextColor(TEMA_ALERTA, FUNDO_TERM);
    char m[64];
    snprintf(m, sizeof(m), tr("%dx%d (do PC) - digite para ajustar"), i.cols, i.linhas);
    d.drawString(m, s_a.x + s_a.largura - MX, gy + grade_a() - 2);
    d.setTextDatum(top_left);
  }
}

static void desenhar_teclas(lgfx::LovyanGFX& d)
{
  const int y = s_a.y + teclas_y();
  tab5_tela_limpar(s_a.x, y, s_a.largura, TECLAS_A, TEMA_SUPERFICIE);
  const int l = (s_a.largura - 2 * MX) / N_TECLAS;
  tab5_fonte_corpo(d);
  for (int k = 0; k < N_TECLAS; ++k) {
    const bool ativa = !TECLAS[k].seq && s_ctrl;
    tab5_botao(d, s_a.x + MX + k * l + 2, y + 6, l - 4, TECLAS_A - 12, TECLAS[k].rot,
               ativa ? (uint16_t)TEMA_DESTAQUE : (uint16_t)TEMA_SUPERFICIE_ALTA, ativa ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO);
  }
}

// A text field (destination or password) with the label above.
static void campo(lgfx::LovyanGFX& d, int y, const char* rotulo, const char* valor, bool oculto)
{
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.2f);
  d.setTextDatum(top_left);
  d.setTextColor(TEMA_TEXTO_2, FUNDO_TERM);
  d.drawString(rotulo, s_a.x + 24, y);
  d.fillRoundRect(s_a.x + 24, y + 36, s_a.largura - 48, 60, TEMA_RAIO_M, TEMA_SUPERFICIE_ALTA);
  char t[160];
  if (oculto) { size_t k = 0; for (const char* c = valor; *c && k + 2 < sizeof(t); ++c) { if ((*c & 0xC0) != 0x80) { t[k++] = '*'; } } t[k] = 0; }
  else        { snprintf(t, sizeof(t), "%s", valor); }
  strlcat(t, "_", sizeof(t));
  tab5_texto(d, 2.8f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE_ALTA);
  tab5_cortar_com_reticencias(d, t, sizeof(t), s_a.largura - 80);
  d.drawString(t, s_a.x + 40, y + 36 + 30);
  d.setTextDatum(top_left);
}

static void texto_centro(lgfx::LovyanGFX& d, const char* t, int y, float tam, uint16_t cor)
{
  char b[160];
  snprintf(b, sizeof(b), "%s", t);
  tab5_fonte_corpo(d);
  tab5_texto(d, tam);
  tab5_cortar_com_reticencias(d, b, sizeof(b), s_a.largura - 48);
  d.setTextDatum(top_center);
  d.setTextColor(cor, FUNDO_TERM);
  d.drawString(b, s_a.x + s_a.largura / 2, y);
  d.setTextDatum(top_left);
}

static const int BT_L = 260, BT_A = 64;
static const int BL = 130;      // the top buttons (Encerrar, No PC)

static void desenhar_topo(lgfx::LovyanGFX& d, const tab5_ssh_sessao_info_t& i)
{
  tab5_tela_limpar(s_a.x, s_a.y, s_a.largura, TOPO_A, TEMA_SUPERFICIE);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.0f);
  d.setTextDatum(middle_left);
  char t[128];
  if (s_pc) {
    tab5_botao(d, s_a.x + 8, s_a.y + 6, 140, TOPO_A - 12, tr("< Voltar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  } else {
    if (i.estado == TAB5_SSH_ABERTA) { snprintf(t, sizeof(t), "%s@%s  %dx%d", i.usuario, i.host, i.cols, i.linhas); }
    else                             { snprintf(t, sizeof(t), "%s@%s", i.usuario, i.host); }
    d.setTextColor(i.estado == TAB5_SSH_ABERTA ? (uint16_t)TEMA_SUCESSO : (uint16_t)TEMA_TEXTO_2, TEMA_SUPERFICIE);
    tab5_cortar_com_reticencias(d, t, sizeof(t), s_a.largura - 2 * BL - 48);
    d.drawString(t, s_a.x + 16, s_a.y + TOPO_A / 2);
  }
  tab5_botao(d, s_a.x + s_a.largura - BL - 8, s_a.y + 6, BL, TOPO_A - 12, tr("No PC"),
             tab5_web_aberto(DONO_WEB) ? (uint16_t)TEMA_DESTAQUE : (uint16_t)TEMA_SUPERFICIE_ALTA,
             tab5_web_aberto(DONO_WEB) ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO);
  if (!s_pc) {
    const bool conf = s_encerrar_ms && tab5_agora_ms() - s_encerrar_ms < 3000;
    tab5_botao(d, s_a.x + s_a.largura - 2 * BL - 16, s_a.y + 6, BL, TOPO_A - 12,
               conf ? tr("Encerrar?") : tr("Encerrar"), TEMA_PERIGO, TEMA_SOBRE_DESTAQUE);
  }
  d.setTextDatum(top_left);
}

void ssh_tela_desenhar(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (a.largura != s_a.largura || a.altura != s_a.altura || a.x != s_a.x || a.y != s_a.y) { tudo = true; }
  s_a = a;
  fonte(d);
  s_cw = d.textWidth("M");
  s_lh = d.fontHeight() + 2;
  if (s_cw <= 0) { s_cw = 14; }

  tab5_ssh_sessao_info_t i;
  tab5_ssh_sessao_info(&i);
  ajustar_teclado(i.estado);
  if (i.versao != s_versao) { s_versao = i.versao; tudo = true; }

  if (tudo) { desenhar_topo(d, i); }
  if (s_pc) {
    if (tudo) { painel_web_desenhar(d, a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A, DONO_WEB); }
    return;
  }
  if (i.estado == TAB5_SSH_ABERTA) {
    desenhar_grade(d, i, tudo);
    if (com_teclas() && tudo) { desenhar_teclas(d); }
  } else if (tudo) {
    tab5_tela_limpar(a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A - kb_a(), FUNDO_TERM);
    const int y = a.y + TOPO_A + 30;
    if (i.estado == TAB5_SSH_CONECTANDO) {
      texto_centro(d, i.msg[0] ? i.msg : tr("conectando..."), y + 60, 2.4f, TEMA_TEXTO_2);
    } else if (i.estado == TAB5_SSH_CONFIRMAR) {
      texto_centro(d, tr("Servidor novo - confira a impressão digital"), y, 2.4f, TEMA_TEXTO);
      char h[96];
      snprintf(h, sizeof(h), "%s:%d", i.host, i.porta);
      texto_centro(d, h, y + 50, 2.2f, TEMA_TEXTO_2);
      texto_centro(d, i.impressao, y + 100, 2.0f, TEMA_ALERTA);
      const int bx = a.x + a.largura / 2;
      tab5_botao(d, bx - BT_L - 12, y + 170, BT_L, BT_A, tr("Recusar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
      tab5_botao(d, bx + 12, y + 170, BT_L, BT_A, tr("Aceitar"), TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE);
    } else if (i.estado == TAB5_SSH_SENHA) {
      if (i.msg[0]) { texto_centro(d, i.msg, y, 2.2f, TEMA_ALERTA); }
      char q[120];
      snprintf(q, sizeof(q), tr("senha de %s@%s"), i.usuario, i.host);
      campo(d, y + 50, q, s_senha, true);
    }
  }
  tecvirt_posicionar(&s_kb, a);
  tecvirt_pintar(&s_kb, d, tudo);
  tab5_fonte_corpo(d);
}

// ---------------------------------------------------------------------------
// TOUCH AND KEYS
// ---------------------------------------------------------------------------
void ssh_tela_tocar(int x, int y)
{
  if (tecvirt_contem(&s_kb, s_a.x + x, s_a.y + y)) {
    const int ac = tecvirt_clique(&s_kb, s_a.x + x, s_a.y + y);
    if (ac) { acao_teclado(ac); }
    return;
  }
  tab5_ssh_sessao_info_t i;
  tab5_ssh_sessao_info(&i);
  if (y < TOPO_A) {
    if (s_pc && x < 160) { s_pc = false; sistema_pedir_redesenho_completo(); return; }
    if (x >= s_a.largura - BL - 8) { s_pc = !s_pc; tecvirt_fechar(&s_kb); sistema_pedir_redesenho_completo(); return; }
    if (!s_pc && x >= s_a.largura - 2 * BL - 16) {
      if (s_encerrar_ms && tab5_agora_ms() - s_encerrar_ms < 3000) { s_encerrar_ms = 0; tab5_ssh_sessao_fechar(); }
      else { s_encerrar_ms = tab5_agora_ms(); }
      sistema_pedir_redesenho_completo();
    }
    return;
  }
  if (s_pc) {
    painel_web_tocar(0, TOPO_A, s_a.largura, s_a.altura - TOPO_A, x, y, DONO_WEB, tab5_terminal_web_ligar);
    return;
  }
  const int y0 = TOPO_A + 30;
  if (i.estado == TAB5_SSH_ABERTA) {
    if (com_teclas() && y >= teclas_y() && y < teclas_y() + TECLAS_A) {
      const int l = (s_a.largura - 2 * MX) / N_TECLAS;
      const int k = (x - MX) / l;
      if (k >= 0 && k < N_TECLAS) {
        if (!TECLAS[k].seq) { s_ctrl = !s_ctrl; }
        else                { mandar(TECLAS[k].seq, strlen(TECLAS[k].seq)); }   // an armed Ctrl applies to it (mandar)
        sistema_pedir_redesenho_completo();
      }
      return;
    }
    // Tapping the grid opens the keyboard (without the physical one).
    if (!tecvirt_aberto(&s_kb) && !tab5_teclado_presente()) {
      tecvirt_abrir(&s_kb, TECVIRT_COMANDO);
      sistema_pedir_redesenho_completo();
    }
    return;
  }
  if (i.estado == TAB5_SSH_CONFIRMAR && y >= y0 + 170 && y < y0 + 170 + BT_A) {
    tab5_ssh_sessao_confirmar(x >= s_a.largura / 2);
    sistema_pedir_redesenho_completo();
    return;
  }
  if (i.estado == TAB5_SSH_SENHA && !tecvirt_aberto(&s_kb)) {
    tecvirt_abrir(&s_kb, TECVIRT_SENHA);
    sistema_pedir_redesenho_completo();
  }
}

void ssh_tela_tocar_nivel(int, int, bool)
{
  if (tecvirt_aberto(&s_kb) && tecvirt_altura(s_a) > 0) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb, acoes, 8);
    for (int i = 0; i < n; ++i) { acao_teclado(acoes[i]); }
  }
}

// The physical keyboard: in the open terminal, each key becomes the sequence a
// real terminal sends; in the fields, it edits.
void ssh_tela_teclar(const char* txt, uint8_t codigo, uint8_t mod)
{
  tab5_ssh_sessao_info_t i;
  tab5_ssh_sessao_info(&i);
  if (i.estado != TAB5_SSH_ABERTA) {
    if (codigo == TAB5_TECLA_ENTER)       { acao_teclado(TECVIRT_ENVIAR); }
    else if (codigo == TAB5_TECLA_APAGAR) { acao_teclado(TECVIRT_APAGAR); }
    else if (!codigo && txt) {
      for (const char* p = txt; *p; ) {
        const unsigned char b = (unsigned char)*p;
        const int n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        int cp = n == 1 ? b : n == 2 ? ((b & 0x1F) << 6) | (p[1] & 0x3F)
               : n == 3 ? ((b & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)
               : ((b & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        acao_teclado(cp);
        p += n;
      }
    }
    return;
  }
  const char* seq = nullptr;
  switch (codigo) {
    case TAB5_TECLA_ENTER:    seq = "\r"; break;
    case TAB5_TECLA_APAGAR:   seq = "\x7f"; break;
    case TAB5_TECLA_ESC:      seq = "\x1b"; break;
    case TAB5_TECLA_TAB:      seq = "\t"; break;
    case TAB5_TECLA_ESQUERDA: seq = "\x1b[D"; break;
    case TAB5_TECLA_DIREITA:  seq = "\x1b[C"; break;
    case TAB5_TECLA_CIMA:     seq = "\x1b[A"; break;
    case TAB5_TECLA_BAIXO:    seq = "\x1b[B"; break;
    case TAB5_TECLA_DEL:      seq = "\x1b[3~"; break;
    default: break;
  }
  if (seq) { mandar(seq, strlen(seq)); return; }
  if (!txt || !txt[0]) { return; }
  if ((mod & TAB5_TECLA_CTRL) && !txt[1]) {
    char c = txt[0];
    if (c >= 'a' && c <= 'z') { c = (char)(c - 32); }
    if (c >= '@' && c <= '_') { const char k = (char)(c - 64); reclamar_tamanho(); tab5_ssh_sessao_escrever(&k, 1); }
    return;
  }
  if (mod & TAB5_TECLA_ALT) { mandar("\x1b", 1); }
  mandar(txt, strlen(txt));
}

void ssh_tela_aviso(void) { sistema_pedir_redesenho(); }

void ssh_tela_abrir(void)
{
  s_versao = 0xFFFFFFFF;
  s_estado_kb = -1;
  s_pc = false;
  memset(s_hash, 0, sizeof(s_hash));
}

void ssh_tela_fechar(void)
{
  tecvirt_fechar(&s_kb);
  memset(s_senha, 0, sizeof(s_senha));
  s_estado_kb = -1;
}
