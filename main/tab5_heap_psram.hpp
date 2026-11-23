// THE PSRAM HEAP AT BOOT - guard, sentinel and check (tab5_heap_psram.cpp).
//
// Open problem (2026-10-21): rarely (~1 in 65 boots on 0.8.0) the
// PSRAM heap header turns up corrupted and M5.begin() crashes while allocating
// the screen frame (Load/Store access fault in tlsf_*). What was read in place
// of the heap_data pointer was the heap's own free_bytes counter.
// It happens INSIDE M5.begin, between the first and second 1.8 MB frame
// of esp_lcd (num_fbs = 2): that is why every large PSRAM allocation and every
// large cache sync also checks (tab5_heap_psram_checar, via the
// --wrap in main/CMakeLists.txt). With that check the defect vanished (0 in
// 44 boots on a build that crashed 30% of the time) - one more sign that it depends on
// layout/timing, and that the cause is still open.
#pragma once

#include <stdint.h>

// Checks the PSRAM heap header against the snapshot taken right after
// mounting. Corrupted: writes to the console and restarts cleanly (esp_restart, which
// stops the DMAs) instead of letting M5.begin() panic. At most 3 times
// in a row - after that it carries on, and the panic (with core dump) is the answer again.
// Call BEFORE M5.begin().
void tab5_heap_psram_conferir(void);

// How many consecutive clean restarts the check did before this boot (0 = none).
// tab5_log writes to the card log, which only exists after app_main.
uint32_t tab5_heap_psram_reinicios(void);

// Test of the mechanism, from the console ('mem estresse [voltas] [linha]'): writes
// a cache line in PSRAM, zeroes 1.8 MB and checks the line. 'zerar' false =
// only the line, without sweeping the cache (the test's control).
void tab5_heap_psram_estresse(int voltas, bool zerar);
