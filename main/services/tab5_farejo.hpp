// FAREJO (SNIFFER) - WiFi in promiscuous mode, seen from the P4.
//
// esp_hosted doesn't pass promiscuous mode through, so it runs INSIDE the C6
// (our firmware, folder c6/) and arrives here already SUMMARISED: the C6 gathers by MAC
// during the batch period and sends a single batch (see FAREJADOR in
// compartilhado/tab5_c6.h). This service keeps the last state of each MAC
// heard; users (Radar, console) read the table whenever they like.
//
// COST WHEN OFF: zero. There is no permanent task or table: on switching on, it takes a
// pool slot (renews the request on the C6 and waits) and allocates the table in PSRAM; on
// switching off, it gives both back.
//
// POWER: sniffing keeps the C6's receiver on the whole time. That is why the C6
// SWITCHES OFF BY ITSELF if this service stops renewing (app closed, crashed) - and
// why whoever switches it on has to switch it off when the screen using the sniffer closes.
//
// CHANNELS: with WiFi connected the C6 stays on the router's channel (changing
// would drop the connection). To sweep several channels, detach from the router first
// (tab5_wifi_modo_so_radio) - which is what ESP-NOW's fixed channel already does.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define FAREJO_MAX 128   // MACs kept; when full, the one heard longest ago goes

typedef struct {
  uint8_t  mac[6];
  int8_t   rssi_ult;      // from the last batch
  int8_t   rssi_max;      // from the last batch
  int8_t   rssi_med;      // from the last batch
  uint8_t  tipos;         // C6_FT_*, accumulated since it appeared
  uint8_t  canal;
  uint32_t quadros;       // accumulated
  uint32_t visto_ms;      // tab5_agora_ms() of the last batch it appeared in
} farejo_item_t;

// Switches on, or changes the configuration (c6_farejo_cfg_t in tab5_c6.h; cfg->liga is
// ignored). Doesn't block. false = C6 without our firmware, or no slot.
bool tab5_farejo_ligar(const c6_farejo_cfg_t* cfg);
void tab5_farejo_desligar(void);
bool tab5_farejo_ativo(void);

// Can the C6 firmware sniff? Read from PRONTO/INFO; false until known.
bool tab5_farejo_c6_suporta(void);
// All the capabilities of the flashed C6 (C6_CAP_*), from PRONTO/INFO; 0 until known.
uint8_t tab5_farejo_caps_c6(void);

// EVERY RECORD OF EVERY BATCH, for whoever needs RSSI over time (Radar matches
// each one with the device's heading at that moment). In esp_hosted's event
// task: just copy and leave. 't_us' is the MIDDLE of the batch period (esp_timer),
// the best guess of when that mean RSSI was measured. A single listener;
// nullptr removes it.
typedef void (*tab5_farejo_ouvinte_t)(const c6_farejo_reg_t* reg, int64_t t_us);
void tab5_farejo_ouvinte(tab5_farejo_ouvinte_t cb);

// Table. Copies (the batch arrives in another task). Order: arrival order.
int      tab5_farejo_n(void);
bool     tab5_farejo_item(int i, farejo_item_t* out);
uint32_t tab5_farejo_lotes(void);      // only goes up: "a new batch has arrived since I looked"
uint32_t tab5_farejo_perdidos(void);   // MACs that didn't fit on the C6, accumulated
void     tab5_farejo_limpar(void);

// Asks the C6 (contract version, heap, frames). BLOCKS for up to 'espera_ms'
// - only from a task that can wait (console), never from the UI.
bool tab5_farejo_info_c6(c6_info_t* out, uint32_t espera_ms);

// Called by the ESP-NOW service when PRONTO arrives (carries version and capabilities).
void tab5_farejo_anotar_pronto(const uint8_t* dados, uint16_t n);
