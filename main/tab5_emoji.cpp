#include "tab5_emoji.hpp"

#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "tab5_unicode.hpp"
#include "fontes/fontes_suaves.hpp"

// BINARY SEARCH, not a scan - see the warning in tab5_emoji_dados.cpp. With
// 1357 entries, a scan would cost up to 1357 comparisons per unknown
// emoji; binary search costs at most 11. The table MUST be
// sorted by code point for this to work - the generator already guarantees it.
static const tab5_emoji_t* achar_emoji(uint32_t code)
{
  int ini = 0, fim = TAB5_EMOJI_N - 1;
  while (ini <= fim) {
    const int meio = ini + (fim - ini) / 2;
    const uint32_t c = TAB5_EMOJI_TABELA[meio].codepoint;
    if (c == code) { return &TAB5_EMOJI_TABELA[meio]; }
    if (c < code)  { ini = meio + 1; } else { fim = meio - 1; }
  }
  return nullptr;
}

// THE CALLBACK LOVYANGFX ALREADY KNOWS HOW TO CALL.
//
// Signature fixed by the library (LGFXBase.hpp: emoji_draw_cb_t) - a
// parameter cannot change without breaking registration. 'altura' comes in pixels ALREADY
// scaled by the current text size (see the calculation in
// LGFXBase.cpp: "metrics.height * sy >> 16") - which is why this callback does
// not need to know which font size was in use.
//
// Returning <=0 says "I do not know this code point": the library falls back by itself to
// the usual behaviour (code point 0, which draws nothing). That is why the
// table only needs to know the ones it recognises - the rest stays invisible
// instead of becoming garbage on screen, exactly as before this file existed.
static int32_t emoji_cb(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint32_t code, int32_t altura)
{
  const tab5_emoji_t* e = achar_emoji(code);
  if (!e) { return 0; }

  // SCALED TO THE CELL, not drawn at native size.
  //
  // This used plain pushImage(), at the bitmap's FIXED size
  // (TAB5_EMOJI_LADO, 24px) - and was never tested against the most common case:
  // message text uses size 2, and the cell the library reserves for
  // an emoji there is 16px square, not 24. The emoji came out 8px BIGGER than the
  // cell on each axis, spilling over the neighbouring text.
  //
  // pushImageRotateZoom() fixes this by scaling the 24x24 bitmap to
  // ANY requested 'altura', always centred - the same bitmap serves for
  // a message's small text (16px) and for a possible size 4 elsewhere
  // (32px), without having to store more than one resolution.
  //
  // NO SMOOTHING (the "WithAA" version was removed). The reason:
  // antialiasing BLENDS the colour of neighbouring pixels at the edge of the drawing - and
  // near the edge of any emoji, one of the neighbours is usually the transparent
  // background (at the time, a magenta sentinel colour). The blend produced
  // a colour that WAS NO LONGER the sentinel, and became a real pixel, in the
  // wrong colour - it was seen live: a stray magenta dot on the black background.
  // Today the background is palette INDEX 0, and with a palette the library cannot
  // even blend - but without AA is still right: each source pixel
  // either IS background (skip) or is not (draw), no in-between.
  //
  // GEOMETRY: (src_x,src_y) in the bitmap maps to (dst_x,dst_y) on screen.
  //
  // THE SUM THAT CANCELS OUT FOR A LETTER BUT NOT FOR AN EMOJI - found by
  // tracing the library source (LGFXBase::write() +
  // GFXfont::drawChar(), both in lgfx_fonts.cpp/LGFXBase.cpp), after
  // a real test ("📡 Teste Ok.") showed the antenna clearly lower
  // than the text in the SAME bubble, on the SAME line.
  //
  // write() shifts the WHOLE line's 'y' by "-(metrics.y_offset)*sy" BEFORE
  // the character loop (y_offset is the only value FROM THE FONT, not from the glyph -
  // GFXfont::updateFontMetric() never writes to it; only getDefaultMetric()
  // writes it, once, when setFont() runs). Each NORMAL character goes through
  // GFXfont::drawChar(), which applies "y += metrics.y_offset*sy" again -
  // CANCELLING the line shift before drawing the glyph. The EMOJI,
  // since it does not go through drawChar(), gets the line's 'y' WITHOUT the cancellation -
  // and ends up shifted down by exactly "-y_offset * sy" pixels.
  //
  // FOR FREESANS12PT7B (the body font - see
  // TAB5_FONTE_CORPO in tab5_ui.hpp): I scanned the glyph table (grep+awk over
  // FreeSans12pt7b.h looking for the smallest yOffset, the SAME METHOD validated against
  // DejaVu24 before switching - it gave -18, matching the "18" already documented
  // here before) and arrived at glyph_ab=17 (the TALLEST ascender in the whole
  // font) - y_offset = -17, height (yAdvance) = 29 = TAB5_FONTE_CORPO_ALT_PX.
  //
  // The fix is to undo that shift (subtract 17/29 of 'altura' from the 'y'
  // received) and only THEN centre the emoji on the font's own body (the
  // upper half of the height, where the letters actually draw) - the two
  // sums become a SINGLE fraction, applied to the centre.
  //
  // THIS NUMBER IS SPECIFIC TO FREESANS12PT7B - if TAB5_FONTE_CORPO changes
  // again, recalculate glyph_ab/height for the new font (same method: smallest yOffset
  // in the glyph table) and change the sum below with it.
  // the fraction is READ from the font in use (the body font became the
  // smoothed Roboto, in several heights) - the fixed 17/29 was only for FreeSans12.
  lgfx::FontMetrics fm;
  gfx->getFont()->getDefaultMetric(&fm);
  const float Y_OFFSET_FRACAO = (fm.height > 0) ? (float)(-fm.y_offset) / (float)fm.height : 17.0f / 29.0f;
  const float FRACAO_VERTICAL = 0.5f - Y_OFFSET_FRACAO; // = -0.22: emoji centre, corrected and centred
  // INTEGER SCALE: the bitmap is 24 px, and enlarging by a fraction (24 -> 29 px)
  // duplicates some pixel rows and not others - the same jaggedness the
  // fonts used to have. Up to 2x the text height it comes out at native size,
  // centred in the character space; above that, at the largest integer multiple.
  //
  // BELOW 24 px it SHRINKS to the line height: the smooth fonts go
  // down to 12 px (gerar_fontes.py), and there a 24 px emoji intruded on the line
  // above and the one below. Shrinking skips pixel rows instead of duplicating - the
  // loss of detail in a drawing this size goes unnoticed; the intrusion does not.
  const float lado = (float)TAB5_EMOJI_LADO;
  const int   mult = (altura >= 2 * TAB5_EMOJI_LADO) ? altura / TAB5_EMOJI_LADO : 1;
  const float zoom = (altura < TAB5_EMOJI_LADO) ? (float)altura / lado : (float)mult;
  gfx->pushImageRotateZoom(
      (float)x + (float)altura / 2.0f, (float)y + (float)altura * FRACAO_VERTICAL,
      lado / 2.0f, lado / 2.0f,
      0.0f,                 // no rotation
      zoom, zoom,
      TAB5_EMOJI_LADO, TAB5_EMOJI_LADO,
      e->indices, TAB5_EMOJI_TRANSPARENTE, lgfx::color_depth_t::palette_8bit,
      // swap565_t: the same reading (swapped bytes) the library used to do of the
      // raw uint16_t before - the palette was written that way by the generator.
      reinterpret_cast<const lgfx::swap565_t*>(e->paleta));

  // Advances the cursor by the whole CELL (altura), not just the bitmap - it is
  // the same spacing a normal character would leave, and what keeps the
  // next character from sticking to the emoji.
  return altura;
}

