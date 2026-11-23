#include "tab5_gnss.hpp"
#include "tab5_linhas.hpp"
#include "tab5_gps.hpp"
#include "tab5_sd.hpp"
#include "tab5_arquivos.hpp"   // listing a folder is the system's job

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "gnss";
#define GNSS_PASTA "Azimutal"

// ---------------------------------------------------------------------------
// WRITING
// ---------------------------------------------------------------------------
static const char* nome_constelacao(GnssType t)
{
  switch (t) {
    case GPS:     return "GPS";
    case GLONASS: return "GLONASS";
    case GALILEO: return "GALILEO";
    case BEIDOU:  return "BEIDOU";
    case QZSS:    return "QZSS";
    case SBAS:    return "SBAS";
    default:      return "?";
  }
}

uint32_t tab5_gnss_gravar(const SatelliteSnapshot_t* snap, uint32_t idade_max_ms)
{
  int com_posicao = 0;
  for (uint8_t i = 0; i < snap->count; ++i) {
    if (snap->satellites[i].tem_posicao) { com_posicao++; }
  }
  if (com_posicao == 0) { return 0; }          // nothing useful to log

  const nmea_tempo_t   tempo = tab5_gps_tempo();
  const nmea_posicao_t pos   = tab5_gps_posicao();

  // Without the GPS time there's no file to write to. It happens in the first
  // few seconds, until the first RMC arrives - and goes away by itself.
  if (!tempo.valida) {
    ESP_LOGW(TAG, "no GPS time yet: %d satellite(s) not logged", com_posicao);
    return 0;
  }

  // NO FIX, NO WRITING. Elevation and azimuth are RELATIVE TO THE OBSERVER: without
  // a fix the receiver still emits GSV, but with almanac geometry that does NOT
  // update - 17 dumps in a row came out byte-for-byte identical. The only
  // reliable signal is the RMC's own status.
  if (!tempo.fix) {
    ESP_LOGW(TAG, "NO FIX: %d satellite(s) not logged "
                  "(the receiver hears, but doesn't know where it is - the geometry would be false)",
             com_posicao);
    return 0;
  }

  char arquivo[40];
  snprintf(arquivo, sizeof(arquivo), "satelites-%04u-%02u-%02u.csv",
           (unsigned)tempo.ano, (unsigned)tempo.mes, (unsigned)tempo.dia);

  // Through the system writer (hal/tab5_sd.hpp): hands over the line and moves on - it
  // mounts the SD card by itself.
  char linha[96];
  if (pos.valida && pos.qualidade > 0) {
    snprintf(linha, sizeof(linha), "POS,%02u:%02u:%02u,%.6f,%.6f,%.1f,%.1f,%u,%u\n",
             (unsigned)tempo.hora, (unsigned)tempo.min, (unsigned)tempo.seg,
             pos.lat, pos.lon, (double)pos.altitude_m, (double)pos.hdop,
             (unsigned)pos.qualidade, (unsigned)pos.sats_usados);
    tab5_sd_anexar(GNSS_PASTA, arquivo, linha);
  }

  const uint32_t agora_ms = pdTICKS_TO_MS(xTaskGetTickCount());
  uint32_t gravadas = 0;
  int frescos = 0, velhos = 0;
  for (uint8_t i = 0; i < snap->count; ++i) {
    const SatelliteInfo_t* s = &snap->satellites[i];
    if (!s->tem_posicao) { continue; }
    // AN OLD POSITION IS NOT AN OBSERVATION (see pos_ms in nmea.h): if it hasn't been
    // re-observed since the last dump, it doesn't go in.
    if ((agora_ms - s->pos_ms) > idade_max_ms) { velhos++; continue; }
    frescos++;
    snprintf(linha, sizeof(linha), "SAT,%02u:%02u:%02u,%s,%u,%u,%u,%u\n",
             (unsigned)tempo.hora, (unsigned)tempo.min, (unsigned)tempo.seg,
             nome_constelacao(s->type), s->prn, s->elevation, s->azimuth, s->snr);
    if (tab5_sd_anexar(GNSS_PASTA, arquivo, linha)) { ++gravadas; }
  }
  ESP_LOGI(TAG, "%s: %d written, %d discarded for stale position",
           arquivo, frescos, velhos);
  return gravadas;
}

