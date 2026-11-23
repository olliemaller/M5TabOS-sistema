#include "tab5_mesh_ble.hpp"
#include "idioma.h"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "tab5_ble.hpp"
#include "tab5_donos.hpp"
#include "pool_tarefas.h"
#include "parada.hpp"      // cooperative task stop (kernel)
#include "ajustes.h"
#include "notificacao.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "mesh_ble";

// Meshtastic service UUIDs (docs: meshtastic.org, "Bluetooth API").
#define UUID_TORADIO   "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define UUID_FROMRADIO "2c55e69e-4993-11ed-b878-0242ac120002"
#define UUID_FROMNUM   "ed9da18c-a800-4f66-a670-aa7547e34453"

#define CORPO_MAX       512    // the firmware's MAX_TO_FROM_RADIO_SIZE
#define PRAZO_OP_MS   10000    // each GATT operation
#define PRAZO_PAR_MS  30000    // pairing (the node may be showing the PIN)
#define VOLTA_MS       5000    // safety read, even without a fromNum notification

typedef struct { uint16_t n; uint8_t d[CORPO_MAX]; } envio_t;

// ---- state -------------------------------------------------------------------
static TaskHandle_t        s_tarefa = nullptr;
static QueueHandle_t       s_tx = nullptr;
static MessageBufferHandle_t s_rx = nullptr;
static SemaphoreHandle_t   s_op = nullptr;        // end of a GATT/GAP operation
static volatile int        s_op_rc = 0;
static parada_t            s_parada = { };      // stop request / confirmed task exit
static volatile bool       s_conectado = false, s_conectou_agora = false;
static volatile bool       s_ler = false;
static volatile uint16_t   s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool       s_pin_pedido = false;
// PIN ON THE SPOT. Meshtastic has two modes: FIXED PIN (123456 on
// devices without a screen, or whatever was configured) and RANDOM PIN, which the node shows
// on screen ON EVERY pairing - a saved PIN never matches that one. Hence: the
// first attempt uses the PIN from Settings; if the node refuses, the next one is
// INTERACTIVE - the request waits (pairing allows ~30 s) and Settings >
// Meshtastic opens the keyboard to type what the node is showing.
static volatile bool       s_pin_interativo = false;   // next time, ask
static volatile bool       s_pin_esperando = false;    // there is a request waiting for the PIN now
static volatile uint16_t   s_pin_conn = 0;
static int                 s_notif = -1;
static char                s_estado[48] = "parado";
static char                s_nome[24] = "", s_end[18] = "";

static ble_uuid_any_t s_u_svc, s_u_to, s_u_from, s_u_num;
static uint16_t s_svc_ini, s_svc_fim, s_h_to, s_h_from, s_h_num, s_h_cccd;

// The send being written (only the link task uses it) - PSRAM, off the stack.
EXT_RAM_BSS_ATTR static envio_t s_envio;

// Read in progress (NimBLE callback -> task)
static uint8_t  s_lido[CORPO_MAX];
static uint16_t s_lido_n;

// Every state change also goes to the log: a connection attempt that
// fails leaves the whole trail (searching -> connecting -> pairing -> ...).
static void estado(const char* fmt, const char* arg = nullptr)
{
  char novo[sizeof(s_estado)];
  snprintf(novo, sizeof(novo), fmt, arg ? arg : "");
  if (strcmp(novo, s_estado) != 0) { ESP_LOGI(TAG, "%s", novo); }
  snprintf(s_estado, sizeof(s_estado), "%s", novo);
}

static int esperar_op(uint32_t ms)
{
  if (parada_pedida(&s_parada)) { return BLE_HS_ENOTCONN; }   // asked to stop: no new operation
  if (xSemaphoreTake(s_op, pdMS_TO_TICKS(ms)) != pdTRUE) { return BLE_HS_ETIMEOUT; }
  return s_op_rc;
}
static void fim_op(int rc) { s_op_rc = rc; xSemaphoreGive(s_op); }

