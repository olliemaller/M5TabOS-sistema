#include "teclado_virtual.hpp"
#include "idioma.h"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "tab5_teclado.hpp" // key-repeat timings
#include <stdlib.h>
#include <string.h>

// FOUR PAGES, same geometry (10/10/9/7 keys): switching page only changes
// labels, nothing jumps around. Each key is ONE UTF-8 character.
//  0 letters: the number row always stays (password, PIN and time without
//    switching page).
//  1 "?123": punctuation and pairs - parentheses, braces, brackets, signs.
//  2 "=\<": the rest of ASCII and the Portuguese accents (the system font
//    has these and not much more - see ferramentas/gerar_fontes.py). The
//    accents follow shift, like the letters.
//  3 emoji: everyday ones, all from the system's light table (tab5_emoji -
//    the same as Mesh's); on the symbol pages the left-hand key cycles
//    "=\<" -> emoji -> "?123". Emoji does not go back to letters by itself: you can
//    send several in a row.
static const int N_FILA[TECVIRT_FILAS] = { 10, 10, 9, 7 };
static const char* const PAGINA[4][TECVIRT_FILAS] = {
  { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
  { "1234567890", "@#$%&-+()/", "*\"':;!?_=", "[]{}<>\\" },
  { "1234567890", "áàâãéêíóôõ", "úç°ºª|~^`", "•…«»×÷±" },
  { "😀😂🤣😊😍😘😉😎🤔😢", "😭😡😱🥳😇🤗👍👎👏🙏", "💪👋👀🔥🎉❤⭐✅❌", "💡📍🏠☀⚡☕🎵" },
};

#define FILAS   TECVIRT_FILAS

// THE PROFILE OF EACH FIELD TYPE (the table in the .hpp). The single source: opening and
// tecvirt_tipo copy from here into the keyboard.
enum { ACAO_OK = 0, ACAO_LINHA, ACAO_BUSCAR };
typedef struct {
  uint8_t acao;        // what the green key is
  bool    frase;       // automatic capitals and double space (with tecvirt_contexto)
  bool    variantes;   // accents on long press
  bool    emoji;       // the emoji page
  bool    ascii;       // ASCII only
  bool    digitos;     // 0-9 only
} perfil_t;
static const perfil_t PERFIL[TECVIRT_TIPOS] = {
  /* TEXTO     */ { ACAO_OK,     false, true,  false, false, false },
  /* ASCII     */ { ACAO_OK,     false, false, false, true,  false },
  /* SENHA     */ { ACAO_OK,     false, false, false, true,  false },
  /* NUMERO    */ { ACAO_OK,     false, false, false, true,  true  },
  /* BUSCA     */ { ACAO_BUSCAR, false, true,  false, false, false },
  /* PARAGRAFO */ { ACAO_LINHA,  true,  true,  true,  false, false },
  /* CODIGO    */ { ACAO_LINHA,  false, true,  false, false, false },
  /* COMANDO   */ { ACAO_LINHA,  false, true,  false, false, false },
};
static const perfil_t& perfil(const tecvirt_t* t) { return PERFIL[t->tipo < TECVIRT_TIPOS ? t->tipo : TECVIRT_TEXTO]; }

// LONG PRESS: the variants of each key (lower case; shift applies), in
// Android keyboard order. Only what the system font draws
// (ferramentas/gerar_fontes.py): no i grave and no capital U grave.
static const struct { int base; const char* var; } VARIANTES[] = {
  { 'a', "áàâãä" }, { 'e', "éêèë" }, { 'i', "íîï" }, { 'o', "óôõòö" },
  { 'u', "úüû" },   { 'c', "ç" },    { 'n', "ñ" },
  { '.', "!?,;:…" },   // the full-stop key, on the command row
};

// ---------------------------------------------------------------------------
// MEASUREMENTS BY ORIENTATION
// ---------------------------------------------------------------------------
// In landscape (1280 wide) a key is ~120 px wide and 52 tall: the
// keyboard is short and wide, and there is little height to spare on screen. In portrait it is the opposite:
// ten keys in 720 px give only ~67 px of width, and the screen has height to spare.
// So, in portrait, the key grows UPWARDS (68 px, almost square), the gap
// shrinks a little to give width back to the keys, and the lettering gets bigger. The
// whole keyboard rises ~80 px - what the portrait conversation can spare.
static bool em_pe(const area_t& a) { return a.largura < a.altura; }
static int  alt_t(const area_t& a)    { return em_pe(a) ? 68 : 52; }   // height of each key
static int  vao(const area_t& a)      { return em_pe(a) ? 5 : 6; }
static int  px_letra(const area_t& a) { return em_pe(a) ? 34 : 30; }   // font for letters and numbers

static int altura_teclas(const area_t& a) { return vao(a) + (FILAS + 1) * (alt_t(a) + vao(a)); }

// The docked physical keyboard hides the virtual one (see the .hpp).
static bool fisico(void) { return tab5_teclado_presente(); }

int tecvirt_altura(const area_t& a)
{
  return fisico() ? 0 : altura_teclas(a);
}

// ---------------------------------------------------------------------------
// UTF-8 AND SHIFT
// ---------------------------------------------------------------------------
int tecvirt_utf8(int cp, char out[4])
{
  if (cp < 0x80)    { out[0] = (char)cp; return 1; }
  if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// The codepoint of column 'col' in a row (UTF-8). 0 = outside it.
static int cp_em(const char* s, int col)
{
  const unsigned char* u = (const unsigned char*)s;
  for (int k = 0; *u; ++k) {
    int cp, n;
    if      (u[0] < 0x80)           { cp = u[0]; n = 1; }
    else if ((u[0] & 0xE0) == 0xC0) { cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); n = 2; }
    else if ((u[0] & 0xF0) == 0xE0) { cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); n = 3; }
    else                            { cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); n = 4; }
    if (k == col) { return cp; }
    u += n;
  }
  return 0;
}

// A letter (ASCII or Latin-1 lower/upper case)? And its capital.
static bool eh_letra(int cp)
{
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
      || (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7);
}
static int maiuscula(int cp)
{
  if (cp >= 'a' && cp <= 'z') { return cp - 32; }
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) { return cp - 0x20; }
  return cp;
}

