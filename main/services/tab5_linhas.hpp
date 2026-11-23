// LINHAS (LINES) - read a LARGE text file, line by line, through the fast reader.
//
// WHY IT EXISTS. fgets on the SD card stutters (fread gives 52-572 KB/s, see
// tab5_volumes.hpp); tab5_leitor reaches 4.6 MB/s, but hands over bytes, not
// lines. This module is the bridge: whoever reads CSV (the track, the satellite
// log) asks for the next line and doesn't worry about blocks.
//
//   tab5_linhas_t* l = tab5_linhas_abrir("/sdcard/apps/Trilha/trilha_0001.csv");
//   char linha[112];
//   while (l && tab5_linhas_proxima(l, linha, sizeof(linha))) { ... }
//   tab5_linhas_fechar(l);
//
// Blocks (mounts the disk): outside the UI task when the file is
// large. A line reader belongs to ONE task.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct tab5_linhas tab5_linhas_t;

// nullptr = failed (no disk or file doesn't exist; the disk has already been released).
tab5_linhas_t* tab5_linhas_abrir(const char* caminho);
// The next line, WITHOUT the '\n' (or the '\r'). false at the end of the file. A line
// longer than 'tam' - 1 is cut: the rest of it is discarded, and '*cortada' (if
// given) becomes true.
bool tab5_linhas_proxima(tab5_linhas_t* l, char* out, size_t tam, bool* cortada = nullptr);
// Jumps to byte 'pos' (start of a line, from tab5_linhas_pos).
// false = couldn't (reading carries on from where it was).
bool tab5_linhas_ir(tab5_linhas_t* l, uint64_t pos);
// Position, in bytes, of the next byte to hand over.
uint64_t tab5_linhas_pos(const tab5_linhas_t* l);
void tab5_linhas_fechar(tab5_linhas_t* l);   // closes and releases the disk; accepts nullptr
