// CONTRACT between the P4 (M5tabOS) and the C6 firmware (the c6/ folder).
//
// It lives in compartilhado/ and not in main/ for a concrete reason: these are
// TWO IDF projects, for TWO chips, compiled separately. If each side has its
// own copy of the numbers, one day they disagree and the compiler does not
// warn - it already happened in this very stage, with the grace-period timeout
// written as 90 on one side and 120 on the other. Here both include the SAME
// file.
//
// This file exists so the two sides do NOT drift apart. The numbers are
// agreed, travel raw over esp_hosted's peer_data channel, and there is no
// negotiation at all - if one side changes on its own, the other will only see
// unknown messages and silence.
//
// Payload format, always raw - no protobuf. The channel already delivers
// (msg_id, bytes, length), and serialising on top of that would be work for no
// return when both sides are ours.

#pragma once

#include <stdint.h>

// P4 -> C6
#define C6_MSG_HOST_VIVO    1   // no payload: "I am listening to you"
#define C6_MSG_HOST_ENVIAR  2   // 6B destination MAC + text
#define C6_MSG_HOST_CANAL   3   // 1B Wi-Fi channel
#define C6_MSG_HOST_ESPNOW  4   // no payload: "Wi-Fi is up, you can start ESP-NOW"
#define C6_MSG_HOST_FAREJO  5   // c6_farejo_cfg_t: starts/renews/stops promiscuous mode
#define C6_MSG_HOST_CRU     6   // 6B destination MAC + bytes: ESP-NOW with no header at all, once
#define C6_MSG_HOST_AJUSTE  7   // 1B key (C6_AJ_*) + value
#define C6_MSG_HOST_RADIO   8   // 1B sub (C6_RX_*) + config; the 8th and LAST handler

// C6 -> P4
#define C6_MSG_PRONTO      10   // 6B own MAC + 1B channel
#define C6_MSG_RECEBIDO    11   // 6B source MAC + 1B flags (bit0 broadcast, bit1 Espressif format) + text
#define C6_MSG_ESTADO      12   // 6B destination MAC + 1B ok
#define C6_MSG_FAREJO      13   // c6_farejo_lote_t + n x c6_farejo_reg_t
#define C6_MSG_RECEBIDO_CRU 14  // 6B source MAC + 1B rssi + 1B channel + 1B flags (bit0 broadcast) + raw bytes
#define C6_MSG_INFO        15   // c6_info_t
#define C6_MSG_CSI         16   // c6_csi_cab_t + raw CSI bytes
#define C6_MSG_FTM         17   // c6_ftm_rep_t
#define C6_MSG_154         18   // c6_154_cab_t + raw bytes of the 802.15.4 frame
#define C6_MSG_PM          19   // text: the C6's PM lock table (esp_pm_dump_locks), under C6_AJ_PM

// HOW MANY msg_ids EACH SIDE LISTENS TO. The C6's peer_data has a FIXED table of 8
// callbacks (EH_CP_FEAT_PEER_DATA_MAX_HANDLERS); the 9th registration fails silently
// and that message is ignored from then on. Today the C6 listens to 8 (1..8): table full.
// A new P4 -> C6 command goes in as a KEY of C6_MSG_HOST_AJUSTE, not as a new msg_id.
// The C6 -> P4 direction has no such ceiling (the host grows its table).

// CONTRACT VERSION. Sent in PRONTO (byte 7) and in INFO. Old C6 firmware
// sends PRONTO with only 7 bytes: the reader treats "no byte 7" as version 1.
//   1 - ESP-NOW (text, two dialects) and channel
//   2 - + promiscuous sniffer, raw ESP-NOW, INFO
//   3 - + CSI, FTM, 802.15.4 (each on request from the P4)
#define C6_PROTO_VERSAO    3
#define C6_CAP_ESPNOW      0x01
#define C6_CAP_FAREJO      0x02
#define C6_CAP_CRU         0x04
#define C6_CAP_INFO        0x08
// These 3 only light up if the module was COMPILED into the C6 (depends on
// c6/'s sdkconfig). Firmware flashed without one of them leaves the bit at
// zero, and the P4 hides the feature - that is how new features stay on the P4
// side without reopening the device.
#define C6_CAP_CSI         0x10
#define C6_CAP_FTM         0x20
#define C6_CAP_154         0x40
// PRONTO v2: 6B MAC + 1B channel + 1B C6_PROTO_VERSAO + 1B C6_CAP_*

