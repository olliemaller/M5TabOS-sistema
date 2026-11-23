// Meshtastic node configuration - see the big notice in tab5_mesh_admin.hpp.
#include "tab5_mesh_admin.hpp"
#include "idioma.h"
#include "mesh_pb.hpp"   // the service's codec, a single one for reading and writing

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app.h"          // sistema_avisar
#include "tab5_gps.hpp"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "ajustes.h"       // AJUSTES_MESH_BLUETOOTH

static const char* TAG = "mesh_admin";

// ---------------------------------------------------------------------------
// Provided by the service (tab5_mesh.cpp) - not part of the public API.
// ---------------------------------------------------------------------------
bool tab5_mesh_interno_admin(const uint8_t* adm, size_t n);   // AdminMessage to our node
bool tab5_mesh_interno_pode_admin(void);                      // node on the other end, full dump
void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou);   // request the configuration shortly

// AdminMessage - field numbers checked against admin.pb.h (2.5 to current).
#define ADM_SET_OWNER          32
#define ADM_SET_CHANNEL        33
#define ADM_SET_CONFIG         34
#define ADM_SET_MODULE_CONFIG  35
#define ADM_SET_FIXED_POSITION 41
#define ADM_REMOVE_FIXED_POS   42
#define ADM_SET_TIME_ONLY      43
#define ADM_BEGIN_EDIT         64
#define ADM_COMMIT_EDIT        65
#define ADM_REBOOT_SECONDS     97

// Data.payload max_size (mesh.pb.h): the whole AdminMessage has to fit.
#define ADM_MAX 233

// Sections: Config 1..10 (device, position, power, network, display, lora,
// bluetooth, security, sessionkey, device_ui) and ModuleConfig 1..17.
#define CFG_SECOES 11
#define MOD_SECOES 18
#define BRUTO_MAX  256

typedef struct { bool tem; uint16_t n; uint8_t b[BRUTO_MAX]; } bruto_t;

// What each editable field is in the protobuf. All varint.
typedef struct { bool modulo; uint8_t secao; uint8_t num; } campo_def_t;
static const campo_def_t CAMPOS[MESH_CFG_N] = {
  /* PAPEL       */ { false, 1,  1 },   // DeviceConfig.role
  /* NODEINFO_S  */ { false, 1,  7 },   // DeviceConfig.node_info_broadcast_secs
  /* POS_S       */ { false, 2,  1 },   // PositionConfig.position_broadcast_secs
  /* SALTOS      */ { false, 6,  8 },   // LoRaConfig.hop_limit
  /* TX_DBM      */ { false, 6, 10 },   // LoRaConfig.tx_power (int32)
  /* BT          */ { false, 7,  1 },   // BluetoothConfig.enabled
  /* MQTT        */ { true,  1,  1 },   // ModuleConfig.MQTTConfig.enabled
  /* TELE_S      */ { true,  6,  1 },   // ModuleConfig.TelemetryConfig.device_update_interval
  /* MQTT_PROXY  */ { true,  1,  9 },   // ModuleConfig.MQTTConfig.proxy_to_client_enabled
  /* POS_INTELIG */ { false, 2,  2 },   // PositionConfig.position_broadcast_smart_enabled
  /* GPS_MODO    */ { false, 2, 13 },   // PositionConfig.gps_mode
  /* TELA_S      */ { false, 5,  1 },   // DisplayConfig.screen_on_secs
  /* OK_MQTT     */ { false, 6, 105 },  // LoRaConfig.config_ok_to_mqtt
};

// Sections that are NEVER resent (see the .hpp): power 3, network 4,
// security 8, sessionkey 9; serial module 2.
static bool secao_proibida(bool modulo, int secao)
{
  return modulo ? (secao == 2) : (secao == 3 || secao == 4 || secao == 8 || secao == 9);
}

