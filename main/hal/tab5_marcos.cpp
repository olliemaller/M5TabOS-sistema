#include "tab5_marcos.hpp"

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/rtc.h"
#include "esp_cache.h"
#include "sdmmc_cmd.h"
#include <atomic>

extern "C" void tab5_heap_psram_checar(const char* onde, size_t tam);

#define MARCOS_N 64

static DRAM_ATTR tab5_marco_t s_anel[MARCOS_N];
static DRAM_ATTR uint32_t     s_prox = 0;
static DRAM_ATTR portMUX_TYPE s_mux  = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR tab5_marco(const char* o_que, int32_t valor)
{
  const int64_t agora = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&s_mux);
  tab5_marco_t& m = s_anel[s_prox % MARCOS_N];
  m.us    = agora;
  m.o_que = o_que;
  m.valor = valor;
  ++s_prox;
  portEXIT_CRITICAL_SAFE(&s_mux);
}

int tab5_marcos_entre(int64_t de_us, int64_t ate_us, tab5_marco_t* out, int max)
{
  tab5_marco_t copia[MARCOS_N];
  uint32_t prox;
  portENTER_CRITICAL(&s_mux);
  prox = s_prox;
  for (int i = 0; i < MARCOS_N; ++i) { copia[i] = s_anel[i]; }
  portEXIT_CRITICAL(&s_mux);

  // When there are more markers in the window than fit in 'out', the NEWEST
  // are kept - those that landed closest to the delay. The first version kept
  // the oldest and cut precisely the last milliseconds before it.
  const uint32_t total = prox < MARCOS_N ? prox : MARCOS_N;
  int na_janela = 0;
  for (uint32_t k = prox - total; k != prox; ++k) {
    const tab5_marco_t& m = copia[k % MARCOS_N];
    if (m.o_que && m.us >= de_us && m.us <= ate_us) { ++na_janela; }
  }
  int pular = na_janela > max ? na_janela - max : 0;
  int n = 0;
  for (uint32_t k = prox - total; k != prox && n < max; ++k) {
    const tab5_marco_t& m = copia[k % MARCOS_N];
    if (!(m.o_que && m.us >= de_us && m.us <= ate_us)) { continue; }
    if (pular > 0) { --pular; continue; }
    out[n++] = m;
  }
  return n;
}

// THE CLOCK-CHANGE WRAP (see the .hpp). Runs inside esp_pm's do_switch(), in
// a critical section and possibly with the cache off: IRAM, data in DRAM,
// nothing beyond noting it down and moving on.
extern "C" void __real_rtc_clk_cpu_freq_set_config_fast(const rtc_cpu_freq_config_t* config);

extern "C" void IRAM_ATTR __wrap_rtc_clk_cpu_freq_set_config_fast(const rtc_cpu_freq_config_t* config)
{
  tab5_marco("CPU clock ->", config ? (int32_t)config->freq_mhz : -1);
  __real_rtc_clk_cpu_freq_set_config_fast(config);
}

// THE SD CARD: sector reads and writes, which are SDMMC DMA. The 9.7 s delay
// showed up with no clock change at all, with Mesh loading nodes and
// conversations from the card - the next suspect is that traffic.
extern "C" esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t bloco, size_t n);
extern "C" esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t bloco, size_t n);

extern "C" esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t bloco, size_t n)
{
  tab5_marco("sd: read sectors", (int32_t)n);
  return __real_sdmmc_read_sectors(card, dst, bloco, n);
}

extern "C" esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t bloco, size_t n)
{
  tab5_marco("sd: write sectors", (int32_t)n);
  return __real_sdmmc_write_sectors(card, src, bloco, n);
}

// LARGE CACHE SYNC: writing back or invalidating tens of KB of PSRAM occupies
// the bus the panel DMA uses. Only the large ones (>= 16 KB) - the small ones
// are constant and would just fill the ring.
extern "C" esp_err_t __real_esp_cache_msync(void* addr, size_t size, int flags);

extern "C" esp_err_t IRAM_ATTR __wrap_esp_cache_msync(void* addr, size_t size, int flags)
{
  if (size >= 16384) { tab5_marco("cache: msync KB", (int32_t)(size / 1024)); }
  const esp_err_t r = __real_esp_cache_msync(addr, size, flags);
  // The PSRAM heap header intact after a large sync (esp_lcd does a 1.8 MB
  // one per frame - see tab5_heap_psram.cpp).
  if (size >= (1u << 20)) { tab5_heap_psram_checar("after msync", size); }
  return r;
}
