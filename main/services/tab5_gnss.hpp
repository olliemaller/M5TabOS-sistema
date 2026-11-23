// GNSS - the Azimutal satellite log: writing the constellation to the SD card
// and loading back the day you want to see. The Azimutal app only draws.
//
// FILE FORMAT (one per day, Azimutal/satelites-AAAA-MM-DD.csv, GPS UTC
// day) - old files still open:
//   SAT,<hh:mm:ss>,<constelacao>,<prn>,<elev>,<azim>,<snr>
// where I was
// Old files may not have the <snr>.
//
// The SD card is only touched through the gateways in hal/tab5_sd.hpp (writing) and the reader
// in services/tab5_volumes.hpp (reading).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nmea.h"

// A satellite in the day's catalogue: whoever appears in the file.
#define TAB5_GNSS_SATS_MAX 40
typedef struct { char cons[12]; int prn; } tab5_gnss_sat_t;

// A logged position. Six bytes per point, in PSRAM.
typedef struct {
  uint8_t  sat;      // index in the catalogue (tab5_gnss_sats)
  uint8_t  elev;     // 0..90
  uint16_t azim;     // 0..359
  uint16_t minuto;   // hh*60+mm, for the start and end labels
  uint8_t  snr;      // dB (0 = no lock)
} tab5_gnss_ponto_t;

// WRITE the constellation to the day's file (through the system writer: hands over
// the line and moves on, with the app open or not). Only writes what has a real
// position, with GPS time and fix. 'idade_max_ms': a satellite whose position hasn't
// been re-observed for longer than this isn't an observation and is left out. Returns
// how many SAT lines it handed over.
uint32_t tab5_gnss_gravar(const SatelliteSnapshot_t* snap, uint32_t idade_max_ms);

// THE DAYS on the SD card (names "satelites-AAAA-MM-DD.csv", newest to
// oldest). Doesn't mount the SD card: lists if it is already mounted. Returns how many.
#define TAB5_GNSS_DIA_NOME_TAM 32
int tab5_gnss_listar_dias(char (*dias)[TAB5_GNSS_DIA_NOME_TAM], int max);

// LOAD A DAY into memory (PSRAM), once: changing the satellite filter
// afterwards is just a loop in RAM, without touching the SD card. If it is already the loaded
// day it does nothing. false = out of memory or no file.
// May take a while (MB-sized file): yields the CPU now and then.
bool tab5_gnss_carregar_dia(const char* arquivo);
// The loaded day. The pointers are valid until unloading/loading another day.
const tab5_gnss_ponto_t* tab5_gnss_pontos(int* n);
const tab5_gnss_sat_t*   tab5_gnss_sats(int* n);
// Frees the day's memory (a closed app has to cost zero).
void tab5_gnss_descarregar(void);