// Written by the service task, read by the interface: a spinlock, because the
// critical sections are copies and scans of a few hundred bytes.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static bruto_t s_cfg[CFG_SECOES];
EXT_RAM_BSS_ATTR static bruto_t s_mod[MOD_SECOES];
EXT_RAM_BSS_ATTR static bruto_t s_user, s_meta, s_myinfo;
EXT_RAM_BSS_ATTR static bruto_t s_canal0;   // the whole PRIMARY Channel (index 0)
static char     s_aviso[96];
static uint32_t s_aviso_ms = 0;
static bool     s_dump_ok = false;
static bool     s_gravando = false;
static uint32_t s_gravando_ms = 0;
static char     s_motivo[72];   // why the last mesh_cfg_gravar sent nothing (for the screen)

#define GRAVANDO_PRAZO_MS 90000   // the node restarts in ~5 s; 90 s covers a slow boot


// ---------------------------------------------------------------------------
// Service hooks
// ---------------------------------------------------------------------------
static void guardar(bruto_t* d, const uint8_t* b, size_t n)
{
  if (n > BRUTO_MAX) {
    // Larger than the buffer: storing it truncated and resending later would lose fields
    // on the node. Mark it unknown - editing it is blocked.
    ESP_LOGD(TAG, "section of %u bytes does not fit - editing it disabled", (unsigned)n);
    taskENTER_CRITICAL(&s_mux); d->tem = false; d->n = 0; taskEXIT_CRITICAL(&s_mux);
    return;
  }
  taskENTER_CRITICAL(&s_mux);
  memcpy(d->b, b, n); d->n = (uint16_t)n; d->tem = true;
  taskEXIT_CRITICAL(&s_mux);
}

void mesh_admin_guardar_secao(bool modulo, int secao, const uint8_t* b, size_t n)
{
  if (modulo) { if (secao >= 1 && secao < MOD_SECOES) { guardar(&s_mod[secao], b, n); } }
  else        { if (secao >= 1 && secao < CFG_SECOES) { guardar(&s_cfg[secao], b, n); } }
}
static void despejar_um(const char* nome, int secao, const bruto_t& b)
{
  if (!b.tem) { printf("  %s %d: (not received)\n", nome, secao); return; }
  printf("  %s %d (%u bytes):", nome, secao, (unsigned)b.n);
  pb_leitor_t l = pb_leitor(b.b, b.n);
  pb_campo_t c;
  while (pb_campo(&l, &c)) {
    if (c.tipo == 0)      { printf(" %d=%llu", c.num, (unsigned long long)c.v); }
    else if (c.tipo == 2) { printf(" %d=[%u]", c.num, (unsigned)c.n_val); }
    else                  { printf(" %d=0x%08lx", c.num, (unsigned long)c.v); }
  }
  printf("\n");
}

void mesh_admin_despejar(void)
{
  // Copy before printing: printf with the spinlock held would take too long.
  // In PSRAM: ~7.5 KB that only this console command uses.
  EXT_RAM_BSS_ATTR static bruto_t cfg[CFG_SECOES], mod[MOD_SECOES], c0;
  taskENTER_CRITICAL(&s_mux);
  memcpy(cfg, s_cfg, sizeof(cfg)); memcpy(mod, s_mod, sizeof(mod)); c0 = s_canal0;
  taskEXIT_CRITICAL(&s_mux);
  printf("Config (1 device, 2 position, 4 network, 6 lora, 7 bluetooth):\n");
  for (int i = 1; i < CFG_SECOES; ++i) { if (i != 8 && i != 9) { despejar_um("config", i, cfg[i]); } }
  printf("ModuleConfig (1 mqtt, 2 serial, 6 telemetry):\n");
  for (int i = 1; i < MOD_SECOES; ++i) { if (mod[i].tem) { despejar_um("modulo", i, mod[i]); } }
  despejar_um("canal", 0, c0);
}

void mesh_admin_guardar_user(const uint8_t* b, size_t n)     { guardar(&s_user, b, n); }
void mesh_admin_guardar_metadata(const uint8_t* b, size_t n) { guardar(&s_meta, b, n); }
void mesh_admin_guardar_my_info(const uint8_t* b, size_t n)  { guardar(&s_myinfo, b, n); }
void mesh_admin_guardar_canal(int idx, const uint8_t* b, size_t n) { if (idx == 0) { guardar(&s_canal0, b, n); } }

