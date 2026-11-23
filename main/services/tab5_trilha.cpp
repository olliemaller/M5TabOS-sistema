#include "tab5_trilha.hpp"
#include "tab5_linhas.hpp"
#include "tab5_gps.hpp"
#include "tab5_sd.hpp"
#include "tab5_arquivos.hpp"   // listing a folder is the system's job
#include "tab5_hora.hpp"
#include "tab5_mutex.hpp"
#include "notificacao.h"
#include "ajustes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static const char* TAG = "trilha";
#define TRILHA_PASTA "Trilha"
#define AMOSTRA_MS   5000   // one point every 5 s - a track needs no more than that

static uint32_t       s_pontos  = 0;   // a lone uint32_t: word read/write is atomic on this CPU
// The name changes at two moments: tab5_trilha_iniciar (before the task starts) and the
// day rollover (in the task).
static char           s_nome[32] = "";
static uint32_t       s_numero   = 0;     // the N of the trilha_N.csv being written (0 = stopped)
static uint32_t       s_continuar = 0;    // request for the next start (tab5_trilha_continuar)
static bool           s_carregar = false; // the loop starts by reading the existing file
static uint32_t       s_dia = 0;          // LOCAL AAAAMMDD of the track being written (0 = not known yet)

// ---------------------------------------------------------------------------
// THE DECIMATION - a single one, used by the live map and by reading the history.
// ---------------------------------------------------------------------------
void tab5_trilha_decimar_iniciar(tab5_trilha_decimador_t* d, tab5_trilha_ponto_t* pts, uint32_t max)
{
  d->pts   = pts;
  d->max   = max;
  d->n     = 0;
  d->passo = 1;
  d->idx   = 0;
}

void tab5_trilha_decimar_somar(tab5_trilha_decimador_t* d, float lat, float lon)
{
  if ((d->idx++ % d->passo) != 0) { return; }
  if (d->n >= d->max) {
    for (uint32_t k = 0; k < d->max / 2; ++k) { d->pts[k] = d->pts[k * 2]; }
    d->n      = d->max / 2;
    d->passo *= 2;
    // The step doubled - the current sample may no longer fit the new cadence.
    if ((d->idx % d->passo) != 0) { return; }
  }
  d->pts[d->n].lat = lat;
  d->pts[d->n].lon = lon;
  ++d->n;
}

// ---------------------------------------------------------------------------
// THE LIVE MAP
// ---------------------------------------------------------------------------
// Written by the "trilha" task (loop), read by the app's task (from inside
// desenhar()) - DIFFERENT tasks, hence the mutex; not the same case as
// s_pontos/s_nome above (those are only published AFTER they are stable, and a
// lone word does not tear on read).
static tab5_trilha_ponto_t     s_mapa[TAB5_TRILHA_MAPA_MAX];
static tab5_trilha_decimador_t s_mapa_dec;       // what decimates s_mapa (n = valid points)
static bool                    s_mapa_pronto = false;   // s_mapa_dec has been initialised
static uint32_t                s_mapa_dia   = 0;   // AAAAMMDD of the current trace; 0 = none yet
static Trava s_mapa_mutex;

static inline void travar_mapa(void)    { s_mapa_mutex.travar(); }
static inline void destravar_mapa(void) { s_mapa_mutex.destravar(); }

// Restarts the trace from scratch. Call with the map locked.
static void mapa_zerar(void)
{
  tab5_trilha_decimar_iniciar(&s_mapa_dec, s_mapa, TAB5_TRILHA_MAPA_MAX);
  s_mapa_pronto = true;
}

// ONE TRACE PER DAY (piling everything onto the same image would make it illegible).
// A recording can stay on for days (and the app only stops on the button, never
// by itself); without this, a Monday walk would be joined to Tuesday's
// on the SAME image - and, once decimated enough, would become a shapeless
// smudge, representing no real walk at all.
// 'dia_chave' is AAAAMMDD on the LOCAL day (tab5_trilha_dia_local) - compared
// against the CURRENT trace's day; if it rolled over, resets as if it were a new start,
// but WITHOUT touching the file/point count (recording to the card
// carries on, only the drawing restarts from scratch).
static void mapa_virar_dia_se_precisar(uint32_t dia_chave)
{
  if (dia_chave == s_mapa_dia) { return; }
  if (s_mapa_dia != 0) {   // 0 = first point of this session, not a real "rollover"
    ESP_LOGI(TAG, "map: the day rolled over (%lu -> %lu) - trace cleared",
             (unsigned long)s_mapa_dia, (unsigned long)dia_chave);
  }
  mapa_zerar();
  s_mapa_dia = dia_chave;
}

