// GROVE WATCHER - who is on the cable, and when to ask again.
//
// Asking "who is there?" (the probe) costs up to ~10 s: stopping Mesh or the
// GPS (up to 1 s each), turning on the rail, waiting for the module to boot and listening on the
// two baud rates. That is why it runs on a pool task, never on the UI (touch
// went deaf while someone swapped the module).
//
// "Is anyone there?" is cheap, and a timer asks every 2 s (10 s when
// idle). Each state has something watching it:
//   - FREE port: the RX pin with pull-down (tab5_grove_presenca). Two
//     identical reads before believing it - a cable being plugged in makes contact before
//     it seats;
//   - MESH port: the radio silent for more than 60 s. A wide margin on purpose:
//     the service asks for an introduction every 10 s while it is silent, and an alarm
//     too early would drop a radio that is merely busy. Its OWN deadline, not the
//     service's SILENCIO_MS - added together, a module swap took 150 s;
//   - GPS port: 5 s silent. The GPS talks by itself at ~1 Hz, so silence
//     there means a pulled cable with much more certainty.
//
// RETRIES: the first probe is hostage to the moment. The C6L takes a few
// seconds to respond, and may return noise ("desconhecido", unknown) instead of
// silence while it finishes booting. With nothing conclusive, it tries again at
// 20 s and at 50 s - and that is all. A real module swap (requested, or the pin
// changed) reopens those two attempts; the automatic attempt does not.
#include "grove.hpp"
#include "internal.hpp"
#include "app.h"
#include "ajustes.h"
#include "pool_tarefas.h"
#include "tab5_grove.hpp"
#include "tab5_hora.hpp"
#include "tab5_mesh.hpp"
#include "tab5_gps.hpp"

#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>

static const char* TAG = "grove";

static const uint32_t VIGIA_MS     = 2000;    // watch the pin (awake)
static const uint32_t VIGIA_REP_MS = 10000;   // likewise, when idle
static const uint32_t MESH_MUDO_MS = 60000;
static const uint32_t GPS_MUDO_MS  = 5000;
static const uint32_t PRIMEIRA_MS  = 1000;    // the boot one: after the first draw

static esp_timer_handle_t s_timer = nullptr;
static std::atomic<bool>  s_sondando{false};   // from the trigger until the task finishes
static std::atomic<bool>  s_pedido{false};     // probe requested, not yet triggered
// Only the timer and the probe task (one at a time) touch these:
static int      s_extras = 0;         // automatic attempts already made
static uint32_t s_desde  = 0;         // from when the attempts count
static bool     s_havia  = false;     // last pin read
static int      s_iguais = 0;         // consecutive identical reads

static void sondar(void* reabrir)
{
  if (reabrir) { s_extras = 0; s_desde = tab5_agora_ms(); }
  const bool mesh_ok = tab5_mesh_soltar_grove();   // via Bluetooth the node is not on the cable
  const bool gps_ok  = tab5_gps_parar();
  if (!mesh_ok || !gps_ok) {
    ESP_LOGE(TAG, "could not release Grove - probe cancelled");
  } else if (!ajustes_grove()) {
    ESP_LOGI(TAG, "Grove off in settings - stopped the service and did not probe");
  } else {
    const grove_tipo_t achado = tab5_grove_detectar();
    if (achado == GROVE_MESHTASTIC) {
      // GROVE HAS PRIORITY: finding the C6L on the cable moves Mesh to it, from
      // any path. Only here, on discovery - afterwards, the manual choice in Settings
      // holds until the next discovery.
      if (ajustes_mesh_via() != AJUSTES_MESH_SERIAL) {
        ESP_LOGI(TAG, "Meshtastic on Grove: the cable has priority - Mesh moves to it");
        ajustes_definir_mesh_via(AJUSTES_MESH_SERIAL);
      }
      tab5_mesh_iniciar();
    } else if (achado == GROVE_GPS) {
      tab5_gps_iniciar();
    }
  }
  // The next pin read is compared with what the probe has just seen.
  s_havia  = (tab5_grove_tipo() != GROVE_NADA);
  s_iguais = 0;
  s_sujo_layout = true;
  s_sondando = false;   // LAST - only now can the next one start
  sistema_acordar_interface();
}

