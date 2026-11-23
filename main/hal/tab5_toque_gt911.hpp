// GT911 touch (Tab5's ILI9881C) without a "phantom finger" when I2C fails.
//
// M5GFX DRIVER DEFECT (Touch_GT911.inl, getTouchRaw): if the I2C read
// fails, the internal buffer (_readdata) keeps the LAST good read and
// getTouchRaw returns those points as if they were new. If the last good
// state had a finger, a single I2C failure becomes a finger stuck on the screen (touch
// held). The members are private, so it can't be fixed by simple
// inheritance: this class redoes only getTouchRaw, with its own buffer, and returns
// 0 points when the read fails. The rest (init/sleep/wakeup) is the original's.
//
// HOW IT GOES IN: tab5_toque_gt911_instalar(), called once after M5.begin().
#pragma once

#include <M5Unified.h>
#include <stdint.h>

// Swaps the panel's touch for Toque_GT911. Only acts on the ILI9881C (GT911); on
// other panels it does nothing. Returns true if it swapped. Call once, after
// M5.begin().
bool tab5_toque_gt911_instalar(void);

// How many I2C reads have failed since boot (for diagnostics).
uint32_t tab5_toque_gt911_falhas(void);
