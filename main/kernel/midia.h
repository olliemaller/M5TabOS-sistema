// NOW PLAYING - what is playing, for the SYSTEM to show and control.
//
// Music plays in an app's background part (kernel/fundo.h), with the window
// closed. Someone in another app still needs to see what is playing and pause without
// opening the player: the bar shows a pill of its own (note + title), which
// sits alongside the notices pill, and the notification centre (tap the bar)
// gains a mini player at the bottom - previous, play/pause, next and the progress
// line.
//
// A single slot: one player at a time. Whoever plays PUBLISHES the state (from
// any task) and registers the controls; the system only reads and calls.
//
// The controls run ON THE UI TASK: they must only mark a request for the
// background part to serve (no waiting, no I2C, no card).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MIDIA_TITULO 64

typedef struct {
  void (*tocar_pausar)(void);
  void (*proxima)(void);
  void (*anterior)(void);
  void (*ir_para)(uint32_t ms);   // optional (nullptr = the progress line does not seek)
} midia_controles_t;

// Takes the slot. 'fonte' = the name of the background part that plays (the centre removes
// its card: the mini player already is the card); 'app' = the app that tapping the
// title opens. Both must outlive the call (literal or static).
void midia_assumir(const char* fonte, const char* app, const midia_controles_t* c);
// Releases the slot (stopped playing for good): the pill and the mini player disappear.
void midia_soltar(void);

// The current state. titulo nullptr = keeps the previous one.
void midia_publicar(const char* titulo, bool tocando, uint32_t pos_ms, uint32_t dur_ms);

typedef struct {
  bool        ativa;
  bool        tocando;
  bool        busca;      // has ir_para
  const char* fonte;      // the background part that plays
  const char* app;        // the app the mini player opens
  char        titulo[MIDIA_TITULO];
  uint32_t    pos_ms, dur_ms;
  uint16_t    cor;        // the player's accent colour (RGB565); 0 = the system's (lilac)
} midia_info_t;

// The player's accent colour (its theme): the bar pill and the
// mini player start using this. 0 = the system's.
void midia_definir_cor(uint16_t cor565);

bool     midia_ler(midia_info_t* out);   // false = nothing playing (out zeroed)
// Changes when the title, playing/paused or the slot change - what the BAR shows.
uint32_t midia_versao(void);
// Also changes every second of position - what the MINI PLAYER shows.
uint32_t midia_versao_posicao(void);

// For whoever draws (the UI): passes on to the slot owner.
void midia_tocar_pausar(void);
void midia_proxima(void);
void midia_anterior(void);
void midia_ir_para(uint32_t ms);
