// SCREEN ORIENTATION - decides whether the open app is portrait or landscape.
//
// The app declares it (app_t.orientacao, see kernel/app.h); this module applies it. For
// ORI_EM_PE and ORI_DEITADO it is just the right rotation on opening. For
// ORI_SEGUE_IMU it also watches gravity while the app is open and
// rotates between the TWO states that exist - portrait (rotation 0) and landscape
// (rotation 3, ExtPort1 at the bottom). Never the opposite side of either.
//
// IT IS NOT A TASK. The IMU sits on the internal I2C, which only the UI task
// may use (see hal/tab5_imu.hpp); so the watcher is a function the UI
// loop calls on every turn. With no app following the IMU it returns on the
// first line: whoever does not use it pays for no reading at all.
//
// EVERYTHING HERE BELONGS TO THE UI TASK.
#pragma once

#include <stdint.h>
#include "app.h"

// Applies the initial orientation of the app being opened. Call BEFORE the profile
// and any drawing: rotating changes width and height, and everything
// calculated from them needs to see the new value.
void orientacao_ao_abrir(const app_t* app);

// The app closed: back to portrait (home is portrait; landscape with the physical keyboard
// docked) and stops watching the IMU.
void orientacao_ao_fechar(void);

// One turn of the watcher. 'pode_girar' = no finger on the screen, nothing in front
// (lock screen). Sampling continues even when it cannot - only the switch
// waits. Also applies on home: docking or removing the physical keyboard rotates the screen
// for anyone (see s_teclado in the .cpp); the IMU only for ORI_SEGUE_IMU apps.
//
// Returns true when it ROTATED on this call: the caller requests the full
// pass and notifies the app (ao_girar).
bool orientacao_vigiar(uint32_t agora_ms, bool pode_girar);

// How long until the next sample, in ms - so the still UI does not
// sleep past it. UINT32_MAX when no app follows the IMU.
uint32_t orientacao_prazo_ms(uint32_t agora_ms);
