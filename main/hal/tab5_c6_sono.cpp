#include "tab5_c6_sono.hpp"
#include "idioma.h"
#include "tab5_wifi.hpp"
#include "tab5_ble.hpp"
#include "tab5_farejo.hpp"   // tab5_farejo_info_c6 - INFO says whether the C6 can sleep
#include "tab5_hora.hpp"
#include "tab5_c6.h"
#include "tab5_power.hpp"   // tab5_power_em_repouso
#include "tab5_sd.hpp"      // tab5_sd_controlador_travar: the SDMMC is shared with the SD card
#include "ajustes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_hosted.h"
#include "eh_host_mcu_transport.h"   // eh_host_mcu_transport_inform_slave_ps_enter
#include "eh_host_port_power.h"      // eh_host_port_wakeup_reason_t
#include "eh_host_port_sync.h"       // EH_HOST_PORT_WAIT_FOREVER (sdio_read's wait)
#include "eh_host_peer_data.h"       // C6_AJ_IP: the C6 answers ARP for the P4 while it sleeps
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"              // WIFI_EVENT, WIFI_EVENT_STA_START (the esp_event_post wrap)

#include <atomic>
#include <stdio.h>
#include <string.h>

static const char* TAG = "c6sono";

// The wires (Tab5): C6 IO2 -> P4 G14 (the C6 calls), and the CMD line of the
// C6's SDIO slot (P4 G13 = C6 GPIO18), which the P4 pulls low to call the C6.
#define PINO_C6_CHAMA   GPIO_NUM_14
#define PINO_CMD        GPIO_NUM_13
#define CMD_PULSO_MS    2
#define ESPERA_C6_MS    1200      // per attempt, until the C6's pulse arrives
#define TENTATIVAS      3

static SemaphoreHandle_t s_trava = nullptr;   // sleep vs wake
static TaskHandle_t      s_tarefa = nullptr;  // "c6sono": services G14
static std::atomic<int>  s_estado{TAB5_C6_SONO_ACORDADO};
static std::atomic<bool> s_religando{false};
static tab5_c6_sono_estado_t s_est = { };
static uint32_t          s_dormiu_em = 0;
static bool              s_isr = false;
static bool              s_estava_associado = false;   // at tear-down time
static std::atomic<bool> s_p4_quer{false};             // someone wanted to transmit with the link down
static std::atomic<bool> s_desmontando{false};         // from signalling the C6 until esp_hosted_deinit ends

// THE AUTOMATIC MODE (ajustes_c6_sono): in the "c6sono" task, one pass per
// second. With the screen off and the link quiet for QUIETO_MS, tear down;
// with the screen on, re-mount. ESP-NOW only listens with the screen on: its
// default window (always listening) is what prevented the C6's modem sleep.
#define QUIETO_MS        3000
#define DEPOIS_ACORDAR_MS 5000
#define JANELA_INTERVALO 1000
static bool     s_janela_fechada = false;
static uint32_t s_acordou_ms = 0;
static uint32_t s_pacotes_ant = 0, s_quieto_desde = 0;
#if CONFIG_ESP_HOSTED_HOST_PKT_STATS
struct pkt_stats_espelho_t { uint32_t tx_in, tx_trans, tx_out, tx_drop, rx_in, rx_out, stalls; };
extern "C" pkt_stats_espelho_t pkt_stats;
// Only what the P4 SENDS counts as use: with the link up the router sends ~2
// broadcasts per second (ARP), and with those the link was never "quiet".
static uint32_t pacotes(void) { return pkt_stats.tx_in; }
#else
static uint32_t pacotes(void) { return 0; }
#endif

// ---------------------------------------------------------------------------
// THE LINK-TIME WRAP (main/CMakeLists.txt, --wrap). On SDIO bring-up
// esp_hosted resets the C6 - except when it thinks the host came back from
// deep sleep, in which case it only redoes the card and signals the C6
// (POWER_SAVE_OFF). During our re-mount it hears "woke by GPIO"; the rest of
// the time, the usual answer.
// ---------------------------------------------------------------------------
extern "C" eh_host_port_wakeup_reason_t __real_eh_host_port_wakeup_reason_get(void);
extern "C" eh_host_port_wakeup_reason_t __wrap_eh_host_port_wakeup_reason_get(void)
{
  if (s_religando.load()) { return EH_HOST_PORT_WAKEUP_GPIO; }
  return __real_eh_host_port_wakeup_reason_get();
}

