#include "tab5_ensaio.hpp"
#include "tab5_hora.hpp"   // tab5_passou_s
#include "tab5_power.hpp"
#include "tab5_wifi.hpp"
#include "tab5_mesh.hpp"
#include "tab5_sd.hpp"
#include "ajustes.h"
#include "app.h"
#include "trabalho.hpp"

#include <atomic>
#include <stdio.h>
#include "esp_log.h"

static const char* TAG = "ensaio";

#define N_FASES 5
static const char* const FASE_NOME[N_FASES] = {
  "baseline (as is)", "no Mesh internet", "no Grove 5 V", "no Bluetooth", "no Wi-Fi",
};
static const int ACOMODAR_S = 120;

typedef enum { E_PARADO, E_ESPERANDO, E_FASE, E_FIM } etapa_t;

static std::atomic<int>  s_pedido_min{0};      // console -> UI: start
static std::atomic<bool> s_pedido_parar{false};
static volatile etapa_t  s_etapa = E_PARADO;
static std::atomic<int>      s_fase = 0;
static int               s_min = 6;
static int               s_fases = N_FASES;
static std::atomic<int>  s_pedido_fases{N_FASES};
static int               s_seg_fase = 0;       // valid seconds (idle + battery) in this stage
static double            s_soma_ma[N_FASES];
static int               s_n[N_FASES];
static bool              s_anotada[N_FASES];   // the stage has already gone to the CSV (anotar_fase)

// THE ORIGINALS ONLY LEAVE NVS AFTER THE RESTORED SETTINGS HAVE BEEN WRITTEN.
// Settings go to NVS via the deferred path (up to 1 s after the last
// change); deleting the originals right after asking for the restore left a window in
// which a crash lost both (2026-11-13: panic when switching Wi-Fi back on, and the
// following boot came up with Wi-Fi, Grove and Mesh internet off). Wait
// ORIGINAIS_APAGAR_S and only then delete.
static const int ORIGINAIS_APAGAR_S = 5;
static int64_t   s_apagar_originais_us = 0;    // 0 = nothing pending

// The originals, for restoring.
static bool    o_wifi, o_ble, o_grove, o_mesh_net;
static uint8_t o_mesh_via;

#include "esp_timer.h"

static void mesh_descer(void*) { if (tab5_mesh_servico_ativo()) { tab5_mesh_parar(); } }

// Switches off the service for stage 'f' (the earlier ones are already off).
static void aplicar_fase(int f)
{
  switch (f) {
    case 1:
      ajustes_definir_mesh_internet(false);
      if (ajustes_mesh_via() == AJUSTES_MESH_INTERNET) { ajustes_definir_mesh_via(AJUSTES_MESH_SERIAL); }
      sistema_trabalho_solto(mesh_descer, nullptr);
      break;
    case 2: ajustes_definir_grove(false); tab5_power_grove(false); break;
    case 3: sistema_bluetooth(false); break;
    case 4: sistema_wifi(false); break;
    default: break;
  }
  ESP_LOGW(TAG, "stage %d: %s", f, FASE_NOME[f]);
}

static void devolver(void)
{
  ajustes_definir_mesh_via(o_mesh_via);
  ajustes_definir_mesh_internet(o_mesh_net);
  if (o_wifi) { sistema_wifi(true); }
  if (o_ble != ajustes_bluetooth()) { sistema_bluetooth(o_ble); }
  if (o_grove && !ajustes_grove()) {
    ajustes_definir_grove(true);
    tab5_power_grove(true);
    sistema_grove_redetectar();
  }
  ajustes_gravar_agora();   // brings forward the deferred write of the restored settings
  s_apagar_originais_us = esp_timer_get_time() + (int64_t)ORIGINAIS_APAGAR_S * 1000000;
  ESP_LOGW(TAG, "settings restored");
}

// A stage goes to the CSV as soon as it finishes: a crash in the middle of the test
// doesn't take with it the stages already measured (previously only the end wrote the table).
static void anotar_fase(int f)
{
  if (s_n[f] == 0 || s_anotada[f]) { return; }
  s_anotada[f] = true;
  char linha[96];
  const double m = s_soma_ma[f] / s_n[f];
  snprintf(linha, sizeof(linha), "%d,%s,%.1f,%d\n", f, FASE_NOME[f], m, s_n[f]);
  tab5_sd_anexar("Sistema", "ensaio_energia.csv", linha, "fase,nome,mA_medio,segundos\n");
  ESP_LOGW(TAG, "result stage %d (%s): %.1f mA over %d s", f, FASE_NOME[f], m, s_n[f]);
}

// At the end (or on 'parar'): the stages not yet noted down.
static void gravar_tabela(void)
{
  for (int f = 0; f < N_FASES; ++f) { anotar_fase(f); }
}

