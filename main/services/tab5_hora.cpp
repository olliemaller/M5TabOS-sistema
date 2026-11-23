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

// Carimbo mais antigo que isto e "essa fonte ainda nao sincronizou de
// verdade" (2025-01-01T00:00:00Z).
#define EPOCH_MINIMO 1735689600u

// NAO ESCREVE O RTC TODA VOLTA - o chip tem vida finita de escrita, e o
// relogio do sistema anda sozinho entre acertos. 10 minutos corrige a deriva
// sem martelar o barramento I2C.
#define RESSINCRONIZAR_MS (10u * 60u * 1000u)

// Uma fonte pior so assume depois de a melhor ficar este tempo sem acertar.
#define VALIDADE_MS (6u * 60u * 60u * 1000u)

// Abaixo disto a diferenca e deriva, nao relogio errado: ninguem precisa
// corrigir carimbo por isso.
#define SALTO_S 60

// Uma resposta do NTP so vale como "agora" enquanto esta FRESCA (o lwIP
// pergunta de novo a cada CONFIG_LWIP_SNTP_UPDATE_DELAY, 1 h). Antes valia 2 h,
// estendida pelo esp_timer - mas no sono leve o esp_timer anda pelo RC interno
// (~5% de erro): a cada RESSINCRONIZAR_MS a resposta velha "estendida" gravava
// no RTC uma hora ja atrasada (~3 min/h), estragando a referencia. Entre duas
// respostas quem segura a hora e o RTC (cristal), em corrigir_deriva_pelo_rtc.
#define NTP_VALIDO_MS (30u * 1000u)

typedef enum { NIVEL_NENHUM = 0, NIVEL_MESH, NIVEL_GPS, NIVEL_NTP } nivel_t;
static const char* const NOME_NIVEL[] = { "relogio interno", "Mesh", "GPS", "internet" };
static const char* const PELO_NIVEL[] = { "pelo relogio interno", "pelo Mesh", "pelo GPS", "pela internet" };

static nivel_t  s_nivel          = NIVEL_NENHUM;
static uint32_t s_ultima_sync_ms = 0;

// Sono leve mede o tempo com o RC interno (~5% de erro, ~3 min/h de atraso).
// O RTC (RX8130, com cristal) e a referencia entre os acertos das fontes: a
// cada CONFERIR_RTC_MS o relogio do sistema e puxado de volta para ele.
#define CONFERIR_RTC_MS (60u * 1000u)

// Abaixo disto o relogio do sistema e deixado quieto (o RTC tem resolucao de 1 s).
#define DERIVA_MINIMA_S 2

// So loga a correcao pelo RTC quando ela e deste tamanho ou maior.
#define LOGAR_CORRECAO_S 5

// No maximo 1 aviso de fonte implausivel por minuto, por fonte.
#define AVISO_FONTE_MS (60u * 1000u)

static bool     s_rtc_conferido_no_boot = false;
static uint32_t s_ultima_conf_rtc_ms    = 0;
static uint32_t s_aviso_fonte_ms[4]     = { 0, 0, 0, 0 };

#define MAX_ASSINANTES 4
static tab5_hora_salto_cb s_assinantes[MAX_ASSINANTES];


// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------
// O lwIP chama sntp_sync_time() com a hora recebida, e a versao padrao (weak)
// faz settimeofday na hora. Esta a substitui: a resposta so e ANOTADA, e o
// acerto passa pela mesma porta das outras fontes - com a prioridade, o RTC
// e o aviso de salto. Roda na tarefa do lwIP; os dois campos sao lidos na de
// interface, e um par rasgado custaria no maximo um acerto adiado.
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
    ESP_LOGI(TAG, "NTP ligado (pool.ntp.org)");
  }
}

static uint32_t hora_ntp(void)
{
  const uint32_t ms = s_ntp_ms, ep = s_ntp_epoch;
  if (ms == 0 || ep < EPOCH_MINIMO || (tab5_agora_ms() - ms) > NTP_VALIDO_MS) { return 0; }
  return ep + (tab5_agora_ms() - ms) / 1000;
}