void mesh_admin_guardar_aviso(const uint8_t* b, size_t n)
{
  char msg[sizeof(s_aviso)] = "";
  if (!pb_achar_texto(b, n, 4, msg, sizeof(msg)) || !msg[0]) { return; }   // ClientNotification.message
  ESP_LOGW(TAG, "notice from the node: %s", msg);
  taskENTER_CRITICAL(&s_mux);
  memcpy(s_aviso, msg, sizeof(s_aviso));
  s_aviso_ms = tab5_agora_ms() | 1u;
  taskEXIT_CRITICAL(&s_mux);
  sistema_avisar(AVISO_MESH_NOS);
}

void mesh_admin_inicio_dump(void) { s_dump_ok = false; }

void mesh_admin_fim_dump(void)
{
  s_dump_ok = true;
  if (s_gravando) { ESP_LOGI(TAG, "node reappeared - new values confirmed"); }
  s_gravando = false;
  sistema_avisar(AVISO_MESH_NOS);
}

// KEEPS TRYING while the node is not back. The first configuration request goes out 15 s
// after writing (tab5_mesh_interno_reapresentar_em); a slower boot
// misses that request, and without this the next one would only go out at the service's
// silence rhythm (minutes). Every 20 s until the dump arrives or the timeout ends.
void mesh_admin_tique(uint32_t t)
{
  static uint32_t ultimo = 0;
  if (!s_gravando) { ultimo = 0; return; }
  // THE TIMEOUT APPLIES HERE TOO, and not only when the screen asks (estado()):
  // with the page closed, a node that never comes back would be asked every 20 s
  // forever.
  if (t - s_gravando_ms > GRAVANDO_PRAZO_MS) {
    ESP_LOGW(TAG, "the node did not reappear within %d s of writing", GRAVANDO_PRAZO_MS / 1000);
    s_gravando = false;
    return;
  }
  if (t - s_gravando_ms < 35000) { return; }
  if (ultimo == 0 || t - ultimo >= 20000) {
    ultimo = t;
    ESP_LOGI(TAG, "the node is not back yet - requesting the configuration again");
    tab5_mesh_interno_reapresentar_em(0, true);
  }
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
// POSITION PRECISION ON THE CHANNEL: Channel{ 2: settings{ 7: module_settings{
// 1: position_precision } } }. Without module_settings the firmware treats the
// primary like channels from before the field existed: 13 bits (~3 km) - see
// PositionModule::allocPositionPacket. With the lock held.
static int32_t precisao_de(const bruto_t& c)
{
  if (!c.tem) { return -1; }
  pb_campo_t st, mod;
  if (!pb_achar(c.b, c.n, 2, PB_LEN, &st)) { return 13; }
  if (!pb_achar(st.val, st.n_val, 7, PB_LEN, &mod)) { return 13; }
  uint64_t v;
  return pb_achar_varint(mod.val, mod.n_val, 1, &v) ? (int32_t)v : 0;
}

// CHANNEL UPLINK: Channel{ 2: settings{ 5: uplink_enabled } }. Off, the
// node does not take to MQTT what it receives over the radio on this channel. With the lock held.
static int8_t uplink_de(const bruto_t& c)
{
  if (!c.tem) { return -1; }
  pb_campo_t st;
  if (!pb_achar(c.b, c.n, 2, PB_LEN, &st)) { return 0; }
  uint64_t v;
  return pb_achar_varint(st.val, st.n_val, 5, &v) && v ? 1 : 0;
}

static mesh_cfg_estado_t estado(void)
{
  if (s_gravando && (tab5_agora_ms() - s_gravando_ms) > GRAVANDO_PRAZO_MS) {
    ESP_LOGW(TAG, "the node did not reappear within %d s of writing", GRAVANDO_PRAZO_MS / 1000);
    s_gravando = false;
  }
  if (s_gravando) { return MESH_CFG_GRAVANDO; }
  return (s_dump_ok && tab5_mesh_interno_pode_admin()) ? MESH_CFG_PRONTO : MESH_CFG_SEM_NO;
}

// The field's value as the person sees it: the pending one, if touched; otherwise the node's.
static int32_t valor_pendente(const mesh_cfg_mudanca_t* m, const mesh_cfg_t* c, int campo)
{
  return m->muda[campo] ? m->v[campo] : c->v[campo];
}

void mesh_cfg_mqtt_ligar(mesh_cfg_mudanca_t* m, const mesh_cfg_t* c, bool ligar)
{
  if (!ligar && valor_pendente(m, c, MESH_CFG_MQTT_PROXY) && c->conhecido[MESH_CFG_MQTT_PROXY]) {
    m->v[MESH_CFG_MQTT_PROXY] = 0;
    m->muda[MESH_CFG_MQTT_PROXY] = (c->v[MESH_CFG_MQTT_PROXY] != 0);
  }
  if (ligar && m->muda[MESH_CFG_MQTT_PROXY] && c->v[MESH_CFG_MQTT_PROXY]) { m->muda[MESH_CFG_MQTT_PROXY] = false; }
}

void mesh_cfg_proxy_ligar(mesh_cfg_mudanca_t* m, const mesh_cfg_t* c, bool ligar)
{
  if (ligar && !valor_pendente(m, c, MESH_CFG_MQTT) && c->conhecido[MESH_CFG_MQTT]) {
    m->v[MESH_CFG_MQTT] = 1;
    m->muda[MESH_CFG_MQTT] = (c->v[MESH_CFG_MQTT] != 1);
  }
  if (!ligar && m->muda[MESH_CFG_MQTT] && !c->v[MESH_CFG_MQTT]) { m->muda[MESH_CFG_MQTT] = false; }
}

void mesh_cfg_ler(mesh_cfg_t* o)
{
  memset(o, 0, sizeof(*o));
  o->estado = (uint8_t)estado();
  o->regiao = -1;
  o->nos_no_radio = -1;
  uint64_t v;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < MESH_CFG_N; ++i) {
    const campo_def_t& c = CAMPOS[i];
    const bruto_t& b = c.modulo ? s_mod[c.secao] : s_cfg[c.secao];
    o->conhecido[i] = b.tem;
    o->v[i] = (b.tem && pb_achar_varint(b.b, b.n, c.num, &v)) ? (int32_t)v : 0;
  }
  if (s_cfg[2].tem && pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 3, &v)) { o->pos_fixa = (v != 0); }
  o->precisao = precisao_de(s_canal0);
  o->uplink   = uplink_de(s_canal0);
  if (s_cfg[6].tem) {
    o->regiao     = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 7, &v) ? (int32_t)v : 0;
    o->preset     = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 2, &v) ? (int32_t)v : 0;
    o->usa_preset = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 1, &v) && v != 0;
  }
  if (s_cfg[4].tem) {
    o->wifi_conhecido = true;
    o->wifi_ligado = pb_achar_varint(s_cfg[4].b, s_cfg[4].n, 1, &v) && v != 0;
    pb_achar_texto(s_cfg[4].b, s_cfg[4].n, 3, o->wifi_ssid, sizeof(o->wifi_ssid));
  }
  if (s_meta.tem) {
    pb_achar_texto(s_meta.b, s_meta.n, 1, o->firmware, sizeof(o->firmware));
    if (pb_achar_varint(s_meta.b, s_meta.n, 9, &v)) { o->hw = (uint8_t)v; }
    o->tem_wifi = pb_achar_varint(s_meta.b, s_meta.n, 4, &v) && v;
    o->tem_bt   = pb_achar_varint(s_meta.b, s_meta.n, 5, &v) && v;
  }
  if (s_myinfo.tem) {
    if (pb_achar_varint(s_myinfo.b, s_myinfo.n, 8, &v))  { o->reinicios = (uint32_t)v; }
    // nodedb_count (15) only exists in newer firmware: if absent it stays -1, and the screen
    // does not make up a number.
    if (pb_achar_varint(s_myinfo.b, s_myinfo.n, 15, &v)) { o->nos_no_radio = (int32_t)v; }
  }
  memcpy(o->aviso, s_aviso, sizeof(o->aviso));
  o->aviso_ms = s_aviso_ms;
  taskEXIT_CRITICAL(&s_mux);
}

