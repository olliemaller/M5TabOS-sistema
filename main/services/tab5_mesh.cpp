// Mesh service (Meshtastic) - the engine: the link with the radio (C6L over Grove, over
// Bluetooth or internet only), the task, sending and the send queue, the in-flight
// sends and the outcome written to the SD card, and the public API (tab5_mesh.hpp).
//
// The service was split by subject; what the parts share lives in
// mesh/tab5_mesh_interno.hpp:
//   mesh/tab5_mesh_hora.cpp       corrected timestamp, what time it is, live time
//   mesh/tab5_mesh_nos.cpp        node table, favourites, search, name cache, NodeDB on the SD card
//   mesh/tab5_mesh_conversas.cpp  conversations and history on the SD card, deleted ones, writing, column order
//   mesh/tab5_mesh_canais.cpp     the node's channels
//   mesh/tab5_mesh_receber.cpp    interpreting messages from the radio
//   mesh/tab5_mesh_gps.cpp        GPS position for the mesh

#include "app.h"   // sistema_avisar
#include "idioma.h"
#include "esp_mac.h"   // numero_do_tab5
#include "tab5_donos.hpp"
#include "pool_tarefas.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "parada.hpp"      // cooperative task stop (kernel)
#include "driver/uart.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "tab5_grove.hpp"
#include "notificacao.h"
#include "ajustes.h"
#include "tab5_mesh_ble.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_mesh_admin.hpp"
#include "tab5_mesh_diario.hpp"
#include "tab5_mesh_wifi_no.hpp"
#include "mesh_pb.hpp"
#include "tab5_wifi.hpp"
#include "tab5_hora.hpp"
#include "nvs.h"
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

#define MESH_UART   UART_NUM_1
#define MESH_RX     54
#define MESH_TX     53
#define MESH_BAUD   115200

// With no frame for this long, we consider the link down.
// HOW MUCH SILENCE BEFORE DECLARING THE LINK MUTE.
//
// It was 30s, and 30s is LESS than a quiet mesh takes to speak. The radio doesn't
// say anything of its own accord: its telemetry goes out every half hour, and if
// nobody transmits anything no frame arrives at all. In other words, on a quiet mesh the
// link spent most of its time marked as "mute" - with the C6L
// perfectly connected. And "mute" had consequences: it fired a configuration
// request every 10s (see the task), and each request RESTARTS the whole download
// on the radio. We were stuck in a cycle of requesting configuration forever, and
// while the dump runs live traffic waits in the radio's queue - which is
// small and discards the excess. A message arriving in that window vanishes.
//
// 90s: still notices a yanked cable in a little over a minute, and no longer
// confuses "the mesh is quiet" with "the radio has gone".
#define SILENCIO_MS 90000

// ---------------------------------------------------------------------------
// Protobuf: services/mesh_pb.hpp (the same codec as the MQTT bridge and admin)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Trava s_trava;

// SERVICE LIFETIME. Until now it only knew how to be born: once up, it held the Grove
// UART forever. Swapping the C6L for the GPS on the cable achieved nothing - the
// new module was there, mute, with the port in the hands of someone who had already left.
//
// COOPERATIVE stop (kernel/parada.hpp), and not vTaskDelete from outside: the
// task takes a mutex and writes to the SD card, and killing it in the middle of that would leave the
// mutex held and the file open. The requester marks the request; the task finishes
// what it was doing, confirms, and only then does the clean-up happen.
static parada_t      s_parada = { };
bool          s_servico_ativo = false;  // there is a task and the link is up
// WHICH WAY: the Grove cable or Bluetooth. Fixed at iniciar, by the
// preference (ajustes_mesh_via) - only changes by stopping and starting again.
uint8_t       s_via = AJUSTES_MESH_SERIAL;
EXT_RAM_BSS_ATTR mesh_no_t   s_nos[MESH_MAX_NOS];
uint32_t    s_despejos = 0;   // how many times achar_no() evicted the oldest since boot
EXT_RAM_BSS_ATTR mesh_msg_t  s_msgs[MESH_MAX_MSGS];
int         s_n_msgs = 0;
// A REAL TOTAL, NEVER SATURATES. Symptom: a new message only appeared after
// switching screen and coming back. app_mesh.cpp detected
// "a new message arrived" by comparing tab5_mesh_num_msgs() (which returns
// s_n_msgs) against the value from the last pass - but s_n_msgs is the size of the
// 8-slot RING (MESH_MAX_MSGS), which stops growing as soon as 8
// messages (from ANY conversation) have passed through it. After that the
// number stays still forever, and the comparator never saw a difference again -
// only switching screen forced other paths (s_hist_pronto=false directly) to
// reload. This counter only grows, never resets at MESH_MAX_MSGS.
uint32_t     s_total_msgs = 0;
int         s_nao_lidas = 0;
int         s_nao_lidas_dir = 0;
mesh_saude_t s_saude = { };
uint32_t    s_meu_num = 0;
char        s_firmware[24] = { 0 };
uint32_t    s_ultimo_quadro_ms = 0;
static uint32_t    s_iniciado_em_ms   = 0;   // see the notice in tab5_mesh_silencio_ms()
int         s_notif = -1;   // source in the notification service

// REAL CHANNEL NAMES, as told by the radio itself.
//
// Previously channel 0 was "LongFast" hard-coded and the rest became "Canal 1",
// "Canal 2". But Meshtastic SENDS the channel configuration in the initial dump
// (FromRadio field 10) - with the name you chose on the phone. Ignoring that
// was inventing a label when the right one was arriving over serial.
char s_canal_nome[MESH_MAX_CANAIS][MESH_NOME_CONVERSA];

// WHETHER THE CHANNEL IS ACTIVE NOW, per the LAST config the radio sent - born
// together with "available channels" (see further down). Before this
// nothing kept that state: s_canal_nome[] was only WRITTEN when the role
// came != 0 (see the field 10 parser below), but it never found out that
// a channel had been SWITCHED OFF - the old name just sat there,
// indistinguishable from "still active". Everything starts false: it only becomes true when the
// FIRST config arrives, and no channel should be offered as
// "available" before that (we know nothing about it yet).
bool s_canal_ativo[MESH_MAX_CANAIS];
bool s_canais_conhecidos = false;   // there is a configuration (from the radio or NVS)
bool s_canais_sujos      = false;   // changed and hasn't gone to NVS yet

// The radio announced it restarted (FromRadio field 8). Without this, we only
// noticed through silence - and waited the whole 10s timeout before
// requesting the configuration again.
volatile bool s_radio_reiniciou = false;
mesh_conversa_t s_conv[MESH_MAX_CONVERSAS];

// COLUMN ORDER (see tab5_mesh_conversas_versao in the .hpp). 's_conv_seq' is the
// conversation's last activity IN THIS session, in a counter that only grows -
// not rx_time, which is 0 when the clock hasn't been set yet and
// would tie everything. A conversation only read from the SD card stays at 0: it goes to the end, in
// folder order.
uint32_t s_conv_seq[MESH_MAX_CONVERSAS];
uint32_t s_seq = 0;
uint32_t s_conv_ver = 0;

// Diagnostic counters. They exist because "nodes 0/0" doesn't say whether LocalStats
// doesn't ARRIVE or arrives and the filter discards it - and those are questions with opposite fixes.
uint32_t s_c_quadros = 0, s_c_pacotes = 0, s_c_telemetria = 0;

// THE GROVE FROM THE INSIDE (console 'mesh grove'): what went through the UART in both
// directions and what its hardware complained about. Bytes outside a frame are line
// garbage (or the node's log text, if it is sending it); FIFO or buffer
// overflow is a genuinely lost byte. Written only by the service task
// (the sending ones, under s_tx).
static struct {
  uint32_t rx_bytes, tx_bytes, tx_quadros, fora_de_quadro, grandes;
  uint32_t ev_fifo, ev_cheio, ev_quadro, ev_paridade, ev_break;
} s_g;
static volatile bool s_rastro = false;   // 'mesh rastro': every frame in the log
static QueueHandle_t s_uart_ev = nullptr;
uint32_t s_c_localstats = 0, s_c_texto = 0, s_c_nodeinfo = 0;
int      s_ultima_variante = -1;   // last field seen inside Telemetry

// How many times a Routing arrived AFTER the packet was already DELIVERED (see
// marcar_envio). It doesn't change the displayed state - "confirmed" still stands -
// but it is a number with its own purpose: if it grows fast, it means
// the "someone heard" confirmation is coming easily and the radio's strong
// (end-to-end) confirmation is coming with difficulty - in other words, the mesh IS working,
// just at the edge of range. It is a route-quality thermometer that wouldn't
// exist if the second Routing were just discarded without counting.
static uint32_t s_c_routing_extra = 0;

// ---------------------------------------------------------------------------
// IN-FLIGHT SENDS
// ---------------------------------------------------------------------------
// Ring of the last packets WE sent, with the state of each. It stays only
// in RAM on purpose: the SD card keeps the id with the message, but not the
// state - reopening the app tomorrow and seeing "sending" on yesterday's packet would be
// a lie. An id that is no longer here returns MESH_ENVIO_NA, and the screen
// draws no mark at all.
#define MESH_MAX_ENVIOS 16

