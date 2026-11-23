// INTERNET LINK (MQTT) - the Meshtastic mesh without the radio node.
//
// The Mesh service's third link, alongside serial (Grove) and BLE. Nodes
// with internet (gateways) publish to an MQTT server what they hear over the
// radio, and pass down to the radio what arrives from there. Speaking the same
// protocol, the Tab5 talks to the mesh using only Wi-Fi: with the C6L off,
// messages keep arriving, and in an emergency with internet you can still
// write.
//
// TO THE SERVICE, THIS IS A RADIO. It hands over FromRadio and takes ToRadio,
// like BLE; inside it does the translation:
//   receiving: ServiceEnvelope{packet, channel_id} -> decrypts the payload with
//              the channel key -> FromRadio{packet: MeshPacket{decoded, ...}};
//   sending:   ToRadio{packet} -> encrypts the Data -> ServiceEnvelope -> publishes
//              to <raiz>/2/e/<canal>/!<no>;
//   want_config: answers by itself with our number, the known channels and
//              the end of the configuration.
//
// THE CONFIGURATION COMES FROM THE NODE. Channels (name and key), region and MQTT
// server are learnt from the C6L's dump when it is connected (mesh_mqtt_aprender_*)
// and stored in NVS: internet mode uses exactly what the node uses. With
// nothing learnt: the Mesh Brasil server (root meshdev), LongFast channel with the
// default key. A server CHOSEN by the person (the list below) takes precedence
// over what the node taught.
//
// LIMITS (see tab5_mesh.hpp): CHANNEL messages only - a direct message
// encrypted with a public key (PKI) is not decrypted here; on the public server
// what arrives from the internet is not repeated by the mesh (zero hops), so the
// reach is whoever is on MQTT plus the radio nodes near a gateway with
// downlink on.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ajustes.h"   // ajustes_mqtt_srv_t

// Connects to the server (Wi-Fi must already be connected). Asynchronous.
bool mesh_mqtt_iniciar(void);
void mesh_mqtt_parar(void);
bool mesh_mqtt_ativo(void);       // started (connected or trying)
bool mesh_mqtt_conectado(void);   // session with the server up
bool mesh_mqtt_conectou_agora(void);   // true ONCE per new connection

// Same contract as the BLE link: ToRadio/FromRadio body, no frame.
bool   mesh_mqtt_enviar(const uint8_t* toradio, size_t n);
size_t mesh_mqtt_receber(uint8_t* buf, size_t max, uint32_t espera_ms);

// "internet: conectado a mqtt.meshtastic.org (msh/BR_902)" etc.
const char* mesh_mqtt_descricao(void);

// Who we are on the mesh: our node number (the C6L's, if already known - the
// same identity over both paths) and the names that go in the NodeInfo.
void mesh_mqtt_definir_no(uint32_t num, const char* curto, const char* longo);

// THE SERVERS THE PERSON CAN CHOOSE for internet mode (Settings >
// Meshtastic, the Mesh app's "No" (Node) page and the welcome wizard). It is
// THEIR LIST, stored in settings: add, edit and delete - no list fixed
// in code. The index is the same as ajustes_mqtt_servidor(): 0 =
// automatic (the node's, or the Brazilian one), 1..n-1 = the saved ones. Choosing one
// other than 0 takes precedence over what the node taught. To switch:
// ajustes_definir_mqtt_servidor() and restart the service (the client picks up the
// new one on reconnecting).
//
//   nome  "Meshtastic Europa"       srv   "mqtt.meshtastic.org" or "host:porta"
//   usr / pwd (may be empty)        raiz  "msh/EU_868" (empty = "msh")
typedef ajustes_mqtt_srv_t mesh_mqtt_servidor_t;
int         mesh_mqtt_n_servidores(void);                    // includes 0 (automatic)
bool        mesh_mqtt_servidor(int i, mesh_mqtt_servidor_t* out);   // 0: only the name "Automático"
const char* mesh_mqtt_servidor_nome(uint8_t escolha);
bool        mesh_mqtt_pode_adicionar(void);                  // the list still has room
// Writes server i (1..n-1) or, with i = 0, a NEW one. An empty address is
// refused; an empty name becomes the address. Returns its index (1..), 0 = not
// written (no address, or list full).
int         mesh_mqtt_salvar_servidor(int i, const mesh_mqtt_servidor_t* s);
// Deletes i (1..n-1). If it was the chosen one, the choice goes back to automatic.
void        mesh_mqtt_apagar_servidor(int i);

