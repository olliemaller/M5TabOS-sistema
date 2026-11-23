#include "tab5_ble.hpp"
#include "idioma.h"
#include "tab5_ouvinte.hpp"
#include "tab5_sinal.hpp"
#include "tab5_wifi.hpp"
#include "tab5_donos.hpp"
#include "pool_tarefas.h"   // on/off in a pool slot, without a task of its own

#include "esp_hosted_bt_host_stack.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include <cstdio>
#include <cstring>
#include <atomic>

static const char* TAG = "ble";

extern "C" void ble_store_config_init(void);

// How long to wait for the C6 to bring up the link (first time at boot: WiFi
// may be off and the esp_hosted stack is only now being created) and for
// NimBLE to sync with its controller.
#define PRAZO_PILHA_MS  20000
#define PRAZO_SYNC_MS    8000


// ON/OFF WITHOUT A TASK OF ITS OWN. There was a 4 KB task (internal RAM)
// sleeping for ever just to serve these two rare requests. Now the request
// goes to a slot in the task pool (kernel/pool_tarefas.h), which exists only
// while bring-up/tear-down lasts. ORDER is still guaranteed: only one run at
// a time (s_rodando), and each request gets a number - the run only ends once
// it has served the last number requested, and anything arriving while it
// runs is not lost (see disparar).
static volatile bool     s_quer      = false;   // the last request: on?
static volatile uint32_t s_pedido    = 0;       // number of the last request
static volatile uint32_t s_atendido  = 0;       // number of the last one served
static volatile bool     s_rodando   = false;   // a run is in the slot
static bool              s_iniciado  = false;
static SemaphoreHandle_t      s_trava = nullptr;       // the list of seen devices
static SemaphoreHandle_t      s_sync = nullptr;        // NimBLE has synced
// THE STACK IN USE. Scan/cancel (from any task) and tear-down (BLE task) go
// through here: without it, a ble_gap_disc from the screen could land in the
// middle of nimble_port_deinit - NimBLE mutex already destroyed, and the
// device reboots.
static SemaphoreHandle_t      s_pilha = nullptr;
static volatile bool          s_ressincronizar = false; // the stack reset by itself
static volatile tab5_ble_estado_t s_estado = TAB5_BLE_DESLIGADO;
static volatile bool          s_varrendo = false;
static std::atomic<uint32_t>      s_versao = 1;
static bool                   s_pilha_nimble = false;  // nimble_port_init done
static ble_uuid_any_t         s_uuid_mesh;

static tab5_ble_visto_t s_vistos[TAB5_BLE_VISTOS];
static int              s_n_vistos = 0;
static int8_t           s_rssi_publicado[TAB5_BLE_VISTOS];   // what the screen has already shown

static void mudou(void) { ++s_versao; }

// ---------------------------------------------------------------------------
// STACK
// ---------------------------------------------------------------------------
static void ao_sincronizar(void)
{
  // Own address: the C6's public one. Without this step the first ble_gap_*
  // returns BLE_HS_ENOADDR.
  ble_hs_util_ensure_addr(0);
  xSemaphoreGive(s_sync);
  // After the stack resets by itself (ao_reiniciar) nobody else puts the
  // state back to PRONTO: subir() finished long ago.
  if (s_ressincronizar && s_estado == TAB5_BLE_SUBINDO) {
    s_ressincronizar = false;
    s_estado = TAB5_BLE_PRONTO;
    mudou();
  }
}

static void ao_reiniciar(int motivo)
{
  ESP_LOGW(TAG, "the stack reset (reason %d)", motivo);
  s_varrendo = false;
  // Going down or still coming up: whoever is serving the request (execucao) decides the state.
  if (s_estado == TAB5_BLE_PRONTO) { s_ressincronizar = true; s_estado = TAB5_BLE_SUBINDO; }
  mudou();
}

static void tarefa_pilha(void*)
{
  nimble_port_run();            // only returns on nimble_port_stop()
  // The task is about to die: the owner registry must not keep the handle
  // (FreeRTOS reuses it, and another task would inherit the "Bluetooth" owner).
  tab5_dono_esquecer(xTaskGetCurrentTaskHandle());
  nimble_port_freertos_deinit();
}

