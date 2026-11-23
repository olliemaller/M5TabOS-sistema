// LOCK SCREEN ANIMATIONS.
//
// Each time the lock screen appears, a different animation from the set
// is chosen at random and keeps running BEHIND the text until unlock.
//
// Rules that keep this light:
//   - the state of all animations shares a single static union
//     (~2 KB); only the chosen one is alive;
//   - no repainting the screen: each object erases only its own box (the old
//     and the new in one) and draws itself again. All shapes are without
//     anti-aliasing, so drawing twice in the same place is harmless;
//   - overlapping boxes become one (nothing is erased twice);
//   - the frame comes out at the system frame pace (kernel/ritmo.hpp), like
//     any periodic app's, and advances by its sistema_quadro_dt();
//   - the background (abstract lines) is an image painted once per lock
//     (PSRAM, only while locked): "erasing" a box means copying its
//     piece, the same cost as painting black;
//   - the text (time, date, notice, hint) stays in front: the shell redraws it,
//     clipped, only where the animation touched.
//
// Call only from the UI task, from inside desenhar_conteudo().

#pragma once

#include <M5Unified.h>
#include "app.h"

typedef struct {
  int16_t x, y, w, h;
} bq_ret_t;

// Chooses the animation for the next display (never the same one twice in a row).
void bq_anim_sortear(void);

// The period requested while locked (~30 f/s). The system rounds it to
// whole panel scans (2, at any clock).
static const uint32_t BQ_ANIM_PERIODO_MS = 33;

// Frees the background memory (unlock, idle). The next display
// paints another one.
void bq_anim_soltar(void);

// Starts the chosen animation on a freshly cleared (black) area.
void bq_anim_iniciar(const area_t& a);

// Advances and draws one frame. 'extras' are rectangles the shell has just
// had to erase (text that changed): the animation erases them and redraws itself in them.
// 'caixas' are the text boxes; for each one, 'clips' returns the area (already
// erased, w == 0 if the animation didn't touch it) where the text should be repainted -
// ONCE, clipped to it.
void bq_anim_quadro(lgfx::LovyanGFX& d, const bq_ret_t* extras, int n_extras,
                    const bq_ret_t* caixas, int n_caixas, bq_ret_t* clips);

// HOME SCREEN WALLPAPER: one of the abstract background drawings (style 0..4:
// threads, waves, web, rays, circles - the order of AJUSTES_PAREDE_*), painted
// on area 'a'. The same seed always gives the same drawing. false = no memory
// or invalid style (the area stays as it was - black).
bool bq_fundo_parede(lgfx::LovyanGFX& d, const area_t& a, int estilo, uint32_t semente);
// The finished wallpaper stays cached (PSRAM, the size of the area) while
// style, seed and area are the same - the home screen comes back without generating it again.
// Release: on opening an app and when idle (the kernel calls it).
void bq_parede_soltar(void);
