#include "trabalho.hpp"

#include "app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <atomic>

static const char *TAG = "trabalho";

typedef struct {
  trabalho_t fn;
  void*      arg;
  uint32_t   geracao;   // which "life" of the app the request belongs to
  bool       solto;     // owns its own data: outlives the app (see .hpp)
} pedido_t;

static QueueHandle_t     s_fila    = nullptr;
static SemaphoreHandle_t s_rodando = nullptr;   // held while a job is running
static std::atomic<uint32_t> s_geracao = 1;         // goes up every time an app closes
static volatile uint32_t s_geracao_rodando = 0; // that of the (non-detached) job running

static void vTaskTrabalho(void*)
{
  pedido_t p;
  for (;;) {
    if (xQueueReceive(s_fila, &p, portMAX_DELAY) != pdTRUE) { continue; }
    if (p.solto) {
      // Always runs, and outside s_rodando: closing the app does not wait for it.
      // The notice is only sent if the SAME app is still open.
      const int64_t t0 = esp_timer_get_time();
      p.fn(p.arg);
      ESP_LOGI(TAG, "background job finished in %lld ms", (esp_timer_get_time() - t0) / 1000);
      if (p.geracao == s_geracao) { sistema_avisar(AVISO_TRABALHO); }
      continue;
    }
    xSemaphoreTake(s_rodando, portMAX_DELAY);
    // Request from an app that has already closed: its buffers may no longer exist.
    const bool vale = (p.geracao == s_geracao);
    s_geracao_rodando = p.geracao;
    if (vale) { p.fn(p.arg); }
    xSemaphoreGive(s_rodando);
    if (vale) { sistema_avisar(AVISO_TRABALHO); }
  }
}

void trabalho_iniciar(void)
{
  if (s_fila) { return; }
  s_fila    = xQueueCreate(8, sizeof(pedido_t));
  s_rodando = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(vTaskTrabalho, "trabalho", 6144, nullptr, 2, nullptr, tskNO_AFFINITY);
}

static bool pedir(trabalho_t fn, void* arg, bool solto)
{
  if (!s_fila || !fn) { return false; }
  const pedido_t p = { fn, arg, s_geracao, solto };
  if (xQueueSend(s_fila, &p, 0) != pdTRUE) {
    ESP_LOGW(TAG, "queue full - request refused");
    return false;
  }
  return true;
}

bool sistema_trabalho(trabalho_t fn, void* arg)       { return pedir(fn, arg, false); }
bool sistema_trabalho_solto(trabalho_t fn, void* arg) { return pedir(fn, arg, true); }

// Closing bumps s_geracao BEFORE waiting on s_rodando: the job that is
// running sees the difference at once and can bail out midway.
bool sistema_trabalho_cancelado(void) { return s_geracao_rodando != s_geracao; }

void trabalho_encerrar_do_app(void)
{
  if (!s_fila) { return; }
  // Invalidate first: whatever is in the queue (or arrives now) is discarded
  // when the worker takes it out (old generation). The queue is NOT cleared:
  // a DETACHED job in there owns memory that only it frees.
  ++s_geracao;   // single writer: the UI task
  // Then wait for what was already running to finish - that is what makes
  // ao_fechar() safe for freeing memory.
  xSemaphoreTake(s_rodando, portMAX_DELAY);
  xSemaphoreGive(s_rodando);
}