// How long to wait for the confirmation before giving up. The radio retransmits
// by itself a few times and, when it gives up, IT announces it with Routing.MAX_RETRANSMIT
// - this timeout is just the safety net for when the link dies midway.
#define MESH_ACK_MS 60000

typedef struct {
  uint32_t id;
  uint32_t para;     // destination: decides what an ACK means (see marcar_envio)
  uint32_t em_ms;
  uint8_t  estado;
  uint8_t  erro;
} envio_t;

static envio_t  s_envios[MESH_MAX_ENVIOS];
static int      s_prox_envio = 0;
static uint32_t s_envios_ver = 0;

void anotar_envio(uint32_t id, uint32_t para)
{
  if (id == 0) { return; }

  // Already tracked? Don't duplicate. It matters because there are now TWO paths that
  // call this - our send and the echo of a message that left from the phone -,
  // and the same message may go through both.
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id == id) { return; }
  }

  envio_t* e = &s_envios[s_prox_envio];
  s_prox_envio = (s_prox_envio + 1) % MESH_MAX_ENVIOS;
  e->id     = id;
  e->para   = para;
  e->em_ms  = tab5_agora_ms();
  e->estado = MESH_ENVIANDO;
  e->erro   = 0;
  s_envios_ver++;
  sistema_avisar(AVISO_MESH_ENVIO);
}

// ---------------------------------------------------------------------------
// THE OUTCOME GOES TO THE SD CARD. The path doesn't.
// ---------------------------------------------------------------------------
// The first version kept the state only in RAM, on the argument that
// "reopening tomorrow and seeing SENDING on yesterday's packet would be a lie". The
// argument holds - for SENDING. It doesn't hold for the rest.
//
// "Failed for lack of a route" is a FINAL FACT: it doesn't become more or less
// true with time, and it vanishes from the screen only because the 16-send ring wrapped or
// the device restarted. Whoever sent the message wants to know it didn't arrive,
// today and next week.
//
// So the split is by the NATURE of the state, not by convenience:
//   ENVIANDO (sending)        - transient, RAM only. Never written.
//   ENTREGUE (delivered)      - final, written.
//   FALHOU (failed)           - final, written, with the reason.
//   SEM_RESPOSTA (no reply)   - final ("gave up waiting"), written.
//
// Its OWN file and not the message line: the message is written at the moment
// of sending, when the outcome doesn't exist yet. Rewriting that line later
// would mean rewriting the whole file on every ACK. Here it is just appending.
static void gravar_desfecho(uint32_t id, uint8_t estado, uint8_t erro)
{
  if (id == 0) { return; }
  char linha[40];
  snprintf(linha, sizeof(linha), "E,%08lX,%u,%u\n", (unsigned long)id, (unsigned)estado, (unsigned)erro);
  tab5_sd_anexar(MESH_PASTA, MESH_ARQ_ENVIOS, linha);   // system writer (hal/tab5_sd.hpp)
}

// WHAT A ROUTING MEANS depends on WHO sent it ('de') and where
// our packet was going. Silent when the id isn't ours: the mesh is full of
// routing that doesn't concern us.
//
// The node generates an IMPLICIT ACK when it hears a neighbour repeat our packet:
// a Routing with error 0 coming FROM ITSELF (de == our number). It proves that
// the mesh heard - and, to save airtime, the node STOPS retransmitting there.
//   - BROADCAST: there is no recipient to confirm; the implicit one is the best there
//     is, and counts as ENTREGUE (the official app does the same).
//   - DIRECT: the implicit one is halfway - counts as OUVIDO (heard). ENTREGUE only with the ACK
//     from the recipient ITSELF (de == para).
//
// Previously the two were the same: a direct message became "confirmed" on the first echo from
// a neighbour, and when the node later reported that the destination never replied
// (error 5, MAX_RETRANSMIT), the "ENTREGUE is terminal" lock swallowed the
// report. The screen said delivered for a message that didn't arrive.
//
// MQTT'S IMPLICIT ACK IS NOT "HEARD". With the C6L connected to the internet, the
// firmware (src/mqtt/MQTT.cpp) generates an implicit ACK for itself when the
// SERVER returns our own packet - just like a neighbour's, only
// marked transport_mechanism = MQTT. It proves the packet reached the
// server, not that any radio heard it. Counted as OUVIDO, a direct message to
// a node that was OFF got the grey tick immediately and, since OUVIDO doesn't go down, the
// tr("tentou demais") the radio sends afterwards was swallowed: the X only appeared with
// the internet off. The firmware itself already ignores this ACK to stop
// retransmissions (ReliableRouter::sniffReceived); here it also doesn't change the
// mark of a direct message. For broadcast ENTREGUE still applies - whoever is on the
// server receives it, and it is the same rule as before.
void marcar_envio(uint32_t id, int erro, uint32_t de, bool so_mqtt)
{
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    envio_t* e = &s_envios[i];
    if (e->id != id || id == 0) { continue; }

    const bool difusao = (e->para == 0xFFFFFFFFu);
    if (so_mqtt && erro == 0 && !difusao && de != e->para) {
      ESP_LOGI(TAG, "message %08lX: reached the MQTT server - waiting for the destination", (unsigned long)id);
      return;
    }
    uint8_t novo;
    if (erro == 0) {
      novo = (difusao || de == e->para) ? MESH_ENTREGUE : MESH_OUVIDO;
    } else {
      novo = MESH_FALHOU;
    }

    // ENTREGUE is terminal: nothing later undoes it. OUVIDO only goes up (to
    // ENTREGUE); an error after it doesn't become "failed" - the mesh HEARD, what
    // was missing was the destination's confirmation, and the OUVIDO mark already says that.
    if (e->estado == MESH_ENTREGUE || (e->estado == MESH_OUVIDO && novo != MESH_ENTREGUE)) {
      if (erro != 0) {
        ++s_c_routing_extra;   // see the comment on the declaration - it isn't just noise
        ESP_LOGI(TAG, "message %08lX: notice '%s' arrived after %s - stays that way", (unsigned long)id,
                 mesh_erro_nome((uint8_t)erro), e->estado == MESH_ENTREGUE ? "delivered" : "heard");
      }
      return;
    }
    if (e->estado == novo && e->erro == (uint8_t)erro) { return; }
    e->estado = novo;
    e->erro   = (uint8_t)erro;
    s_envios_ver++;
    sistema_avisar(AVISO_MESH_ENVIO);
    gravar_desfecho(id, novo, (uint8_t)erro);
    ESP_LOGI(TAG, "message %08lX: %s%s", (unsigned long)id,
             novo == MESH_ENTREGUE ? "delivered to the destination" : novo == MESH_OUVIDO ? "heard by another node (no confirmation from the destination)" : "FAILED - ",
             novo == MESH_FALHOU ? mesh_erro_nome((uint8_t)erro) : "");
    return;
  }
}

uint8_t tab5_mesh_estado_envio(uint32_t id, uint8_t* erro)
{
  if (id == 0) { return MESH_ENVIO_NA; }
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id != id) { continue; }
    if (erro) { *erro = s_envios[i].erro; }
    return s_envios[i].estado;
  }
  return MESH_ENVIO_NA;
}

uint32_t tab5_mesh_envios_versao(void) { return s_envios_ver; }

const char* mesh_erro_nome(uint8_t erro)
{
  // Meshtastic's Routing.Error. Only those that can happen here; the list
  // grows with every firmware version and the rest falls into "?".
  switch (erro) {
    case 0:   return "ok";
    case 1:   return tr("sem rota");
    case 2:   return tr("recusado");
    case 3:   return tr("sem resposta");
    case 4:   return tr("sem interface");
    case 5:   return tr("tentou demais");
    case 6:   return tr("canal errado");
    case 7:   return tr("grande demais");
    case 8:   return tr("ninguém respondeu");
    case 9:   return tr("limite de transmissão");
    case 32:  return tr("pedido inválido");
    case 33:  return tr("sem autorização");
    case 34:  return tr("falha de chave");
    case 35:  return tr("chave desconhecida");
    case 38:  return tr("rápido demais");
    case MESH_ERRO_FILA: return tr("fila do rádio cheia");
    default:  return "?";
  }
}

void travar(void)   { s_trava.travar(); }
void destravar(void){ s_trava.destravar(); }

bool     tab5_mesh_no_ar(void)       { return (tab5_agora_ms() - s_ultimo_quadro_ms) < SILENCIO_MS
                                           && s_ultimo_quadro_ms != 0; }