// ---------------------------------------------------------------------------
// THE DAYS
// ---------------------------------------------------------------------------
// Scans the app's folder for the day files. Sorts in descending order so the
// most recent day shows first - it is nearly always the one you want to see. The name
// has the date in ISO, so descending alphabetical order = reverse chronological.
static int dia_mais_novo_primeiro(const void* a, const void* b)
{
  return strcmp((const char*)b, (const char*)a);
}

typedef struct { char (*dias)[TAB5_GNSS_DIA_NOME_TAM]; int max, n; } varredura_t;

static bool visita_dia(const tab5_arq_item_t* it, void* ctx)
{
  varredura_t* v = (varredura_t*)ctx;
  if (!strncmp(it->nome, "satelites-", 10)) {
    snprintf(v->dias[v->n], TAB5_GNSS_DIA_NOME_TAM, "%s", it->nome);
    v->n++;
  }
  return v->n < v->max;
}

int tab5_gnss_listar_dias(char (*dias)[TAB5_GNSS_DIA_NOME_TAM], int max)
{
  varredura_t v = { dias, max, 0 };
  char pasta[96];
  if (tab5_sd_montado() && tab5_sd_pasta(GNSS_PASTA, pasta, sizeof(pasta))) {
    tab5_arq_percorrer(pasta, visita_dia, &v);
  }
  if (v.n > 1) { qsort(dias, v.n, TAB5_GNSS_DIA_NOME_TAM, dia_mais_novo_primeiro); }
  return v.n;
}

// ---------------------------------------------------------------------------
// THE DAY LOADED IN MEMORY
// ---------------------------------------------------------------------------
// Previously, CHANGING SATELLITE reread the whole file from the SD card and reparsed
// it line by line - thousands of them - just to draw a subset of what had already
// been read. Now the file is read ONCE per chosen day, into this
// vector.
//
// A full day of logging (one sample per minute, ~20 satellites) gives some 29
// thousand positions - 174KB, on a device with 29MB free. It is by far the cheapest
// place for this information to be.
#define MAX_PONTOS 40000

static tab5_gnss_ponto_t* s_pontos = nullptr;
static int  s_n_pontos = 0;
static tab5_gnss_sat_t s_sats[TAB5_GNSS_SATS_MAX];
static int  s_n_sats = 0;
static char s_dia_carregado[TAB5_GNSS_DIA_NOME_TAM] = "";

// Hand-parsing, instead of sscanf.
//
// The format is fixed: SAT,hh:mm:ss,CONS,prn,elev,azim,snr. sscanf has to
// reparse the format string on every line and "%11[^,]" is among its
// most expensive parts - with thousands of lines that dominates the load time.
// strtoul walks the pointer and returns where it stopped, which is all we need.
//
// Returns false at the first thing out of place: a truncated or corrupted line
// is discarded whole, without inventing a value.
static bool interpretar_sat(char* l, unsigned* hh, unsigned* mm,
                            char** cons, unsigned* prn,
                            unsigned* elev, unsigned* azim, unsigned* snr)
{
  char* p = l + 4;                                  // after "SAT,"
  *hh = strtoul(p, &p, 10); if (*p != ':') { return false; } ++p;
  *mm = strtoul(p, &p, 10); if (*p != ':') { return false; } ++p;
  strtoul(p, &p, 10);                               // seconds: not used
  if (*p != ',') { return false; } ++p;

  *cons = p;
  while (*p && *p != ',') { ++p; }
  if (!*p) { return false; }
  *p++ = 0;                                         // terminates the constellation

  *prn  = strtoul(p, &p, 10); if (*p != ',') { return false; } ++p;
  *elev = strtoul(p, &p, 10); if (*p != ',') { return false; } ++p;
  *azim = strtoul(p, &p, 10);
  *snr = (*p == ',') ? strtoul(p + 1, &p, 10) : 0;   // old files may not have it
  return true;
}