static void mapa_adicionar(float lat, float lon, uint32_t dia_chave)
{
  travar_mapa();
  mapa_virar_dia_se_precisar(dia_chave);
  tab5_trilha_decimar_somar(&s_mapa_dec, lat, lon);
  destravar_mapa();
}

uint32_t tab5_trilha_mapa(tab5_trilha_ponto_t* out, uint32_t max)
{
  travar_mapa();
  const uint32_t total = s_mapa_pronto ? s_mapa_dec.n : 0;
  const uint32_t n = (total < max) ? total : max;
  for (uint32_t i = 0; i < n; ++i) { out[i] = s_mapa[i]; }
  destravar_mapa();
  return n;
}

// ---------------------------------------------------------------------------
// THE SD CARD
// ---------------------------------------------------------------------------
// Absolute path of "trilha_NNNN.csv" (for the reader). false = no folder.
static bool caminho_da_trilha(uint32_t numero, char* out, size_t tam)
{
  char pasta[96];
  if (!tab5_sd_pasta_caminho(TRILHA_PASTA, pasta, sizeof(pasta))) { return false; }
  snprintf(out, tam, "%s/trilha_%04lu.csv", pasta, (unsigned long)numero);
  return true;
}

// "AAAA-MM-DDTHH:MM:SSZ,lat,lon,altitude_m,qualidade" - only the date and lat/lon
// matter for the trace. The first field (date) has no comma
// inside, so the FIRST strchr already points past it. false = the
// header, or a truncated line.
static bool ler_ponto(const char* linha, uint32_t* ep, float* lat, float* lon)
{
  if (!tab5_trilha_ler_carimbo(linha, ep)) { return false; }
  const char* apos_data = strchr(linha, ',');
  return apos_data && sscanf(apos_data + 1, "%f,%f", lat, lon) == 2;
}

// HIGHEST "trilha_NNNN.csv" ALREADY ON THE CARD - each recording is a new file, and the
// number has to come from what already exists on the card, not from a variable that
// starts at zero on every boot. MOUNTS the card: without that, tab5_sd_pasta() saw
// the card as absent, returned false, and every START began from zero at
// "trilha_0001.csv" - overwriting the previous session.
static uint32_t maior_trilha_existente(void)
{
  uint32_t maior = 0;
  if (!tab5_sd_montar()) { return 0; }
  char pasta[128];
  if (tab5_sd_pasta(TRILHA_PASTA, pasta, sizeof(pasta))) {
    maior = tab5_arq_maior_numero(pasta, "trilha_%04lu.csv");
  }
  tab5_sd_desmontar();
  return maior;
}

// ---------------------------------------------------------------------------
// THE RECORDING (the background part)
// ---------------------------------------------------------------------------
// CONTINUE: the trace and the count come back from the file itself - read here, on the
// background task (a whole day is ~17 thousand lines: seconds of card access,
// which in the interface would be seconds of frozen screen).
static void carregar_existente(fundo_t* f)
{
  char caminho[160];
  if (!caminho_da_trilha(s_numero, caminho, sizeof(caminho))) { return; }
  tab5_linhas_t* l = tab5_linhas_abrir(caminho);   // writes pending data before reading
  if (!l) { return; }
  char linha[112];
  uint32_t n = 0, voltas = 0;
  while (tab5_linhas_proxima(l, linha, sizeof(linha))) {
    if ((++voltas % 64) == 0 && !fundo_ceder(f)) { break; }
    uint32_t ep;
    float lat, lon;
    if (!ler_ponto(linha, &ep, &lat, &lon)) { continue; }
    const uint32_t dia = tab5_trilha_dia_local(ep);
    mapa_adicionar(lat, lon, dia);
    s_dia = dia;
    ++n;
  }
  tab5_linhas_fechar(l);
  s_pontos = n;
  ESP_LOGI(TAG, "continuing %s: %lu point(s) already in the file", s_nome, (unsigned long)n);
}