// What the character key types right now (page and shift applied).
static int cp_tecla(const tecvirt_t* t, int fila, int col)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return 0; }
  const int cp = cp_em(PAGINA[t->pagina][fila], col);
  return t->shift != TECVIRT_SHIFT_NAO ? maiuscula(cp) : cp;
}

// ---------------------------------------------------------------------------
// SPECIAL KEYS
// ---------------------------------------------------------------------------
// As on the Android keyboard: shift and delete at the ends of the "zxcv" row,
// and below them page switch, comma, space, full stop, SEND and close. Colour,
// what it does and the width in one place: the full drawing, the drawing of an
// isolated key and touch all need to agree. On the command row the width is
// in HALF units of 1/10 of the width (20 in total); shift and delete take
// up the leftover of the 7-key row (1.5 units each).
//
// DRAWN ICONS, NOT TYPED: codes < 0x20 become drawings in
// pintar_tecla (shift released/one letter/locked, and delete).
#define ICO_SHIFT  "\x01"
#define ICO_SHIFT1 "\x02"
#define ICO_CAPS   "\x03"
#define ICO_APAGA  "\x04"
#define ICO_ENTER  "\x05"   // new line (the font does not have the symbol)
enum { E_SHIFT, E_APAGA, E_MODO, E_VIRG, E_ESPACO, E_PONTO, E_ENVIAR, E_FECHAR, NESP };
#define ID_ESP(i) (64 + (i))
#define CMD0 E_MODO   // the first special key on the command row
typedef struct { uint16_t cor; int acao; int meias; } esp_t;
static const esp_t ESP[NESP] = {
  { TEMA_LINHA,    0,              0 },   // shift / symbol page switch (internal)
  { TEMA_LINHA,    TECVIRT_APAGAR, 0 },
  { TEMA_LINHA,    0,              3 },   // ?123 / ABC (internal)
  { TEMA_LINHA,    ',',            2 },
  { TEMA_LINHA,    ' ',            8 },
  { TEMA_LINHA,    '.',            2 },
  { TEMA_DESTAQUE, TECVIRT_ENVIAR, 3 },   // the only accent key: send / OK
  { TEMA_LINHA,    TECVIRT_FECHAR, 2 },
};

// The special key's label in the current state.
static const char* rotulo_esp(const tecvirt_t* t, int i)
{
  switch (i) {
    case E_SHIFT:
      if (t->pagina == 1) { return "=\\<"; }
      if (t->pagina == 2) { return "😀"; }
      if (t->pagina == 3) { return "?123"; }
      return t->shift == TECVIRT_SHIFT_TRAVA ? ICO_CAPS : t->shift == TECVIRT_SHIFT_UMA ? ICO_SHIFT1 : ICO_SHIFT;
    case E_APAGA:  return ICO_APAGA;
    case E_MODO:   return t->pagina ? "ABC" : "?123";
    case E_VIRG:   return ",";
    case E_ESPACO: return tr("espaço");
    case E_PONTO:  return ".";
    case E_ENVIAR:
      switch (perfil(t).acao) {
        case ACAO_LINHA:  return ICO_ENTER;
        case ACAO_BUSCAR: return tr("BUSCAR");
        default:          return "OK";
      }
    default:       return "X";
  }
}

static uint16_t cor_esp(const tecvirt_t* t, int i)
{
  if (i == E_SHIFT && t->pagina == 0 && t->shift != TECVIRT_SHIFT_NAO) { return TEMA_DESTAQUE; }   // lit = active
  return ESP[i].cor;
}

// ---------------------------------------------------------------------------
// GEOMETRY - calculated, not tabulated: the width changes with rotation.
// ---------------------------------------------------------------------------
static int topo_de(const area_t& a) { return a.y + a.altura - altura_teclas(a); }
static int y_fila(const area_t& a, int f) { return topo_de(a) + vao(a) + f * (alt_t(a) + vao(a)); }
static int larg_tecla(const area_t& a) { return (a.largura - 2 * vao(a) - vao(a) * 9) / 10; }   // by the longest row
static int recuo_fila(const area_t& a, int f)
{
  const int u = larg_tecla(a) + vao(a);
  return (a.largura - 2 * vao(a) - N_FILA[f] * u + vao(a)) / 2;
}

static bool tecla_em(const area_t& a, int fila, int col, int* x, int* y, int* l)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return false; }
  *l = larg_tecla(a);
  *x = a.x + vao(a) + recuo_fila(a, fila) + col * (*l + vao(a));
  *y = y_fila(a, fila);
  return true;
}

static bool especial_em(const area_t& a, int i, int* x, int* y, int* l)
{
  if (i < 0 || i >= NESP) { return false; }
  const int total = a.largura - 2 * vao(a);
  if (i == E_SHIFT || i == E_APAGA) {
    const int r = recuo_fila(a, FILAS - 1);
    const int fim_letras = r + N_FILA[FILAS - 1] * (larg_tecla(a) + vao(a));
    *y = y_fila(a, FILAS - 1);
    if (i == E_SHIFT) { *x = a.x + vao(a);              *l = r - vao(a); }
    else              { *x = a.x + vao(a) + fim_letras; *l = total - fim_letras; }
    return true;
  }
  int inicio = 0;
  for (int k = CMD0; k < i; ++k) { inicio += ESP[k].meias; }
  *x = a.x + vao(a) + (inicio * total) / 20;
  *y = y_fila(a, FILAS);
  *l = ((inicio + ESP[i].meias) * total) / 20 - (inicio * total) / 20 - vao(a);
  return true;
}

static bool retangulo(const area_t& a, int id, int* x, int* y, int* l)
{
  if (id >= 64) { return especial_em(a, id - 64, x, y, l); }
  if (id < 0) { return false; }
  return tecla_em(a, id / 16, id % 16, x, y, l);
}

