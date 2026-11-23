// TRILHA (track) - the GPS track engine: writes the position to the SD card
// every 5 s (with the window open or closed), keeps the path for drawing, lists
// the days already recorded and reads a CSV back. The Trilha app only draws
// and plays.
//
// It started as the "real multitasking" pilot and is a client of the system
// pattern (kernel/fundo.h): this service describes the work (TRILHA_FUNDO) and
// keeps its state - whoever creates the task, shows the badge, adds up the
// power profile and stops it cleanly is the KERNEL. On and off:
// sistema_fundo_iniciar/parar(&TRILHA_FUNDO).
//
// Does not take ownership of the Grove port nor power the GPS - it only
// CONSUMES tab5_gps_posicao()/tab5_gps_tempo(). Without a GPS or a fix, points
// simply do not come out. The SD card is only touched through the ports of
// hal/tab5_sd.hpp (writing) and the reader in services/tab5_volumes.hpp (reading).
//
// CSV FORMAT (one file per day: Trilha/trilha_NNNN.csv):
//   data_hora_utc,lat,lon,altitude_m,qualidade
//   AAAA-MM-DDTHH:MM:SSZ,-23.550000,-46.630000,760.0,1
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "fundo.h"

// The description the kernel runs (see kernel/fundo.h).
extern const fundo_def_t TRILHA_FUNDO;

// Points of the track being recorded (in a CONTINUED track, includes those
// already in the file).
uint32_t tab5_trilha_pontos(void);

// ONE TRACK PER DAY (LOCAL day, TAB5_FUSO_S). Before sistema_fundo_iniciar:
//   tab5_trilha_continuar(N) - the next start continues trilha_N.csv (appends
//                              to the same file, and the path is read back from the card);
//   tab5_trilha_continuar(0) - the next start opens a new track (the default).
// Applies to ONE start. While recording, midnight closes the day's track (it
// stays in the history) and opens the next one by itself - recording does not stop.
void     tab5_trilha_continuar(uint32_t numero);
// The number of the track recording now (0 = not recording).
uint32_t tab5_trilha_numero(void);

// CSV TIMESTAMP: "AAAA-MM-DDTHH:MM:SSZ" (UTC) at the start of the line -> UTC epoch.
// false if the line does not start with a timestamp (header, truncated line).
bool     tab5_trilha_ler_carimbo(const char* linha, uint32_t* epoch_utc);
// The LOCAL day of a UTC instant, as AAAAMMDD (0 = clock not set).
uint32_t tab5_trilha_dia_local(uint32_t epoch_utc);

// SOUND NOTICE. Saves the preference and updates the "Trilha" source of the
// notification (only that one - Mesh keeps sounding as normal).
void tab5_trilha_definir_beep(bool tocar);
bool tab5_trilha_beep_ligado(void);

// -------------------------------------------------------------------------
// The "MAP" - only the SHAPE of the path, for the app to draw: connected points
// on an empty background, with no map underneath.
// -------------------------------------------------------------------------
// RAM only, never the card - the CSV is already the real record; this is only
// for DRAWING, it can lose everything on a reboot with no problem at all.
//
// TAB5_TRILHA_MAPA_MAX fits WITHOUT decimating 5 s * 240 = 20 min of walking at
// full resolution. After that, decimation (below): when it fills, it keeps
// HALF the points and doubles the step - the WHOLE walk still fits,
// just with less detail in the middle, instead of "forgetting" the beginning.
#define TAB5_TRILHA_MAPA_MAX 240

typedef struct { float lat, lon; } tab5_trilha_ponto_t;

// Copies up to 'max' points of THIS session's path (oldest to newest) into
// 'out', returns how many it copied. The points are APPROXIMATE after the
// first decimation - they are for DRAWING, never for navigation or for
// recalculating distance precisely.
uint32_t tab5_trilha_mapa(tab5_trilha_ponto_t* out, uint32_t max);

// SINGLE-PASS DECIMATION - ONE function for anyone who needs to fit a long
// series into a short array: keeps 1 in every 'passo' samples; when the array fills,
// it discards the odd positions (keeps half) and doubles the step. The result
// covers the WHOLE series, from the first to the last point.
typedef struct {
  tab5_trilha_ponto_t* pts;   // the array (the caller's)
  uint32_t max;               // capacity of pts
  uint32_t n;                 // points kept
  uint32_t passo;             // 1 in every 'passo' samples goes in
  uint32_t idx;               // samples seen
} tab5_trilha_decimador_t;
void tab5_trilha_decimar_iniciar(tab5_trilha_decimador_t* d, tab5_trilha_ponto_t* pts, uint32_t max);
void tab5_trilha_decimar_somar(tab5_trilha_decimador_t* d, float lat, float lon);

// -------------------------------------------------------------------------
// The HISTORY - reading what is already on the card. Blocks (card): call from
// the worker (sistema_trabalho), never from touch. 'cancelado' (may be nullptr) is
// polled now and then; true = the result is no longer needed, stop.
// -------------------------------------------------------------------------
typedef bool (*tab5_trilha_cancelado_t)(void);

// "06/10 08:15" fits in 14 bytes.
#define TAB5_TRILHA_QUANDO_TAM 14

// The numbers of the tracks on the card, from the HIGHEST (most recent) to the
// lowest, without 'exceto_a' and 'exceto_b' (the one recording and today's: not
// history yet). 'quando' (may be nullptr; size TAB5_TRILHA_QUANDO_TAM each) gets
// the start of each track in local time, only for the 'com_data' most recent
// ("" = not read: opening one file per track would be seconds of card time).
// Returns how many.
int tab5_trilha_listar(uint32_t* numeros, char (*quando)[TAB5_TRILHA_QUANDO_TAM], int max,
                       int com_data, uint32_t exceto_a, uint32_t exceto_b,
                       tab5_trilha_cancelado_t cancelado);

// A track read into 'pts' (decimated to 'max'). 'primeiro'/'ultimo': UTC epoch
// of the first and last point (0 = none). 'total' = valid CSV lines.
typedef struct {
  tab5_trilha_ponto_t* pts;
  uint32_t max;
  uint32_t n, total;
  uint32_t primeiro, ultimo;
} tab5_trilha_leitura_t;
void tab5_trilha_ler(uint32_t numero, tab5_trilha_leitura_t* r, tab5_trilha_cancelado_t cancelado);

// TODAY's track that can be continued: the newest on the card, if its last
// point is from today (local day). Returns the number (0 = none) and, in that
// case, leaves it read into 'r'.
uint32_t tab5_trilha_de_hoje(tab5_trilha_leitura_t* r, tab5_trilha_cancelado_t cancelado);
