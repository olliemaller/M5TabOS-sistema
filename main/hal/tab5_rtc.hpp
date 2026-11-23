// RTC (RX8130) - own crystal and battery, keeps the time between boots.
// M5.begin() already calls M5.Rtc.setSystemTimeFromRtc() itself (see M5Unified.cpp)
// and reads the RTC only once; after that the system clock runs off the
// internal clock (in light sleep, an RC of ~5%). The RTC, with a crystal, is the
// precise reference: services/tab5_hora.cpp re-reads it to correct that drift.
//
// Unless someone SETS the RX8130 at least once, it holds a meaningless
// (factory) time - the battery preserves what was written, it doesn't make up
// the right time by itself. See services/tab5_hora.hpp for WHO decides when to
// set it and from which source.
//
// EVERYTHING HERE IS UI TASK ONLY - the RX8130 is internal I2C (same
// bus as touch/IMU/PMIC), same rule documented in tab5_power.hpp.
#pragma once

#include <stdbool.h>
#include <time.h>

// Date range accepted as real time: 2025-01-01 to 2040-01-01 (UTC).
bool tab5_rtc_plausivel(time_t e);

// Converts a UTC date/time (struct tm) to epoch, without relying on timegm().
time_t tab5_rtc_utc_para_epoch(const struct tm* t);

// Reads the RTC. Only returns true if the time is trustworthy: RTC on, read ok,
// VLF flag clear and plausible epoch. 'epoch_utc' in seconds since 1970.
bool tab5_rtc_ler(time_t* epoch_utc);

// RX8130 flags byte (register 0x1D) for diagnostics, or -1 if the RTC
// is off or the read failed. Bit 0x02 = VLF (time invalid), bit 0x80
// = VBLF (backup battery low, warning only).
int tab5_rtc_flags(void);

// Writes the RTC AND the system clock (settimeofday) IN THE SAME CALL - without
// the second step, only the NEXT time the device powered on would the session
// start answering correctly in time()/gmtime(); with both together, whoever
// calls time() RIGHT NOW sees the correct time.
//
// Rejects an epoch outside tab5_rtc_plausivel() (false, nothing written). After
// writing, reads back and checks (+-2 s): if it doesn't match returns false, but the
// system clock has already been set. Once verified, clears the chip's VLF flag.
// 'epoch_utc' in seconds since 1970, UTC.
bool tab5_rtc_definir(time_t epoch_utc);
