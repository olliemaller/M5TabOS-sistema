// MESHTASTIC NODE RESTART JOURNAL.
//
// Through the protocol the node does not say WHY it restarted (the ESP32
// "reset reason" only exists in its console log). What the Tab5 can do is
// note, the moment it notices the restart, everything IT was doing around
// then: whether it touched the Grove 5V, whether it reapplied the power
// profile, what it last sent to the node, the battery voltage. That separates
// "the Tab5 caused it" from "the node went down by itself" - deduction, not certainty.
//
// How it notices: MyNodeInfo.reboot_count (kept in the node's NVS) went up
// since the last read. Persisted on the SD card (Meshtastic/reinicios.txt), one per line.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_DIARIO_MAX 40

typedef enum {
  MESH_REI_BASE = 0,      // first read of the counter: starting point, not a restart
  MESH_REI_SOZINHO,       // the counter went up with the Tab5 connected, without a request from us
  MESH_REI_PEDIDO,        // the Tab5 sent a restart / write-configuration
  MESH_REI_AUSENTE,       // went up while the Tab5 was not connected: exact time unknown
} mesh_rei_origem_t;

typedef enum { MESH_ENV_NENHUM = 0, MESH_ENV_PACOTE, MESH_ENV_CONFIG, MESH_ENV_HEARTBEAT, MESH_ENV_ADMIN } mesh_env_tipo_t;

typedef struct {
  uint32_t epoch;        // time (UTC) of the record; 0 = the Tab5 did not know the time yet
  uint32_t up_tab5_s;    // Tab5 uptime at the record
  uint32_t contador;     // MyNodeInfo.reboot_count after the restart
  uint32_t up_no_s;      // node uptime in the last telemetry BEFORE it went down; 0 = unknown
  uint8_t  origem;       // mesh_rei_origem_t
  uint8_t  envio_tipo;   // our last send that was not a heartbeat (mesh_env_tipo_t)
  uint16_t centivolts;   // battery bus voltage at that moment (0 = no reading)
  int32_t  ma;           // bus current
  int32_t  envio_s;      // how long before the record that send was; -1 = never
  int32_t  grove_s;      // how long before the Tab5 touched the Grove 5V; -1 = never
  int32_t  perfil_s;     // how long before the Tab5 reapplied the power profile; -1 = never
} mesh_reinicio_t;

// ---- calls from the service (tab5_mesh.cpp) --------------------------------
void mesh_diario_carregar(void);                 // from the SD card, when the service starts
void mesh_diario_nota_envio(uint8_t tipo);       // every outgoing ToRadio
// Snapshot of the context, when the node reports it restarted (FromRadio.rebooted).
void mesh_diario_foto(uint32_t up_no_s);
// MyNodeInfo arrived with 'contador'; decides whether it becomes a record.
void mesh_diario_contador(uint32_t contador, uint32_t up_no_s, bool conectados_antes, bool pedido_nosso);

// ---- reading / screen ------------------------------------------------------
int      mesh_diario_ler(mesh_reinicio_t* saida, int max);   // newest to oldest
uint32_t mesh_diario_versao(void);
void     mesh_diario_limpar(void);                           // erases from RAM and the SD card
// Short phrase with the strongest clue for this record.
const char* mesh_diario_veredito(const mesh_reinicio_t* r);
