// MUSIC PLAYER - spectrum: FFT of the blocks entering the speaker queue
// and the ring of frames the screen reads. See tab5_musica_interno.hpp.
#include "tab5_musica_interno.hpp"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <atomic>
#include <cstring>
#include <math.h>

// ---------------------------------------------------------------------------
// SPECTRUM (see tab5_musica.hpp)
// ---------------------------------------------------------------------------
// 1024-point FFT (Hann), no overlap: one frame every ~23 ms of sound.
// The energy of each band (log, from 40 Hz to 16 kHz) becomes dB and the dB
// becomes 0..255. Measured cost: see the CHANGELOG / app musica estado.
#define FFT_N        1024
#define ESP_ANEL     128            // ~3 s of frames: covers the queue (~0.8 s) with room to spare
#define DB_PISO      (-72.0f)       // below this, empty bar

typedef struct { int64_t t_us; uint8_t v[TAB5_MUSICA_FAIXAS]; } quadro_esp_t;
static quadro_esp_t      s_anel[ESP_ANEL];
static uint32_t          s_anel_n = 0;          // frames written so far (the index is n % ESP_ANEL)
static std::atomic<int64_t> s_visual_ate{0};    // compute until this time (renewed on each read)
static float*            s_fft = nullptr;      // interleaved re/im, 2*FFT_N (PSRAM)
static float*            s_trig = nullptr;     // cos (N/2) and sin (N/2)
static float*            s_hann = nullptr;
static uint16_t          s_borda[TAB5_MUSICA_FAIXAS + 1];   // starting bin of each band
static uint32_t          s_borda_hz = 0;       // the rate the edges were computed for
int64_t                  g_musica_fim_prev_us = 0;    // when the already-queued sound ends (0 = empty queue)

bool musica_espectro_alocar(void)
{
  s_fft  = (float*)heap_caps_malloc(2 * FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
  s_trig = (float*)heap_caps_malloc(FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
  s_hann = (float*)heap_caps_malloc(FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!s_fft || !s_trig || !s_hann) { return false; }
  for (int k = 0; k < FFT_N / 2; ++k) {
    s_trig[k]              = cosf(2.0f * (float)M_PI * k / FFT_N);
    s_trig[FFT_N / 2 + k]  = sinf(2.0f * (float)M_PI * k / FFT_N);
  }
  for (int i = 0; i < FFT_N; ++i) { s_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1)); }
  s_borda_hz = 0;
  return true;
}

void musica_espectro_soltar(void)
{
  heap_caps_free(s_fft);  s_fft = nullptr;
  heap_caps_free(s_trig); s_trig = nullptr;
  heap_caps_free(s_hann); s_hann = nullptr;
}

// The speaker queue was discarded (pause, seek, track change): the frames
// that were waiting to go out never will.
void musica_espectro_esquecer(void)
{
  g_musica_trava.travar();
  s_anel_n = 0;
  g_musica_trava.destravar();
  g_musica_fim_prev_us = 0;
}

// Bands on a log scale, from 40 Hz to 16 kHz (or Nyquist): edges in bins.
static void espectro_bordas(uint32_t hz)
{
  const float f0 = 40.0f, f1 = fminf(16000.0f, hz * 0.5f);
  int ant = 1;
  for (int b = 0; b <= TAB5_MUSICA_FAIXAS; ++b) {
    const float f = f0 * powf(f1 / f0, (float)b / TAB5_MUSICA_FAIXAS);
    int bin = (int)(f * FFT_N / hz + 0.5f);
    if (bin <= ant && b > 0) { bin = ant + 1; }   // every band with at least one bin
    if (bin > FFT_N / 2) { bin = FFT_N / 2; }
    s_borda[b] = (uint16_t)bin;
    ant = bin;
  }
  s_borda_hz = hz;
}

static void fft_complexa(float* x)
{
  for (int i = 1, j = 0; i < FFT_N; ++i) {   // bit-reversed order
    int bit = FFT_N >> 1;
    for (; j & bit; bit >>= 1) { j ^= bit; }
    j ^= bit;
    if (i < j) {
      float t = x[2 * i]; x[2 * i] = x[2 * j]; x[2 * j] = t;
      t = x[2 * i + 1]; x[2 * i + 1] = x[2 * j + 1]; x[2 * j + 1] = t;
    }
  }
  for (int len = 2; len <= FFT_N; len <<= 1) {
    const int meio = len >> 1, passo = FFT_N / len;
    for (int i = 0; i < FFT_N; i += len) {
      for (int k = 0; k < meio; ++k) {
        const float wr = s_trig[k * passo], wi = -s_trig[FFT_N / 2 + k * passo];
        float* a = x + 2 * (i + k);
        float* b = x + 2 * (i + k + meio);
        const float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
        b[0] = a[0] - tr; b[1] = a[1] - ti;
        a[0] += tr;       a[1] += ti;
      }
    }
  }
}

// The frames of a block that has just entered the queue and starts playing at
// 'ini_us'.
void musica_espectro_bloco(const int16_t* pcm, size_t q, uint32_t hz, bool estereo, int64_t ini_us)
{
  if (esp_timer_get_time() > s_visual_ate.load() || !s_fft || !hz) { return; }
  if (hz != s_borda_hz) { espectro_bordas(hz); }
  // 0 dB reference: a full-scale sine, Hann window (peak ~A*N/4).
  const float ref = 32767.0f * FFT_N / 4.0f;
  for (size_t j = 0; j + FFT_N <= q; j += FFT_N) {
    for (int i = 0; i < FFT_N; ++i) {
      const size_t k = j + (size_t)i;
      const float v = estereo ? 0.5f * ((float)pcm[2 * k] + (float)pcm[2 * k + 1]) : (float)pcm[k];
      s_fft[2 * i] = v * s_hann[i];
      s_fft[2 * i + 1] = 0.0f;
    }
    fft_complexa(s_fft);
    quadro_esp_t qd;
    qd.t_us = ini_us + (int64_t)(j + FFT_N / 2) * 1000000 / hz;
    for (int b = 0; b < TAB5_MUSICA_FAIXAS; ++b) {
      float soma = 0.0f;
      for (int k = s_borda[b]; k < s_borda[b + 1]; ++k) {
        soma += s_fft[2 * k] * s_fft[2 * k] + s_fft[2 * k + 1] * s_fft[2 * k + 1];
      }
      const float db = 10.0f * log10f(soma / (ref * ref) + 1e-12f);
      float n = (db - DB_PISO) / -DB_PISO;
      n = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
      qd.v[b] = (uint8_t)(n * 255.0f);
    }
    g_musica_trava.travar();
    s_anel[s_anel_n % ESP_ANEL] = qd;
    ++s_anel_n;
    g_musica_trava.destravar();
  }
}

bool tab5_musica_espectro(uint8_t* faixas)
{
  const int64_t agora = esp_timer_get_time();
  s_visual_ate = agora + 1000000;   // someone is watching: keep computing for 1 more s
  bool achou = false;
  g_musica_trava.travar();
  // The newest frame that has already started playing (the ring is in time order).
  const uint32_t n = s_anel_n, de = n > ESP_ANEL ? n - ESP_ANEL : 0;
  for (uint32_t i = n; i-- > de;) {
    const quadro_esp_t& qd = s_anel[i % ESP_ANEL];
    if (qd.t_us <= agora) {
      if (agora - qd.t_us < 100000) { memcpy(faixas, qd.v, TAB5_MUSICA_FAIXAS); achou = true; }
      break;   // older than 100 ms: the sound stopped (pause, end of queue)
    }
  }
  g_musica_trava.destravar();
  return achou;
}

