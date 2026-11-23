// Mesh service - GPS POSITION for the mesh. Split out of tab5_mesh.cpp (the
// service); what the parts share lives in mesh/tab5_mesh_interno.hpp.

#include "esp_log.h"
#include "esp_random.h"
#include "ajustes.h"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_mesh_admin.hpp"
#include "mesh_pb.hpp"
#include "tab5_gps.hpp"
#include "tab5_hora.hpp"
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

// ---------------------------------------------------------------------------
// GPS POSITION FOR THE MESH
// ---------------------------------------------------------------------------
// With the C6L on Bluetooth the Grove port is free for the GPS. The position goes
// in a MeshPacket, port POSITION_APP (3), with a Position inside:
//   1: latitude_i  (sfixed32, degrees x 1e7)
//   2: longitude_i (sfixed32, degrees x 1e7)
//   3: altitude    (int32, metres)
//   4: time        (fixed32, epoch UTC)
//
// TO WHOM - and the difference is airtime.
//   - WITH THE NODE on the other end: ONLY TO IT (to = its number), as the
//     phone app does. It then uses it as its own position and broadcasts it
//     at its own rate (Position > "send every", "on movement"), with the
//     channel's precision. It used to go as a BROADCAST, 3 hops: the mesh got
//     the same position twice - the Tab5's and, right after, the node repeating
//     what it already knew, each at its own rate.
//   - The node ALREADY KNOWS where it is (fixed position, or its own GPS on):
//     nothing to send. A fixed one would ignore ours; its GPS would override it.
//   - OVER THE INTERNET (no node, or Bluetooth dropped and MQTT covers): the
//     Tab5 is the node, so it broadcasts itself - with the precision of the
//     primary channel the node left configured (0 = don't send; under 32 = region only).
// Without a fix it sends nothing (returns false and the task retries in 30 s);
// true = settled until the next round (sent, or nothing to send).
bool enviar_posicao(void)
{
  if (!s_servico_ativo || !tab5_gps_servico_ativo()) { return false; }
  const bool com_no = radio_aceita_admin();
  const bool pela_net = !com_no && (s_via == AJUSTES_MESH_INTERNET || s_via == AJUSTES_MESH_BLUETOOTH)
                     && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();
  if (!com_no && !pela_net) { return false; }
  if (com_no && mesh_admin_no_tem_posicao_propria()) { return true; }
  const int32_t prec = mesh_admin_precisao_primario();
  if (pela_net && prec == 0) { return true; }

  const nmea_posicao_t g = tab5_gps_posicao();
  if (!g.valida || g.qualidade == 0) { return false; }
  int32_t lat = (int32_t)(g.lat * 1e7), lon = (int32_t)(g.lon * 1e7);
  if (pela_net && prec > 0 && prec < 32) {
    // The same truncation as the firmware (PositionModule): the leftover bits go
    // to the middle of the cell, not the corner.
    const uint32_t mascara = 0xFFFFFFFFu << (32 - prec);
    lat = (int32_t)(((uint32_t)lat & mascara) + (1u << (31 - prec)));
    lon = (int32_t)(((uint32_t)lon & mascara) + (1u << (31 - prec)));
  }

  uint8_t pos[32];
  pb_escritor_t w = pb_escritor(pos, sizeof(pos));
  mesh_pb_posicao(&w, lat, lon, (int32_t)g.altitude_m, tab5_hora_epoch());

  // To the node: no hops (it doesn't leave it). Broadcast: 3 hops (see tab5_mesh_enviar).
  const mesh_pb_envelope_t env = { com_no ? s_meu_num : MESH_PB_DIFUSAO, -1, PORT_POSICAO, pos,
                                   pb_tamanho(&w), esp_random() | 1u, (uint8_t)(com_no ? 0 : 3), false };
  uint8_t quadro[96];
  const size_t n = pb_tamanho(&w) ? mesh_pb_to_radio(&env, quadro, sizeof(quadro)) : 0;
  const bool ok = n && enlace_enviar(quadro, n);
  if (ok) {
    ESP_LOGI(TAG, "GPS position %s (%.5f, %.5f)", com_no ? "passed to the node" : "broadcast over the internet",
             g.lat, g.lon);
  }
  return ok;
}
