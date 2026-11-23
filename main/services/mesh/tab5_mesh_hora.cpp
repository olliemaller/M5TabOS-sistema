// Mesh service - TIME: the corrected timestamp when the clock jumps, the time
// borrowed from the radio and the live time. Split out of tab5_mesh.cpp (the service);
// what the parts share lives in mesh/tab5_mesh_interno.hpp.

#include "app.h"   // sistema_avisar
#include "esp_log.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "tab5_hora.hpp"
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

// ---------------------------------------------------------------------------
// CORRECTED TIMESTAMP - when the clock jumps (see tab5_hora.hpp)
// ---------------------------------------------------------------------------
// A sent message carries OUR clock's time. If it was wrong and a source
// corrects it later, the message would sit on the wrong day forever: its line
// is already on the SD card. Rewriting the conversation file for that would be
// expensive; instead an "H,<id>,<when>" line goes into envios.txt, the same
// append-only file of outcomes, and the history reader replaces the timestamp
// of whoever has an id there (last line wins, as with "E").
//
// Only THIS session's messages: earlier ones were stamped by another
// clock (another boot), which may have been right.
#define MESH_CARIMBADAS 16
typedef struct { uint32_t id; uint32_t quando; } carimbada_t;
static carimbada_t s_carimbadas[MESH_CARIMBADAS];
static int         s_prox_carimbada = 0;

// With the lock held.
void lembrar_carimbo(uint32_t id, uint32_t quando)
{
  if (id == 0 || quando == 0) { return; }
  s_carimbadas[s_prox_carimbada].id     = id;
  s_carimbadas[s_prox_carimbada].quando = quando;
  s_prox_carimbada = (s_prox_carimbada + 1) % MESH_CARIMBADAS;
}

// UI task, right after the clock is set. Only RAM and the system writer
// (tab5_sd_anexar copies and returns): no SD card here.
void ao_saltar_relogio(int32_t delta)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_CARIMBADAS; ++i) {
    carimbada_t* c = &s_carimbadas[i];
    if (c->id == 0) { continue; }
    c->quando = (uint32_t)((int64_t)c->quando + delta);
    for (int k = 0; k < s_n_msgs; ++k) {
      if (s_msgs[k].id == c->id) { s_msgs[k].quando = c->quando; }
    }
    char linha[40];
    snprintf(linha, sizeof(linha), "H,%08lX,%lu\n", (unsigned long)c->id, (unsigned long)c->quando);
    tab5_sd_anexar(MESH_PASTA, MESH_ARQ_ENVIOS, linha);
    ++n;
  }
  // The app rereads the history when this counter changes: that is what takes
  // the wrong-day bubble off the screen.
  if (n) { ++s_total_msgs; }
  destravar();
  if (n) {
    ESP_LOGI(TAG, "device time corrected by %+ld s: time of %d sent message(s) "
                  "from before the correction adjusted", (long)delta, n);
    sistema_avisar(AVISO_MESH_MSG);
  }
}

// ---------------------------------------------------------------------------
// WHAT TIME IT IS - borrowed from the radio
// ---------------------------------------------------------------------------
// The Tab5 wakes without the right time, but every NodeInfo carries last_heard and
// every packet carries rx_time, both in epoch seconds from the C6L's clock.
// We keep the largest timestamp seen so far along with our uptime at that moment;
// from then on the time advances with our own clock, and the "how long ago"
// calculation closes with an error of seconds.
static uint32_t s_epoch_ref    = 0;       // radio timestamp
static uint32_t s_epoch_ref_ms = 0;       // our uptime when it arrived

uint32_t tab5_mesh_epoch(void)
{
  if (s_epoch_ref == 0) { return 0; }
  return s_epoch_ref + (tab5_agora_ms() - s_epoch_ref_ms) / 1000;
}

// Only accepts a timestamp that moves the clock FORWARD. That way an old
// last_heard from the initial dump never pulls the reference backwards.
void marcar_epoch(uint32_t carimbo)
{
  if (carimbo < EPOCH_MINIMO) { return; }
  const uint32_t agora = tab5_mesh_epoch();
  if (carimbo <= agora) { return; }
  s_epoch_ref    = carimbo;
  s_epoch_ref_ms = tab5_agora_ms();
}

// ---------------------------------------------------------------------------
// LIVE TIME - the only one in this file that sets the system clock
// ---------------------------------------------------------------------------
// tab5_mesh_epoch() above is for "how long ago", and only that. Anchored to the
// LARGEST last_heard in the node list, it lags BEHIND by however long the mesh
// has been silent - the most recent node may have spoken yesterday. Used as "now"
// it set the clock with that lag: a sent message went out stamped on the
// wrong day, and the conversation's "TODAY" only jumped to the right day when
// a real reply arrived.
//
// Only timestamps that are "now" by construction get in here:
//   - rx_time of a packet that arrived AFTER the node finished the dump and drained
//     the queue of what it kept while we were disconnected (those carry the
//     rx_time of when IT received them, minutes or hours ago);
//   - the 'time' field of our own node's telemetry, through the same filter.
// rx_time always comes from OUR node's clock (it stamps on receipt), even
// for a packet that crossed the mesh or came over the MQTT bridge.
#define QUEUE_DRENADA_MS 10000u            // after the end of the dump
// Only valid while fresh: extending the node's time with esp_timer accumulates the
// internal RC error in light sleep (~5%, see NTP_VALIDO_MS in tab5_hora.cpp), and the
// correction would write that error to the RTC. Packets arrive all the time with the node active.
#define HORA_VIVA_VALE_MS (30u * 1000u)

uint32_t s_config_ok_ms = 0;        // end of dump; 0 = still running
static uint32_t s_vivo_ref     = 0;
static uint32_t s_vivo_ref_ms  = 0;

bool trafego_ao_vivo(void)
{
  return s_config_ok_ms != 0 && (tab5_agora_ms() - s_config_ok_ms) > QUEUE_DRENADA_MS;
}

void marcar_hora_viva(uint32_t carimbo)
{
  if (carimbo < EPOCH_MINIMO || !trafego_ao_vivo()) { return; }
  s_vivo_ref    = carimbo;
  s_vivo_ref_ms = tab5_agora_ms();
}

uint32_t tab5_mesh_hora_viva(void)
{
  const uint32_t ref = s_vivo_ref, ms = s_vivo_ref_ms;
  if (ref == 0 || (tab5_agora_ms() - ms) > HORA_VIVA_VALE_MS) { return 0; }
  return ref + (tab5_agora_ms() - ms) / 1000;
}
