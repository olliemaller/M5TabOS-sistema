#include "editor.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "transferencia.h"
#include "idioma.h"
#include "tab5_unicode.hpp"

#include "esp_heap_caps.h"
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdio>

static const float TAM     = 3.2f;   // the same letter size as the conversation's compose bar
static const int   MARGEM  = 16;
static const int   ARRASTO = 12;     // px of finger movement before it becomes a scroll
static const int   MENU_BT_L = 160, MENU_BT_A = 76, MENU_VAO = 12;   // the long-press menu (4 buttons)
static const int   MENU_N = 4;                                        // Word, Line, All, Paste
// CODE MODE: the line-number column is as wide as the DIGITS of the
// largest number, hugging the edge - the screen is small, and what is left belongs to the
// code. No fixed gap to the left of "1".
static const int   CALHA_BORDA = 4;  // from the edge to the numbers
static const int   CALHA_VAO   = 10; // from the numbers to the text (with the rule in the middle)

// The editor font: the body one, or the monospaced one in code mode.
static void fonte(const editor_t* e, lgfx::LovyanGFX& d)
{
  if (e->codigo) { d.setFont(&fonts::FreeMono12pt7b); d.setTextSize(1); }
  else           { tab5_fonte_corpo(d); tab5_texto(d, TAM); }
}
static int margem(const editor_t* e) { return e->codigo ? e->calha : MARGEM; }

// The gutter for the current text: the digits of the FILE's lines (the '\n's, not
// the screen's - which depend on the width, which depends on the gutter).
static void medir_calha(editor_t* e, lgfx::LovyanGFX& d)
{
  int linhas = 1;
  for (const char* p = e->txt; (p = strchr(p, '\n')) != nullptr; ++p) { ++linhas; }
  int dig = 1;
  for (int n = linhas; n >= 10; n /= 10) { ++dig; }
  e->calha = CALHA_BORDA + dig * d.textWidth("0") + CALHA_VAO;
}

// ---------------------------------------------------------------------------
// TEXT - always on a UTF-8 character boundary
// ---------------------------------------------------------------------------
static int seguinte(const char* s, int i, int fim) { return tab5_utf8_seguinte(s, i, fim); }
static int anterior(const char* s, int i)          { return tab5_utf8_anterior(s, i); }

static void marcar(editor_t* e, bool texto)
{
  if (texto) { e->mudou = true; e->requebrar = true; e->linha_erro = 0; }
  e->sujo = true;
  sistema_pedir_redesenho();
}

static void inserir(editor_t* e, const char* t, int n)
{
  const int len = (int)strlen(e->txt);
  if (len + n > e->cap - 1) { n = e->cap - 1 - len; }
  n = tab5_utf8_corte(t, n);   // never cuts a character
  if (n <= 0) { return; }
  memmove(e->txt + e->cursor + n, e->txt + e->cursor, len - e->cursor + 1);
  memcpy(e->txt + e->cursor, t, n);
  e->cursor += n;
  tecvirt_contexto(&e->kb, e->txt, e->cursor);
  marcar(e, true);
}

static void apagar(editor_t* e, bool depois)
{
  const int len = (int)strlen(e->txt);
  const int a = depois ? e->cursor : anterior(e->txt, e->cursor);
  const int b = depois ? seguinte(e->txt, e->cursor, len) : e->cursor;
  if (a == b) { return; }
  memmove(e->txt + a, e->txt + b, len - b + 1);
  e->cursor = a;
  tecvirt_contexto(&e->kb, e->txt, e->cursor);
  marcar(e, true);
}

// ---------------------------------------------------------------------------
// LINE WRAPPING: by word; by character only when a single word does not
// fit. The '\n' closes the line and belongs to it. ini[k] = start of line k.
// ---------------------------------------------------------------------------
static bool cabe(lgfx::LovyanGFX& d, const char* s, int n, int larg)
{
  char t[160];
  if (n >= (int)sizeof(t)) { return false; }
  memcpy(t, s, n);
  t[n] = 0;
  return (int)d.textWidth(t) <= larg;
}

