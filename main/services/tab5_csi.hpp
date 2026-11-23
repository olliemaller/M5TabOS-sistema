// CSI - the state of the WiFi channel between the router and the Tab5, seen from the P4.
//
// WHAT IT TELLS. CSI is the amplitude and phase of each OFDM subcarrier of a
// received frame. With a single antenna it gives NO direction - but it changes a lot when
// something moves in the path between the router and the Tab5: a person walking, a
// door opening, even with no phone at all. This service turns that into a
// MOTION level (still / light / strong).
//
// WHERE IT COMES FROM. CSI is measured on the OFDM preamble, and the router's beacon at
// 2.4 GHz (1 Mbps, 11b) doesn't have one: with no traffic to the C6, almost nothing arrives. That is
// why the service pings the router at 20 Hz while on (the same trick as
// Espressif's esp-csi) and asks the C6 only for CSI coming FROM THE ROUTER - with
// sniffing on, the C6 also measures CSI from other people's frames, which are no use as a
// reference.
//
// THE DEVICE MUST NOT MOVE. Rotating or holding the Tab5 in your hand changes the CSI as much
// as a person walking. The user signals it (tab5_csi_aparelho_mexendo) and the
// service freezes the reference; the level comes back after the device is still.
//
// SOLE OWNER OF THE MESSAGE. The P4's peer_data keeps ONE listener per msg_id, and
// registering again REPLACES the previous one. That is why only this service registers
// C6_MSG_CSI; anyone else who wants the raw records (the console test) uses
// tab5_csi_ouvinte().
//
// COST WHEN OFF: zero - no task, no ping, no request to the C6. On: a pool
// slot, the 20 Hz ping and the C6's receiver always on.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

// Who requests (one bit each): the service stays on while there is one and declares
// RECURSO_RADIO_ESCUTA (kernel/recursos.h); the kernel keeps the receiver on.
#define TAB5_CSI_RADAR    (1u << 0)
#define TAB5_CSI_CONSOLE  (1u << 1)

void tab5_csi_ligar(uint32_t quem);
void tab5_csi_desligar(uint32_t quem);

typedef enum {
  TAB5_CSI_DESLIGADO = 0,
  TAB5_CSI_SEM_C6,          // C6 without our firmware, or without CSI flashed
  TAB5_CSI_SEM_ROTEADOR,    // no WiFi connected: nothing to measure from
  TAB5_CSI_APRENDENDO,      // measuring this place's "still" baseline (first few seconds)
  TAB5_CSI_APARELHO_MEXENDO,
  TAB5_CSI_MEDINDO,
} tab5_csi_fase_t;

typedef enum { TAB5_CSI_PARADO = 0, TAB5_CSI_LEVE, TAB5_CSI_FORTE } tab5_csi_nivel_t;

typedef struct {
  tab5_csi_fase_t  fase;
  tab5_csi_nivel_t nivel;      // valid in TAB5_CSI_MEDINDO
  float            razao;      // deviation now / still deviation (1 = still)
  uint16_t         por_s;      // CSI records in the last second
  int8_t           rssi;       // from the last record
} tab5_csi_estado_t;

void tab5_csi_estado(tab5_csi_estado_t* out);

// Is the Tab5 moving (rotation, hand)? While it is, the measurement is suspended and
// the reference restarts when it stops. Cheap: just writes a variable.
void tab5_csi_aparelho_mexendo(bool sim);

// The raw records, in esp_hosted's event task: just copy and leave.
// A single listener (the last to call); nullptr removes it.
typedef void (*tab5_csi_ouvinte_t)(const c6_csi_cab_t* cab, const uint8_t* dados, uint16_t n);
void tab5_csi_ouvinte(tab5_csi_ouvinte_t cb);
