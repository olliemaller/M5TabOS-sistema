// Meshtastic service - the link to the M5 Unit C6L over Grove.
//
// A SERVICE, not an app. The task stays up while the Grove port is
// given over to Meshtastic, regardless of which app is on screen. That is what
// allows notifying a new message without the app open - if the link died with
// the window, there would only be messages while you were looking.
//
// Same design as tab5_wifi and tab5_teclado: the state lives here, the app reads it.
//
// -------------------------------------------------------------------------
// THE PROTOCOL, IN TWO LINES
// -------------------------------------------------------------------------
// Meshtastic in PROTO mode sends framed protobuf:
//
//   94 C3 <len_hi> <len_lo> <FromRadio protobuf>
//
// Verified on the bench: 20 consecutive frames decoded, and the
// node number read here matched what the CLI read over USB (4044735316).
//
// We do NOT use Meshtastic's official .proto files. They are dozens of files and a
// generator to read half a dozen fields. Protobuf is simple enough to
// walk by hand: each field is <tag varint><value>, and the tag carries number and type.
// Same path as NMEA and the keyboard, which worked out.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // size_t - used in tab5_mesh_nome_exibicao()

// LIVE NODES. It was 32 - the same tight ceiling as a small node. With MQTT
// on at the C6L (downlink), every internet node that speaks becomes an entry here,
// and 32 slots churned in seconds: a quiet RADIO node (announcing every 3
// h, telemetry every 30 min) was evicted before appearing on screen,
// even with its packets passing through the C6L. 256 in PSRAM is ~30 KB.
#define MESH_MAX_NOS     256
#define MESH_MAX_MSGS      8
#define MESH_NOME_LONGO   40
#define MESH_NOME_CURTO    6

// RECEIVED TEXT buffer: 238 fits any valid payload with room to spare plus the
// terminator. The protocol ceiling is smaller - see MESH_PAYLOAD_MAX.
#define MESH_TEXTO       238

// SEND CEILING: Data.payload has max_size 233 in the firmware's mesh.pb.h
// (meshtastic_Constants_DATA_PAYLOAD_LEN). A ToRadio with a bigger payload fails
// nanopb decoding INSIDE the node and is discarded with no response at all -
// no queueStatus, no Routing: the message only becomes "no response" 60 s later.
// That is why the cut happens BEFORE it goes out, on a UTF-8 character boundary,
// and the compose bar no longer accepts more than this (conversa_t.limite).
//
// AND THE PROTOBUF CEILING IS NOT THE ON-AIR ONE. The firmware still adds, to the
// encoded Data, the 'bitfield' (2 bytes) and the 16-byte header; the whole packet
// cannot exceed 255 (MAX_LORA_PAYLOAD_LEN). Channel text fits up to ~232; a
// DIRECT message encrypted with a public key (PKI) takes 12 more and fits up to
// ~220. Above that the node answers TOO_LARGE (Router.cpp, perhapsEncode). The
// official clients stop at 200 - and that is the ceiling here too, with room for the
// fields newer versions may add.
#define MESH_PAYLOAD_MAX 200

// Valid time: tab5_hora_valida() / tab5_hora_epoch() (services/tab5_hora.hpp).

// Sentinel for mesh_msg_t::saltos - "cannot tell", never "zero hops".
#define MESH_SALTOS_DESCONHECIDO 255

