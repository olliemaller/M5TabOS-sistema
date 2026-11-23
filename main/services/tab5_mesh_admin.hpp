// CONFIGURING THE MESHTASTIC NODE FROM THE TAB5 - no PC or phone needed.
//
// Speaks Meshtastic's own AdminMessage (port ADMIN_APP, 6), the same one the
// web client and the phone app use. Protocol details in docs/MESHTASTIC.md.
//
// HOW A CHANGE TRAVELS
//   1. The configuration dump (FromRadio 5 and 9) carries each whole SECTION; the
//      raw bytes of each are stored here (mesh_admin_guardar_*).
//   2. set_config REPLACES the whole section on the node. Sending only the changed
//      field would zero the others - so the stored section goes with the new fields
//      APPENDED at the end. In protobuf, a repeated scalar field takes the last
//      occurrence (that is how the firmware's nanopb decodes it).
//   3. All in one transaction: begin_edit_settings, the sections, set_owner,
//      commit_edit_settings. The node writes once and restarts by itself; the service
//      requests the configuration again and the screen shows the values the NODE
//      confirmed - never the ones we asked for.
//
// SAFETY
//   - Only with the node on the other end (cable or Bluetooth) and with the full dump.
//     There is no admin over the internet.
//   - LOCAL admin (no 'from') needs no session key: AdminModule.cpp,
//     from 2.5 to the current one. The key only matters for administering a REMOTE node.
//   - The node's Wi-Fi (network) is only resent when the user turns the
//     Wi-Fi on/off or picks/types a network (name AND password: the stored password
//     cannot be read back, so there is no "change only the name"). Only wifi_enabled,
//     ssid and psk change; the rest of the section stays as the node sent it.
//   - Sections this module never resends: network without the two fields above,
//     power (the dump swaps ls_secs for the default), security (keys) and the serial
//     module (it is the Grove link itself).
//   - The PRIMARY channel is only resent (set_channel) when the position
//     precision or the uplink (sending to MQTT what the node receives) change; name,
//     key, downlink and the rest stay as the node sent them.
//   - An AdminMessage over 233 bytes (the Data.payload ceiling) is refused here.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "tab5_mesh.hpp"

// What can be changed. Each is ONE field of ONE section (see CAMPOS in the .cpp).
typedef enum {
  MESH_CFG_PAPEL = 0,      // Config.device.role
  MESH_CFG_NODEINFO_S,     // Config.device.node_info_broadcast_secs
  MESH_CFG_POS_S,          // Config.position.position_broadcast_secs
  MESH_CFG_SALTOS,         // Config.lora.hop_limit
  MESH_CFG_TX_DBM,         // Config.lora.tx_power (0 = the region's legal maximum)
  MESH_CFG_BT,             // Config.bluetooth.enabled
  MESH_CFG_MQTT,           // ModuleConfig.mqtt.enabled
  MESH_CFG_TELE_S,         // ModuleConfig.telemetry.device_update_interval
  MESH_CFG_MQTT_PROXY,     // ModuleConfig.mqtt.proxy_to_client_enabled (the Tab5 lends its internet)
  MESH_CFG_POS_INTELIGENTE,// Config.position.position_broadcast_smart_enabled (sends when moving)
  MESH_CFG_GPS_MODO,       // Config.position.gps_mode: 0 off, 1 on, 2 no GPS on the board
  MESH_CFG_TELA_S,         // Config.display.screen_on_secs (0 = the firmware default, 10 min)
  // Config.lora.config_ok_to_mqtt: the node AUTHORISES ITS messages to go up
  // to MQTT via other gateways. It stamps each message with this
  // (Data.bitfield, OK_TO_MQTT - firmware Router.cpp); a gateway on a public
  // server (address that is not a private IP) drops another node's message
  // without the stamp (MQTT.cpp, onSend: "DontMqttMeBro"). Off is the default.
  MESH_CFG_OK_MQTT,
  MESH_CFG_N
} mesh_cfg_campo_t;

typedef enum {
  MESH_CFG_SEM_NO = 0,     // no link to the node, or dump not finished yet
  MESH_CFG_PRONTO,         // can read and write
  MESH_CFG_GRAVANDO,       // transaction sent; waiting for the node to restart and reappear
} mesh_cfg_estado_t;

typedef struct {
  uint8_t  estado;                 // mesh_cfg_estado_t
  bool     conhecido[MESH_CFG_N];  // the field's section came in the dump
  int32_t  v[MESH_CFG_N];          // value (proto3: absent = 0)

  // LOCATION ON THE MESH: ChannelSettings.module_settings.position_precision
  // of the PRIMARY channel - this is what turns position on and off in Meshtastic
  // (0 = the node sends no position at all; 32 = exact; 10..19 = region only,
  // with fewer bits). -1 = the channel did not come in the dump. Edited via
  // mesh_cfg_mudanca_t.precisao.
  int32_t  precisao;
  int8_t   uplink;                 // primary channel sends to MQTT what the node receives; -1 = did not come

  // Read only.
  bool     pos_fixa;               // Config.position.fixed_position
  int32_t  regiao;                 // Config.lora.region (-1 = did not come)
  int32_t  preset;                 // Config.lora.modem_preset
  bool     usa_preset;
  char     firmware[24];
  uint8_t  hw;                     // DeviceMetadata.hw_model
  bool     tem_wifi, tem_bt;
  uint32_t reinicios;              // MyNodeInfo.reboot_count
  int32_t  nos_no_radio;           // MyNodeInfo.nodedb_count; -1 = old firmware does not send it
  bool     wifi_conhecido;         // the network section came in the dump
  bool     wifi_ligado;            // Config.network.wifi_enabled
  char     wifi_ssid[33];          // Config.network.wifi_ssid (the password is never read)
  char     aviso[96];              // last ClientNotification from the node
  uint32_t aviso_ms;               // our uptime when it arrived; 0 = none
} mesh_cfg_t;