// ---------------------------------------------------------------------------
// GPS - so com fix e com o receptor falando agora
// ---------------------------------------------------------------------------
// Conversao data UTC -> epoch: tab5_rtc_utc_para_epoch (no HAL do RTC).
static uint32_t hora_gps(void)
{
  if (!tab5_gps_no_ar()) { return 0; }   // RMC guardado de um receptor que calou
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
// O ACERTO
// ---------------------------------------------------------------------------
static bool pode_usar(nivel_t nivel)
{
  if (nivel > s_nivel) { return true; }   // fonte melhor: na hora
  const uint32_t desde = tab5_agora_ms() - s_ultima_sync_ms;
  if (nivel == s_nivel) { return desde >= RESSINCRONIZAR_MS; }
  return desde >= VALIDADE_MS;            // a melhor sumiu faz tempo
}

// Quanto e para que lado o relogio estava errado, em palavras. O log dizia
// "relogio mudou +1s", e nao dava para saber se o +1 era o relogio andando
// para frente, a correcao ou a diferenca de fuso.
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

// Avisa os assinantes quando o relogio saltou. O relogio velho so e comparavel
// se era uma hora de verdade: saindo de 1970 (RTC nunca acertado) nada foi
// carimbado - carimbo abaixo do minimo vira "sem data" em todo lugar.
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
  // Fonte com hora absurda (ex. um no Mesh com relogio errado) nao entra no
  // RTC nem no relogio do sistema.
  if (!tab5_rtc_plausivel((time_t)epoch)) {
    const uint32_t agora = tab5_agora_ms();
    if (s_aviso_fonte_ms[nivel] == 0 || agora - s_aviso_fonte_ms[nivel] >= AVISO_FONTE_MS) {
      s_aviso_fonte_ms[nivel] = agora ? agora : 1;
      ESP_LOGW(TAG, "hora %s ignorada: fora da faixa plausivel (%lu)", NOME_NIVEL[nivel], (unsigned long)epoch);
    }
    return false;
  }

  const int64_t antes = (int64_t)time(nullptr);
  if (!tab5_rtc_definir((time_t)epoch)) { return false; }
  const int64_t delta = (int64_t)epoch - antes;

  if (nivel != s_nivel || delta > SALTO_S || delta < -SALTO_S) {
    // HORA LOCAL no log, a mesma que a barra mostra. Em UTC ("18:19Z") ela
    // parecia errada para quem le, que esta 3 h atras.
    struct tm t = { };
    tab5_hora_local(epoch, &t);
    char ajuste[48];
    descrever_ajuste(antes, delta, ajuste, sizeof(ajuste));
    const int fm = ajustes_fuso_min();
    ESP_LOGI(TAG, "hora acertada %s: %02d/%02d/%04d %02d:%02d:%02d (UTC%c%d:%02d) - %s",
             PELO_NIVEL[nivel], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
             t.tm_hour, t.tm_min, t.tm_sec, fm < 0 ? '-' : '+', abs(fm) / 60, abs(fm) % 60, ajuste);
  }
  s_nivel = nivel;
  s_ultima_sync_ms = tab5_agora_ms();

  avisar_salto(antes, delta);
  return true;
}

// No boot: o relogio do sistema veio do RTC (M5.begin). Se o RTC nao tem hora
// valida, esse relogio e uma data falsa - zera para os logs sairem "sem-data"
// ate chegar uma fonte (internet/GPS/Mesh).
static void conferir_rtc_no_boot(void)
{
  const int flags = tab5_rtc_flags();
  ESP_LOGI(TAG, "flags do RTC: 0x%02x", flags < 0 ? 0 : flags);
  if (flags >= 0 && (flags & 0x80)) { ESP_LOGW(TAG, "bateria do RTC fraca"); }

  time_t rtc = 0;
  if (tab5_rtc_ler(&rtc)) { return; }

  ESP_LOGW(TAG, "RTC sem hora valida (flags 0x%02x) - aguardando internet/GPS/Mesh", flags < 0 ? 0 : flags);
  const struct timeval zero = { 0, 0 };
  settimeofday(&zero, nullptr);
}

// Entre acertos de fonte, o relogio do sistema atrasa no sono leve (RC de ~5%).
// O RTC tem cristal: se ele esta valido (tab5_rtc_ler true) e diverge, ele
// manda. Nao grava o RTC aqui - ele e a referencia. Um RTC invalido (VLF, ano
// absurdo) nunca corrige nada.
static void corrigir_deriva_pelo_rtc(void)
{
  time_t rtc = 0;
  if (!tab5_rtc_ler(&rtc)) { return; }

  const int64_t antes = (int64_t)time(nullptr);
  const int64_t delta = (int64_t)rtc - antes;
  if (delta > -DERIVA_MINIMA_S && delta < DERIVA_MINIMA_S) { return; }

  // +0,5 s: o RTC trunca para segundos inteiros, o meio segundo centra o erro.
  struct timeval tv = { };
  tv.tv_sec  = rtc;
  tv.tv_usec = 500000;
  settimeofday(&tv, nullptr);

  if (delta >= LOGAR_CORRECAO_S || delta <= -LOGAR_CORRECAO_S) {
    char ajuste[48];
    descrever_ajuste(antes, delta, ajuste, sizeof(ajuste));
    ESP_LOGI(TAG, "relogio corrigido pelo RTC: %s", ajuste);
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
  ESP_LOGW(TAG, "assinantes de salto esgotados");
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
  if (!tab5_rtc_plausivel((time_t)epoch_utc)) {   // fora da faixa: sem hora de verdade
    snprintf(out, tam, "%ssem-data%s", prefixo, sufixo);
    return;
  }
  struct tm lt;
  tab5_hora_local(epoch_utc, &lt);
  snprintf(out, tam, "%s%04d-%02d-%02d%s", prefixo, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, sufixo);
}
