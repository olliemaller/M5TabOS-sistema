// COOPERATIVE STOP OF A SERVICE TASK.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// Three services (GPS, Mesh, Mesh Bluetooth link) stop their own task
// the same way - and each wrote the mechanism again: one flag
// "asked to stop", another "the task exited", and whoever asks WAITS
// BY POLLING every 10 ms (for ... vTaskDelay(10)). Three copies with
// differences nobody chose: in GPS the flags were not even volatile
// (read from one task, written from another), and the end of each wait lagged
// up to 10 ms after the task had already exited.
//
// Here the task GIVES a semaphore on exiting and whoever asked WAITS ON IT: wakes at the
// instant of exit, without polling anything in between. The safety rule of the
// three stays the same, now written only once (parada_pedir).
//
// -------------------------------------------------------------------------
// WHY COOPERATIVE (and not vTaskDelete, nor xTaskAbortDelay)
// -------------------------------------------------------------------------
// The task takes a mutex and writes to the card; killing it midway leaves the mutex
// held forever (FreeRTOS does not revoke ownership). And xTaskAbortDelay, which
// the app framework uses (tarefa_app.h), is NOT suitable here either: it takes the
// task out of ANY wait - including an xSemaphoreTake(mutex,
// portMAX_DELAY) whose return these services do not check. The task would carry on
// believing it holds the mutex, and the following Give, from a non-owner, brings down the
// device. So the task exits ON ITS OWN, at its exit point, where it does not
// hold anything; the requester's wait is at most one turn (typically the
// UART read timeout).
//
// -------------------------------------------------------------------------
// USAGE
// -------------------------------------------------------------------------
//   static parada_t s_parada;
//
//   iniciar():  if (!parada_preparar(&s_parada)) { ...out of memory... }
//               if (!pool_criar(...)) { parada_desistir(&s_parada); ... }
//   tarefa:     while (!parada_pedida(&s_parada)) { ...one turn... }
//               ...release what only the task holds...
//               parada_confirmar(&s_parada);   // the LAST thing
//   parar():    if (!parada_pedir(&s_parada, 1000)) { return false; }
//               ...only now tear down UART, queues, mutex...

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
  SemaphoreHandle_t saiu;     // given by the task on exiting; whoever asked waits on it
  SemaphoreHandle_t acordar;  // takes the task out of parada_dormir (request or nudge)
  volatile bool     pedido;   // asked to stop - the task reads it on every turn
  volatile bool     viva;     // there is a run that has not yet been collected
} parada_t;

// Before creating the task. The semaphores are created once and serve forever.
// false = out of memory (do not create the task).
static inline bool parada_preparar(parada_t* p)
{
  if (!p->saiu)    { p->saiu = xSemaphoreCreateBinary(); }
  if (!p->acordar) { p->acordar = xSemaphoreCreateBinary(); }
  if (!p->saiu || !p->acordar) { return false; }
  xSemaphoreTake(p->saiu, 0);      // leftover from an earlier run: does not count for this one
  xSemaphoreTake(p->acordar, 0);
  p->pedido = false;
  p->viva   = true;
  return true;
}

// The task was never created: undoes the preparation.
static inline void parada_desistir(parada_t* p) { p->viva = false; p->pedido = false; }

// In the task: true when it is time to exit.
static inline bool parada_pedida(const parada_t* p) { return p->pedido; }

// In the task: the LAST thing before returning. From here on whoever asked
// may tear down everything it used.
static inline void parada_confirmar(parada_t* p) { xSemaphoreGive(p->saiu); }

// THE TASK'S NAP BETWEEN TWO TURNS. A task that works "every N
// seconds" and needs to stop quickly used to sleep in 100 ms slices just to check
// the request - 10 wake-ups per second, for nothing, with the battery paying. Here it
// sleeps the WHOLE period on a semaphore that parada_pedir (or a
// parada_cutucar) gives: wakes at the right time or the instant of the request, and never
// otherwise. Returns true if it was woken before the deadline.
static inline bool parada_dormir(parada_t* p, uint32_t ms)
{
  return xSemaphoreTake(p->acordar, pdMS_TO_TICKS(ms)) == pdTRUE;
}

// Wakes the task from its nap WITHOUT asking it to stop (e.g.: the configuration
// changed and the next turn should come now, not at the end of the period).
static inline void parada_cutucar(parada_t* p) { if (p->acordar) { xSemaphoreGive(p->acordar); } }

// The task exists and has not yet confirmed exit.
static inline bool parada_rodando(const parada_t* p)
{
  return p->viva && p->saiu && uxSemaphoreGetCount(p->saiu) == 0;
}

// Request made and not yet collected (the services' "still stopping").
static inline bool parada_em_curso(const parada_t* p) { return p->viva && p->pedido; }

// REQUESTS AND WAITS up to 'ms'. 'cutucar' (optional) is called immediately and every 100 ms
// while waiting - for the task that sleeps on a wait of ITS OWN (the Bluetooth
// link waits for a notification and a reply from the BLE stack) and needs to be
// woken to see the request.
//
// true  = the task exited: it is safe to tear down what it used. Also true if
//         there was no task.
// false = it did not confirm within the deadline. THE REQUEST STAYS STANDING and NOTHING should be
//         torn down: the task exits when it can, and the next call (which
//         finds the semaphore already given) finishes the job. Lowering the request here
//         would open the race in which the task has already read "stop", exits, and nobody
//         collects it any more - orphaned UART and mutex with the service saying "up".
static inline bool parada_pedir(parada_t* p, uint32_t ms, void (*cutucar)(void) = nullptr)
{
  if (!p->viva) { return true; }
  p->pedido = true;
  parada_cutucar(p);   // whoever is in parada_dormir wakes now and sees the request
  const uint32_t fatia = cutucar ? 100 : ms;
  for (uint32_t esperou = 0; ; esperou += fatia) {
    if (cutucar) { cutucar(); }
    if (xSemaphoreTake(p->saiu, pdMS_TO_TICKS(fatia)) == pdTRUE) {
      p->viva   = false;
      p->pedido = false;
      return true;
    }
    if (esperou + fatia >= ms) { return false; }
  }
}