bool mesh_admin_no_tem_posicao_propria(void)
{
  uint64_t fixa = 0, gps = 0;
  taskENTER_CRITICAL(&s_mux);
  if (s_cfg[2].tem) {
    pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 3, &fixa);    // fixed_position
    pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 13, &gps);    // gps_mode (1 = ENABLED)
  }
  taskEXIT_CRITICAL(&s_mux);
  return fixa != 0 || gps == 1;
}

int32_t mesh_admin_precisao_primario(void)
{
  taskENTER_CRITICAL(&s_mux);
  const int32_t p = precisao_de(s_canal0);
  taskEXIT_CRITICAL(&s_mux);
  return p;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------
// AdminMessage{ field: varint value }
static bool admin_varint(int campo, uint64_t valor)
{
  uint8_t a[16];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, campo, valor);
  return tab5_mesh_interno_admin(a, pb_tamanho(&w));
}

// AdminMessage{ field: <already-built submessage> } into 'out'. 0 = does not fit
// in the buffer OR exceeds what a packet carries (ADM_MAX) - in both cases the
// section cannot go, and the caller refuses the whole write.
static size_t admin_msg(uint8_t* out, size_t tam, int campo, const pb_escritor_t* corpo)
{
  pb_escritor_t w = pb_escritor(out, tam);
  pb_w_sub(&w, campo, corpo);
  const size_t n = pb_tamanho(&w);
  return n <= ADM_MAX ? n : 0;
}