// ---------------------------------------------------------------------------
// CONNECTION EVENTS (NimBLE task - short, just note and wake)
// ---------------------------------------------------------------------------
static int ao_gap(struct ble_gap_event* ev, void*)
{
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; }
      fim_op(ev->connect.status);
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      s_conectado = false;
      fim_op(BLE_HS_ENOTCONN);   // unblocks whoever was waiting on an operation
      if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
      break;

    case BLE_GAP_EVENT_ENC_CHANGE:
      fim_op(ev->enc_change.status);
      break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      // The PIN. The node says what it wants: ENTER (the usual - it shows one or has a
      // fixed one, 123456 on those without a screen) or CONFIRM a number (we accept).
      struct ble_sm_io io;
      memset(&io, 0, sizeof(io));
      io.action = ev->passkey.params.action;
      if (io.action == BLE_SM_IOACT_INPUT) {
        s_pin_pedido = true;
        if (s_pin_interativo || ajustes_mesh_pin_na_tela()) {
          s_pin_interativo = true;
          // Wait for someone to type it (mesh_ble_digitar_pin): nothing is injected here.
          s_pin_conn = ev->passkey.conn_handle;
          s_pin_esperando = true;
          estado(tr("digite o PIN que o nó mostra"));
          if (s_notif >= 0) { notif_publicar(s_notif, 1, NOTIF_ATENCAO, tr("PIN do Meshtastic")); }
          ESP_LOGI(TAG, "the node asked for the PIN - waiting for the user (Settings > Meshtastic)");
          break;
        }
        io.passkey = ajustes_mesh_pin();
        ESP_LOGI(TAG, "the node asked for the PIN - using the one from Settings");
      } else if (io.action == BLE_SM_IOACT_NUMCMP) {
        io.numcmp_accept = 1;
      }
      ble_sm_inject_io(ev->passkey.conn_handle, &io);
      break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The node forgot the bond (reset) and wants to pair again: we delete
      // ours and let it repeat - otherwise it would keep refusing forever.
      struct ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
        ble_store_util_delete_peer(&d.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
      if (ev->notify_rx.attr_handle == s_h_num) {
        s_ler = true;
        if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
      }
      break;

    default: break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// SYNCHRONOUS GATT - one operation at a time, always from the link task
// ---------------------------------------------------------------------------
static int cb_mtu(uint16_t, const struct ble_gatt_error* e, uint16_t mtu, void*)
{
  if (e->status == 0) { ESP_LOGI(TAG, "MTU %u", (unsigned)mtu); }
  fim_op(e->status);
  return 0;
}

static int cb_svc(uint16_t, const struct ble_gatt_error* e, const struct ble_gatt_svc* s, void*)
{
  if (e->status == 0) { s_svc_ini = s->start_handle; s_svc_fim = s->end_handle; return 0; }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_chr(uint16_t, const struct ble_gatt_error* e, const struct ble_gatt_chr* c, void*)
{
  if (e->status == 0) {
    if      (ble_uuid_cmp(&c->uuid.u, &s_u_to.u)   == 0) { s_h_to   = c->val_handle; }
    else if (ble_uuid_cmp(&c->uuid.u, &s_u_from.u) == 0) { s_h_from = c->val_handle; }
    else if (ble_uuid_cmp(&c->uuid.u, &s_u_num.u)  == 0) { s_h_num  = c->val_handle; }
    return 0;
  }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_dsc(uint16_t, const struct ble_gatt_error* e, uint16_t chr_val,
                  const struct ble_gatt_dsc* d, void*)
{
  if (e->status == 0) {
    if (chr_val == s_h_num && !s_h_cccd
     && ble_uuid_u16(&d->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
      s_h_cccd = d->handle;
    }
    return 0;
  }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_ler(uint16_t, const struct ble_gatt_error* e, struct ble_gatt_attr* a, void*)
{
  if (e->status == 0 && a) {
    const uint16_t n = OS_MBUF_PKTLEN(a->om);
    if (a->offset + n <= CORPO_MAX) {
      os_mbuf_copydata(a->om, 0, n, s_lido + a->offset);
      s_lido_n = a->offset + n;
    }
    return 0;
  }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_escrever(uint16_t, const struct ble_gatt_error* e, struct ble_gatt_attr*, void*)
{
  fim_op(e->status);
  return 0;
}

static bool conexao_viva(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static int escrever(uint16_t h, const uint8_t* d, size_t n)
{
  if (!conexao_viva()) { return BLE_HS_ENOTCONN; }
  xSemaphoreTake(s_op, 0);
  const uint16_t mtu = ble_att_mtu(s_conn);
  int rc;
  if (n + 3 <= mtu) {
    rc = ble_gattc_write_flat(s_conn, h, d, (uint16_t)n, cb_escrever, nullptr);
  } else {
    struct os_mbuf* om = ble_hs_mbuf_from_flat(d, (uint16_t)n);
    rc = om ? ble_gattc_write_long(s_conn, h, 0, om, cb_escrever, nullptr) : BLE_HS_ENOMEM;
  }
  return rc ? rc : esperar_op(PRAZO_OP_MS);
}

// FromRadio: one message per read; an empty read = the node has nothing more.
// Up to 64 at a time (the send queue cannot wait for the whole dump). Returns
// false when it stopped at the ceiling WITHOUT draining: the rest comes on the next round,
// immediately - before, it waited for the next fromNum notification (which only comes with a
// NEW packet) or the 5 s safety read, and a dump of many nodes went along in
// fits and starts.
static bool ler_ate_vazio(void)
{
  for (int k = 0; k < 64 && conexao_viva() && !parada_pedida(&s_parada); ++k) {
    xSemaphoreTake(s_op, 0);
    s_lido_n = 0;
    if (ble_gattc_read_long(s_conn, s_h_from, 0, cb_ler, nullptr) != 0) { return true; }
    if (esperar_op(PRAZO_OP_MS) != 0) { return true; }
    if (s_lido_n == 0) { return true; }
    if (xMessageBufferSend(s_rx, s_lido, s_lido_n, pdMS_TO_TICKS(500)) == 0) {
      ESP_LOGW(TAG, "the Mesh service is not consuming - message of %u bytes lost",
               (unsigned)s_lido_n);
    }
  }
  return !conexao_viva() || parada_pedida(&s_parada);
}

// ---------------------------------------------------------------------------
// FIND, CONNECT, PAIR, DISCOVER
// ---------------------------------------------------------------------------
static bool achar_no(ble_addr_t* alvo)
{
  char salvo[18];
  ajustes_mesh_ble_endereco(salvo);
  uint8_t quer[6];
  const bool tem_salvo = tab5_ble_endereco_ler(salvo, quer);

  estado(tem_salvo ? tr("procurando %s") : tr("procurando um no Meshtastic"), salvo);
  // If the screen is already scanning, we piggyback on its scan.
  // 8 s: with Wi-Fi connected the C6 shares the radio, and the search hears less.
  if (!tab5_ble_varrendo() && !tab5_ble_varrer(8000)) { return false; }
  while (tab5_ble_varrendo() && !parada_pedida(&s_parada)) { vTaskDelay(pdMS_TO_TICKS(200)); }

  int melhor = -1;
  int8_t rssi = -128;
  tab5_ble_visto_t v;
  const uint32_t agora = tab5_agora_ms();
  for (int i = 0; i < tab5_ble_n_vistos(); ++i) {
    if (!tab5_ble_visto(i, &v) || agora - v.visto_ms > 15000) { continue; }
    if (tem_salvo ? (memcmp(v.addr, quer, 6) == 0) : (v.meshtastic && v.rssi > rssi)) {
      melhor = i; rssi = v.rssi;
      if (tem_salvo) { break; }
    }
  }
  if (melhor < 0) { return false; }
  tab5_ble_visto(melhor, &v);
  alvo->type = v.tipo;
  memcpy(alvo->val, v.addr, 6);
  snprintf(s_nome, sizeof(s_nome), "%s", v.nome[0] ? v.nome : "Meshtastic");
  tab5_ble_endereco_txt(v.addr, s_end);
  return true;
}

// 0 = connected and ready; <0 = try again soon; >0 = wait longer (PIN)
static int conectar(const ble_addr_t* alvo)
{
  uint8_t proprio;
  if (ble_hs_id_infer_auto(0, &proprio) != 0) { return -1; }

  estado("conectando a %s", s_nome);
  xSemaphoreTake(s_op, 0);
  int rc = ble_gap_connect(proprio, alvo, 10000, nullptr, ao_gap, nullptr);
  if (rc != 0) { ESP_LOGW(TAG, "ble_gap_connect: %d", rc); return -1; }
  rc = esperar_op(12000);
  if (rc != 0 || !conexao_viva()) {
    ESP_LOGW(TAG, "did not connect (%d)", rc);
    ble_gap_conn_cancel();
    return -1;
  }

  // This connection's addresses: what the stack thinks is ours (id), what
  // actually went on air (ota) and the node's. The final check of secure
  // pairing includes the addresses - if id and ota diverge, that is where it breaks.
  {
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) == 0) {
      char a1[18], a2[18], a3[18];
      tab5_ble_endereco_txt(d.our_id_addr.val, a1);
      tab5_ble_endereco_txt(d.our_ota_addr.val, a2);
      tab5_ble_endereco_txt(d.peer_ota_addr.val, a3);
      ESP_LOGI(TAG, "addresses: our id %s (type %u), on air %s (type %u), node %s (type %u)",
               a1, d.our_id_addr.type, a2, d.our_ota_addr.type, a3, d.peer_ota_addr.type);
    }
  }

  xSemaphoreTake(s_op, 0);
  if (ble_gattc_exchange_mtu(s_conn, cb_mtu, nullptr) == 0) { esperar_op(PRAZO_OP_MS); }

  // Pairing. Already bonded: just resumes encryption, no PIN. No bond: the node
  // asks for the PIN (see PASSKEY_ACTION).
  estado(tr("pareando com %s"), s_nome);
  s_pin_pedido = false;
  xSemaphoreTake(s_op, 0);
  rc = ble_gap_security_initiate(s_conn);
  if (rc == 0) { rc = esperar_op(PRAZO_PAR_MS); }
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    struct ble_gap_conn_desc d;
    const bool achou = conexao_viva() && ble_gap_conn_find(s_conn, &d) == 0;
    if (conexao_viva()) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    if (s_pin_esperando) {
      s_pin_esperando = false;
      if (s_notif >= 0) { notif_limpar(s_notif); }
    }
    // DHKEY CHECK FAILED: the 20 PIN rounds passed (the PIN was right),
    // but the final check of secure pairing (LE Secure Connections)
    // did not match. LEGACY mode with PIN has no such step, and Meshtastic
    // accepts it: switch to it and ask for the PIN again now (the node shows another).
    if ((rc == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_DHKEY) || rc == BLE_HS_SM_US_ERR(BLE_SM_ERR_DHKEY))
     && ble_hs_cfg.sm_sc) {
      ESP_LOGW(TAG, "final check of secure pairing failed (%d) - trying legacy pairing", rc);
      ble_hs_cfg.sm_sc = 0;
      s_pin_interativo = true;
      estado(tr("tentando outro modo de pareamento - o no vai mostrar outro PIN"));
      return -1;
    }
    // THE SAVED PIN WAS REFUSED: the node shows a new PIN on every pairing.
    // Remember that (NVS) and try again now - this time asking for the PIN.
    if (s_pin_pedido && !s_pin_interativo) {
      ESP_LOGW(TAG, "saved PIN refused (%d) - the node uses an on-screen PIN; asking from now on", rc);
      ajustes_definir_mesh_pin_na_tela(true);
      ajustes_definir_mesh_pin(123456);   // the saved one was wrong: back to the default
      s_pin_interativo = true;
      estado(tr("o nó mostra o PIN na tela - tentando de novo"));
      return -1;
    }
    if (s_pin_pedido) {
      // Wrong PIN (or the timeout expired). Does not retry by itself: each attempt
      // bothers the node and generates a new PIN on it. The next one asks for the PIN on the
      // spot, and only starts when the user asks (mesh_ble_tentar_de_novo, which
      // the PIN's SEND in Settings > Meshtastic calls).
      ESP_LOGW(TAG, "pairing refused (%d) - wrong PIN or timeout expired", rc);
      estado(tr("PIN recusado - toque no nó para tentar"));
      s_pin_interativo = true;
      return 2;
    }
    // No PIN requested and it still failed: the stored bond is no longer valid
    // (the node was reset). Delete it and start from scratch on the next round.
    if (achou) { ble_store_util_delete_peer(&d.peer_id_addr); }
    ESP_LOGW(TAG, "encryption refused (%d) - old bond deleted", rc);
    return -1;
  }

  // Discovery of the service and the three characteristics.
  s_svc_ini = s_svc_fim = s_h_to = s_h_from = s_h_num = s_h_cccd = 0;
  xSemaphoreTake(s_op, 0);
  rc = ble_gattc_disc_svc_by_uuid(s_conn, &s_u_svc.u, cb_svc, nullptr);
  if (rc == 0) { rc = esperar_op(PRAZO_OP_MS); }
  if (rc == 0 && s_svc_ini) {
    xSemaphoreTake(s_op, 0);
    rc = ble_gattc_disc_all_chrs(s_conn, s_svc_ini, s_svc_fim, cb_chr, nullptr);
    if (rc == 0) { rc = esperar_op(PRAZO_OP_MS); }
  }
  if (rc == 0 && s_h_num) {
    xSemaphoreTake(s_op, 0);
    rc = ble_gattc_disc_all_dscs(s_conn, s_h_num, s_svc_fim, cb_dsc, nullptr);
    if (rc == 0) { rc = esperar_op(PRAZO_OP_MS); }
  }
  if (rc != 0 || !s_h_to || !s_h_from) {
    ESP_LOGW(TAG, "the device does not have the complete Meshtastic service (%d)", rc);
    if (conexao_viva()) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    return -1;
  }

  // Subscribes to fromNum. Without it, it still works (the VOLTA_MS round reads
  // anyway), just more slowly.
  if (s_h_cccd) {
    const uint8_t liga[2] = { 0x01, 0x00 };
    if (escrever(s_h_cccd, liga, 2) != 0) { ESP_LOGW(TAG, "could not subscribe to fromNum"); }
  }

  s_pin_interativo = false;
  if (s_notif >= 0) { notif_limpar(s_notif); }

  // From this node on it is this node: save it to reconnect directly.
  ajustes_definir_mesh_ble_endereco(s_end);
  ESP_LOGI(TAG, "connected to %s (%s)", s_nome, s_end);
  estado(tr("conectado a %s"), s_nome);
  s_conectado = true;
  s_conectou_agora = true;
  return 0;
}

// ---------------------------------------------------------------------------
// THE LINK TASK
// ---------------------------------------------------------------------------
static void vTaskMeshBle(void*)
{
  uint32_t espera = 0;
  for (;;) {
    if (parada_pedida(&s_parada)) { break; }
    if (espera) {
      ulTaskNotifyTake(pdTRUE, espera == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(espera));
      espera = 0;
      continue;
    }

    if (!tab5_ble_pronto()) {
      estado(tab5_ble_estado() == TAB5_BLE_FALHOU ? tr("Bluetooth falhou") : tr("esperando o Bluetooth"));
      espera = 1000;
      continue;
    }

    ble_addr_t alvo;
    if (!achar_no(&alvo)) {
      estado(tr("nenhum Meshtastic por perto"));
      espera = 4000;
      continue;
    }
    const int rc = conectar(&alvo);
    if (rc != 0) {
      // The connection drop wakes this task (see DISCONNECT in ao_gap) - without
      // draining that notification, the wait ended at once and the next search ran
      // while the node had not started advertising again yet.
      vTaskDelay(pdMS_TO_TICKS(500));
      ulTaskNotifyTake(pdTRUE, 0);
      espera = (rc > 1) ? portMAX_DELAY : (rc > 0) ? 60000 : 3000;
      continue;
    }

    s_ler = true;   // whatever the node already had queued
    TickType_t ultima = xTaskGetTickCount();
    while (conexao_viva() && !parada_pedida(&s_parada)) {
      // Everything that matters WAKES this task: the fromNum notification, a send
      // in the queue, the connection dropping and the stop request (xTaskNotifyGive).
      // The timeout is just the safety read (VOLTA_MS) - it was 1 s, five
      // pointless wake-ups for each useful one with the node connected.
      ulTaskNotifyTake(pdTRUE, s_ler ? 0 : pdMS_TO_TICKS(VOLTA_MS));
      while (xQueueReceive(s_tx, &s_envio, 0) == pdTRUE) {
        if (escrever(s_h_to, s_envio.d, s_envio.n) != 0) { ESP_LOGW(TAG, "write to toRadio failed"); }
        s_ler = true;   // the answer comes via fromRadio
      }
      if (s_ler || (xTaskGetTickCount() - ultima) > pdMS_TO_TICKS(VOLTA_MS)) {
        s_ler = false;
        ultima = xTaskGetTickCount();
        if (!ler_ate_vazio()) { s_ler = true; }
      }
    }
    s_conectado = false;
    if (!parada_pedida(&s_parada)) { estado(tr("conexão caiu - reconectando")); espera = 2000; }
  }

  if (conexao_viva()) {
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int k = 0; k < 20 && conexao_viva(); ++k) { vTaskDelay(pdMS_TO_TICKS(50)); }
  }
  s_conectado = false;
  estado("parado");
  parada_confirmar(&s_parada);
  // Returns: the pool slot gives back the owner and goes back to sleep (kernel/pool_tarefas.h).
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
bool mesh_ble_iniciar(void)
{
  if (parada_rodando(&s_parada)) { return true; }
  if (!s_op) {
    s_op = xSemaphoreCreateBinary();
    s_tx = xQueueCreateWithCaps(4, sizeof(envio_t), MALLOC_CAP_SPIRAM);
    s_rx = xMessageBufferCreateWithCaps(8192, MALLOC_CAP_SPIRAM);
    ble_uuid_from_str(&s_u_svc,  TAB5_BLE_UUID_MESHTASTIC);
    ble_uuid_from_str(&s_u_to,   UUID_TORADIO);
    ble_uuid_from_str(&s_u_from, UUID_FROMRADIO);
    ble_uuid_from_str(&s_u_num,  UUID_FROMNUM);
  }
  if (!s_op || !s_tx || !s_rx) { ESP_LOGE(TAG, "out of memory"); return false; }
  if (s_notif < 0) { s_notif = notif_registrar("Bluetooth"); notif_associar_app(s_notif, "Mesh"); }
  xQueueReset(s_tx);
  xMessageBufferReset(s_rx);
  s_conectado = false; s_conectou_agora = false;
  s_nome[0] = s_end[0] = 0;
  if (!parada_preparar(&s_parada)) { return false; }
  pool_id_t id;
  if (!pool_criar(&id, vTaskMeshBle, nullptr, "mesh_ble", 4096, 3, DONO_SERVICO, "Mesh BLE")) {
    parada_desistir(&s_parada);
    return false;
  }
  s_tarefa = id.handle;
  return true;
}

// The task sleeps on its OWN waits (notification, end of a BLE stack
// operation): the signal is REPEATED while waiting. A single one could be swallowed by the
// xSemaphoreTake(s_op, 0) that precedes each operation, and the task stayed stuck for
// the 12 s of connecting (or the ~30 s of the PIN) - shutdown went ahead without it and the
// stack vanished under an ongoing ble_gap_*.
static void cutucar_tarefa(void)
{
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
  fim_op(BLE_HS_ENOTCONN);
}

bool mesh_ble_parar(void)
{
  if (!s_tarefa) { return true; }
  if (!parada_pedir(&s_parada, 6000, cutucar_tarefa)) {
    ESP_LOGE(TAG, "the link task did not exit within 6 s");
    return false;
  }
  s_tarefa = nullptr;
  return true;
}

bool mesh_ble_enviar(const uint8_t* corpo, size_t n)
{
  if (!s_conectado || !s_tx || !corpo || n == 0 || n > CORPO_MAX) { return false; }
  envio_t* e = (envio_t*)heap_caps_malloc(sizeof(envio_t), MALLOC_CAP_SPIRAM);
  if (!e) { return false; }
  e->n = (uint16_t)n;
  memcpy(e->d, corpo, n);
  const bool ok = xQueueSend(s_tx, e, pdMS_TO_TICKS(200)) == pdTRUE;
  heap_caps_free(e);
  if (ok && s_tarefa) { xTaskNotifyGive(s_tarefa); }
  return ok;
}

size_t mesh_ble_receber(uint8_t* buf, size_t max, uint32_t espera_ms)
{
  if (!s_rx) { vTaskDelay(pdMS_TO_TICKS(espera_ms)); return 0; }
  return xMessageBufferReceive(s_rx, buf, max, pdMS_TO_TICKS(espera_ms));
}

bool mesh_ble_conectou_agora(void)
{
  if (!s_conectou_agora) { return false; }
  s_conectou_agora = false;
  return true;
}

bool        mesh_ble_conectado(void) { return s_conectado; }
const char* mesh_ble_descricao(void) { return s_estado; }

bool mesh_ble_rodando(void) { return parada_rodando(&s_parada); }

bool mesh_ble_esperando_pin(void) { return s_pin_esperando; }

bool mesh_ble_digitar_pin(uint32_t pin)
{
  if (!s_pin_esperando) { return false; }
  s_pin_esperando = false;
  if (s_notif >= 0) { notif_limpar(s_notif); }
  struct ble_sm_io io;
  memset(&io, 0, sizeof(io));
  io.action = BLE_SM_IOACT_INPUT;
  io.passkey = pin;
  estado("conferindo o PIN...");
  return ble_sm_inject_io(s_pin_conn, &io) == 0;
}

void mesh_ble_tentar_de_novo(void)
{
  if (parada_rodando(&s_parada)) { xTaskNotifyGive(s_tarefa); }
}