// --- C6_MSG_HOST_AJUSTE: keys ----------------------------------------------
#define C6_AJ_INFO         1    // no value: replies with C6_MSG_INFO
#define C6_AJ_CRU          2    // 1B: 1 = also forward every ESP-NOW received as C6_MSG_RECEBIDO_CRU
#define C6_AJ_IP           3    // 4B: the P4's IPv4 (network order) - with the host asleep the C6 answers its ARP
#define C6_AJ_PM           4    // no value: replies with C6_MSG_PM (what is preventing the C6's light sleep)
// THE ESP-NOW WINDOW. With ESP-NOW started, the IDF default is the MAXIMUM
// window: the radio never sleeps between beacons (modem sleep has no effect -
// measured on the C6: the "wifi" lock held 99% of the time). 2B interval + 2B
// window, in ms (little-endian): every 'interval' the radio listens to ESP-NOW
// for 'window' ms. window 0xFFFF = always listening (the default); interval
// 0 = the IDF's.
#define C6_AJ_ESPNOW_JANELA 5

// --- C6_MSG_HOST_RADIO: sub-commands ---------------------------------------
// The radio extras, each turned on/off from here. All follow the SAME rule as
// the sniffer: the receiver stays on while the mode is live, and each one
// TURNS ITSELF OFF if the P4 does not resend the config within
// C6_RADIO_VALIDADE_MS (an app that closed must not leave the radio draining
// power). FTM is the exception: it is a one-shot, with no renewal.
#define C6_RX_CSI          1    // c6_csi_cfg_t
#define C6_RX_FTM          2    // c6_ftm_cfg_t (one-shot)
#define C6_RX_154          3    // c6_154_cfg_t
#define C6_RADIO_VALIDADE_MS 10000

// --- SNIFFER (promiscuous mode) ----------------------------------------------
// esp_hosted does NOT pass promiscuous mode through (the IDs exist in
// rpc_v2.proto, but neither side implements them). Here it runs ON THE C6 and
// the P4 receives only the SUMMARY: the C6 aggregates by MAC for `lote_ms` and
// sends a batch. A busy channel exceeds a thousand frames per second; frame by
// frame over SDIO would be a thousand interrupts, mallocs and events per
// second on the P4. In batches it is 4.
//
// VALIDITY: sniffing TURNS ITSELF OFF if the P4 does not resend the
// configuration within C6_FAREJO_VALIDADE_MS. Promiscuous mode keeps the
// receiver on all the time (no modem sleep); an app that closed or hung must
// not leave the radio draining power. Resending the SAME configuration only
// renews, it does not restart the channel hopping.
//
// CHANNELS: `canais` is a mask (bit N = channel N). Zero = stay on the current
// channel. With more than one bit the C6 hops channel every `permanencia_ms` -
// but ONLY when detached from the router (tab5_wifi_modo_so_radio): while
// associated, changing channel drops Wi-Fi, so the C6 stays on the router's
// channel and ignores the mask. When turned off, it goes back to the channel
// it was on.
#define C6_FAREJO_VALIDADE_MS 10000
#define C6_FAREJO_LOTE_MAX    48     // records per message (48 x 12 + 6 = 582 B)

#define C6_FJ_MGMT    0x01   // filter: management frames (beacon, probe...)
#define C6_FJ_DADOS   0x02   // filter: data frames