static bool subir(void)
{
  // The C6 has to be up. If WiFi is off the esp_hosted stack does not even
  // exist yet: whoever asked to turn on has already called
  // tab5_wifi_garantir_pilha() (UI task, because of I2C); here we just wait
  // for it to be ready.
  for (uint32_t t = 0; !tab5_wifi_pilha_de_pe() && t < PRAZO_PILHA_MS; t += 100) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!tab5_wifi_pilha_de_pe()) {
    ESP_LOGE(TAG, "the C6 did not come up within %d s - no Bluetooth", PRAZO_PILHA_MS / 1000);
    return false;
  }

  if (!s_pilha_nimble) {
    esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    esp_err_t err = esp_hosted_bt_host_stack_setup(&cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "the C6 did not provide the BLE controller (%s) - its firmware has no BT?",
               esp_err_to_name(err));
      return false;
    }
    tab5_mem_marco("BLE: C6 controller");
    err = nimble_port_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
      esp_hosted_bt_host_stack_teardown();
      return false;
    }

    ble_hs_cfg.sync_cb  = ao_sincronizar;
    ble_hs_cfg.reset_cb = ao_reiniciar;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // SECURITY: Meshtastic requires PIN pairing. The Tab5 is the side that
    // TYPES the PIN (the node displays it or has a fixed one) - "keyboard
    // only". Bond stored (NVS): the PIN is only asked for the first time.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    s_pilha_nimble = true;
    xSemaphoreTake(s_sync, 0);
    nimble_port_freertos_init(tarefa_pilha);
    TaskHandle_t h = xTaskGetHandle("nimble_host");
    if (h) { tab5_dono_registrar(h, DONO_SERVICO, "Bluetooth"); }
    tab5_mem_marco("NimBLE (stack + task)");
  }

  if (xSemaphoreTake(s_sync, pdMS_TO_TICKS(PRAZO_SYNC_MS)) != pdTRUE && !ble_hs_synced()) {
    ESP_LOGE(TAG, "the stack did not sync with the C6 controller");
    return false;
  }
  return true;
}

static bool descer(void)
{
  if (!s_pilha_nimble) { return true; }
  xSemaphoreTake(s_pilha, portMAX_DELAY);
  if (s_varrendo) { ble_gap_disc_cancel(); s_varrendo = false; }
  bool ok = false;
  // nimble_port_stop() already drops open connections (ble_hs_stop) and
  // waits for the stack task to leave its loop.
  if (nimble_port_stop() == 0) {
    nimble_port_deinit();
    esp_hosted_bt_host_stack_teardown();
    s_pilha_nimble = false;
    ok = true;
  } else {
    ESP_LOGW(TAG, "the stack did not stop - staying up");
  }
  xSemaphoreGive(s_pilha);
  return ok;
}

// One request: the same body as the old task, to turn on or off.
static void aplicar(bool ligar)
{
  if (ligar) {
    if (s_estado == TAB5_BLE_PRONTO) { return; }
    s_estado = TAB5_BLE_SUBINDO; mudou();
    const bool ok = subir();
    s_estado = ok ? TAB5_BLE_PRONTO : TAB5_BLE_FALHOU; mudou();
    if (ok) { ESP_LOGI(TAG, "Bluetooth up (NimBLE stack on the P4, C6 radio)"); }
  } else {
    if (s_estado == TAB5_BLE_DESLIGADO) { return; }
    if (descer()) {
      s_estado = TAB5_BLE_DESLIGADO; mudou();
      ESP_LOGI(TAG, "Bluetooth off");
    } else {
      s_estado = TAB5_BLE_PRONTO; mudou();
    }
  }
}

static void disparar(void);

static void execucao(void*)
{
  uint32_t n;
  do {
    n = s_pedido;
    aplicar(s_quer);
    s_atendido = n;
  } while (s_atendido != s_pedido);   // another arrived meanwhile: serve it too
  s_rodando = false;
  // A request that arrived between the while and the line above found
  // s_rodando still set and did not fire: check again.
  if (s_atendido != s_pedido) { disparar(); }
}

// No free pool slot: try again shortly (slots free up quickly), until it
// works. Otherwise the request vanishes and the state gets stuck (e.g.
// DESLIGANDO). The timer is one-shot and created once; the callback runs in
// the esp_timer task, which is fine: disparar() only does an atomic +
// pool_criar, and pool_criar does not need a particular task.
#define BLE_REPETE_MS 500
static esp_timer_handle_t s_timer_repete = nullptr;
static bool               s_falha_logada = false;   // log only the first failure in a run

