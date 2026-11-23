#include "recursos.h"
#include "tab5_farejo.hpp"
#include "tab5_ouvinte.hpp"
#include "tab5_wifi.hpp"
#include "tab5_hora.hpp"      // tab5_agora_ms
#include "pool_tarefas.h"
#include "parada.hpp"      // request/confirm the task's exit (the same as GPS and Mesh)
#include "tab5_mutex.hpp"
#include "eh_host_feat_peer_data.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <atomic>

static const char* TAG = "farejo";

// The C6 switches off by itself after C6_FAREJO_VALIDADE_MS without renewal. Renewing every
// 3 s survives two lost requests in a row.
#define RENOVA_MS 3000

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx = nullptr;
static StaticSemaphore_t s_info_buf;
static SemaphoreHandle_t s_info_sem = nullptr;

static c6_farejo_cfg_t s_cfg;          // the request in force (under s_mtx)
static volatile bool   s_quer = false; // the P4 wants to sniff
// SWITCHING ON AND OFF, WITHOUT A RACE (the same as CSI). Previously, switching on right
// after switching off found 's_rodando' still up, didn't set s_quer back to
// true, and the task that was exiting switched sniffing off and FREED THE TABLE
// after 'ligar' had returned true. Now switching off REQUESTS the exit
// (parada_t) and switching on with a run exiting waits for it to confirm before
// allocating the table and creating the new one.
static parada_t          s_parada = { };
static Trava s_trava_cmd;
static pool_id_t       s_id = POOL_ID_VAZIO;

static farejo_item_t* s_tab = nullptr; // PSRAM, only with sniffing on
static int            s_n = 0;
static std::atomic<uint32_t> s_lotes = 0, s_perdidos = 0;

static volatile uint8_t s_proto = 0, s_caps = 0;   // 0 = don't know yet
static c6_info_t s_info;
static volatile bool s_tem_info = false;
static bool s_registrado = false;

static void iniciar_travas(void)
{
  if (!s_mtx) {
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    s_info_sem = xSemaphoreCreateBinaryStatic(&s_info_buf);
  }
}

// ---------------------------------------------------------------------------
// Coming from the C6 - in esp_hosted's event task: just gather and leave.
// ---------------------------------------------------------------------------
static int achar_mac(const uint8_t mac[6])   // under s_mtx
{
  for (int i = 0; i < s_n; ++i) {
    if (memcmp(s_tab[i].mac, mac, 6) == 0) { return i; }
  }
  return -1;
}

static int vaga_para(const uint8_t mac[6])   // under s_mtx; full: the oldest
{
  int j = achar_mac(mac);
  if (j >= 0) { return j; }
  if (s_n < FAREJO_MAX) { return s_n++; }
  int pior = 0;
  for (int i = 1; i < s_n; ++i) {
    if (s_tab[i].visto_ms < s_tab[pior].visto_ms) { pior = i; }
  }
  memset(&s_tab[pior], 0, sizeof(s_tab[pior]));
  return pior;
}

static tab5_ouvinte_t<tab5_farejo_ouvinte_t> s_ouvinte;   // removing waits for the in-flight call

static void juntar_lote(const uint8_t* d, size_t n)
{
  if (n < sizeof(c6_farejo_lote_t)) { return; }
  c6_farejo_lote_t cab;
  memcpy(&cab, d, sizeof(cab));
  const uint8_t* reg = d + sizeof(cab);
  int k = (int)((n - sizeof(cab)) / sizeof(c6_farejo_reg_t));
  if (k > cab.n) { k = cab.n; }
  const uint32_t agora = tab5_agora_ms();

  // The batch arrives when the period CLOSES: its mean RSSI is from the middle of the
  // period. (The C6 -> P4 path takes a few ms; the period takes 100+.)
  if (s_ouvinte.fn.load()) {
    uint16_t lote_ms = s_cfg.lote_ms ? s_cfg.lote_ms : 250;
    if (lote_ms < 50) { lote_ms = 50; }
    const int64_t t = esp_timer_get_time() - (int64_t)lote_ms * 500;
    for (int i = 0; i < k; ++i) {
      c6_farejo_reg_t g;
      memcpy(&g, reg + i * sizeof(g), sizeof(g));
      tab5_ouvinte_chamar(&s_ouvinte, (const c6_farejo_reg_t*)&g, t);
    }
  }

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) {
    for (int i = 0; i < k; ++i) {
      c6_farejo_reg_t g;
      memcpy(&g, reg + i * sizeof(g), sizeof(g));   // reg may be unaligned
      int j = vaga_para(g.mac);
      farejo_item_t* it = &s_tab[j];
      memcpy(it->mac, g.mac, 6);
      it->rssi_ult = g.rssi_ult;
      it->rssi_max = g.rssi_max;
      it->rssi_med = g.rssi_med;
      it->tipos   |= g.tipos;
      it->canal    = g.canal;
      it->quadros += g.n;
      it->visto_ms = agora;
    }
    s_perdidos += cab.perdidos;
  }
  xSemaphoreGive(s_mtx);
  ++s_lotes;
}