// A KEY'S TARGET (where a touch counts for it) is bigger than its drawing:
//  - the gap between keys belongs to someone: each responds up to half of it,
//    as on a phone keyboard;
//  - at the ends of the indented rows ("asdf..."), half a key further
//    out: the indent was a dead spot, and a finger aiming for "a" that lands a little to
//    its left wants "a", not nothing. Shift and delete go to the edge;
//  - the command row goes edge to edge and downwards without limit (the
//    locked navigation strip delivers the touch on the area's last row).
static const int ALVO_SEM_FIM = 1 << 14;

static bool alvo_de(const area_t& a, int id, int* x0, int* y0, int* x1, int* y1)
{
  const int f = (vao(a) + 1) / 2;
  int x, y, l;
  if (!retangulo(a, id, &x, &y, &l)) { return false; }
  *x0 = x - f;
  *x1 = x + l + f;
  *y0 = y - f;
  *y1 = y + alt_t(a) + f;
  if (id >= 64) {
    const int i = id - 64;
    if (i == E_SHIFT || i == CMD0) { *x0 = a.x; }
    if (i == E_APAGA || i == E_FECHAR) { *x1 = a.x + a.largura; }
    if (i >= CMD0) { *y1 = ALVO_SEM_FIM; }
    return true;
  }
  const int fi = id / 16, c = id % 16;
  if (fi < FILAS - 1) {
    if (c == 0)              { *x0 -= l / 2; }
    if (c == N_FILA[fi] - 1) { *x1 += l / 2; }
  }
  return true;
}

static bool no_alvo(const area_t& a, int id, int xs, int ys, int folga)
{
  int x0, y0, x1, y1;
  if (!alvo_de(a, id, &x0, &y0, &x1, &y1)) { return false; }
  return xs >= x0 - folga && xs < x1 + folga && ys >= y0 - folga && ys < y1 + folga;
}

// HYSTERESIS: a finger already on a key only moves to the neighbour once it
// enters HISTERESE_PX into it. On the exact boundary, finger tremor (and the
// GT911's) switched the key from one sample to the next - and the one that counted was
// the last sample's before lifting, a lottery. 'atual' = the key the
// finger was on (TECVIRT_NENHUMA for a new touch).
static const int HISTERESE_PX = 8;

static int tecla_no_ponto(const area_t& a, int xs, int ys, int atual)
{
  if (atual != TECVIRT_NENHUMA && no_alvo(a, atual, xs, ys, HISTERESE_PX)) { return atual; }
  for (int fi = 0; fi < FILAS; ++fi) {
    for (int c = 0; c < N_FILA[fi]; ++c) {
      if (no_alvo(a, fi * 16 + c, xs, ys, 0)) { return fi * 16 + c; }
    }
  }
  for (int i = 0; i < NESP; ++i) {
    if (no_alvo(a, ID_ESP(i), xs, ys, 0)) { return ID_ESP(i); }
  }
  return TECVIRT_NENHUMA;
}

static bool eh_apagar(int id) { return id == ID_ESP(E_APAGA); }

// ---------------------------------------------------------------------------
// DRAWING - only primitives and the system font (glyph cache): no
// stored image. An image of the finished keyboard would cost ~420 KB of PSRAM per
// state to save a few ms on a paint that only happens on opening and on
// switching shift/page; day to day only 1-2 keys are repainted per touch.
// ---------------------------------------------------------------------------

// Each special key's font, once per width: the LONGEST label it can
// have ("ENVIAR", "espaço", "?123") drops 2 at a time until it fits (5 px of
// breathing room on each side).
static void medir(tecvirt_t* t, lgfx::LovyanGFX& d)
{
  const char* const VARIANTES[NESP][2] = {   // the labels in the current language
    { "=\\<", "?123" }, { "", "" }, { "?123", "ABC" }, { ",", "" },
    { tr("espaço"), "" }, { ".", "" }, { "OK", tr("BUSCAR") }, { "X", "" },
  };
  const int chave = t->a.largura ^ (int)(idioma_versao() << 16);   // another width or another language: measure again
  if (t->med_larg == chave) { return; }
  t->med_larg = chave;
  for (int i = 0; i < NESP; ++i) {
    int x, y, l;
    especial_em(t->a, i, &x, &y, &l);
    int px = px_letra(t->a);
    for (; px > 14; px -= 2) {
      d.setFont(tab5_fonte_suave(true, px));
      int w = 0;
      for (int v = 0; v < 2; ++v) { const int wv = (int)d.textWidth(VARIANTES[i][v]); if (wv > w) { w = wv; } }
      if (w <= l - 10) { break; }
    }
    t->px_esp[i] = (uint8_t)px;
  }
}

// Shift arrow: filled when active (one letter or locked), outline only
// when released; locked gets the bar underneath.
static void icone_shift(lgfx::LovyanGFX& d, int cx, int cy, char tipo, uint16_t tinta)
{
  const int oy = (tipo == ICO_CAPS[0]) ? cy - 3 : cy;
  if (tipo == ICO_SHIFT[0]) {
    for (int o = 0; o < 2; ++o) {
      d.drawTriangle(cx, oy - 15 + o, cx - 15 + o, oy + 1 - o, cx + 15 - o, oy + 1 - o, tinta);
      d.drawRect(cx - 6 + o, oy + 1 - o, 12 - 2 * o, 13, tinta);
    }
    return;
  }
  d.fillTriangle(cx, oy - 15, cx - 15, oy + 1, cx + 15, oy + 1, tinta);
  d.fillRect(cx - 6, oy + 1, 12, 13, tinta);
  if (tipo == ICO_CAPS[0]) { d.fillRect(cx - 13, oy + 18, 26, 4, tinta); }
}

// New line: the usual "return" drawing - goes down on the right and back
// to the left with the arrowhead.
static void icone_enter(lgfx::LovyanGFX& d, int cx, int cy, uint16_t tinta)
{
  for (int o = 0; o < 3; ++o) {
    d.drawFastVLine(cx + 12 + o, cy - 14, 18, tinta);
    d.drawFastHLine(cx - 12, cy + 2 + o, 27, tinta);
  }
  d.fillTriangle(cx - 22, cy + 3, cx - 10, cy - 7, cx - 10, cy + 13, tinta);
}

