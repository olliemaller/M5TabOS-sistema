#include "recursos.h"
#include "tab5_csi.hpp"
#include "tab5_ouvinte.hpp"
#include "tab5_farejo.hpp"     // tab5_farejo_caps_c6: what the flashed C6 can do
#include "tab5_wifi.hpp"
#include "pool_tarefas.h"
#include "parada.hpp"      // request/confirm the task's exit (the same as GPS and Mesh)
#include "tab5_mutex.hpp"
#include "eh_host_feat_peer_data.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>
#include <atomic>

static const char* TAG = "csi";

// The C6 switches CSI off by itself after C6_RADIO_VALIDADE_MS without renewal.
#define RENOVA_MS      3000
// Traffic source: TWICE the rate requested from the C6 (1 record every 100 ms).
// With the source at the same rate as the limit, the variable air delay makes frames
// land less than 100 ms after the previous one and get discarded.
#define PING_MS        50
#define CSI_PASSO_MS   100

// ---------------------------------------------------------------------------
// THE MOTION MEASURE
// ---------------------------------------------------------------------------
// Uses only the L-LTF, the part every OFDM frame carries (11g and HT) and which comes
// first in the buffer: 64 subcarriers, 2 bytes each, in the order 0..31, -32..-1.
// +-1..+-26 are valid; the rest is DC and guard band (nearly zero, and normalising
// would divide noise by noise). Valid indices in the buffer: 1..26 and 38..63.
//
// 1. Amplitude per subcarrier, NORMALISED by the frame mean: the C6's automatic
//    gain (AGC) changes from frame to frame and would move everything together.
// 2. Deviation = mean of |a - ref| over the 52 subcarriers, where ref is the channel's
//    own moving average (~1 s). When still, the channel is stable and the deviation is just
//    noise; something moving changes the reflection pattern and the deviation rises.
// 3. The "still" baseline of each place is different (walls, distance to the router): the
//    service LEARNS the typical still deviation in the first few seconds and then
//    follows it slowly, only going down quickly. The level comes from the RATIO between the
//    deviation now and the still one - a number that holds in any house.
#define SC_N           52
#define REF_ALFA       0.10f    // channel moving average: ~1 s at 10 records/s
#define DESV_ALFA      0.30f    // smooths the deviation (~0.3 s)
#define APRENDER_N     30       // ~3 s of records for the first "still" baseline
#define BASE_DESCE     0.20f    // the still baseline follows quickly downwards...
#define BASE_SOBE      0.003f   // ...and slowly upwards (~30 s): long movement doesn't become "normal" too soon
#define BASE_MIN       0.004f
#define RAZAO_LEVE     2.0f
#define RAZAO_FORTE    4.0f
#define HISTERESE      0.85f    // to drop a level the ratio falls 15% below the threshold

static uint8_t s_idx[SC_N];

// Measurement state: only the event task (the callback) writes it.
static float    s_ref[SC_N];
static bool     s_tem_ref = false;
static float    s_desv = 0, s_base = 0, s_base_soma = 0;
static int      s_aprendidos = 0;
static volatile float   s_razao = 1.0f;
static volatile uint8_t s_nivel = TAB5_CSI_PARADO;
static volatile int8_t  s_rssi = 0;
static std::atomic<uint32_t> s_cont = 0;
static volatile uint16_t s_por_s = 0;
static int64_t  s_t_seg = 0;
static uint32_t s_cont_seg = 0;

static volatile bool s_mexendo = false;
static volatile bool s_zerar = false;      // request to restart the reference (another task)
static tab5_ouvinte_t<tab5_csi_ouvinte_t> s_ouvinte;   // removing waits for the in-flight call

