// THE PANEL'S TWO PAGES, and the area drawing primitives.
//
// The Tab5's MIPI-DSI panel scans a framebuffer in PSRAM non-stop. The
// IDF driver already allocates TWO (num_fbs = 2 in M5GFX's Panel_DSI) and switches
// page just by changing an index (esp_lcd_panel_dpi.c:506-524) - the switch takes effect at the
// next end of frame.
//
// The ONLY thing that writes to the pages is the compositor (kernel/compositor.cpp). All the
// rest of the system draws on the virtual screen (kernel/tela_virtual.cpp), which the
// panel never reads. See docs/COMPOSITOR.md.
//
// HISTORY, for anyone surprised by the size: this module had
// ~800 lines of defence against writing to the page the panel scans - bands,
// breathing gaps, double buffering only for full repaints, scrolling racing against
// the beam. The root cause was PSRAM bandwidth at half (L2 cache line
// of 64B instead of 128B) combined with drawing on the visible page. Both
// are gone: the compositor draws behind, and the cache line is 128B.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <M5Unified.h>
#include "tab5_quadro.hpp"

// Prepares the two pages. Idempotent: detects by itself when the panel was
// recreated (idle destroys and rebuilds the DSI, and the framebuffers change).
bool tab5_tela_iniciar(void);

// Frees what was set up. Call BEFORE destroying the panel (idle).
void tab5_tela_encerrar(void);

// PAGE EPOCH: goes up whenever the pages stop being valid (panel
// shut down) or are born again. Free to read - it is what lets the compositor
// check "does my page still exist?" on every write without calling anything.
extern std::atomic<uint32_t> g_tab5_tela_epoca;

// Goes up with each new panel. The compositor uses it to know that the pages it
// knew have died (and that the whole screen needs to go to the new ones).
uint32_t tab5_tela_geracao(void);

// ---------------------------------------------------------------------------
// COMPOSITOR PRIMITIVES - nobody else should use them
// ---------------------------------------------------------------------------
// Back page (true) or front page (false).
void* tab5_tela_pagina(bool tras);

// Requests the switch WITHOUT waiting. The caller waits with tab5_quadro_esperar_troca()
// before touching the back page again. [y0, y1) limits the driver's msync.
bool tab5_tela_pedir_troca(int y0, int y1);

// ---------------------------------------------------------------------------
// AREA DRAWING, on the system target
// ---------------------------------------------------------------------------
// Where clear/scroll write to. The kernel points it at the virtual screen at
// boot; nullptr = M5.Display (only if the virtual screen cannot exist).
void tab5_tela_definir_alvo(lgfx::LovyanGFX* d);

// Paints a rectangle in one colour. Skips the work when the area is already
// known to be that colour (see tab5_tela_marcar_limpo) - app writers ask for
// clears always, unconditionally; the decision is made here.
void tab5_tela_limpar(int x, int y, int largura, int altura, uint16_t cor);

// FOR THE SYSTEM: "this rectangle has just become entirely this colour". Valid
// for ONE drawing pass. Avoids the double clear (the system clears the
// work area and then the app asks for the same clear).
void tab5_tela_marcar_limpo(int x, int y, int largura, int altura, uint16_t cor);
void tab5_tela_esquecer_limpo(void);
uint32_t tab5_tela_limpezas_evitadas(void);   // diagnostics

// Shifts the content of an area by 'delta' lines (>0 up, <0 down) and paints
// the exposed strip with 'cor'. Reusing already drawn pixels saves CPU when
// scrolling. false = |delta| >= height, nothing to reuse.
bool tab5_tela_rolar(int x, int y, int largura, int altura, int delta, uint16_t cor);

// Diagnostics: how much scrolling has cost the CPU since boot - pixels shifted
// by copyRect and the time spent on it (us). Cumulative, never reset: whoever
// measures keeps a snapshot and takes the difference.
void tab5_tela_rolagem_custo(uint64_t* px, uint64_t* us);

// OBSOLETE - does nothing. It was the write "budget" for
// breathing between bursts on the visible page. It stays only because the Cobra app (game
// tab) still calls it; it goes when that stops calling it.
static inline void tab5_tela_gastou(size_t) { }