// Delete: body with a point on the left and a hollow X in the key's colour.
static void icone_apagar(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor, uint16_t tinta)
{
  d.fillTriangle(cx - 24, cy, cx - 9, cy - 14, cx - 9, cy + 14, tinta);
  d.fillRect(cx - 9, cy - 14, 33, 29, tinta);
  for (int o = -1; o <= 1; ++o) {
    d.drawLine(cx + 1 + o, cy - 6, cx + 13 + o, cy + 6, cor);
    d.drawLine(cx + 13 + o, cy - 6, cx + 1 + o, cy + 6, cor);
  }
}

static void pintar_tecla(const tecvirt_t* t, lgfx::LovyanGFX& d, int id, bool acesa)
{
  int x, y, l;
  if (!retangulo(t->a, id, &x, &y, &l)) { return; }
  char letra[5];
  const char* txt;
  uint16_t cor;
  int px;
  if (id >= 64) {
    txt = rotulo_esp(t, id - 64);
    cor = cor_esp(t, id - 64);
    px  = t->px_esp[id - 64];
  } else {
    letra[tecvirt_utf8(cp_tecla(t, id / 16, id % 16), letra)] = 0;
    txt = letra;
    cor = TEMA_SUPERFICIE_ALTA;
    px  = px_letra(t->a);
  }
  // Letter and icon: white on the accent, normal text on the others.
  const uint16_t tinta = cor == TEMA_DESTAQUE ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO;
  // Lit = DESTAQUE_SUAVE, not "the same colour lighter": under the finger only
  // the edge shows, and a DIFFERENT colour is what you see there.
  if (acesa) { cor = TEMA_DESTAQUE_SUAVE; }
  d.fillRoundRect(x, y, l, alt_t(t->a), TEMA_RAIO_M, cor);

  const int cx = x + l / 2, cy = y + alt_t(t->a) / 2;
  if ((unsigned char)txt[0] < 0x20) {
    if (txt[0] == ICO_APAGA[0])      { icone_apagar(d, cx, cy, cor, tinta); }
    else if (txt[0] == ICO_ENTER[0]) { icone_enter(d, cx, cy, tinta); }
    else                             { icone_shift(d, cx, cy, txt[0], tinta); }
    return;
  }
  d.setFont(tab5_fonte_suave(true, px));
  d.setTextColor(tinta, cor);
  d.drawString(txt, cx, cy);
}

static void soltar_dedos(tecvirt_t* t);

// THE VARIANTS STRIP (long press), over the row above the key:
// one cell per variant, the chosen one highlighted.
static void pintar_faixa(tecvirt_t* t, lgfx::LovyanGFX& d)
{
  const int a = alt_t(t->a);
  d.fillRoundRect(t->pop_x - 4, t->pop_y - 4, t->pop_n * t->pop_l + 8, a + 8, TEMA_RAIO_M, TEMA_LINHA);
  d.setFont(tab5_fonte_suave(true, px_letra(t->a)));
  for (int k = 0; k < t->pop_n; ++k) {
    const int x = t->pop_x + k * t->pop_l;
    const uint16_t cor = k == t->pop_sel ? (uint16_t)TEMA_DESTAQUE : (uint16_t)TEMA_SUPERFICIE_ALTA;
    d.fillRoundRect(x + 2, t->pop_y, t->pop_l - 4, a, TEMA_RAIO_M, cor);
    char u[5];
    u[tecvirt_utf8(t->pop_cp[k], u)] = 0;
    d.setTextColor(k == t->pop_sel ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO, cor);
    d.drawString(u, x + t->pop_l / 2, t->pop_y + a / 2);
  }
}

// Visible or not (the physical keyboard came or went with it open): the navigation
// lock follows, and the app redraws entirely - its area changed size.
static void sincronizar(tecvirt_t* t)
{
  const bool quer = t->aberto && !fisico();
  if (quer == t->trava) { return; }
  t->trava = quer;
  t->sujo = true;
  soltar_dedos(t);
  sistema_travar_navegacao(quer);
  sistema_pedir_redesenho_completo();
}

