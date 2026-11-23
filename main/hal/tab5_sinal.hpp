// DISTANCE FROM SIGNAL - estimate from RSSI, for Wi-Fi and Bluetooth.
//
// Log-distance model, the same one used by "find my tracker" apps:
//
//   d = 10 ^ ((P1m - RSSI) / (10 * n))
//
//   P1m - the expected RSSI at 1 metre from the transmitter;
//   n   - how much the environment attenuates (2 in free space, 2.5-3.5 indoors).
//
// IT IS AN ESTIMATE, and a rough one: walls, bodies, antenna orientation and reflections
// shift the RSSI by 5-10 dB, which multiplies or divides the distance by ~2.
// It is good for "near / in the room / far" and for seeing whether you're getting closer
// while walking with the device - not for measuring. That's why the screen rounds it.
#pragma once

#include <math.h>
#include <stdio.h>
#include <stddef.h>

// Typical P1m when the signal itself doesn't say: router (transmits ~20 dBm) and
// ordinary BLE device (~0 dBm). At 2.4 GHz the loss in the first metre is ~40 dB.
#define TAB5_SINAL_P1M_WIFI   (-40)
#define TAB5_SINAL_P1M_BLE    (-59)
#define TAB5_SINAL_PERDA_1M   41     // advertisement tx_power -> P1m
#define TAB5_SINAL_N_INTERNO  2.7f

static inline float tab5_sinal_distancia_m(int rssi, int p1m)
{
  return powf(10.0f, (float)(p1m - rssi) / (10.0f * TAB5_SINAL_N_INTERNO));
}

// "~40 cm", "~3 m", "~15 m", "> 50 m" - rounded to one significant figure that the
// estimate's error can still support.
static inline void tab5_sinal_distancia_txt(int rssi, int p1m, char* out, size_t tam)
{
  const float d = tab5_sinal_distancia_m(rssi, p1m);
  if      (d < 1.0f)  { snprintf(out, tam, "~%d cm", ((int)(d * 10.0f + 0.5f)) * 10); }
  else if (d < 10.0f) { snprintf(out, tam, "~%d m", (int)(d + 0.5f)); }
  else if (d < 50.0f) { snprintf(out, tam, "~%d m", ((int)(d / 5.0f + 0.5f)) * 5); }
  else                { snprintf(out, tam, "> 50 m"); }
}
