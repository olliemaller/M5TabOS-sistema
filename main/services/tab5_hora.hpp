// TIME SYNC - sets the RTC (and the system clock along with it, see
// tab5_rtc.hpp) from the BEST source available at the moment.
//
// -------------------------------------------------------------------------
// THE SOURCES, from most to least reliable
// -------------------------------------------------------------------------
// NTP (internet, with WiFi connected) - a real time server, error
// of milliseconds. Starts by itself the first time WiFi gets an IP.
//
// GPS (tab5_gps_tempo()) - the receiver has its own clock, synchronised
// by the satellites. Only counts WITH A FIX: before that many receivers send the
// time from their internal clock, which could be anything.
//
// Mesh (tab5_mesh_hora_viva()) - the Meshtastic node's clock, read from the rx_time
// of a packet that arrived NOW. It doesn't count the last_heard from the node list nor
// what was sitting in the node's queue during the connection: those are times from the PAST,
// and used as "now" they set the clock back by hours or days - a message
// sent in that interval gets stamped with the wrong date.
//
// -------------------------------------------------------------------------
// CONSISTENCY
// -------------------------------------------------------------------------
// - A source better than the current one sets the time immediately; the same source, every
//   RESSINCRONIZAR_MS; a WORSE source only after the better one has been gone for
//   VALIDADE_MS. That way Mesh never undoes what NTP or GPS set.
// - JUMP: when a correction changes the clock by more than SALTO_S, whoever stamped
//   things with the old time is notified (tab5_hora_ao_saltar) and corrects its
//   own timestamps from this session - the message sent before the correction leaves
//   the wrong day along with the clock.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_timer.h"
struct tm;   // forward-declared only for tab5_hora_local()'s pointer -
             // whoever really uses struct tm includes <ctime> themselves.

// Checks the sources and adjusts the RTC when the consistency rule above
// allows. Call 1x/s, FROM THE UI TASK - it is the one that may write to the RTC
// (see tab5_rtc.hpp).
void tab5_hora_sincronizar(void);

// Where this session's last correction came from: "internet", "GPS", "Mesh", or
// "relogio interno" (internal clock: no source yet - whatever the RTC kept applies).
const char* tab5_hora_fonte(void);

// JUMP notice: 'delta_s' is how far the clock moved (positive = went
// forward). Called from the UI task, right after the correction; subscribers
// only correct their own timestamps and return (no SD card, no long
// wait). Up to 4 subscribers.
typedef void (*tab5_hora_salto_cb)(int32_t delta_s);
void tab5_hora_ao_saltar(tab5_hora_salto_cb cb);

// -------------------------------------------------------------------------
// TIME ZONE.
// -------------------------------------------------------------------------
// CHOSEN BY THE USER: a region with its daylight-saving rule
// (services/tab5_fuso, ajustes_fuso_zona) or a fixed offset (ajustes_fuso_min),
// in the welcome assistant and Settings > Date and time.
//
// ONLY AFFECTS DISPLAY. The RTC, the sync and the logs stay in PURE UTC
// on purpose - it is the only way to compare timestamps between mesh nodes,
// log files and GPS without ambiguity. Only whoever is going to SHOW the time
// calls this - gmtime_r() directly is still right for any other use.
int tab5_hora_fuso_s(void);
#define TAB5_FUSO_S (tab5_hora_fuso_s())

// HAS THE CLOCK BEEN SET? Below this (2020-09-13) a time isn't
// real: the RTC never set (no GPS, network or mesh since boot) or an
// old timestamp stored as 0. It was the same number written in twelve places,
// under four different names - a single one, here.
#define TAB5_EPOCH_VALIDO 1600000000u
static inline bool tab5_hora_valida(uint32_t epoch_utc) { return epoch_utc > TAB5_EPOCH_VALIDO; }

// NOW, in UTC epoch - or 0 while the clock hasn't been set. 0 is the
// "undated" that screens, files and Meshtastic already know how to handle.
static inline uint32_t tab5_hora_epoch(void)
{
  const time_t t = time(nullptr);
  return (t > (time_t)TAB5_EPOCH_VALIDO) ? (uint32_t)t : 0;
}

// Converts UTC epoch to struct tm IN THE LOCAL TIME ZONE (TAB5_FUSO_S) - same
// format as gmtime_r() (which is what it does underneath, only on the already
// shifted epoch).
void tab5_hora_local(uint32_t epoch_utc, struct tm* out);

// HH:MM AS THE USER LIKES IT: "21:05", or "9:05pm" with the 12-hour clock
// (Settings > Date and time). Fits in 8 bytes either way.
void tab5_hora_hhmm(const struct tm* t, char* out, size_t n);

// THE DAY'S FILE NAME: "<prefixo>AAAA-MM-DD<sufixo>", on the LOCAL day - the
// user's day. Every daily file in the system (log, energy, monitor) uses
// this, so files for the same day roll over together at midnight (with each one doing
// the sum its own way, the UTC log rolled over 3 hours before the others).
// Only the NAME is local: the content stays stamped in UTC.
// Clock not yet set (epoch before 2020): "<prefixo>sem-data<sufixo>".
//   tab5_hora_nome_do_dia(time(nullptr), "log_", ".txt", nome, sizeof(nome));
void tab5_hora_nome_do_dia(uint32_t epoch_utc, const char* prefixo, const char* sufixo,
                           char* out, size_t tam);

// MONOTONIC CLOCK, in ms since boot - for measuring intervals, never for
// showing the time. One for the whole system (there were eight local copies, half
// on the FreeRTOS tick and half on esp_timer). Wraps around in ~49 days:
// always compare by difference (now - before), never by greater/less than.
static inline uint32_t tab5_agora_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// SECONDS SINCE THE PREVIOUS CALL, rounded, for whoever accumulates time on
// each measurement (battery, power test, Wi-Fi doze). When idle the
// kernel measures every 5 s, awake every 1 (power-saving contract, app.h).
// First call = 1 s; a big gap (count restart) counts as at
// most 10. There were three identical copies.
static inline int tab5_passou_s(int64_t* ult)
{
  const int64_t agora = esp_timer_get_time();
  int s = *ult ? (int)((agora - *ult + 500000) / 1000000) : 1;
  *ult = agora;
  if (s < 0)  { s = 0; }
  if (s > 10) { s = 10; }
  return s;
}