static void repetir_disparo(void*) { disparar(); }

static void disparar(void)
{
  bool livre = false;
  if (!__atomic_compare_exchange_n(&s_rodando, &livre, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;   // a run is already going: it serves this request too
  }
  if (pool_criar(nullptr, execucao, nullptr, "ble", 4096, 3, DONO_SERVICO, "Bluetooth")) {
    s_falha_logada = false;
    return;
  }
  s_rodando = false;
  if (!s_falha_logada) {
    s_falha_logada = true;
    ESP_LOGE(TAG, "no slot to turn Bluetooth on/off - retrying in %d ms", BLE_REPETE_MS);
  }
  if (!s_timer_repete) {
    const esp_timer_create_args_t args = { .callback = repetir_disparo, .arg = nullptr,
                                           .dispatch_method = ESP_TIMER_TASK, .name = "ble_repete" };
    if (esp_timer_create(&args, &s_timer_repete) != ESP_OK) {
      s_timer_repete = nullptr;
      ESP_LOGE(TAG, "no timer to retry the Bluetooth request");
      return;
    }
  }
  esp_timer_start_once(s_timer_repete, (uint64_t)BLE_REPETE_MS * 1000);
}

static void pedir(bool ligar)
{
  s_quer = ligar;
  __atomic_add_fetch(&s_pedido, 1, __ATOMIC_ACQ_REL);
  disparar();
}

// ---------------------------------------------------------------------------
// SCAN
// ---------------------------------------------------------------------------
// TRACKERS AND BEACONS - recognised by the advert's public fields.
//   Apple (company 0x004C): type 0x12 = Find My network (AirTag, and offline
//     iPhone/Mac); type 0x02 0x15 = iBeacon, with RSSI at 1 m in the last byte.
//   Tile: service 0xFEED/0xFEEC.  SmartTag (Samsung): service 0xFD5A.
//   Google 0xFEAA: frame 0x40/0x41 = Find My Device network; the other
//     frames are Eddystone (0x00 and 0x10 carry RSSI at 0 m).
static bool uuid16_anunciado(const struct ble_hs_adv_fields& f, uint16_t u)
{
  for (int k = 0; k < f.num_uuids16; ++k) { if (f.uuids16[k].value == u) { return true; } }
  return f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2
      && (uint16_t)(f.svc_data_uuid16[0] | (f.svc_data_uuid16[1] << 8)) == u;
}

static void classificar(tab5_ble_visto_t* v, const struct ble_hs_adv_fields& f)
{
  const uint8_t* m = f.mfg_data;
  const int mn = f.mfg_data ? f.mfg_data_len : 0;
  auto marcar = [v](uint8_t classe, const char* marca) {
    v->classe = classe;
    snprintf(v->marca, sizeof(v->marca), "%s", marca);
  };
  if (mn >= 3 && m[0] == 0x4C && m[1] == 0x00) {
    if (m[2] == 0x12) { marcar(TAB5_BLE_RASTREADOR, "Find My"); }
    else if (mn >= 25 && m[2] == 0x02 && m[3] == 0x15) { marcar(TAB5_BLE_BEACON, "iBeacon"); v->p1m = (int8_t)m[24]; }
  }
  if (uuid16_anunciado(f, 0xFEED) || uuid16_anunciado(f, 0xFEEC)) { marcar(TAB5_BLE_RASTREADOR, "Tile"); }
  if (uuid16_anunciado(f, 0xFD5A)) { marcar(TAB5_BLE_RASTREADOR, "SmartTag"); }
  if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 3
      && f.svc_data_uuid16[0] == 0xAA && f.svc_data_uuid16[1] == 0xFE) {
    const uint8_t quadro = f.svc_data_uuid16[2];
    if (quadro == 0x40 || quadro == 0x41) { marcar(TAB5_BLE_RASTREADOR, "Google"); }
    else {
      marcar(TAB5_BLE_BEACON, "Eddystone");
      if ((quadro == 0x00 || quadro == 0x10) && f.svc_data_uuid16_len >= 4) { v->p1m = (int8_t)(f.svc_data_uuid16[3] - TAB5_SINAL_PERDA_1M); }
    }
  }
  if (v->p1m == 0 && f.tx_pwr_lvl_is_present) { v->p1m = (int8_t)(f.tx_pwr_lvl - TAB5_SINAL_PERDA_1M); }
}

