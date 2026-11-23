// NOTES - short texts, one .txt per note in the app's folder on the SD card
// (/sdcard/apps/Notas/nota_0001.txt).
//
// ONLY THROUGH THE SYSTEM GATEWAYS: writing is tab5_sd_substituir (whole swap,
// the same lock as everyone else, journal while the SD card is lent out), reading is
// tab5_sd_abrir, deleting is tab5_sd_apagar, listing is tab5_arq_listar. No
// fopen/remove/mkdir on the side.
//
// THE FILE NAME IS FIXED (a number); the TITLE is the first line of the text and
// the PREVIEW is the next one with text, read when listing (as in phone
// notes apps). Changing the first line renames nothing.
//
// They all BLOCK (they touch the SD card): call from the worker (sistema_trabalho),
// never from the UI task.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define NOTAS_APP       "Notas"
#define NOTA_MAX        4096   // text bytes, including the '\0'
#define NOTA_ARQ_MAX    24     // "nota_0001.txt", including the '\0'
#define NOTA_TITULO_MAX 48

typedef struct {
  char    arquivo[NOTA_ARQ_MAX];
  char    titulo[NOTA_TITULO_MAX];
  char    previa[NOTA_TITULO_MAX];   // the second line with text ("" = title only)
  int64_t data;              // last change (epoch), 0 = unknown
} nota_t;

// The notes, newest first. 'prox' receives the file for the next
// new note. -1 = no SD card.
int  notas_listar(nota_t* out, int max, char prox[NOTA_ARQ_MAX]);
bool notas_ler(const char* arquivo, char* buf, int cap);
// Empty text deletes the note.
bool notas_gravar(const char* arquivo, const char* texto);
bool notas_apagar(const char* arquivo);

// WRITES IN THE BACKGROUND, WITHOUT WAITING: keeps a copy of the text and writes it in a detached
// job (sistema_trabalho_solto) - the caller can close and free its own
// buffer before the SD card finishes. May be called from the UI.
// false = out of memory or no slot for the job (nothing was written).
bool notas_gravar_em_fundo(const char* arquivo, const char* texto);