// HOW LONG SINCE THE RADIO LAST SPOKE, in ms. UINT32_MAX = never spoke.
//
// It exists because "is the link alive?" and "did someone pull the module off the cable?" are
// different questions and want different timeouts. no_ar() is generous on
// purpose (90s) so as not to call dead a mesh that is merely quiet. The
// Grove watcher needs the RAW number to choose its own timeout - and, with
// it relying on no_ar(), raising SILENCIO_MS from 30s to 90s pushed
// module-swap detection from 90s to 150s without anyone asking.
uint32_t tab5_mesh_silencio_ms(void)
{
  if (s_ultimo_quadro_ms == 0) {
    // NO frame YET - but it may just be because the service has only just
    // started, not because the radio has gone. SAME BUG as the GPS (see the big notice
    // in tab5_gps_silencio_ms(), found on hardware):
    // an unconditional UINT32_MAX makes the Grove watcher (kernel/core.cpp) see
    // "mute for 60s" on the FIRST tick (up to 2s) after ANY restart,
    // without giving the radio even the 60s of MUDO_MS the code meant to give. In
    // practice the C6L usually answers quickly enough never to hit
    // that window - but the mesh never had a guarantee of that, and a radio
    // slow to reconnect (cold boot, noise) would fall straight into the same
    // restart-every-2s loop the GPS showed. Counting from WHEN THE
    // SERVICE STARTED, rather than a fixed sentinel, closes the hole.
    return s_servico_ativo ? (tab5_agora_ms() - s_iniciado_em_ms) : UINT32_MAX;
  }
  return tab5_agora_ms() - s_ultimo_quadro_ms;
}
uint32_t tab5_mesh_meu_numero(void)  { return s_meu_num; }
const char* tab5_mesh_firmware(void) { return s_firmware; }

void tab5_mesh_definir_beep(bool tocar)
{
  ajustes_definir_beep_mesh(tocar);
  // If the source doesn't exist yet (service never started this session), there is no
  // flag to update now - tab5_mesh_iniciar() applies the stored
  // preference the moment the source is born, see above.
  if (s_notif >= 0) { notif_silenciar(s_notif, !tocar); }
}

bool tab5_mesh_beep_ligado(void) { return ajustes_beep_mesh(); }
int      tab5_mesh_nao_lidas(void)   { return s_nao_lidas; }
int      tab5_mesh_nao_lidas_diretas(void) { return s_nao_lidas_dir; }

void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* texto, uint32_t* nodeinfo,
                          int* ultima_variante)
{
  if (quadros)    *quadros    = s_c_quadros;
  if (pacotes)    *pacotes    = s_c_pacotes;
  if (telemetria) *telemetria = s_c_telemetria;
  if (localstats) *localstats = s_c_localstats;
  if (texto)      *texto      = s_c_texto;
  if (nodeinfo)   *nodeinfo   = s_c_nodeinfo;
  if (ultima_variante) *ultima_variante = s_ultima_variante;
}

// See the comment on the declaration of s_c_routing_extra: it isn't noise, it is a
// route-quality thermometer - it grows when "someone heard" arrives easily
// but the radio's strong confirmation doesn't.
uint32_t tab5_mesh_routing_extra(void) { return s_c_routing_extra; }

void tab5_mesh_saude(mesh_saude_t* s) { if (s) { travar(); *s = s_saude; destravar(); } }

int tab5_mesh_num_nos(void)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_NOS; ++i) { if (s_nos[i].usado) { ++n; } }
  destravar();
  return n;
}

uint32_t tab5_mesh_despejos(void) { return s_despejos; }

// OUR NUMBER SURVIVES POWER-OFF.
//
// It only came from the radio's my_info. Without the C6L plugged in it was 0 - and since the
// screen decides the bubble's side by (m->de == meu_numero), EVERY message in the
// file appeared as received, including the ones you sent. The file
// opened telling the conversation wrongly.
//
// It lives in mesh's own NVS, and not in ajustes: ajustes keeps what the PERSON
// chose, and this is something the radio told us. Mixing the two would make the
// preferences module know about things that aren't preferences.

void lembrar_meu_numero(uint32_t num)
{
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u32(h, "meu_num", num);
  nvs_commit(h);
  nvs_close(h);
}

uint32_t recordar_meu_numero(void)
{
  nvs_handle_t h;
  uint32_t v = 0;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return 0; }
  if (nvs_get_u32(h, "meu_num", &v) != ESP_OK) { v = 0; }
  nvs_close(h);
  return v;
}

bool tab5_mesh_servico_ativo(void) { return s_servico_ativo; }

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
// ToRadio{ heartbeat: {} } framed.
//   field 7, type 2 -> tag (7<<3)|2 = 0x3A, length 0
//
// The radio expects the client to show itself from time to time; the C6L's log
// shows "Got client heartbeat" coming from the phone. Without this it may consider
// the client absent and stop delivering traffic - and the bridge stays alive but mute.
// THE LINK: delivers the body of a ToRadio through the service's path. On serial it
// goes in a 0x94 0xC3 <size> frame; on BLE it is the value of the
// toRadio characteristic itself (see tab5_mesh_ble.hpp). It is the ONLY place that knows
// which path it is - the rest of the service just builds protobuf.
// With the node out and Wi-Fi up, the internet takes over (see usar_internet in the
// service loop): what would go out through the radio goes over MQTT.
// Created once and never deleted: the link may be called from the UI
// while the service stops and starts again.
static Trava s_tx;

// A ToRadio from the proxy (downlink): the firmware's ceiling, over the cable and over BLE.
#define QUADRO_PROXY_MAX 512

bool enlace_enviar(const uint8_t* corpo, size_t n)
{
  // For the restart journal: what the Tab5 sent last. ToRadio:
  // 1 = packet, 3 = want_config_id, 7 = heartbeat.
  if (n > 0) {
    const int campo = corpo[0] >> 3;
    mesh_diario_nota_envio(campo == 1 ? MESH_ENV_PACOTE : campo == 3 ? MESH_ENV_CONFIG
                         : campo == 7 ? MESH_ENV_HEARTBEAT : MESH_ENV_NENHUM);
  }
  if (s_via == AJUSTES_MESH_INTERNET) { return mesh_mqtt_enviar(corpo, n); }
  if (s_via == AJUSTES_MESH_BLUETOOTH) {
    if (!mesh_ble_conectado() && mesh_mqtt_conectado()) { return mesh_mqtt_enviar(corpo, n); }
    return mesh_ble_enviar(corpo, n);
  }
  // ONE FRAME AT A TIME ON THE UART. Header and body are two writes, and
  // both the Mesh task (heartbeat, configuration) and the UI one
  // (text, favourite, admin) write here. Without the lock, a heartbeat could land between the
  // header and the body of a message: the node read one size and received another
  // frame in its place - both were lost without any error.
  s_tx.travar();
  uint8_t cab[4] = { 0x94, 0xC3, (uint8_t)(n >> 8), (uint8_t)(n & 0xFF) };
  uart_write_bytes(MESH_UART, cab, sizeof(cab));
  uart_write_bytes(MESH_UART, corpo, n);
  s_g.tx_bytes += (uint32_t)(n + 4);
  ++s_g.tx_quadros;
  s_tx.destravar();
  if (s_rastro) { ESP_LOGW("grove", "TX field %d, %u bytes", n ? (corpo[0] >> 3) : 0, (unsigned)n); }
  return true;
}

static void mandar_heartbeat(void)
{
  const uint8_t q[] = { 0x3A, 0x00 };
  enlace_enviar(q, sizeof(q));
}

// REQUEST THE CONFIGURATION AGAIN shortly - after writing something to the node
// (tab5_mesh_admin): it restarts and, over serial, doesn't speak of its own accord.
static volatile uint32_t s_reapresentar_ms = 0;
static volatile bool     s_acordar_proxima = false;

static volatile bool     s_so_config = false;

void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou)
{
  // Restarted, it may be in the serial low-power mode and need
  // the 0x94 burst. Without a restart, NO: the burst in the middle of a conversation is
  // noise in its frame state machine (see pedir_apresentacao).
  // Without a restart (fixed position set or removed) only the CONFIGURATION changed: the
  // request is the "configuration only" one, without the whole NodeDB behind it.
  if (reiniciou) { s_acordar_proxima = true; s_so_config = false; }
  else           { s_so_config = true; }
  s_reapresentar_ms = (tab5_agora_ms() + ms) | 1u;
}

