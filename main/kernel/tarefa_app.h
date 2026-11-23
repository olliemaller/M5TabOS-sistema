// Frame for an application's tasks, with ORDERLY SHUTDOWN.
//
// THE PROBLEM it solves: killing a task from outside with vTaskDelete() stops
// it wherever it is - including INSIDE a driver, holding an internal
// semaphore. And FreeRTOS has no way to revoke ownership: a dead task's mutex stays
// taken forever, with the owner pointer pointing at garbage. There is no
// "force it back".
//
// The ONLY correct way is not to kill: ask it to exit and let the task itself
// give back what it took. What makes this fast is xTaskAbortDelay(),
// which takes the task out of the blocked state AT ONCE - it returns from
// uart_read_bytes, xQueueReceive, vTaskDelay, as if the timeout had
// expired. Without it, you would have to wait for each wait's natural timeout.
//
// HOW TO USE:
//
//   static tarefa_app_t s_t;
//
//   static void meu_laco(tarefa_app_t* t) {
//       while (tarefa_app_ativa(t)) {
//           ...work...                          // with BOUNDED waits
//       }
//       // give back what you took here: fclose, xSemaphoreGive, etc.
//   }
//
//   ao_abrir():   tarefa_app_criar(&s_t, meu_laco, "gps", 4096, 3);
//   ao_fechar():  if (tarefa_app_encerrar(&s_t, 200)) { /* only then destroy queues */ }
//
// When the loop RETURNS, the frame notifies whoever is waiting and the slot in the
// task pool (kernel/pool_tarefas.h) goes back to sleep - the app NEVER calls
// vTaskDelete (that would kill the whole slot). And encerrar() only returns after the
// task has confirmed it exited.

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "pool_tarefas.h"

typedef struct tarefa_app_s tarefa_app_t;

typedef void (*tarefa_app_corpo_t)(tarefa_app_t* t);

struct tarefa_app_s {
  TaskHandle_t       handle;   // nullptr = there is no task (the slot may belong to another run later)
  pool_id_t          id;       // the run in the fixed task pool (kernel/pool_tarefas.h)
  SemaphoreHandle_t  fim;      // the task gives it when it has finished cleaning up
  volatile bool      parar;
  tarefa_app_corpo_t corpo;
};

// Creates the task. Returns false if memory ran out (and nothing is left half-done).
bool tarefa_app_criar(tarefa_app_t* t, tarefa_app_corpo_t corpo,
                      const char* nome, uint32_t pilha, UBaseType_t prioridade);

// The task's loop asks this on every pass.
static inline bool tarefa_app_ativa(const tarefa_app_t* t) { return !t->parar; }

// Requests exit, unblocks the task and WAITS for it to confirm, up to ms.
// Returns true if the task confirmed exit (or if there was no task): then it is
// safe to destroy queues, drivers and buffers. Returns false if the deadline passed:
// the task was ABANDONED and may be using what the caller created - whoever
// receives false must NOT destroy any of it (leaking is bad; corrupting is worse).
bool tarefa_app_encerrar(tarefa_app_t* t, uint32_t ms);