// MIDNIGHT: the day's track closes (becomes history) and the next day's
// opens - the number after the highest that exists, as on a START.
static void virar_trilha(uint32_t dia_novo)
{
  uint32_t proximo = s_numero + 1;
  const uint32_t m = maior_trilha_existente() + 1;
  if (m > proximo) { proximo = m; }
  ESP_LOGI(TAG, "the day rolled over: %s goes to history (%lu point(s)), continuing in trilha_%04lu.csv",
           s_nome, (unsigned long)s_pontos, (unsigned long)proximo);
  travar_mapa();
  s_numero = proximo;
  snprintf(s_nome, sizeof(s_nome), "trilha_%04lu.csv", (unsigned long)proximo);
  destravar_mapa();
  s_pontos = 0;
  s_dia = dia_novo;
}

// THE LOOP - runs on the task the kernel creates (kernel/fundo.h). Sleeps
// AMOSTRA_MS, checks the GPS, writes if it has a fix. fundo_esperar() returns IMMEDIATELY
// when asked to stop.
static void laco_trilha(fundo_t* f)
{
  if (s_carregar) {
    s_carregar = false;
    carregar_existente(f);
    char resumo[NOTIF_RESUMO];
    snprintf(resumo, sizeof(resumo), "%lu ponto(s)", (unsigned long)s_pontos);
    fundo_estado(f, (int)s_pontos, resumo);
  }
  while (fundo_esperar(f, AMOSTRA_MS)) {
    const nmea_posicao_t pos  = tab5_gps_posicao();
    const nmea_tempo_t   hora = tab5_gps_tempo();

    // NO FIX, NO POINT - not worth writing zeroed lat/lon just to
    // say "I tried". The next round tries again by itself.
    if (!pos.valida || pos.qualidade == 0 || !hora.valida) { continue; }

    // THE LOCAL DAY OF THIS POINT. Changed (and the track's day was known): close
    // this one and open the new day's BEFORE writing the point.
    char carimbo[24];
    snprintf(carimbo, sizeof(carimbo), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned)hora.ano, (unsigned)hora.mes, (unsigned)hora.dia,
             (unsigned)hora.hora, (unsigned)hora.min, (unsigned)hora.seg);
    uint32_t ep = 0;
    const uint32_t dia_chave = tab5_trilha_ler_carimbo(carimbo, &ep) ? tab5_trilha_dia_local(ep) : 0;
    if (dia_chave && s_dia && dia_chave != s_dia) { virar_trilha(dia_chave); }
    if (dia_chave) { s_dia = dia_chave; }

    // Through the system writer (hal/tab5_sd.hpp): hands over the line and carries on -
    // the loop no longer mounts the card on every point. The header goes along and is only
    // written if the session's file is empty.
    char linha[112];
    snprintf(linha, sizeof(linha), "%s,%.6f,%.6f,%.1f,%u\n",
             carimbo, pos.lat, pos.lon, pos.altitude_m, (unsigned)pos.qualidade);
    if (tab5_sd_anexar(TRILHA_PASTA, s_nome, linha, "data_hora_utc,lat,lon,altitude_m,qualidade\n")) {
      mapa_adicionar(pos.lat, pos.lon, dia_chave);
      ++s_pontos;   // lone word, no lock - see the notice at the top of the file
      char resumo[NOTIF_RESUMO];
      snprintf(resumo, sizeof(resumo), "%lu ponto(s)", (unsigned long)s_pontos);
      fundo_estado(f, (int)s_pontos, resumo);
    }
  }
}

