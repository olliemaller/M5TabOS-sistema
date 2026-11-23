#include "tab5_c6_sono.hpp"
#include "idioma.h"
#include "tab5_wifi.hpp"
#include "tab5_ble.hpp"
#include "tab5_farejo.hpp"   // tab5_farejo_info_c6 - o INFO diz se o C6 sabe dormir
#include "tab5_hora.hpp"
#include "tab5_c6.h"
#include "tab5_power.hpp"   // tab5_power_em_repouso
#include "tab5_sd.hpp"      // tab5_sd_controlador_travar: o SDMMC e dividido com o cartao
#include "ajustes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_hosted.h"
#include "eh_host_mcu_transport.h"   // eh_host_mcu_transport_inform_slave_ps_enter
#include "eh_host_port_power.h"      // eh_host_port_wakeup_reason_t
#include "eh_host_port_sync.h"       // EH_HOST_PORT_WAIT_FOREVER (a espera da sdio_read)
#include "eh_host_peer_data.h"       // C6_AJ_IP: o C6 responde o ARP do P4 enquanto ele dorme
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"              // WIFI_EVENT, WIFI_EVENT_STA_START (o desvio do esp_event_post)

#include <atomic>
#include <stdio.h>
#include <string.h>

static const char* TAG = "c6sono";

// Os fios (Tab5): C6 IO2 -> P4 G14 (o C6 chama), e a linha CMD do slot do C6
// no SDIO (P4 G13 = C6 GPIO18), que o P4 baixa para chamar o C6.
#define PINO_C6_CHAMA   GPIO_NUM_14
#define PINO_CMD        GPIO_NUM_13
#define CMD_PULSO_MS    2
#define ESPERA_C6_MS    1200      // por tentativa, ate o pulso do C6 chegar
#define TENTATIVAS      3

static SemaphoreHandle_t s_trava = nullptr;   // dormir x acordar
static TaskHandle_t      s_tarefa = nullptr;  // "c6sono": atende o G14
static std::atomic<int>  s_estado{TAB5_C6_SONO_ACORDADO};
static std::atomic<bool> s_religando{false};
static tab5_c6_sono_estado_t s_est = { };
static uint32_t          s_dormiu_em = 0;
static bool              s_isr = false;
static bool              s_estava_associado = false;   // na hora de desmontar
static std::atomic<bool> s_p4_quer{false};             // alguem quis transmitir com o enlace desmontado
static std::atomic<bool> s_desmontando{false};         // do aviso ao C6 ate o fim do esp_hosted_deinit

// O MODO AUTOMATICO (ajustes_c6_sono): na tarefa "c6sono", uma volta por
// segundo. Com a tela apagada e o enlace quieto por QUIETO_MS, desmonta; com
// a tela acesa, remonta. O ESP-NOW so ouve com a tela acesa: a janela padrao
// dele (sempre ouvindo) e o que impedia o modem sleep do C6.
#define QUIETO_MS        3000
#define DEPOIS_ACORDAR_MS 5000
#define JANELA_INTERVALO 1000
static bool     s_janela_fechada = false;
static uint32_t s_acordou_ms = 0;
static uint32_t s_pacotes_ant = 0, s_quieto_desde = 0;
#if CONFIG_ESP_HOSTED_HOST_PKT_STATS
struct pkt_stats_espelho_t { uint32_t tx_in, tx_trans, tx_out, tx_drop, rx_in, rx_out, stalls; };
extern "C" pkt_stats_espelho_t pkt_stats;
// So o que o P4 MANDA conta como uso: com o enlace de pe o roteador manda ~2
// broadcasts por segundo (ARP), e com eles o enlace nunca ficava "quieto".
static uint32_t pacotes(void) { return pkt_stats.tx_in; }
#else
static uint32_t pacotes(void) { return 0; }
#endif