// Service.
static volatile uint32_t s_quem = 0;
// SWITCHING ON AND OFF, WITHOUT A RACE. Previously 's_rodando' only became true INSIDE
// the task: two 'ligar' calls in a row created two tasks, and switching on right after
// switching off landed on a task that was already exiting. Now it is the kernel's
// parada_t: switching off (the last 'quem') REQUESTS the exit; switching on with a run
// exiting WAITS for it to confirm (on the semaphore) and only then creates the new one. Both
// go through the same lock.
static parada_t          s_parada = { };
static Trava s_trava_cmd;
static volatile tab5_csi_fase_t s_fase = TAB5_CSI_DESLIGADO;
static pool_id_t         s_id = POOL_ID_VAZIO;
static bool              s_registrado = false;

static void medir(const c6_csi_cab_t& c, const uint8_t* d, uint16_t n)
{
  // Only 11g and HT: they start with the L-LTF. HE has a different subcarrier arrangement.
  const uint8_t fmt = C6_CSI_FORMATO(c.flags);
  if ((fmt != 1 && fmt != 2) || n < 128) { return; }

  if (s_zerar || s_mexendo) {
    s_zerar = false;
    s_tem_ref = false;
    s_desv = 0;
    return;
  }

  float a[SC_N];
  float soma = 0;
  const bool w0_ruim = (c.flags & C6_CSI_F_W0_RUIM) != 0;   // subcarriers 0 and 1 invalid
  int validos = 0;
  for (int k = 0; k < SC_N; ++k) {
    const int i = s_idx[k];
    if (w0_ruim && i < 2) { a[k] = -1.0f; continue; }
    const float im = (float)(int8_t)d[2 * i], re = (float)(int8_t)d[2 * i + 1];
    a[k] = sqrtf(im * im + re * re);
    soma += a[k];
    ++validos;
  }
  if (validos < SC_N - 2 || soma < (float)validos) { return; }   // frame with no energy: don't measure
  const float m = soma / (float)validos;
  for (int k = 0; k < SC_N; ++k) { if (a[k] >= 0) { a[k] /= m; } }

  if (!s_tem_ref) {
    for (int k = 0; k < SC_N; ++k) { s_ref[k] = a[k] >= 0 ? a[k] : 1.0f; }
    s_tem_ref = true;
    return;
  }
  float desv = 0;
  for (int k = 0; k < SC_N; ++k) {
    if (a[k] < 0) { continue; }
    desv += fabsf(a[k] - s_ref[k]);
    s_ref[k] += REF_ALFA * (a[k] - s_ref[k]);
  }
  desv /= (float)validos;
  s_desv += DESV_ALFA * (desv - s_desv);

  if (s_aprendidos < APRENDER_N) {
    s_base_soma += s_desv;
    if (++s_aprendidos == APRENDER_N) {
      s_base = s_base_soma / (float)APRENDER_N;
      if (s_base < BASE_MIN) { s_base = BASE_MIN; }
    }
    return;
  }
  s_base += (s_desv < s_base ? BASE_DESCE : BASE_SOBE) * (s_desv - s_base);
  if (s_base < BASE_MIN) { s_base = BASE_MIN; }

  const float r = s_desv / s_base;
  s_razao = r;
  uint8_t nv = s_nivel;
  if      (r >= RAZAO_FORTE)                            { nv = TAB5_CSI_FORTE; }
  else if (r >= RAZAO_LEVE && nv == TAB5_CSI_PARADO)    { nv = TAB5_CSI_LEVE; }
  else if (nv == TAB5_CSI_FORTE && r < RAZAO_FORTE * HISTERESE) { nv = r >= RAZAO_LEVE ? TAB5_CSI_LEVE : TAB5_CSI_PARADO; }
  else if (nv == TAB5_CSI_LEVE  && r < RAZAO_LEVE * HISTERESE)  { nv = TAB5_CSI_PARADO; }
  s_nivel = nv;
}

