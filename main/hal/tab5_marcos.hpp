// SYSTEM MARKERS - "what happened, and when", to cross-check against the late
// frame.
//
// The late frame has a fixed signature: one whole frame lost (~30 ms with a
// 15 ms period), with BOTH cores idle - i.e. it is not software delaying the
// ISR, it is the panel DMA running out of data. What is missing is knowing
// what the HARDWARE was doing at that instant. Each suspect leaves a marker
// here, and the monitor, on seeing a delay, lists the markers that landed
// just before it.
//
// Suspect number 1 registers itself: EVERY CPU clock change made by the power
// manager (PM locks from UART, I2C, PPA; esp_pm_configure) goes through
// rtc_clk_cpu_freq_set_config_fast() - and BEFORE it IDF readjusts the
// PSRAM/flash bus mode (esp_clk_utils_mspi_speed_mode_sync_*). The linker
// diverts that call here (-Wl,--wrap, main/CMakeLists.txt).
//
// Ring in INTERNAL RAM (the wrap runs with the cache in any state, inside a
// critical section): 32 markers, text always a LITERAL (only the pointer is
// stored).
#pragma once

#include <stdint.h>

// Leaves a marker. 'o_que' must be a literal. Callable from a task or an ISR.
void tab5_marco(const char* o_que, int32_t valor);

typedef struct {
  int64_t     us;      // esp_timer_get_time()
  const char* o_que;
  int32_t     valor;
} tab5_marco_t;

// The markers within [de_us, ate_us], oldest to newest. Returns how many.
int tab5_marcos_entre(int64_t de_us, int64_t ate_us, tab5_marco_t* out, int max);
