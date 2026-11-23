// THE C6 SLEEPING WITH WI-FI ASSOCIATED - the P4 side.
//
// With the SDIO link up the C6 cannot enter light sleep, and connected Wi-Fi
// costs ~30 mA on battery with the screen off. The C6 firmware (c6/, from
// c6-5 onwards) can sleep while associated: the P4 signals it (POWER_SAVE_ON
// via the SDIO interrupt), both sides tear down the link, and the C6 sleeps,
// listening to the router from time to time.
//
// COMING BACK, FROM EITHER SIDE:
//   - the C6 has something for the P4 (an MQTT message, an ESP-NOW): it
//     re-mounts its slave and pulses IO2, which arrives here on G14;
//   - the P4 needs the network (screen on, keepalive): there is no wire to the
//     C6, so the P4 pulls the SDIO CMD line (G13) low for 2 ms - while idle,
//     that line is the C6's wake-up - and waits for the same pulse on G14.
// In both cases the P4 re-mounts its side WITHOUT resetting the C6 (a reset
// would drop the association): esp_hosted only does that when it thinks the
// host came back from deep sleep, and the re-mount here convinces it of that
// via a link-time wrap (__wrap_eh_host_port_wakeup_reason_get,
// main/CMakeLists.txt).
//
// UNDER A SETTING: the automatic mode (below) is off by default; the console
// has 'c6 sono auto ligar|desligar' and the manual test commands.
// With the link torn down, whatever the P4 sends to the network is dropped,
// and Bluetooth must be off (the P4's NimBLE stack talks to the C6 over the
// same link).
//
// From any task with its stack in internal RAM, except the UI task (the
// re-mount takes hundreds of ms).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
  TAB5_C6_SONO_ACORDADO = 0,   // link up (the normal case)
  TAB5_C6_SONO_DORMINDO,       // link torn down, the C6 may sleep
  TAB5_C6_SONO_FALHOU,         // the re-mount did not complete: Wi-Fi must be turned back on
} tab5_c6_sono_t;

typedef struct {
  tab5_c6_sono_t estado;
  uint32_t entradas;           // times the link was torn down
  uint32_t pelo_p4;            // times the P4 brought it back (CMD)
  uint32_t pelo_c6;            // times the C6 called (traffic for the P4)
  uint32_t falhas;
  uint32_t ultimo_religar_ms;  // how long the last re-mount took
  uint32_t dormindo_ms;        // total time with the link torn down
} tab5_c6_sono_estado_t;

// Tears down the link and lets the C6 sleep. false = it could not, and
// 'motivo' says why (no link, Bluetooth on, C6 firmware without the feature...).
bool tab5_c6_sono_dormir(char* motivo, size_t tam);

// Brings the link back (if torn down). false = it did not come back.
bool tab5_c6_sono_acordar(void);

// THE AUTOMATIC MODE (ajustes_c6_sono): the link tears itself down with the
// screen off and quiet for 3 s, and comes back with the screen on, with C6
// traffic or with the P4 transmitting. iniciar(): at boot, starts the task if
// the setting is on.
void tab5_c6_sono_iniciar(void);
void tab5_c6_sono_automatico(bool ligar);
void tab5_c6_sono_estado(tab5_c6_sono_estado_t* out);
void tab5_c6_sono_rx(uint32_t* arp, uint32_t* icmp, uint32_t* outro, uint32_t* falha);   // test debugging
