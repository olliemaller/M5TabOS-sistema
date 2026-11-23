// THE SYSTEM DRAWING OBJECT (historical name: "tela virtual", virtual screen).
//
// An ordinary LovyanGFX whose panel is a TRACKER: it records every rectangle that
// any M5GFX primitive touched and hands it to the compositor
// (comp_invalidar). The home screen, the bars and every app draw here, knowing
// nothing of this.
//
// DIRECT MODE (docs/COMPOSITOR.md): the tracker writes to the panel's BACK PAGE.
// The first write of each pass opens the frame (comp_abrir: waits for the
// previous swap and synchronises), and only then is the pixel written. Up to stage 3a
// this was a 1.84 MB canvas in PSRAM that the compositor copied to the
// page every frame - the copy went, and so did the memory.
//
// What still holds, without changing a line of any app:
//   - no drawing happens on the page the panel is scanning;
//   - changed areas are discovered AUTOMATICALLY.
// What changed: the content NO LONGER survives idle (the pages die
// with the panel). On waking, the core redraws everything - as it already did.

#pragma once

#include <M5Unified.h>

// Binds the drawing object to the panel pages and makes it the system's target
// (tab5_tela_definir_alvo). Call once, from the UI task.
bool tela_virtual_iniciar(void);

// The drawing target. Until it exists, it is M5.Display.
lgfx::LovyanGFX& tela_virtual(void);

// Copies M5.Display's rotation (Mesh opens in landscape). Touch is still
// converted by M5.Display; here we only need to draw the same way.
void tela_virtual_seguir_rotacao(void);

// The rectangle (x, y, w, h), in drawing coordinates, will be ENTIRELY
// overwritten by the caller, still within this frame - whatever the back page
// had out of date there need not come from the front. fillRect already says
// this by itself; this is for whoever covers with an image (pushSprite) instead of a colour.
void tela_virtual_cobrir(int x, int y, int w, int h);

// Smooth transparent text, fast: blends the grey mask (w x h,
// 'passo' bytes per row) in the given colour, STRAIGHT onto the back page - only the
// ink pixels are touched. The rectangle arrives already clipped. false = 'gfx'
// is not this object (the caller carries on down the ordinary path).
bool tela_virtual_misturar_mascara(lgfx::LGFXBase* gfx, int x, int y, int w, int h,
                                   const uint8_t* mascara, int passo, uint32_t cor888);

// Pastes an RGB565 image ('passo' pixels per row) at (x, y), w x h, STRAIGHT
// onto the back page, as a full cover. The rectangle arrives already clipped.
// false = 'gfx' is not this object.
bool tela_virtual_colar(lgfx::LGFXBase* gfx, int x, int y, int w, int h, const uint16_t* src, int passo);

// Pastes a full-screen band (whole width, 'alt' rows starting at
// y0) via DMA2D (comp_copiar_faixa). Only with the screen in portrait (rotation 0) and the
// band in DMA memory. false = it did not work (use tela_virtual_colar).
bool tela_virtual_colar_faixa(const uint16_t* faixa, int y0, int alt);

// Requests the whole screen on the next frame (any task).
void tela_virtual_invalidar_tudo(void);

// Returns the DMA surface (sistema_superficie) of the app that closed. Call
// after ao_fechar() - whoever was writing to it (the camera) has already stopped.
void tela_virtual_soltar_superficie(void);
