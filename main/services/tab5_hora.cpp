#include "tab5_hora.hpp"
#include "tab5_fuso.hpp"
#include "tab5_rtc.hpp"
#include "tab5_gps.hpp"
#include "tab5_mesh.hpp"
#include "tab5_wifi.hpp"
#include "ajustes.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/time.h>

static const char* TAG = "hora";

// A timestamp older than this means "this source hasn't really synced
// yet" (2025-01-01T00:00:00Z).
#define EPOCH_MINIMO 1735689600u

// DOESN'T WRITE THE RTC EVERY ROUND - the chip has a finite write life, and the
// system clock runs on its own between corrections. 10 minutes corrects the drift
// without hammering the I2C bus.
#define RESSINCRONIZAR_MS (10u * 60u * 1000u)

// A worse source only takes over after the better one has gone this long without setting the time.
#define VALIDADE_MS (6u * 60u * 60u * 1000u)

// Below this the difference is drift, not a wrong clock: nobody needs to
// correct timestamps for it.
#define SALTO_S 60

// An NTP reply only counts as "now" while it is FRESH (lwIP
// asks again every CONFIG_LWIP_SNTP_UPDATE_DELAY, 1 h). It used to count for 2 h,
// extended by esp_timer - but in light sleep esp_timer runs off the internal RC
// (~5% error): every RESSINCRONIZAR_MS the old "extended" reply wrote
// an already-late time to the RTC (~3 min/h), spoiling the reference. Between two
// replies the time is held by the RTC (crystal), in corrigir_deriva_pelo_rtc.
#define NTP_VALIDO_MS (30u * 1000u)

typedef enum { NIVEL_NENHUM = 0, NIVEL_MESH, NIVEL_GPS, NIVEL_NTP } nivel_t;
static const char* const NOME_NIVEL[] = { "relogio interno", "Mesh", "GPS", "internet" };
static const char* const PELO_NIVEL[] = { "pelo relogio interno", "pelo Mesh", "pelo GPS", "pela internet" };

static nivel_t  s_nivel          = NIVEL_NENHUM;
static uint32_t s_ultima_sync_ms = 0;

// Light sleep measures time with the internal RC (~5% error, ~3 min/h lag).
// The RTC (RX8130, with a crystal) is the reference between the sources' corrections: every
// CONFERIR_RTC_MS the system clock is pulled back to it.
#define CONFERIR_RTC_MS (60u * 1000u)

// Below this the system clock is left alone (the RTC has 1 s resolution).
#define DERIVA_MINIMA_S 2

// Only logs the RTC correction when it is this size or larger.
#define LOGAR_CORRECAO_S 5

// At most 1 implausible-source notice per minute, per source.
#define AVISO_FONTE_MS (60u * 1000u)

static bool     s_rtc_conferido_no_boot = false;
static uint32_t s_ultima_conf_rtc_ms    = 0;
static uint32_t s_aviso_fonte_ms[4]     = { 0, 0, 0, 0 };

#define MAX_ASSINANTES 4
static tab5_hora_salto_cb s_assinantes[MAX_ASSINANTES];


// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------
// lwIP calls sntp_sync_time() with the received time, and the default (weak) version
// does settimeofday immediately. This one replaces it: the reply is only NOTED, and the
// correction goes through the same gateway as the other sources - with priority, the RTC
// and the jump notice. Runs in the lwIP task; the two fields are read in the UI
// one, and a torn pair would cost at most one postponed correction.
static volatile uint32_t s_ntp_epoch = 0;
static volatile uint32_t s_ntp_ms    = 0;
static bool              s_sntp_de_pe = false;

extern "C" void sntp_sync_time(struct timeval* tv)
{
  s_ntp_ms    = tab5_agora_ms();
  s_ntp_epoch = (uint32_t)tv->tv_sec;
}

static void garantir_sntp(void)
{
  if (s_sntp_de_pe || tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return; }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&cfg) == ESP_OK) {
    s_sntp_de_pe = true;
    ESP_LOGI(TAG, "NTP on (pool.ntp.org)");
  }
}

static uint32_t hora_ntp(void)
{
  const uint32_t ms = s_ntp_ms, ep = s_ntp_epoch;
  if (ms == 0 || ep < EPOCH_MINIMO || (tab5_agora_ms() - ms) > NTP_VALIDO_MS) { return 0; }
  return ep + (tab5_agora_ms() - ms) / 1000;
}