static void pedir_apresentacao(void)
{
  // THE 0x94 BURST ONLY THE FIRST TIME.
  //
  // It exists to take the radio out of serial low-power mode - it is the
  // documented wake-up procedure. But it is 32 bytes of garbage entering ITS
  // framing state machine, and sending that with the conversation already established
  // is injecting noise in the middle of a frame that may be arriving at the same
  // time. After the first contact the radio is awake by definition: it
  // has just spoken to us.
  if ((s_meu_num == 0 || s_acordar_proxima) && s_via == AJUSTES_MESH_SERIAL) {   // BLE has no serial low-power mode
    s_acordar_proxima = false;
    uint8_t acorda[32];
    memset(acorda, 0x94, sizeof(acorda));
    s_tx.travar();
    uart_write_bytes(MESH_UART, acorda, sizeof(acorda));
    s_tx.destravar();
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  // ONLY THE CONFIGURATION when only it changed: want_config_id 69420 is the
  // firmware's SPECIAL_NONCE_ONLY_CONFIG (PhoneAPI) - channels, config and
  // modules, without the NodeDB, which over Bluetooth with a full mesh is
  // hundreds of frames just to find out that one field changed. Old firmware doesn't
  // know the number and sends everything, as before. Any other reason (first
  // contact, restart, silence) asks for everything.
  const bool so_config = s_so_config && s_meu_num != 0;
  s_so_config = false;
  static const uint8_t TUDO_Q[]   = { 0x18, 0x2A };               // ToRadio{ want_config_id: 42 }
  static const uint8_t CONFIG_Q[] = { 0x18, 0xAC, 0x9E, 0x04 };   // ToRadio{ want_config_id: 69420 }
  s_config_ok_ms = 0;   // new dump: what comes until its end is past
  mesh_admin_inicio_dump();
  if (so_config) { enlace_enviar(CONFIG_Q, sizeof(CONFIG_Q)); }
  else           { enlace_enviar(TUDO_Q, sizeof(TUDO_Q)); }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
// Text, admin and position go out in the SAME envelope ToRadio{ packet: MeshPacket{
// to, channel, decoded: Data{ portnum, payload }, id, hop_limit, want_ack } },
// built in a single place: mesh_pb_to_radio() (services/mesh_pb.hpp).

// ---------------------------------------------------------------------------
// TEXT SEND QUEUE
// ---------------------------------------------------------------------------
typedef struct { uint16_t n; uint32_t id; uint8_t d[MESH_TEXTO + 96]; } envio_fila_t;
static QueueHandle_t s_fila_envio = nullptr;
#define MESH_FILA_ENVIO 8

// Can we already tell it won't go out? (see tab5_mesh_enviar)
static bool pode_sair(uint32_t para)
{
  const bool difusao = (para == 0xFFFFFFFFu);
  if (s_via == AJUSTES_MESH_INTERNET) { return difusao && mesh_mqtt_conectado(); }
  if (s_via == AJUSTES_MESH_BLUETOOTH && !mesh_ble_conectado()) {
    return difusao && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();   // the internet covering for its absence
  }
  return true;
}

// TWO SECONDS BETWEEN TEXTS. The firmware discards text that arrives from the
// client less than 2 s after the previous one (PhoneAPI, TEXT port rate limit,
// from 2.5 to the current one): on 2.7 with no error at all - the message stayed "sending" and
// became tr("sem resposta"). The queue waits out the remainder. Called by the
// service task, without the lock.
static void laco_envios(void)
{
  static uint32_t ultimo_ms = 0;
  static envio_fila_t e;
  if (!s_fila_envio) { return; }
  for (int k = 0; k < 4; ++k) {
    if (xQueuePeek(s_fila_envio, &e, 0) != pdTRUE) { return; }
    if (ultimo_ms && tab5_agora_ms() - ultimo_ms < 2100) { return; }
    xQueueReceive(s_fila_envio, &e, 0);
    const bool ok = enlace_enviar(e.d, e.n);
    ultimo_ms = tab5_agora_ms();
    if (!ok) {
      // The link dropped between the tap and its turn: failed, and the screen says so.
      ESP_LOGW(TAG, "packet %08lX: the link refused", (unsigned long)e.id);
      travar();
      marcar_envio(e.id, MESH_ERRO_FILA, s_meu_num);
      destravar();
    }
  }
}

bool tab5_mesh_enviar(uint32_t para, uint8_t canal, const char* texto)
{
  if (!texto || !texto[0]) { return false; }

  // WITHOUT THE SERVICE THERE IS NOWHERE TO WRITE. The screen already prevents getting here, but the
  // guard ALSO lives here on purpose: without the service no UART is installed, and
  // a uart_write_bytes on a port that doesn't exist isn't "didn't send" - it is
  // undefined behaviour. What protects the hardware is the hardware layer.
  if (!s_servico_ativo) {
    ESP_LOGW(TAG, "send refused: service stopped (no Meshtastic node)");
    return false;
  }
  // A MUTE LINK IS NO REASON TO REFUSE, and refusing was worse than the problem.
  //
  // "on air" here only means "some frame arrived from the radio in the last 30s".
  // On a quiet mesh that is the NORMAL state: the radio doesn't speak on its own, and there's
  // nothing wrong with the cable. By refusing, the ENV button did nothing, with no
  // error message and no history entry - from the outside it is
  // indistinguishable from a broken system, and that is how it stayed all along.
  //
  // Sending is right: if the radio really isn't there, the queueStatus doesn't come, the
  // ACK doesn't come, and the 60s timeout marks the message as tr("sem resposta") on the
  // screen itself. That is information; the silence wasn't.
  if (!tab5_mesh_no_ar()) {
    ESP_LOGW(TAG, "link with no recent frame - sending anyway");
  }

  // At most MESH_PAYLOAD_MAX (233) bytes - above that the node silently discards the
  // whole packet (see the .hpp). Cuts on a character boundary:
  // an accent split in half comes out as garbage on the recipient's screen.
  size_t n_txt = strnlen(texto, MESH_PAYLOAD_MAX + 1);
  if (n_txt > MESH_PAYLOAD_MAX) {
    n_txt = MESH_PAYLOAD_MAX;
    while (n_txt > 0 && ((unsigned char)texto[n_txt] & 0xC0) == 0x80) { --n_txt; }
    ESP_LOGW(TAG, "text over %d bytes - sent truncated", MESH_PAYLOAD_MAX);
  }
  char enviado[MESH_PAYLOAD_MAX + 1];
  memcpy(enviado, texto, n_txt);
  enviado[n_txt] = 0;

  mesh_pb_envelope_t env = { };
  env.para    = para;
  env.canal   = canal;
  env.porta   = PORT_TEXTO;
  env.carga   = (const uint8_t*)texto;
  env.n_carga = n_txt;
  // OUR OWN ID, and that is what makes confirmation possible. Without an id the radio
  // invents one, and we never find out which - the ACK would arrive with a
  // request_id that matches nothing of ours. Zero is reserved, hence the |1.
  env.id = esp_random() | 1u;
  // hop_limit 3. Without it the packet goes out with ZERO HOPS - only those who hear our
  // antenna directly receive it, and nothing is repeated onwards. 3 is Meshtastic's
  // own default (we don't read the radio's Config to inherit its value).
  env.saltos = 3;
  // want_ack in BOTH cases - broadcast too. There, there's no recipient to
  // confirm, but there is the IMPLICIT CONFIRMATION: when a neighbour repeats the
  // packet, the firmware returns a Routing with our request_id. It is the only
  // proof that someone heard - the same as the official app does on a channel.
  env.quer_ack = true;
  const uint32_t id = env.id;

  // Framing (if any) belongs to the link.
  uint8_t quadro[MESH_TEXTO + 96];
  const size_t n_quadro = mesh_pb_to_radio(&env, quadro, sizeof(quadro));
  if (!n_quadro) { ESP_LOGE(TAG, "text didn't fit in the frame"); return false; }

  // THE REFUSAL WE CAN KNOW NOW goes out now, with the send failing on
  // screen: the internet (channels only - direct needs PKI), Bluetooth without a
  // connection. The rest is decided by the link, in the queue.
  if (!pode_sair(para)) {
    ESP_LOGW(TAG, "send refused: %s", tab5_mesh_enlace_descricao());
    return false;
  }

  // IN THE QUEUE, NOT IMMEDIATELY. The caller is the UI task, and the send may
  // have to wait out the node's 2 s limit between texts (see laco_envios) -
  // waiting here froze the screen. The message appears in the conversation already as
  // "sending"; the service task delivers it at the rate the node accepts.
  if (!s_fila_envio) { return false; }
  static envio_fila_t e;   // only the UI sends text; 300 bytes off its stack
  e.n = (uint16_t)n_quadro;
  e.id = id;
  memcpy(e.d, quadro, e.n);
  if (xQueueSend(s_fila_envio, &e, 0) != pdTRUE) {
    ESP_LOGW(TAG, "send queue full - message not sent");
    return false;
  }

  // Stores our own message: the screen shows yours on the right, and without
  // this it would vanish - the radio doesn't return what you yourself sent.
  travar();
  anotar_envio(id, para);
  // No patching the counter: guardar_msg already knows the message is ours
  // (de == s_meu_num) and doesn't count it as unread, neither here nor in the conversation.
  // via_mqtt = false: it is OUR transmission going out through our own radio,
  // it never arrives via the bridge.
  // saltos = unknown: it is OUR message, it hasn't travelled any hop yet
  // from the recipient's point of view - and the meta-line doesn't even appear for
  // MY messages anyway (see desenhar_msgs in app_mesh.cpp).
  // THE SEND TIME, in UTC like the radio's rx_time. It was 0 - and
  // 0 in time zone -3 becomes 31/12/1969 on screen. A clock not yet set (no GPS
  // or mesh since boot) stays 0 = "undated", which the screen knows how to handle.
  const uint32_t quando = tab5_hora_epoch();
  guardar_msg(s_meu_num, para, canal, enviado, quando, 0, 0, id, false, MESH_SALTOS_DESCONHECIDO);
  lembrar_carimbo(id, quando);   // corrected if the clock jumps (ao_saltar_relogio)
  destravar();

  ESP_LOGI(TAG, "sent to %08lX (channel %u) id=%08lX: %s",
           (unsigned long)para, (unsigned)canal, (unsigned long)id, enviado);
  return true;
}

// Who we are, for the MQTT client: it publishes and answers want_config on
// behalf of this number and these names. Called before starting the client.
// A NUMBER UNIQUE TO THIS TAB5, for when there has never been a node: the last 4 bytes of the
// MAC, the same calculation Meshtastic does for a node's number. It used to stay
// 0 - every Tab5 without a node (the M5Burner firmware, used only over the internet) joined
// the server as "tab5-00000000" with sender !00000000: clients with the
// same id knocked each other off, and everyone's messages looked like they came from
// one. It doesn't go to NVS: the real number is the node's, when it comes.
static uint32_t numero_do_tab5(void)
{
  uint8_t mac[6] = { 0 };
  if (esp_efuse_mac_get_default(mac) != ESP_OK) { esp_read_mac(mac, ESP_MAC_BASE); }
  const uint32_t n = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  return n ? n : 1u;
}

static void informar_no_ao_mqtt(void)
{
  if (s_meu_num == 0) {
    s_meu_num = numero_do_tab5();
    ESP_LOGI(TAG, "no known node: the Tab5 talks over the internet as !%08lx", (unsigned long)s_meu_num);
  }
  char curto[MESH_NOME_CURTO] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(s_meu_num, curto, sizeof(curto));
  tab5_mesh_nome_longo_cache(s_meu_num, longo, sizeof(longo));
  mesh_mqtt_definir_no(s_meu_num, curto, longo);
}

// MESH'S SLOW WRITES, PIGGYBACKING ON THE "cartao" TASK (tab5_sd_carona).
// They used to be done in the RADIO loop: while the NodeDB went to the SD card (mount,
// write dozens of KB, fsync) nobody read the UART, and the 4 KB buffer (~350
// ms at 115200) overflowed on a slow SD card - a node frame lost. The system
// rule (hal/tab5_sd.hpp) is this: whoever only flushes data to the SD card piggybacks
// on its task. Channels: every 15 s if they changed (and right after the
// dump, which marks them). NodeDB: every 10 min if it changed.
static void mesh_na_carona(void)
{
  if (!s_servico_ativo) { return; }
  const uint32_t t = tab5_agora_ms();
  static uint32_t proximos_canais = 0, proxima_nodedb = 600000;
  if (s_canais_sujos && (int32_t)(t - proximos_canais) >= 0) {
    proximos_canais = t + 15000;
    salvar_canais();
  }
  if ((int32_t)(t - proxima_nodedb) >= 0) {
    proxima_nodedb = t + 600000;
    if (s_nos_ver != s_nodedb_ver_salva) { salvar_nodedb(); }
  }
}

static void vTaskMesh(void*)
{
  static uint8_t quadro[1024];
  size_t usado = 0;
  int    estado = 0;          // 0=looking for 0x94  1=saw 0x94  2=size  3=body
  size_t falta = 0;
  uint8_t tam_hi = 0;
  uint32_t proxima_apresentacao = 0;
  uint32_t proximo_heartbeat = 15000;   // first only after the initial dump

  // WATCHDOG: a deadlock here is SILENT and costly in a specific way -
  // this task holds the mesh mutex (travar()/destravar()) in several
  // stretches, and stuck INSIDE one of them the mutex would never be returned. The
  // screen doesn't lock up (it would just ask for the mutex and wait, but the
  // UI loop stays alive outside), so the symptom would be "messages stopped
  // arriving" with no log saying why - hard to tell apart from a
  // genuinely mute radio. See the same comment in kernel/core.cpp for the
  // full reasoning.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "couldn't subscribe the task to the watchdog");
  }

  for (;;)
  {
    // EXIT POINT. At the top of the loop, outside any critical section: here the
    // task holds no mutex or file, so leaving from here leaves nothing
    // half-done. Since the uart_read_bytes down below waits at most 200ms, the
    // stop costs that in the worst case.
    if (parada_pedida(&s_parada)) {
      // UNSUBSCRIBE BEFORE DYING. The watchdog keeps this task's HANDLE;
      // deleting without telling it first, the handle is left dangling inside and the
      // watchdog's next check cycle would find a task that
      // no longer exists - the kind of "ghost" this system has been hunting
      // in other places (Grove, radio introduction). The order matters:
      // unsubscribing only works while the task still exists to make the
      // call.
      esp_task_wdt_delete(NULL);
      parada_confirmar(&s_parada);
      return;   // the pool slot returns the owner and goes back to sleep (kernel/pool_tarefas.h)
    }

    // PROOF OF LIFE. Getting here again means the previous round
    // finished - no uart_read_bytes, mutex lock or SD card I/O
    // got stuck. 200ms is the longest block this loop does on purpose
    // (the uart_read_bytes timeout down below); well within the watchdog's 5s.
    esp_task_wdt_reset();

    const uint32_t t = tab5_agora_ms();

    // Introduction ONLY when the link is mute. Asking again with the link
    // alive makes the radio RESTART the whole configuration download - that is what
    // produced 11 node_info for 6 nodes: the dump ran twice.
    // The radio announced it restarted: request the configuration IMMEDIATELY. Without this
    // we waited for the link to go mute and the 10s timeout to expire - and in the
    // meantime the node list and channels stayed from before the restart.
    mesh_admin_tique(t);
    if (s_reapresentar_ms && (int32_t)(t - s_reapresentar_ms) >= 0) {
      s_reapresentar_ms = 0;
      proxima_apresentacao = t + 10000;
      pedir_apresentacao();
    }
    else if (s_radio_reiniciou) {
      s_radio_reiniciou = false;
      proxima_apresentacao = t + 10000;
      pedir_apresentacao();
    }
    else if (!tab5_mesh_no_ar() && t > proxima_apresentacao) {
      // TWO RHYTHMS, and the difference between them is what solves it.
      //
      // Without my_info we don't even know our own number: nothing works, and
      // insisting every 10 seconds is right - it is the first contact.
      //
      // WITH it, we have already talked to this radio. Silence now means
      // "the mesh is quiet", not "the radio has gone" - and requesting the configuration
      // again makes the C6L RESTART the whole download, throwing away the live traffic
      // that may be in its queue. It was a dump every 30s, forever, for
      // most of the time. Every two minutes still recovers a radio
      // that locked up, without turning the conversation into a cycle of re-introductions.
      const uint32_t espera = (s_meu_num == 0) ? 10000 : 120000;
      proxima_apresentacao = t + espera;
      ESP_LOGI(TAG, "no frame for %d s - requesting the configuration from the radio",
               SILENCIO_MS / 1000);
      pedir_apresentacao();
    }

    // Pending node names, every 15 seconds. Spaced out on purpose: it is
    // what turns the boot's burst of mounts into a single write.
    static uint32_t proxima_descarga = 15000;
    if (t > proxima_descarga) {
      proxima_descarga = t + 15000;
      descarregar_nomes();   // only hands over to the writer (tab5_sd_anexar): doesn't block
    }
    // Channels (NVS) and NodeDB (SD card) are NOT written here: see mesh_na_carona.

    // Timeout for unanswered sends. Safety net: when the radio gives up
    // of its own accord it ANNOUNCES it (Routing.MAX_RETRANSMIT), so getting here
    // means the link died midway - and "don't know" is what the screen should
    // say, not "failed".
    for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
      if (s_envios[i].estado == MESH_ENVIANDO
       && (t - s_envios[i].em_ms) > MESH_ACK_MS) {
        s_envios[i].estado = MESH_SEM_RESPOSTA;
        s_envios_ver++;
        sistema_avisar(AVISO_MESH_ENVIO);
        gravar_desfecho(s_envios[i].id, MESH_SEM_RESPOSTA, 0);
        ESP_LOGW(TAG, "packet %08lX: no confirmation within %d s",
                 (unsigned long)s_envios[i].id, MESH_ACK_MS / 1000);
      }
    }

    laco_envios();   // text in the queue (see tab5_mesh_enviar)

    // Periodic heartbeat. Keeps the radio convinced there's a client on the other
    // side; without it live traffic may simply stop coming.
    if (t > proximo_heartbeat) {
      proximo_heartbeat = t + 30000;
      mandar_heartbeat();
    }

    // BLOCK READING, and the difference is energy, not elegance.
    //
    // It used to be one call per BYTE, with a 200ms timeout. At 115200 baud that
    // comes to 11,520 calls per second, each with a queue crossing and a
    // context switch - and it runs all the time, because this is a service, not an app.
    //
    // Reading in blocks, a whole packet comes out of ONE call. The task wakes
    // a few times per second instead of thousands, and the core goes back to being
    // idle between them - which is what lets DFS do its job.
    // GPS POSITION FOR THE MESH - see enviar_posicao().
    static uint32_t proxima_posicao = 60000;
    if (ajustes_mesh_posicao_min() && t > proxima_posicao) {
      proxima_posicao = t + (enviar_posicao() ? ajustes_mesh_posicao_min() * 60000u : 30000u);
    }

    // THE MQTT CLIENT, IN ONE OF TWO ROLES (see services/tab5_mesh_mqtt.hpp):
    //   INTERNET - the Tab5 talks to the mesh through the server, when the node isn't there
    //              (or when the chosen path is the Internet);
    //   PROXY    - the node is connected and has the proxy enabled: the Tab5 just carries
    //              what IT publishes and subscribes to (proxy enabled on the node).
    // The two are exclusive by construction: the proxy requires the node present, the
    // internet as a stopgap requires its absence. With slack in both
    // directions: BLE drops and comes back by itself in a reconnection, and the server shouldn't
    // be opened and closed on every blink of it.
    {
      enum { MQ_NADA = 0, MQ_NET, MQ_PROXY };
      static int      modo_pedido = MQ_NADA;
      static uint32_t modo_desde = 0;
      // TWO WAYS FOR THE INTERNET TO BE HERE:
      //   - it IS the path (chosen in Ajustes, or the kernel started the service just
      //     for it): runs with Wi-Fi, full stop. The C6L on the cable doesn't knock it down
      //     from here - what decides to switch to Grove is the kernel's probe, on
      //     FINDING the node (Grove's priority), not this loop every round. With
      //     the Grove test here, choosing "internet" with the C6L plugged in
      //     knocked MQTT down right after it came up.
      //   - the path is the node (cable or Bluetooth): the internet only covers its
      //     absence, if the "internet when without the node" setting is on.
      const bool wifi = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
      const bool no_ok = (s_via == AJUSTES_MESH_SERIAL)
                      || (s_via == AJUSTES_MESH_BLUETOOTH && mesh_ble_conectado())
                      || tab5_grove_tipo() == GROVE_MESHTASTIC;
      const bool quer_net = wifi && ((s_via == AJUSTES_MESH_INTERNET)
                                  || (ajustes_mesh_internet() && !no_ok));
      // The proxy needs the node TALKING (not just the chosen path) and with the dump
      // complete: it is the dump that says whether the proxy is enabled on it and which channels
      // come down.
      const bool no_vivo = (s_via == AJUSTES_MESH_SERIAL && tab5_mesh_no_ar())
                        || (s_via == AJUSTES_MESH_BLUETOOTH && mesh_ble_conectado());
      // THE SWITCH BELONGS TO THE NODE: the proxy is enabled on the "No" (Node) page of the Mesh app, which writes
      // proxy_to_client_enabled to it. Enabled on the node, the Tab5 lends it the internet.
      const bool quer_proxy = wifi && no_vivo && s_config_ok_ms != 0 && mesh_mqtt_no_tem_proxy();
      const int modo = quer_proxy ? MQ_PROXY : quer_net ? MQ_NET : MQ_NADA;
      int atual = !mesh_mqtt_ativo() ? MQ_NADA : mesh_mqtt_em_proxy() ? MQ_PROXY : MQ_NET;
      if (modo != modo_pedido) { modo_pedido = modo; modo_desde = t; }
      // Going down: a node that has just come back takes the internet down IMMEDIATELY (the proxy is about to
      // come up); the rest wait a steady 10 s, so as not to drop on a blink.
      if (atual != MQ_NADA && atual != modo
       && (modo == MQ_PROXY || t - modo_desde >= 10000)) {
        mesh_mqtt_parar();
        atual = MQ_NADA;
      }
      // Going up: the internet as a stopgap waits 20 s (BLE usually comes back
      // before that); the Internet path and the proxy come up straight away.
      const uint32_t folga = (modo == MQ_NET && s_via != AJUSTES_MESH_INTERNET) ? 20000 : 0;
      if (atual == MQ_NADA && modo != MQ_NADA && t - modo_desde >= folga) {
        informar_no_ao_mqtt();
        if (modo == MQ_PROXY) { mesh_mqtt_iniciar_proxy(); } else { mesh_mqtt_iniciar(); }
        atual = !mesh_mqtt_ativo() ? MQ_NADA : modo;
      }
      if (atual == MQ_NET) {
        if (mesh_mqtt_conectou_agora() && s_via == AJUSTES_MESH_INTERNET) { s_radio_reiniciou = true; }
        // DRAINS THE QUEUE, not one per round: with the node on the cable a round waits up to
        // 200 ms on serial, and one packet per round (5/s) lost the server's
        // burst. Only the first waits (on the Internet path it is the loop's rhythm).
        for (int k = 0; k < 16; ++k) {
          const size_t n = mesh_mqtt_receber(quadro, sizeof(quadro),
                                             (k == 0 && s_via == AJUSTES_MESH_INTERNET) ? 200 : 0);
          if (n == 0) { break; }
          travar();
          s_c_quadros++;
          ler_from_radio(quadro, n);
          s_ultimo_quadro_ms = tab5_agora_ms();
          destravar();
        }
      } else if (atual == MQ_PROXY) {
        // PROXY DOWNLINK: what came from the server goes to the node. Up to 8 per round -
        // the round lasts at most the 200 ms of the link read, and the queue (32)
        // holds the burst in the meantime. It was 4 (20/s), and a public server
        // delivering the whole channel filled the queue. More than 8 would hold up the
        // round writing to serial/Bluetooth.
        static uint8_t desce[QUADRO_PROXY_MAX];
        for (int k = 0; k < 8; ++k) {
          const size_t n = mesh_mqtt_proxy_receber(desce, sizeof(desce));
          if (n == 0) { break; }
          enlace_enviar(desce, n);
        }
      }
      if (s_via == AJUSTES_MESH_INTERNET) {
        if (!mesh_mqtt_ativo()) { vTaskDelay(pdMS_TO_TICKS(200)); }
        continue;
      }
    }

    // OVER BLUETOOTH: the link already delivers whole FromRadio, with no frame - there's no
    // framing state machine. Same 200 ms timeout as serial.
    if (s_via == AJUSTES_MESH_BLUETOOTH) {
      if (mesh_ble_conectou_agora()) { s_radio_reiniciou = true; }   // request the configuration right away
      // What has already arrived in one go (the dump comes in a burst); only the first waits.
      for (int k = 0; k < 16; ++k) {
        const size_t n = mesh_ble_receber(quadro, sizeof(quadro), k == 0 ? 200 : 0);
        if (n == 0) { break; }
        travar();
        s_c_quadros++;
        ler_from_radio(quadro, n);
        s_ultimo_quadro_ms = tab5_agora_ms();
        destravar();
      }
      continue;
    }

    // WAKE ON THE FIRST BYTE, READ THE REST IMMEDIATELY. uart_read_bytes only returns
    // with the requested size OR at the timeout: asking for 256, a 60-byte frame
    // sat waiting the whole 200 ms - every message arrived late, and the
    // whole loop (send queue, internet) moved at that pace. Waiting for
    // ONE byte sleeps just the same when the serial is quiet and wakes when it speaks.
    static uint8_t entrada[512];
    // WHAT THE UART HARDWARE COMPLAINED ABOUT since the last round (see s_g).
    if (s_uart_ev) {
      uart_event_t ev;
      while (xQueueReceive(s_uart_ev, &ev, 0) == pdTRUE) {
        switch (ev.type) {
          case UART_FIFO_OVF:    ++s_g.ev_fifo; break;
          case UART_BUFFER_FULL: ++s_g.ev_cheio; break;
          case UART_FRAME_ERR:   ++s_g.ev_quadro; break;
          case UART_PARITY_ERR:  ++s_g.ev_paridade; break;
          case UART_BREAK:       ++s_g.ev_break; break;
          default: break;
        }
        if (s_rastro && ev.type != UART_DATA) { ESP_LOGW("grove", "UART event %d", (int)ev.type); }
      }
    }
    int lidos = uart_read_bytes(MESH_UART, entrada, 1, pdMS_TO_TICKS(200));
    if (lidos <= 0) { continue; }
    size_t pronto = 0;
    if (uart_get_buffered_data_len(MESH_UART, &pronto) == ESP_OK && pronto > 0) {
      if (pronto > sizeof(entrada) - 1) { pronto = sizeof(entrada) - 1; }
      const int mais = uart_read_bytes(MESH_UART, entrada + 1, pronto, 0);
      if (mais > 0) { lidos += mais; }
    }

    s_g.rx_bytes += (uint32_t)lidos;
    for (int k = 0; k < lidos; ++k) {
    const uint8_t b = entrada[k];

    switch (estado) {
      case 0: if (b == 0x94) { estado = 1; } else { ++s_g.fora_de_quadro; } break;
      case 1: estado = (b == 0xC3) ? 2 : (b == 0x94 ? 1 : 0); break;
      case 2: tam_hi = b; estado = 3; break;
      case 3: {
        falta = ((size_t)tam_hi << 8) | b;
        usado = 0;
        if (falta == 0) { estado = 0; }
        else if (falta > sizeof(quadro)) {
          // SKIPS THE BODY, doesn't go back to looking for a header inside it.
          //
          // Going back to state 0 the machine would scan the discarded frame's own
          // bytes for 0x94 0xC3 - and that pair turns up by
          // chance in binary data. Finding one, it read a "size" that is
          // garbage and swallowed the GOOD frame that came after. A message vanishes, and
          // nothing in the log says why.
          ESP_LOGW(TAG, "frame of %u bytes doesn't fit - skipping", (unsigned)falta);
          ++s_g.grandes;
          estado = 5;
        }
        else { estado = 4; }
        break;
      }
      case 5:                      // swallowing a frame that is too large
        if (--falta == 0) { estado = 0; }
        break;
      case 4:
        quadro[usado++] = b;
        if (usado >= falta) {
          if (s_rastro) {
            int cf = usado ? (quadro[0] >> 3) : 0;
            if (cf == 1 && usado > 5) { cf = quadro[5] >> 3; }   // the id (fixed32) before the variant
            ESP_LOGW("grove", "RX field %d, %u bytes", cf, (unsigned)usado);
          }
          travar();
          s_c_quadros++;
          ler_from_radio(quadro, usado);
          s_ultimo_quadro_ms = tab5_agora_ms();
          destravar();
          estado = 0;
        }
        break;
    }
    }   // for k
  }
}

