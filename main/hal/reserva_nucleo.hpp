// THE JOURNAL OF DEFERRED WRITES - the core, without ESP-IDF.
//
// Its user is tab5_reserva (the same core over the "reserva" flash
// partition); this file is pure so it runs on the Mac (testes/rodar_reserva.sh),
// with a fake flash that behaves like the real one.
//
// -------------------------------------------------------------------------
// WHAT IT IS FOR
// -------------------------------------------------------------------------
// While the SD card is lent to the computer (USB flash drive mode), whatever
// the system writes to it becomes a RECORD here, in the order it happened.
// When the card comes back, the records are applied to the card in the same
// order and the journal is erased. Flash keeps its contents without power: a
// reboot in the middle of USB drive mode loses nothing.
//
// -------------------------------------------------------------------------
// THE FLASH RULES THIS RESPECTS
// -------------------------------------------------------------------------
// - Writing only turns 1 into 0. Erased area is 0xFF; writing again over
//   something already written does not work - only ERASING (a whole 4 KB
//   sector) brings everything back to 0xFF. So the journal only grows, and is
//   erased as a whole at the end.
// - The useful exception: each record's "applied" mark starts as 0xFF and
//   becomes 0x00 once the record has gone to the card - turning 1 into 0 is
//   allowed. That is what lets applying resume where it stopped after a
//   reboot, without repeating a line on the card.
//
// -------------------------------------------------------------------------
// POWER LOSS IN THE MIDDLE OF A WRITE
// -------------------------------------------------------------------------
// Each record carries a CRC of itself. A half-written record fails the CRC
// and is ignored; all earlier ones remain valid. The bytes it did manage to
// write cannot be rewritten, so the next record starts at the next sector
// that is still erased ("sujo" (dirty) in the state). The scan makes the same
// jump and finds the records that came after.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RESERVA_SETOR  4096u

typedef enum {
  RESERVA_ACRESCENTAR = 1,   // line(s) at the end of the file (with header if it is empty)
  RESERVA_SUBSTITUIR  = 2,   // the whole file becomes 'dados'
} reserva_tipo_t;

// The storage medium: the flash partition, or the test's fake one.
typedef struct {
  bool  (*ler)(void* ctx, uint32_t pos, void* buf, uint32_t n);
  bool  (*gravar)(void* ctx, uint32_t pos, const void* buf, uint32_t n);
  bool  (*apagar)(void* ctx, uint32_t pos, uint32_t n);    // multiples of RESERVA_SETOR
  void* (*alocar)(size_t n);                               // to read a whole record
  void  (*liberar)(void* p);
  void*    ctx;
  uint32_t tamanho;                                        // multiple of RESERVA_SETOR
} reserva_meio_t;

// A record, as the writer hands it over and as the applier receives it. The
// strings are NOT NUL-terminated inside the journal; when applied they are.
typedef struct {
  uint8_t     tipo;          // reserva_tipo_t
  uint8_t     flags;         // the owner's (tab5_sd: TAB5_SD_SIGILOSO)
  const char* app;           // the app's folder ("Mesh", "Sistema/logs")
  const char* arquivo;
  const void* cabecalho;     // ACRESCENTAR: written first if the file is empty
  uint16_t    n_cabecalho;
  const void* dados;
  uint32_t    n_dados;
  // SUBSTITUIR: how the file was when the card was lent out. On return, if it
  // is no longer like that, the computer changed it.
  bool        base_existia;
  uint32_t    base_tamanho;
  uint32_t    base_data;     // FAT date/time (st_mtime)
} reserva_reg_t;

typedef struct {
  reserva_meio_t meio;
  uint32_t fim;         // where the next record starts (always in erased area)
  uint32_t validos;     // complete records in the journal
  uint32_t pendentes;   // of the valid ones, those not yet on the card
  bool     sujo;        // there was a half-written record (power loss)
  bool     pronto;
} reserva_t;

// Scans the medium and readies the state. false = medium unreadable.
bool reserva_abrir(reserva_t* r, const reserva_meio_t* meio);

// Appends a record. false = journal full or write error (the record did not
// go in; the earlier ones are intact).
bool reserva_gravar(reserva_t* r, const reserva_reg_t* reg);

// Applies the pending ones IN ORDER. 'fn' writes a record to the destination
// and returns true once it is durably there; only then is the record marked
// as applied. Stops at the first false (order matters: none is skipped).
// With nothing pending at the end, the journal is erased. Returns how many
// were applied in this call, or -1 if the medium failed.
typedef bool (*reserva_aplicar_fn)(const reserva_reg_t* reg, void* ctx);
int reserva_aplicar(reserva_t* r, reserva_aplicar_fn fn, void* ctx);

// Bytes used (records and garbage) and the medium's total.
static inline uint32_t reserva_usado(const reserva_t* r) { return r->fim; }