// ---------------------------------------------------------------------------
// GPS - only with a fix and with the receiver talking now
// ---------------------------------------------------------------------------
// UTC date -> epoch conversion: tab5_rtc_utc_para_epoch (in the RTC HAL).
static uint32_t hora_gps(void)
{
  if (!tab5_gps_no_ar()) { return 0; }   // RMC stored from a receiver that has gone quiet
  const nmea_tempo_t g = tab5_gps_tempo();
  if (!g.valida || !g.fix) { return 0; }
  struct tm t = { };
  t.tm_year = g.ano - 1900;
  t.tm_mon  = g.mes - 1;
  t.tm_mday = g.dia;
  t.tm_hour = g.hora;
  t.tm_min  = g.min;
  t.tm_sec  = g.seg;
  const time_t e = tab5_rtc_utc_para_epoch(&t);
  return (e > (time_t)EPOCH_MINIMO) ? (uint32_t)e : 0;
}

// ---------------------------------------------------------------------------
// SETTING THE TIME
// ---------------------------------------------------------------------------
static bool pode_usar(nivel_t nivel)
{
  if (nivel > s_nivel) { return true; }   // better source: immediately
  const uint32_t desde = tab5_agora_ms() - s_ultima_sync_ms;
  if (nivel == s_nivel) { return desde >= RESSINCRONIZAR_MS; }
  return desde >= VALIDADE_MS;            // the better one disappeared a while ago
}

// How much and which way the clock was wrong, in words. The log said
// "relogio mudou +1s", and there was no telling whether the +1 was the clock running
// forward, the correction or the time-zone difference.
static void descrever_ajuste(int64_t antes, int64_t delta, char* out, size_t n)
{
  if (antes < (int64_t)EPOCH_MINIMO) { snprintf(out, n, "the clock had no time yet"); return; }
  if (delta == 0)                     { snprintf(out, n, "the clock was already right"); return; }
  const long long a = (long long)(delta < 0 ? -delta : delta);
  const char* lado  = (delta > 0) ? "behind" : "ahead";
  if      (a < 120)        { snprintf(out, n, "the clock was %lld s %s", a, lado); }
  else if (a < 2 * 3600)   { snprintf(out, n, "the clock was %lld min %s", a / 60, lado); }
  else if (a < 2 * 86400)  { snprintf(out, n, "the clock was %lld h %s", a / 3600, lado); }
  else                     { snprintf(out, n, "the clock was %lld days %s", a / 86400, lado); }
}

// Notifies the subscribers when the clock jumped. The old clock is only comparable
// if it was a real time: coming from 1970 (RTC never set) nothing was
// stamped - a timestamp below the minimum becomes "undated" everywhere.
static void avisar_salto(int64_t antes, int64_t delta)
{
  if ((delta > SALTO_S || delta < -SALTO_S) && antes > (int64_t)EPOCH_MINIMO) {
    for (int i = 0; i < MAX_ASSINANTES; ++i) {
      if (s_assinantes[i]) { s_assinantes[i]((int32_t)delta); }
    }
  }
}

static bool acertar(uint32_t epoch, nivel_t nivel)
{
  // A source with an absurd time (e.g. a Mesh node with a wrong clock) doesn't get into the
  // RTC or the system clock.
  if (!tab5_rtc_plausivel((time_t)epoch)) {
    const uint32_t agora = tab5_agora_ms();
    if (s_aviso_fonte_ms[nivel] == 0 || agora - s_aviso_fonte_ms[nivel] >= AVISO_FONTE_MS) {
      s_aviso_fonte_ms[nivel] = agora ? agora : 1;
      ESP_LOGW(TAG, "time %s ignored: outside the plausible range (%lu)", NOME_NIVEL[nivel], (unsigned long)epoch);
    }
    return false;
  }

  const int64_t antes = (int64_t)time(nullptr);
  if (!tab5_rtc_definir((time_t)epoch)) { return false; }
  const int64_t delta = (int64_t)epoch - antes;

  if (nivel != s_nivel || delta > SALTO_S || delta < -SALTO_S) {
    // LOCAL TIME in the log, the same the bar shows. In UTC ("18:19Z") it
    // looked wrong to the reader, who is 3 h behind.
    struct tm t = { };
    tab5_hora_local(epoch, &t);
    char ajuste[48];
    descrever_ajuste(antes, delta, ajuste, sizeof(ajuste));
    const int fm = ajustes_fuso_min();
    ESP_LOGI(TAG, "time set %s: %02d/%02d/%04d %02d:%02d:%02d (UTC%c%d:%02d) - %s",
             PELO_NIVEL[nivel], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
             t.tm_hour, t.tm_min, t.tm_sec, fm < 0 ? '-' : '+', abs(fm) / 60, abs(fm) % 60, ajuste);
  }
  s_nivel = nivel;
  s_ultima_sync_ms = tab5_agora_ms();

  avisar_salto(antes, delta);
  return true;
}