void tecvirt_pintar(tecvirt_t* t, lgfx::LovyanGFX& d, bool completo)
{
  sincronizar(t);
  if (!t->aberto || fisico()) { return; }
  const bool tudo = completo || t->sujo;
  const bool rot  = !tudo && t->rotulos;
  if (!tudo && !rot && t->tecla_sob == t->tecla_pinta && !t->pop_pintar) { return; }   // nothing changed: do not even touch the screen

  const lgfx::TextStyle estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  medir(t, d);
  d.setTextSize(1);
  d.setTextDatum(middle_center);

  if (tudo || rot) {
    // The background is only cleared when the keyboard appears or moves; on a
    // shift/page switch the keys are opaque, the corners are already in the panel background, and
    // repainting them all is simpler (and almost as cheap) than choosing which.
    if (tudo) { tab5_tela_limpar(t->a.x, topo_de(t->a), t->a.largura, altura_teclas(t->a), TEMA_SUPERFICIE); }
    else if (t->pop_limpar) {   // the strip closed: only its rectangle (the keys come next)
      tab5_tela_limpar(t->pop_x - 4, t->pop_y - 4, t->pop_n * t->pop_l + 8, alt_t(t->a) + 8, TEMA_SUPERFICIE);
    }
    t->pop_limpar = false;
    for (int f = 0; f < FILAS; ++f) {
      for (int c = 0; c < N_FILA[f]; ++c) { pintar_tecla(t, d, f * 16 + c, f * 16 + c == t->tecla_sob); }
    }
    for (int i = 0; i < NESP; ++i) { pintar_tecla(t, d, ID_ESP(i), ID_ESP(i) == t->tecla_sob); }
    t->sujo = t->rotulos = false;
  } else {
    // Key feedback: at most two - the one the finger left and the one it found.
    if (t->tecla_pinta != TECVIRT_NENHUMA) { pintar_tecla(t, d, t->tecla_pinta, false); }
    if (t->tecla_sob   != TECVIRT_NENHUMA) { pintar_tecla(t, d, t->tecla_sob, true); }
  }
  t->tecla_pinta = t->tecla_sob;
  if (t->pop_dedo >= 0 && (tudo || rot || t->pop_pintar)) { pintar_faixa(t, d); }
  t->pop_pintar = false;

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

// ---------------------------------------------------------------------------
// STATE: shift, page, automatic capitals
// ---------------------------------------------------------------------------
static void mudar(tecvirt_t* t, uint8_t shift, uint8_t pagina)
{
  if (shift == t->shift && pagina == t->pagina) { return; }
  t->shift = shift;
  t->pagina = pagina;
  t->rotulos = true;
  sistema_pedir_redesenho();
}

void tecvirt_tipo(tecvirt_t* t, uint8_t tipo)
{
  if (tipo >= TECVIRT_TIPOS) { tipo = TECVIRT_TEXTO; }
  t->tipo = tipo;
  t->frase = PERFIL[tipo].frase;
  t->tem_contexto = false;
  t->ult_acao = 0;
  // The current page may have disappeared (emoji in a password field): back to letters.
  if (t->pagina == 3 && !PERFIL[tipo].emoji) { t->pagina = 0; }
  if (!t->frase && t->shift == TECVIRT_SHIFT_UMA) { t->shift = TECVIRT_SHIFT_NAO; }
  t->rotulos = true;
  sistema_pedir_redesenho();
}

void tecvirt_contexto(tecvirt_t* t, const char* texto, int cursor)
{
  if (!t->frase || !t->aberto || t->shift == TECVIRT_SHIFT_TRAVA) { return; }
  int i = cursor;
  const int n = texto ? (int)strlen(texto) : 0;
  if (i > n) { i = n; }
  if (i < 0) { i = 0; }
  t->antes[0] = i > 0 ? texto[i - 1] : 0;
  t->antes[1] = i > 1 ? texto[i - 2] : 0;
  t->tem_contexto = true;
  bool espaco = false;
  while (i > 0 && texto[i - 1] == ' ') { --i; espaco = true; }
  const bool inicio = i == 0 || texto[i - 1] == '\n'
                  || (espaco && (texto[i - 1] == '.' || texto[i - 1] == '!' || texto[i - 1] == '?'));
  mudar(t, inicio ? TECVIRT_SHIFT_UMA : TECVIRT_SHIFT_NAO, t->pagina);
}

bool tecvirt_editar(char* buf, size_t cap, int acao)
{
  size_t n = strlen(buf);
  if (acao == TECVIRT_APAGAR) {
    while (n > 0) {
      const unsigned char b = (unsigned char)buf[--n];
      if ((b & 0xC0) != 0x80) { break; }   // UTF-8 continuation: remove the whole character
    }
    buf[n] = 0;
    return true;
  }
  if (acao <= 0) { return false; }
  if (acao < 0x20) { return true; }
  char u[4];
  const int k = tecvirt_utf8(acao, u);
  if (n + k < cap) { memcpy(buf + n, u, k); buf[n + k] = 0; }
  return true;
}

// ---------------------------------------------------------------------------
// LIFECYCLE
// ---------------------------------------------------------------------------
static void soltar_dedos(tecvirt_t* t)
{
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    auto& dd = t->dedos[k];
    dd.id = -1;
    dd.tecla = dd.firme = dd.cand = TECVIRT_NENHUMA;
    dd.cand_n = 0;
    dd.eco = dd.repetiu = false;
  }
  if (t->pop_dedo >= 0) { t->pop_dedo = -1; t->pop_limpar = t->rotulos = true; }   // the strip goes with the fingers
}

void tecvirt_abrir(tecvirt_t* t, uint8_t tipo)
{
  if (t->aberto) { if (tipo != t->tipo) { tecvirt_tipo(t, tipo); } return; }
  t->aberto = true;
  t->shift = TECVIRT_SHIFT_NAO;
  t->pagina = 0;
  t->sujo = true;
  t->rotulos = false;
  t->livre = 0;
  t->tecla_sob = t->tecla_pinta = t->ultima_tecla = TECVIRT_NENHUMA;
  t->solta.tecla = TECVIRT_NENHUMA;
  t->pop_dedo = -1;
  t->pop_limpar = false;
  soltar_dedos(t);
  tecvirt_tipo(t, tipo);
  // INICIO (home) sits just below the space key - but only with the virtual one on
  // screen: with the physical one, nothing covers navigation.
  t->trava = !fisico();
  if (t->trava) { sistema_travar_navegacao(true); }
}

void tecvirt_fechar(tecvirt_t* t)
{
  if (!t->aberto) { return; }
  t->aberto = false;
  t->tecla_sob = t->tecla_pinta = TECVIRT_NENHUMA;
  soltar_dedos(t);
  if (t->trava) { t->trava = false; sistema_travar_navegacao(false); }
}

void tecvirt_posicionar(tecvirt_t* t, const area_t& a)
{
  if (a.x != t->a.x || a.y != t->a.y || a.largura != t->a.largura || a.altura != t->a.altura) {
    t->a = a;
    t->sujo = true;
  }
}

bool tecvirt_contem(const tecvirt_t* t, int xs, int ys)
{
  return t->aberto && !fisico() && ys >= topo_de(t->a) && xs >= t->a.x && xs < t->a.x + t->a.largura;
}

// ---------------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------------

// THE FINGER THAT OPENED THE KEYBOARD DOES NOT TYPE. The release of the touch that opened it (or
// a late GT911 sample right after it) landed on a key and the text
// was born with a letter in it. Until the screen has had no finger for two consecutive
// samples after opening, nothing responds (level or click).
static const int LIVRE_AMOSTRAS = 2;

static bool dedos_ativos(const tecvirt_t* t)
{
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) { if (t->dedos[k].id >= 0) { return true; } }
  return false;
}