bool tab5_ensaio_comecar(int min_por_fase, int fases)
{
  s_pedido_fases = (fases < 1 || fases > N_FASES) ? N_FASES : fases;
  if (s_etapa != E_PARADO && s_etapa != E_FIM) { return false; }
  if (min_por_fase < 2)  { min_por_fase = 2; }
  if (min_por_fase > 30) { min_por_fase = 30; }
  s_pedido_min = min_por_fase;
  return true;
}

void tab5_ensaio_parar(void) { s_pedido_parar = true; }

void tab5_ensaio_vigiar(void)
{
  // AN INTERRUPTED TEST (restart, battery running out): the originals were left
  // stored - restore them on the first UI round after boot.
  // The originals pending deletion (see ORIGINAIS_APAGAR_S).
  if (s_apagar_originais_us && esp_timer_get_time() >= s_apagar_originais_us) {
    s_apagar_originais_us = 0;
    ajustes_definir_ensaio_originais(nullptr);
  }
  static bool conferido = false;
  if (!conferido) {
    conferido = true;
    uint8_t o[AJUSTES_ENSAIO_N];
    if (ajustes_ensaio_originais(o)) {
      o_wifi = o[0]; o_ble = o[1]; o_grove = o[2]; o_mesh_net = o[3]; o_mesh_via = o[4];
      ESP_LOGW(TAG, "test interrupted before the end: restoring the settings");
      devolver();
    }
  }
  const int pedido = s_pedido_min.exchange(0);
  if (pedido > 0) {
    s_min = pedido;
    s_fases = s_pedido_fases;
    o_wifi = ajustes_wifi(); o_ble = ajustes_bluetooth(); o_grove = ajustes_grove();
    o_mesh_net = ajustes_mesh_internet(); o_mesh_via = ajustes_mesh_via();
    const uint8_t o[AJUSTES_ENSAIO_N] = { o_wifi, o_ble, o_grove, o_mesh_net, o_mesh_via };
    ajustes_definir_ensaio_originais(o);
    ESP_LOGW(TAG, "originals: wifi=%d ble=%d grove=%d mesh_internet=%d mesh_via=%d",
             o_wifi, o_ble, o_grove, o_mesh_net, o_mesh_via);
    s_apagar_originais_us = 0;   // the new originals have just been written
    for (int f = 0; f < N_FASES; ++f) { s_soma_ma[f] = 0; s_n[f] = 0; s_anotada[f] = false; }
    s_fase = 0; s_seg_fase = 0;
    s_etapa = E_ESPERANDO;
  }
  if (s_etapa == E_PARADO || s_etapa == E_FIM) { s_pedido_parar = false; return; }

  if (s_pedido_parar.exchange(false)) {
    devolver(); gravar_tabela(); s_etapa = E_FIM;
    return;
  }

  static int64_t ult = 0;
  const int dt = tab5_passou_s(&ult);
  const tab5_medida_t m = tab5_power_ultima();
  const bool vale = m.volts > 1.0f && m.mA < -5.0f && tab5_power_em_repouso();

  if (s_etapa == E_ESPERANDO) {
    // Starts stage 0 only with the cable out and the screen off.
    if (vale) { s_etapa = E_FASE; s_seg_fase = 0; ESP_LOGW(TAG, "on battery and idle: stage 0"); }
    return;
  }

  if (!vale) { return; }   // woke up or the cable was plugged in: pause
  s_seg_fase += dt;
  if (s_seg_fase > ACOMODAR_S) {     // time-weighted average: when idle a sample comes every 5 s
    s_soma_ma[s_fase] += -m.mA * dt;
    s_n[s_fase] += dt;
  }
  if (s_seg_fase >= ACOMODAR_S + s_min * 60) {
    if (s_fase + 1 >= s_fases) {
      devolver(); gravar_tabela(); s_etapa = E_FIM;
      return;
    }
    anotar_fase(s_fase);
    ++s_fase;
    s_seg_fase = 0;
    aplicar_fase(s_fase);
  }
}

void tab5_ensaio_imprimir(void)
{
  switch (s_etapa) {
    case E_PARADO:    printf("trial: stopped\n"); break;
    case E_ESPERANDO: printf("trial: waiting - unplug the cable and let the screen go dark\n"); break;
    case E_FASE:
      printf("trial: phase %d/%d (%s), %d s of %d valid\n", s_fase.load(), N_FASES - 1,
             FASE_NOME[s_fase], s_seg_fase, ACOMODAR_S + s_min * 60);
      break;
    case E_FIM:       printf("ensaio: terminou - ajustes devolvidos\n"); break;
  }
  double ant = -1;
  for (int f = 0; f < N_FASES; ++f) {
    if (s_n[f] == 0) { continue; }
    const double v = s_soma_ma[f] / s_n[f];
    if (ant < 0) { printf("  %-20s %6.1f mA  (%d s)\n", FASE_NOME[f], v, s_n[f]); }
    else         { printf("  %-20s %6.1f mA  (%d s)   service cost: %5.1f mA\n", FASE_NOME[f], v, s_n[f], ant - v); }
    ant = v;
  }
}