// THE RAW BYTES STORED FROM THE DUMP, as the start of a section to resend. The changed
// fields go AFTER it: nanopb keeps the last occurrence of each
// field, so the new value beats the old without rewriting the section.
// nullptr = ready to receive the fields; otherwise, WHY not (for the log).
static const char* comecar_do_bruto(pb_escritor_t* w, const bruto_t& b)
{
  taskENTER_CRITICAL(&s_mux);
  const bool tem = b.tem;
  if (tem) { pb_w_cru(w, b.b, b.n); }
  taskEXIT_CRITICAL(&s_mux);
  if (!tem)   { return tr("nao veio no dump"); }
  if (!w->ok) { return tr("e grande demais para um pacote"); }
  return nullptr;
}

// THE PRIMARY CHANNEL with another precision and/or another uplink (-1 = keep): the
// stored Channel field by field, raw, except what changes. The precision lives in
// settings.module_settings, a SUBMESSAGE - two occurrences of it do not guarantee the
// last winning field by field, so it is rebuilt (is_muted stays
// the same). The uplink (settings.uplink_enabled) is scalar: the old goes out, the new goes in.
static bool montar_canal(pb_escritor_t* w, int32_t precisao, int uplink)
{
  EXT_RAM_BSS_ATTR static bruto_t c;   // only the interface writes
  taskENTER_CRITICAL(&s_mux);
  c = s_canal0;
  taskEXIT_CRITICAL(&s_mux);
  if (!c.tem) { return false; }
  pb_leitor_t l = pb_leitor(c.b, c.n);
  pb_campo_t f;
  bool tem_settings = false;
  while (pb_campo(&l, &f)) {
    if (!(f.num == 2 && f.tipo == PB_LEN)) { pb_w_cru(w, f.ini, f.n_total); continue; }
    tem_settings = true;
    uint8_t sb[160];
    pb_escritor_t ws = pb_escritor(sb, sizeof(sb));
    pb_leitor_t ls = pb_leitor(f.val, f.n_val);
    pb_campo_t g;
    uint8_t mb[16];
    pb_escritor_t wm = pb_escritor(mb, sizeof(mb));
    if (precisao >= 0) { pb_w_num(&wm, 1, (uint64_t)precisao); }   // position_precision
    while (pb_campo(&ls, &g)) {
      if (precisao >= 0 && g.num == 7 && g.tipo == PB_LEN) {    // module_settings: the rest of it
        pb_leitor_t lm = pb_leitor(g.val, g.n_val);
        pb_campo_t h;
        while (pb_campo(&lm, &h)) { if (h.num != 1) { pb_w_cru(&wm, h.ini, h.n_total); } }
      } else if (uplink >= 0 && g.num == 5) {
        continue;                                               // uplink_enabled: the new one goes
      } else {
        pb_w_cru(&ws, g.ini, g.n_total);
      }
    }
    if (uplink >= 0)   { pb_w_num(&ws, 5, (uint64_t)uplink); }
    if (precisao >= 0) { pb_w_sub(&ws, 7, &wm); }
    if (!ws.ok || !wm.ok) { return false; }
    pb_w_sub(w, 2, &ws);
  }
  return tem_settings && w->ok;
}

