#include "registro.h"
#include "tab5_power.hpp"
#include "tab5_sensores.hpp"
#include "tab5_mesh.hpp"
#include <time.h>
#include "tab5_sd.hpp"
#include "tab5_hora.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "registro";

// One sample per minute: a full discharge (5 to 15 hours) yields 300 to
// 900 points, more than enough resolution to plot the curve and calibrate the battery.
// Every 10 minutes it goes to the SD card.
static const uint32_t AMOSTRA_MS = 60000;
static const int      LOTE       = 5;       // samples per flush (was 10; the Energia app's TODAY chart reads from here)
static const int      CAPACIDADE = 32;      // slack: if the SD card fails, it holds more

typedef struct {
  uint32_t t;        // seconds since boot
  uint32_t epoch;    // real UTC time of the sample; 0 = clock not set
  float    volts;
  float    mA;
  float    mW;
  int16_t  pct;
  int16_t  mhz;
  uint8_t  repouso;
  float    temp;     // P4 silicon, -273 when not measured
  char     app[12];
} amostra_t;

static amostra_t s_buf[CAPACIDADE];
static int       s_n = 0;
static uint32_t  s_gravadas = 0;
static uint32_t  s_falhas = 0;
static char      s_app[12] = "home";

void registro_app_atual(const char* nome)
{
  // Copy by value: the app's pointer may die when it closes.
  snprintf(s_app, sizeof(s_app), "%s", nome ? nome : "home");
}

void registro_estado(int* na_ram, uint32_t* gravadas, uint32_t* falhas)
{
  if (na_ram)   { *na_ram   = s_n; }
  if (gravadas) { *gravadas = s_gravadas; }
  if (falhas)   { *falhas   = s_falhas; }
}

// ---------------------------------------------------------------------------
// TASK AND HEAP HEALTH - see the full explanation in registro.h.
// ---------------------------------------------------------------------------
// Up to 32 simultaneous tasks - the same ceiling the Tarefas (Tasks) app already uses
// (tarefas/app_tarefas.cpp). Repeated here rather than shared on
// purpose: it is a CONSTANT, not state, and the two readings are
// independent of each other - the app may not even be open.
// (The task snapshot in tarefas.csv, on every flush, moved to
// the system monitor (services/tab5_monitor), which measures the tasks every
// second, with peaks and events, and writes what matters to the SD card.)

// ONE FILE PER DAY, WITH THE REAL TIME. Until now it was a single
// energia.csv, growing forever, with time in seconds since
// BOOT - impossible to cut "the day" out of it. Now:
//   Sistema/energia/energia-AAAA-MM-DD.csv   (LOCAL date - the user's day)
//   ENE3,<epoch UTC>,<HH:MM local>,<V>,<mA>,<mW>,<pct>,<MHz>,<repouso>,<temp>,<app>
// With no clock set (no GPS/mesh since boot) the samples go to
// energia-sem-data.csv, with epoch 0. The old energia.csv stays on the SD card,
// untouched.
static void nome_do_dia(uint32_t epoch, char* out, size_t tam)
{
  tab5_hora_nome_do_dia(epoch, "energia-", ".csv", out, tam);
}

// Through the system writer (hal/tab5_sd.hpp): each sample becomes a line
// handed over immediately; mounting, opening and writing are up to the "cartao" task. The
// header goes along and is only written if the day's file is empty.
static void descarregar(void)
{
  if (s_n == 0) { return; }
  int ok = 0;
  for (int i = 0; i < s_n; ++i) {
    const amostra_t* a = &s_buf[i];
    char nome[32];
    nome_do_dia(a->epoch, nome, sizeof(nome));
    char hora[8] = "--:--";
    if (a->epoch) { struct tm lt; tab5_hora_local(a->epoch, &lt); snprintf(hora, sizeof(hora), "%02d:%02d", lt.tm_hour, lt.tm_min); }
    char linha[160];
    snprintf(linha, sizeof(linha), "ENE3,%lu,%s,%.3f,%.1f,%.1f,%d,%d,%d,%.1f,%s\n",
             (unsigned long)a->epoch, hora, a->volts, a->mA, a->mW,
             (int)a->pct, (int)a->mhz, (int)a->repouso, a->temp, a->app);
    if (tab5_sd_anexar(TAB5_SD_ENERGIA, nome, linha,
                       "tipo,epoch,hora,volts,mA,mW,pct,mhz,repouso,temp_c,app\n")) { ++ok; }
  }
  if (ok == 0) { s_falhas++; return; }
  s_gravadas += ok;
  s_n = 0;
}

