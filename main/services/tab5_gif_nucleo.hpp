// GIF DECODER CORE - only bytes and pixels, no screen or disk.
//
// Isolated on purpose: it doesn't include LovyanGFX, ESP-IDF or the SD card, and so
// it runs ON THE MAC (testes/gif_teste.cpp). Its user is services/tab5_gif.cpp, which
// brings the file into memory and draws the result on screen.
//
// In goes the whole GIF file (GIF87a/89a) in a memory area; out comes, frame by
// frame, the "logical screen" in RGB565 (natural value, 16 bits) already composited with the
// previous frames. The caller does the allocating (alocar/liberar): on the device
// PSRAM, in the test malloc.
//
// Understands: global and local colour tables, LZW (codes of 2 to 12 bits), interlaced
// frames, Graphic Control Extension (delay, transparent colour and disposal
// 0/1 = keep, 2 = background, 3 = restore previous) and skips the other extensions.
// A corrupt file never reads outside the area: every index is checked.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define GIF_TELA_MAX 4096   // maximum side of the logical screen

typedef struct gif_nucleo gif_nucleo_t;

typedef void* (*gif_alocar_t)(size_t bytes);
typedef void  (*gif_liberar_t)(void* p);

// Reads the header and prepares. 'dados' must stay valid until gif_n_fechar.
// nullptr = not a GIF, screen larger than GIF_TELA_MAX, no frames at all or out of memory.
gif_nucleo_t* gif_n_abrir(const uint8_t* dados, size_t n, gif_alocar_t alocar, gif_liberar_t liberar);
void          gif_n_fechar(gif_nucleo_t* g);

int  gif_n_largura(const gif_nucleo_t* g);
int  gif_n_altura(const gif_nucleo_t* g);
bool gif_n_animado(const gif_nucleo_t* g);          // the file has more than one frame
int  gif_n_quadros_vistos(const gif_nucleo_t* g);   // how many it has decoded so far (counts the loops)

// The logical screen (width x height, natural RGB565, row by row from top to bottom).
const uint16_t* gif_n_tela(const gif_nucleo_t* g);

// Decodes the NEXT frame over the logical screen. Returns its delay in
// ms (minimum 20; no delay in the file = 100) or -1 on error. At the end of the file
// it goes back to the first frame (loop).
int gif_n_proximo(gif_nucleo_t* g);
