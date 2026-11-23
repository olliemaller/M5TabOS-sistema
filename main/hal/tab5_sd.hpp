// Tab5 SD card, treated as the "system disk".
//
// The idea: each application has its own folder in /sdcard/apps/<Nome>/ and doesn't
// need to know anything about mounting, pins or FAT. It asks for a file inside
// its own folder and writes with plain fopen/fprintf/fwrite - the IDF VFS
// does the rest.
//
// WIRING (from M5Unified itself, table _pin_table_sd):
//   CLK=43  CMD=44  D0=39  D1=40  D2=41  D3=42
// Six pins = 4-bit SDMMC, not SPI. Much faster, and the P4 allows
// remapping because it has a GPIO matrix for SDMMC.
//
// REFERENCE-COUNTED MOUNTING: mounting and unmounting takes ~100ms and causes no
// wear, but a mounted card draws power. So whoever needs it calls montar(), whoever
// is done calls desmontar(), and the real mount only happens on the first and the
// unmount on the last. The system does this itself for apps that declared
// usa_sd - the app doesn't need to call either.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"

// THE SDMMC CONTROLLER IS SHARED: the card uses slot 0 and the ESP32-C6 (Wi-Fi,
// via esp_hosted) uses slot 1 of the SAME P4 controller. The IDF only serialises
// TRANSACTIONS; mounting or unmounting a slot (sdmmc_host_init, init_slot,
// deinit_slot, card_init) touches the controller's clock, timeouts, slot count, queue and
// mutex with no lock at all against the other slot. Two mounts
// at the same time (the card and the C6 waking from sleep) caused a driver abort
// (2026-11-13). Whoever mounts or unmounts ANY slot holds this lock:
// tab5_sd (card and flash drive), tab5_wifi (esp_hosted bring-up) and
// tab5_c6_sono (link teardown and return). It is not recursive.
// Lock order: the caller's (card, C6 sleep) first, this one after.
void tab5_sd_controlador_travar(void);
void tab5_sd_controlador_soltar(void);

// Where the card appears in the file system.
#define TAB5_SD_RAIZ   "/sdcard"
#define TAB5_SD_APPS   TAB5_SD_RAIZ "/apps"

// Mounts the card (or just increments the count, if already mounted).
// Returns false if there is no card or mounting failed - and in that case the
// count is NOT incremented.
// Prepares the module. Call ONCE, before any task exists.
void tab5_sd_iniciar(void);

bool tab5_sd_montar(void);

// Decrements the count; really unmounts when it reaches zero.
void tab5_sd_desmontar(void);

bool tab5_sd_montado(void);

// Diagnostics: uptime (us) of the last real mount and unmount (0 = never).
void tab5_sd_ultimas_transicoes(int64_t* montou_us, int64_t* desmontou_us);

// Path of the app's folder, CREATED if it doesn't exist.
//   tab5_sd_pasta("Azimutal", buf, sizeof buf) -> "/sdcard/apps/Azimutal"
// Problematic characters in the name (space, slash, accent) become '_'.
bool tab5_sd_pasta(const char* app, char* out, size_t out_tam);

// The SAME path as tab5_sd_pasta, without creating anything (to know where an
// app's data lives without bringing the folder into existence).
bool tab5_sd_pasta_caminho(const char* app, char* out, size_t out_tam);

// Opens a file INSIDE the app's folder, creating the folder if needed.
// modo is fopen's ("w", "a", "r"...). Returns NULL on any failure.
//   FILE* f = tab5_sd_abrir("Azimutal", "satelites.csv", "a");
FILE* tab5_sd_abrir(const char* app, const char* arquivo, const char* modo);

// "app" accepts a subfolder: "Sistema/logs" -> /sdcard/apps/Sistema/logs (each
// level is created if missing). The system subfolders:
#define TAB5_SD_LOGS    "Sistema/logs"      // log_AAAA-MM-DD.txt
#define TAB5_SD_ENERGIA "Sistema/energia"   // energia-AAAA-MM-DD.csv