// At boot: the system clock came from the RTC (M5.begin). If the RTC has no valid
// time, that clock is a false date - zero it so logs come out "sem-data" (undated)
// until a source arrives (internet/GPS/Mesh).
static void conferir_rtc_no_boot(void)
{
  const int flags = tab5_rtc_flags();
  ESP_LOGI(TAG, "RTC flags: 0x%02x", flags < 0 ? 0 : flags);
  if (flags >= 0 && (flags & 0x80)) { ESP_LOGW(TAG, "RTC battery low"); }

  time_t rtc = 0;
  if (tab5_rtc_ler(&rtc)) { return; }

  ESP_LOGW(TAG, "RTC has no valid time (flags 0x%02x) - waiting for internet/GPS/Mesh", flags < 0 ? 0 : flags);
  const struct timeval zero = { 0, 0 };
  settimeofday(&zero, nullptr);
}

// Between source corrections, the system clock lags in light sleep (~5% RC).
// The RTC has a crystal: if it is valid (tab5_rtc_ler true) and diverges, it
// rules. Doesn't write the RTC here - it is the reference. An invalid RTC (VLF, absurd
// year) never corrects anything.
static void corrigir_deriva_pelo_rtc(void)
{
  time_t rtc = 0;
  if (!tab5_rtc_ler(&rtc)) { return; }

  const int64_t antes = (int64_t)time(nullptr);
  const int64_t delta = (int64_t)rtc - antes;
  if (delta > -DERIVA_MINIMA_S && delta < DERIVA_MINIMA_S) { return; }

  // +0.5 s: the RTC truncates to whole seconds, the half second centres the error.
  struct timeval tv = { };
  tv.tv_sec  = rtc;
  tv.tv_usec = 500000;
  settimeofday(&tv, nullptr);

  if (delta >= LOGAR_CORRECAO_S || delta <= -LOGAR_CORRECAO_S) {
    char ajuste[48];
    descrever_ajuste(antes, delta, ajuste, sizeof(ajuste));
    ESP_LOGI(TAG, "clock corrected by the RTC: %s", ajuste);
  }
  avisar_salto(antes, delta);
}

void tab5_hora_sincronizar(void)
{
  if (!s_rtc_conferido_no_boot) {
    s_rtc_conferido_no_boot = true;
    s_ultima_conf_rtc_ms = tab5_agora_ms();
    conferir_rtc_no_boot();
  } else if (tab5_agora_ms() - s_ultima_conf_rtc_ms >= CONFERIR_RTC_MS) {
    s_ultima_conf_rtc_ms = tab5_agora_ms();
    corrigir_deriva_pelo_rtc();
  }

  garantir_sntp();

  const uint32_t ntp = hora_ntp();
  if (ntp && pode_usar(NIVEL_NTP)) { acertar(ntp, NIVEL_NTP); return; }

  const uint32_t gps = hora_gps();
  if (gps && pode_usar(NIVEL_GPS)) { acertar(gps, NIVEL_GPS); return; }

  const uint32_t mesh = tab5_mesh_hora_viva();
  if (mesh > EPOCH_MINIMO && pode_usar(NIVEL_MESH)) { acertar(mesh, NIVEL_MESH); }
}

const char* tab5_hora_fonte(void) { return NOME_NIVEL[s_nivel]; }

void tab5_hora_ao_saltar(tab5_hora_salto_cb cb)
{
  for (int i = 0; i < MAX_ASSINANTES; ++i) {
    if (s_assinantes[i] == cb) { return; }
    if (!s_assinantes[i]) { s_assinantes[i] = cb; return; }
  }
  ESP_LOGW(TAG, "jump subscribers exhausted");
}

int tab5_hora_fuso_s(void) { return tab5_fuso_deslocamento_s((uint32_t)time(nullptr)); }

void tab5_hora_hhmm(const struct tm* t, char* out, size_t n)
{
  if (!ajustes_relogio_12h()) { snprintf(out, n, "%02d:%02d", t->tm_hour, t->tm_min); return; }
  const int h = t->tm_hour % 12;
  snprintf(out, n, "%d:%02d%s", h ? h : 12, t->tm_min, t->tm_hour < 12 ? "am" : "pm");
}

void tab5_hora_local(uint32_t epoch_utc, struct tm* out)
{
  // The offset AT THAT MOMENT: a message from last winter shows winter time.
  const time_t deslocado = (time_t)epoch_utc + tab5_fuso_deslocamento_s(epoch_utc);
  gmtime_r(&deslocado, out);
}

void tab5_hora_nome_do_dia(uint32_t epoch_utc, const char* prefixo, const char* sufixo,
                           char* out, size_t tam)
{
  if (!tab5_rtc_plausivel((time_t)epoch_utc)) {   // out of range: no real time
    snprintf(out, tam, "%ssem-data%s", prefixo, sufixo);
    return;
  }
  struct tm lt;
  tab5_hora_local(epoch_utc, &lt);
  snprintf(out, tam, "%s%04d-%02d-%02d%s", prefixo, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, sufixo);
}