bool tab5_gnss_carregar_dia(const char* arquivo)
{
  if (strcmp(s_dia_carregado, arquivo) == 0 && s_pontos) { return true; }

  if (!s_pontos) {
    s_pontos = (tab5_gnss_ponto_t*)heap_caps_malloc(sizeof(tab5_gnss_ponto_t) * MAX_PONTOS,
                                                    MALLOC_CAP_SPIRAM);
    if (!s_pontos) {
      ESP_LOGE(TAG, "no PSRAM for the history");
      return false;
    }
  }

  s_n_pontos = 0;
  s_n_sats   = 0;
  s_dia_carregado[0] = 0;

  char pasta[96], caminho[160];
  if (!tab5_sd_pasta_caminho(GNSS_PASTA, pasta, sizeof(pasta))) { return false; }
  snprintf(caminho, sizeof(caminho), "%s/%s", pasta, arquivo);
  tab5_linhas_t* l = tab5_linhas_abrir(caminho);   // mounts the SD card and writes what is pending first
  if (!l) { return false; }

  const uint32_t t0 = pdTICKS_TO_MS(xTaskGetTickCount());
  char linha[96];
  int voltas = 0;
  while (s_n_pontos < MAX_PONTOS && tab5_linhas_proxima(l, linha, sizeof(linha))) {
    // Yields the CPU periodically: this may run in the UI task and the
    // file goes beyond megabytes with hours of logging. Holding the core for more
    // than 5s would make the task watchdog restart the board.
    if ((++voltas % 256) == 0) { vTaskDelay(1); }
    if (strncmp(linha, "SAT,", 4) != 0) { continue; }

    unsigned hh, mm, prn, elev, azim, snr;
    char* cons;
    if (!interpretar_sat(linha, &hh, &mm, &cons, &prn, &elev, &azim, &snr)) { continue; }
    if (elev > 90 || azim > 359) { continue; }

    // Catalogues who appears. The search is linear, but over AT MOST 40 names -
    // a hash table here would be more code than gain.
    int idx = -1;
    for (int k = 0; k < s_n_sats; ++k) {
      if (s_sats[k].prn == (int)prn && strcmp(s_sats[k].cons, cons) == 0) { idx = k; break; }
    }
    if (idx < 0) {
      if (s_n_sats >= TAB5_GNSS_SATS_MAX) { continue; }
      idx = s_n_sats++;
      snprintf(s_sats[idx].cons, sizeof(s_sats[idx].cons), "%s", cons);
      s_sats[idx].prn = (int)prn;
    }

    tab5_gnss_ponto_t* pt = &s_pontos[s_n_pontos++];
    pt->sat    = (uint8_t)idx;
    pt->elev   = (uint8_t)elev;
    pt->azim   = (uint16_t)azim;
    pt->minuto = (uint16_t)(hh * 60 + mm);
    pt->snr    = (uint8_t)(snr > 99 ? 99 : snr);
  }
  tab5_linhas_fechar(l);

  snprintf(s_dia_carregado, sizeof(s_dia_carregado), "%s", arquivo);
  ESP_LOGI(TAG, "%s: %d points from %d satellites in %lu ms",
           arquivo, s_n_pontos, s_n_sats,
           (unsigned long)(pdTICKS_TO_MS(xTaskGetTickCount()) - t0));
  return true;
}

const tab5_gnss_ponto_t* tab5_gnss_pontos(int* n) { *n = s_n_pontos; return s_pontos; }
const tab5_gnss_sat_t*   tab5_gnss_sats(int* n)   { *n = s_n_sats;   return s_sats; }

void tab5_gnss_descarregar(void)
{
  if (s_pontos) { heap_caps_free(s_pontos); s_pontos = nullptr; }
  s_n_pontos = 0;
  s_n_sats   = 0;
  s_dia_carregado[0] = 0;
}
