// SAVE AN RGB565 IMAGE AS .BMP on the SD card.
//
// 16 bits with the colour masks in the header (BI_BITFIELDS) - opens in
// any viewer. Mounts the SD card by itself (and unmounts), so it works from
// any task: the system worker (Camera app) or the console.
//
// WRITING IN LARGE BLOCKS. The previous version (inside the camera HAL)
// wrote one ROW per fwrite (1440 bytes): FAT rewrote partial sectors
// on every call, and 1.4 MB took over 10 s - with the preview
// frozen and the app locked up the whole time. Here the rows are gathered into a
// 32 KB block in PSRAM and written in one go.
//
// Deliberately slow for the UI task: call from the worker
// (sistema_trabalho) or from a background task.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Optional adjustment applied to each row IN THE COPY that goes to the file (the
// source buffer isn't altered). Runs alongside the write, row by row - with no
// extra pass over the whole image in PSRAM.
typedef void (*tab5_bmp_filtro_t)(uint16_t* linha, int largura);

// 'nome' without extension. 'dados' = width x height RGB565, row by row from top
// to bottom (screen order).
bool tab5_bmp_salvar(const char* pasta, const char* nome, const uint16_t* dados, int largura, int altura,
                     tab5_bmp_filtro_t filtro = nullptr);