// Confirms a key (with the debounce brake). Shift and page switching
// stay in here; the rest goes to the app's list. Returns the action, or 0.
//
// SHIFT AS ON A PHONE: one tap arms it for ONE letter (it comes out capitalised and
// shift releases by itself); another tap within SHIFT_DUPLO_MS locks it (caps lock);
// a tap while locked releases it. On the symbol pages the same key toggles
// between the two pages. SPACE on a symbol page goes back to letters, and
// so does an accent (page 2): the word carries on in letters.
// Does the field type accept this character?
static bool aceita(const tecvirt_t* t, int cp)
{
  const perfil_t& p = perfil(t);
  if (p.digitos) { return cp >= '0' && cp <= '9'; }
  if (p.ascii)   { return cp >= 0x20 && cp < 0x7F; }
  return true;
}

static int confirmar(tecvirt_t* t, int tecla)
{
  static const uint32_t REPIQUE_MS     = 45;
  static const uint32_t SHIFT_DUPLO_MS = 400;
  if (tecla == TECVIRT_NENHUMA) { return 0; }
  const uint32_t agora = tab5_agora_ms();
  // DELETE is the key hammered in bursts on the same spot: no debounce brake.
  if (!eh_apagar(tecla) && tecla == t->ultima_tecla && agora - t->tecla_ms < REPIQUE_MS) { return 0; }
  t->tecla_ms = agora;
  t->ultima_tecla = (int16_t)tecla;

  if (tecla == ID_ESP(E_SHIFT)) {
    // On the symbol pages it cycles "?123" -> "=\<" -> emoji, and emoji
    // only exists in the types that accept it.
    if (t->pagina) {
      const uint8_t prox = t->pagina == 1 ? 2 : (t->pagina == 2 && perfil(t).emoji) ? 3 : 1;
      mudar(t, t->shift, prox);
      return 0;
    }
    uint8_t s = TECVIRT_SHIFT_NAO;
    if (t->shift == TECVIRT_SHIFT_NAO)                                   { s = TECVIRT_SHIFT_UMA; }
    else if (t->shift == TECVIRT_SHIFT_UMA && agora - t->shift_ms < SHIFT_DUPLO_MS) { s = TECVIRT_SHIFT_TRAVA; }
    t->shift_ms = agora;
    mudar(t, s, 0);
    return 0;
  }
  if (tecla == ID_ESP(E_MODO)) { mudar(t, t->shift, t->pagina ? 0 : 1); return 0; }

  int ac;
  if (tecla >= 64) {
    if (tecla - 64 >= NESP) { return 0; }
    ac = ESP[tecla - 64].acao;
    if (ac == TECVIRT_ENVIAR && perfil(t).acao == ACAO_LINHA) { ac = TECVIRT_LINHA; }
  } else {
    ac = cp_tecla(t, tecla / 16, tecla % 16);
  }
  // WHAT THE FIELD DOES NOT ACCEPT DOES NOT COME OUT (and changes neither shift nor page).
  if (ac > 0 && !aceita(t, ac)) { return 0; }
  if (ac > 0) {
    // After the character: one-letter shift releases; space and accent go back to letters.
    const uint8_t s = t->shift == TECVIRT_SHIFT_UMA ? (uint8_t)TECVIRT_SHIFT_NAO : t->shift;
    const bool volta = t->pagina && (ac == ' ' || (t->pagina == 2 && eh_letra(ac)));
    mudar(t, s, volta ? 0 : t->pagina);
  }
  return ac;
}

// ---------------------------------------------------------------------------
// LONG PRESS: THE VARIANTS STRIP
// ---------------------------------------------------------------------------
// The key's variants right now (only on the letters page). 0 = none.
static int variantes_de(const tecvirt_t* t, int tecla, int32_t* out)
{
  if (t->pagina != 0 || !perfil(t).variantes) { return 0; }
  int base;
  if (tecla == ID_ESP(E_PONTO))      { base = '.'; }
  else if (tecla >= 0 && tecla < 64) { base = cp_em(PAGINA[0][tecla / 16], tecla % 16); }
  else                               { return 0; }
  for (const auto& v : VARIANTES) {
    if (v.base != base) { continue; }
    int n = 0;
    for (int k = 0; n < 6; ++k) {
      const int cp = cp_em(v.var, k);
      if (!cp) { break; }
      out[n++] = (t->shift != TECVIRT_SHIFT_NAO && eh_letra(cp)) ? maiuscula(cp) : cp;
    }
    return n;
  }
  return 0;
}

// Opens the key's strip for finger k: one row above, starting over the
// key and pushed inwards at the edges. false = the key has no variants.
static bool abrir_faixa(tecvirt_t* t, int k, int tecla)
{
  const int n = variantes_de(t, tecla, t->pop_cp);
  int x, y, l;
  if (!n || !retangulo(t->a, tecla, &x, &y, &l)) { return false; }
  const int cel = larg_tecla(t->a) + vao(t->a);
  const int esq = t->a.x + vao(t->a), dir = t->a.x + t->a.largura - vao(t->a);
  int x0 = x + l / 2 - cel / 2;
  if (x0 + n * cel > dir) { x0 = dir - n * cel; }
  if (x0 < esq) { x0 = esq; }
  t->pop_dedo = (int8_t)k;
  t->pop_tecla = (int16_t)tecla;
  t->pop_n = (uint8_t)n;
  t->pop_sel = 0;
  t->pop_x = (int16_t)x0;
  t->pop_y = (int16_t)(y - alt_t(t->a) - vao(t->a));
  t->pop_l = (int16_t)cel;
  t->pop_dedo_x = (int16_t)(x + l / 2);
  t->pop_mexeu = false;
  t->pop_pintar = true;
  return true;
}

// The chosen variant counts as a typed key: one-letter shift releases.
static int confirmar_variante(tecvirt_t* t)
{
  t->tecla_ms = tab5_agora_ms();
  t->ultima_tecla = t->pop_tecla;
  if (t->shift == TECVIRT_SHIFT_UMA) { mudar(t, TECVIRT_SHIFT_NAO, t->pagina); }
  return (int)t->pop_cp[t->pop_sel];
}

static bool letra_ou_numero(int cp) { return eh_letra(cp) || (cp >= '0' && cp <= '9'); }

