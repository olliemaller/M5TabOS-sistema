// IMAGEM (IMAGE) - open and draw an image from any disk, of any size.
//
// This service does the decoding; the app only provides the box the image has to
// fit in. Formats (recognised by CONTENT, not by extension): JPEG
// (baseline; progressive isn't supported), PNG, BMP, QOI and GIF.
//
// NO FILE SIZE LIMIT. The file is read bit by bit by the fast reader
// (tab5_leitor, 64 KB blocks) and decoded as it arrives; the
// memory used is the decoder's, not the file's. The exception is GIF, which
// comes whole into PSRAM (up to 16 MB, see tab5_gif.hpp).
//
// Everything here BLOCKS (disk + decoding): run on the worker (sistema_trabalho),
// never on the UI task.
#pragma once

#include "app.h"   // lgfx::LovyanGFX

// Extensions the system opens as images: .jpg .jpeg .png .bmp .qoi .gif
bool tab5_imagem_suportada(const char* nome);

// Draws the WHOLE image inside 'destino' (an already allocated canvas/sprite):
// at the largest scale that fits, only SHRINKS (never enlarges), centred, black background.
// 'cancelado' (may be nullptr) is polled during the read; if it returns
// true, it stops and returns false. Also false if it didn't read/recognise the image.
// A GIF shows its first frame (to animate, see tab5_gif.hpp).
bool tab5_imagem_desenhar(const char* caminho, lgfx::LovyanGFX* destino, bool (*cancelado)(void));

// Real width and height, reading only the header. false if not recognised.
bool tab5_imagem_tamanho(const char* caminho, int* largura, int* altura);

// The size of an l x a image scaled down to fit in L x A (same aspect ratio;
// never larger than the original). Also used by GIF.
void tab5_imagem_caber(int l, int a, int L, int A, int* saida_l, int* saida_a);
