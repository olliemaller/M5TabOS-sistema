#include "tab5_wifi_cochilo.hpp"
#include "tab5_hora.hpp"   // tab5_passou_s
#include "tab5_wifi.hpp"
#include "tab5_power.hpp"
#include "ajustes.h"
#include "fundo.h"
#include "app.h"
#include "tab5_mesh.hpp"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "cochilo";

static const int COCHILO_APOS_S = 3 * 60;    // screen off this long before turning off
static const int JANELA_CADA_S  = 15 * 60;   // how often it comes back on by itself
static const int JANELA_S       = 60;        // connected this long in each window
static const int JANELA_MAX_S   = 120;       // not connected by now: give up on the window

typedef enum { C_NORMAL, C_COCHILANDO, C_JANELA } cochilo_t;
static cochilo_t s_estado = C_NORMAL;
static int       s_seg = 0;          // seconds in the current state (or idle, in NORMAL)
static int       s_conectado_s = 0;  // seconds connected within the window
static uint32_t  s_cochilos = 0;     // how many times it turned off (diagnostics)

// MESH OVER THE INTERNET AND RECEIVING: MQTT keeps nothing for whoever is away -
// whatever arrives while Wi-Fi is off is lost (it happened: 2026-10-17, a night
// of lost messages). With the service live over the internet, the network stays up.
static bool mesh_pela_internet(void)
{
  return tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_INTERNET;
}

// WI-FI HEARTBEAT. The C6's SDIO does not bring the P4 out of light sleep: with Wi-Fi
// connected and the screen off, instead of blocking sleep (72 mA measured), the P4
// sleeps and an esp_timer wakes it every BATIDA_MS to handle what the C6
// has gathered. The router and TCP tolerate that delay; nothing is lost.
static const uint64_t BATIDA_MS       = 100;   // screen off
static const uint64_t BATIDA_DOZE_MS  = 300;   // screen off for DOZE_APOS_S (phones' "Doze")
static const int      DOZE_APOS_S     = 30 * 60;
static esp_timer_handle_t s_batida = nullptr;
static uint64_t s_batendo_ms = 0;   // the current period (0 = stopped)
static void batida_cb(void*) { }   // waking up is the whole job

static void batida(uint64_t ms)
{
  if (ms == s_batendo_ms) { return; }
  if (!s_batida) {
    const esp_timer_create_args_t a = { .callback = batida_cb, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "batida_wifi" };
    if (esp_timer_create(&a, &s_batida) != ESP_OK) { return; }
  }
  if (s_batendo_ms) { esp_timer_stop(s_batida); }
  if (ms) { esp_timer_start_periodic(s_batida, ms * 1000); }
  s_batendo_ms = ms;
}

static bool alguem_precisa(void)
{
  if (tab5_wifi_receptor_pedido()) { return true; }
  if (tab5_wifi_precisada()) { return true; }   // the web server open (tab5_wifi_precisar)
  if (mesh_pela_internet()) { return true; }
  tab5_perfil_t p;               // nothing on
  fundo_somar_perfil(p);
  return p.radio;
}

static void desligar(const char* por)
{
  tab5_wifi_desligar();
  s_estado = C_COCHILANDO; s_seg = 0; ++s_cochilos;
  ESP_LOGI(TAG, "Wi-Fi dozing (%s)", por);
}

static void religar(const char* por)
{
  tab5_wifi_ligar();
  ESP_LOGI(TAG, "Wi-Fi back on (%s)", por);
}

void tab5_wifi_cochilo_vigiar(void)
{
  static int64_t ult = 0;
  const int dt = tab5_passou_s(&ult);
  // Connected while idle: the P4 sleeps, but with the heartbeat. A requested receiver
  // (ESP-NOW chatting, CSI, sniffing) wants an immediate answer: then hold it.
  const bool conectado = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
  static int s_repouso_s = 0;   // how long the screen has been off
  s_repouso_s = tab5_power_em_repouso() ? s_repouso_s + dt : 0;
  batida(!(conectado && tab5_power_em_repouso()) ? 0
         : s_repouso_s >= DOZE_APOS_S ? BATIDA_DOZE_MS : BATIDA_MS);
  sistema_economia_segurar(ECONOMIA_SEGURA_WIFI, conectado && tab5_wifi_receptor_pedido());

  const bool vale = ajustes_wifi_cochilo() && ajustes_wifi();
  const bool repouso = tab5_power_em_repouso();

  // The setting was turned off, the screen came on or someone needs it: back to normal.
  if (!vale || !repouso || alguem_precisa()) {
    if (s_estado == C_COCHILANDO && ajustes_wifi()) {
      religar(!vale ? "setting" : !repouso ? "screen on" : "request");
    }
    s_estado = C_NORMAL; s_seg = 0;
    return;
  }

  s_seg += dt;
  switch (s_estado) {
    case C_NORMAL: {
      // Only take down what is up: with Wi-Fi already stopped (Radar pauses, a
      // wrong password gave up) there is nothing to save nor to give back.
      const bool de_pe = tab5_wifi_radio_ativo() || tab5_wifi_estado() == TAB5_WIFI_CONECTANDO;
      if (s_seg >= COCHILO_APOS_S && de_pe) { desligar("screen off"); }
      break;
    }
    case C_COCHILANDO:
      if (s_seg >= JANELA_CADA_S) {
        religar("janela");
        s_estado = C_JANELA; s_seg = 0; s_conectado_s = 0;
      }
      break;
    case C_JANELA:
      if (tab5_wifi_estado() == TAB5_WIFI_CONECTADO) { s_conectado_s += dt; }
      if (s_conectado_s >= JANELA_S || s_seg >= JANELA_MAX_S) { desligar("end of window"); }
      break;
  }
}

void tab5_wifi_cochilo_resumo(char* out, size_t tam)
{
  if (!ajustes_wifi_cochilo()) {
    snprintf(out, tam, "connected, power saving%s (maximum saving off in Settings)",
             s_batendo_ms ? (s_batendo_ms == BATIDA_DOZE_MS ? ", 300 ms beat (doze)" : ", 100 ms beat") : "");
    return;
  }
  switch (s_estado) {
    case C_NORMAL:
      snprintf(out, tam, "acordado%s%s (cochilou %lu vezes)",
               !tab5_power_em_repouso() ? "" : mesh_pela_internet() ? " - Mesh over the internet: no napping"
                                          : " - screen off, counting down to nap",
               s_batendo_ms ? ", batida" : "", (unsigned long)s_cochilos);
      break;
    case C_COCHILANDO:
      snprintf(out, tam, "cochilando ha %d s (janela em %d s)", s_seg, JANELA_CADA_S - s_seg);
      break;
    case C_JANELA:
      snprintf(out, tam, "window: %d s connected of %d", s_conectado_s, JANELA_S);
      break;
  }
}