// Prepares the session, on the interface, before the kernel starts the loop.
static bool ao_iniciar(void)
{
  uint32_t proximo = s_continuar;
  s_carregar = (s_continuar != 0);
  s_continuar = 0;   // the request applies to ONE start
  if (!proximo) { proximo = maior_trilha_existente() + 1; }
  travar_mapa();
  s_numero = proximo;
  snprintf(s_nome, sizeof(s_nome), "trilha_%04lu.csv", (unsigned long)proximo);
  destravar_mapa();
  s_pontos = 0;
  s_dia = 0;

  // A NEW map per session (does not pile different walks together) - but only
  // cleared here, never in ao_parar(): that way you can look at the complete trace
  // after pressing STOP, until the next time START is pressed.
  travar_mapa();
  mapa_zerar();
  s_mapa_dia = 0;   // 0 = "no trace yet" - the 1st point sets the day, without logging a "rollover"
  destravar_mapa();

  ESP_LOGI(TAG, "%s in %s/%s", s_carregar ? "continuing" : "recording", TRILHA_PASTA, s_nome);
  return true;
}

// After the loop exits: the file is already closed (each point opens and closes it).
static void ao_parar(void)
{
  ESP_LOGI(TAG, "stopped - %lu point(s) in %s", (unsigned long)s_pontos, s_nome);
  travar_mapa();
  s_nome[0] = '\0';
  s_numero = 0;
  destravar_mapa();
}

// THE DESCRIPTION THE KERNEL RUNS. Profile deliberately empty: the GPS is a service
// (the Grove power is decided by the kernel), and the card is lent on each
// point. Priority, pill, stopping and clean-up belong to the system.
const fundo_def_t TRILHA_FUNDO = {
  .nome       = "Trilha",
  .corpo      = laco_trilha,
  .pilha      = 4096,
  .perfil     = { },
  .ao_iniciar = ao_iniciar,
  .ao_parar   = ao_parar,
  .com_som    = ajustes_beep_trilha,
};

uint32_t tab5_trilha_pontos(void) { return s_pontos; }

void     tab5_trilha_continuar(uint32_t numero) { s_continuar = numero; }
uint32_t tab5_trilha_numero(void) { return sistema_fundo_ativo(&TRILHA_FUNDO) ? s_numero : 0; }

// ---------------------------------------------------------------------------
// TIMESTAMP AND DAY
// ---------------------------------------------------------------------------
// Days since 1970-01-01 of a civil date (Howard Hinnant) - newlib has no
// timegm(), and mktime() would use the system time zone.
static int32_t dias_de_civil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

bool tab5_trilha_ler_carimbo(const char* linha, uint32_t* epoch_utc)
{
  unsigned a, me, d, h, mi, se;
  if (!linha || sscanf(linha, "%4u-%2u-%2uT%2u:%2u:%2u", &a, &me, &d, &h, &mi, &se) != 6) { return false; }
  if (a < 2000 || me < 1 || me > 12 || d < 1 || d > 31) { return false; }
  *epoch_utc = (uint32_t)dias_de_civil((int)a, me, d) * 86400u + h * 3600u + mi * 60u + se;
  return true;
}

uint32_t tab5_trilha_dia_local(uint32_t epoch_utc)
{
  if (epoch_utc < 1577836800u) { return 0; }   // before 2020: clock not set
  struct tm t;
  tab5_hora_local(epoch_utc, &t);
  return (uint32_t)(t.tm_year + 1900) * 10000 + (uint32_t)(t.tm_mon + 1) * 100 + (uint32_t)t.tm_mday;
}

void tab5_trilha_definir_beep(bool tocar)
{
  ajustes_definir_beep_trilha(tocar);
  // The notification source is the kernel's, by name (idempotent registration).
  notif_silenciar(notif_registrar(TRILHA_FUNDO.nome), !tocar);
}

bool tab5_trilha_beep_ligado(void) { return ajustes_beep_trilha(); }

