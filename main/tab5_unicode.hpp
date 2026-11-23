// TEXT FROM OUTSIDE, READY FOR THE SYSTEM FONTS.
//
// The smooth fonts (ferramentas/gerar_fontes.py) have ASCII, the Latin-1 of
// Portuguese and half a dozen symbols; the rest that has a drawing is the single
// code point emoji (tab5_emoji). EVERYTHING else becomes M5GFX's "dummy" - a
// RECTANGLE (IFont::drawCharDummy) - and that is where the messages "all in
// boxes" from the Telegram bridge came from: people writing on a phone send flags
// (two regional indicators), skin tones, "styled" letters (the fancy-font
// generators use the Mathematical Alphanumeric Symbols, 𝗻𝗲𝗴𝗿𝗶𝘁𝗼 /
// 𝘪𝘵𝘢𝘭𝘪𝘤𝘰 / 𝓈𝒸𝓇𝒾𝓅𝓉), special spaces, combining accents, Cyrillic...
//
// Here each code point becomes whatever can be drawn, in this order:
//   1. the font or the emoji set has it -> stays as is;
//   2. it has a readable equivalent -> replaced (𝗔->A, ａ->a, Ⓐ->A, ł->l, Ж->Zh,
//      🇧🇷->BR, a+◌́->á, thin space->space, „->", ...);
//   3. it is invisible by nature (zero width, selector, skin tone, ZWJ) ->
//      removed;
//   4. none of the above -> "?" (a question mark says "there was something here" without
//      the wall of little boxes).
//
// Pure: no M5GFX, no ESP-IDF - runs on the Mac (testes/rodar_unicode.sh). The
// caller is the one who knows what the font and the emoji draw, via the predicate.
#pragma once

#include <stddef.h>
#include <stdint.h>

// true = this code point has its own drawing (font or emoji).
typedef bool (*tab5_unicode_desenha_t)(uint32_t codigo);

// 'out' always ends in NUL; the cut always falls on a character boundary.
// Also collapses ZWJ chains to the first emoji (see tab5_emoji_preparar) and
// replaces line breaks and tabs with a space.
void tab5_unicode_preparar(const char* in, char* out, size_t out_tam, tab5_unicode_desenha_t desenha);

// ---- POSITIONS IN A UTF-8 TEXT (cursor, cut) -------------------------------
// Always on a character boundary: an accent (2 bytes) or emoji (4) is never
// split in half.
static inline int tab5_utf8_anterior(const char* s, int i)
{
  if (i <= 0) { return 0; }
  --i;
  while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) { --i; }
  return i;
}

// The character after 'i', without going past 'fim' (-1 = up to the '\0').
static inline int tab5_utf8_seguinte(const char* s, int i, int fim = -1)
{
  if (fim >= 0 ? i >= fim : !s[i]) { return i; }
  ++i;
  while ((fim >= 0 ? i < fim : s[i] != 0) && ((unsigned char)s[i] & 0xC0) == 0x80) { ++i; }
  return i;
}

// How many of the first 'n' bytes of 's' fit without splitting a character.
// Decides ONLY from the n bytes given - never looks at s[n]. (It used to, 04/11/2026: the
// editor inserts each key from a stack buffer, and the byte after it was
// garbage; when the garbage looked like a UTF-8 continuation, the key vanished - Enter
// in Notes "did not take" every so often. notas_ler, after fread, also
// looked at an unread byte.)
static inline int tab5_utf8_corte(const char* s, int n)
{
  if (n <= 0) { return 0; }
  int i = n - 1;   // the start of the last character
  while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) { --i; }
  const unsigned char b = (unsigned char)s[i];
  const int tam = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
  return i + tam <= n ? n : i;
}