// STOPS THE SERVICE AND RELEASES THE GROVE.
//
// It exists for the case the system simply didn't cover: swapping the module on the
// cable with the device on. Without this the service stayed up talking to
// a C6L that was no longer there, and the freshly plugged GPS was never looked for
// because the port had an owner.
//
// Returns false when it COULDN'T stop - and in that case cleans up nothing. If the
// task didn't confirm it exited, it may still be alive: deleting the UART and the
// mutex from under it would swap a stubborn service for a crash.
bool tab5_mesh_parar(void)
{
  if (!s_servico_ativo) { return true; }   // already stopped; asking again isn't an error

  ESP_LOGI(TAG, "stopping the service...");
  // The task waits at most 200 ms on the UART (or on the link queue): exiting
  // costs one round. 1 s is ample slack. Without confirmation, the request STAYS
  // and nothing is torn down - the next call finishes it (see parada_pedir).
  if (!parada_pedir(&s_parada, 1000)) {
    ESP_LOGE(TAG, "the task didn't confirm within 1s - still stopping, nothing was torn down");
    return false;
  }

  // From here down the task NO LONGER EXISTS. Only now is it safe to tear down
  // what it used.

  // WRITING TASK FIRST - independent of the UART/mutex above (it only
  // depends on its own queue), but running until here. 300ms of slack: a
  // write usually takes much less, and there's nothing in progress in the "mesh"
  // task that will still enqueue anything (it has already confirmed exit). Any
  // request still in the queue when it exits is discarded - the same trade-off
  // accepted in every queue in this system that enqueues I/O (see the big notice
  // next to MESH_FILA_GRAVACAO).

  // What was pending goes now: the task that would do the flush has just
  // died, and without this the names learnt in this session would be lost.
  descarregar_nomes();

  mesh_mqtt_parar();
  if (s_via == AJUSTES_MESH_BLUETOOTH) {
    mesh_ble_parar();
  } else if (s_via == AJUSTES_MESH_SERIAL) {
    uart_driver_delete(MESH_UART); s_uart_ev = nullptr;   // the event queue belonged to the driver
    tab5_grove_soltar(GROVE_DONO_MESH);
    sistema_recurso(RECURSO_SERIAL_GROVE, "Meshtastic", false);
  }

  // THE NOTICE ONLY GOES IF THERE IS NOTHING TO READ. It used to be cleared on every
  // stop - and Mesh stops on its own (path change, Wi-Fi dropping): the notice
  // vanished without the user having read anything. The unread ones stay stored and come back
  // with the conversations; what is published now still stands.
  guardar_nao_lidas();
  if (s_notif >= 0 && !ha_nao_lidas_guardadas()) { notif_limpar(s_notif); }
  salvar_canais();   // what the piggyback hasn't taken yet (it stops with the service)
  salvar_nodedb();   // before the table memset, just below
  // THE MUTEX STAYS. It used to be deleted here and recreated on the next iniciar - but
  // the public queries (the home bar, the Mesh app) take it from OTHER
  // tasks at any moment: deleting a mutex that is held or with someone
  // waiting on it is freed memory in use, and between deleting and recreating
  // the reads ran with no exclusion at all. Created once, it lasts
  // forever (the same as tab5_sd).

  // Link reset: without this tab5_mesh_no_ar() would keep answering based on the
  // last frame received, and the bar would say "connected" to an absent radio.
  s_ultimo_quadro_ms = 0;
  s_config_ok_ms = 0;
  // s_meu_num is NOT zeroed: our number doesn't change because the cable came out, and the
  // file needs it to know whose each message is.
  s_n_msgs = 0;   // the NODE count has no variable: it comes from the memset below
  s_total_msgs = 0;
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  // Under the lock: the UI may be reading the tables right now (the
  // home bar, the Mesh app) - this used to run with the mutex already deleted.
  travar();
  memset(s_nos,  0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  ++s_conv_ver;
  destravar();

  s_servico_ativo = false;
  sistema_avisar(AVISO_MESH_NOS);   // the writing bar becomes "read only"
  ESP_LOGI(TAG, "service stopped%s", s_via == AJUSTES_MESH_SERIAL ? ", Grove free" : "");
  return true;
}

bool tab5_mesh_soltar_grove(void)
{
  // Over Bluetooth the service isn't on the Grove: swapping the module on the cable is no
  // reason to drop the conversation with the node.
  if (s_servico_ativo && s_via != AJUSTES_MESH_SERIAL) { return true; }
  return tab5_mesh_parar();
}

uint8_t tab5_mesh_via(void) { return s_via; }

const char* tab5_mesh_enlace_descricao(void)
{
  if (!s_servico_ativo) { return N_("parado"); }
  if (s_via == AJUSTES_MESH_INTERNET) { return mesh_mqtt_descricao(); }
  if (s_via == AJUSTES_MESH_BLUETOOTH) {
    return (!mesh_ble_conectado() && mesh_mqtt_ativo()) ? mesh_mqtt_descricao() : mesh_ble_descricao();
  }
  return tab5_mesh_no_ar() ? N_("cabo Grove, conversando") : N_("cabo Grove, esperando o no");
}

static void iniciar_via(uint8_t via);

void tab5_mesh_iniciar(void)
{
  // STARTS ON THE CHOSEN PATH. If the service is up via ANOTHER path (internet
  // covering for the node's absence, or Bluetooth when Grove has just gained
  // priority), stop first: without this iniciar_via saw "already up" and
  // did nothing, and Mesh carried on on the old path.
  const uint8_t via = ajustes_mesh_via();
  if (s_servico_ativo && s_via != via && !parada_em_curso(&s_parada)) { tab5_mesh_parar(); }
  iniciar_via(via);
}

void tab5_mesh_rastro(bool ligado) { s_rastro = ligado; }

void tab5_mesh_grove_diag(char* out, size_t tam)
{
  snprintf(out, tam,
           "Grove: received %lu bytes in %lu good frames, %lu bytes outside frames, %lu oversized\n"
           "       sent %lu bytes in %lu frames | last frame from node %lu ms ago\n"
           "UART:  FIFO overflow %lu, buffer full %lu, frame error %lu, parity %lu, break %lu\n",
           (unsigned long)s_g.rx_bytes, (unsigned long)s_c_quadros, (unsigned long)s_g.fora_de_quadro,
           (unsigned long)s_g.grandes, (unsigned long)s_g.tx_bytes, (unsigned long)s_g.tx_quadros,
           (unsigned long)(s_ultimo_quadro_ms ? tab5_agora_ms() - s_ultimo_quadro_ms : 0),
           (unsigned long)s_g.ev_fifo, (unsigned long)s_g.ev_cheio, (unsigned long)s_g.ev_quadro,
           (unsigned long)s_g.ev_paridade, (unsigned long)s_g.ev_break);
}

const char* tab5_mesh_internet_estado(void)
{
  if (ajustes_mesh_via() == AJUSTES_MESH_INTERNET) {
    if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return N_("Internet escolhida: esperando o Wi-Fi conectar"); }
    if (!mesh_mqtt_ativo())                        { return N_("Internet escolhida: vai conectar em instantes"); }
    return mesh_mqtt_descricao();
  }
  if (!ajustes_mesh_internet())                   { return N_("Desligada"); }
  if (tab5_grove_tipo() == GROVE_MESHTASTIC
   || (s_servico_ativo && s_via == AJUSTES_MESH_SERIAL)) { return N_("Em espera: o nó no Grove tem prioridade"); }
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO)  { return N_("Esperando o Wi-Fi conectar"); }
  if (!mesh_mqtt_ativo())                         { return N_("Ligada, vai conectar em instantes"); }
  return mesh_mqtt_descricao();
}

