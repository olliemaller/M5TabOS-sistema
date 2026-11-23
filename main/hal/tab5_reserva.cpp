#include "tab5_reserva.hpp"

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "reserva";

// Type "data", own subtype (0x40..0xFE is for applications) - see partitions.csv.
#define RESERVA_SUBTIPO ((esp_partition_subtype_t)0x99)

static const esp_partition_t* s_part = nullptr;
static reserva_t              s_r = { };
static SemaphoreHandle_t      s_trava = nullptr;

static bool p_ler(void* ctx, uint32_t pos, void* buf, uint32_t n)
{
  return esp_partition_read((const esp_partition_t*)ctx, pos, buf, n) == ESP_OK;
}
static bool p_gravar(void* ctx, uint32_t pos, const void* buf, uint32_t n)
{
  return esp_partition_write((const esp_partition_t*)ctx, pos, buf, n) == ESP_OK;
}
static bool p_apagar(void* ctx, uint32_t pos, uint32_t n)
{
  return esp_partition_erase_range((const esp_partition_t*)ctx, pos, n) == ESP_OK;
}
// The whole log (up to tens of KB - the Mesh NodeDB) goes to PSRAM.
static void* p_alocar(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  p_liberar(void* p) { heap_caps_free(p); }

bool tab5_reserva_iniciar(void)
{
  if (s_r.pronto) { return true; }
  if (!s_trava) { s_trava = xSemaphoreCreateMutex(); }
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, RESERVA_SUBTIPO, "reserva");
  if (!s_part || !s_trava) {
    ESP_LOGE(TAG, "no \"reserva\" partition - flash the new partition table (./gravar.sh)");
    return false;
  }
  const reserva_meio_t m = { p_ler, p_gravar, p_apagar, p_alocar, p_liberar,
                             (void*)s_part, (uint32_t)(s_part->size / RESERVA_SETOR * RESERVA_SETOR) };
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = reserva_abrir(&s_r, &m);
  xSemaphoreGive(s_trava);
  if (!ok) { ESP_LOGE(TAG, "partition unreadable"); return false; }
  if (s_r.pendentes) {
    ESP_LOGW(TAG, "%u write(s) from an earlier USB flash drive session waiting for the SD card%s",
             (unsigned)s_r.pendentes, s_r.sujo ? " (one was left half-written and was discarded)" : "");
  }
  return true;
}

bool tab5_reserva_gravar(const reserva_reg_t* reg)
{
  if (!s_r.pronto) { return false; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = reserva_gravar(&s_r, reg);
  xSemaphoreGive(s_trava);
  return ok;
}

bool tab5_reserva_pendente(void) { return s_r.pronto && s_r.pendentes > 0; }

int tab5_reserva_aplicar(reserva_aplicar_fn fn, void* ctx)
{
  if (!s_r.pronto) { return 0; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const int n = reserva_aplicar(&s_r, fn, ctx);
  xSemaphoreGive(s_trava);
  return n;
}

void tab5_reserva_uso(uint32_t* usado, uint32_t* total, uint32_t* pendentes)
{
  if (usado)     { *usado = s_r.fim; }
  if (total)     { *total = s_r.meio.tamanho; }
  if (pendentes) { *pendentes = s_r.pendentes; }
}