// In esp_hosted's event task: measure and leave.
static void ao_peer_data(uint32_t msg_id, const uint8_t* d, size_t n, void*)
{
  if (msg_id != C6_MSG_CSI || n < sizeof(c6_csi_cab_t)) { return; }
  c6_csi_cab_t c;
  memcpy(&c, d, sizeof(c));
  const uint8_t* bytes = d + sizeof(c);
  uint16_t len = (uint16_t)(n - sizeof(c));
  if (len > c.len) { len = c.len; }

  ++s_cont;
  s_rssi = c.rssi;
  const int64_t agora = esp_timer_get_time();
  ++s_cont_seg;
  if (agora - s_t_seg >= 1000000) { s_por_s = (uint16_t)s_cont_seg; s_cont_seg = 0; s_t_seg = agora; }

  medir(c, bytes, len);
  tab5_ouvinte_chamar(&s_ouvinte, (const c6_csi_cab_t*)&c, bytes, len);
}

// ---------------------------------------------------------------------------
// Renewal + traffic source - a pool slot, only while on.
// ---------------------------------------------------------------------------
static void mandar(const c6_csi_cfg_t& cfg)
{
  uint8_t b[1 + sizeof(c6_csi_cfg_t)];
  b[0] = C6_RX_CSI;
  memcpy(b + 1, &cfg, sizeof(cfg));
  eh_host_peer_data_send(C6_MSG_HOST_RADIO, b, sizeof(b));
}

static esp_ping_handle_t ping_iniciar(uint32_t gw)
{
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr.type = IPADDR_TYPE_V4;
  cfg.target_addr.u_addr.ip4.addr = gw;
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = PING_MS;
  cfg.timeout_ms = 1000;
  cfg.data_size = 16;
  esp_ping_callbacks_t cb = { };
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { return nullptr; }
  esp_ping_start(h);
  return h;
}

static void ping_parar(esp_ping_handle_t* h)
{
  if (!*h) { return; }
  esp_ping_stop(*h);
  esp_ping_delete_session(*h);
  *h = nullptr;
}

static void vTarefa(void*)
{
  esp_ping_handle_t ping = nullptr;
  uint32_t gw_ping = 0;
  bool pediu_c6 = false;
  sistema_recurso(RECURSO_RADIO_ESCUTA, "CSI", true);   // in modem sleep the C6 loses frames

  while (!parada_pedida(&s_parada)) {
    // Is there something to measure from? Our firmware with CSI, and WiFi connected to the router.
    const bool c6_ok = tab5_wifi_c6_nosso() && tab5_wifi_c6_enlace()
                       && (tab5_farejo_caps_c6() & C6_CAP_CSI);
    uint32_t gw = 0;
    wifi_ap_record_t ap;
    bool tem_ap = false;
    if (c6_ok && tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
      esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      esp_netif_ip_info_t ip;
      if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) { gw = ip.gw.addr; }
      tem_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    }

    if (!c6_ok) {
      s_fase = TAB5_CSI_SEM_C6;
    } else if (!gw || !tem_ap) {
      s_fase = TAB5_CSI_SEM_ROTEADOR;
    }

    if (c6_ok && gw && tem_ap) {
      if (s_fase == TAB5_CSI_SEM_C6 || s_fase == TAB5_CSI_SEM_ROTEADOR) { s_fase = TAB5_CSI_APRENDENDO; }
      // Only CSI coming FROM THE ROUTER: it is the fixed link that serves as the reference.
      c6_csi_cfg_t cfg = { };
      cfg.liga = 1;
      cfg.so_alvo = 1;
      memcpy(cfg.alvo, ap.bssid, 6);
      cfg.min_intervalo_ms = CSI_PASSO_MS;
      mandar(cfg);
      pediu_c6 = true;
      if (gw != gw_ping) { ping_parar(&ping); gw_ping = 0; }
      if (!ping) { ping = ping_iniciar(gw); if (ping) { gw_ping = gw; } }
    } else {
      if (pediu_c6) { c6_csi_cfg_t off = { }; mandar(off); pediu_c6 = false; }
      ping_parar(&ping);
      gw_ping = 0;
      s_zerar = true;
    }

    // Sleeps until the next round; stopping wakes it at once (parada_pedir).
    parada_dormir(&s_parada, RENOVA_MS);
  }

  ping_parar(&ping);
  if (pediu_c6) { c6_csi_cfg_t off = { }; mandar(off); }
  sistema_recurso(RECURSO_RADIO_ESCUTA, "CSI", false);
  s_fase = TAB5_CSI_DESLIGADO;
  ESP_LOGI(TAG, "CSI off");
  parada_confirmar(&s_parada);   // the LAST thing: from here on a 'ligar' may create another
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
static void preparar(void)
{
  if (s_registrado) { return; }
  // The 52 valid L-LTF indices in the buffer (see THE MOTION MEASURE).
  int k = 0;
  for (int i = 1; i <= 26; ++i)  { s_idx[k++] = (uint8_t)i; }
  for (int i = 38; i <= 63; ++i) { s_idx[k++] = (uint8_t)i; }
  eh_host_peer_data_register(C6_MSG_CSI, ao_peer_data, nullptr);
  s_registrado = true;
}