typedef struct {
  uint32_t num;                        // the node's identifier on the mesh
  char     longo[MESH_NOME_LONGO];     // "Meshtastic c354"
  char     curto[MESH_NOME_CURTO];     // "capc"
  float    snr;                        // quality of its last packet
  uint32_t visto_em;                   // last_heard, the radio's UTC time

  // OUR uptime at the last packet we heard from it; 0 = we have heard none
  // since we started. It exists because last_heard above is the RADIO's time, and
  // we do not always know what time it is - see tab5_mesh_epoch().
  uint32_t visto_local_ms;

  // How many seconds since the node was last heard; -1 = cannot
  // tell. NOT stored in the table: it is computed in tab5_mesh_no(), at the moment
  // of copying, precisely so it does not age between reading and drawing.
  int32_t  idade_s;

  // HARDWARE IDENTITY, which is what does not change.
  //
  // 'longo' and 'curto' are LABELS: whoever renames the node on the phone changes
  // both, and they are no use for telling which device you are talking to. What
  // is useful is this:
  //
  //   id      - "!a1b2c3d4", the identifier Meshtastic uses everywhere
  //             (log, map, app). It is derived from the MAC and does not change.
  //   mac     - the 6 bytes, when the node sends them. Field marked obsolete
  //             in the protobuf, so newer firmware tends to omit it - hence
  //             'tem_mac' exists, so as not to draw 00:00:00:00:00:00 as if
  //             it were a real address.
  //   hw      - HardwareModel: which board it is (Heltec, T-Beam, Unit C6L...).
  char     id[16];                     // "!a1b2c3d4"; empty = did not come
  uint8_t  mac[6];
  bool     tem_mac;
  uint8_t  hw;                         // HardwareModel; 0 = unknown

  uint8_t  bateria;                    // 0-100; 101 = on mains power
  float    tensao;
  uint8_t  saltos;                     // hops_away: 0 = direct neighbour

  // The node came over the INTERNET (MQTT), not over the radio. That changes the meaning of everything
  // we see about it: the SNR is not from our antenna, the hops are not RF, and
  // it cannot be reached if the internet goes down.
  bool     via_mqtt;
  int32_t  lat_i, lon_i;               // degrees x 1e7 (0 = no position)
  bool     tem_pos;
  bool     usado;
  // It was the network (1 = the American one, removed). The byte stays, in the
  // alignment gap: the old nodedb.bin still works, and anything with 1 there
  // is not loaded. Always zero on new nodes.
  uint8_t  rede_antiga;

  // FIXED PLACE AT THE TOP OF THE TABLE: the order in which the node was first heard over the air (with
  // SNR); 0 = not yet. Given once, it does not change - the node does not
  // drop when another speaks afterwards. See ordem_nos() in the .cpp.
  uint32_t fixado;

  // Star - derived when copying (tab5_mesh_no), the list lives in the service.
  bool     favorito;
} mesh_no_t;

// ---------------------------------------------------------------------------
// DELIVERY CONFIRMATION
// ---------------------------------------------------------------------------
// Meshtastic really does confirm. When sending with want_ack, the radio returns a
// packet on the ROUTING port (5) carrying request_id = the id of OUR packet and a
// Routing.error_reason: zero is ACK, any other value is the reason for failure.
//
// A DISTINCTION THAT MATTERS:
//   - DIRECT message: the ACK comes from the RECIPIENT. "It reached them."
//   - CHANNEL message (broadcast): the ACK is IMPLICIT - the radio heard its own
//     packet being repeated by a neighbour. It means "it entered the mesh", not
//     "someone read it". There is no way to know who received a broadcast, and no
//     Meshtastic client knows.
typedef enum {
  MESH_ENVIO_NA = 0,     // received, or ours but from before the last restart
  MESH_ENVIANDO,         // waiting for the radio to confirm
  MESH_ENTREGUE,         // ACK
  MESH_FALHOU,           // NAK with a reason, or the radio refused to queue it
  MESH_SEM_RESPOSTA,     // the timeout ran out without the radio saying anything
  // DIRECT MESSAGE HEARD BY THE MESH, WITHOUT CONFIRMATION FROM THE DESTINATION. The node generates
  // an IMPLICIT ACK (Routing from the node itself, error 0) when it hears a
  // neighbour repeat the packet. For a broadcast that is the best there is and counts as
  // ENTREGUE; for a direct one it is only halfway - ENTREGUE is reserved for the ACK
  // from the RECIPIENT ITSELF. New number at the END of the enum: envios.txt
  // stores the value, and the old ones keep meaning the same.
  MESH_OUVIDO,
} mesh_envio_t;

// Our own code, outside Meshtastic's Routing.Error range: the radio refused
// to put the packet in the queue (QueueStatus with res != 0).
#define MESH_ERRO_FILA 200

// Name of the reason, for the screen. Returns "?" for a code we do not know - the
// Meshtastic list grows with every version and making up a name would be worse.
const char* mesh_erro_nome(uint8_t erro);