// An ENE3 line from the day's file becomes its minute in 'd'. A line in another
// format, before 't0' or after the 24 h is ignored.
static void ler_arquivo_dia(registro_dia_t* d, const char* nome)
{
  FILE* f = tab5_sd_abrir(TAB5_SD_ENERGIA, nome, "r");
  if (!f) { return; }
  char linha[160];
  while (fgets(linha, sizeof(linha), f)) {
    unsigned long ep; char hora[8]; float v, ma, mw, temp; int pct, mhz, rep;
    if (sscanf(linha, "ENE3,%lu,%7[^,],%f,%f,%f,%d,%d,%d,%f", &ep, hora, &v, &ma, &mw, &pct, &mhz, &rep, &temp) < 9) { continue; }
    if (ep < d->t0) { continue; }
    const uint32_t m = ((uint32_t)ep - d->t0) / 60;
    if (m >= REGISTRO_MIN_DIA) { continue; }
    d->tem[m] = rep ? 2 : 1;
    d->v[m] = v; d->ma[m] = ma; d->temp[m] = temp;
    d->pct[m] = (int8_t)((pct >= 0 && pct <= 100) ? pct : -1);
  }
  fclose(f);
}

bool registro_ler_dia(registro_dia_t* d, uint32_t t0)
{
  memset(d, 0, sizeof(*d));
  d->t0 = t0;
  if (!t0 || !tab5_sd_montar()) { return false; }
  // The 24 h window may cross midnight: t0's day and today's
  // (the same file, if it didn't cross, is read only once).
  char nome_t0[40], nome_hoje[40];
  nome_do_dia(t0, nome_t0, sizeof(nome_t0));
  nome_do_dia((uint32_t)time(nullptr), nome_hoje, sizeof(nome_hoje));
  ler_arquivo_dia(d, nome_t0);
  if (strcmp(nome_t0, nome_hoje) != 0) { ler_arquivo_dia(d, nome_hoje); }
  tab5_sd_desmontar();
  return true;
}

// NO TASK OF ITS OWN: one sample a minute isn't worth 4 KB of idle stack.
// It piggybacks on the "cartao" task (tab5_sd_carona), which already wakes every 1 s and
// is what writes to the SD card anyway; here we just count the minute.
static void registro_na_carona(void)
{
  static int64_t proxima_us = esp_timer_get_time() + (int64_t)AMOSTRA_MS * 1000;
  const int64_t agora_us = esp_timer_get_time();
  if (agora_us < proxima_us) { return; }
  proxima_us += (int64_t)AMOSTRA_MS * 1000;
  if (proxima_us <= agora_us) { proxima_us = agora_us + (int64_t)AMOSTRA_MS * 1000; }   // fell too far behind: restart
  {
    // Reads from the CACHE, never from I2C: the bus belongs to the UI task, which
    // updates this value every second. Going to I2C from here would reproduce the
    // race that already cost us an afternoon (voltage alternating 4.37/8.37V).
    tab5_medida_t m = tab5_power_ultima();

    if (s_n >= CAPACIDADE) {
      // Buffer full (SD card absent for a long time): discard the oldest.
      // Losing the distant past is better than losing the present.
      memmove(&s_buf[0], &s_buf[1], sizeof(amostra_t) * (CAPACIDADE - 1));
      s_n = CAPACIDADE - 1;
    }

    amostra_t* a = &s_buf[s_n++];
    a->t       = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    a->epoch = tab5_hora_epoch();
    a->volts   = m.volts;
    a->mA      = m.mA;
    a->mW      = m.mW;
    a->pct     = (int16_t)m.pct;
    a->mhz     = (int16_t)tab5_power_cpu_mhz();
    a->repouso = tab5_power_em_repouso() ? 1 : 0;
    { float c; a->temp = tab5_sensores_temp(&c) ? c : -273.0f; }
    snprintf(a->app, sizeof(a->app), "%s", s_app);

    // Flushes ONLY per batch. There is deliberately no early write: during
    // development the board restarts on every upload, and a write in the very
    // first minute would fill the file with stray samples from sessions that
    // lasted seconds. To find out whether the logger is alive without waiting ten
    // minutes, look at the Energia app - it shows how many samples are in RAM.
    //
    // WITH THE BATTERY NEARLY FLAT, THE BATCH BECOMES ONE - and not every sample is worth the same.
    //
    // At ten minutes per flush, a battery shutdown takes away up to
    // ten samples. And precisely THOSE ten: the end of the discharge is where the voltage
    // plummets, the knee of the curve, the only part that says how much time is
    // left when the answer matters. The rest of the file has hundreds of
    // full-battery samples, all of which look alike.
    //
    // Below 15% the device may shut down any minute, and then the cost of
    // writing every time (one SD card mount per minute) is cheap next to
    // losing the only stretch that can't be reproduced without discharging the
    // whole battery again.
    const bool no_fim = (m.pct >= 0 && m.pct < 15);
    if (s_n >= (no_fim ? 1 : LOTE)) { descarregar(); }
  }
}

void registro_iniciar(void)
{
  // Once a minute reads the energy cache and, batch by batch, hands the
  // lines to the writer - all inside the "cartao" task.
  tab5_sd_carona(registro_na_carona);
  ESP_LOGI(TAG, "running: sample every %lus, flush every %d samples",
           (unsigned long)(AMOSTRA_MS / 1000), LOTE);
}
