// EMOJI - a small set, drawn as an IMAGE, instead of a font.
//
// -------------------------------------------------------------------------
// WHY AN IMAGE, AND WHY THIS BELONGS TO THE KERNEL AND NOT TO A SINGLE APP
// -------------------------------------------------------------------------
// This system's font is a 6x8 ASCII bitmap - it has no emoji, and never will
// as a font: the art of a real emoji does not fit in a character
// drawing. The way out is not to redraw them, but to SHOW the drawing that
// already exists - LovyanGFX already has the right hook for this.
//
// setEmojiCallback() (see LGFXBase.hpp) is called automatically by the
// library whenever write()/drawString() meets a Unicode code point that
// the current font does not know - and that covers EVERY emoji, because this
// system's font is pure ASCII. Registering ONCE at boot, any app that calls
// d.drawString() with a received message shows this set's emoji
// without knowing this file exists - the same principle as the rest
// of the kernel: whoever draws text should not have to worry about this.
//
// -------------------------------------------------------------------------
// THE SET, THE LIMIT AND THE LICENCE
// -------------------------------------------------------------------------
// EVERY emoji made of a SINGLE Unicode code point - 1357 of them (of the 1391 that
// exist in version 17.0; 34 are recent additions that Twemoji has not yet
// drawn). Art from Twemoji (twemoji.twitter.com), licence CC-BY 4.0 -
// attribution required, which is why it lives here.
//
// "A SINGLE CODE POINT" is the real limit, and it is worth understanding why.
// The variation selector (U+FE0F) does NOT count against it - the library itself
// already discards it before any emoji reaches the callback (LGFXBase.cpp:
// "if (uniCode >= 0xFE00 && uniCode < 0xFE10) continue"). What is LEFT OUT:
//
//   - flags: two "regional indicators" stuck together (🇧🇷 = U+1F1E7 U+1F1F7)
//   - skin tone: the base emoji + a modifier (👍🏽 = U+1F44D U+1F3FD)
//   - ZWJ sequences: several emoji joined by U+200D (👨‍👩‍👧 = person+ZWJ+person+ZWJ+child)
//
// Each of these is SEVERAL Unicode code points forming ONE composite image, and the
// LovyanGFX hook (setEmojiCallback) fires once PER CODE POINT - there is no
// way for this architecture, as it stands, to merge N calls into a single image
// without real extra work (collecting the pending code points, deciding when the
// sequence has ended). Left for when it is needed.
//
// -------------------------------------------------------------------------
// FORMAT
// -------------------------------------------------------------------------
// 24x24 (TAB5_EMOJI_LADO) with a PALETTE: each emoji has its own list of RGB565
// colours (at most 256 - no Twemoji exceeds 237, the average is 62) and a
// 1-byte index per pixel. Index 0 (TAB5_EMOJI_TRANSPARENTE) is always
// the background: "do not draw". Transparency is decided by the INDEX, never by
// the colour - no pixel blending can fabricate a false "transparent".
//
// Why a palette and not raw RGB565: 2 bytes per pixel become 1, plus the palette.
// 1357 emoji went from ~1.49MB to ~0.91MB of flash, with the SAME image
// pixel for pixel (the generator only changes the storage, not the art).
//
// SIZE ON SCREEN - see tab5_emoji.cpp. Up to 2x the line height the bitmap
// comes out at native size (24px); above that, at the largest integer multiple; on a
// line SMALLER than 24px it shrinks to the line height, so as not to intrude on the
// lines above and below.
//
// -------------------------------------------------------------------------
// LOOKUP
// -------------------------------------------------------------------------
// TAB5_EMOJI_TABELA is SORTED by code point, and tab5_emoji.cpp searches it by
// BINARY SEARCH - with more than a thousand entries, a linear scan would be
// wasteful (up to 1357 comparisons per unknown emoji, against at most
// 11 with binary search). The generator (gerar_emoji.py) already writes the table
// sorted; if anyone ever edits it by hand, the order MUST be kept.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint32_t        codepoint;   // Unicode code point, WITHOUT the variation selector (FE0F)
  const uint8_t*  indices;     // TAB5_EMOJI_LADO x TAB5_EMOJI_LADO, 1 byte per pixel
  const uint16_t* paleta;      // RGB565 with swapped bytes; [0] is the background
} tab5_emoji_t;

static const uint32_t TAB5_EMOJI_TRANSPARENTE = 0;   // background index

extern const int             TAB5_EMOJI_LADO;
extern const tab5_emoji_t    TAB5_EMOJI_TABELA[];   // SORTED by code point - see above
extern const int             TAB5_EMOJI_N;

// Registers the callback on M5.Display. Call ONCE, at boot, after
// M5.begin() (the callback is a panel method, which only exists after that).
void tab5_emoji_iniciar(void);

// -------------------------------------------------------------------------
// PREPARING TEXT FOR DRAWING - the cleanup that avoids "garbage on screen"
// -------------------------------------------------------------------------
// Whoever builds a message or a name for d.drawString() should pass it through
// here first, whenever the text comes from outside (another node, another person) and may
// contain emoji. It solves TWO problems at once:
//
// 1) SAFE TRUNCATION. A snprintf("%.60s", txt) cuts by BYTE, not by
//    character - and an emoji takes 4 UTF-8 bytes. Cutting in the middle of one
//    leaves orphan bytes at the end of the string, and the UTF-8 decoder has no
//    way of knowing what to do with them: the result is unpredictable, and that was
//    the most likely cause of the "garbage" seen in conversations.
//    Here the cut ALWAYS falls on a character boundary.
//
// 2) ZWJ CHAINS COLLAPSED TO A SINGLE ICON. A composite emoji (family,
//    couple, profession...) is SEVERAL Unicode code points joined by U+200D (the
//    "zero width joiner"). This system has only ONE drawing per code point (see the
//    limit documented at the top of this file) - untreated, "family"
//    becomes 3 or 4 loose icons with no visible relation between them, side
//    by side - a line full of meaningless symbols. Here we keep only the FIRST code point of the chain and discard the
//    ZWJ and the rest - a single icon, not perfect (it is not the real composite
//    image), but CONSISTENT: always one per emoji typed, never a
//    random row.
//
// 'out' always ends in NUL. If 'in' is larger than 'out_tam' allows, the
// cut falls on the last character boundary that fits - never in the middle.
void tab5_emoji_preparar(const char* in, char* out, size_t out_tam);
