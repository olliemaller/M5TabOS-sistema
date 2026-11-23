#include "tab5_rtc.hpp"
#include <M5Unified.h>
#include <sys/time.h>
#include "esp_log.h"

static const char* TAG = "rtc";

// RX8130: I2C address and flags register (RX8130CE datasheet).
#define RX8130_ENDERECO   0x32
#define RX8130_REG_FLAGS  0x1D
#define RX8130_FLAG_VLF   0x02   // oscillator stopped / power lost: time invalid
#define I2C_FREQ_HZ       400000

// The flags at 0x1D are "write 0 to clear" (writing 1 is ignored). This
// mask has 0 only at VLF: clears it and leaves TF/AF (alarm/timer) alone,
// which a read-modify-write could wipe by accident.
#define RX8130_LIMPA_VLF  0xFD

// Tolerance for verifying the write: the RTC has 1 s resolution and the
// write/read may straddle the second rollover.
#define TOLERANCIA_S      2

// Accepted date range: 2025-01-01 to 2040-01-01 (UTC).
#define EPOCH_PISO        1735689600
#define EPOCH_TETO        2208988800

bool tab5_rtc_plausivel(time_t e)
{
  // The ceiling exists because the RX8130 accepts any year up to 2099 and a bad
  // source (e.g. Mesh) has already written the year 2049. Revisit the ceiling in ~2035.
  return e >= (time_t)EPOCH_PISO && e < (time_t)EPOCH_TETO;
}

// Howard Hinnant's formula (days_from_civil, public domain): UTC days since
// 1970-01-01, correct for any year. Replaces timegm() - this IDF's libc
// does not declare it in <ctime> without an extension macro.
time_t tab5_rtc_utc_para_epoch(const struct tm* t)
{
  int y = t->tm_year + 1900;
  const int m = t->tm_mon + 1;
  const int d = t->tm_mday;
  y -= (m <= 2);
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long dias = era * 146097 + (long)doe - 719468;
  return (time_t)dias * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

int tab5_rtc_flags(void)
{
  if (!M5.Rtc.isEnabled()) { return -1; }
  uint8_t flags = 0;
  // readRegister (bool) instead of readRegister8, which returns 0 on failure and
  // can't tell "no flags" from "read failed".
  if (!M5.In_I2C.readRegister(RX8130_ENDERECO, RX8130_REG_FLAGS, &flags, 1, I2C_FREQ_HZ)) { return -1; }
  return flags;
}

// Reads the chip calendar and converts to UTC epoch, without checking flags or range.
static bool ler_calendario(time_t* epoch_utc)
{
  if (!M5.Rtc.isEnabled()) { return false; }
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) { return false; }

  struct tm t = { };
  t.tm_year = dt.date.year - 1900;
  t.tm_mon  = dt.date.month - 1;
  t.tm_mday = dt.date.date;
  t.tm_hour = dt.time.hours;
  t.tm_min  = dt.time.minutes;
  t.tm_sec  = dt.time.seconds;
  *epoch_utc = tab5_rtc_utc_para_epoch(&t);
  return true;
}

bool tab5_rtc_ler(time_t* epoch_utc)
{
  const int flags = tab5_rtc_flags();
  if (flags < 0 || (flags & RX8130_FLAG_VLF)) { return false; }

  time_t e = 0;
  if (!ler_calendario(&e) || !tab5_rtc_plausivel(e)) { return false; }
  *epoch_utc = e;
  return true;
}

bool tab5_rtc_definir(time_t epoch_utc)
{
  if (!M5.Rtc.isEnabled()) { return false; }
  if (!tab5_rtc_plausivel(epoch_utc)) {
    ESP_LOGW(TAG, "time %lld outside the plausible range - not written", (long long)epoch_utc);
    return false;
  }

  struct tm t = { };
  gmtime_r(&epoch_utc, &t);

  // RTC_Class::setDateTime(const tm*) returns void: a failure only shows up by
  // reading back, further down.
  M5.Rtc.setDateTime(&t);

  // THE SYSTEM CLOCK TOO, in the same call - see the note in the .hpp. Applies
  // even if the RTC write fails.
  struct timeval tv = { };
  tv.tv_sec  = epoch_utc;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  // Check what ended up in the chip.
  time_t lido = 0;
  if (!ler_calendario(&lido) || lido < epoch_utc - TOLERANCIA_S || lido > epoch_utc + TOLERANCIA_S) {
    ESP_LOGW(TAG, "RTC did not keep the written time (read %lld, expected %lld)",
             (long long)lido, (long long)epoch_utc);
    return false;
  }

  // Time written and verified: the chip's "time invalid" flag no longer applies.
  if (!M5.In_I2C.writeRegister8(RX8130_ENDERECO, RX8130_REG_FLAGS, RX8130_LIMPA_VLF, I2C_FREQ_HZ)) {
    ESP_LOGW(TAG, "could not clear the RTC's VLF");
  }
  return true;
}