const char* tab5_mesh_proxy_estado(void)
{
  // Short: goes beside "ligado" (on) in a row of the "No" (Node) page.
  static char d[48];
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return N_("sem Wi-Fi no Tab5"); }
  if (!mesh_mqtt_no_tem_proxy())                 { return N_("o nó ainda não confirmou"); }
  if (!mesh_mqtt_em_proxy())                     { return N_("vai conectar"); }
  if (!mesh_mqtt_conectado())                    { return N_("conectando ao servidor"); }
  uint32_t sobe, desce, perde;
  mesh_mqtt_proxy_contadores(&sobe, &desce, &perde);
  snprintf(d, sizeof(d), tr("conectado (%lu/%lu)"), (unsigned long)sobe, (unsigned long)desce);
  return d;
}

void tab5_mesh_iniciar_internet(void)
{
  if (s_servico_ativo) { return; }
  iniciar_via(AJUSTES_MESH_INTERNET);
}

static void iniciar_via(uint8_t via)
{
  // IDEMPOTENT. Without this guard, calling twice would create a second mutex
  // (leaking the first, which tasks would still be synchronising on) and
  // a second task fighting over the same UART. It never happened because the only
  // caller was boot; now there is the re-probe path.
  if (s_servico_ativo) {
    // A stop in progress must not be run over by a start: the old
    // task is still alive and the UART still belongs to it.
    if (parada_em_curso(&s_parada)) {
      ESP_LOGW(TAG, "still stopping the previous service - won't start on top of it");
    } else {
      ESP_LOGI(TAG, "service already up - nothing to do");
    }
    return;
  }

  tab5_sd_carona(mesh_na_carona);   // idempotent: the piggyback is registered once
  if (!s_fila_envio) { s_fila_envio = xQueueCreateWithCaps(MESH_FILA_ENVIO, sizeof(envio_fila_t), MALLOC_CAP_SPIRAM); }
  if (s_fila_envio) { xQueueReset(s_fila_envio); }
  tab5_hora_ao_saltar(ao_saltar_relogio);   // idempotent


  s_notif = notif_registrar("Meshtastic");
  notif_associar_app(s_notif, "Mesh");   // tapping the notice opens the Mesh app
  // The preference (ajustes) rules from the MOMENT the source
  // is born - this registration happens again on every probe that finds the
  // module again (cable swap), so without this the mute setting would "forget" on every
  // hot-swap.
  if (s_notif >= 0) { notif_silenciar(s_notif, !ajustes_beep_mesh()); }
  memset(s_nos, 0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  // THE SEEN-IDS RING IS NOT ZEROED: switching path (Grove -> internet) or
  // reconnecting the cable doesn't change which messages are already on the SD card. Zeroed, the
  // same message heard by the radio and then via the MQTT bridge went into the
  // history twice.
  memset(&s_saude, 0, sizeof(s_saude));   // health belongs to this path's node, not the previous one's

  ++s_conv_ver;   // the column starts over

  // THE CHANNELS EXIST FROM THE START, without waiting for a message: the active ones from the
  // last known configuration (NVS), or the primary with none. On a quiet
  // mesh, without this, there was nowhere to write. The deleted ones are left out
  // - and they used to be recreated here, channel 0 always, even when deleted.
  criar_conversas_dos_canais();

  // The nodes the Tab5 already knew (beyond the radio's ceiling).
  carregar_nodedb();
  mesh_diario_carregar();   // node restarts already noted (SD card)
  mesh_wifi_no_carregar();  // the node's Wi-Fi networks already used (SD card)

  // FAVOURITES IN THE TABLE FROM THE START, even without having spoken: they are fixed.
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { achar_no(s_favs[i]); }

  // RESTORES WHAT THE memset() ABOVE HAS JUST ERASED - observed defect:
  // opening the app BEFORE plugging in the C6L already loads everything from the SD card
  // (ao_abrir() calls tab5_mesh_carregar_do_cartao()), but the Grove probe
  // that arrives afterwards, finding the module, zeroes s_conv[] a few milliseconds
  // later - the list keeps only channel 0 (line above) until the user leaves and
  // comes back to the app, which forces a new ao_abrir(). Calling again here, right
  // after the memset, closes the same gap without depending on the user reopening anything.
  // Idempotent (achar_ou_criar_conversa finds what already exists instead of
  // duplicating) and safe in this task: tab5_mesh_iniciar() only runs inside
  // the Grove probe (kernel/grove.cpp), the lowest-priority task in the
  // system, made to put up with slow I/O (here, mounting the SD card) without stalling
  // touch.
  tab5_mesh_carregar_do_cartao();

  // The conversations were reborn with the stored unread counts (guardar_nao_lidas):
  // the bar now counts from them, and not from what was left published.
  travar();
  publicar_notif();
  destravar();

  s_via = via;
  if (s_via == AJUSTES_MESH_INTERNET) {
    // No cable and no radio: the service loop opens MQTT (usar_internet).
  } else if (s_via == AJUSTES_MESH_BLUETOOTH) {
    // No Grove and no UART: the BLE link looks for the node, connects and delivers
    // whatever arrives. Bluetooth has already been switched on by the caller.
    if (!mesh_ble_iniciar()) { ESP_LOGE(TAG, "Bluetooth link didn't start"); return; }
  } else {
  uart_config_t cfg = { };
  cfg.baud_rate = MESH_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (!tab5_grove_tomar(GROVE_DONO_MESH)) {
    ESP_LOGE(TAG, "Grove busy - service won't start");
    return;
  }

  esp_log_level_set("uart", ESP_LOG_ERROR);   // old Ex_I2C reservation, see tab5_grove
  memset(&s_g, 0, sizeof(s_g));
  if (uart_driver_install(MESH_UART, 4096, 512, 20, &s_uart_ev, 0) != ESP_OK
   || uart_param_config(MESH_UART, &cfg) != ESP_OK
   || uart_set_pin(MESH_UART, MESH_TX, MESH_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    esp_log_level_set("uart", ESP_LOG_WARN);
    // UNDOES WHAT HAS ALREADY BEEN TAKEN. The service isn't active yet, so
    // tab5_mesh_parar() wouldn't release anything: without this Grove stayed with Mesh
    // forever and the GPS got "Grove busy". The driver may have been
    // installed before param_config/set_pin failed.
    if (uart_is_driver_installed(MESH_UART)) { uart_driver_delete(MESH_UART); }
    s_uart_ev = nullptr;
    tab5_grove_soltar(GROVE_DONO_MESH);
    ESP_LOGE(TAG, "couldn't open the Grove UART");
    return;
  }
  esp_log_level_set("uart", ESP_LOG_WARN);
  // THE UART DOESN'T RECEIVE WITH THE P4 IN LIGHT SLEEP. With the screen off the P4 sleeps
  // between events (power-saving contract, kernel/app.h), and what the node sends
  // in that interval is lost: messages that never arrived and cut frames
  // turning into nodes with absurd numbers in the NodeDB. With the node on Grove, light sleep
  // is held off while the service runs (the frequency still drops).
  sistema_recurso(RECURSO_SERIAL_GROVE, "Meshtastic", true);
  }   // via serial

  // Priority 3, same as WiFi: below the UI (4), so as never to stall
  // touch; above the logger (1), because losing a serial byte is irreversible.
  // ACTIVE BEFORE THE TASK EXISTS: with priority 3 it starts immediately,
  // before the caller (the probe, priority 1) returns - and already sends.
  s_iniciado_em_ms = tab5_agora_ms();   // see the notice in tab5_mesh_silencio_ms()
  s_servico_ativo  = true;
  // 6 KB: with 4 KB the monitor recorded a minimum of 264 bytes free in real use.
  if (!parada_preparar(&s_parada)
   || !pool_criar(nullptr, vTaskMesh, nullptr, "mesh", 6144, 3, DONO_SERVICO, "Mesh")) {
    // GIVES BACK WHAT INICIAR TOOK. Previously only the flag went back: the UART and
    // Grove stayed with Mesh forever, and no probe found anything
    // on the cable again until a restart.
    parada_desistir(&s_parada);
    s_servico_ativo = false;
    if (s_via == AJUSTES_MESH_BLUETOOTH) { mesh_ble_parar(); }
    else if (s_via == AJUSTES_MESH_SERIAL) {
      uart_driver_delete(MESH_UART); s_uart_ev = nullptr; tab5_grove_soltar(GROVE_DONO_MESH);
      sistema_recurso(RECURSO_SERIAL_GROVE, "Meshtastic", false);
    }
    ESP_LOGE(TAG, "out of memory for the Mesh task");
    return;
  }
  sistema_avisar(AVISO_MESH_NOS);
  tab5_mem_marco("Mesh (service)");
  if (s_via == AJUSTES_MESH_INTERNET) { ESP_LOGI(TAG, "service on air (internet only, no node)"); }
  else if (s_via == AJUSTES_MESH_BLUETOOTH) { ESP_LOGI(TAG, "service on air (Bluetooth)"); }
  else { ESP_LOGI(TAG, "service on air (Grove %d baud, RX=%d TX=%d)", MESH_BAUD, MESH_RX, MESH_TX); }
}