static void anotar(const ble_addr_t* a, int8_t rssi, const uint8_t* dados, uint8_t n)
{
  struct ble_hs_adv_fields f;
  const bool campos = (ble_hs_adv_parse_fields(&f, dados, n) == 0);

  xSemaphoreTake(s_trava, portMAX_DELAY);
  int i = 0;
  for (; i < s_n_vistos; ++i) {
    if (memcmp(s_vistos[i].addr, a->val, 6) == 0) { break; }
  }
  bool novo = false;
  if (i == s_n_vistos) {
    if (s_n_vistos < TAB5_BLE_VISTOS) {
      ++s_n_vistos;
    } else {
      // Full: the one seen longest ago goes.
      i = 0;
      for (int k = 1; k < TAB5_BLE_VISTOS; ++k) {
        if (s_vistos[k].visto_ms < s_vistos[i].visto_ms) { i = k; }
      }
    }
    memset(&s_vistos[i], 0, sizeof(s_vistos[i]));
    memcpy(s_vistos[i].addr, a->val, 6);
    novo = true;
  }
  tab5_ble_visto_t* v = &s_vistos[i];
  v->tipo = a->type;
  v->rssi = novo ? rssi : (int8_t)((v->rssi * 3 + rssi) / 4);
  // The on-screen distance follows someone walking with the device looking
  // for a tag: a 4 dB difference (~40% in distance) already counts as news.
  if (novo || v->rssi - s_rssi_publicado[i] >= 4 || s_rssi_publicado[i] - v->rssi >= 4) {
    s_rssi_publicado[i] = v->rssi;
    novo = true;
  }
  v->visto_ms = tab5_agora_ms();
  if (campos) {
    // The name comes in the scan response, the service in the advert: each
    // report carries one part, and what is already known is not erased by a
    // report without it.
    if (f.name && f.name_len) {
      const size_t k = f.name_len < sizeof(v->nome) - 1 ? f.name_len : sizeof(v->nome) - 1;
      if (k != strlen(v->nome) || memcmp(v->nome, f.name, k) != 0) { novo = true; }
      memcpy(v->nome, f.name, k);
      v->nome[k] = 0;
    }
    // By name too: not every firmware puts the service UUID in the advert (it
    // only sometimes fits in the 31-byte packet with the name), and the
    // default Meshtastic name starts with "Meshtastic".
    if (!v->meshtastic && strncmp(v->nome, "Meshtastic", 10) == 0) { v->meshtastic = true; novo = true; }
    for (int k = 0; k < f.num_uuids128; ++k) {
      if (ble_uuid_cmp(&f.uuids128[k].u, &s_uuid_mesh.u) == 0) {
        if (!v->meshtastic) { novo = true; }
        v->meshtastic = true;
      }
    }
    const uint8_t antes = v->classe;
    classificar(v, f);
    if (v->meshtastic) { v->classe = TAB5_BLE_MESHTASTIC; }
    if (v->classe != antes) { novo = true; }
  }
  xSemaphoreGive(s_trava);
  if (novo) { mudou(); }
}

static tab5_ouvinte_t<tab5_ble_ouvinte_t> s_ouvinte;   // removing waits for the call in flight