typedef struct __attribute__((packed)) {
  uint8_t  liga;             // 0 = turn off now
  uint8_t  filtro;           // C6_FJ_*; 0 = both
  uint16_t canais;           // mask; 0 = current channel
  uint16_t permanencia_ms;   // per channel while hopping; 0 = 200
  uint16_t lote_ms;          // batch period; 0 = 250 (minimum 50)
  int8_t   rssi_min;         // discard anything weaker than this; 0 = accept everything
  uint8_t  reserva;
  uint8_t  alvo[6];          // only this MAC; zeros = all
} c6_farejo_cfg_t;           // 16 B

// What each MAC did during the period (bits of c6_farejo_reg_t.tipos)
#define C6_FT_BEACON   0x01  // advertised a network (beacon / probe response): it is an AP
#define C6_FT_PROCURA  0x02  // probe request: a device looking for a network (phone!)
#define C6_FT_DADOS    0x04  // data frame
#define C6_FT_PARA_AP  0x08  // data with ToDS: a STATION talking to the AP
#define C6_FT_DO_AP    0x10  // data with FromDS: the AP talking
#define C6_FT_GERENCIA 0x20  // other management (auth, assoc, deauth, action)

typedef struct __attribute__((packed)) {
  uint8_t  canal;            // radio channel when the batch closed
  uint8_t  n;                // records that follow
  uint16_t perdidos;         // MACs that did not fit in the period's table
  uint16_t quadros;          // frames seen during the period (saturates at 65535)
} c6_farejo_lote_t;          // 6 B

typedef struct __attribute__((packed)) {
  uint8_t  mac[6];           // transmitter (addr2)
  int8_t   rssi_ult;         // latest
  int8_t   rssi_max;
  int8_t   rssi_med;         // average over the period
  uint8_t  n;                // frames during the period (saturates at 255)
  uint8_t  tipos;            // C6_FT_*
  uint8_t  canal;            // channel where it was heard
} c6_farejo_reg_t;           // 12 B

typedef struct __attribute__((packed)) {
  uint8_t  proto;            // C6_PROTO_VERSAO
  uint8_t  caps;             // C6_CAP_*
  uint8_t  canal;
  uint8_t  farejando;        // 1 = promiscuous mode on right now
  uint32_t heap_livre;
  uint32_t heap_min;
  uint32_t no_ar_s;          // since the C6 booted
  uint32_t quadros;          // sniffed since boot
  // Diagnostics for the radio extras (only firmware that includes them; the P4
  // accepts the short INFO from an older C6 and leaves this zeroed).
  int16_t  csi_err;          // first non-OK esp_err_t when turning CSI on (0 = all OK)
  int16_t  z154_err;         // same for 802.15.4
  uint32_t csi_cb;           // times the driver called the CSI callback
  uint32_t csi_env;          // CSI records handed to the queue for the P4
  uint32_t z154_rx;          // 802.15.4 frames received by the radio (before the rate filter)
  int16_t  csi_err2;         // result of the LAST time CSI was turned on (0 = it turned on)
  uint8_t  csi_etapa;        // where it stopped: 1 config, 2 callback, 3 enable
  uint8_t  csi_modo;         // 1 = CSI on right now, 0 = off
  // (ESP_FAIL in csi_err = the C6's Wi-Fi was initialised with csi_enable 0. That
  // field is set by the P4, in esp_wifi_init - see main/hal/tab5_wifi.cpp.)
  uint32_t saida_perdas;     // messages dropped because the output was full (the P4 did not consume in time)
  uint8_t  z154_estado;      // 0 never turned on, 1 listening, 2 sleeping (driver enabled, RF released)
  uint8_t  chip_rev;         // 0x80 | major<<4 | minor (0 = firmware that does not report it)
  uint8_t  flags;            // C6_INFO_F_* (firmware c6-4 onwards; before that it was reserved = 0)
  uint8_t  sono_estado;      // C6_SONO_* (firmware c6-5 onwards; before that it was reserved = 0)
  // THE C6 SLEEPING WITH Wi-Fi ASSOCIATED (firmware c6-5). Counted on the C6
  // itself - the P4 cannot see its serial: how many times it entered light
  // sleep, the total time spent in it, and how many times the link was torn
  // down/rebuilt.
  uint32_t sono_n;
  uint32_t sono_ms;
  uint16_t sono_entradas;    // times the P4 asked (POWER_SAVE_ON) and SDIO went down
  uint16_t sono_despertares; // times the link came back (via the P4 or via traffic)
  // What the P4 sent to the network and the C6 passed to Wi-Fi (esp_wifi_internal_tx).
  uint32_t tx_wifi_n;
  uint32_t tx_wifi_erros;
  int16_t  tx_wifi_ultimo_err;
  uint8_t  sta_conectada;    // esp_hosted thinks the station is associated
  uint8_t  reserva_sono;
  // THE FILTER with the host asleep: what the C6 handled alone, without waking the P4.
  uint32_t filtro_arp;       // ARP for the P4's IP, answered by the C6
  uint32_t filtro_descartes; // broadcast/multicast and other hosts' ARP, discarded
  uint32_t filtro_acordou;   // frames for the P4 that woke it
} c6_info_t;                 // 84 B (firmware c6-5; the P4 accepts shorter ones)
// C6_SONO_*: the state of "host sleeps" on the C6 side.
#define C6_SONO_SEM        0      // firmware without the feature (or older)
#define C6_SONO_ACORDADO   1      // link up, light sleep forbidden (lock)
#define C6_SONO_DORMINDO   2      // SDIO torn down, light sleep allowed, CMD arms the wake-up
#define C6_INFO_V1_TAM 20
#define C6_INFO_F_COREDUMP 0x01   // there is a crash snapshot saved in the C6's coredump partition
#define C6_INFO_F_EM_PROVA 0x02   // new image (OTA) not yet confirmed by VIVO    // size of INFO from firmware without the diagnostics

