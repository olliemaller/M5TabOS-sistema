// CORE - the radios: Bluetooth (the user's button rules), Mesh over the internet
// when the node is absent, and Wi-Fi on/off, Grove and the Mesh path. Split
// from core.cpp (the engine); what the parts share is in core_interno.hpp.

#include "trabalho.hpp"
#include "ajustes.h"
#include "tab5_ble.hpp"
#include "tab5_wifi.hpp"
#include "tab5_grove.hpp"
#include "grove.hpp"
#include "tab5_mesh.hpp"
#include "tab5_mesh_ble.hpp"
#include "esp_log.h"
#include "core_interno.hpp"

static const char *TAG = "core";

// ---------------------------------------------------------------------------
// BLUETOOTH - the user's button rules
// ---------------------------------------------------------------------------
// Boot turned Bluetooth on if the button was on OR if Mesh was
// configured for Bluetooth - turning BT off in Settings lasted until the
// next restart. Now the saved choice (ajustes_bluetooth) is the only thing
// that turns the radio on, and Mesh over Bluetooth goes up and down with it.
static void bt_mesh_subir(void*)
{
  if (!tab5_mesh_servico_ativo() || tab5_mesh_via() != AJUSTES_MESH_BLUETOOTH) { tab5_mesh_iniciar(); }
}

// ---------------------------------------------------------------------------
// MESH OVER THE INTERNET WHEN THE NODE IS ABSENT
// ---------------------------------------------------------------------------
// Without the C6L (empty cable, or Bluetooth off) and with Wi-Fi connected, the
// Mesh service comes up over the internet only (services/tab5_mesh_mqtt.hpp): the
// messages keep arriving without the radio on. Finding the node later drops
// this mode (tab5_mesh_iniciar). With Mesh on Bluetooth and BT on, what
// covers for the missing node is the service itself, which opens the internet while
// BLE searches.
static void mesh_internet_subir(void*) { tab5_mesh_iniciar_internet(); }
static void mesh_internet_descer(void*)
{
  if (tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_INTERNET) { tab5_mesh_parar(); }
}

void vigiar_mesh_internet(uint32_t t)
{
  static uint32_t s_ultima = 0;
  if (t - s_ultima < 15000) { return; }
  s_ultima = t;
  const bool ativo_net = tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_INTERNET;
  // INTERNET CHOSEN in Settings: it is the path, not the stopgap - runs even with
  // the C6L on the cable (Grove's priority was already exercised at discovery; see
  // kernel/grove.cpp). It only needs Wi-Fi.
  if (ajustes_mesh_via() == AJUSTES_MESH_INTERNET) {
    if (!tab5_mesh_servico_ativo() && !grove_sondando()
     && tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
      sistema_trabalho_solto(mesh_internet_subir, nullptr);
    }
    return;
  }
  // Internet as a stopgap: Grove with the node makes it give way, the cable is primary.
  if (!ajustes_mesh_internet() || tab5_grove_tipo() == GROVE_MESHTASTIC) {
    if (ativo_net) { sistema_trabalho_solto(mesh_internet_descer, nullptr); }
    return;
  }
  // Without Wi-Fi the internet-only mode has nothing to do: bring the service down. Two
  // consecutive reads (30 s) without Wi-Fi, so as not to drop it on a network blip.
  static int s_sem_wifi = 0;
  if (ativo_net && tab5_wifi_estado() != TAB5_WIFI_CONECTADO) {
    if (++s_sem_wifi >= 2) { s_sem_wifi = 0; sistema_trabalho_solto(mesh_internet_descer, nullptr); return; }
  } else { s_sem_wifi = 0; }
  const bool no_pelo_bt = ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH && ajustes_bluetooth();
  if (!tab5_mesh_servico_ativo() && !no_pelo_bt && !grove_sondando()
   && tab5_grove_tipo() != GROVE_MESHTASTIC
   && tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
    sistema_trabalho_solto(mesh_internet_subir, nullptr);
  }
}

static void bt_desligar(void*)
{
  // Mesh goes first: the stack must not vanish from under an open connection.
  if (tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH) { tab5_mesh_parar(); }
  // And check: if the link is still up (stopping failed, or it came up outside the
  // service), insist - bringing down the stack with it inside a ble_gap_* is the
  // device restart that was seen when turning Bluetooth off.
  if (mesh_ble_rodando()) { mesh_ble_parar(); }
  if (mesh_ble_rodando()) {
    ESP_LOGE(TAG, "the Mesh link did not go down - Bluetooth stays on");
    return;
  }
  tab5_ble_desligar();
}

void bt_aplicar(bool ligar)
{
  if (ligar) {
    tab5_ble_ligar();   // C6 rail (I2C): UI task
    // Mesh start reads the history from the card: on the worker.
    if (ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH) { sistema_trabalho_solto(bt_mesh_subir, nullptr); }
  } else {
    sistema_trabalho_solto(bt_desligar, nullptr);
  }
}

void sistema_bluetooth(bool ligar)
{
  if (ligar != ajustes_bluetooth()) { ajustes_definir_bluetooth(ligar); }
  bt_aplicar(ligar);
}

// ---- TURNING ON A SERVICE: the choice and the effect in one place (app.h) --
// Previously the Settings app, the Mesh app and the power test each did both steps
// (saving the wish + touching the hardware/service) on their own -
// and each in its own way (Grove by profile in one, by the rail in the other).
void sistema_wifi(bool ligar)
{
  if (ligar == ajustes_wifi()) { return; }
  ajustes_definir_wifi(ligar);
  if (ligar) { tab5_wifi_ligar(); } else { tab5_wifi_desligar(); }
}

void sistema_grove(bool ligar)
{
  if (ligar != ajustes_grove()) { ajustes_definir_grove(ligar); }
  sistema_reaplicar_perfil();     // the 5V follows the profile (sistema_impor_preferencias)
  sistema_grove_redetectar();     // what is on the cable may have changed
}

// Mesh again, via the saved path. On the worker: stopping and starting read the card.
static void mesh_reiniciar_trabalho(void*)
{
  tab5_mesh_parar();
  // Bluetooth and internet come up directly (the internet waits for Wi-Fi on its
  // own). The cable: the probe finds the C6L and brings up the serial.
  if (ajustes_mesh_via() != AJUSTES_MESH_SERIAL) { tab5_mesh_iniciar(); }
  else { sistema_grove_redetectar(); }
}

void sistema_mesh_reiniciar(void) { sistema_trabalho_solto(mesh_reiniciar_trabalho, nullptr); }

void sistema_mesh_via(uint8_t via)
{
  if (via != ajustes_mesh_via()) { ajustes_definir_mesh_via(via); }
  // The Bluetooth path needs Bluetooth: turn it on, and it stays saved as with the button.
  if (via == AJUSTES_MESH_BLUETOOTH) {
    if (!ajustes_bluetooth()) { ajustes_definir_bluetooth(true); }
    tab5_ble_ligar();   // rail I2C: UI task
  }
  sistema_mesh_reiniciar();
}