// ---------------------------------------------------------------------------
// THE WRITE PORTS - all data the system writes goes through one of them
// ---------------------------------------------------------------------------
// Three ways to write, and only three:
//
//   tab5_sd_anexar      append a line, WITHOUT waiting (goes in a batch)
//   tab5_sd_acrescentar append a block NOW (for those who already batch: the log)
//   tab5_sd_substituir  replace the whole file, never leaving half a file
//
// Why only through them: the card may be LENT to the computer (USB flash drive
// mode, see further down). Then whatever is written through a port goes to the
// journal in internal flash (hal/tab5_reserva) and reaches the card on return, in the
// order it happened. Anyone writing with fopen() directly loses the data at that
// point - and, if it wrote to the card while the computer was using it, would corrupt it.
//
// tab5_sd_abrir() is for READING, and for LARGE new files (photo, firmware
// received over the console): those need the real card, and the system doesn't
// let whoever produces them open while the card is lent (app_t.exige_sd).
//
// ORDER is guaranteed: the three ports, the loan and applying the journal
// all go through the same lock. Replacing first writes whatever was waiting to
// be appended. A single file should not receive through BOTH append
// ports (the anexar batch may go out later).
//
// A READ ALWAYS SEES THE LAST WRITE: tab5_sd_abrir() writes the pending data and
// applies a flash drive session's journal BEFORE opening. Whoever reads with fopen() directly
// (Arquivos) calls tab5_sd_gravar_pendentes() first.
//
// ---- anexar (the writer) -----------------------------------------------------
// Previously, SEVEN modules wrote to the card each in its own way. Now whoever wants
// to append a line HANDS IT IN and moves on: tab5_sd_anexar copies to a buffer
// in PSRAM and returns immediately, without touching the card. A single task ("cartao")
// writes in batches: mounts ONCE, opens each file ONCE, writes everything
// pending for it, in arrival order. Writes:
//   - every 5 s, if anything is pending (with the screen off, up to ~10 s:
//     the task does a round every 5 s when idle);
//   - immediately, for anything requested with 'urgente' (conversation message);
//   - when the buffer passes half full.
//
// 'cabecalho' (optional): written once, if the file is empty on opening
// - the title row of a CSV.
//
// Failed (no card, buffer full): the line is lost and counted in 'perdidas' -
// same treatment as before, when each module simply didn't write.
bool tab5_sd_anexar(const char* app, const char* arquivo, const char* texto,
                    const char* cabecalho = nullptr, bool urgente = false);

// Writes NOW everything pending (and a flash drive session's journal, if the
// card is already back) and waits for it to finish. For anyone about to read outside
// tab5_sd_abrir.
void tab5_sd_gravar_pendentes(void);

// ---- acrescentar and substituir ---------------------------------------------
// They block (card or flash): never from the UI task - use the worker
// (sistema_trabalho) or a ride on the "cartao" task. false = not written (no
// card and no loan, or the journal is full).
bool tab5_sd_acrescentar(const char* app, const char* arquivo, const void* dados, size_t n);

// SIGILOSO: the old content is zeroed before being deleted (passwords) - on FAT,
// deleting only marks the sectors as free.
#define TAB5_SD_SIGILOSO  0x01
bool tab5_sd_substituir(const char* app, const char* arquivo, const void* dados, size_t n,
                        uint8_t flags = 0);

// DELETE a file from the app's folder, through the same lock as the ports above (whatever
// was waiting to be appended goes first). Blocks like them. With the card
// lent to the computer (or the journal still to be applied) it REFUSES: deleting doesn't
// go into the journal. false = not deleted.
bool tab5_sd_apagar(const char* app, const char* arquivo);

// Diagnostics: bytes waiting, lines written and lost since boot.
void tab5_sd_anexos_estado(uint32_t* pendentes, uint32_t* gravadas, uint32_t* perdidas);

// RIDING ON THE "cartao" TASK. A service that only needs, from time to time,
// to flush something of its own to the card (the log) doesn't get its own task: it registers a
// function, and the "cartao" task calls it every round - at most 1 s between
// rounds with the screen on and 5 s when idle, sometimes less (when an urgent
// attachment wakes it). Whoever rides along
// measures its own time (esp_timer), without counting rounds.
// The function runs on the "cartao" stack (6 KB): it may mount and open files, it
// must not wait long (everyone's writes wait along with it). Up to 8 (CARONAS_MAX).
typedef void (*tab5_sd_carona_t)(void);
bool tab5_sd_carona(tab5_sd_carona_t fn);

// Card space, in bytes. Any pointer may be NULL.
bool tab5_sd_espaco(uint64_t* total, uint64_t* livre);