// Copies the current state (values confirmed by the node).
void mesh_cfg_ler(mesh_cfg_t* saida);

// One transaction with EVERYTHING that changed. muda[i] = true carries v[i]; empty
// names stay as they are. The node writes and restarts. false = nothing sent (no node, no
// dump, or unknown section) - and the reason goes to the log.
typedef struct {
  bool    muda[MESH_CFG_N];
  int32_t v[MESH_CFG_N];
  char    longo[MESH_NOME_LONGO];
  char    curto[MESH_NOME_CURTO];
  char    wifi_ssid[33];           // BOTH together or neither; turns the node's Wi-Fi on
  char    wifi_psk[65];
  bool    wifi_aberta;             // network without a password: psk goes empty
  bool    wifi_muda;               // touch wifi_enabled (with or without a new network)
  bool    wifi_liga;               // value of wifi_enabled when wifi_muda
  bool    precisao_muda;           // set_channel on the primary channel with the new precision
  int32_t precisao;                // see mesh_cfg_t.precisao
  bool    uplink_muda;             // set_channel on the primary with the new uplink
  bool    uplink;
} mesh_cfg_mudanca_t;
bool mesh_cfg_gravar(const mesh_cfg_mudanca_t* m);

// The node's MQTT x PROXY RULE, applied at both ends of the SAME transaction
// (the app calls it after deciding the new value of the field the person touched and
// marks that field itself; these only adjust the "other" one):
//  - turning MQTT off takes the proxy with it (without MQTT it has nothing to carry);
//    turning it back on restores the proxy to what the node had;
//  - turning the proxy on takes MQTT with it; turning the proxy off leaves MQTT alone, and
//    giving up on turning it on restores MQTT to what the node had.
void mesh_cfg_mqtt_ligar(mesh_cfg_mudanca_t* m, const mesh_cfg_t* c, bool ligar);
void mesh_cfg_proxy_ligar(mesh_cfg_mudanca_t* m, const mesh_cfg_t* c, bool ligar);
// Why the last mesh_cfg_gravar() sent nothing, short, for the screen
// ("" = it sent, or the reason is only in the log).
const char* mesh_cfg_motivo(void);

// Actions that do not restart the node (except the last).
bool mesh_cfg_posicao_fixa_do_gps(void);   // set_fixed_position with the Tab5's GPS
bool mesh_cfg_tirar_posicao_fixa(void);    // remove_fixed_position
bool mesh_cfg_acertar_hora(void);          // set_time_only with the Tab5's time
bool mesh_cfg_reiniciar(void);             // reboot_seconds
bool mesh_admin_gravando(void);            // the Tab5 sent write/restart and the node is not back yet

// FOR SENDING THE TAB5'S GPS POSITION (tab5_mesh.cpp), without the caller's lock:
//  - the node already knows where it is by itself (fixed position, or its own GPS on): what
//    the Tab5 sent would be ignored (fixed) or overridden (its GPS);
//  - the primary channel's precision (-1 = unknown), so internet mode
//    respects the same choice the node respects.
bool    mesh_admin_no_tem_posicao_propria(void);
int32_t mesh_admin_precisao_primario(void);

// ---- service hooks (tab5_mesh.cpp), with its lock held ---------------------
void mesh_admin_guardar_secao(bool modulo, int secao, const uint8_t* bytes, size_t n);
// DIAGNOSTICS (console 'mesh no'): each stored section of the dump, field by field
// (varint and the size of texts/bytes - never the content, which may be a password).
void mesh_admin_despejar(void);
void mesh_admin_guardar_user(const uint8_t* bytes, size_t n);         // the User of OUR node
void mesh_admin_guardar_metadata(const uint8_t* bytes, size_t n);     // FromRadio 13
void mesh_admin_guardar_my_info(const uint8_t* bytes, size_t n);      // FromRadio 3
void mesh_admin_guardar_aviso(const uint8_t* bytes, size_t n);        // FromRadio 16
void mesh_admin_guardar_canal(int indice, const uint8_t* bytes, size_t n);   // FromRadio 10 (whole Channel)
void mesh_admin_inicio_dump(void);   // want_config went out: whatever comes is the new truth
void mesh_admin_fim_dump(void);      // config_complete_id arrived
void mesh_admin_tique(uint32_t agora_ms);   // on every round of the service task
