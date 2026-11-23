// ESP-NOW - a system service.
//
// The P4 has no radio, and esp_hosted doesn't carry ESP-NOW: the esp_now_* functions
// don't even link on this side. That is why ESP-NOW runs INSIDE the C6, in our
// firmware (folder c6/), and this service talks to it over esp_hosted's peer_data
// channel - the message contract lives in compartilhado/tab5_c6.h.
//
// Same mould as Mesh: the service owns the radio and the history, notifies via
// notification with the app closed, and the ESP-NOW app is just the window.
//
// ONLY EXISTS with our firmware on the C6 (tab5_wifi_c6_nosso). With the factory one,
// the service sits waiting, quietly - nothing breaks.
//
// THE CHANNEL: ESP-NOW only gets through between devices on the SAME WiFi channel. Associated with
// a router, the C6 stays on ITS channel - whoever talks to the Tab5 (Cardputer)
// needs to be on that channel. tab5_espnow_canal() says which one it is.
//
// POWER: dozing between beacons (modem sleep) the C6 loses ESP-NOW
// frames. The service declares RECURSO_RADIO_ESCUTA (kernel/recursos.h) and the
// kernel keeps the receiver on, only with a fixed channel, with the app open and the
// screen on, or with a conversation in the last 5 min; otherwise the radio saves power
// like any WiFi.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define ESPNOW_HIST 64   // messages kept in RAM (the SD card keeps everything)

typedef enum {
  ESPNOW_ENVIO_NADA = 0,   // received
  ESPNOW_ENVIO_PENDENTE,   // left the Tab5, the C6 hasn't confirmed yet
  ESPNOW_ENVIO_OK,         // the C6 transmitted (broadcast has no ACK: "left through the antenna")
  ESPNOW_ENVIO_FALHOU,
} espnow_envio_t;

typedef struct {
  uint8_t  mac[6];         // who sent it (received) or to whom (mine)
  bool     minha;
  bool     difusao;
  bool     espressif;       // arrived in the esp-now component's format (official Cardputer); false = plain text
  uint8_t  envio;          // espnow_envio_t
  uint32_t quando;         // UTC epoch; 0 = clock not set yet
  char     texto[C6_TEXTO_MAX + 1];   // for display: control characters become spaces, cut at the '\0'
  uint16_t n;              // REAL bytes (the text may have 0x00 in the middle: strlen would cut it)
  uint8_t  bruto[C6_TEXTO_MAX];   // the n bytes as they arrived (or as they left), untreated
} espnow_msg_t;

// Starts the task. Call at boot, after WiFi.
void tab5_espnow_iniciar(void);

// The C6 confirmed ESP-NOW is on air (sent PRONTO).
bool    tab5_espnow_pronto(void);
uint8_t tab5_espnow_canal(void);
void    tab5_espnow_meu_mac(uint8_t out[6]);
bool    tab5_espnow_c6_compativel(void);   // does the C6 have our firmware?

// History in RAM: 0 = the oldest. Copies (the table changes in another task).
int      tab5_espnow_n(void);
bool     tab5_espnow_msg(int i, espnow_msg_t* out);
uint32_t tab5_espnow_total(void);    // only goes up - for "changed since I looked"

// Distinct devices heard this boot, and how long ago the last frame was.
int      tab5_espnow_vistos(void);
uint32_t tab5_espnow_ms_desde_rx(void);   // UINT32_MAX = nothing yet

// CHANNEL. 0 = the router's (default: WiFi stays connected). 1..13 = FIXED
// channel: WiFi lets go of the router (the radio can only be on one channel at a time) and
// ESP-NOW stays on that channel - that is how to talk to a device that isn't
// on the router's channel (a Cardputer on its own stays on 1), and how ESP-NOW
// stays alive out of the router's range. Doesn't block.
bool    tab5_espnow_definir_canal(uint8_t canal);
uint8_t tab5_espnow_canal_fixo(void);

// Sends as a broadcast (everyone on the channel hears). Doesn't block: goes into the service's
// queue. false = queue full, empty text, or ESP-NOW off air.
bool tab5_espnow_enviar(const char* texto);

// Sends DIRECTLY to a MAC (unicast; the C6 adds the peer by itself). Same
// path and same rules as tab5_espnow_enviar (which is this one with FF:FF:...).
bool tab5_espnow_enviar_para(const uint8_t mac[6], const char* texto);

// Sends on ALL channels (1 to 13), one after the other, and returns to the previous
// channel. ESP-NOW only gets through on the SAME channel as the other device, and with Wi-Fi
// connected the radio is stuck on the router's channel: the sweep lets go of the
// router (Wi-Fi DROPS for a few seconds) and reconnects by itself at the end.
// mac_ou_null: nullptr = broadcast. Stores ONE message in the history.
bool tab5_espnow_enviar_todos_canais(const uint8_t* mac_ou_null, const char* texto);

// "AA:BB:CC:DD:EE:01" (or with '-') to 6 bytes. false = invalid format.
bool tab5_espnow_mac_de_texto(const char* txt, uint8_t mac[6]);

// The app signals that the window opened/closed - with it open (and the screen on) the
// receiver stays on. See POWER above.
void tab5_espnow_janela(bool aberta);

// Unread (received since the last time someone looked).
int  tab5_espnow_nao_lidas(void);
void tab5_espnow_marcar_lidas(void);
