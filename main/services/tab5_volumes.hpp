// VOLUMES - the device's disks, seen from a single place.
//
// The system has more than one place where files live: the SD card (/sdcard,
// hal/tab5_sd) and the flash drive on USB-A (/usb, services/tab5_usb). Code that
// works with FILES - playing a song, listing a folder, copying - should not
// need to know which of the two the path came from, nor how each one mounts. This
// module is that door: everything by ABSOLUTE PATH ("/usb/Rock/x.mp3").
//
//   if (tab5_volume_montar(caminho)) {      // holds the disk where it lives
//     int fd = open(caminho, O_RDONLY);
//     ...
//     tab5_volume_desmontar(caminho);       // releases
//   }
//
// MOUNT = HOLD. The card really mounts on the first reference (see
// tab5_sd.hpp); the flash drive is already mounted while plugged in, and holding
// it only makes whoever removes it WAIT for readers to finish (up to 3 s).
// Whoever holds it for a long time (the player) asks tab5_volume_disponivel() on
// every block and lets go when it turns false: the card was lent to the
// computer, or the flash drive was yanked.
//
// CHANGED: when a disk arrives or leaves, the system publishes AVISO_VOLUMES (app.h)
// - whoever lists volumes reloads.
//
// Everything here is safe from any task, but mounting the card blocks
// (~100 ms): keep it off the UI task.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

#define TAB5_VOL_CARTAO_RAIZ    "/sdcard"   // = TAB5_SD_RAIZ
#define TAB5_VOL_PENDRIVE_RAIZ  "/usb"      // = TAB5_USB_RAIZ

typedef enum {
  TAB5_VOL_NENHUM = -1,     // the path is not on any disk
  TAB5_VOL_CARTAO = 0,
  TAB5_VOL_PENDRIVE,
  TAB5_VOL_N,
} tab5_vol_id_t;

typedef struct {
  tab5_vol_id_t id;
  const char*   raiz;        // "/sdcard", "/usb"
  const char*   tipo;        // "Cartão SD", "Pendrive" (for display)
  char          nome[40];    // the disk's name on the computer (label; UTF-8), or ""
  bool          disponivel;  // usable NOW (mounting should work)
} tab5_vol_t;

// The disks that exist now (the card always appears - "no card" is
// discovered on mounting; the flash drive only when plugged in and read). Returns how many.
int tab5_volumes_listar(tab5_vol_t* out, int max);

// Which disk this absolute path is on (by its prefix: "/usb", "/usb/...").
tab5_vol_id_t tab5_volume_de(const char* caminho);

// The disk of this path, for display ("Pendrive", "Cartão SD"); never NULL.
const char* tab5_volume_tipo(const char* caminho);

// Holds / releases the disk where 'caminho' lives. false = failed (no card,
// card lent out, flash drive missing or on its way out) - and then it does NOT count a reference.
bool tab5_volume_montar(const char* caminho);
void tab5_volume_desmontar(const char* caminho);

// READ a file from any disk (card or flash drive). Holds the disk where it
// lives and, on the card, first writes out what was pending (the read sees the last
// write). NULL = failed (the disk has already been released). ALWAYS close with
// tab5_volume_fechar (with the same path) - only if it opened.
FILE* tab5_volume_abrir_leitura(const char* caminho);
void  tab5_volume_fechar(FILE* f, const char* caminho);   // closes and releases the disk

// The WHOLE file in PSRAM (up to 'max' bytes; larger = failure). Returns the
// buffer (free with heap_caps_free) and the size in *n; NULL = failed.
uint8_t* tab5_volume_ler_tudo(const char* caminho, size_t max, size_t* n);

// FAST READER - for a LARGE file read bit by bit (image, GIF).
//
// WHY NOT FREAD. fread on the card yields 52-572 KB/s: each call asks FAT for
// a small piece, which reads sector by sector. POSIX read() in 64 KB
// blocks, into a buffer aligned to the cache line (128 B) in PSRAM, reaches 4.6 MB/s
// (see the BMP16 reader the Files app used to have). The reader keeps that
// buffer and hands the caller whatever it asks for, of whatever size.
//
//   tab5_leitor_t* r = tab5_leitor_abrir(caminho);   // holds the disk
//   if (r) {
//     tab5_leitor_ler(r, cab, 16);
//     tab5_leitor_ir(r, 4096);                       // jumps to a position
//     ...
//     tab5_leitor_fechar(r);                         // releases the disk
//   }
//
// Like abrir_leitura: mounts the disk (and on the card first writes out what was
// pending). Blocks: keep it off the UI task. A reader belongs to ONE task.
typedef struct tab5_leitor tab5_leitor_t;
tab5_leitor_t* tab5_leitor_abrir(const char* caminho);   // nullptr = failed (the disk has already been released)
// Reads up to 'n' bytes into 'dst' (nullptr = just discards). Returns how many it read
// (fewer than 'n' only at end of file) or -1 on error.
int            tab5_leitor_ler(tab5_leitor_t* r, void* dst, size_t n);
// Moves to position 'pos' in the file (false = past the end or error). Within
// what is already in the buffer it is free; outside, the disk is read from the sector.
bool           tab5_leitor_ir(tab5_leitor_t* r, uint64_t pos);
uint64_t       tab5_leitor_pos(const tab5_leitor_t* r);       // where the next read starts
uint64_t       tab5_leitor_tamanho(const tab5_leitor_t* r);
void           tab5_leitor_fechar(tab5_leitor_t* r);          // closes and releases the disk

// Size and free space of the disk of 'caminho', in bytes. May be slow (FAT
// counts the free clusters the first time): off the UI task.
bool tab5_volume_espaco(const char* caminho, uint64_t* total, uint64_t* livre);

// Is the disk still usable? (For whoever holds it for a long time.)
bool tab5_volume_disponivel(const char* caminho);

// EJECT - flash drive only (the card does not come out with the device on). Waits
// for whoever holds the disk to let go (up to 'espera_ms'), unmounts and says "safe
// to remove" (see tab5_usb_ejetar). Blocks: keep it off the UI task.
typedef enum {
  TAB5_VOL_EJETADO = 0,
  TAB5_VOL_EJETAR_NAO_DA,    // not an ejectable disk, or it is no longer there
  TAB5_VOL_EJETAR_EM_USO,    // someone (a copy, the music) held it past the deadline
} tab5_vol_ejetar_t;
bool              tab5_volume_ejetavel(const char* caminho);
tab5_vol_ejetar_t tab5_volume_ejetar(const char* caminho, uint32_t espera_ms);

// For the disks' OWNERS (tab5_usb, tab5_sd): a disk arrived or left.
// Bumps the version, publishes AVISO_VOLUMES and wakes the UI.
void tab5_volumes_mudou(void);
