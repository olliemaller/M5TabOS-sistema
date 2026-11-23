// BLE LINK TO THE MESHTASTIC NODE.
//
// The other path, besides the Grove cable. The protocol is the SAME - ToRadio
// and FromRadio in protobuf - only the transport changes:
//
//   serial (Grove):  each message goes in a frame 0x94 0xC3 <length> <body>
//   BLE:             each message is ONE characteristic value, no frame
//                      toRadio   (write)    - what the Tab5 sends
//                      fromRadio (read)     - one message per read; empty = done
//                      fromNum   (notify)   - "there is something new in fromRadio"
//
// That is why the Mesh service (tab5_mesh.cpp) knows nothing of BLE: it hands
// over the ToRadio body and receives the FromRadio body, and the link is what
// frames it (serial) or writes it to the characteristic (here).
//
// With the C6L on Bluetooth the Grove port is free - and the GPS can go on it,
// with the position going out to the mesh (ajustes_mesh_posicao).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Starts the link task: looks for the node (the address saved in settings, or
// the first Meshtastic that shows up), connects, pairs with the PIN and keeps
// delivering what arrives. Reconnects by itself if dropped. The caller has
// already turned Bluetooth on (tab5_ble_ligar, interface task).
bool mesh_ble_iniciar(void);
// Stops and disconnects. Waits for the task to exit (up to 6 s).
bool mesh_ble_parar(void);
// The link task exists and has not exited yet (whoever is about to tear down
// the BLE stack asks first: it must not vanish under a connection in progress).
bool mesh_ble_rodando(void);

// Queues a ToRadio (body, without the 0x94 0xC3). false with no connection.
bool mesh_ble_enviar(const uint8_t* corpo, size_t n);
// Next FromRadio that arrived (body), waiting up to 'espera_ms'. 0 = nothing.
size_t mesh_ble_receber(uint8_t* buf, size_t max, uint32_t espera_ms);
// true ONCE per new connection: the cue to request the configuration from the node.
bool mesh_ble_conectou_agora(void);

bool        mesh_ble_conectado(void);
const char* mesh_ble_descricao(void);

// PIN ON THE SPOT: the node asked for the PIN and the link is waiting for
// someone to type it (a node with a random PIN shows a new one on every pairing).
// Settings > Meshtastic opens the keyboard by itself; mesh_ble_digitar_pin delivers it (~30 s).
bool mesh_ble_esperando_pin(void);
bool mesh_ble_digitar_pin(uint32_t pin);
// After a rejected PIN the link stops and waits: this restarts it now (the
// next pairing asks for the PIN on the spot).
void mesh_ble_tentar_de_novo(void);
