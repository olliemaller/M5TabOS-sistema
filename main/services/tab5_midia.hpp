// MEDIA - the bridge between the media libraries (components/medf: MP4 and
// other container readers; components/audf: sound decoders such as AAC) and
// the system.
//
// medf reads the file through functions the system registers: here they use
// the tab5_volumes reader (SD card or USB flash drive; the disk stays safe
// while the file is open). Anyone using medf calls tab5_midia_preparar() first.
#pragma once
#include <stddef.h>
#include "medf_buffer.h"

// Registers the file functions with medf (once; calling again does nothing).
void tab5_midia_preparar(void);

// The read memory for a file (medf's "arena"), in PSRAM and with the
// alignment it asks for. 'bytes' in multiples of 64 KB; the largest packet
// that can be read in one go is 1/4 of that. false = out of memory.
bool tab5_midia_arena(medf_arena_t* a, size_t bytes);
void tab5_midia_arena_soltar(medf_arena_t* a);
