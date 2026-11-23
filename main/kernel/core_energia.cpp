// CORE - power: screen kept on by request, power-saving contract (light sleep
// lock), panel clock and link changes on the UI task, and the hardware profile
// (what apps and preferences keep switched on). Split from
// core.cpp (the engine); what the parts share is in core_interno.hpp.

#include "tab5_camera.hpp"
#include "ajustes.h"
#include "tab5_wifi.hpp"
#include "tab5_usb.hpp"
#include "grove.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_pm.h"
#include "core_interno.hpp"

// SCREEN KEPT ON BY REQUEST (app.h): one bit per reason, from any task. The
// UI looks at the mask on every turn; when it reaches zero, the idle deadline
// counts from THAT instant - releasing after an hour on must not turn the
// screen off immediately.
std::atomic<uint32_t> s_acordado{0};
volatile bool s_pedir_acordar = false;
void sistema_manter_acordado(uint32_t motivo, bool sim)
{
  if (sim) {
    // A new request with the screen already off: preventing the NEXT idle is not enough -
    // the current one carries on with the clock at the floor and the person seeing nothing. Wake.
    const uint32_t antes = s_acordado.fetch_or(motivo);
    if (!(antes & motivo) && tab5_power_em_repouso()) { s_pedir_acordar = true; sistema_acordar_interface(); }
  } else {
    s_acordado.fetch_and(~motivo);
  }
}
// ---- POWER-SAVING CONTRACT (app.h) ------------------------------------------
economia_t sistema_economia(void) { return tab5_power_em_repouso() ? ECONOMIA_REPOUSO : ECONOMIA_ACORDADO; }

static uint32_t s_seguros = 0;
static esp_pm_lock_handle_t s_trava_sono = nullptr;

void sistema_economia_segurar(uint32_t quem, bool sim)
{
#if CONFIG_PM_ENABLE
  // The lock is created the first time (from any task: esp_pm_lock_create is
  // safe); the atomic OR/AND pair decides who takes it and who releases it.
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  if (!s_trava_sono) {
    esp_pm_lock_handle_t h = nullptr;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "economia", &h) != ESP_OK) { return; }
    taskENTER_CRITICAL(&mux);
    if (!s_trava_sono) { s_trava_sono = h; h = nullptr; }
    taskEXIT_CRITICAL(&mux);
    if (h) { esp_pm_lock_delete(h); }
  }
  const uint32_t antes = sim ? __atomic_fetch_or(&s_seguros, quem, __ATOMIC_ACQ_REL)
                             : __atomic_fetch_and(&s_seguros, ~quem, __ATOMIC_ACQ_REL);
  const uint32_t depois = sim ? (antes | quem) : (antes & ~quem);
  if (!antes && depois)      { esp_pm_lock_acquire(s_trava_sono); }
  else if (antes && !depois) { esp_pm_lock_release(s_trava_sono); }
#else
  (void)quem; (void)sim;
#endif
}

uint32_t sistema_economia_seguros(void) { return __atomic_load_n(&s_seguros, __ATOMIC_ACQUIRE); }
// PANEL CLOCK CHANGES ONLY ON THE UI TASK. Changing the clock
// destroys and recreates the panel - and the pages with it. In direct mode the UI
// writes TO THE PAGES throughout the drawing pass: destroying the panel from
// another task (the console) in the middle of that would be writing to freed memory.
// Whoever is not the UI leaves the request, wakes the UI and WAITS ON THE
// SEMAPHORE it gives when done - no asking every 10 ms.
static std::atomic<int> s_relogio_pedido{0};
static std::atomic<int> s_relogio_resposta{0};   // 1 = ok, -1 = failed
SemaphoreHandle_t s_relogio_feito = nullptr;   // created with the UI (sistema_iniciar)

bool sistema_trocar_relogio_painel(int mhz)
{
  if (sistema_na_interface() || !s_tarefa_interface) { return tab5_power_relogio_dpi(mhz); }
  if (!s_relogio_feito) { return false; }
  xSemaphoreTake(s_relogio_feito, 0);   // leftover from a request that gave up waiting
  s_relogio_resposta.store(0);
  s_relogio_pedido.store(mhz);
  sistema_acordar_interface();
  if (xSemaphoreTake(s_relogio_feito, pdMS_TO_TICKS(5000)) != pdTRUE) { return false; }
  return s_relogio_resposta.load() == 1;
}

// The same request, for the DSI link speed (test: 'tela link').
static std::atomic<int> s_link_pedido{0};

bool sistema_trocar_link_painel(int mbps)
{
  if (sistema_na_interface() || !s_tarefa_interface) { return tab5_power_link_dsi(mbps); }
  if (!s_relogio_feito) { return false; }
  xSemaphoreTake(s_relogio_feito, 0);
  s_relogio_resposta.store(0);
  s_link_pedido.store(mbps);
  sistema_acordar_interface();
  if (xSemaphoreTake(s_relogio_feito, pdMS_TO_TICKS(5000)) != pdTRUE) { return false; }
  return s_relogio_resposta.load() == 1;
}

