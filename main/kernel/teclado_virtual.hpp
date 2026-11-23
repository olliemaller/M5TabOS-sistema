// The SYSTEM on-screen keyboard.
//
// A single component for every app that needs text: the conversation (Mesh,
// ESP-NOW), the Wi-Fi password, the Bluetooth PIN. The app does not draw keys,
// does not handle fingers and does not lock navigation - it only:
//
//   static tecvirt_t s_kb;
//   open:       tecvirt_abrir(&s_kb, TECVIRT_SENHA); (the field TYPE; locks INICIO/home)
//   close:      tecvirt_fechar(&s_kb);               (also in the app's ao_fechar)
//   draw:       tecvirt_posicionar(&s_kb, a);        (the area, in SCREEN coords, at whose base it opens)
//               tecvirt_pintar(&s_kb, d, completo);  (EVERY pass: cheap when nothing changed)
//   level:      n = tecvirt_nivel(&s_kb, acoes, 8);  (in ao_tocar_nivel; apply the actions)
//   click:      a = tecvirt_clique(&s_kb, xs, ys);   (in ao_tocar, if tecvirt_contem)
//
// It does not replace the physical one on ExtPort1: with both available the physical
// one wins, being faster and not taking screen space. WITH THE PHYSICAL ONE DOCKED the open
// virtual keyboard becomes INVISIBLE: it stays "open" for the app (the field is still being
// edited and the physical key arrives via ao_teclar), but height 0, it does not paint, does not
// respond to the finger and does not lock navigation. Docking or removing it while open
// switches on the next paint, with a full redraw of the app.

#pragma once
#include "app.h"

// THE FIELD TYPE - the app says WHAT the field is, once, on opening (like
// Android's inputType and the iPhone's keyboardType/returnKeyType); the keyboard
// derives everything else from that. A single table (PERFIL, in the .cpp) - no app turns on
// capitals, accents or labels by itself, and two fields of the same type
// behave the same in any app.
//
//   type        green key     automatic   ". " on    accents on   emoji   accepts
//                             capitals    space x2   long press
//   TEXTO       OK            -           -          yes          -       everything (file name, title)
//   ASCII       OK            -           -          -            -       ASCII only (server, user, SSID, node name)
//   SENHA       OK            -           -          -            -       ASCII only
//   NUMERO      OK            -           -          -            -       digits only (PIN, port)
//   BUSCA       SEARCH        -           -          yes          -       everything
//   PARAGRAFO   new line      sentence    yes        yes          yes     everything (Notes, conversation)
//   CODIGO      new line      -           -          yes          -       everything (the Berry editor)
//   COMANDO     new line      -           -          yes          -       everything (Terminal, SSH: Enter runs)
//
// "sentence" = capital at the start of the text, after ". ", "! ", "? " and after a
// new line; the app reports the text and the cursor (tecvirt_contexto).
// What the field does not accept the keyboard does not even deliver (the accented
// variant does not appear; the emoji page disappears).
enum {
  TECVIRT_TEXTO = 0, TECVIRT_ASCII, TECVIRT_SENHA, TECVIRT_NUMERO, TECVIRT_BUSCA,
  TECVIRT_PARAGRAFO, TECVIRT_CODIGO, TECVIRT_COMANDO, TECVIRT_TIPOS
};

// 4 rows of characters (the last with shift and delete at the ends) + the command row.
#define TECVIRT_FILAS    4

// Height it takes up in area 'a' (the same one passed to tecvirt_posicionar):
// the rows and the gaps. DEPENDS ON ORIENTATION - in portrait the keys are
// taller, because there width is short and height is plentiful (see the .cpp). Whoever reserves
// space for the keyboard asks here, and never stores a number.
//
// NO dead strip at the bottom: while it is open navigation is
// locked and its strip belongs to the app (sistema_travar_navegacao), so
// the keyboard sits right on the bottom bar and a finger that overshoots the command
// row still counts on it.
int tecvirt_altura(const area_t& a);