typedef struct {
  uint32_t de;                         // source node
  uint32_t para;                       // 0xFFFFFFFF = channel; our num = direct
  uint8_t  canal;                      // channel index (0 = the primary)
  bool     direta;                     // convenience: para == our number
  char     texto[MESH_TEXTO];
  uint32_t quando;                     // the radio's rx_time
  float    snr;
  int32_t  rssi;

  // THE PACKET ARRIVED OVER THE INTERNET (some node's MQTT bridge), NOT OVER THE RADIO.
  // MeshPacket field 13, to tell apart "I heard this
  // on air" from "this reached me via the cloud". When true, snr/rssi are still
  // filled in by the radio (it is the LAST local hop that delivered the bridge),
  // but do not describe the message's end-to-end link - only the RF leg that
  // brought it to whoever had the bridge.
  bool     via_mqtt;

  // HOW MANY RADIO HOPS this specific message took to get here
  // (only meaningful for LoRa; via MQTT there are no hops). Computed from
  // hop_start - hop_limit (MeshPacket fields 14 and 9, BOTH read from the
  // SAME packet): hop_start is the ceiling the sender set, hop_limit is what
  // was left when the packet arrived here - the difference is how many
  // repeaters it passed through.
  //
  // 255 = CANNOT TELL (old firmware does not send hop_start, or the
  // message is via_mqtt and the question makes no sense on the internet side) -
  // same spirit as the SNR sentinel (-128): do not make up a number that
  // does not exist.
  uint8_t  saltos;

  // The packet id on the mesh. It is what links the message stored on the card to the ACK
  // that arrives later. 0 when it is not ours.
  uint32_t id;

  // DERIVED fields, filled in when copying from the id - not kept on the
  // card. The state lives only in RAM: writing "enviando" (sending) to a file and waking
  // up with it the next day would be a lie.
  uint8_t  envio;                      // mesh_envio_t
  uint8_t  erro;                       // reason, when envio == MESH_FALHOU
} mesh_msg_t;

// Creates the service task. Does NOT turn on the Grove rail - that is done by the
// power profile, via the user's setting (see sistema_impor_preferencias).
void tab5_mesh_iniciar(void);

// Starts the service ONLY over the internet (MQTT), without the node - see
// services/tab5_mesh_mqtt.hpp. The kernel calls it, when Wi-Fi is
// connected and the node is not (empty cable, Bluetooth off). Finding the node
// later (tab5_mesh_iniciar) tears this mode down and brings up the radio one.
void tab5_mesh_iniciar_internet(void);

// Stops the service, closes the UART and releases the Grove port. false = could not (the
// task did not confirm its exit), and in that case NOTHING was torn down.
bool tab5_mesh_parar(void);

// Stops the service ONLY if it is on the Grove port (via serial). Whoever touches the cable
// (probing, Grove emptied) calls this: via Bluetooth the node is not on the cable and
// the conversation carries on.
bool tab5_mesh_soltar_grove(void);

// How the service talks to the node (AJUSTES_MESH_SERIAL/BLUETOOTH) and how the
// link is going, as text for the screen ("conectado a ...", "procurando ...").
uint8_t     tab5_mesh_via(void);
const char* tab5_mesh_enlace_descricao(void);

// Internet mode (MQTT over Wi-Fi) state as text for the screen: off,
// waiting for the Grove, waiting for Wi-Fi, or the server and root in use.
const char* tab5_mesh_internet_estado(void);

// MQTT proxy state (the Tab5's internet lent to the node), short, for the
// "No" (Node) page line: no Wi-Fi, connecting, or connected with the counters
// (published by the node / delivered to it).
const char* tab5_mesh_proxy_estado(void);

// true when there is a task and the UART is up. false = read-only: you can look at
// what is on the card, but cannot send.
bool tab5_mesh_servico_ativo(void);

// Reads the card's FOLDER and creates a conversation for each history file.
// Allows browsing the file without the radio plugged in. Returns how many it saw.
int  tab5_mesh_carregar_do_cartao(void);

// Is the link alive? (we received a valid frame recently)
bool tab5_mesh_no_ar(void);

// How many ms since the radio last sent a frame; UINT32_MAX if it never did. For whoever
// needs to pick their own timeout instead of inheriting no_ar()'s - see the
// Grove watch in kernel/core.cpp.
uint32_t tab5_mesh_silencio_ms(void);

// Our own node number; 0 until my_info arrives.
uint32_t tab5_mesh_meu_numero(void);

// C6L firmware, empty string if it has not come yet.
const char* tab5_mesh_firmware(void);

// SOUND ALERT, the counterpart of tab5_trilha_definir_beep() (services/tab5_trilha)
// (see the notice there). Turns on/off and SAVES the preference
// (ajustes_definir_beep_mesh()) and, if the "Meshtastic" source already exists in
// notificacao.h, updates its flag too - only it; Trilha and
// any other source keep sounding normally.
void tab5_mesh_definir_beep(bool tocar);
bool tab5_mesh_beep_ligado(void);

