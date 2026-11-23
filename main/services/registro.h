// Energy logger - a SYSTEM SERVICE, not an app.
//
// WHY IT ISN'T AN APP: apps here are created on open and destroyed
// on close. If the logger lived in the Energia app, there would only be data while it
// was on screen - and the question it answers ("the battery ran out;
// when?") needs CONTINUOUS logging, including with the screen
// off and with another app open. So it is a system service, which starts
// at boot and never stops - with no task of its own: it piggybacks on the
// "cartao" task (tab5_sd_carona), which already wakes every second.
//
// HOW IT WRITES WITHOUT DRAINING POWER: a mounted SD card consumes power. So it samples into RAM every
// minute and only FLUSHES to the SD card every 10 - mounting, appending and
// unmounting. The SD card is off more than 99% of the time, and even so
// no sample is lost.
//
// File: /sdcard/apps/Sistema/energia.csv
//   ENE,<t_s>,<V>,<mA>,<mW>,<pct>,<MHz>,<repouso>,<estado>,<app>
//
// -------------------------------------------------------------------------
// SECOND FILE, IN THE SAME WINDOW: TASK AND HEAP HEALTH
// -------------------------------------------------------------------------
// Explicit request: a folder that keeps, over time, how much stack
// each task still has spare and how the heap is doing - to review days
// later and notice EARLY a consumption that only grows, instead of finding out
// when it has already become a Guru Meditation. The same reasoning as energia.csv
// (a curve is worth more than an isolated number), applied to memory.
//
// Written in the SAME flush as energia.csv - same SD card mount,
// same interval, no new task. Only the marginal cost of opening one more
// file while the SD card is already up.
//
// File: /sdcard/apps/Sistema/tarefas.csv
//   TASKS,<t_s>,<n_tarefas>,<heap_int_livre>,<heap_int_minimo>,<psram_livre>,<psram_minimo>
//   TASK,<t_s>,<nome>,<pilha_livre_bytes>,<core>,<prioridade>   (one per task)
//
// pilha_livre_bytes is the HIGH WATER MARK - the smallest headroom that stack has
// ever had since it was born, not the headroom now. It is the number that matters: it only
// falls, never rises, and so it is what warns "this task got close" before
// the actual overflow.

#pragma once

#include <stdint.h>

// Starts the logger (piggybacking on the "cartao" task). Call after M5.begin()
// and tab5_sd_iniciar().
void registro_iniciar(void);

// Name of the foreground app, for the system to tell the logger.
// Pass NULL when on the home screen.
void registro_app_atual(const char* nome);

// How many samples are waiting in RAM, and how many have already gone to the SD card.
// Lets the Energia app show that the logger is alive.
void registro_estado(int* na_ram, uint32_t* gravadas, uint32_t* falhas);

// ---- reading the recorded day (for the Energia app to draw) -------------------------------
// One day in minutes: position 'm' is minute 'm' after 't0'. The one that knows the
// line format (ENE3, above) is the logger; the app only receives the points.
#define REGISTRO_MIN_DIA 1440
typedef struct {
  uint32_t t0;                          // UTC epoch of slot 0 (0 = no clock)
  float    v[REGISTRO_MIN_DIA], ma[REGISTRO_MIN_DIA], temp[REGISTRO_MIN_DIA];
  int8_t   pct[REGISTRO_MIN_DIA];       // the % the system was showing (estimator), -1 = none
  uint8_t  tem[REGISTRO_MIN_DIA];       // 0 = no sample, 1 = awake, 2 = idle
} registro_dia_t;

// Fills 'd' (zeroed here) with the samples for the 24 h starting at 'd->t0',
// read from the energy files for that moment's day and for today.
// Mounts the SD card and unmounts at the end: call OUTSIDE the UI
// (sistema_trabalho). false = t0 = 0 or the SD card didn't mount.
bool registro_ler_dia(registro_dia_t* d, uint32_t t0);