const char* mesh_cfg_motivo(void) { return s_motivo; }

bool mesh_cfg_gravar(const mesh_cfg_mudanca_t* m)
{
  s_motivo[0] = 0;
  if (estado() != MESH_CFG_PRONTO) {
    ESP_LOGW(TAG, "write refused: no node or incomplete dump");
    snprintf(s_motivo, sizeof(s_motivo), tr("sem o nó, ou a configuração ainda chegando"));
    return false;
  }

  // BUILD EVERYTHING BEFORE SENDING ANYTHING. If a section cannot go
  // (unknown, forbidden, too big), nothing goes out - half a transaction
  // written would be a configuration nobody asked for.
  // device, position, lora, bluetooth, mqtt, telemetry + network + the primary
  // channel + the owner = 9.
  enum { MAX_MSGS = 10 };
  EXT_RAM_BSS_ATTR static uint8_t msgs[MAX_MSGS][ADM_MAX + 16];   // only the interface writes
  size_t tam[MAX_MSGS];
  int n_msgs = 0;

  bool feito[MESH_CFG_N] = { };
  int msg_mqtt = -1;   // index, in msgs[], of the MQTT section (see the Bluetooth rule below)
  for (int i = 0; i < MESH_CFG_N; ++i) {
    if (!m->muda[i] || feito[i]) { continue; }
    const campo_def_t& c = CAMPOS[i];
    if (secao_proibida(c.modulo, c.secao)) { ESP_LOGE(TAG, "forbidden section"); return false; }

    // The stored section + ALL its changed fields, at the end.
    EXT_RAM_BSS_ATTR static uint8_t corpo[ADM_MAX];
    pb_escritor_t wc = pb_escritor(corpo, sizeof(corpo));
    if (const char* porque = comecar_do_bruto(&wc, c.modulo ? s_mod[c.secao] : s_cfg[c.secao])) {
      ESP_LOGW(TAG, "section %s %d %s - cannot write", c.modulo ? "module" : "config", c.secao, porque);
      return false;
    }
    for (int k = i; k < MESH_CFG_N; ++k) {
      if (!m->muda[k] || CAMPOS[k].modulo != c.modulo || CAMPOS[k].secao != c.secao) { continue; }
      pb_w_int32(&wc, CAMPOS[k].num, m->v[k]);
      feito[k] = true;
    }

    // Config{ section: body } or ModuleConfig{ section: body }, inside the
    // AdminMessage{ set_config | set_module_config }.
    EXT_RAM_BSS_ATTR static uint8_t sec[ADM_MAX];
    pb_escritor_t ws = pb_escritor(sec, sizeof(sec));
    pb_w_sub(&ws, c.secao, &wc);
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), c.modulo ? ADM_SET_MODULE_CONFIG : ADM_SET_CONFIG, &ws);
    if (!tam[n_msgs]) {
      ESP_LOGE(TAG, "section too big for one packet");
      snprintf(s_motivo, sizeof(s_motivo), tr("configuração grande demais para um pacote"));
      return false;
    }
    if (c.modulo && c.secao == 1) { msg_mqtt = n_msgs; }
    ++n_msgs;
  }

  // NODE WI-FI: Config.network = stored section + wifi_enabled(1) + ssid(3)
  // + psk(4) at the end. The password does not come back from the node, so it only goes with name AND password.
  if (m->wifi_ssid[0] || m->wifi_psk[0] || m->wifi_muda) {
    const bool tem_rede = m->wifi_ssid[0] || m->wifi_psk[0];
    if (tem_rede && (!m->wifi_ssid[0] || (!m->wifi_psk[0] && !m->wifi_aberta))) { ESP_LOGW(TAG, "wifi: needs name and password together"); return false; }
    EXT_RAM_BSS_ATTR static uint8_t corpo[ADM_MAX];
    pb_escritor_t wc = pb_escritor(corpo, sizeof(corpo));
    if (const char* porque = comecar_do_bruto(&wc, s_cfg[4])) { ESP_LOGW(TAG, "wifi: the network section %s - nothing sent", porque); return false; }
    pb_w_num(&wc, 1, m->wifi_liga ? 1 : 0);                    // wifi_enabled
    if (tem_rede) {
      pb_w_texto(&wc, 3, m->wifi_ssid, 32);                    // wifi_ssid
      pb_w_texto(&wc, 4, m->wifi_psk, 64);                     // wifi_psk
    }
    EXT_RAM_BSS_ATTR static uint8_t sec[ADM_MAX];
    pb_escritor_t ws = pb_escritor(sec, sizeof(sec));
    pb_w_sub(&ws, 4, &wc);                                     // Config.network = 4
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_CONFIG, &ws);
    if (!tam[n_msgs]) { ESP_LOGE(TAG, "wifi: section too big for one packet"); return false; }
    ++n_msgs;
  }

  // THE PRIMARY CHANNEL: set_channel with the new precision and/or uplink.
  if (m->precisao_muda || m->uplink_muda) {
    EXT_RAM_BSS_ATTR static uint8_t corpo[ADM_MAX];
    pb_escritor_t wc = pb_escritor(corpo, sizeof(corpo));
    if (!montar_canal(&wc, m->precisao_muda ? m->precisao : -1, m->uplink_muda ? (m->uplink ? 1 : 0) : -1)) {
      ESP_LOGW(TAG, "primary channel: not in the dump or does not fit - nothing sent");
      snprintf(s_motivo, sizeof(s_motivo), "o canal principal do nó ainda não chegou");
      return false;
    }
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_CHANNEL, &wc);
    if (!tam[n_msgs]) { ESP_LOGE(TAG, "channel too big for one packet"); return false; }
    ++n_msgs;
  }

  // NAMES: our node's stored User + the names at the end. Resending the whole
  // User preserves what set_owner also reads (is_licensed,
  // is_unmessagable) - with only the names, a ham radio node would lose its licence.
  if (m->longo[0] || m->curto[0]) {
    EXT_RAM_BSS_ATTR static uint8_t u[ADM_MAX];
    pb_escritor_t wu = pb_escritor(u, sizeof(u));
    // Without the node's User it does not go: with only the names, set_owner would turn off
    // is_licensed on a ham radio node (it compares the field, absent = false).
    if (const char* porque = comecar_do_bruto(&wu, s_user)) { ESP_LOGW(TAG, "names: our node's User %s - nothing sent", porque); return false; }
    if (m->longo[0]) { pb_w_texto(&wu, 2, m->longo, MESH_NOME_LONGO - 1); }   // long_name
    if (m->curto[0]) { pb_w_texto(&wu, 3, m->curto, MESH_NOME_CURTO - 1); }   // short_name
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_OWNER, &wu);
    if (!tam[n_msgs]) { ESP_LOGE(TAG, "name too big for one packet"); return false; }
    ++n_msgs;
  }

  if (n_msgs == 0) { return false; }

  // OVER BLUETOOTH THE MQTT SECTION GOES ALONE, WITHOUT A TRANSACTION. The firmware
  // (AdminModule::handleSetModuleConfig, mqtt case) calls disableBluetooth()
  // AS SOON AS it receives the MQTT section - even with the transaction open. Inside
  // begin/commit, BLE dropped before the commit: the transaction stayed open (the
  // firmware does not expire it), nothing was written and the node did not restart, now without
  // Bluetooth until someone unplugged it.
  // Outside the transaction the same set_module_config validates, writes to disk and
  // restarts the node by itself (saveChanges with shouldReboot) - BLE dropping straight
  // after no longer gets in the way. That is why over Bluetooth the MQTT section
  // must be the ONLY change: any other, after it, would not arrive.
  // Over the cable there is no Bluetooth to drop, and the normal transaction applies.
  if (msg_mqtt >= 0 && tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH) {
    if (n_msgs > 1) {
      ESP_LOGW(TAG, "over Bluetooth the MQTT section is written alone - other changes pending with it");
      snprintf(s_motivo, sizeof(s_motivo), "pelo Bluetooth, MQTT/proxy grava sozinho: grave o resto antes");
      return false;
    }
    if (!tab5_mesh_interno_admin(msgs[msg_mqtt], tam[msg_mqtt])) {
      snprintf(s_motivo, sizeof(s_motivo), tr("o enlace com o nó recusou o envio"));
      return false;
    }
    ESP_LOGI(TAG, "MQTT section sent without a transaction (Bluetooth) - the node writes, turns BLE off and restarts");
    s_gravando = true;
    s_gravando_ms = tab5_agora_ms();
    tab5_mesh_interno_reapresentar_em(15000, true);
    return true;
  }

  // begin -> sections -> commit. The commit writes and restarts the node.
  bool ok = admin_varint(ADM_BEGIN_EDIT, 1);
  for (int i = 0; ok && i < n_msgs; ++i) { ok = tab5_mesh_interno_admin(msgs[i], tam[i]); }
  if (ok) { ok = admin_varint(ADM_COMMIT_EDIT, 1); }
  if (!ok) {
    // Transaction open without commit. The firmware does NOT expire it: the changes stay
    // only in the node's RAM, unwritten, until it restarts.
    ESP_LOGE(TAG, "link refused mid-transaction - the node stays unwritten until it restarts");
    snprintf(s_motivo, sizeof(s_motivo), tr("o enlace caiu no meio da gravação - reinicie o nó"));
    return false;
  }
  ESP_LOGI(TAG, "transaction sent (%d message(s)) - the node writes and restarts", n_msgs);
  s_gravando = true;
  s_gravando_ms = tab5_agora_ms();
  // Over serial the restarted node does not speak by itself: request the configuration after
  // boot (BLE reconnects and asks by itself; asking again does no harm).
  tab5_mesh_interno_reapresentar_em(15000, true);
  return true;
}