// Size of an already open file, in bytes; the pointer goes back to the start.
// -1 = error. (Replaces the repeated fseek END / ftell / fseek SET trio.)
long tab5_sd_tamanho(FILE* f);

// Short card description to show on screen ("SDHC 29.7GB"), or
// "sem cartao" (no card). Never returns NULL.
const char* tab5_sd_descricao(void);

// The card name (volume label, the same one the computer shows), or ""
// if it has no name or hasn't mounted yet this boot. Never NULL.
const char* tab5_sd_nome(void);

// ---------------------------------------------------------------------------
// LENDING TO THE COMPUTER - USB flash drive mode (services/tab5_pendrive)
// ---------------------------------------------------------------------------
// The card cannot have TWO file-system owners. The Tab5's FAT
// caches the table and directory; so does the computer's. If both
// write, each overwrites what the other thinks is there - and that is a
// corrupted card, not a lost file. That is why lending is exclusive:
//
//   1. whatever was waiting to be written (tab5_sd_anexar) goes to the card first;
//   2. montar() starts refusing, immediately and quietly - for the rest of the system
//      it is as if there were no card (they all already handle that);
//   3. waits for whoever already had it to hand it back, up to 'espera_ms'; past that,
//      gives up (SD_EMPRESTIMO_EM_USO) and everything goes back as it was;
//   4. really unmounts (no Tab5 FAT cache) and initialises the card
//      RAW, sectors only - that is what the computer receives.
//
// While lent, the write ports (above) go to the JOURNAL in
// internal flash. On return the "cartao" task applies it, in order: append
// goes to the end of the file; replace swaps the file IF the computer didn't
// touch it (compared with the size-and-date snapshot taken at lending time) -
// if it did, the computer's version stays and the Tab5's goes to "<nome>.tab5". A
// restart midway loses nothing: the journal is in flash and is applied when
// the card appears.
//
// Blocks (up to espera_ms + card initialisation): never from the UI
// task.
typedef enum {
  SD_EMPRESTADO = 0,
  SD_EMPRESTIMO_EM_USO,      // someone held the card beyond the timeout
  SD_EMPRESTIMO_SEM_CARTAO,  // there is no card, or it didn't respond
  SD_EMPRESTIMO_SEM_DIARIO,  // the "reserva" partition doesn't exist (old table)
} tab5_sd_emprestimo_t;
tab5_sd_emprestimo_t tab5_sd_emprestar(uint32_t espera_ms);

// SECTORS OF THE LENT CARD. Only the loan's owner (the flash drive); nobody
// outside the HAL holds the card. All through the raw card's own lock.
//
// RECOVERY: if a read/write fails, the port resets the SDMMC
// controller, waits for the card to leave busy (up to 10 s), reinitialises the card if it
// doesn't respond and retries - at most 2 times per operation. It can take a while
// (tens of seconds in the worst case). Does NOT write to the log (the caller may be
// in a USB callback): it only counts, in tab5_sd_cru_estat.
bool      tab5_sd_cru_info(uint32_t* setores, uint32_t* tam_setor);   // false = not lent
esp_err_t tab5_sd_cru_ler(uint32_t setor, void* buf, uint32_t n);
esp_err_t tab5_sd_cru_gravar(uint32_t setor, const void* buf, uint32_t n);
typedef struct {
  uint32_t recuperacoes;   // times an operation needed recovery
  uint32_t falhas;         // operations that failed even after recovery
  uint32_t max_ms;         // the slowest operation (including recoveries)
} tab5_sd_cru_estat_t;
void      tab5_sd_cru_estat(tab5_sd_cru_estat_t* e);

// Hands back: releases the raw card, montar() works again and whatever was
// waiting gets written. Call ONLY after the computer has stopped using the
// card (USB disconnected).
void tab5_sd_devolver(void);

bool tab5_sd_emprestado(void);

// What happened to this loan's journal (for the screen).
typedef struct {
  uint32_t aplicadas;     // writes that have already gone to the card
  uint32_t conflitos;     // files the computer changed: the Tab5's version went to .tab5
  uint32_t descartadas;   // records with no way to apply (invalid name)
  uint32_t perdidas;      // did not fit in the journal (since boot)
  uint32_t no_diario;     // still waiting for the card
  uint32_t diario_usado, diario_total;   // bytes
  bool     completo;      // the whole journal has been applied
} tab5_sd_resumo_t;
void tab5_sd_resumo(tab5_sd_resumo_t* r);