// ---------------------------------------------------------------------------
// O DESVIO DO LINK (main/CMakeLists.txt, --wrap). Na subida do SDIO o
// esp_hosted reseta o C6 - menos quando acha que o host voltou de um deep
// sleep, e ai so refaz o cartao e avisa o C6 (POWER_SAVE_OFF). Durante a
// remontagem daqui ele ouve "voltei por GPIO"; no resto do tempo, a resposta
// de sempre.
// ---------------------------------------------------------------------------
extern "C" eh_host_port_wakeup_reason_t __real_eh_host_port_wakeup_reason_get(void);
extern "C" eh_host_port_wakeup_reason_t __wrap_eh_host_port_wakeup_reason_get(void)
{
  if (s_religando.load()) { return EH_HOST_PORT_WAKEUP_GPIO; }
  return __real_eh_host_port_wakeup_reason_get();
}

// A FALHA ESPERADA DA DESMONTAGEM (desvio do link, main/CMakeLists.txt). O C6
// desmonta o escravo dele logo depois do aviso; ate o esp_hosted_deinit parar
// a tarefa de leitura (o s_running do eh_host_bus_sdio.c so zera no fim do
// deinit), uma interrupcao que caia nessa janela le PKT_LEN = 0xFFFFFFFF, e o
// esp_hosted trata como transporte morto: eh_host_port_restart_host() =
// abort(). Na bateria, com o trafego do boot, isso virou um loop de 37
// reinicios (2026-11-13). Dentro da janela, a falha e o proprio C6 saindo:
// so registra e volta. Fora dela, o reinicio de seguranca continua valendo
// (CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y fica como esta).
extern "C" eh_host_port_err_t __real_eh_host_port_restart_host(void);
extern "C" eh_host_port_err_t __wrap_eh_host_port_restart_host(void)
{
  if (s_desmontando.load()) {
    ESP_EARLY_LOGW(TAG, "falha do SDIO esperada: o C6 ja desmontou (sem reiniciar)");
    return EH_HOST_PORT_OK;
  }
  return __real_eh_host_port_restart_host();
}

// A TAREFA DE LEITURA PARA NUM PONTO SEGURO (desvio do link, main/CMakeLists.txt).
// O esp_hosted_deinit nao espera a tarefa sdio_read terminar: apaga-a a forca
// (eh_host_port_task_cancel), onde quer que ela esteja. Se nesse instante a
// interrupcao do C6 que esta saindo acabou de acorda-la (ou ela esta no meio de
// uma leitura), o FreeRTOS apaga uma tarefa em transito entre listas ->
// uxListRemove com ponteiro nulo -> panic na tarefa c6sono (7 reinicios no
// ensaio de 2026-11-13, ~1 a cada 100 desmontagens).
// O conserto: a espera dela passa por aqui. Durante a desmontagem, em vez de
// voltar a esperar o C6, ela estaciona (dorme para sempre, parada, sem lock
// nenhum) e avisa; a desmontagem acorda a espera dela (xTaskAbortDelay) e so
// chama o deinit depois do aviso. Fora da desmontagem, a espera de sempre.
extern "C" int __real_eh_host_port_sdio_wait_slave_intr(void* ctx, uint32_t espera);
static std::atomic<TaskHandle_t> s_leitora{nullptr};      // a sdio_read, enquanto espera o C6
static std::atomic<bool>         s_leitora_parada{false};  // ela ja estacionou

extern "C" int __wrap_eh_host_port_sdio_wait_slave_intr(void* ctx, uint32_t espera)
{
  for (;;) {
    // Primeiro se anuncia, depois olha a desmontagem: assim, ou ela ve o
    // aviso, ou a desmontagem ve a tarefa e a acorda (ver estacionar_leitora).
    s_leitora = xTaskGetCurrentTaskHandle();
    if (s_desmontando.load()) {
      s_leitora = nullptr;
      s_leitora_parada = true;
      for (;;) { vTaskDelay(portMAX_DELAY); }   // o deinit a apaga aqui, parada
    }
    const int r = __real_eh_host_port_sdio_wait_slave_intr(ctx, espera);
    s_leitora = nullptr;
    // Espera "para sempre" que voltou sem interrupcao: foi a desmontagem que a
    // acordou. Volta ao topo para estacionar.
    if (r == ESP_ERR_TIMEOUT && espera == EH_HOST_PORT_WAIT_FOREVER) { continue; }
    return r;
  }
}