// Estimated UTC time, in epoch seconds; 0 until there is a reference.
//
// The Tab5 has no clock with the correct time, but the radio does: it stamps
// last_heard on every NodeInfo and rx_time on every packet. We keep the HIGHEST
// stamp we have seen together with our uptime at that instant, and from then
// on the time advances with our clock. Enough to answer "how long
// ago", which is the only use.
uint32_t tab5_mesh_epoch(void);

// The node's clock time read from LIVE traffic (see the .cpp); 0 with none
// in the last hour. It is what tab5_hora uses to set the clock - the one above,
// anchored on last_heard, may be hours behind.
uint32_t tab5_mesh_hora_viva(void);

// A node's name READY FOR DISPLAY, by the same rule everywhere (conversation
// column, sender label in the channel):
//   1. the SHORT name, if legible;
//   2. otherwise the legible LONG name;
//   3. only then the id "!a1b2c3d4".
// "Legible" = text remains after removing what the system fonts do not
// draw (emoji and 3-4 byte UTF-8 symbols; accents stay) and the "?"
// Meshtastic puts when nobody chose a short name. The id is the last
// resort: it identifies, but nobody recognises a friend by eight hex
// digits. Works with the node offline, via the card cache (nos.txt).
void tab5_mesh_nome_exibicao(uint32_t num, char* out, size_t out_tam);

// ONLY the persistent cache (nos.txt) - without the hex fallback that
// tab5_mesh_nome_exibicao() has. Returns false (out untouched) when there is no
// known name for that number.
//
// For NARROW columns where repeating the hex ID would be redundant (see
// montar_linha() in app_mesh.cpp: the nodes table already has its own ID
// column that shows the hex when the name is missing - showing it again in the
// short-name column adds no information, it just takes space that a
// real name would use better).
bool tab5_mesh_nome_cache(uint32_t num, char* out, size_t out_tam);

// SAME IDEA as tab5_mesh_nome_cache(), but for the LONG name. Before,
// only the short one went into the cache (see the notice above) because
// only tab5_mesh_nome_exibicao() (which never uses the long one) consulted it - but the
// nodes table (montar_linha() in app_mesh.cpp) always showed the long one, and
// ONLY from the LIVE entry (this session's NodeInfo). NodeInfo is rare (see the
// notice in ler_node_info()); most nodes are learnt from telemetry/
// position/text, with no name yet - hence "always nameless". Now
// the long one persists too. Returns false (out untouched) when there is none.
bool tab5_mesh_nome_longo_cache(uint32_t num, char* out, size_t out_tam);

// The node's last KNOWN link (via_mqtt/hops_away), from its NodeInfo -
// not from a specific packet. Serves as a FALLBACK for a message whose
// own packet did not bring hop_start (the sender's firmware sometimes does not
// send it) - "from whom" gives a reasonable approximation of "how", when the exact
// "how" did not come in this message: "the nodes page
// always has it, the conversation does not show it - can we use the same information?".
// Returns false if the node is not in the live table now (never
// announced itself, or was evicted for inactivity - see achar_no).
bool tab5_mesh_no_enlace(uint32_t num, bool* via_mqtt, uint8_t* saltos);

// ---- nodes -----------------------------------------------------------------
// The node was heard OVER THE AIR (radio): it has a measured SNR and did not come via the
// MQTT bridge (there the SNR is the gateway's, not our antenna's). The table pins them at the
// top, just below ours.
bool tab5_mesh_no_pelo_ar(const mesh_no_t* no);

// FAVOURITES (star). The same as the Meshtastic node's: marking here sends the
// AdminMessage set/remove_favorite_node to the radio (or leaves it pending until the
// next dump, without it), and the dump's is_favorite updates the list here. In the
// table they sit at the top, just below ours, in the order they were marked,
// and are never evicted.
bool tab5_mesh_favorito(uint32_t num);
void tab5_mesh_definir_favorito(uint32_t num, bool favorito);

int  tab5_mesh_num_nos(void);
// Copies the node at index i, in table order (ours, those heard over the air,
// the rest; within each band the most recently heard first). Returns false
// if i is invalid. Copies instead of
// returning a pointer on purpose: the service task rewrites the table at
// any moment, and a pointer handed to the screen would go stale in the middle of
// drawing.
bool tab5_mesh_no(int i, mesh_no_t* saida);