// ---------------------------------------------------------------------------
// THE HISTORY
// ---------------------------------------------------------------------------
// "06/10 08:15" of the first point of track N (local time); "" if not possible.
static void quando_comecou(uint32_t numero, char* out, size_t n)
{
  out[0] = 0;
  char caminho[160];
  if (!caminho_da_trilha(numero, caminho, sizeof(caminho))) { return; }
  tab5_linhas_t* l = tab5_linhas_abrir(caminho);
  if (!l) { return; }
  char linha[96];
  uint32_t ep;
  for (int k = 0; k < 3 && tab5_linhas_proxima(l, linha, sizeof(linha)); ++k) {   // skips the header
    if (tab5_trilha_ler_carimbo(linha, &ep) && tab5_trilha_dia_local(ep)) {
      struct tm t;
      tab5_hora_local(ep, &t);
      snprintf(out, n, "%02d/%02d %02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
      break;
    }
  }
  tab5_linhas_fechar(l);
}

// What the folder scan needs to carry in and out.
typedef struct {
  uint32_t* numeros;
  int       max, n;
  uint32_t  exceto_a, exceto_b;
} varredura_t;

static bool visita_trilha(const tab5_arq_item_t* it, void* ctx)
{
  varredura_t* v = (varredura_t*)ctx;
  unsigned long num;
  if (sscanf(it->nome, "trilha_%04lu.csv", &num) == 1
   && (uint32_t)num != v->exceto_a && (uint32_t)num != v->exceto_b) {
    v->numeros[v->n++] = (uint32_t)num;
  }
  return v->n < v->max;
}

static int maior_primeiro(const void* a, const void* b)
{
  const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x < y) ? 1 : (x > y) ? -1 : 0;
}

int tab5_trilha_listar(uint32_t* numeros, char (*quando)[TAB5_TRILHA_QUANDO_TAM], int max,
                       int com_data, uint32_t exceto_a, uint32_t exceto_b,
                       tab5_trilha_cancelado_t cancelado)
{
  varredura_t v = { numeros, max, 0, exceto_a, exceto_b };
  char pasta[128];
  // Hold the card for the whole list: quando_comecou opens one file per
  // track, and really mounting for each one would be slow.
  const bool montou = tab5_sd_montar();
  if (montou && tab5_sd_pasta(TRILHA_PASTA, pasta, sizeof(pasta))) {
    tab5_arq_percorrer(pasta, visita_trilha, &v);
  }
  int n = v.n;

  // HIGHEST FIRST: the most recent session at the top.
  if (n > 1) { qsort(numeros, n, sizeof(uint32_t), maior_primeiro); }
  for (int i = 0; i < n; ++i) {
    if (quando) {
      if (i < com_data && montou) { quando_comecou(numeros[i], quando[i], TAB5_TRILHA_QUANDO_TAM); }
      else                        { quando[i][0] = 0; }
    }
    if ((i % 16) == 15 && cancelado && cancelado()) { n = i + 1; break; }
  }
  if (montou) { tab5_sd_desmontar(); }
  return n;
}

void tab5_trilha_ler(uint32_t numero, tab5_trilha_leitura_t* r, tab5_trilha_cancelado_t cancelado)
{
  r->n = r->total = r->primeiro = r->ultimo = 0;
  char caminho[160];
  if (!caminho_da_trilha(numero, caminho, sizeof(caminho))) { return; }
  tab5_linhas_t* l = tab5_linhas_abrir(caminho);
  if (!l) { return; }

  tab5_trilha_decimador_t dec;
  tab5_trilha_decimar_iniciar(&dec, r->pts, r->max);
  char linha[96];
  uint32_t voltas = 0;
  while (tab5_linhas_proxima(l, linha, sizeof(linha))) {
    if ((++voltas % 64) == 0) {
      vTaskDelay(1);                                   // do not hog the core on a big file
      if (cancelado && cancelado()) { break; }         // the app closed: the result is no longer needed
    }
    uint32_t ep;
    float lat, lon;
    if (!ler_ponto(linha, &ep, &lat, &lon)) { continue; }   // the header, or a truncated line

    if (!r->primeiro) { r->primeiro = ep; }
    r->ultimo = ep;
    ++r->total;
    tab5_trilha_decimar_somar(&dec, lat, lon);
  }
  tab5_linhas_fechar(l);
  r->n = dec.n;
}

uint32_t tab5_trilha_de_hoje(tab5_trilha_leitura_t* r, tab5_trilha_cancelado_t cancelado)
{
  const uint32_t maior = maior_trilha_existente();
  const uint32_t hoje = tab5_trilha_dia_local((uint32_t)time(nullptr));
  if (!maior || !hoje) { return 0; }
  tab5_trilha_ler(maior, r, cancelado);
  return (r->total && tab5_trilha_dia_local(r->ultimo) == hoje) ? maior : 0;
}
