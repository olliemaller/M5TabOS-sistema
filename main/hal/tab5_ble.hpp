// SYSTEM BLUETOOTH - BLE via the Tab5's internal C6.
//
// The P4 has no radio. The BLE controller lives on the C6 (the same chip as
// WiFi and ESP-NOW), and our C6 firmware starts with it enabled; HCI packets
// travel over the same SDIO as WiFi (esp_hosted). The STACK lives here -
// NimBLE, on the P4 - plus what everyone using Bluetooth needs in common:
//
//   - turning on and off (the C6 time-shares the radio between WiFi, ESP-NOW
//     and BLE by itself; all three coexist, each a little slower);
//   - SCANNING: who is nearby, with name, signal and whether it is a
//     Meshtastic node.
//
// Connection and GATT belong to whoever is talking (see
// services/tab5_mesh_ble.cpp): NimBLE delivers a connection's events to the
// callback of whoever opened it, and each protocol has its own way of reading
// and writing.
//
// BLE only: the C6 does not do Classic Bluetooth (headphones, old speakers).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  TAB5_BLE_DESLIGADO = 0,
  TAB5_BLE_SUBINDO,      // waiting for the C6 and the stack to sync
  TAB5_BLE_PRONTO,
  TAB5_BLE_FALHOU,       // the C6 did not provide the controller (firmware without BT?)
  TAB5_BLE_DESLIGANDO,   // shutdown request accepted; the stack is going down
} tab5_ble_estado_t;

// Creates the task. Turns nothing on - like tab5_wifi_iniciar().
void tab5_ble_iniciar(void);

// Turns the stack on/off. Turning on brings up the link to the C6 if needed
// (radio rail = I2C): call from the UI task. Asynchronous.
void tab5_ble_ligar(void);
void tab5_ble_desligar(void);

tab5_ble_estado_t tab5_ble_estado(void);
bool        tab5_ble_pronto(void);
const char* tab5_ble_descricao(void);

// ---------------------------------------------------------------------------
// SCAN - who is nearby
// ---------------------------------------------------------------------------
#define TAB5_BLE_VISTOS 24

// WHAT THE DEVICE IS, from what it advertises (without connecting). Tracker
// is an item-finder tag - AirTag and Apple's Find My network, Tile, Samsung
// SmartTag, Google's Find My Device network; beacon is the fixed shop/museum
// beacon (iBeacon, Eddystone).
typedef enum {
  TAB5_BLE_OUTRO = 0,
  TAB5_BLE_MESHTASTIC,
  TAB5_BLE_RASTREADOR,
  TAB5_BLE_BEACON,
} tab5_ble_classe_t;

typedef struct {
  uint8_t  addr[6];      // NimBLE order (byte 0 = the last one in the text)
  uint8_t  tipo;         // BLE_ADDR_PUBLIC / RANDOM
  int8_t   rssi;         // moving average: a single report varies 5-10 dB
  bool     meshtastic;   // advertises the Meshtastic service
  uint8_t  classe;       // tab5_ble_classe_t
  char     marca[16];    // "Find My", "Tile", "iBeacon"... ("" = unknown)
  int8_t   p1m;          // RSSI at 1 m as declared by the advert itself; 0 = not declared
  char     nome[24];
  uint32_t visto_ms;
} tab5_ble_visto_t;

// ADVERT LISTENER - the RAW report, one per packet received, without the
// moving average of the list above. For whoever needs the exact instant of
// each reading (the radar matches RSSI against the device's heading at the
// moment the packet arrived). Runs in the NimBLE task: do not block, do not
// draw, do not talk I2C - just enqueue and return. One listener only;
// nullptr removes it.
typedef void (*tab5_ble_ouvinte_t)(const uint8_t addr[6], int8_t rssi);
void tab5_ble_ouvinte(tab5_ble_ouvinte_t f);

// Cancels the scan in progress (no-op if there is none).
void tab5_ble_varredura_parar(void);

// Scans for 'ms' (without waiting). false if the stack is not ready or a
// scan/connection is already using the radio. The list is NOT cleared: those
// that disappeared age out.
bool     tab5_ble_varrer(uint32_t ms);
bool     tab5_ble_varrendo(void);
int      tab5_ble_n_vistos(void);
bool     tab5_ble_visto(int i, tab5_ble_visto_t* out);
// Changes on every update (state, scan, new device): the screen compares it
// with what it drew and only repaints when it changes.
uint32_t tab5_ble_versao(void);

// "AA:BB:CC:DD:EE:FF" <-> NimBLE order.
void tab5_ble_endereco_txt(const uint8_t a[6], char out[18]);
bool tab5_ble_endereco_ler(const char* txt, uint8_t a[6]);

// The Meshtastic service UUID (whoever connects to it needs the same one).
#define TAB5_BLE_UUID_MESHTASTIC "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