static int ao_evento_gap(struct ble_gap_event* ev, void*)
{
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
      anotar(&ev->disc.addr, ev->disc.rssi, ev->disc.data, ev->disc.length_data);
      // Outside the list lock. Whoever removes the listener waits for this
      // call to finish (tab5_ouvinte.hpp) before destroying what it uses.
      tab5_ouvinte_chamar(&s_ouvinte, (const uint8_t*)ev->disc.addr.val, ev->disc.rssi);
      break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
      s_varrendo = false;
      mudou();
      break;
    default: break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void tab5_ble_iniciar(void)
{
  if (s_iniciado) { return; }
  s_iniciado = true;
  s_trava = xSemaphoreCreateMutex();
  s_sync  = xSemaphoreCreateBinary();
  s_pilha = xSemaphoreCreateMutex();
  ble_uuid_from_str(&s_uuid_mesh, TAB5_BLE_UUID_MESHTASTIC);
  // No task: on/off go to a pool slot (see disparar).
}

void tab5_ble_ligar(void)
{
  if (!s_iniciado) { return; }
  tab5_wifi_garantir_pilha();   // radio rail I2C: hence "from the UI task"
  pedir(true);
}

void tab5_ble_desligar(void)
{
  if (!s_iniciado) { return; }
  // Now, not when the run picks up the request: from here on nobody sees
  // "pronto" (ready) and starts a new scan or connection.
  if (s_estado != TAB5_BLE_DESLIGADO) { s_estado = TAB5_BLE_DESLIGANDO; mudou(); }
  pedir(false);
}

tab5_ble_estado_t tab5_ble_estado(void) { return s_estado; }
bool tab5_ble_pronto(void) { return s_estado == TAB5_BLE_PRONTO; }

const char* tab5_ble_descricao(void)
{
  switch (s_estado) {
    case TAB5_BLE_DESLIGADO: return N_("desligado");
    case TAB5_BLE_SUBINDO:   return N_("ligando");
    case TAB5_BLE_PRONTO:    return s_varrendo ? "procurando aparelhos" : "ligado";
    case TAB5_BLE_FALHOU:    return N_("falhou (o C6 não deu o Bluetooth)");
    case TAB5_BLE_DESLIGANDO: return N_("desligando");
  }
  return "?";
}

bool tab5_ble_varrer(uint32_t ms)
{
  if (!s_pilha || !tab5_ble_pronto() || s_varrendo) { return false; }
  // Without waiting: if the stack is going down right now, the answer is "no".
  if (xSemaphoreTake(s_pilha, 0) != pdTRUE) { return false; }
  if (!tab5_ble_pronto() || !s_pilha_nimble) { xSemaphoreGive(s_pilha); return false; }
  uint8_t proprio;
  if (ble_hs_id_infer_auto(0, &proprio) != 0) { xSemaphoreGive(s_pilha); return false; }
  struct ble_gap_disc_params p;
  memset(&p, 0, sizeof(p));
  p.passive = 0;            // active: the name comes in the scan response
  p.filter_duplicates = 0;  // name and service arrive in separate reports
  const int rc = ble_gap_disc(proprio, (int32_t)ms, &p, ao_evento_gap, nullptr);
  if (rc == 0) { s_varrendo = true; }
  xSemaphoreGive(s_pilha);
  if (rc != 0) {
    ESP_LOGW(TAG, "scan refused (%d)", rc);   // BLE_HS_EBUSY: connecting
    return false;
  }
  mudou();
  return true;
}

bool tab5_ble_varrendo(void) { return s_varrendo; }

void tab5_ble_ouvinte(tab5_ble_ouvinte_t f) { tab5_ouvinte_definir(&s_ouvinte, f); }

void tab5_ble_varredura_parar(void)
{
  if (!s_varrendo || !s_pilha) { return; }
  // If the stack is going down, descer() cancels anyway - do not wait for it.
  if (xSemaphoreTake(s_pilha, 0) != pdTRUE) { return; }
  if (s_pilha_nimble && s_varrendo) { ble_gap_disc_cancel(); }
  s_varrendo = false;
  xSemaphoreGive(s_pilha);
  mudou();
}

int tab5_ble_n_vistos(void) { return s_n_vistos; }

bool tab5_ble_visto(int i, tab5_ble_visto_t* out)
{
  if (!s_trava || !out) { return false; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = (i >= 0 && i < s_n_vistos);
  if (ok) { *out = s_vistos[i]; }
  xSemaphoreGive(s_trava);
  return ok;
}

uint32_t tab5_ble_versao(void) { return s_versao; }

void tab5_ble_endereco_txt(const uint8_t a[6], char out[18])
{
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool tab5_ble_endereco_ler(const char* txt, uint8_t a[6])
{
  unsigned b[6];
  if (!txt || sscanf(txt, "%2x:%2x:%2x:%2x:%2x:%2x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) {
    return false;
  }
  for (int k = 0; k < 6; ++k) { a[k] = (uint8_t)b[k]; }
  return true;
}