static int fim_da_linha(lgfx::LovyanGFX& d, const char* s, int i, int fp, int larg)
{
  if (i >= fp) { return fp; }
  int bom = i, j = i;
  while (j < fp) {
    int fim = j;
    while (fim < fp && s[fim] != ' ') { ++fim; }
    int prox = fim;
    while (prox < fp && s[prox] == ' ') { ++prox; }
    if (!cabe(d, s + i, fim - i, larg)) { break; }
    bom = prox;
    j = prox;
  }
  if (bom > i) { return bom; }
  int c = i;
  for (int nx = seguinte(s, c, fp); c < fp && cabe(d, s + i, nx - i, larg); nx = seguinte(s, c, fp)) { c = nx; }
  return c > i ? c : seguinte(s, i, fp);   // always moves forward
}

static void quebrar(editor_t* e, lgfx::LovyanGFX& d)
{
  const char* s = e->txt;
  const int len = (int)strlen(s);
  const int max = e->cap - 1;
  int n = 0, i = 0;
  e->ini[n++] = 0;
  while (i < len && n < max) {
    int fp = i;
    while (fp < len && s[fp] != '\n') { ++fp; }
    const int fim = fim_da_linha(d, s, i, fp, e->larg);
    if (fim < fp) { i = fim; e->ini[n++] = (int16_t)i; continue; }
    if (fp == len) { break; }
    i = fp + 1;                     // the '\n' belongs to this line; the next one starts after it
    e->ini[n++] = (int16_t)i;       // text ending in '\n' gets the cursor's empty line
  }
  e->ini[n] = (int16_t)len;
  e->n_lin = n;
  e->requebrar = false;
}

static int linha_do(const editor_t* e, int pos)
{
  int k = 0;
  while (k + 1 < e->n_lin && e->ini[k + 1] <= pos) { ++k; }
  return k;
}

// Visible end of line k (without the '\n').
static int fim_visivel(const editor_t* e, int k)
{
  int f = e->ini[k + 1];
  if (f > e->ini[k] && e->txt[f - 1] == '\n') { --f; }
  return f;
}

// The position on line k closest to 'alvo' px from the margin.
static int posicao_em(editor_t* e, int k, int alvo)
{
  lgfx::LovyanGFX& d = *e->disp;
  const int ini = e->ini[k], fim = fim_visivel(e, k);
  int melhor = ini, dist = 1 << 30;
  for (int i = ini;; i = seguinte(e->txt, i, fim)) {
    char t[160];
    const int n = i - ini;
    if (n >= (int)sizeof(t)) { break; }
    memcpy(t, e->txt + ini, n); t[n] = 0;
    const int dd = abs((int)d.textWidth(t) - alvo);
    if (dd < dist) { dist = dd; melhor = i; }
    if (i >= fim) { break; }
  }
  return melhor;
}

static int x_do_cursor(editor_t* e)
{
  const int k = linha_do(e, e->cursor);
  char t[160];
  int n = e->cursor - e->ini[k];
  if (n >= (int)sizeof(t)) { n = sizeof(t) - 1; }
  memcpy(t, e->txt + e->ini[k], n); t[n] = 0;
  return (int)e->disp->textWidth(t);
}

// ---------------------------------------------------------------------------
// LIFE CYCLE
// ---------------------------------------------------------------------------
bool editor_abrir(editor_t* e, char* buf, int cap)
{
  int16_t* ini = (int16_t*)heap_caps_malloc(sizeof(int16_t) * (cap + 1), MALLOC_CAP_SPIRAM);
  if (!ini) { return false; }
  *e = editor_t{};
  e->txt = buf;
  e->cap = cap;
  e->ini = ini;
  e->cursor = (int)strlen(buf);
  e->requebrar = e->sujo = true;
  return true;
}

void editor_fechar(editor_t* e)
{
  tecvirt_fechar(&e->kb);
  heap_caps_free(e->ini);
  e->ini = nullptr;
}