static void ao_peer_data(uint32_t msg_id, const uint8_t* dados, size_t n, void*)
{
  switch (msg_id) {
    case C6_MSG_FAREJO:
      juntar_lote(dados, n);
      break;
    case C6_MSG_INFO:
      if (n >= C6_INFO_V1_TAM) {
        // An older C6 sends the short INFO (without the diagnostics): the rest stays zero.
        memset((void*)&s_info, 0, sizeof(c6_info_t));
        memcpy((void*)&s_info, dados, n < sizeof(c6_info_t) ? n : sizeof(c6_info_t));
        s_proto = s_info.proto;
        s_caps = s_info.caps;
        s_tem_info = true;
        if (s_info_sem) { xSemaphoreGive(s_info_sem); }
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Renewal - a pool slot, only while on.
// ---------------------------------------------------------------------------
static void vTarefaRenova(void*)
{
  while (!parada_pedida(&s_parada)) {
    c6_farejo_cfg_t cfg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cfg = s_cfg;
    xSemaphoreGive(s_mtx);
    cfg.liga = 1;
    eh_host_peer_data_send(C6_MSG_HOST_FAREJO, (const uint8_t*)&cfg, sizeof(cfg));

    // Sleeps until the next renewal; switching off (or a new config) wakes it at once.
    parada_dormir(&s_parada, RENOVA_MS);
  }

  // Switches off on the C6 and releases the table.
  c6_farejo_cfg_t off = { 0 };
  eh_host_peer_data_send(C6_MSG_HOST_FAREJO, (const uint8_t*)&off, sizeof(off));

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) { heap_caps_free(s_tab); s_tab = nullptr; }
  s_n = 0;
  xSemaphoreGive(s_mtx);

  sistema_recurso(RECURSO_RADIO_ESCUTA, "farejo", false);
  ESP_LOGI(TAG, "sniffing off");
  parada_confirmar(&s_parada);   // the LAST thing: from here on a 'ligar' may create another
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
static void registrar(void)
{
  if (s_registrado) { return; }
  eh_host_peer_data_register(C6_MSG_FAREJO, ao_peer_data, nullptr);
  eh_host_peer_data_register(C6_MSG_INFO,   ao_peer_data, nullptr);
  s_registrado = true;
}

bool tab5_farejo_c6_suporta(void)
{
  return (s_caps & C6_CAP_FAREJO) != 0;
}

uint8_t tab5_farejo_caps_c6(void) { return s_caps; }

void tab5_farejo_ouvinte(tab5_farejo_ouvinte_t cb) { tab5_ouvinte_definir(&s_ouvinte, cb); }

bool tab5_farejo_ligar(const c6_farejo_cfg_t* cfg)
{
  if (!cfg) { return false; }
  iniciar_travas();
  if (!tab5_wifi_c6_nosso() || !tab5_wifi_c6_enlace()) { return false; }
  registrar();

  s_trava_cmd.travar();
  const bool vivo = parada_rodando(&s_parada);
  if (vivo && parada_pedida(&s_parada) && !parada_pedir(&s_parada, 1500)) {
    // Exiting and hasn't exited: switching on now would share the table with whoever frees it.
    s_trava_cmd.destravar();
    ESP_LOGE(TAG, "the previous sniffing task hasn't exited yet - not switched on");
    return false;
  }
  const bool de_pe = vivo && parada_rodando(&s_parada);   // wasn't exiting

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_cfg = *cfg;
  s_cfg.liga = 1;
  if (!s_tab) {
    s_tab = (farejo_item_t*)heap_caps_calloc(FAREJO_MAX, sizeof(farejo_item_t),
                                             MALLOC_CAP_SPIRAM);
    s_n = 0;
  }
  const bool sem_tab = (s_tab == nullptr);
  xSemaphoreGive(s_mtx);
  if (sem_tab) { s_trava_cmd.destravar(); ESP_LOGE(TAG, "no PSRAM for the table"); return false; }

  // Receiver always on: promiscuous mode loses whatever arrives during a doze.
  sistema_recurso(RECURSO_RADIO_ESCUTA, "farejo", true);

  s_quer = true;
  if (!de_pe) {
    if (!parada_preparar(&s_parada)
     || !pool_criar(&s_id, vTarefaRenova, nullptr, "farejo", 3072, 3,
                    DONO_SERVICO, "Farejo")) {
      parada_desistir(&s_parada);
      s_quer = false;
      sistema_recurso(RECURSO_RADIO_ESCUTA, "farejo", false);
      s_trava_cmd.destravar();
      ESP_LOGE(TAG, "no slot for the sniffing task");
      return false;
    }
  } else {
    // Already running: wake it from its doze and the next round sends the new config.
    // (It used to be pool_acordar = xTaskAbortDelay, which also yanks the task
    // out of an xSemaphoreTake(s_mtx) - the risk described in kernel/parada.hpp.)
    parada_cutucar(&s_parada);
  }
  s_trava_cmd.destravar();
  ESP_LOGI(TAG, "sniffing on: mask 0x%04x, batch %u ms",
           (unsigned)cfg->canais, (unsigned)cfg->lote_ms);
  return true;
}

void tab5_farejo_desligar(void)
{
  s_trava_cmd.travar();
  s_quer = false;
  // Requests the exit and doesn't wait (the one switching off is the screen): the task exits on the next
  // round, releases the table and confirms.
  if (parada_rodando(&s_parada)) { s_parada.pedido = true; parada_cutucar(&s_parada); }
  s_trava_cmd.destravar();
}

bool tab5_farejo_ativo(void) { return s_quer; }

int tab5_farejo_n(void)
{
  if (!s_mtx) { return 0; }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  int n = s_n;
  xSemaphoreGive(s_mtx);
  return n;
}

bool tab5_farejo_item(int i, farejo_item_t* out)
{
  if (!s_mtx || !out) { return false; }
  bool ok = false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab && i >= 0 && i < s_n) { *out = s_tab[i]; ok = true; }
  xSemaphoreGive(s_mtx);
  return ok;
}

uint32_t tab5_farejo_lotes(void)    { return s_lotes; }
uint32_t tab5_farejo_perdidos(void) { return s_perdidos; }

void tab5_farejo_limpar(void)
{
  if (!s_mtx) { return; }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) { memset(s_tab, 0, FAREJO_MAX * sizeof(farejo_item_t)); }
  s_n = 0;
  xSemaphoreGive(s_mtx);
  s_perdidos = 0;
}

bool tab5_farejo_info_c6(c6_info_t* out, uint32_t espera_ms)
{
  if (!out) { return false; }
  iniciar_travas();
  if (!tab5_wifi_c6_nosso() || !tab5_wifi_c6_enlace()) { return false; }
  registrar();
  s_tem_info = false;
  if (s_info_sem) { xSemaphoreTake(s_info_sem, 0); }   // drain it
  const uint8_t chave = C6_AJ_INFO;
  uint8_t pedido[2] = { C6_AJ_INFO, 0 };
  (void)chave;
  eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, pedido, 1);
  if (s_info_sem && xSemaphoreTake(s_info_sem, pdMS_TO_TICKS(espera_ms)) == pdTRUE) {
    memcpy(out, (const void*)&s_info, sizeof(c6_info_t));
    return true;
  }
  return false;
}

void tab5_farejo_anotar_pronto(const uint8_t* dados, uint16_t n)
{
  // PRONTO v2: 6B MAC + 1B channel + 1B proto + 1B caps. Old: only 7 bytes.
  if (n >= 9) { s_proto = dados[7]; s_caps = dados[8]; }
  else        { s_proto = 1; s_caps = C6_CAP_ESPNOW; }
}