// THE SAME TABLE, ONLY THE NODES MATCHING THE SEARCH, in the same order - it is what the
// nodes page's search box shows. A match is any node with the text in its long
// name, short name (the live ones or, without them, those in the name cache) or in the id
// "!a1b2c3d4"; ignoring case and accents. An empty (or
// null) search = all.
#define MESH_BUSCA_MAX 40
int  tab5_mesh_num_nos_busca(const char* busca);
// THE GROVE INTERNALS (console 'mesh grove' / 'mesh rastro on|off'): bytes and
// frames in both directions, junk outside frames and UART errors.
void tab5_mesh_grove_diag(char* out, size_t tam);
void tab5_mesh_rastro(bool ligado);
bool tab5_mesh_no_busca(const char* busca, int i, mesh_no_t* saida);

// How many times the nodes table (MESH_MAX_NOS - see achar_no) was
// full and had to evict the oldest to make room for a new node,
// since boot. Not an error nor an automatic log (it would pollute the log) - just a number to check on demand, in
// the console's "mesh" command, when you want to know if the mesh has more than 32
// real nodes.
uint32_t tab5_mesh_despejos(void);

// ---- messages -------------------------------------------------------------
int  tab5_mesh_num_msgs(void);
bool tab5_mesh_msg(int i, mesh_msg_t* saida);   // 0 = most recent

// DO NOT USE tab5_mesh_num_msgs() TO DETECT "a new message arrived" - it
// returns the size of the 8-slot ring (MESH_MAX_MSGS) and STOPS CHANGING
// as soon as 8 messages (from any conversation) have passed through it, staying
// stuck forever from then on (the Mesh app compared this number and
// a new message only appeared on switching screens). tab5_mesh_total_msgs() is the counter that NEVER saturates -
// use that for "changed since I last looked".
// It also changes when a history timestamp is corrected (clock jump):
// for whoever rereads, "the history changed" is what matters.
uint32_t tab5_mesh_total_msgs(void);

// Messages that arrived and have not been seen yet: the SUM over conversations
// (mesh_conversa_t.nao_lidas). Opening a conversation marks it read
// (tab5_mesh_marcar_conversa_lida); opening the app does not clear the others.
int  tab5_mesh_nao_lidas(void);
// Unread DIRECT messages only. They deserve their own highlight: a
// message addressed to you is not the same thing as channel traffic.
int  tab5_mesh_nao_lidas_diretas(void);

// ---------------------------------------------------------------------------
// CONVERSATIONS
// ---------------------------------------------------------------------------
// A conversation is a CHANNEL (LongFast and the others) or a NODE (direct message).
// They appear by themselves as messages arrive - there is no fixed list.
//
// The history lives on the card, one file per conversation, in
// /sdcard/apps/Meshtastic/. Reason: RAM keeps only the latest for the screen
// to draw quickly, but a conversation that vanishes on restart is no use.
#define MESH_MAX_CONVERSAS 12
#define MESH_MAX_CANAIS     8   // channels of a Meshtastic node (0 = the primary)
#define MESH_NOME_CONVERSA 24

typedef struct {
  bool     canal;                       // true = channel; false = direct message
  uint8_t  idx;                         // channel index, if a channel
  uint32_t no;                          // node number, if direct
  char     nome[MESH_NOME_CONVERSA];    // "LongFast", "capc"...
  // SECOND LINE, derived when copying (not stored): the node's long name in the
  // direct conversation, "canal N" (channel N) for a channel. It is what lets the column say WHO it is
  // without falling back to the hex id.
  char     sub[MESH_NOME_LONGO];
  int      nao_lidas;
  uint32_t ultima_em;
  bool     usada;
} mesh_conversa_t;

// Opens (or creates) the direct conversation with a node and returns its VISIBLE index,
// or -1 if the number is no good. It is what allows starting to talk to someone
// from the nodes list, without waiting for the person to speak first.
int  tab5_mesh_conversa_com(uint32_t no);

// Same role as tab5_mesh_conversa_com(), for a CHANNEL - created
// together with the option to delete a channel, so that "available channels" (further down)
// can reopen one that was hidden.
int  tab5_mesh_conversa_do_canal(uint8_t idx);

// Deletes the conversation at VISIBLE index - channel OR direct (until
// then it refused channels: "they come back by themselves, pretending to delete would be
// a lie" - that stopped being true when channels started using the same
// deleted flag that already existed for nodes). Does not touch the card history
// in either case - only the in-memory record disappears from the list.
bool tab5_mesh_esquecer_conversa(int i);

