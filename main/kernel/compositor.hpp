// COMPOSITOR - the system's display foundation (docs/COMPOSITOR.md).
//
// DIRECT MODE (LVGL's "direct mode" with two pages). All drawing lands
// DIRECTLY on the panel's back page - there is no longer a third image
// in between. The drawing object (kernel/tela_virtual) notes every rectangle
// touched (comp_invalidar), and the frame has three phases:
//
//   1. OPEN (lazy, on the round's first drawing - comp_abrir):
//      waits for the previous frame's switch to finish (only then is the back page
//      free). The areas the previous frame drew become PENDING:
//      each comes from the front only when something is about to write or read it
//      (comp_preparar), and the rest at presentation. One that a DMA copy
//      overwrites entirely (scrolling) is never copied.
//   2. DRAW on the back page, as usual.
//   3. PRESENT (comp_apresentar, once per round): writes back from cache the
//      drawn lines and requests the switch. It does NOT wait - the next OPEN
//      does the waiting.
//
// Each changed pixel passes through PSRAM 3 times (the CPU writes; the sync reads and
// writes once), not 5 as with the virtual screen. And without the virtual screen
// 1.84 MB of PSRAM is left over.
//
// Physical coordinates (portrait). Call only from the UI task.

#pragma once

#include <M5Unified.h>
#include "tab5_quadro.hpp"

// Marks a rectangle as drawn in this frame.
void comp_invalidar(const tab5_ret_t& r);

// Requests the whole screen in the next frame. May be called from ANY task
// (the console uses it): it only arms a request that the UI serves when presenting.
void comp_invalidar_tudo(void);

// Opens the frame, if not already open, and returns the back page
// (where to draw). nullptr = no panel (idle, clock change).
void* comp_abrir(void);

// Before writing OR READING 'r' on the back page: brings from the front what the
// previous frame changed there and the back page doesn't have yet (lazy sync).
// The tracker calls this itself; nobody else needs to.
void comp_preparar(const tab5_ret_t& r);

// The same, for whoever is going to write 'r' WHOLE and opaque (fillRect, DMA
// copy): whatever was pending inside 'r' is discarded without copying - it was going to be
// overwritten. Only what lies outside 'r' stays pending.
void comp_cobrir(const tab5_ret_t& r);

// Has rectangle 'src' already been drawn IN THIS frame? (Scrolling only copies from the
// front page what the front has identical to the back.)
bool comp_desenhado_neste_quadro(const tab5_ret_t& src);

// Moves rectangle 'r' from the FRONT page, starting at (ox, oy), to the
// same size at 'r' on the back one, via DMA. Opens the frame. false = couldn't
// (the caller does it with the CPU).
bool comp_copiar_da_frente(const tab5_ret_t& r, int ox, int oy);

// Moves rectangle 'r' from a 720x1280 RGB565 image (an app's DMA surface
// - camera) to the back page, via DMA, and marks it.
bool comp_copiar_imagem(const void* imagem, const tab5_ret_t& r);

// Moves a full-width screen BAND (FB_L x 'alt' RGB565 lines, contiguous, in
// memory the DMA can reach) to lines y0..y0+alt of the back page,
// via DMA2D, and marks it covered and drawn. It is how whoever builds the screen
// piece by piece in fast memory (the boot animation) delivers each piece
// without the CPU copying pixel by pixel in PSRAM. false = couldn't (the caller
// pastes with the CPU).
bool comp_copiar_faixa(const void* faixa, int y0, int alt);

// Presents the frame, if something was drawn (false = nothing to do, or no
// panel - in that case the frame is discarded).
bool comp_apresentar(void);

// Is there something drawn waiting to be presented?
bool comp_tem_sujeira(void);

// Waits for the switch requested by the last frame to actually happen. For whoever
// needs the frame ON SCREEN before continuing (turning the backlight on at wake).
void comp_esperar_troca(void);

// THE PANEL IS NEW (boot, idle, clock change): both pages
// were born with nothing of ours, and there is nowhere to sync from. Returns true ONCE
// per new panel - the core responds with a full redraw. Call only with
// the panel up (outside idle).
bool comp_painel_novo(void);

// Counts how many times the area list has been consumed. Whoever keeps "I've already
// invalidated this" uses it to know the list is new.
uint32_t comp_rodada(void);

// Diagnostics: uptime (us) of the last assembled frame and how many pixels it took.
void comp_ultimo_quadro(int64_t* quando_us, uint32_t* px);

// ---------------------------------------------------------------------------
// MEASUREMENT - the cost of each frame, to compare before and after changes.
// ---------------------------------------------------------------------------
// Everything is CUMULATIVE since boot and never reset: the console and the monitor read at
// the same time, and each keeps its own snapshot and takes the difference. That way no
// reader wipes out the other's window.
//
// Histogram of the cost of the ROUND that produced a frame (from the UI waking up to
// the switch being requested), in PANEL PERIODS: fitted in 1 scan, in 2, in 3,
// or more. It is the number that decides smoothness - a round that exceeds 1 period
// leaves the previous frame on screen for one more scan.
#define COMP_FAIXAS 4

typedef struct {
  uint32_t quadros;               // frames presented
  uint32_t cheios;                // frames that changed >= 90% of the screen
  uint32_t estouros;              // times the area list filled up
  uint64_t px_quadro;             // pixels drawn (presented areas)
  uint64_t px_sinc;               // sync pixels (front -> back)
  uint64_t px_rolar_dma;          // scroll pixels copied via DMA
  uint64_t us_copia;              // time blocked in DMA copies (all)
  uint64_t us_espera;             // time waiting for the previous frame's switch
  uint64_t us_desenho;            // CPU drawing (reported by the core, excluding copies)
  uint32_t n_desenho;             // drawing passes measured
  uint64_t us_desenho_completo;   // ...only the FULL passes (opening an app, home screen)
  uint32_t n_desenho_completo;
  uint64_t us_volta;              // whole rounds that produced a frame
  uint32_t voltas[COMP_FAIXAS];   // histogram of the cost of those rounds, in periods
  uint64_t px_rolados;            // total scrolling (hal/tab5_tela: DMA or CPU)
  uint64_t us_rolar;
} comp_estat_t;

void comp_estat(comp_estat_t* out);

// The core measures the drawing pass between comp_marcar_desenho() and
// comp_contar_desenho(us); the time the compositor itself spent in between
// (opening, syncing, copying) is subtracted - "drawing" is just the CPU painting.
void comp_marcar_desenho(void);
// 'completo' = the pass redrew the whole screen (opening/closing an app).
// Returns the pass's CPU time (us, excluding the compositor's time).
uint32_t comp_contar_desenho(uint32_t us, bool completo);
void comp_contar_volta(uint32_t us);
