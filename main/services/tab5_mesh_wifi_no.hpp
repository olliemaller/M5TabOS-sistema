// MESHTASTIC NODE WI-FI NETWORKS, stored on the SD card.
//
// The node has only ONE network configured at a time (Config.network), and
// switching meant typing the name and password again. This keeps the networks
// already written to it, to pick from a list. File: Meshtastic/wifi_no.txt, a
// pair of lines per network (name, password encrypted with the device key - the
// same as the Tab5's wifi.txt; "-" = open network). The C6L sees only 2.4 GHz, like the Tab5.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_WIFI_REDES_MAX 8

typedef struct {
  char ssid[33];
  char senha[65];     // plain text, only in RAM; empty = open network
  bool aberta;
} mesh_wifi_rede_t;

void     mesh_wifi_no_carregar(void);                    // SD card -> RAM, when the service starts
int      mesh_wifi_no_n(void);
bool     mesh_wifi_no_rede(int i, mesh_wifi_rede_t* o);  // 0 = the most recent
// Stores (or updates) and moves it to the top of the list. Empty password = open network.
bool     mesh_wifi_no_salvar(const char* ssid, const char* senha);
bool     mesh_wifi_no_esquecer(int i);
// Copies in the networks stored on the Tab5 that are not in the list yet.
// Returns how many were added, or -1 if the Tab5 has none loaded.
int      mesh_wifi_no_trazer_do_tab5(void);
uint32_t mesh_wifi_no_versao(void);