// THE PERSON'S OPERATIONS ON THE SERVER (the apps - Settings and the Mesh "No"
// page - only draw and call these; there is one rule).
// Choose the server (0 = automatic, 1..n-1) and restart the Mesh service:
// the MQTT client reconnects to the new one.
void        mesh_mqtt_usar_servidor(uint8_t escolha);
// Writes (see mesh_mqtt_salvar_servidor) and, if written, starts using it and
// reconnects. Returns its index; 0 = not written and nothing changed.
int         mesh_mqtt_salvar_e_usar(int i, const mesh_mqtt_servidor_t* s);
// Deletes i (1..n-1). If it was the one in use, goes back to automatic and reconnects
// (returns true); otherwise nothing reconnects (false).
bool        mesh_mqtt_apagar_e_reconectar(int i);
// Opens the editor: i = 0 starts a NEW server from the first template; 1..n-1
// loads the saved one. false = the index does not exist.
bool        mesh_mqtt_para_editar(int i, mesh_mqtt_servidor_t* p);
// On a new server, switches to the next template and fills in the fields.
// Takes the current template and returns the new one.
int         mesh_mqtt_proximo_modelo(int atual, mesh_mqtt_servidor_t* p);
// The form's five fields, in screen order: 0 name, 1 address,
// 2 user, 3 password, 4 root. Returns the text and the buffer size.
char*       mesh_mqtt_campo(mesh_mqtt_servidor_t* p, int k, size_t* tam);

// TEMPLATES to fill in a new server (the known servers and their
// roots per region; the last is "blank"). They only fill in the fields - the
// person adjusts and saves.
int                         mesh_mqtt_n_modelos(void);
const mesh_mqtt_servidor_t* mesh_mqtt_modelo(int i);

// Learnt from the node's dump (called by the service). Only written when they change.
void mesh_mqtt_aprender_canal(int idx, int papel, const char* nome, const uint8_t* psk, size_t n_psk);
void mesh_mqtt_aprender_regiao(int regiao);
void mesh_mqtt_aprender_servidor(const char* endereco, const char* usuario,
                                 const char* senha, const char* raiz);

// ---------------------------------------------------------------------------
// MQTT CLIENT PROXY - the same client, in the other role
// ---------------------------------------------------------------------------
// With ModuleConfig.mqtt.proxy_to_client_enabled on, the NODE does not open its
// own connection: it hands to the connected client (FromRadio 14, MqttClientProxyMessage
// {1 topic, 2 data, 3 text, 4 retained}) what it would publish, and receives from the
// client (ToRadio 6, the same message) whatever arrives on the subscribed topics. It is the
// phone's "MQTT via the app". Here the Tab5 plays the app's role: the node becomes a full
// gateway - what it hears on the radio goes up, what comes from the internet goes down to
// the mesh - without turning its Wi-Fi on (on the C6L, Wi-Fi and Bluetooth cannot coexist).
//
// DIFFERENCE FROM INTERNET MODE: here the Tab5 does NOT encrypt or decrypt anything. The
// node publishes the finished envelope; the Tab5 just carries bytes. That is why direct
// messages with PKI work (the node decrypts), and the server is the NODE's
// (empty address = mqtt.meshtastic.org, the firmware default), not the internet
// mode default.
//
// The two modes share the client and are exclusive: proxy only with the node
// connected; internet only without it (or with the Internet path). The service
// loop decides (tab5_mesh.cpp). The switch is the node's own
// (proxy_to_client_enabled), toggled on the Mesh app's "No" page: on and
// confirmed in the dump, the Tab5 lends its internet.

// From the node's dump: ModuleConfig.mqtt (1 enabled, 9 proxy_to_client_enabled,
// 7 tls_enabled) and ChannelSettings.downlink_enabled (6) of each channel. RAM only:
// the proxy only exists with the node connected, and the dump comes on every connection.
void mesh_mqtt_aprender_no_mqtt(bool ligado, bool proxy, bool tls);
void mesh_mqtt_aprender_downlink(int idx, bool downlink);
bool mesh_mqtt_no_tem_proxy(void);   // the node came with mqtt and proxy on
// ChannelSettings.uplink_enabled (5): only for diagnostics (console 'mesh').
void mesh_mqtt_aprender_uplink(int idx, bool uplink);
// The NODE'S MQTT as text, for the console: on, proxy, server and root, and the
// up/down of each active channel. A send that reaches the server with a reply that
// never comes back is, almost always, the channel with uplink and without downlink.
void mesh_mqtt_no_resumo(char* out, size_t tam);

bool mesh_mqtt_iniciar_proxy(void);  // Wi-Fi must already be connected
bool mesh_mqtt_em_proxy(void);       // the client that is up is in the proxy role

// UPLINK: what the node asked to publish. Called from the service task.
bool   mesh_mqtt_proxy_publicar(const char* topico, const uint8_t* d, size_t n, bool retido);
// DOWNLINK: the next ToRadio{ mqttClientProxyMessage } ready for the node;
// 0 = nothing queued. Called from the service task, without waiting.
size_t mesh_mqtt_proxy_receber(uint8_t* buf, size_t max);
void   mesh_mqtt_proxy_contadores(uint32_t* subiu, uint32_t* desceu, uint32_t* perdeu);