static void pedir_completo(void) { sistema_pedir_redesenho_completo(); }

void editor_fechar_teclado(editor_t* e)
{
  if (!tecvirt_aberto(&e->kb)) { return; }
  tecvirt_fechar(&e->kb);
  pedir_completo();
}

// ---------------------------------------------------------------------------
// CODE MODE: Berry syntax colours
// ---------------------------------------------------------------------------
static bool palavra_chave(const char* s, int n)
{
  static const char* const K[] = { "def", "end", "if", "elif", "else", "for", "while", "do", "break",
    "continue", "return", "class", "var", "import", "as", "try", "except", "raise", "static", "nil",
    "true", "false", "self", "super", nullptr };
  for (int i = 0; K[i]; ++i) { if ((int)strlen(K[i]) == n && !strncmp(K[i], s, n)) { return true; } }
  return false;
}

// Does the file line starting at 'ini' have a '#' outside quotes before
// 'ate'? That is what says whether the continuation of a wrapped line is a comment.
static bool comentario_antes(const char* s, int ini, int ate)
{
  char aspa = 0;
  for (int i = ini; i < ate; ++i) {
    const char c = s[i];
    if (aspa) { if (c == '\\') { ++i; } else if (c == aspa) { aspa = 0; } }
    else if (c == '"' || c == '\'') { aspa = c; }
    else if (c == '#') { return true; }
  }
  return false;
}

// One screen line, coloured piece by piece: comment (#...), quoted text,
// number, keyword, name followed by '(' (call) and the rest.
// A line wrapped by width starts afresh - a comment or string that
// crosses the wrap comes out uncoloured on the continuation (rare in code).
static void pintar_codigo(lgfx::LovyanGFX& d, const char* t, int x, int y, bool comentario)
{
  // Syntax highlighting is DATA: each category has its own tone, taken from the
  // theme palette (same lightness) so it stays legible on the dark background.
  const uint16_t C_COMENT = TEMA_TEXTO_3, C_TEXTO = TEMA_APP_VERDE,
                 C_NUM = TEMA_APP_LARANJA, C_CHAVE = TEMA_APP_ROXO,
                 C_FUNC = TEMA_DESTAQUE, C_RESTO = TEMA_TEXTO;
  char pedaco[160];
  int i = 0;
  const int n = (int)strlen(t);
  if (comentario) { d.setTextColor(C_COMENT); d.drawString(t, x, y); return; }   // continuation of a comment
  while (i < n) {
    int j = i;
    uint16_t cor = C_RESTO;
    const char c = t[i];
    if (c == '#') { j = n; cor = C_COMENT; }
    else if (c == '"' || c == '\'') {
      j = i + 1;
      while (j < n && t[j] != c) { if (t[j] == '\\' && j + 1 < n) { ++j; } ++j; }
      if (j < n) { ++j; }
      cor = C_TEXTO;
    } else if (c >= '0' && c <= '9') {
      while (j < n && (isalnum((unsigned char)t[j]) || t[j] == '.')) { ++j; }
      cor = C_NUM;
    } else if (isalpha((unsigned char)c) || c == '_') {
      while (j < n && (isalnum((unsigned char)t[j]) || t[j] == '_')) { ++j; }
      int k = j;
      while (k < n && t[k] == ' ') { ++k; }
      cor = palavra_chave(t + i, j - i) ? C_CHAVE : (k < n && t[k] == '(') ? C_FUNC : C_RESTO;
    } else {
      while (j < n && !(isalnum((unsigned char)t[j]) || t[j] == '_' || t[j] == '#' || t[j] == '"' || t[j] == '\'')) { ++j; }
      if (j == i) { j = i + 1; }
    }
    memcpy(pedaco, t + i, (size_t)(j - i));
    pedaco[j - i] = 0;
    d.setTextColor(cor);
    d.drawString(pedaco, x, y);
    x += (int)d.textWidth(pedaco);
    i = j;
  }
}