bool mesh_cfg_posicao_fixa_do_gps(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const nmea_posicao_t g = tab5_gps_posicao();
  if (!g.valida || g.qualidade == 0) { ESP_LOGW(TAG, "fixed position: Tab5 GPS has no fix"); return false; }
  // The same Position the service sends to the mesh (mesh_pb_posicao).
  uint8_t pos[32];
  pb_escritor_t wp = pb_escritor(pos, sizeof(pos));
  mesh_pb_posicao(&wp, (int32_t)(g.lat * 1e7), (int32_t)(g.lon * 1e7), (int32_t)g.altitude_m,
                  tab5_hora_epoch());
  uint8_t a[48];
  const size_t n = admin_msg(a, sizeof(a), ADM_SET_FIXED_POSITION, &wp);
  const bool ok = n && tab5_mesh_interno_admin(a, n);
  // The position section in the stored dump still says "not fixed": ask again.
  if (ok) { ESP_LOGI(TAG, "fixed position sent (%.5f, %.5f)", g.lat, g.lon); tab5_mesh_interno_reapresentar_em(3000, false); }
  return ok;
}

bool mesh_cfg_tirar_posicao_fixa(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const bool ok = admin_varint(ADM_REMOVE_FIXED_POS, 1);
  if (ok) { tab5_mesh_interno_reapresentar_em(3000, false); }
  return ok;
}

bool mesh_cfg_acertar_hora(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const uint32_t agora = tab5_hora_epoch();
  if (!agora) { ESP_LOGW(TAG, "Tab5 time not set yet"); return false; }
  uint8_t a[8];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_fixo32(&w, ADM_SET_TIME_ONLY, agora);
  return tab5_mesh_interno_admin(a, pb_tamanho(&w));
}

bool mesh_cfg_reiniciar(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const bool ok = admin_varint(ADM_REBOOT_SECONDS, 5);
  if (ok) {
    s_gravando = true;   // same wait: the node vanishes and comes back
    s_gravando_ms = tab5_agora_ms();
    tab5_mesh_interno_reapresentar_em(15000, true);
  }
  return ok;
}

bool mesh_admin_gravando(void) { return s_gravando; }