// THE EXPECTED TEAR-DOWN FAILURE (link-time wrap, main/CMakeLists.txt). The
// C6 tears down its slave right after the signal; until esp_hosted_deinit
// stops the read task (eh_host_bus_sdio.c's s_running is only cleared at the
// end of deinit), an interrupt landing in that window reads PKT_LEN =
// 0xFFFFFFFF, and esp_hosted treats it as a dead transport:
// eh_host_port_restart_host() = abort(). On battery, with boot traffic, that
// became a loop of 37 reboots (2026-11-13). Inside the window, the failure is
// the C6 itself leaving: just log and return. Outside it, the safety reboot
// still applies (CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y stays).
extern "C" eh_host_port_err_t __real_eh_host_port_restart_host(void);
extern "C" eh_host_port_err_t __wrap_eh_host_port_restart_host(void)
{
  if (s_desmontando.load()) {
    ESP_EARLY_LOGW(TAG, "expected SDIO failure: the C6 has already torn down (not rebooting)");
    return EH_HOST_PORT_OK;
  }
  return __real_eh_host_port_restart_host();
}

// THE READ TASK STOPS AT A SAFE POINT (link-time wrap, main/CMakeLists.txt).
// esp_hosted_deinit does not wait for the sdio_read task to finish: it deletes
// it forcibly (eh_host_port_task_cancel), wherever it is. If at that instant
// the interrupt from the departing C6 has just woken it (or it is mid-read),
// FreeRTOS deletes a task in transit between lists -> uxListRemove with a
// null pointer -> panic in the c6sono task (7 reboots in the 2026-11-13 test,
// ~1 in every 100 tear-downs).
// The fix: its wait goes through here. During tear-down, instead of going
// back to waiting for the C6, it parks (sleeps for ever, stopped, holding no
// lock) and says so; the tear-down wakes its wait (xTaskAbortDelay) and only
// calls deinit after that signal. Outside tear-down, the usual wait.
extern "C" int __real_eh_host_port_sdio_wait_slave_intr(void* ctx, uint32_t espera);
static std::atomic<TaskHandle_t> s_leitora{nullptr};      // sdio_read, while it waits for the C6
static std::atomic<bool>         s_leitora_parada{false};  // it has parked

extern "C" int __wrap_eh_host_port_sdio_wait_slave_intr(void* ctx, uint32_t espera)
{
  for (;;) {
    // First announce itself, then check for tear-down: that way, either it
    // sees the signal, or the tear-down sees the task and wakes it (see
    // estacionar_leitora).
    s_leitora = xTaskGetCurrentTaskHandle();
    if (s_desmontando.load()) {
      s_leitora = nullptr;
      s_leitora_parada = true;
      for (;;) { vTaskDelay(portMAX_DELAY); }   // deinit deletes it here, parked
    }
    const int r = __real_eh_host_port_sdio_wait_slave_intr(ctx, espera);
    s_leitora = nullptr;
    // A "for ever" wait that returned without an interrupt: the tear-down woke
    // it. Back to the top to park.
    if (r == ESP_ERR_TIMEOUT && espera == EH_HOST_PORT_WAIT_FOREVER) { continue; }
    return r;
  }
}

