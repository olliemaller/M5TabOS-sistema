#include "ritmo.hpp"

#include "esp_timer.h"
#include "tab5_quadro.hpp"

// Start a little AFTER the scan: the ISR that records it has already run, and the
// page swap requested in this frame can only take effect on the next one.
static const int64_t FOLGA_US = 1500;
// A drawing that starts less than this before the next scan will not be ready in
// time: it goes on air on the following one (and the dt comes out right anyway).
static const int64_t EM_CIMA_US = 3000;

static const void* s_dono     = nullptr;
static int64_t     s_ao_ar    = 0;     // when the frame in progress goes on air (estimated)
static int64_t     s_prox_us  = 0;     // when to start the next periodic one (0 = none)
static float       s_dt       = 0.033f;

static uint32_t periodo_painel(void)
{
  uint32_t p = 0;
  tab5_quadro_atrasos(nullptr, &p);
  return (p >= 8000 && p <= 60000) ? p : 0;
}

void ritmo_quadro(const void* dono, uint32_t periodo_ms)
{
  const int64_t t  = esp_timer_get_time();
  const int64_t P  = periodo_painel();
  const int64_t vs = tab5_quadro_ultimo_fim_us();
  const int64_t pedido = (int64_t)periodo_ms * 1000;

  // No panel measurement (boot, right after waking): the old clock.
  const bool medido = P && vs > 0 && t >= vs && t - vs < 1000000;
  int64_t ao_ar = t;
  if (medido) {
    ao_ar = vs + ((t - vs) / P + 1) * P;   // the next scan
    if (ao_ar - t < EM_CIMA_US) { ao_ar += P; }
  }

  const int64_t ant = (dono == s_dono) ? s_ao_ar : 0;
  // Two frames never go on air in the same scan: the second waits for the swap
  // of the first (comp_abrir).
  if (medido && ant && ao_ar < ant + P) { ao_ar = ant + P; }
  s_dt = ant ? (float)(ao_ar - ant) * 1e-6f
             : (pedido ? (float)pedido * 1e-6f : 0.033f);
  if (s_dt < 0.001f) { s_dt = 0.001f; }
  s_dono  = dono;
  s_ao_ar = ao_ar;

  if (!periodo_ms) { s_prox_us = 0; return; }
  if (medido) {
    int64_t n = (pedido + P / 2) / P;      // whole scans, the closest to the request
    if (n < 1) { n = 1; }
    s_prox_us = ao_ar + (n - 1) * P + FOLGA_US;
  } else {
    s_prox_us = t + pedido;
  }
}

uint32_t ritmo_falta_ms(void)
{
  if (!s_prox_us) { return 0; }
  const int64_t t = esp_timer_get_time();
  if (t >= s_prox_us) { return 0; }
  return (uint32_t)((s_prox_us - t + 999) / 1000);
}

float ritmo_dt(void) { return s_dt; }