// 6 KB, the large slot: the probe brings up Mesh, which reads the card (FATFS
// mount, NodeDB, journal) on this stack - the same path that overflowed ESP-NOW
// with 4 KB. With 4 KB the monitor recorded 316 B free (25/10/2026).
static void disparar(bool reabrir)
{
  bool livre = false;
  if (!s_sondando.compare_exchange_strong(livre, true)) { return; }   // there is already one running
  s_pedido = false;
  if (!pool_criar(nullptr, sondar, reabrir ? (void*)1 : nullptr, "grove_sonda", 6144, 1)) {
    s_sondando = false;
  }
}

// Has something changed on the cable? Only the cheap question; the expensive one goes to disparar().
static bool cabo_mudou(void)
{
  const grove_dono_t dono = tab5_grove_dono();
  if (dono == GROVE_DONO_MESH) {
    if (tab5_mesh_silencio_ms() <= MESH_MUDO_MS) { return false; }
    ESP_LOGW(TAG, "radio silent for %us - releasing Grove and probing", (unsigned)(MESH_MUDO_MS / 1000));
    return true;
  }
  if (dono == GROVE_DONO_GPS) {
    if (tab5_gps_silencio_ms() <= GPS_MUDO_MS) { return false; }
    ESP_LOGW(TAG, "GPS silent for %us - releasing Grove and probing", (unsigned)(GPS_MUDO_MS / 1000));
    return true;
  }
  const bool ha = tab5_grove_presenca();
  if (ha != s_havia) { s_havia = ha; s_iguais = 1; return false; }
  if (++s_iguais != 2) { return false; }   // acts only on the second identical read, once
  const bool tinha = (tab5_grove_tipo() != GROVE_NADA);
  if (ha && !tinha) { ESP_LOGI(TAG, "something was plugged into Grove - probing"); return true; }
  if (!ha && tinha) {
    // No probe is paid for to conclude "nothing": the pin already said so. With the port
    // free there is no service to stop - just forget the type.
    ESP_LOGI(TAG, "Grove emptied");
    tab5_grove_esquecer();
    s_sujo_layout = true;
    sistema_acordar_interface();
  }
  return false;
}

static void ao_timer(void*)
{
  if (s_pedido) {
    disparar(true);
  } else if (ajustes_grove() && !s_sondando) {
    if (cabo_mudou()) {
      disparar(true);
    } else if (s_extras < 2 && tab5_grove_dono() == GROVE_DONO_NINGUEM
            && (tab5_grove_tipo() == GROVE_NADA || tab5_grove_tipo() == GROVE_DESCONHECIDO)
            && tab5_agora_ms() - s_desde > 20000u + (uint32_t)s_extras * 30000u) {
      ++s_extras;
      ESP_LOGI(TAG, "Grove inconclusive (%s) - trying again (%d/2)",
               tab5_grove_tipo() == GROVE_NADA ? "nothing" : "unknown", s_extras);
      disparar(false);
    }
  }
  // GROVE OFF (setting): with no power on the cable there is nothing to watch, and the
  // timer would only wake the P4 from light sleep every 10 s for nothing. It stops
  // here; whatever turns Grove on (sistema_grove, the return from the power test) goes through
  // sistema_grove_redetectar -> grove_pedir_sondagem, which turns it back on.
  if (!ajustes_grove() && !s_pedido && !s_sondando) { return; }
  esp_timer_start_once(s_timer, (uint64_t)sistema_ritmo_ms(VIGIA_MS, VIGIA_REP_MS) * 1000u);
}

void grove_iniciar(void)
{
  const esp_timer_create_args_t a = { .callback = ao_timer, .arg = nullptr,
                                      .dispatch_method = ESP_TIMER_TASK, .name = "grove",
                                      .skip_unhandled_events = true };
  if (esp_timer_create(&a, &s_timer) != ESP_OK) { ESP_LOGE(TAG, "no timer - Grove unwatched"); return; }
  if (ajustes_grove()) { s_pedido = true; }
  else { ESP_LOGI(TAG, "Grove without power (setting off) - not probing"); }
  esp_timer_start_once(s_timer, (uint64_t)PRIMEIRA_MS * 1000u);
}

void grove_pedir_sondagem(void)
{
  s_pedido = true;
  if (s_timer) { esp_timer_stop(s_timer); esp_timer_start_once(s_timer, 0); }
}

bool grove_sondando(void) { return s_pedido || s_sondando; }