// The indent (leading spaces) of the file line where the cursor is.
static int recuo_atual(const editor_t* e)
{
  int i = e->cursor;
  while (i > 0 && e->txt[i - 1] != '\n') { --i; }
  int n = 0;
  while (e->txt[i + n] == ' ') { ++n; }
  return n;
}

// Enter: in code mode, the new line starts with the same indent.
static void nova_linha(editor_t* e)
{
  char t[1 + 40];
  int n = e->codigo ? recuo_atual(e) : 0;
  if (n > 40) { n = 40; }
  t[0] = '\n';
  memset(t + 1, ' ', (size_t)n);
  inserir(e, t, n + 1);
}

void editor_modo_codigo(editor_t* e)
{
  e->codigo = true;
  e->requebrar = e->sujo = true;
  if (tecvirt_aberto(&e->kb)) { tecvirt_tipo(&e->kb, TECVIRT_CODIGO); }   // code: no automatic capital or ". "
}

void editor_marcar_erro(editor_t* e, int linha)
{
  e->linha_erro = linha;
  if (linha > 0) {
    // The cursor goes to the start of the error line (the screen scrolls to it).
    int i = 0;
    for (int l = 1; l < linha && e->txt[i]; ++i) { if (e->txt[i] == '\n') { ++l; } }
    e->cursor = i;
  }
  e->sujo = true;
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
void editor_desenhar(editor_t* e, lgfx::LovyanGFX& d, const area_t& r, bool completo)
{
  if (!e->ini) { return; }
  e->disp = &d;
  fonte(e, d);
  if (e->codigo && (e->requebrar || !e->calha)) { medir_calha(e, d); }
  const int larg = r.largura - margem(e) - MARGEM;
  if (larg != e->larg) { e->larg = larg; e->requebrar = true; }
  if (e->requebrar) { quebrar(e, d); }
  if (r.x != e->ret.x || r.y != e->ret.y || r.largura != e->ret.largura || r.altura != e->ret.altura) { completo = true; }
  e->ret = r;
  e->passo = d.fontHeight() + 8;
  e->alt_texto = r.altura - (tecvirt_aberto(&e->kb) ? tecvirt_altura(r) : 0);
  e->linhas_vis = e->alt_texto / e->passo;
  if (e->linhas_vis < 1) { e->linhas_vis = 1; }

  // The cursor always in view (except while the finger scrolls).
  if (!e->arrastando) {
    const int lc = linha_do(e, e->cursor);
    if (lc < e->topo) { e->topo = lc; }
    if (lc >= e->topo + e->linhas_vis) { e->topo = lc - e->linhas_vis + 1; }
  }
  const int topo_max = e->n_lin > e->linhas_vis ? e->n_lin - e->linhas_vis : 0;
  if (e->topo > topo_max) { e->topo = topo_max; }
  if (e->topo < 0) { e->topo = 0; }

  if (e->sujo || completo) {
    e->sujo = false;
    // Background of the text area: the same the system uses for the app area
    // (also right for an app that has not yet turned on fundo_tema).
    const uint16_t fundo = sistema_cor_fundo();
    tab5_tela_limpar(r.x, r.y, r.largura, e->alt_texto, fundo);
    if (e->codigo) { d.drawFastVLine(r.x + e->calha - CALHA_VAO / 2, r.y, e->alt_texto, TEMA_LINHA); }
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO, fundo);
    const int lc = linha_do(e, e->cursor);
    char t[160];
    // Code mode: the line number (the file's, not the screen's) of the
    // first visible line - the '\n's before it.
    int num = 1;
    if (e->codigo) { for (int i = 0; i < e->ini[e->topo]; ++i) { if (e->txt[i] == '\n') { ++num; } } }
    for (int k = e->topo; k < e->n_lin && k < e->topo + e->linhas_vis; ++k) {
      int n = fim_visivel(e, k) - e->ini[k];
      if (n >= (int)sizeof(t)) { n = sizeof(t) - 1; }
      memcpy(t, e->txt + e->ini[k], n); t[n] = 0;
      const int y = r.y + 4 + (k - e->topo) * e->passo;
      if (e->codigo) {
        const bool comeca = k == 0 || e->txt[e->ini[k] - 1] == '\n';   // the continuation of a wrapped line does not count
        if (k > e->topo && comeca) { ++num; }
        if (e->linha_erro && num == e->linha_erro) {
          d.fillRect(r.x, y - 2, r.largura, e->passo - 2, d.color565(70, 16, 16));
        }
        if (comeca) {
          char nb[8];
          snprintf(nb, sizeof(nb), "%d", num);
          d.setTextDatum(top_right);
          d.setTextColor(e->linha_erro == num ? (uint16_t)TEMA_PERIGO : (uint16_t)TEMA_TEXTO_3);
          d.drawString(nb, r.x + e->calha - CALHA_VAO, y);
          d.setTextDatum(top_left);
        }
        int ini_log = e->ini[k];
        while (ini_log > 0 && e->txt[ini_log - 1] != '\n') { --ini_log; }
        pintar_codigo(d, t, r.x + margem(e), y, !comeca && comentario_antes(e->txt, ini_log, e->ini[k]));
      } else {
        d.drawString(t, r.x + MARGEM, y);
      }
      if (k == lc) { d.fillRect(r.x + margem(e) + x_do_cursor(e), y, 3, d.fontHeight(), TEMA_DESTAQUE); }
    }
    if (!e->txt[0] && !tecvirt_aberto(&e->kb)) {
      d.setTextColor(TEMA_TEXTO_3, fundo);
      d.drawString(tr("toque para escrever"), r.x + margem(e) + 8, r.y + 4);
    }
    // THE LONG-PRESS MENU, on top of the text (closing it repaints the text).
    if (e->menu) {
      static const char* const ROT[MENU_N] = { N_("Palavra"), N_("Linha"), N_("Tudo"), N_("Colar") };
      for (int b = 0; b < MENU_N; ++b) {
        const bool apagado = (b == 3 && transf_vazia()) || (b < 3 && !e->txt[0]);
        tab5_botao(d, e->menu_x + b * (MENU_BT_L + MENU_VAO), e->menu_y, MENU_BT_L, MENU_BT_A, tr(ROT[b]),
                   apagado ? TEMA_SUPERFICIE_ALTA : TEMA_DESTAQUE,
                   apagado ? TEMA_TEXTO_3 : TEMA_SOBRE_DESTAQUE);
      }
    }
  }
  tecvirt_posicionar(&e->kb, r);
  tecvirt_pintar(&e->kb, d, completo);
}

