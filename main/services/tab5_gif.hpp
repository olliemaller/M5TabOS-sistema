// GIF (animated or not) on screen. The decoder itself is tab5_gif_nucleo (only
// bytes and pixels, tested on the Mac); here come the file and the drawing.
//
// The file comes WHOLE into PSRAM (up to 16 MB): a GIF decodes frame by
// frame and loops back to the first, so reading from the SD card on every frame would be slow and
// would keep the disk held. Once opened the disk is already released.
//
//   tab5_gif_t* g = tab5_gif_abrir("/sdcard/gato.gif");     // worker
// worker; draws a frame
//   ...wait 'atraso' ms and repeat...
//   tab5_gif_fechar(g);
//
// Blocks (disk and decoding): run on the worker (sistema_trabalho), never
// on the UI. Closing only frees memory and can be done from any task.
#pragma once

#include "app.h"   // lgfx::LovyanGFX

typedef struct tab5_gif tab5_gif_t;

// nullptr = didn't open (not a GIF, larger than 16 MB, logical screen larger than
// 4096x4096, out of memory or no disk).
tab5_gif_t* tab5_gif_abrir(const char* caminho);

int  tab5_gif_largura(const tab5_gif_t* g);   // of the logical screen
int  tab5_gif_altura(const tab5_gif_t* g);
int  tab5_gif_quadros_vistos(const tab5_gif_t* g);   // how many it has decoded so far
bool tab5_gif_animado(const tab5_gif_t* g);          // more than one frame

// Decodes the NEXT frame onto the logical screen (compositing with the previous one) and
// draws the whole logical screen into 'destino', scaled down to fit (never
// enlarged), centred, on a black background. The black background is painted on the first
// call: use a destination for this GIF only. Returns the frame's delay in ms
// (minimum 20) or -1 on error. At the end of the file it goes back to the first frame.
int tab5_gif_proximo(tab5_gif_t* g, lgfx::LovyanGFX* destino);

void tab5_gif_fechar(tab5_gif_t* g);
