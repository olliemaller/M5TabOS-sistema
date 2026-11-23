#include "tarefa_app.h"
#include "tab5_donos.hpp"
#include "app.h"          // sistema_app_atual
#include "internal.hpp"   // sistema_na_interface
#include "esp_log.h"

static const char *TAG = "tarefa";

// Wraps the app's loop: when it returns, it notifies whoever is waiting and only
// then does the task go away. The app never calls vTaskDelete.
//
// 'fim' CAPTURED HERE, BEFORE the body runs - NOT read from 't->fim' afterwards.
// A real bug, hit on hardware with the camera: a 720x1280 photo
// took more than 10s to write to the SD card (well over any
// tarefa_app_encerrar() timeout in this system), the task was "abandoned"
// - and the tarefa_app_encerrar() comment already warned, correctly, that
// abandoning does NOT delete the semaphore (only the CALLER's 't->fim' becomes nullptr,
// so the caller stops touching it). The problem is that this trampoline
// also sees the SAME 't' (and the same pointer passed to
// xTaskCreatePinnedToCore) - so, when it finally finished, it read
// 't->fim' AGAIN and found nullptr, and xSemaphoreGive(nullptr) brought down
// the whole system (assert in xQueueGenericSend, null "pxQueue").
//
// By keeping the handle HERE, on this task's stack, before the body starts,
// the trampoline always gives the REAL semaphore (it still exists
// - only 't->fim' was cleared, not the object), whether 't' is abandoned or not.
// If nobody is waiting for it any more (the abandoned case), the give just marks the
// semaphore as "given" and nobody ever reads it back - a controlled
// leak, exactly the trade-off tarefa_app_encerrar() already accepted
// in writing ("leaking is bad; corrupting is worse"), only now without a crash.
static void trampolim(void* arg)
{
  tarefa_app_t* t = (tarefa_app_t*)arg;
  SemaphoreHandle_t fim = t->fim;
  t->corpo(t);
  xSemaphoreGive(fim);
  // Returns: the pool slot gives the owner back and goes back to sleep. Never vTaskDelete.
}

bool tarefa_app_criar(tarefa_app_t* t, tarefa_app_corpo_t corpo,
                      const char* nome, uint32_t pilha, UBaseType_t prioridade)
{
  t->handle = nullptr;
  t->parar  = false;
  t->corpo  = corpo;

  // Binary, not counting: only one "I'm done" interests us.
  t->fim = xSemaphoreCreateBinary();
  if (!t->fim) { return false; }

  // AUTOMATIC OWNER (hal/tab5_donos.hpp): created by the UI with an app in
  // front = a task of THAT app (the Camera opens "cam", Azimutal "NMEAParse").
  // Whoever creates outside of that (a service, at boot) registers its own owner - and
  // a registration after this one changes the owner, so the service always wins.
  // The owner goes along with the request: the slot registers it BEFORE the function starts.
  const char* app = (sistema_app_atual() && sistema_na_interface()) ? sistema_app_atual() : nullptr;
  if (!pool_criar(&t->id, trampolim, t, nome, pilha, prioridade,
                  app ? DONO_APP : DONO_SISTEMA, app)) {
    vSemaphoreDelete(t->fim);
    t->fim = nullptr;
    return false;
  }
  t->handle = t->id.handle;
  return true;
}

bool tarefa_app_encerrar(tarefa_app_t* t, uint32_t ms)
{
  if (!t->handle) { return true; }   // there was no task: nothing to wait for

  t->parar = true;

  // Takes the task out of the blocked state NOW, without waiting for its timeout. Returns pdFAIL
  // if it was not blocked - and in that case it will see the flag on its
  // next pass anyway, so there is nothing to do.
  pool_acordar(&t->id);

  if (xSemaphoreTake(t->fim, pdMS_TO_TICKS(ms)) == pdTRUE) {
    vSemaphoreDelete(t->fim);
    t->fim = nullptr;
    t->handle = nullptr;
    t->id = (pool_id_t)POOL_ID_VAZIO;
    return true;
  }

  // It did not confirm in time. We do NOT kill the task and do NOT destroy the semaphore:
  // it may be alive and deliver later, and deleting the semaphore now would
  // crash. Leaking a task and a semaphore is bad; corrupting a driver or
  // writing to freed memory is worse. The log exists so this does not go
  // unnoticed.
  ESP_LOGE(TAG, "task did not confirm exit within %lums - deliberately abandoned",
           (unsigned long)ms);
  t->fim = nullptr;
  t->handle = nullptr;
  t->id = (pool_id_t)POOL_ID_VAZIO;
  return false;   // the caller decides what it must NOT free (see tarefa_app.h)
}