void tab5_emoji_iniciar(void)
{
  M5.Display.setEmojiCallback(emoji_cb);
}

// WHAT HAS A DRAWING: the font (read from the VLW itself - the glyph list is the
// one gerar_fontes.py wrote, no copy to keep in sync) or the emoji.
// The body and bold fonts come from the same CARACTERES; one is enough.
static bool fonte_tem(uint32_t code)
{
  static uint32_t* s_cods = nullptr;
  static int       s_n = -1;
  if (s_n < 0) {
    s_n = 0;
    if (FONTES_CORPO_N > 0 && FONTES_CORPO[0].tamanho >= 24) {
      const uint8_t* v = FONTES_CORPO[0].dados;
      auto be32 = [](const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; };
      const uint32_t n = be32(v);
      if (24 + n * 28 <= FONTES_CORPO[0].tamanho) {
        s_cods = (uint32_t*)malloc(n * sizeof(uint32_t));
        if (s_cods) { for (uint32_t i = 0; i < n; ++i) { s_cods[i] = be32(v + 24 + i * 28); } s_n = (int)n; }
      }
    }
  }
  for (int i = 0; i < s_n; ++i) { if (s_cods[i] == code) { return true; } }
  return false;
}

static bool desenhavel(uint32_t code)
{
  return fonte_tem(code) || achar_emoji(code) != nullptr;
}

void tab5_emoji_preparar(const char* in, char* out, size_t out_tam)
{
  // The whole cleanup (ZWJ, truncation, and anything with no drawing) lives in
  // tab5_unicode - pure, tested on the Mac (testes/rodar_unicode.sh).
  tab5_unicode_preparar(in, out, out_tam, desenhavel);
}