// --- CSI (Channel State Information) ------------------------------------------
// The channel state per received frame: amplitude/phase of the subcarriers.
// It is what gives "absurd precision" for presence and movement - the pattern
// changes when something between the transmitter and the C6 moves, even
// without the device in hand. The C6 delivers RAW CSI; the maths (variance,
// correlation) stays on the P4.
//
// FLOOD: a busy AP generates a lot of CSI. `min_intervalo_ms` discards on the
// C6 the frames that come too fast - one per interval is enough for movement.
// The MAC filter (target only) cuts the rest.
typedef struct __attribute__((packed)) {
  uint8_t  liga;
  uint8_t  so_alvo;          // 1 = only the target MAC
  uint8_t  alvo[6];
  uint16_t min_intervalo_ms; // minimum between records sent; 0 = 100
  int8_t   rssi_min;         // discard weaker; 0 = everything
  uint8_t  reserva;
} c6_csi_cfg_t;              // 12 B

// WHICH FRAMES GENERATE CSI. CSI is measured on the preamble's OFDM training
// fields (L-LTF, HT-LTF, HE-LTF). 2.4 GHz beacons usually go out at 1 Mbps
// (11b, DSSS), which has none of them: no CSI. Without promiscuous mode, the
// C6 only sees frames addressed to it - with Wi-Fi idle, almost nothing. For a
// constant stream, the reader needs to provoke traffic ('radio csi' pings the
// router, like Espressif's esp-csi); for CSI from other devices, sniffer on.
//
// The C6 driver delivers up to 1023 B (rx_channel_estimate_len has 10 bits):
// L-LTF 128 B, HT20 up to 256, HT40 and HE20 more. The ceiling below covers the
// maximum, and the CORTADO (truncated) bit warns if one day it does not.
#define C6_CSI_MAX 1024
typedef struct __attribute__((packed)) {
  uint8_t  mac[6];           // who it came from
  int8_t   rssi;
  uint8_t  canal;
  uint8_t  sec_canal;
  uint8_t  flags;            // C6_CSI_F_* and the frame format (C6_CSI_FORMATO)
  uint16_t len;              // CSI bytes that follow (<= C6_CSI_MAX)
  uint32_t t_us;             // the C6's receive timestamp
} c6_csi_cab_t;             // 16 B
#define C6_CSI_F_HT40      0x01   // there is a secondary channel (40 MHz)
#define C6_CSI_F_W0_RUIM   0x02   // the first 4 bytes are invalid (hardware limit: first_word_invalid)
#define C6_CSI_F_CORTADO   0x04   // the driver delivered more than C6_CSI_MAX
// Frame format, bits 4..7 (the driver's cur_bb_format): 0 11b, 1 11g,
// 2 HT, 3 VHT, 4 HE SU, 5 HE MU, 6 HE ER SU, 7 HE TB.
#define C6_CSI_FORMATO(f)  ((uint8_t)((f) >> 4))