// ---------------------------------------------------------------------------
// TOUCH AND KEYS
// ---------------------------------------------------------------------------
static void aplicar(editor_t* e, int acao)
{
  if (acao > 0) { char u[4]; inserir(e, u, tecvirt_utf8(acao, u)); return; }
  if (acao == TECVIRT_APAGAR) { apagar(e, false); return; }
  if (acao == TECVIRT_ENVIAR || acao == TECVIRT_LINHA) { nova_linha(e); return; }   // in the editor, ENTER is a new line
  if (acao == TECVIRT_FECHAR) { editor_fechar_teclado(e); }
}

// The text position (bytes) under a screen point.
static int pos_no_ponto(editor_t* e, int xs, int ys)
{
  if (!e->disp || e->passo <= 0) { return e->cursor; }
  int k = e->topo + (ys - e->ret.y - 4) / e->passo;
  if (k >= e->n_lin) { k = e->n_lin - 1; }
  if (k < 0) { k = 0; }
  fonte(e, *e->disp);
  return posicao_em(e, k, xs - e->ret.x - margem(e));
}

// Copies the range [de, ate) of the text to the clipboard.
static void copiar_trecho(editor_t* e, int de, int ate, const char* recado)
{
  if (ate <= de) { return; }
  char* t = (char*)heap_caps_malloc((size_t)(ate - de) + 1, MALLOC_CAP_SPIRAM);
  if (!t) { return; }
  memcpy(t, e->txt + de, (size_t)(ate - de));
  t[ate - de] = 0;
  transf_copiar(t);
  heap_caps_free(t);
  sistema_recado(recado);
}

