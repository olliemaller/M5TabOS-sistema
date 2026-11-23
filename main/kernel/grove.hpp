// Grove watcher (kernel/grove.cpp): probes the cable at boot, when the module
// changes and, with nothing conclusive, twice more. Never on the UI task.
#pragma once

void grove_iniciar(void);           // once, from the UI task, before the loop
void grove_pedir_sondagem(void);    // from any task; does not block
bool grove_sondando(void);          // from the request until the probe finishes