// What a key does (tecvirt_nivel/tecvirt_clique return this):
//   >0  the Unicode CODEPOINT typed, WITH shift ALREADY applied. ASCII mostly;
//       the second symbols page has the Portuguese accents
//       (the system font has them) and the emoji page, codepoints above
//       0xFFFF (4 bytes in UTF-8) - whoever stores text converts with
//       tecvirt_utf8, or uses tecvirt_editar, which already does it. An ASCII-only field
//       (password, PIN) rejects emoji via tecvirt_editar(so_ascii).
//   -2  delete    -3  send    -4  new line    -5  close the keyboard
// Shift and page switching belong to the keyboard itself: they change the labels and never
// reach the app.
#define TECVIRT_APAGAR  (-2)
#define TECVIRT_ENVIAR  (-3)
#define TECVIRT_LINHA   (-4)   // the green key of the multi-line types (PARAGRAFO, CODIGO, COMANDO)
#define TECVIRT_FECHAR  (-5)

// Stable key identifier: characters = row*16 + column; specials = 64 + index.
#define TECVIRT_NENHUMA (-1)

// Shift as on a phone: one tap counts for ONE letter; two taps in a row
// lock it (caps lock); a tap while locked releases it.
#define TECVIRT_SHIFT_NAO    0
#define TECVIRT_SHIFT_UMA    1
#define TECVIRT_SHIFT_TRAVA  2

typedef struct {
  area_t   a;                     // area in SCREEN coords at whose base it opens (tecvirt_posicionar)
  bool     aberto;
  bool     trava;                 // holds the navigation lock right now (open and visible)
  bool     frase;                 // automatic capital at the start of a sentence (tecvirt_frase)
  uint8_t  shift;                 // TECVIRT_SHIFT_*
  uint8_t  pagina;                // 0 = letters, 1 and 2 = symbols, 3 = emoji
  uint32_t shift_ms;              // when shift was tapped (the second tap locks it)
  bool     sujo;                  // next paint redoes everything (opened, moved)
  bool     rotulos;               // shift/page changed: redo the keys, without clearing the background
  int16_t  tecla_sob, tecla_pinta;   // lit under the finger / the one currently painted lit
  int16_t  ultima_tecla;
  uint32_t tecla_ms;
  uint32_t amostra;               // count of calls to tecvirt_nivel (the echo clock)
  uint8_t  livre;                 // samples without a finger since it opened (see the .cpp)
  // One per finger (controller id; -1 = free). 'tecla' = the one under the
  // finger now; 'firme' = the one it types on lifting (changes only after
  // TECLA_FIRME consecutive samples on the new one: 'cand'/'cand_n'). 'eco' =
  // reappearance of the finger that has just lifted (the GT911 loses a finger for one
  // sample): does not type again. 'repetiu' = the held DELETE has already deleted
  // by itself (the release does not delete again).
  struct { int8_t id; bool eco, repetiu; uint8_t cand_n; int16_t tecla, firme, cand, x, y; uint32_t desde, rep_ms; } dedos[SISTEMA_TOQUES_MAX];
  struct { int16_t tecla, x, y; uint32_t amostra, ms; } solta;   // the last release that typed
  // Measurements that depend on the width (the font of each special key), done
  // once per width and not on every key painted.
  int      med_larg;
  uint8_t  px_esp[8];
  uint8_t  tipo;                  // TECVIRT_TEXTO... (tecvirt_abrir / tecvirt_tipo)
  // LONG PRESS ON A LETTER: the accented variants in a strip above,
  // chosen by sliding the finger (as on Android and the iPhone).
  int8_t   pop_dedo;              // the index in 'dedos' that opened it (-1 = closed)
  int16_t  pop_tecla;             // the key it came from
  uint8_t  pop_n, pop_sel;        // how many variants; the one chosen now
  int32_t  pop_cp[6];             // the variants (with shift applied)
  int16_t  pop_x, pop_y, pop_l;   // the strip (SCREEN): x, y and width of each cell
  int16_t  pop_dedo_x;            // where the finger was when it opened
  bool     pop_mexeu;             // the finger has already slid (before that the 1st variant counts)
  bool     pop_pintar;            // the strip changed: repaint
  bool     pop_limpar;            // the strip closed: clear its place and repaint the keys
  // DOUBLE SPACE = ". " (PARAGRAFO): the last action and when it came out, and the two
  // characters before the cursor (the app reports them via tecvirt_contexto - without it,
  // there is no automatic full stop: the keyboard does not guess the text).
  int32_t  ult_acao;
  uint32_t ult_ms;
  char     antes[2];              // [0] just before the cursor, [1] the one before that
  bool     tem_contexto;
} tecvirt_t;

