// POOL OF FIXED TASKS - whoever needs a task for a while (a service that
// turns on and off, part of an app, card mounting, Grove probing) takes
// a slot that already exists, instead of creating and destroying a task.
//
// WHY. xTaskCreate allocates the TCB and stack on the internal heap on every call, and
// vTaskDelete gives them back later - via the idle task, at no set time. Creating and
// destroying 4 to 6 KB blocks in SRAM, all the time, is what fragments the internal
// heap (the "largest block" drops even with free RAM to spare) and what makes
// the apps' memory depend on what the services did before.
//
// HOW. The slots' stacks and TCBs are STATIC (.bss): the total size is
// known at compile time, never fragments the heap and does not come out of what the apps
// see - the free RAM measured at run time already discounts the slots. Each
// slot is a task that sleeps on a semaphore (zero CPU cost) and runs the
// function it is given; when the function RETURNS, the slot goes back to sleep.
//
// RULES FOR USERS
//   - The function must RETURN to finish. Never call vTaskDelete(NULL):
//     that would kill the whole slot.
//   - Do not keep the TaskHandle_t after the function has finished: the slot is
//     reused by another. Use pool_id_t and pool_acordar(), which checks
//     whether the slot is still the same.
//   - With no slot of the requested size (or a stack larger than the largest slot), it falls back to an
//     ordinary task and notes it in the log. Nothing fails: it just loses the advantage.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_donos.hpp"

typedef void (*pool_corpo_t)(void* arg);

// Identity of a run. 'idx' < 0: did not use a slot (ordinary task).
typedef struct {
  int8_t       idx;
  uint32_t     geracao;
  TaskHandle_t handle;   // only valid while the function runs
} pool_id_t;

#define POOL_ID_VAZIO { -1, 0, nullptr }

// Brings up the slots. Call once, early in boot. Idempotent.
void pool_iniciar(void);

// Runs 'fn(arg)' in a slot with a stack of at least 'pilha' bytes, at
// priority 'prio'. 'dono' (optional: DONO_SISTEMA + nullptr = none) is
// registered BEFORE the function starts. 'id' may be nullptr (fire and
// forget). Returns false only if not even the ordinary task fitted.
bool pool_criar(pool_id_t* id, pool_corpo_t fn, void* arg, const char* nome,
                uint32_t pilha, UBaseType_t prio,
                dono_tipo_t tipo = DONO_SISTEMA, const char* dono = nullptr);

// Takes the task out of any timed wait (xTaskAbortDelay), IF it is still the
// same run. Safe to call after it has finished.
void pool_acordar(const pool_id_t* id);

// Diagnostics and budget.
typedef struct {
  int      vagas;        // how many exist
  int      ocupadas;     // how many are running now
  uint32_t bytes_fixos;  // reserved stacks + TCBs
  uint32_t dinamicas;    // times a slot was lacking and it fell back to an ordinary task
} pool_estado_t;
void pool_estado(pool_estado_t* out);

// Lowest free stack (bytes) seen per run NAME, including those that have already
// finished - what the slots repaint at the end of each one. Returns how many.
typedef struct { char nome[16]; uint32_t livre_min; } pool_pilha_t;
int pool_pilhas(pool_pilha_t* out, int max);
void pool_pilhas_zerar(void);

// STACK IN LP SRAM: NO. The P4's 32 KB of LP SRAM look like a good place for
// the stack of a cold task (they sit empty, and the IDF accepts a stack there even during
// flash writes), but LP SRAM is VERY slow for the main CPU - in the
// region of hundreds of ns per byte. The cost shows up where you do not expect it:
// uxTaskGetSystemState (the monitor's snapshot, 1x/s) measures the free stack of EVERY
// task by reading byte by byte, INSIDE taskENTER_CRITICAL (interrupts
// off + kernel lock). With four stacks there, the snapshot went from ~0.8 ms
// to ~9 ms and dropped a panel frame (the blue flash). Any stack
// outside L2MEM lengthens that window - PSRAM included.
