// SYSTEM MONITOR - a service that measures the device ALL THE TIME.
//
// Before, CPU and memory measurement only existed with the Tasks app open: the
// sampling task was born and died with it, and whatever happened while it was
// closed (a spike, memory dropping in the small hours) was never seen.
//
// Now a service task (priority 1, below everything except IDLE)
// samples every second, without drawing anything, and keeps:
//   - the current snapshot: CPU of each task, free stack, memory;
//   - the PEAKS with the time they happened: highest CPU of each task and of the
//     system, lowest free RAM/PSRAM, lowest largest-block, lowest stack;
//   - a 5-minute history (CPU, RAM, PSRAM) for the graph;
//   - EVENTS, fired by small triggers (see tab5_monitor.cpp):
//     memory below a limit, new minimum, sharp drop, fragmentation,
//     stack nearly exhausted, task stuck at high CPU, task new or gone.
//
// Logged to the SD card, in apps/Tarefas/:
//   eventos-AAAA-MM-DD.csv  - one event per line, at the moment it happens;
//   resumo-AAAA-MM-DD.csv   - one line per MINUTE: worst CPU, lowest RAM,
//                             lowest PSRAM, lowest block - "how much memory
//                             was left when" over the day.
// And each event also goes to the log (tag "monitor"), which tab5_log already records.
//
// The Tasks app is just the window: it reads from here, measures nothing.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

// It was 32 - and uxTaskGetSystemState() returns ZERO when there are more tasks
// than slots, silently. With Wi-Fi + Bluetooth + Mesh the system goes past 32
// (esp_hosted alone creates seven), the sample was discarded and the Tasks app
// kept showing the last good snapshot: numbers "out of step with reality".
#define MON_MAX_TAREFAS 64
#define MON_HIST_N      300     // 5 min at 1 sample/s
#define MON_EVENTOS_N   32
#define MON_NOME        16

typedef struct {
  char        nome[MON_NOME];
  UBaseType_t numero;         // xTaskNumber - stable identity
  UBaseType_t prioridade;
  BaseType_t  core;           // tskNO_AFFINITY = any
  float       pct;            // CPU in the last second (of both cores)
  float       pct_pico;       // highest seen since boot (or since reset)
  uint32_t    pct_pico_em;    // uptime seconds of the peak
  uint32_t    pilha_livre;    // bytes - the minimum ever left (high water mark)
  bool        pilha_psram;    // the stack lives in PSRAM (false = internal SRAM)
  uint8_t     tipo;           // dono_tipo_t (hal/tab5_donos.hpp): system, service, app, background
  char        dono[MON_NOME]; // who ("WiFi", "Camera", "Trilha"); empty = system
} mon_tarefa_t;

typedef struct {
  uint32_t livre, total;
  uint32_t maior_bloco;       // largest possible allocation NOW (fragmentation)
  uint32_t min_livre;         // lowest free seen by the monitor
  uint32_t min_livre_em;      // uptime in s
  uint32_t min_bloco;         // lowest largest-block seen
  uint32_t min_bloco_em;
} mon_mem_t;

typedef struct {
  uint32_t     uptime_s;
  float        cpu;           // total usage (100 - IDLE)
  float        cpu_pico;
  uint32_t     cpu_pico_em;
  mon_mem_t    ram;           // internal
  mon_mem_t    psram;
  uint32_t     foto_us;       // duration of the last uxTaskGetSystemState (critical section)
  uint32_t     foto_pior_us;  // the worst since boot - grows with stacks outside L2MEM
  int          n;
  mon_tarefa_t t[MON_MAX_TAREFAS];   // sorted by xTaskNumber
} mon_foto_t;

typedef struct {
  uint32_t uptime_s;
  uint32_t epoch;             // 0 = clock not set yet
  char     texto[72];
} mon_evento_t;

// Starts the task. Call at boot, after the SD card and the log.
void tab5_monitor_iniciar(void);

// Copies the current snapshot. false = monitor has no first sample yet.
bool tab5_monitor_foto(mon_foto_t* out);

// History, oldest to newest. Returns how many.
int tab5_monitor_historico(float* cpu, uint32_t* ram_livre, uint32_t* psram_livre, int max);

// Events, NEWEST to oldest. Returns how many.
int tab5_monitor_eventos(mon_evento_t* out, int max);
uint32_t tab5_monitor_total_eventos(void);   // counter that only goes up

// Restarts the peak tracking (the memory minimums too).
void tab5_monitor_zerar_picos(void);

// The running PROGRAM'S PARTITION: 'total' = its size, 'usado' = how much
// the program occupies in it (reads the image header in flash; may be
// nullptr if only the total matters). Either of the two may be nullptr.
// false = partition not found (or the image did not open, if 'usado' was asked for).
bool tab5_monitor_programa(uint32_t* usado, uint32_t* total);

// INTERNAL RAM STATEMENT - who ate how much.
//
// The monitor sees RAM drop, but does not know WHY: "80 KB free" does not say if it
// was the panel, the C6's SDIO, NimBLE or a new task. Each milestone notes the free
// internal RAM at that point and the difference from the previous milestone - like
// a bank statement. The milestones are at boot (main.cpp, sistema_iniciar) and at
// the start of each radio/service; the console's "mem" command prints the table, and
// each milestone also goes to the log (tag "mem").
//
// Can be called from any task, at any time, including before
// tab5_monitor_iniciar. Allocates nothing. Do not call from an ISR.
#define MEM_MARCOS_N 40

typedef struct {
  uint32_t ms;            // uptime in ms
  uint32_t livre;         // free internal RAM (bytes)
  uint32_t maior_bloco;   // largest free internal block
  uint32_t minimo;        // lowest free internal RAM seen up to here: below
                          // 'livre' = temporary spike WITHIN the step (hidden in the delta)
  int32_t  delta;         // livre - livre of the previous milestone (negative = consumed)
  char     etapa[28];
} mem_marco_t;

void tab5_mem_marco(const char* etapa);

// Copies the milestones, first to last. Returns how many.
int tab5_mem_extrato(mem_marco_t* out, int max);