// Opens with the field TYPE (table above). Opening again while already open only changes the type.
void tecvirt_abrir(tecvirt_t* t, uint8_t tipo = TECVIRT_TEXTO);
// Changes the type with the keyboard open (the same keyboard moves from the SSID field
// to the password one): labels and rules change at once.
void tecvirt_tipo(tecvirt_t* t, uint8_t tipo);
void tecvirt_fechar(tecvirt_t* t);
static inline bool tecvirt_aberto(const tecvirt_t* t) { return t->aberto; }

// The area (SCREEN coordinates) at whose BASE it opens, at its width. If it changed,
// the next paint redoes everything.
void tecvirt_posicionar(tecvirt_t* t, const area_t& a);

// Does the point (SCREEN) fall in the open keyboard's strip?
bool tecvirt_contem(const tecvirt_t* t, int xs, int ys);

// Call on EVERY drawing pass of the app. 'completo' = the app cleared the area
// (the keyboard redoes everything); without it, only what changed: the key that lit and the one that
// went out, or the labels after shift. If nothing changed, it draws nothing.
void tecvirt_pintar(tecvirt_t* t, lgfx::LovyanGFX& d, bool completo);

// In the app's ao_tocar_nivel, with the keyboard open: reads the fingers (sistema_toques),
// lights the key the last one will type, and returns in 'acoes' the keys confirmed in this
// sample (in the order the fingers lifted). Requests the redraw by itself.
// HOLDING A LETTER (~450 ms) opens its accented variants ("a" -> á à
// â ã ä); sliding chooses and lifting types. Holding the full stop gives the
// punctuation (! ? , ; : ...). All inside the keyboard: the app changes nothing.
int tecvirt_nivel(tecvirt_t* t, int* acoes, int max);

// In ao_tocar, a SCREEN point inside the keyboard: a safety net for a touch too quick
// for the level handler to have seen. Returns the action, or 0 (nothing to do).
int tecvirt_clique(tecvirt_t* t, int xs, int ys);

// THE FIELD'S TEXT AND CURSOR, reported by the app whenever either changes
// (and right after opening) - in the running-text types (PARAGRAFO): at the
// start of the text, after ". ", "! ", "? " or a new line the shift
// arms for ONE letter (caps lock is not touched), and a DOUBLE SPACE after a
// letter or number becomes ". ". In the other types, it does nothing.
void tecvirt_contexto(tecvirt_t* t, const char* texto, int cursor);

// WHERE THE KEY IS that types 'acao' right now (the character on the current page and
// shift, or TECVIRT_APAGAR/ENVIAR/FECHAR, ' ', ',', '.'): the centre, in
// SCREEN coordinates. false = there is no such key on the current page. For the
// tests (testes/teclado and the console's 'tocar'), not for the app.
bool tecvirt_centro(const tecvirt_t* t, int acao, int* xs, int* ys);

// Codepoint -> UTF-8 in 'out' (up to 4 bytes, no trailing zero). Returns the byte count.
int tecvirt_utf8(int cp, char out[4]);

// SIMPLE EDITING, for a field without a cursor (password, name): a character goes at the end,
// DELETE removes the last whole character (UTF-8). What the TYPE does not accept the
// keyboard no longer delivers. 'cap' = buffer size (including the zero). Returns true
// if the action was an edit (character or delete) - the others are left to the app.
bool tecvirt_editar(char* buf, size_t cap, int acao);