// ONE FINGER PER KEY, EACH WITH ITS OWN RELEASE.
//
// The key lights under the finger and counts on THAT finger's RELEASE - you can
// slide to the right one before lifting. Typing fast, the next finger
// comes down before the previous one lifts: each finger (controller id) has its own
// key, and the letters come out in the order the fingers lift.
//
// THE KEY THAT COUNTS IS THE SETTLED ONE, not the last sample's. On lifting, the
// contact area shrinks and its centre moves a few pixels - the last sample before
// release caught the neighbour and the wrong letter came out. A finger coming down fixes the
// key at once; sliding to another only switches after TECLA_FIRME
// consecutive samples on it (~40 ms): the correction of someone sliding on purpose
// still works, the stumble on release does not. The lit key is always the settled one
// - what you see lit is what will be typed.
//
// THE LETTER COMES OUT ON THE FIRST SAMPLE WITHOUT THE FINGER. The GT911 sometimes loses a finger
// for one sample, and its return would become a double letter. The return is recognised
// for what it is - the same key, the same spot, ON THE SAMPLE AFTER the
// release - and marked as an ECHO: it lights, but does not type again. The window is
// counted in samples: the interval between samples varies with drawing (20 to
// 45 ms), and a limit in ms alone was sometimes short, sometimes long. ECO_MS_MAX is the
// ceiling: on a slow pass (heavy drawing), "the next sample" may arrive
// late enough to be a genuinely new tap on the same spot.
// If it slid to another key, it is no longer an echo.
//
// THE ECHO DOES NOT APPLY TO DELETE, and never chains. Tapped fast on the same
// spot, delete went up and down within the echo window: the tap was
// swallowed, and its release became the reference for the next - a whole burst
// vanished. Only a release that TYPED serves as a reference, and delete
// is never an echo.
//
// HELD DELETE REPEATS, as on any keyboard: after REP_INICIO_MS
// with the finger still on it, it deletes one every REP_MS.
int tecvirt_nivel(tecvirt_t* t, int* acoes, int max)
{
  static const int      SALTO_PX      = 45;
  static const uint32_t ECO_AMOSTRAS  = 1;
  static const uint32_t ECO_MS_MAX    = 60;   // and never more than this (slow pass)
  static const uint8_t  TECLA_FIRME   = 2;
  // The same timings as the physical keyboard (hal/tab5_teclado.hpp).
  static const uint32_t REP_INICIO_MS = TAB5_TECLA_REPETE_INICIO_MS;
  static const uint32_t REP_MS        = TAB5_TECLA_REPETE_MS;
  static const uint32_t LONGO_MS      = 450;   // holding a letter: the variants
  static const uint32_t ESPACO_DUPLO_MS = 700;
  sincronizar(t);
  if (!t->aberto || fisico()) { return 0; }
  ++t->amostra;
  sistema_toque_t q[SISTEMA_TOQUES_MAX];
  const int n = sistema_toques(q, SISTEMA_TOQUES_MAX);
  const area_t ap = sistema_area();
  // Finger on the keyboard strip: the touch is its, the release does not become a click on
  // anything (see sistema_consumir_toque). Including during the opening grace period.
  for (int i = 0; i < n; ++i) {
    if (tecvirt_contem(t, ap.x + q[i].x, ap.y + q[i].y)) { sistema_consumir_toque(); break; }
  }
  if (t->livre < LIVRE_AMOSTRAS) {
    if (n) { t->livre = 0; } else { ++t->livre; }
    soltar_dedos(t);
    return 0;
  }

  auto tecla_de = [&](const sistema_toque_t& p, int atual) {
    return (int16_t)tecla_no_ponto(t->a, ap.x + p.x, ap.y + p.y, atual);
  };
  int na = 0;
  const uint32_t agora = tab5_agora_ms();
  auto por = [&](int ac) { if (na < max) { acoes[na++] = ac; } };
  // DOUBLE SPACE = ". " (running text): the first space is swapped for the full stop.
  // DOUBLE SPACE = ". " (PARAGRAFO): the first space becomes a full stop. Decided
  // by the TEXT the app reported (tecvirt_contexto), not by the memory of the
  // keys: with the cursor moved between the two spaces, the memory would delete
  // the wrong letter.
  auto anotar = [&](int ac) {
    if (!ac) { return; }
    const bool duplo = ac == ' ' && t->frase && t->tem_contexto && t->ult_acao == ' '
                    && agora - t->ult_ms < ESPACO_DUPLO_MS
                    && t->antes[0] == ' ' && letra_ou_numero((unsigned char)t->antes[1]);
    if (duplo) {
      por(TECVIRT_APAGAR); por('.'); por(' ');
      t->ult_acao = 0;   // the third space in a row is a space
      t->ult_ms = agora;
      return;
    }
    por(ac);
    t->ult_acao = ac; t->ult_ms = agora;
  };
  const int16_t antes = t->tecla_sob;

  // 1) Fingers already down: follow the key under them, or release.
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    auto& dd = t->dedos[k];
    if (dd.id < 0) { continue; }
    int achou = -1;
    for (int i = 0; i < n; ++i) { if (q[i].id == dd.id) { achou = i; break; } }
    // THE STRIP'S FINGER: sliding chooses the variant, lifting types it.
    if (k == t->pop_dedo) {
      if (achou >= 0) {
        // Until the finger SLIDES the first variant counts (as on Android and the
        // iPhone): near the edge the strip is pushed inwards, and the cell
        // under the still finger is not the one the person wanted.
        const int xs = ap.x + q[achou].x;
        if (abs(xs - t->pop_dedo_x) > 12) { t->pop_mexeu = true; }
        int sel = t->pop_mexeu ? (xs - t->pop_x) / t->pop_l : 0;
        if (sel < 0) { sel = 0; }
        if (sel >= t->pop_n) { sel = t->pop_n - 1; }
        if (sel != t->pop_sel) { t->pop_sel = (uint8_t)sel; t->pop_pintar = true; sistema_pedir_redesenho(); }
        dd.x = q[achou].x; dd.y = q[achou].y;
        continue;
      }
      anotar(confirmar_variante(t));
      t->pop_dedo = -1;
      t->pop_limpar = t->rotulos = true;   // clear only the strip's place and repaint the keys
      t->solta.tecla = TECVIRT_NENHUMA;
      dd.id = -1;
      dd.tecla = dd.firme = dd.cand = TECVIRT_NENHUMA;
      dd.cand_n = 0;
      dd.eco = dd.repetiu = false;
      continue;
    }
    if (achou >= 0) {
      const int16_t sob = tecla_de(q[achou], dd.tecla);
      const int dx = q[achou].x - dd.x, dy = q[achou].y - dd.y;
      if (sob != dd.tecla && (abs(dx) > SALTO_PX || abs(dy) > SALTO_PX)) {
        // THE CONTROLLER REUSES THE ID: the finger lifts and the next comes down
        // within one sample, and to the system it is the "same finger" that jumped
        // keys. A real finger does not travel more than ~45 px in one sample;
        // a bigger jump is a new touch - the previous key counts and the new one
        // starts settled.
        if (!dd.eco && !dd.repetiu) { anotar(confirmar(t, dd.firme)); }
        dd.firme = sob;
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
        dd.desde = dd.rep_ms = agora;
        dd.eco = dd.repetiu = false;
      } else if (sob == dd.firme) {
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;      // came back: the detour does not count
      } else {
        if (sob == dd.cand) { if (dd.cand_n < 255) { ++dd.cand_n; } }
        else                { dd.cand = sob; dd.cand_n = 1; }
        if (dd.cand_n >= TECLA_FIRME) {
          dd.firme = sob;
          dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
          dd.eco = dd.repetiu = false;
          dd.rep_ms = agora;
        }
      }
      dd.tecla = sob;
      dd.x = q[achou].x; dd.y = q[achou].y;
      // Repeat of the held DELETE.
      if (eh_apagar(dd.firme) && agora - dd.rep_ms >= (dd.repetiu ? REP_MS : REP_INICIO_MS)) {
        anotar(confirmar(t, dd.firme));
        dd.repetiu = true;
        dd.rep_ms = agora;
      }
      // Held still on a key with variants: opens the strip (one at a time).
      else if (t->pop_dedo < 0 && !dd.eco && !dd.repetiu && dd.cand_n == 0 && dd.firme != TECVIRT_NENHUMA
            && agora - dd.rep_ms >= LONGO_MS && abrir_faixa(t, k, dd.firme)) {
        sistema_pedir_redesenho();
      }
    } else {
      const bool digita = !dd.eco && !dd.repetiu;
      if (digita) { anotar(confirmar(t, dd.firme)); }
      // Only a release that typed is an echo reference - an echo never creates another.
      t->solta = { digita ? dd.firme : (int16_t)TECVIRT_NENHUMA, dd.x, dd.y, t->amostra, agora };
      dd.id = -1;
      dd.tecla = dd.firme = dd.cand = TECVIRT_NENHUMA;
      dd.cand_n = 0;
      dd.eco = dd.repetiu = false;
    }
  }
  // 2) New fingers: the key under the touch-down point starts out settled.
  for (int i = 0; i < n; ++i) {
    bool conhecido = false;
    for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) { if (t->dedos[k].id == q[i].id) { conhecido = true; break; } }
    if (conhecido) { continue; }
    for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
      auto& dd = t->dedos[k];
      if (dd.id >= 0) { continue; }
      dd.id = (int8_t)q[i].id;
      dd.tecla = dd.firme = tecla_de(q[i], TECVIRT_NENHUMA);
      dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
      dd.x = q[i].x; dd.y = q[i].y;
      dd.desde = dd.rep_ms = agora;
      dd.repetiu = false;
      dd.eco = dd.firme != TECVIRT_NENHUMA && dd.firme == t->solta.tecla
            && t->amostra - t->solta.amostra <= ECO_AMOSTRAS && agora - t->solta.ms <= ECO_MS_MAX
            && !eh_apagar(dd.firme)
            && abs(dd.x - t->solta.x) <= SALTO_PX && abs(dd.y - t->solta.y) <= SALTO_PX;
      break;
    }
  }
  // 3) The lit key is the settled one of the finger that came down last.
  int recente = -1;
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    if (t->dedos[k].id < 0) { continue; }
    if (recente < 0 || (int32_t)(t->dedos[k].desde - t->dedos[recente].desde) >= 0) { recente = k; }
  }
  t->tecla_sob = recente >= 0 ? t->dedos[recente].firme : TECVIRT_NENHUMA;
  if (t->tecla_sob != antes || na) { sistema_pedir_redesenho(); }
  return na;
}

