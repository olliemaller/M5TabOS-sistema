// FRAME PACE - the clock for whoever animates, locked to the panel scan.
//
// THE PROBLEM. A periodic app was redrawn every periodo_ms of "wall clock",
// and the panel swaps pages at ITS OWN pace (15.7 to 20.6 ms, depending on the
// panel clock - Ajustes > Tela). The two do not match: a 40 ms app stayed
// on screen for 2 scans almost always and, every so often, 3 - a periodic
// jolt in an animation that was otherwise smooth. It was seen on the lock
// screen, but it applied to every animated app (Radar, Cobra, IMU...).
//
// THE RULE. The app's period becomes a whole number of scans (the closest
// to what it asked for, never less than one), and the frame starts right after
// a scan: it has almost a whole period to get ready and goes on
// air always at the same phase. No app changes a line to gain this.
//
// AND MOTION. Whoever animates by time can ask for sistema_quadro_dt() (app.h):
// the distance between the scans in which this frame and the previous one GO ON AIR -
// not between the instants at which they were drawn. That way what appears has moved
// exactly the time it was on screen, even if the drawing ran a little late.
//
// Only the UI task calls this.

#pragma once

#include <stdint.h>

// Called at the START of every drawing pass (desenhar_conteudo), whatever
// the reason. 'dono' identifies who is drawing (the app, home, the lock screen):
// if the owner changes, the clock restarts. 'periodo_ms' = the current period (0 =
// not periodic: nothing is scheduled).
void ritmo_quadro(const void* dono, uint32_t periodo_ms);

// How long, in ms, until the next periodic frame (0 = already overdue).
uint32_t ritmo_falta_ms(void);

// dt (s) of the frame in progress - see the top. Always > 0; whoever animates limits it
// however suits them (an app that was still for 5 s receives 5 s).
float ritmo_dt(void);