// Chamada com s_desmontando ligado: acorda a espera da sdio_read e aguarda ela
// estacionar. Se ela estiver no meio de uma leitura, termina a leitura antes
// (as falhas de um C6 que ja saiu caem no __wrap_eh_host_port_restart_host).
static bool estacionar_leitora(uint32_t prazo_ms)
{
  const uint32_t t0 = tab5_agora_ms();
  while (!s_leitora_parada.load()) {
    if (tab5_agora_ms() - t0 > prazo_ms) { return false; }
    // Repete o despertar: ela pode ter se anunciado e ainda nao ter bloqueado.
    if (TaskHandle_t h = s_leitora.load()) { xTaskAbortDelay(h); }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

// A PORTA DOS QUADROS DO WI-FI (feature wifi do esp_hosted, eh_host_wifi.c).
// O deinit fecha a "admissao" de RX e zera a trava de STA_START; quem reabre e
// o evento StaConnected. O C6 o reenvia no aviso de "host acordou" - mas esse
// aviso sai na subida do SDIO, ANTES de a feature registrar os tratadores, e o
// evento se perde: todo quadro passava a ser descartado em silencio (medido:
// o ping saia pelo ar e a resposta nunca chegava a pilha). Com a associacao
// intacta do lado do C6, a remontagem devolve os dois estados a mao.
extern "C" void eh_host_wifi_admit_rx(bool is_ap, bool admit);

// O APP CONTINUA COM O WI-FI INICIADO (esp_hosted 3.0.10, eh_host_wifi.c).
// O deinit tambem zera as travas "o app iniciou" e "o netif ja foi adicionado"
// - mas, do lado do P4, o netif do app continua no lwIP (nenhum STA_STOP chega
// a ele). Depois da remontagem, com a trava "iniciou" zerada, o esp_hosted
// para de entregar ao app os eventos de STA (queda, STA_STOP); e o proximo
// esp_wifi_start posta STA_START de novo -> netif_add do mesmo netif ->
// LWIP_ASSERT("netif already added") na tarefa tiT (panic medido em
// 2026-11-13, no ensaio de energia, ao religar o Wi-Fi).
// O conserto: na remontagem, rearmar o esp_hosted com o seu proprio
// eh_host_wifi_start (volta "o app iniciou" e "netif adicionado"), engolindo o
// STA_START repetido que ele posta - o app ja recebeu o dele antes do sono.
extern "C" esp_err_t eh_host_wifi_start(void);
static bool                       s_app_iniciado = false;     // o app tinha o Wi-Fi iniciado ao desmontar
static std::atomic<TaskHandle_t>  s_engolir_de{nullptr};      // quem esta rearmando (so ele e filtrado)

// O DESVIO DOS EVENTOS (main/CMakeLists.txt, --wrap=esp_event_post): passa tudo
// direto, menos o STA_START postado durante o rearme, pela tarefa que rearma.
extern "C" esp_err_t __real_esp_event_post(esp_event_base_t base, int32_t id, const void* dados,
                                           size_t tam, TickType_t espera);
extern "C" esp_err_t __wrap_esp_event_post(esp_event_base_t base, int32_t id, const void* dados,
                                           size_t tam, TickType_t espera)
{
  const TaskHandle_t quem = s_engolir_de.load();
  if (quem && base == WIFI_EVENT && id == WIFI_EVENT_STA_START && quem == xTaskGetCurrentTaskHandle()) {
    ESP_LOGI(TAG, "STA_START repetido da remontagem engolido (o netif do app ja esta adicionado)");
    return ESP_OK;
  }
  return __real_esp_event_post(base, id, dados, tam, espera);
}

// Rearma o esp_hosted depois da remontagem. Chamar com s_religando ligado: o
// eh_host_wifi_start e um RPC, e o desvio do RPC nao pode tentar acordar o
// enlace de novo (estamos dentro do proprio acordar).
static void rearmar_app(void)
{
  s_engolir_de = xTaskGetCurrentTaskHandle();
  const esp_err_t r = eh_host_wifi_start();   // o C6 ja esta com o Wi-Fi iniciado: so as travas do P4 mudam
  s_engolir_de = nullptr;
  if (r != ESP_OK) { ESP_LOGW(TAG, "rearmar o Wi-Fi do app falhou: %s", esp_err_to_name(r)); }
}

// QUEM TRANSMITE COM O ENLACE DESMONTADO o traz de volta: toda saida da
// pilha IP para o Wi-Fi passa por aqui (desvio do link, main/CMakeLists.txt).
// O quadro em si e perdido (a tarefa do lwIP nao pode esperar centenas de ms)
// - o TCP reenvia, e o keepalive do MQTT chega no reenvio.
static void pedir_acordar(void)
{
  if (s_estado.load() != TAB5_C6_SONO_DORMINDO || s_p4_quer.exchange(true)) { return; }
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}
extern "C" int __real_esp_wifi_internal_tx(int ifx, void* buffer, uint16_t len);
extern "C" int __wrap_esp_wifi_internal_tx(int ifx, void* buffer, uint16_t len)
{
  if (s_desmontando.load()) { return -1; }   // o escravo do C6 esta sumindo: nada de escrita (o TCP reenvia)
  if (s_estado.load() == TAB5_C6_SONO_DORMINDO) { pedir_acordar(); return -1; }
  return __real_esp_wifi_internal_tx(ifx, buffer, len);
}
// FICA DE PROPOSITO: o linker descarta este desvio hoje, porque o esp_hosted
// (3.0.9) nao chama este caminho. No dia em que uma versao nova chamar, sem o
// desvio ele nao acordaria o enlace (06/11/2026, docs/ANALISE_2026-11-06.md).
extern "C" esp_err_t __real_esp_wifi_internal_tx_by_ref(int ifx, void* buffer, size_t len, void* netstack_buf);
extern "C" esp_err_t __wrap_esp_wifi_internal_tx_by_ref(int ifx, void* buffer, size_t len, void* netstack_buf)
{
  if (s_estado.load() == TAB5_C6_SONO_DORMINDO) { pedir_acordar(); return ESP_FAIL; }
  return __real_esp_wifi_internal_tx_by_ref(ifx, buffer, len, netstack_buf);
}

// QUEM CONVERSA COM O C6 (RPC: esp_wifi_* remoto, Bluetooth, peer_data) com o
// enlace desmontado esperava 5 s pelo enlace e falhava - medido: o comando
// 'c6', quatro RPCs, levou 20 s. Agora a entrada do RPC (desvio do link) traz
// o enlace de volta antes, e quem chamou espera so a remontagem (~0,6 s).
// Nao de dentro da propria remontagem (a subida das features fala RPC).
static void antes_do_rpc(void)
{
  if (s_estado.load() != TAB5_C6_SONO_DORMINDO || s_religando.load()) { return; }
  if (xTaskGetCurrentTaskHandle() == s_tarefa) { return; }
  tab5_c6_sono_acordar();
}
// FICA DE PROPOSITO: o linker descarta este desvio hoje, porque o esp_hosted
// (3.0.9) nao chama este caminho. No dia em que uma versao nova chamar, sem o
// desvio ele nao acordaria o enlace (06/11/2026, docs/ANALISE_2026-11-06.md).
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
// FICA DE PROPOSITO: o linker descarta este desvio hoje, porque o esp_hosted
// (3.0.9) nao chama este caminho. No dia em que uma versao nova chamar, sem o
// desvio ele nao acordaria o enlace (06/11/2026, docs/ANALISE_2026-11-06.md).
extern "C" int __real_eh_host_feat_rpc_send(int32_t msg_id, void* app_req);
extern "C" int __wrap_eh_host_feat_rpc_send(int32_t msg_id, void* app_req)
{
  antes_do_rpc();
  return __real_eh_host_feat_rpc_send(msg_id, app_req);
}

// DEPURACAO DA PROVA: o que chega da rede ao P4, por tipo (o desvio do link
// em main/CMakeLists.txt). Sai junto com a prova.
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
// O G14: o C6 pulsa (10 ms em alto, a cada 100 ms) ate o enlace voltar.
// ---------------------------------------------------------------------------
static volatile bool s_c6_chamou = false;   // a ISR viu o pulso (ele dura 10 ms)

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
    gpio_install_isr_service(0);   // ja instalado pelo kernel: ESP_ERR_INVALID_STATE
    s_isr = gpio_isr_handler_add(PINO_C6_CHAMA, isr_c6_chama, nullptr) == ESP_OK;
  }
  gpio_intr_disable(PINO_C6_CHAMA);
}