// --- FTM (Fine Timing Measurement, 802.11mc) ---------------------------------
// Distance by time of flight, against an AP that speaks FTM (responder).
// One-shot: the P4 asks, the C6 runs a session and replies with the report.
typedef struct __attribute__((packed)) {
  uint8_t  peer[6];          // responder AP
  uint8_t  canal;
  uint8_t  frm_count;        // frames in the burst: 0(auto),8,16,24,32,64
  uint16_t burst_period;     // units of 100 ms; 0 = auto
} c6_ftm_cfg_t;             // 10 B

typedef struct __attribute__((packed)) {
  uint8_t  peer[6];
  uint8_t  status;           // 0 = success
  uint8_t  num;              // frames used
  uint32_t rtt_est_ns;       // estimated round-trip time of flight
  uint32_t dist_est_cm;      // distance estimated by the controller
  uint32_t rtt_raw_ns;       // with status 0xFF: the esp_err_t from initiate_session
} c6_ftm_rep_t;             // 20 B
// status: 0..6 = the driver's wifi_ftm_status_t (0 success, 1 not supported
// on the other side, 2 rejected, 3 no response, 4 failure, 5 no valid
// measurement, 6 terminated). 0xFF = the C6 could not even start - the reason
// goes in rtt_raw_ns.
#define C6_FTM_NAO_INICIOU 0xFF

// --- 802.15.4 (the C6's other radio: Thread / Zigbee / raw) -------------------
// The C6 has an IEEE 802.15.4 radio besides Wi-Fi/BT. In raw receive mode it
// hears the traffic of Thread and Zigbee meshes on the requested channel
// (11..26). It coexists with Wi-Fi through the same arbiter as BLE (the C6
// enables esp_coex_wifi_i154_enable). The C6 only forwards the raw frames;
// decoding is the P4's job.
typedef struct __attribute__((packed)) {
  uint8_t  liga;
  uint8_t  canal;            // 11..26
  uint8_t  promiscuo;        // 1 = no address filter (hears the whole mesh); 0 = only
                             // frames for the C6's PAN/address, which it does not have: almost nothing
  uint8_t  reserva;
  uint16_t min_intervalo_ms; // throttles forwarding; 0 = no limit
  uint16_t reserva2;
} c6_154_cfg_t;             // 8 B

#define C6_154_MAX 127       // maximum size of an 802.15.4 frame
typedef struct __attribute__((packed)) {
  uint8_t  canal;
  int8_t   rssi;
  uint8_t  lqi;              // link quality (0..255)
  uint8_t  len;              // frame bytes that follow (<= C6_154_MAX)
  uint32_t t_us;
} c6_154_cab_t;             // 8 B

// Limit of ESP-NOW v1.0, which is what the ESP32 and ESP32-S3 speak
// (Cardputer, Basic, StickC, M5Paper). v2.0 goes up to 1470, but ONLY if both
// sides support it - and they do not.
#define C6_TEXTO_MAX      250

// How long the C6 waits for the P4's "I am listening to you" after coming up
// with new firmware. Without that message in time, it points the boot back to
// the previous image itself and restarts.
//
// The maths: the C6 restarts 5 s after `end`, the P4 restarts right after and
// takes about 15-25 s for the esp_hosted stack to come up. 120 s leaves room
// for a slow boot without leaving the user staring at a silent device for too
// long.
#define C6_PRAZO_HOST_S   120