// The WORD under 'p': up to a space, line break or punctuation. A byte >= 0x80 (accent,
// emoji) counts as a letter - "ação" is a single word.
static bool separa(char c) { return c == ' ' || c == '\n' || c == '\t' || (c > 0 && strchr(".,;:!?()[]{}\"'", c)); }
static void copiar_palavra(editor_t* e, int p)
{
  const int n = (int)strlen(e->txt);
  if (p >= n) { p = n - 1; }
  if (p < 0 || separa(e->txt[p])) {            // in the gap between words: the one on the left
    while (p > 0 && separa(e->txt[p])) { --p; }
    if (p < 0 || separa(e->txt[p])) { return; }
  }
  int de = p, ate = p;
  while (de > 0 && !separa(e->txt[de - 1])) { --de; }
  while (ate < n && !separa(e->txt[ate])) { ++ate; }
  copiar_trecho(e, de, ate, tr("Palavra copiada"));
}

// The LINE under 'p': the whole paragraph, from one break to the next.
static void copiar_linha(editor_t* e, int p)
{
  const int n = (int)strlen(e->txt);
  if (p > n) { p = n; }
  int de = p, ate = p;
  while (de > 0 && e->txt[de - 1] != '\n') { --de; }
  while (ate < n && e->txt[ate] != '\n') { ++ate; }
  copiar_trecho(e, de, ate, tr("Linha copiada"));
}

void editor_colar(editor_t* e)
{
  char* t = (char*)heap_caps_malloc(TRANSF_MAX, MALLOC_CAP_SPIRAM);
  if (!t) { return; }
  const size_t n = transf_colar(t, TRANSF_MAX);
  for (size_t i = 0; i < n; ++i) { if ((unsigned char)t[i] < 0x20 && t[i] != '\n') { t[i] = ' '; } }
  if (n) { inserir(e, t, (int)n); sistema_recado(tr("Colado")); }
  heap_caps_free(t);
}

void editor_tocar_nivel(editor_t* e, int x, int y, bool pressionado)
{
  int acoes[8];
  const int n = tecvirt_nivel(&e->kb, acoes, 8);
  for (int i = 0; i < n && tecvirt_aberto(&e->kb); ++i) { aplicar(e, acoes[i]); }

  const area_t a = sistema_area();
  const int ys = a.y + y;
  const bool no_texto = ys >= e->ret.y && ys < e->ret.y + e->alt_texto;
  if (toque_longo(&e->lp, x, y, pressionado) && no_texto) {
    sistema_consumir_toque();
    // The menu just above the finger, entirely inside the text.
    e->menu_pos = pos_no_ponto(e, a.x + x, ys);
    const int l = MENU_N * MENU_BT_L + (MENU_N - 1) * MENU_VAO;
    int mx = a.x + x - l / 2, my = ys - MENU_BT_A - 40;
    if (mx < e->ret.x + 8) { mx = e->ret.x + 8; }
    if (mx + l > e->ret.x + e->ret.largura - 8) { mx = e->ret.x + e->ret.largura - 8 - l; }
    if (my < e->ret.y + 4) { my = ys + 40; }
    e->menu = true; e->menu_x = mx; e->menu_y = my;
    marcar(e, false);
    return;
  }
  // SCROLL WITH THE FINGER: by whole lines, without moving the cursor.
  if (!pressionado) { e->arrastando = false; e->arr_y = -1; return; }
  if (!no_texto) { return; }
  if (e->arr_y < 0) { e->arr_y = y; e->arr_topo = e->topo; return; }
  if (!e->arrastando && abs(y - e->arr_y) > ARRASTO) { e->arrastando = true; sistema_consumir_toque(); }
  if (e->arrastando && e->passo > 0) {
    const int t = e->arr_topo - (y - e->arr_y) / e->passo;
    if (t != e->topo) { e->topo = t; marcar(e, false); }
  }
}

