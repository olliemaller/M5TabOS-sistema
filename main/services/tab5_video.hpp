// SYSTEM VIDEO PLAYER - plays from the SD card (or the USB flash drive):
//   - .mp4/.m4v/.mov with H.264 (phone and internet video), with the AAC
//     audio inside the file;
//   - .avi MJPEG, with the audio from a .mp3 of the SAME NAME next to it
//     ("filme.avi" + "filme.mp3").
//
// USERS ARE UI ONLY. The Video app (main/app_video) lists, asks to open,
// shows the frame and the buttons. None of them reads the card, decodes or
// plays sound: the service does everything, in its own task ("video").
//
// HOW IT WORKS
//   - OPENING is quick (it only starts the task). The task opens the file through
//     the tab5_volumes reader, reads the AVI index (idx1; without it, scans 'movi'),
//     starts the P4's HARDWARE JPEG decoder and, if the .mp3 exists, asks the
//     music player (services/tab5_musica) to play it.
//   - THE AUDIO RULES THE CLOCK. The video clock is the audio position at the
//     speaker (tab5_musica_estado.pos_ms): the frame shown is the one that
//     matches it. If decoding falls behind, it SKIPS frames (counted in
//     'perdidos') instead of delaying the rest. Without a .mp3, the clock is
//     esp_timer; if the audio ends before the video, the video carries on alone.
//   - THE READY FRAME is RGB565 in two buffers (double buffering). The displayer
//     receives the AVISO_VIDEO notice (new frame, pause, end, error) and asks for
//     the frame; the app needs no clock.
//   - SCALING AND COPYING TO THE SCREEN is the service's job (tab5_video_pintar, via
//     the PPA): the app only says in which rectangle of the DMA surface
//     (sistema_superficie) the video should appear.
//
// MP4: the H.264 decoder runs on the CPU (services/tab5_h264): 360p smooth,
// 720p and 1080p choppy (the lateness policy skips frames so the video never
// falls behind the audio). A 1080p with many reference frames does not fit
// in PSRAM (error "too heavy for memory"). See "O MP4" (THE MP4) in the .cpp.
//
// AVI LIMITS: MJPEG only (fccHandler 'MJPG'); frames up to TAB5_VIDEO_L_MAX x
// TAB5_VIDEO_A_MAX; the first RIFF segment (an OpenDML AVI larger than 1 GB plays
// only the first). A well-made .avi (idx1) opens instantly; without an index, the
// scan reads the whole file (seconds to minutes).
//
// TASKS: everything here can be called from any task (UI, console).
// The controls only flag a request; the video task is the one that handles it.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TAB5_VIDEO_L_MAX        1920          // largest accepted width (pixels)
#define TAB5_VIDEO_A_MAX        1088          // largest accepted height (pixels)
#define TAB5_VIDEO_QUADRO_MAX   (2u << 20)    // largest accepted JPEG frame (bytes); anything larger repeats the previous one
#define TAB5_VIDEO_QUADROS_MAX  1000000u      // frames in the table (8 bytes each, in PSRAM)

// The extensions the player opens, in the tab5_arq_listar filter format.
#define TAB5_VIDEO_EXTENSOES    ".avi;.mp4;.m4v;.mov"

// Is the extension one of the above (upper or lower case)? Does not touch the disk.
bool tab5_video_suportado(const char* nome);

// OPENS 'caminho' (absolute: "/sdcard/Videos/filme.mp4") and starts playing, with
// the audio inside (MP4) or from "filme.mp3" in the same folder (AVI). Closes
// whatever was open. false = an extension it does not play or out of memory; a file
// that does not open or is not in an accepted format shows up later in
// tab5_video_estado().erro.
bool tab5_video_abrir(const char* caminho);

void tab5_video_pausar_tocar(void);   // pause; at the end of the video, plays again from the start
void tab5_video_ir_ms(uint32_t ms);   // seeks to the point (the audio follows)
void tab5_video_fechar(void);         // stops everything and releases the disk and memory; stops the audio

typedef struct {
  bool     aberto;      // a video is open (including opening or with an error)
  bool     tocando;     // false = paused, at the end or with an error
  bool     com_som;     // the clock is the .mp3's
  bool     fim;         // reached the end
  bool     erro;        // it failed (erro_txt says why)
  uint32_t pos_ms;      // where it is
  uint32_t dur_ms;      // how long it lasts (0 = still opening)
  uint32_t quadros;     // frames in the file
  uint32_t mostrados;   // frames decoded and published
  uint32_t perdidos;    // frames skipped because the clock had already passed them
  int      l, a;        // frame size (0 = still opening)
  float    fps;         // frames shown per second (last second)
  char     nome[64];    // file name, without the extension
  char     erro_txt[64];
} tab5_video_estado_t;
void tab5_video_estado(tab5_video_estado_t* e);

// THE NEWEST READY FRAME (panel RGB565). 'passo' = pixels per line of the
// buffer (the width rounded up to 16); the useful image is l x a. false = no
// new frame since the last take. The buffer belongs to the caller until
// tab5_video_soltar_quadro() (one at a time).
bool tab5_video_pegar_quadro(const uint16_t** px, int* l, int* a, int* passo);
void tab5_video_soltar_quadro(void);

// SCALES the frame to fit the rectangle (x, y, l, a) of an RGB565 surface
// 'pic_l' x 'pic_a' (sistema_superficie) - keeping the aspect ratio and centred,
// via the PPA, without the CPU - and blacks out the rest of the rectangle.
// 'so_se_novo' = false repaints the last frame (back from idle, the area was
// cleared). Call from the UI task. true = painted (notify sistema_superficie_mudou).
bool tab5_video_pintar(void* destino, size_t destino_bytes, int pic_l, int pic_a,
                       int x, int y, int l, int a, bool so_se_novo);
