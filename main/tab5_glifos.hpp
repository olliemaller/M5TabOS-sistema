// GLYPH CACHE - smooth text without re-rendering the letter on every draw.
//
// M5GFX draws a VLW letter like this (lgfx_fonts.cpp, VLWfont::drawChar):
// it reads the letter's greyscale bitmap and, for EACH antialiased edge pixel, computes
// the blend with the background and does a 1x1 writeFillRect - plus one per
// background line. A 30 px letter becomes ~200 calls, each one going through the
// compositor's tracker. Per-app measurement showed this is almost all the
// cost of opening an app with text (Tasks, Power, Settings, home).
//
// Here the letter is drawn ONCE, by M5GFX itself, on a scratch sprite - the
// result is identical pixel for pixel - and stored as an RGB565 image for
// that combination of font, colour and background. From the second time on, the whole
// letter is ONE pushImage.
//
// Only what can be reproduced exactly goes into the cache:
//   - text WITH A BACKGROUND (background colour different from the foreground): the letter's
//     block is written in full, background included, and does not depend on what was underneath;
//   - TRANSPARENT text: stores the grey mask (valid for any colour); the
//     block is read ONCE, blended with the same sum as the original and written
//     ONCE - the original reads and writes line by line;
//   - size 1 (fonts at native size, which is the system default);
//   - a letter that does not overlap the previous one (M5GFX does not repaint the background where the
//     previous letter has already painted - there it falls back to the original path).
// Everything else (transparent text, space, a letter missing from the font) goes
// through the original path, with no difference at all.

#pragma once

#include <M5Unified.h>

// An M5GFX VLW font with the cache in front of drawChar. Same type
// (ft_vlw), same loading (loadFont), same metrics - it just draws faster.
struct FonteRapida : public lgfx::VLWfont
{
  size_t drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                  const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                  int32_t& filled_x) const override;
private:
  bool letra_transparente(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c, uint16_t g,
                          const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                          size_t* avanco) const;
};

typedef struct {
  uint32_t acertos;    // letters drawn from the stored image
  uint32_t faltas;     // letters drawn for the 1st time (and stored)
  uint32_t recusas;    // letters that took the original path
  uint32_t rec_espaco;     // ...space (background only: already cheap)
  uint32_t rec_sem_fundo;  // ...transparent text on a canvas that cannot be read
  uint32_t rec_invade;     // ...the previous letter overlaps this one
  uint32_t rec_destino;    // ...canvas that is not 16-bit, or has a palette
  uint32_t rec_escala;     // ...scaled size (setTextSize != 1)
  uint32_t rec_outro;      // ...letter not in the font, too big, out of memory
  uint32_t limpezas;   // times the cache filled up and restarted
  uint32_t entradas;   // letters stored now
  uint32_t bytes;      // PSRAM used by the images now
} glifos_estat_t;

void tab5_glifos_estat(glifos_estat_t* out);

// AN UNROTATED RGB565 CANVAS whose memory transparent text may touch
// directly (a Berry app layer): whoever draws on it registers here, and clears it
// (nullptr) on stopping. Without this, text on a canvas that is not the screen goes through
// M5GFX's readRect, which copies pixel by pixel (~40 us per letter).
void tab5_glifos_canvas_direto(const lgfx::LGFXBase* gfx, uint16_t* buf, int largura, int altura);