// THE NODE'S CHANNELS - all those ACTIVE in the last known configuration (from the
// live radio, or the one stored in NVS when it is not there). Answers "which
// channel can I talk on" regardless of whether the conversation is in the column: a channel
// deleted from the column is still here, and tapping it brings it back. With no
// known configuration at all, the primary (LongFast) appears anyway.
// 'visivel' = there is already a conversation for it in the column.
int  tab5_mesh_num_canais(void);
bool tab5_mesh_canal(int i, uint8_t* idx, char* nome, size_t tam, bool* visivel);

// THE COLUMN ORDER: channels first (by index), then the direct ones, from the
// most recent conversation to the oldest. VISIBLE index is the position in this
// order - and it changes (new conversation, deleted, direct one that received a message).
// Whoever keeps an index (the conversation open in the app) compares this version and,
// when it changes, finds theirs again by KEY with tab5_mesh_conversa_indice.
// Without this, a new conversation in an earlier slot shifted the indices and the
// app started showing and writing in the NEIGHBOURING conversation.
uint32_t tab5_mesh_conversas_versao(void);
int      tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no);

int  tab5_mesh_num_conversas(void);
bool tab5_mesh_conversa(int i, mesh_conversa_t* saida);
void tab5_mesh_marcar_conversa_lida(int i);

// Reads a conversation's last messages from the card, oldest to
// newest (which is the order the screen draws). Returns how many it read.
int  tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max);
// The same, by KEY (channel + index, or node number) - the file is named
// after it. It is what the app's worker uses: between the request and the read the column
// order may change, the key does not.
int  tab5_mesh_historico_de(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max);

// Sends text. para = 0xFFFFFFFF broadcasts on channel 'canal'; any other value
// is a direct message to that node. Returns false if the link is silent.
bool tab5_mesh_enviar(uint32_t para, uint8_t canal, const char* texto);

// Delivery state of one of OUR packets, by id. Returns MESH_ENVIO_NA when
// the id is no longer in the table - we only keep the latest sends, and nothing
// survives a restart. Not knowing is a legitimate state: better to draw no
// mark at all than to draw a wrong one.
uint8_t tab5_mesh_estado_envio(uint32_t id, uint8_t* erro);

// Changes every time some send state changes. The screen compares it with the value
// it saw last and only repaints when there is something new - without this the ACK
// would arrive and the bubble would keep the old mark until the next touch.
uint32_t tab5_mesh_envios_versao(void);

// ---- mesh health (LocalStats / the node's own telemetry) -------------------
typedef struct {
  float    uso_canal;      // channel_utilization, %
  float    airtime_tx;     // air_util_tx, %
  uint32_t uptime_s;
  int32_t  piso_ruido;     // noise_floor, dBm
  uint16_t nos_online;
  uint16_t nos_total;
  // LocalStats packet counters (only our node counts them; 'tem_local'
  // says whether they came).
  uint32_t pkt_tx, pkt_rx, pkt_ruins, pkt_repetidos;
  // WHERE IT CAME FROM. Without LocalStats (internet mode, or the node has not sent it yet) the
  // numbers above are zero - and zero is a plausible value, so the screen needs
  // to know it is "did not come" to write "-" instead of lying "0 %".
  bool     tem_local;      // our node's LocalStats arrived
  bool     tem_metricas;   // our node's DeviceMetrics arrived (uptime, usage)
} mesh_saude_t;

void tab5_mesh_saude(mesh_saude_t* saida);

// Readable name of the HardwareModel (the node's 'hw' field). COMPLETE table, generated
// from Meshtastic's official enum by gerar_hw.py (see tab5_mesh_hw_dados.cpp).
// Returns nullptr for what the official list does not have yet (it grows with every
// firmware version) - the caller decides how to show the raw number; making up
// a name here would be worse than admitting ignorance.
const char* mesh_hw_nome(uint8_t hw);

// The table itself, 256 slots indexed by the code itself - mostly
// nullptr. Exposed for anyone who wants to read it directly (mesh_hw_nome already covers the
// common case); generated, do not edit by hand.
extern const char* const TAB5_MESH_HW_NOMES[256];

// Diagnostics: how many frames, packets and telemetry variants went through.
// Helps tell "does not arrive" from "arrives and the filter discards it".
void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* texto, uint32_t* nodeinfo,
                          int* ultima_variante);

// How many times a Routing arrived AFTER the packet was already confirmed
// (a second answer, ignored for the STATE but not for this number). A
// route-quality thermometer: it rises when "someone heard it" comes easily but the
// radio's strong confirmation does not - the mesh works, but at the limit.
uint32_t tab5_mesh_routing_extra(void);
