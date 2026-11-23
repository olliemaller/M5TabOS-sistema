// WI-FI DOZES WHILE IDLE - connected Wi-Fi was 40% of the consumption with the
// screen off.
//
// MEASURED ('energia ensaio', 2026-10-16): idle 72-74 mA, 28-33 of it from connected
// Wi-Fi - even in maximum modem sleep, because the C6 cannot really sleep
// with SDIO active (see the esp_hosted example
// cp_light_sleep_controller.c). Without Wi-Fi, 38 mA: the battery lasts almost twice as long.
//
// THE RULE ("economia em repouso" (idle saving) setting on the Wi-Fi page, on by default):
//   - screen off for COCHILO_APOS_S and nothing needing the network -> turn off;
//   - turning the screen on brings it back at once;
//   - every JANELA_CADA_S it comes back on by itself and stays connected for JANELA_S,
//     so Mesh over the internet and the clock pick up whatever there is;
//   - "needs the network": someone asked for the receiver on (ESP-NOW chatting,
//     CSI, sniffing) or a background part declared the radio in its profile.
// The user's Wi-Fi setting (ajustes_wifi) does not change: only the network goes down.
//
// TASK: tab5_wifi_cochilo_vigiar() runs on the UI task (turning Wi-Fi on
// touches the I2C expander), once a second.
#pragma once

#include <stddef.h>

void tab5_wifi_cochilo_vigiar(void);
void tab5_wifi_cochilo_resumo(char* out, size_t tam);   // one line for the console