void tab5_csi_ligar(uint32_t quem)
{
  preparar();
  s_trava_cmd.travar();
  if (parada_rodando(&s_parada)) {
    if (!parada_pedida(&s_parada)) {     // up and staying: just add the requester
      s_quem = s_quem | quem;
      s_trava_cmd.destravar();
      return;
    }
    // Exiting (the last one switched off a moment ago): wait for the exit, then start another.
    if (!parada_pedir(&s_parada, 1500)) {
      s_trava_cmd.destravar();
      ESP_LOGE(TAG, "the previous CSI task hasn't exited yet - not switched on");
      return;
    }
  }
  s_quem = s_quem | quem;
  // Measuring from scratch: the learnt "still" baseline applied to the previous place.
  s_zerar = true;
  s_aprendidos = 0;
  s_base_soma = 0;
  s_base = 0;
  s_razao = 1.0f;
  s_nivel = TAB5_CSI_PARADO;
  s_fase = TAB5_CSI_APRENDENDO;
  if (!parada_preparar(&s_parada)
   || !pool_criar(&s_id, vTarefa, nullptr, "csi", 3072, 3, DONO_SERVICO, "CSI")) {
    parada_desistir(&s_parada);
    s_quem = 0;
    s_fase = TAB5_CSI_DESLIGADO;
    s_trava_cmd.destravar();
    ESP_LOGE(TAG, "no slot for the CSI task");
    return;
  }
  s_trava_cmd.destravar();
  ESP_LOGI(TAG, "CSI on");
}

void tab5_csi_desligar(uint32_t quem)
{
  s_trava_cmd.travar();
  s_quem = s_quem & ~quem;
  // The last one left: REQUESTS the exit (doesn't wait - the one switching off is the screen). The
  // task sees the request on the next round; the wake-up takes it out of vTaskDelay.
  if (!s_quem && parada_rodando(&s_parada)) { s_parada.pedido = true; parada_cutucar(&s_parada); }
  s_trava_cmd.destravar();
}

void tab5_csi_aparelho_mexendo(bool sim) { s_mexendo = sim; }

void tab5_csi_ouvinte(tab5_csi_ouvinte_t cb)
{
  preparar();
  tab5_ouvinte_definir(&s_ouvinte, cb);
}

void tab5_csi_estado(tab5_csi_estado_t* out)
{
  if (!out) { return; }
  out->por_s = s_por_s;
  out->rssi = s_rssi;
  out->razao = s_razao;
  out->nivel = (tab5_csi_nivel_t)s_nivel;
  tab5_csi_fase_t f = s_fase;
  if (f == TAB5_CSI_APRENDENDO || f == TAB5_CSI_MEDINDO) {
    // por_s is only rewritten when a record arrives: still for more than 2 s is the
    // same as zero.
    if (esp_timer_get_time() - s_t_seg > 2000000) { out->por_s = 0; }
    if (s_mexendo)                         { f = TAB5_CSI_APARELHO_MEXENDO; }
    else if (s_aprendidos < APRENDER_N)    { f = TAB5_CSI_APRENDENDO; }
    else                                   { f = TAB5_CSI_MEDINDO; }
  }
  out->fase = f;
}
