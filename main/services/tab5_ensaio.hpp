// POWER CONSUMPTION TEST - how much each service costs with the screen off.
//
// Born from the battery analysis of 2026-10-16: when idle the device drew
// 75-100 mA (17 h from full to empty), with plateaus of ~46, ~68, ~76, ~97, ~112
// and ~140 mA depending on what was on. It can't be measured over serial: the
// USB cable charges the battery and the INA226 only sees the difference. The test runs
// on its own, ON BATTERY, and writes the result to the SD card.
//
// HOW TO USE: 'energia ensaio' in the console, unplug the cable, let the screen go off and
// wait ~45 min without touching it. When the cable is plugged in again, 'energia ensaio' shows
// the table (also in Sistema/ensaio_energia.csv and in the log).
//
// THE STAGES ARE CUMULATIVE - each one switches off one more service, and the difference
// from the previous one is its cost:
//   0 baseline (as is)   1 without Mesh over the internet   2 without the Grove 5V
//   3 without Bluetooth  4 without Wi-Fi (the C6 stays powered)
// Each stage: 2 min of settling + N min measuring, counting ONLY the seconds
// idle and discharging (waking the screen or plugging in the cable pauses the count).
// At the end (or on 'energia ensaio parar') everything goes back as it was.
//
// The settings changed go to NVS (it is the path the services obey).
// If the device shuts down midway, the original values are in the log ("ensaio:
// originals ...") for restoring by hand.
#pragma once

#include <stddef.h>
#include <stdbool.h>

bool tab5_ensaio_comecar(int min_por_fase, int fases = 5);   // false = one is already running; fases 1 = baseline only
void tab5_ensaio_parar(void);                 // restores the settings (on the next UI round)
void tab5_ensaio_imprimir(void);              // status + table, on stdout (console)

// 1 Hz, in the UI task (touches the I2C expander), right after the measurement.
void tab5_ensaio_vigiar(void);
