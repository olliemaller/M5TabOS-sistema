// FRAME HAL - the lowest layer of the new display foundation (docs/COMPOSITOR.md,
// stage 1). Two pieces of hardware the old foundation never used.
//
// (The display's priority on the AXI bus does NOT live here: it belongs to
// hal/tab5_barramento, set at boot. A copy of it lived here for a few
// hours and fought with the original.)
//
// 1. END OF FRAME. The driver only switches page when the panel DMA
//    restarts (mipi_dsi_dma_trans_done_cb reads cur_fb_index at that moment),
//    and signals it via on_frame_buf_complete. Waiting for that signal after
//    requesting the switch is what guarantees the outgoing page is no longer
//    being read - only then can it be written without tearing the image.
//
// 2. HARDWARE COPY (pure DMA2D, esp_async_fbcpy; the PPA only as a fallback).
//    Copies a rectangle from one page to the other without the CPU, as a
//    linear run - 3x faster than the PPA's SRM engine (measured: 'tela banco').
//    This is how a frame's changed areas are carried over to the other page.
//
// And an UNDERRUN counter, because without a measurement "it got better" is just opinion.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"

// Rectangle in PHYSICAL framebuffer coordinates (portrait, 720x1280).
typedef struct {
  int16_t x, y, l, a;
} tab5_ret_t;

// Attaches the three pieces to the current panel. Idempotent per handle: idle
// destroys and recreates the panel, and the next call registers again.
bool tab5_quadro_iniciar(esp_lcd_panel_handle_t painel);

// Forgets the panel. Call when destroying the panel (idle).
void tab5_quadro_encerrar(void);

// Discards an end-of-frame signal that arrived earlier. Call RIGHT
// AFTER requesting the switch - see why in tab5_quadro.cpp.
void tab5_quadro_armar_espera(void);

// Waits for the panel to start scanning the new page. false = timed out.
bool tab5_quadro_esperar_troca(uint32_t prazo_ms);

// Copies rectangle 'r' from 'origem' to the SAME place in 'destino' (720x1280
// RGB565 images: the panel pages and the virtual screen), via DMA2D.
// Blocks until finished.
bool tab5_quadro_copiar(void* destino, const void* origem, const tab5_ret_t& r);

// The same, with the SOURCE elsewhere: rectangle 'r' of the destination gets the
// same-sized rectangle starting at (ox, oy) in the source. This is scrolling
// via DMA: the front page, shifted, becomes the back one. Source and destination
// must NOT be the same buffer (the DMA does not handle overlap).
bool tab5_quadro_copiar_de(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy);

// Diagnostics.
uint32_t tab5_quadro_underruns(void);   // underrun notices since boot
uint32_t tab5_quadro_quadros(void);     // frames scanned by the panel

// Frames that were LATE (interval between ends of frame 50% above the normal
// period) since boot. pior_us = the largest interval since the last call
// (and resets it); periodo_us = the measured normal period.
uint32_t tab5_quadro_atrasos(uint32_t* pior_us, uint32_t* periodo_us);

// When (uptime, us) the panel finished scanning the last frame. With the
// period above, this gives the scan PHASE: whoever animates can align to it
// (lock screen). 0 = panel has not scanned anything yet.
int64_t tab5_quadro_ultimo_fim_us(void);

// The last delay: which core the panel ISR runs on, and which task was
// running on each core when it was finally serviced (t0/t1: 16 bytes).
// Returns a counter that goes up with each recorded delay.
uint32_t tab5_quadro_atraso_quem(int* nucleo_isr, char* t0, char* t1);
int64_t  tab5_quadro_atraso_quando(void);   // uptime (us) of the last delay
// Where the core was (mepc) when the late ISR entered - see the .cpp.
uint32_t tab5_quadro_atraso_pc(void);
// Lists all allocated interrupts on the console (core, level, source).
void     tab5_quadro_listar_irqs(void);
// TEST (console 'tela atrasar'): holds core 'nucleo' for 'ms' (1-100),
// 'vezes' times with 1 s between them; critica = portENTER_CRITICAL, otherwise
// interrupts disabled. Blocks until finished. false = invalid argument.
bool     tab5_quadro_prova_atraso(int ms, bool critica, int nucleo, int vezes);

// SAFE WINDOW FOR A LONG CRITICAL SECTION. Between two frames the
// panel DMA has ~0.5 ms to be restarted by the ISR - and a critical section
// (interrupts disabled on the core) that lands in that gap loses a whole
// frame. It was the cause of the "FRAME ATRASADO" with both cores IDLE: the
// monitor's task snapshot (uxTaskGetSystemState, ~0.8 ms with the CPU at 360)
// ran 1x/s at a fixed phase and every so often landed right in the gap - all
// the delays had the same fraction of a second (x.728 s).
//
// This function returns just AFTER an end of frame (the ISR has already
// restarted the DMA): from there it is ~14 ms to the next gap. Anyone about to
// sit in a critical section for more than ~100 us calls this first. Without a
// panel (idle), it returns at the timeout.
bool tab5_quadro_esperar_fim(uint32_t prazo_ms);

// DSI host errors read every frame: returns how many frames have had
// some bit set since boot; st0/st1 = OR of the bits since the last call (and resets).
uint32_t tab5_quadro_erros_dsi(uint32_t* st0, uint32_t* st1);

// HARDWARE FILL (PPA, FILL operation): paints rectangle 'r' of
// 'destino' (720x1280 RGB565) with 'cor' (RGB565 as laid out in memory), without
// the CPU. Blocks until finished. false = unavailable or refused - the caller
// paints with the CPU. The first time it checks the written pixel; if the colour
// does not match (byte order), it disables itself and logs a warning.
bool tab5_quadro_preencher(void* destino, const tab5_ret_t& r, uint16_t cor);

// COPY TEST BENCH. Copies the same rectangle between two temporary
// buffers in PSRAM (allocated and freed here) with each available
// engine - PPA (SRM), pure DMA2D, AXI GDMA and CPU - with the panel scanning
// as usual, and prints ms and MB/s for each. It is the measurement that picks
// the engine for page-to-page copies. Call from the console; takes ~1 s.
void tab5_quadro_banco(void);
