// H264 - the system's H.264 decoder (components/h264_dec) with the M5tabOS
// policy: where the working memory lives, which tasks it runs in and the
// life cycle. Whoever plays video (services/tab5_video) just asks "open for this
// resolution, decode this packet, give me the frame".
//
// THE DECODER: Main and High (CABAC, B frames, 8x8 transform), progressive,
// 4:2:0, 8 bits, up to 1920 on a side - matches ffmpeg bit for bit
// (testes/rodar_h264.sh). Frames come out in the packed YUV420 format the
// PPA reads directly (O_UYY_E_VYY), in PSRAM: RGB conversion and scaling are
// done by the PPA, not the CPU.
//
// MEMORY (the delicate point on the P4): the working "arena" is what the
// decoder touches all the time (tables, macroblock rows, reference
// window). In internal SRAM it is much faster, but SRAM is contended
// by Wi-Fi (esp_hosted needs its DMA pool), Mesh and MQTT. Measured:
// with the 256 KB L2 cache the arena in PSRAM performs the same, so the default
// (AUTO) is PSRAM - it never knocks anything over. SRAM is left for the bench, and even
// there only if the floor is left over (H264_PISO_INTERNA). The arena is reserved on open and
// returned on close: nothing stays held without video.
//
// THE TASKS: whoever calls tab5_h264_decodificar() does the parsing and prediction;
// a worker on core 1 ("h264_pos") does the deblocking and writes the frame.
// Both use the PIE extension (SIMD): the stacks are internal and aligned
// (MALLOC_CAP_SIMD) - the caller must have been created with
// tab5_h264_criar_tarefa().
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h264_dec.h"

// The internal SRAM that must be left after the arena. Below that, the arena goes
// to PSRAM. 80 KB: the minimum measured in normal use is ~98 KB, and Wi-Fi
// reconnecting asks for peaks of tens of KB.
#define H264_PISO_INTERNA   (80 * 1024)

typedef enum { TAB5_H264_AUTO = 0, TAB5_H264_SRAM, TAB5_H264_PSRAM } tab5_h264_onde_t;

typedef struct tab5_h264 tab5_h264_t;

typedef struct {
  int               max_l, max_a;   // largest frame accepted, up to 1920 (the file gives the real size)
  tab5_h264_onde_t  onde;           // AUTO = PSRAM (the default)
  uint8_t           presos;         // frames the caller holds at once (0 = 2)
} tab5_h264_cfg_t;

typedef struct {
  const uint8_t* px;      // packed YUV420 (O_UYY_E_VYY), rows of l_cod * 3 / 2 bytes
  int     l_cod, a_cod;   // coded size (multiple of 16)
  int     x, y, l, a;     // the visible part (the SPS crop)
  bool    bt709;          // colour matrix (otherwise BT.601)
  bool    faixa_cheia;    // 0-255 (otherwise 16-235)
  int64_t marca;          // what was passed to tab5_h264_decodificar (the pts)
  uint8_t id;             // for tab5_h264_soltar
} tab5_h264_quadro_t;

// Creates the decoder (arena + worker). nullptr = out of memory.
tab5_h264_t* tab5_h264_abrir(const tab5_h264_cfg_t* cfg);
void         tab5_h264_fechar(tab5_h264_t* d);

// One packet (an "access unit"): Annex-B (nal_tam = 0, with 00 00 01) or with
// the size in front (nal_tam = 1, 2 or 4, as in MP4). 'marca' comes back with the frame.
// Returns the decoder's result (H264_DEC_OK / NO_PICTURE = all fine).
h264_dec_result_t tab5_h264_decodificar(tab5_h264_t* d, const uint8_t* dados, size_t n,
                                        uint8_t nal_tam, int64_t marca);
// The next frame in display order, if any. The frame stays HELD (the
// decoder doesn't rewrite it) until tab5_h264_soltar(id).
bool tab5_h264_quadro(tab5_h264_t* d, tab5_h264_quadro_t* q);
void tab5_h264_soltar(tab5_h264_t* d, uint8_t id);
void tab5_h264_fim(tab5_h264_t* d);       // end of file: releases those that were queued
void tab5_h264_recomecar(tab5_h264_t* d); // before jumping (seek): forgets what was in progress
bool tab5_h264_descartavel(const uint8_t* dados, size_t n, uint8_t nal_tam);   // nobody uses it as a reference
const char* tab5_h264_erro(tab5_h264_t* d);
bool tab5_h264_na_sram(const tab5_h264_t* d);   // some piece of the arena ended up in SRAM
size_t tab5_h264_sram_bytes(const tab5_h264_t* d); // how much of the arena is SRAM
size_t tab5_h264_arena_bytes(const tab5_h264_t* d);

// Creates a task that can call the decoder (internal stack with the
// PIE alignment). Same contract as xTaskCreatePinnedToCore.
BaseType_t tab5_h264_criar_tarefa(TaskFunction_t f, const char* nome, uint32_t pilha, void* arg,
                                  UBaseType_t prio, BaseType_t nucleo, TaskHandle_t* h);
void tab5_h264_encerrar_tarefa(void);   // the end of a task created above

// BENCH (console 'video h264'): decodes a whole raw .h264 (Annex-B),
// with no screen, and measures. With hash, prints the FNV-1a of each frame - the same one the
// Mac test (H264_HASH=1) prints: the two lists must be identical.
typedef struct {
  bool     ok;
  char     erro[64];
  uint32_t quadros, com_erro;
  uint32_t ms;               // decoding only (the file is already in memory)
  int      l, a;
  bool     na_sram;
  uint32_t arena_kb, sram_kb;  // the whole arena and how much of it is SRAM
  uint32_t interna_livre_kb, interna_min_kb;   // SRAM during the bench run
  uint32_t cpu_mhz;          // the clock at the end of the measurement
} tab5_h264_bancada_t;
bool tab5_h264_bancada(const char* caminho, tab5_h264_onde_t onde, bool hash, tab5_h264_bancada_t* r);
// The same, without waiting: the result goes to the log ("h264: bancada: ..."). You can
// look at 'tarefas' while it measures.
bool tab5_h264_bancada_fundo(const char* caminho, tab5_h264_onde_t onde);