bool tecvirt_centro(const tecvirt_t* t, int acao, int* xs, int* ys)
{
  int x, y, l;
  for (int fi = 0; fi < FILAS; ++fi) {
    for (int c = 0; c < N_FILA[fi]; ++c) {
      const int cp = cp_tecla(t, fi, c);   // with shift: the letter counts in both cases
      if ((cp == acao || (acao > 0 && maiuscula(cp) == maiuscula(acao))) && tecla_em(t->a, fi, c, &x, &y, &l)) {
        *xs = x + l / 2; *ys = y + alt_t(t->a) / 2;
        return true;
      }
    }
  }
  for (int i = 0; i < NESP; ++i) {
    if (ESP[i].acao == acao && especial_em(t->a, i, &x, &y, &l)) {
      *xs = x + l / 2; *ys = y + alt_t(t->a) / 2;
      return true;
    }
  }
  return false;
}

int tecvirt_clique(tecvirt_t* t, int xs, int ys)
{
  // Only the safety net for a touch too quick for the level handler to have seen (otherwise it would
  // double the letter): the level handler takes care of everything it saw.
  if (!t->aberto || fisico() || t->livre < LIVRE_AMOSTRAS || dedos_ativos(t) || tab5_agora_ms() - t->tecla_ms < 120) { return 0; }
  return confirmar(t, tecla_no_ponto(t->a, xs, ys, TECVIRT_NENHUMA));   // confirmar() sets tecla_ms
}