// Espera o pulso do C6 (nivel alto no G14), olhando o nivel: a borda pode ter
// vindo antes de a espera comecar.
static bool esperar_c6(uint32_t ms)
{
  for (uint32_t t = 0; t < ms; t += 5) {
    if (gpio_get_level(PINO_C6_CHAMA)) { return true; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return gpio_get_level(PINO_C6_CHAMA) != 0;
}

// A linha CMD parada em alto (pull-up dos dois lados); baixar por 2 ms tira o
// C6 do sono. Com o slot desmontado o pino e um GPIO comum aqui.
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
// REMONTAR
// ---------------------------------------------------------------------------
static bool religar(bool pelo_p4)
{
  const uint32_t t0 = tab5_agora_ms();
  gpio_intr_disable(PINO_C6_CHAMA);
  bool chamou = gpio_get_level(PINO_C6_CHAMA) != 0;
  // Foi o C6 quem chamou: o pulso seguinte dele vem em ate 100 ms. Baixar o
  // CMD com o escravo dele ja remontando seria um comando de lixo no SDIO.
  if (!chamou && !pelo_p4) { chamou = esperar_c6(300); }
  for (int i = 0; i < TENTATIVAS && !chamou; ++i) {
    cmd_pulsar();
    chamou = esperar_c6(ESPERA_C6_MS);
  }
  if (!chamou) {
    ESP_LOGE(TAG, "o C6 nao respondeu ao CMD - o enlace fica desmontado; modo automatico desligado");
    ajustes_definir_c6_sono(false);
    ++s_est.falhas;
    s_estado = TAB5_C6_SONO_FALHOU;
    return false;
  }
  // O C6 ja remontou o escravo dele: agora o nosso lado, sem reset.
  gpio_reset_pin(PINO_CMD);
  s_religando = true;
  tab5_sd_controlador_travar();   // o slot 1 volta ao controlador do cartao (ver tab5_sd.hpp)
  int rc = esp_hosted_init();
  if (rc == 0) { rc = esp_hosted_connect_to_slave(); }
  tab5_sd_controlador_soltar();
  s_religando = false;
  s_est.dormindo_ms += tab5_agora_ms() - s_dormiu_em;
  if (rc != 0) {
    ESP_LOGE(TAG, "remontar o SDIO falhou (%d) - o Wi-Fi precisa ser religado; modo automatico desligado", rc);
    ajustes_definir_c6_sono(false);
    ++s_est.falhas;
    s_estado = TAB5_C6_SONO_FALHOU;
    return false;
  }
  if (s_estava_associado) {
    eh_host_wifi_admit_rx(false, true);          // STA: os quadros voltam a chegar a pilha
  }
  // Depois do admit_rx: com a recepcao ja admitida, o rearme nao reposta o
  // STA_CONNECTED (o netif do app segue de pe, sem refazer o DHCP).
  if (s_app_iniciado) {
    s_religando = true;
    rearmar_app();
    s_religando = false;
  }
  if (pelo_p4) { ++s_est.pelo_p4; } else { ++s_est.pelo_c6; }
  s_est.ultimo_religar_ms = tab5_agora_ms() - t0;
  s_acordou_ms = tab5_agora_ms();
  s_estado = TAB5_C6_SONO_ACORDADO;
  ESP_LOGI(TAG, "enlace de volta (%s) em %lu ms", pelo_p4 ? "pedido do P4" : "o C6 chamou",
           (unsigned long)s_est.ultimo_religar_ms);
  return true;
}

// A janela do ESP-NOW no C6 (C6_AJ_ESPNOW_JANELA). So com o enlace de pe.
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
  // Dorme em dois casos: conectado (o C6 dorme associado e o roteador guarda o
  // que chegar) ou com o radio PARADO (Wi-Fi desligado em Ajustes, cochilo,
  // sem rede guardada) - o C6 acordado a toa custava mais que o Wi-Fi ligado:
  // 20,6 mA contra 21,2 no ensaio de 2026-11-13. Nao dorme com o radio
  // trabalhando sem IP (subindo, procurando, so ESP-NOW): ali ele e preciso.
  const tab5_wifi_estado_t w = tab5_wifi_estado();
  const bool conectado    = w == TAB5_WIFI_CONECTADO;
  const bool radio_parado = !tab5_wifi_radio_ativo() && w != TAB5_WIFI_SUBINDO;
  if (!(conectado || radio_parado) || tab5_ble_estado() != TAB5_BLE_DESLIGADO) { return; }
  janela(true);
  char m[96];
  if (!tab5_c6_sono_dormir(m, sizeof(m))) {
    ESP_LOGW(TAG, "automatico: nao desmontei (%s) - tento de novo depois", m);
    s_acordou_ms = agora;   // espera antes da proxima tentativa
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
      // O C6 chamou (G14 alto) ou o P4 quis transmitir: o mesmo religar - ele
      // so baixa o CMD se o C6 ainda nao estiver chamando.
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
  // Primeiro o aviso (pela interrupcao do SDIO): o C6 desmonta o escravo dele
  // na tarefa do esp_hosted. Depois o nosso lado. O C6 so arma o despertar
  // pelo CMD 500 ms depois - os comandos que o desmontar daqui ainda passa
  // pela linha nao o acordam.
  s_estava_associado = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
  s_app_iniciado = tab5_wifi_radio_ativo();   // esp_wifi_start feito: a remontagem rearma (rearmar_app)
  // O IP vai antes: com o P4 dormindo e o C6 quem responde o ARP dele.
  if (esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
    esp_netif_ip_info_t ip = { };
    if (esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr) {
      uint8_t m[5] = { C6_AJ_IP };
      memcpy(m + 1, &ip.ip.addr, 4);
      eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, m, sizeof(m));
    }
  }
  s_p4_quer = false;
  // Do aviso ao fim do deinit: a janela em que o C6 ja pode ter sumido (ver
  // __wrap_eh_host_port_restart_host) e o slot 1 sai do controlador que o
  // cartao divide (ver tab5_sd.hpp). Ordem das travas: s_trava, depois esta.
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
  // A sdio_read estaciona antes do deinit (ver __wrap_eh_host_port_sdio_wait_slave_intr).
  if (!estacionar_leitora(1500)) {
    ESP_LOGW(TAG, "a tarefa de leitura nao estacionou em 1,5 s - desmonto assim mesmo");
  }
  esp_hosted_deinit();   // ao voltar, a tarefa de leitura ja parou
  s_desmontando = false;
  tab5_sd_controlador_soltar();
  gpio_reset_pin(PINO_CMD);
  gpio_set_pull_mode(PINO_CMD, GPIO_PULLUP_ONLY);
  s_dormiu_em = tab5_agora_ms();
  ++s_est.entradas;
  s_estado = TAB5_C6_SONO_DORMINDO;
  ulTaskNotifyTake(pdTRUE, 0);   // sobra de antes
  gpio_intr_enable(PINO_C6_CHAMA);
  xSemaphoreGive(s_trava);
  ESP_LOGI(TAG, "enlace desmontado: o C6 pode dormir");
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