void atender_pedido_de_relogio(void)
{
  const int mbps = s_link_pedido.exchange(0);
  if (mbps != 0) {
    s_relogio_resposta.store(tab5_power_link_dsi(mbps) ? 1 : -1);
    xSemaphoreGive(s_relogio_feito);
  }
  const int mhz = s_relogio_pedido.exchange(0);
  if (mhz == 0) { return; }
  s_relogio_resposta.store(tab5_power_relogio_dpi(mhz) ? 1 : -1);
  xSemaphoreGive(s_relogio_feito);
}

// ---------------------------------------------------------------------------
// Boot profile: the union of what apps need to EXIST
// ---------------------------------------------------------------------------
tab5_perfil_t sistema_perfil_de_boot(void)
{
  tab5_perfil_t u;
  u.brilho = 80;

  // Only the PHASE 1 flags - the ones that decide whether the driver gets
  // initialised at all. If ONE app needs the IMU, the IMU exists for all; there is
  // no way to initialise it after M5.begin().
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const tab5_perfil_t& p = SISTEMA_APPS[i]->perfil;
    u.imu          = u.imu          || p.imu;
    u.microfone    = u.microfone    || p.microfone;
    u.alto_falante = u.alto_falante || p.alto_falante;
    u.rtc          = u.rtc          || p.rtc;
  }
  return u;
}

// EVERY saved preference enters the profile HERE, and nowhere else.
//
// Born from a bug: Grove stayed off at boot even with the
// preference saved as on, and only came on when some app was opened -
// because the rule lived on the app-opening path and boot did not go through it.
// The screen showed "ON" and the hardware was OFF, which is the worst combination.
//
// Rule: the app's profile says what IT needs; this function imposes what the
// USER asked for. The app can only ADD - which is why everything here is "||", never "=".
void sistema_impor_preferencias(tab5_perfil_t& p)
{
  p.brilho   = ajustes_brilho();                          // preference always wins
  // "necessary" = the stack already came up this boot (tab5_wifi_pilha_de_pe, HAL)
  // OR the user wants WiFi on (ajustes_wifi, service) - the calculation lives
  // HERE, because only the kernel can read both without inverting
  // the dependency (see docs/ESCREVENDO_APPS.md section 8).
  p.radio    = p.radio    || tab5_wifi_pilha_de_pe() || ajustes_wifi()
             || ajustes_bluetooth();   // the C6 is the Bluetooth radio
  p.grove_5v = p.grove_5v || ajustes_grove();
  // USB-A (services/tab5_usb): the flash drive must have power to be
  // seen when it is plugged in - we cannot wait for someone to ask.
  // When idle with nobody using it, services/tab5_usb cuts it (tab5_usb_repouso):
  // a profile reapplied midway must not turn it back on.
  p.usb_a_5v = p.usb_a_5v || (ajustes_usb_a() && !tab5_usb_cortado());

  // THE CAMERA BECAME A SERVICE (reference counting, see the big
  // note in tab5_camera.hpp) - SAME REASON as Grove above: an app open
  // NOW cannot decide on its own to turn off CAM_RST, because whatever is
  // using the camera may be something else (the console command 'foto', for
  // example) that goes through no app profile at all. tab5_camera_ativa()
  // OR'd here guarantees that reapplying ANY app's profile, at any
  // moment, never cuts power to a consumer that is still alive.
  p.camera   = p.camera   || tab5_camera_ativa();

  // BACKGROUND PARTS (kernel/fundo.h): what each one declared it needs goes in
  // here - for the SAME reason as the camera above: whatever is on screen cannot
  // turn off WiFi under an upload running in the background.
  fundo_somar_perfil(p);
}

tab5_perfil_t perfil_da_home(void)
{
  tab5_perfil_t p;                    // nothing on
  sistema_impor_preferencias(p);      // what the user asked for goes in here
  return p;
}

// Declared in app.h. Builds the profile of WHAT IS ON SCREEN now - the open app
// or home - and applies it. It is the only way for a setting to take effect immediately without the
// app touching the hardware separately, on top of the rule that it can only add.
// REQUESTS a Grove re-probe. Does not block: the work happens on the next
// cycle of the UI loop, which STOPS the current owner (Mesh or GPS, both are
// services now - see the re-probe block further down) before probing
// again. This used to refuse when the owner was GPS, because
// Azimutal read the UART directly and a probe on top would break the read;
// GPS became a service with the same clean shutdown Mesh already had, and the
// refusal stopped making sense.
bool sistema_grove_redetectar(void)
{
  grove_pedir_sondagem();
  s_sujo_layout = true;
  return true;
}

bool sistema_grove_sondando(void) { return grove_sondando(); }

void sistema_reaplicar_perfil(void)
{
  tab5_perfil_t p = app_vivo() ? s_app->perfil : tab5_perfil_t();
  sistema_impor_preferencias(p);
  tab5_power_aplicar(p);
}