void editor_tocar(editor_t* e, int x, int y)
{
  const area_t a = sistema_area();
  const int xs = a.x + x, ys = a.y + y;
  if (tecvirt_contem(&e->kb, xs, ys)) {
    const int ac = tecvirt_clique(&e->kb, xs, ys);
    if (ac) { aplicar(e, ac); }
    return;
  }
  if (e->menu) {   // any touch closes the menu; on a button, it does what the button says
    e->menu = false;
    marcar(e, false);
    if (ys >= e->menu_y && ys < e->menu_y + MENU_BT_A && xs >= e->menu_x) {
      const int b = (xs - e->menu_x) / (MENU_BT_L + MENU_VAO);
      const bool no_botao = (xs - e->menu_x) % (MENU_BT_L + MENU_VAO) < MENU_BT_L;
      if (no_botao && b < 3 && e->txt[0]) {
        if (b == 0)      { copiar_palavra(e, e->menu_pos); }
        else if (b == 1) { copiar_linha(e, e->menu_pos); }
        else             { transf_copiar(e->txt); sistema_recado(tr("Copiado")); }
      } else if (no_botao && b == 3) {
        editor_colar(e);
      }
    }
    return;
  }
  if (!e->disp || e->passo <= 0 || ys < e->ret.y || ys >= e->ret.y + e->alt_texto) { return; }
  e->cursor = pos_no_ponto(e, xs, ys);
  if (!tecvirt_aberto(&e->kb) && !tab5_teclado_presente()) {
    tecvirt_abrir(&e->kb, e->codigo ? TECVIRT_CODIGO : TECVIRT_PARAGRAFO);   // the green key is the new-line key
    pedir_completo();
  }
  tecvirt_contexto(&e->kb, e->txt, e->cursor);
  marcar(e, false);
}

static void linha_vizinha(editor_t* e, int passo)
{
  const int k = linha_do(e, e->cursor) + passo;
  if (k < 0 || k >= e->n_lin || !e->disp) { return; }
  e->cursor = posicao_em(e, k, x_do_cursor(e));
}

void editor_teclar(editor_t* e, const char* texto, uint8_t codigo, uint8_t mod)
{
  const bool ctrl = (mod & TAB5_TECLA_CTRL) != 0;
  const int len = (int)strlen(e->txt);
  switch (codigo) {
    case TAB5_TECLA_ENTER:    nova_linha(e); return;
    case TAB5_TECLA_APAGAR:   apagar(e, false); return;
    case TAB5_TECLA_DEL:      apagar(e, true); return;
    case TAB5_TECLA_ESQUERDA: e->cursor = anterior(e->txt, e->cursor); marcar(e, false); return;
    case TAB5_TECLA_DIREITA:  e->cursor = seguinte(e->txt, e->cursor, len); marcar(e, false); return;
    case TAB5_TECLA_CIMA:     if (ctrl) { e->cursor = 0; } else { linha_vizinha(e, -1); } marcar(e, false); return;
    case TAB5_TECLA_BAIXO:    if (ctrl) { e->cursor = len; } else { linha_vizinha(e, 1); } marcar(e, false); return;
    case TAB5_TECLA_TAB:      inserir(e, "  ", 2); return;
    case TAB5_TECLA_ESC:      return;
    default: break;
  }
  if (ctrl && texto && (texto[0] == 'v' || texto[0] == 'V')) { editor_colar(e); return; }
  if (mod & (TAB5_TECLA_CTRL | TAB5_TECLA_ALT)) { return; }
  char limpo[16];
  int n = 0;
  for (const char* c = texto; c && *c && n < (int)sizeof(limpo) - 1; ++c) {
    if ((unsigned char)*c >= 0x20) { limpo[n++] = *c; }
  }
  inserir(e, limpo, n);
}
