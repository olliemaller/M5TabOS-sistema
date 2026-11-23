// BATTERY GUARD - the system doesn't let the battery die with the device
// switched on.
//
// WHAT THE LOGS SHOWED (energia-2026-10-*.csv, 4 discharges to the end
// in one week - all in the small hours, with nobody watching):
//   - the estimator's % (hal/tab5_power) matches the charge counted in mAh: error
//     of 1 to 3 points, always LOW at the end (shows 0% with 1-2% left);
//   - but nothing happened at 0%: the device carried on until brownout (6.57 V, 3.29
//     V per cell - deep discharge, which wears the battery) and, switched on again,
//     went into a restart loop, each round going down a little more.
//
// WHAT THE SYSTEM DOES, without any app asking:
//   - notifies in the notification centre at 20% (attention), 10% and 5% (urgent);
//   - at 0% (with the voltage confirming) for 30 s in a row, or with the voltage below
//     the floor even if the % disagrees, WRITES what is pending to the SD card and
//     SHUTS DOWN (M5.Power.powerOff) - the cell stays at ~3.5 V, with margin;
//   - the same applies at boot: switching on with an empty battery shuts down again, instead
//     of the restart loop;
//   - on mains (charging), all of this clears.
//
// TASK: tab5_bateria_vigiar() runs IN THE UI TASK, the same one that measures
// (kernel/core.cpp, right after tab5_power_medir()): it is the only one that goes
// to I2C, and shutting down is a pulse on the I/O expander.
#pragma once

#include <stddef.h>

// Once per measurement (1 Hz), awake or idle.
void tab5_bateria_vigiar(void);

// A status line for the console ('energia').
void tab5_bateria_resumo(char* out, size_t tam);