// Called with s_desmontando set: wakes sdio_read's wait and waits for it to
// park. If it is mid-read, it finishes the read first (failures from a C6
// that has already left land in __wrap_eh_host_port_restart_host).
static bool estacionar_leitora(uint32_t prazo_ms)
{
  const uint32_t t0 = tab5_agora_ms();
  while (!s_leitora_parada.load()) {
    if (tab5_agora_ms() - t0 > prazo_ms) { return false; }
    // Repeat the wake-up: it may have announced itself and not yet blocked.
    if (TaskHandle_t h = s_leitora.load()) { xTaskAbortDelay(h); }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

// THE WI-FI FRAME GATE (esp_hosted's wifi feature, eh_host_wifi.c).
// deinit closes RX "admission" and clears the STA_START latch; what reopens
// them is the StaConnected event. The C6 resends it with the "host woke"
// notice - but that notice goes out during SDIO bring-up, BEFORE the feature
// registers its handlers, and the event is lost: every frame was then
// silently dropped (measured: the ping went out over the air and the reply
// never reached the stack). With the association intact on the C6 side, the
// re-mount restores both states by hand.
extern "C" void eh_host_wifi_admit_rx(bool is_ap, bool admit);

// THE APP KEEPS WI-FI STARTED (esp_hosted 3.0.10, eh_host_wifi.c).
// deinit also clears the "app started" and "netif already added" latches -
// but, on the P4 side, the app's netif stays in lwIP (no STA_STOP reaches
// it). After the re-mount, with the "started" latch cleared, esp_hosted stops
// delivering STA events to the app (disconnect, STA_STOP); and the next
// esp_wifi_start posts STA_START again -> netif_add of the same netif ->
// LWIP_ASSERT("netif already added") in the tiT task (panic measured on
// 2026-11-13, in the power test, when turning Wi-Fi back on).
// The fix: on re-mount, re-arm esp_hosted with its own eh_host_wifi_start
// (restoring "app started" and "netif added"), swallowing the repeated
// STA_START it posts - the app already got its own before the sleep.
extern "C" esp_err_t eh_host_wifi_start(void);
static bool                       s_app_iniciado = false;     // the app had Wi-Fi started at tear-down
static std::atomic<TaskHandle_t>  s_engolir_de{nullptr};      // who is re-arming (only it is filtered)

// THE EVENT WRAP (main/CMakeLists.txt, --wrap=esp_event_post): passes
// everything straight through, except the STA_START posted during re-arming
// by the re-arming task.
extern "C" esp_err_t __real_esp_event_post(esp_event_base_t base, int32_t id, const void* dados,
                                           size_t tam, TickType_t espera);
extern "C" esp_err_t __wrap_esp_event_post(esp_event_base_t base, int32_t id, const void* dados,
                                           size_t tam, TickType_t espera)
{
  const TaskHandle_t quem = s_engolir_de.load();
  if (quem && base == WIFI_EVENT && id == WIFI_EVENT_STA_START && quem == xTaskGetCurrentTaskHandle()) {
    ESP_LOGI(TAG, "repeated STA_START from re-mount swallowed (the app's netif is already added)");
    return ESP_OK;
  }
  return __real_esp_event_post(base, id, dados, tam, espera);
}

// Re-arms esp_hosted after the re-mount. Call with s_religando set:
// eh_host_wifi_start is an RPC, and the RPC wrap must not try to wake the
// link again (we are inside the wake-up itself).
static void rearmar_app(void)
{
  s_engolir_de = xTaskGetCurrentTaskHandle();
  const esp_err_t r = eh_host_wifi_start();   // the C6 already has Wi-Fi started: only the P4's latches change
  s_engolir_de = nullptr;
  if (r != ESP_OK) { ESP_LOGW(TAG, "re-arming the app's Wi-Fi failed: %s", esp_err_to_name(r)); }
}

// WHOEVER TRANSMITS WITH THE LINK TORN DOWN brings it back: every output from
// the IP stack to Wi-Fi goes through here (link-time wrap, main/CMakeLists.txt).
// The frame itself is lost (the lwIP task cannot wait hundreds of ms) - TCP
// retransmits, and the MQTT keepalive arrives in the retransmission.
static void pedir_acordar(void)
{
  if (s_estado.load() != TAB5_C6_SONO_DORMINDO || s_p4_quer.exchange(true)) { return; }
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}
extern "C" int __real_esp_wifi_internal_tx(int ifx, void* buffer, uint16_t len);
extern "C" int __wrap_esp_wifi_internal_tx(int ifx, void* buffer, uint16_t len)
{
  if (s_desmontando.load()) { return -1; }   // the C6's slave is going away: no writes (TCP retransmits)
  if (s_estado.load() == TAB5_C6_SONO_DORMINDO) { pedir_acordar(); return -1; }
  return __real_esp_wifi_internal_tx(ifx, buffer, len);
}
// KEPT ON PURPOSE: the linker discards this wrap today, because esp_hosted
// (3.0.9) does not call this path. On the day a new version does, without the
// wrap it would not wake the link (06/11/2026, docs/ANALISE_2026-11-06.md).
extern "C" esp_err_t __real_esp_wifi_internal_tx_by_ref(int ifx, void* buffer, size_t len, void* netstack_buf);
extern "C" esp_err_t __wrap_esp_wifi_internal_tx_by_ref(int ifx, void* buffer, size_t len, void* netstack_buf)
{
  if (s_estado.load() == TAB5_C6_SONO_DORMINDO) { pedir_acordar(); return ESP_FAIL; }
  return __real_esp_wifi_internal_tx_by_ref(ifx, buffer, len, netstack_buf);
}

// WHOEVER TALKS TO THE C6 (RPC: remote esp_wifi_*, Bluetooth, peer_data) with
// the link torn down used to wait 5 s for the link and fail - measured: the
// 'c6' command, four RPCs, took 20 s. Now the RPC entry point (link-time wrap)
// brings the link back first, and the caller only waits for the re-mount
// (~0.6 s). Not from inside the re-mount itself (feature bring-up speaks RPC).
static void antes_do_rpc(void)
{
  if (s_estado.load() != TAB5_C6_SONO_DORMINDO || s_religando.load()) { return; }
  if (xTaskGetCurrentTaskHandle() == s_tarefa) { return; }
  tab5_c6_sono_acordar();
}
// KEPT ON PURPOSE: the linker discards this wrap today, because esp_hosted
// (3.0.9) does not call this path. On the day a new version does, without the
// wrap it would not wake the link (06/11/2026, docs/ANALISE_2026-11-06.md).
extern "C" int __real_eh_host_feat_rpc_request(int32_t msg_id, void* app_req, uint32_t timeout_ms, void** out);
extern "C" int __wrap_eh_host_feat_rpc_request(int32_t msg_id, void* app_req, uint32_t timeout_ms, void** out)
{
  antes_do_rpc();
  return __real_eh_host_feat_rpc_request(msg_id, app_req, timeout_ms, out);
}
extern "C" int __real_eh_host_feat_rpc_request_sync(int32_t msg_id, void* req, void** out);
extern "C" int __wrap_eh_host_feat_rpc_request_sync(int32_t msg_id, void* req, void** out)
{
  antes_do_rpc();
  return __real_eh_host_feat_rpc_request_sync(msg_id, req, out);
}
extern "C" int __real_eh_host_feat_rpc_request_sync_best_effort(int32_t msg_id, void* req, void** out);
extern "C" int __wrap_eh_host_feat_rpc_request_sync_best_effort(int32_t msg_id, void* req, void** out)
{
  antes_do_rpc();
  return __real_eh_host_feat_rpc_request_sync_best_effort(msg_id, req, out);
}
// KEPT ON PURPOSE: the linker discards this wrap today, because esp_hosted
// (3.0.9) does not call this path. On the day a new version does, without the
// wrap it would not wake the link (06/11/2026, docs/ANALISE_2026-11-06.md).
extern "C" int __real_eh_host_feat_rpc_send(int32_t msg_id, void* app_req);
extern "C" int __wrap_eh_host_feat_rpc_send(int32_t msg_id, void* app_req)
{
  antes_do_rpc();
  return __real_eh_host_feat_rpc_send(msg_id, app_req);
}

// TEST DEBUGGING: what arrives from the network at the P4, by type (the
// link-time wrap in main/CMakeLists.txt). Goes away together with the test.
static std::atomic<uint32_t> s_rx_arp = 0, s_rx_icmp = 0, s_rx_outro = 0, s_rx_falha = 0;
extern "C" esp_err_t __real_esp_wifi_remote_channel_rx(void* h, void* buffer, void* buff_to_free, size_t len);
extern "C" esp_err_t __wrap_esp_wifi_remote_channel_rx(void* h, void* buffer, void* buff_to_free, size_t len)
{
  const uint8_t* b = (const uint8_t*)buffer;
  if (len >= 14 && b[12] == 0x08 && b[13] == 0x06) { ++s_rx_arp; }
  else if (len >= 34 && b[12] == 0x08 && b[13] == 0x00 && b[23] == 1) { ++s_rx_icmp; }
  else { ++s_rx_outro; }
  const esp_err_t r = __real_esp_wifi_remote_channel_rx(h, buffer, buff_to_free, len);
  if (r != ESP_OK) { ++s_rx_falha; }
  return r;
}
void tab5_c6_sono_rx(uint32_t* arp, uint32_t* icmp, uint32_t* outro, uint32_t* falha)
{
  *arp = s_rx_arp; *icmp = s_rx_icmp; *outro = s_rx_outro; *falha = s_rx_falha;
}

// ---------------------------------------------------------------------------
// G14: the C6 pulses (10 ms high, every 100 ms) until the link is back.
// ---------------------------------------------------------------------------
static volatile bool s_c6_chamou = false;   // the ISR saw the pulse (it lasts 10 ms)

static void IRAM_ATTR isr_c6_chama(void*)
{
  s_c6_chamou = true;
  BaseType_t acordou = pdFALSE;
  if (s_tarefa) { vTaskNotifyGiveFromISR(s_tarefa, &acordou); }
  portYIELD_FROM_ISR(acordou);
}

static void g14_preparar(void)
{
  const gpio_config_t io = {
    .pin_bit_mask = 1ULL << PINO_C6_CHAMA,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_POSEDGE,
  };
  gpio_config(&io);
  if (!s_isr) {
    gpio_install_isr_service(0);   // already installed by the kernel: ESP_ERR_INVALID_STATE
    s_isr = gpio_isr_handler_add(PINO_C6_CHAMA, isr_c6_chama, nullptr) == ESP_OK;
  }
  gpio_intr_disable(PINO_C6_CHAMA);
}

// Waits for the C6's pulse (high level on G14), polling the level: the edge
// may have come before the wait started.
static bool esperar_c6(uint32_t ms)
{
  for (uint32_t t = 0; t < ms; t += 5) {
    if (gpio_get_level(PINO_C6_CHAMA)) { return true; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return gpio_get_level(PINO_C6_CHAMA) != 0;
}

// The CMD line idles high (pull-ups on both sides); pulling it low for 2 ms
// wakes the C6. With the slot torn down the pin is a plain GPIO here.
static void cmd_pulsar(void)
{
  gpio_set_direction(PINO_CMD, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(PINO_CMD, 0);
  vTaskDelay(pdMS_TO_TICKS(CMD_PULSO_MS));
  gpio_set_level(PINO_CMD, 1);
  gpio_set_direction(PINO_CMD, GPIO_MODE_INPUT);
  gpio_set_pull_mode(PINO_CMD, GPIO_PULLUP_ONLY);
}

// ---------------------------------------------------------------------------
// RE-MOUNT
// ---------------------------------------------------------------------------
static bool religar(bool pelo_p4)
{
  const uint32_t t0 = tab5_agora_ms();
  gpio_intr_disable(PINO_C6_CHAMA);
  bool chamou = gpio_get_level(PINO_C6_CHAMA) != 0;
  // The C6 called: its next pulse comes within 100 ms. Pulling CMD low with
  // its slave already re-mounting would be a garbage command on SDIO.
  if (!chamou && !pelo_p4) { chamou = esperar_c6(300); }
  for (int i = 0; i < TENTATIVAS && !chamou; ++i) {
    cmd_pulsar();
    chamou = esperar_c6(ESPERA_C6_MS);
  }
  if (!chamou) {
    ESP_LOGE(TAG, "the C6 did not respond to CMD - the link stays down; automatic mode off");
    ajustes_definir_c6_sono(false);
    ++s_est.falhas;
    s_estado = TAB5_C6_SONO_FALHOU;
    return false;
  }
  // The C6 has re-mounted its slave: now our side, without a reset.
  gpio_reset_pin(PINO_CMD);
  s_religando = true;
  tab5_sd_controlador_travar();   // slot 1 goes back to the card's controller (see tab5_sd.hpp)
  int rc = esp_hosted_init();
  if (rc == 0) { rc = esp_hosted_connect_to_slave(); }
  tab5_sd_controlador_soltar();
  s_religando = false;
  s_est.dormindo_ms += tab5_agora_ms() - s_dormiu_em;
  if (rc != 0) {
    ESP_LOGE(TAG, "SDIO re-mount failed (%d) - Wi-Fi must be turned back on; automatic mode off", rc);
    ajustes_definir_c6_sono(false);
    ++s_est.falhas;
    s_estado = TAB5_C6_SONO_FALHOU;
    return false;
  }
  if (s_estava_associado) {
    eh_host_wifi_admit_rx(false, true);          // STA: frames reach the stack again
  }
  // After admit_rx: with reception already admitted, re-arming does not
  // repost STA_CONNECTED (the app's netif stays up, no DHCP redo).
  if (s_app_iniciado) {
    s_religando = true;
    rearmar_app();
    s_religando = false;
  }
  if (pelo_p4) { ++s_est.pelo_p4; } else { ++s_est.pelo_c6; }
  s_est.ultimo_religar_ms = tab5_agora_ms() - t0;
  s_acordou_ms = tab5_agora_ms();
  s_estado = TAB5_C6_SONO_ACORDADO;
  ESP_LOGI(TAG, "link back (%s) in %lu ms", pelo_p4 ? "P4 request" : "the C6 called",
           (unsigned long)s_est.ultimo_religar_ms);
  return true;
}

// The ESP-NOW window on the C6 (C6_AJ_ESPNOW_JANELA). Only with the link up.
static void janela(bool fechar)
{
  if (fechar == s_janela_fechada) { return; }
  const uint16_t iv = fechar ? JANELA_INTERVALO : 0, jn = fechar ? 0 : 0xFFFF;
  const uint8_t m[5] = { C6_AJ_ESPNOW_JANELA, (uint8_t)iv, (uint8_t)(iv >> 8), (uint8_t)jn, (uint8_t)(jn >> 8) };
  if (eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, m, sizeof(m)) == ESP_OK) { s_janela_fechada = fechar; }
}

static void automatico(void)
{
  const uint32_t agora = tab5_agora_ms();
  const uint32_t p = pacotes();
  if (p != s_pacotes_ant) { s_pacotes_ant = p; s_quieto_desde = agora; }
  const bool repouso = tab5_power_em_repouso();
  const tab5_c6_sono_t e = (tab5_c6_sono_t)s_estado.load();
  if (!repouso) {
    if (e == TAB5_C6_SONO_DORMINDO) { tab5_c6_sono_acordar(); }
    if (s_estado.load() == TAB5_C6_SONO_ACORDADO) { janela(false); }
    return;
  }
  if (e != TAB5_C6_SONO_ACORDADO || !ajustes_c6_sono()) { return; }
  if (agora - s_quieto_desde < QUIETO_MS || agora - s_acordou_ms < DEPOIS_ACORDAR_MS) { return; }
  // Sleeps in two cases: connected (the C6 sleeps associated and the router
  // buffers whatever arrives) or with the radio IDLE (Wi-Fi off in Settings,
  // doze, no saved network) - a needlessly awake C6 cost more than Wi-Fi on:
  // 20.6 mA versus 21.2 in the 2026-11-13 test. Does not sleep with the radio
  // working without an IP (starting, searching, ESP-NOW only): it is needed then.
  const tab5_wifi_estado_t w = tab5_wifi_estado();
  const bool conectado    = w == TAB5_WIFI_CONECTADO;
  const bool radio_parado = !tab5_wifi_radio_ativo() && w != TAB5_WIFI_SUBINDO;
  if (!(conectado || radio_parado) || tab5_ble_estado() != TAB5_BLE_DESLIGADO) { return; }
  janela(true);
  char m[96];
  if (!tab5_c6_sono_dormir(m, sizeof(m))) {
    ESP_LOGW(TAG, "automatic: did not tear down (%s) - will retry later", m);
    s_acordou_ms = agora;   // wait before the next attempt
  }
}

static void vTarefa(void*)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const bool chamou = s_c6_chamou;
    s_c6_chamou = false;
    if (s_estado.load() == TAB5_C6_SONO_DORMINDO && (s_p4_quer.load() || chamou || gpio_get_level(PINO_C6_CHAMA))) {
      xSemaphoreTake(s_trava, portMAX_DELAY);
      // The C6 called (G14 high) or the P4 wanted to transmit: the same
      // religar - it only pulls CMD low if the C6 is not already calling.
      const bool p4 = s_p4_quer.exchange(false);
      if (s_estado.load() == TAB5_C6_SONO_DORMINDO) { religar(p4 && !chamou && !gpio_get_level(PINO_C6_CHAMA)); }
      xSemaphoreGive(s_trava);
    }
    if (ajustes_c6_sono() || s_janela_fechada || s_estado.load() == TAB5_C6_SONO_DORMINDO) { automatico(); }
  }
}

static bool preparar(void)
{
  if (s_trava) { return true; }
  s_trava = xSemaphoreCreateMutex();
  if (!s_trava) { return false; }
  if (xTaskCreatePinnedToCore(vTarefa, "c6sono", 4096, nullptr, 3, &s_tarefa, tskNO_AFFINITY) != pdPASS) {
    s_tarefa = nullptr;
    return false;
  }
  g14_preparar();
  return true;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
bool tab5_c6_sono_dormir(char* motivo, size_t tam)
{
  auto nao = [&](const char* m) { if (motivo && tam) { snprintf(motivo, tam, "%s", m); } return false; };
  if (!preparar()) { return nao(tr("sem memoria")); }
  if (!tab5_wifi_c6_enlace()) { return nao(tr("o enlace com o C6 nao esta de pe")); }
  if (tab5_ble_estado() != TAB5_BLE_DESLIGADO) { return nao(tr("o Bluetooth esta ligado (a pilha BLE fala com o C6 pelo enlace)")); }
  c6_info_t in = { };
  if (!tab5_farejo_info_c6(&in, 1500)) { return nao(tr("o C6 nao respondeu ao INFO")); }
  if (in.sono_estado != C6_SONO_ACORDADO) { return nao(tr("o firmware do C6 nao sabe dormir (grave o c6-5)")); }

  xSemaphoreTake(s_trava, portMAX_DELAY);
  if (s_estado.load() != TAB5_C6_SONO_ACORDADO) { xSemaphoreGive(s_trava); return nao(tr("o enlace ja esta desmontado")); }
  // First the signal (via the SDIO interrupt): the C6 tears down its slave in
  // the esp_hosted task. Then our side. The C6 only arms the CMD wake-up
  // 500 ms later - the commands our tear-down still sends down the line do
  // not wake it.
  s_estava_associado = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
  s_app_iniciado = tab5_wifi_radio_ativo();   // esp_wifi_start done: the re-mount re-arms (rearmar_app)
  // The IP goes first: with the P4 asleep, the C6 answers ARP for it.
  if (esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
    esp_netif_ip_info_t ip = { };
    if (esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr) {
      uint8_t m[5] = { C6_AJ_IP };
      memcpy(m + 1, &ip.ip.addr, 4);
      eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, m, sizeof(m));
    }
  }
  s_p4_quer = false;
  // From the signal to the end of deinit: the window in which the C6 may
  // already be gone (see __wrap_eh_host_port_restart_host) and slot 1 leaves
  // the controller shared with the card (see tab5_sd.hpp). Lock order:
  // s_trava, then this one.
  tab5_sd_controlador_travar();
  s_leitora_parada = false;
  s_desmontando = true;
  const int rc = eh_host_mcu_transport_inform_slave_ps_enter();
  if (rc != 0) {
    s_desmontando = false;
    tab5_sd_controlador_soltar();
    xSemaphoreGive(s_trava);
    return nao(tr("o aviso ao C6 nao saiu"));
  }
  vTaskDelay(pdMS_TO_TICKS(30));
  // sdio_read parks before deinit (see __wrap_eh_host_port_sdio_wait_slave_intr).
  if (!estacionar_leitora(1500)) {
    ESP_LOGW(TAG, "the read task did not park within 1.5 s - tearing down anyway");
  }
  esp_hosted_deinit();   // on return, the read task has stopped
  s_desmontando = false;
  tab5_sd_controlador_soltar();
  gpio_reset_pin(PINO_CMD);
  gpio_set_pull_mode(PINO_CMD, GPIO_PULLUP_ONLY);
  s_dormiu_em = tab5_agora_ms();
  ++s_est.entradas;
  s_estado = TAB5_C6_SONO_DORMINDO;
  ulTaskNotifyTake(pdTRUE, 0);   // leftover from before
  gpio_intr_enable(PINO_C6_CHAMA);
  xSemaphoreGive(s_trava);
  ESP_LOGI(TAG, "link torn down: the C6 may sleep");
  return true;
}

bool tab5_c6_sono_acordar(void)
{
  if (!s_trava) { return true; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = s_estado.load() != TAB5_C6_SONO_DORMINDO || religar(true);
  xSemaphoreGive(s_trava);
  return ok;
}

void tab5_c6_sono_iniciar(void)
{
  if (ajustes_c6_sono()) { preparar(); }
}

void tab5_c6_sono_automatico(bool ligar)
{
  ajustes_definir_c6_sono(ligar);
  if (ligar) { preparar(); }
}

void tab5_c6_sono_estado(tab5_c6_sono_estado_t* out)
{
  *out = s_est;
  out->estado = (tab5_c6_sono_t)s_estado.load();
  if (out->estado == TAB5_C6_SONO_DORMINDO) { out->dormindo_ms += tab5_agora_ms() - s_dormiu_em; }
}
