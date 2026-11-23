// THE SYSTEM WORKER - slow work off the UI task.
//
// The UI task draws and handles touch. Anything that may take a while -
// reading the SD card, scanning a file, building a big list - has to leave
// it, otherwise the screen freezes while the card responds (that was the case with Mesh,
// which reread the history from SD IN THE UI on every new message).
//
// Since only one app is in the foreground at a time, ONE system worker
// is enough for all of them: the app asks, the worker runs it on its own task
// (priority 2, below the UI and the radio services), and when it
// finishes the app receives AVISO_TRABALHO (declare it in app_t.avisos) and picks up the
// result on the UI task.
//
// THE RESULT PATTERN: the job writes into a buffer the UI is NOT
// reading, and only in ao_aviso does the UI switch to it. Never write into
// what the UI is drawing.
//
// CLOSING THE APP IS SAFE: the system waits for the job in progress to finish and
// discards those still in the queue BEFORE calling ao_fechar() - so the
// app can free its buffers there without worry. That is why a job should last
// a few hundred ms at most (one read, not an endless loop) - the
// UI stands still while closing waits. Anything that takes longer is a
// DETACHED job (below); a permanent loop is its own task (tarefa_app).

#pragma once

#include <stdbool.h>

typedef void (*trabalho_t)(void* arg);

// Call once at boot, from the UI task.
void trabalho_iniciar(void);

// Requests a job for the current app. Does not block. false = queue full.
bool sistema_trabalho(trabalho_t fn, void* arg);

// THE APP HAS CLOSED - stop. For a (non-detached) job that may take a while: scanning a
// large file, for example. Closing the app WAITS for the job in progress to
// finish (that is what lets ao_fechar() free buffers without worry), and the
// UI stands still during that wait - a loop that asks this on every block
// gives the UI back at once, instead of at the end of the file. Bailing out midway is
// safe: the result will not be used any more (the app has closed).
bool sistema_trabalho_cancelado(void);

// FOR THE SYSTEM: discards the queue and waits for the job in progress to finish.
// Called by fechar_app() before ao_fechar().
void trabalho_encerrar_do_app(void);

// DETACHED JOB (TRABALHO SOLTO) - for what takes SECONDS and must not hold up closing
// the app (writing a 1.8 MB photo to the SD card: closing the Camera
// waited for the whole write and the UI froze until it was done).
//
// The difference: the system does NOT wait for a detached job when closing the app, and it
// runs even if the app has already closed. The price is the CONTRACT:
//   - 'arg' belongs to the job: memory allocated just for it, which it frees itself
//     at the end. No app buffers (the app may already have freed them).
//   - the result comes back via AVISO_TRABALHO only if the SAME app is still
//     open; the job may leave a result in an app static (the
//     static always exists - the app is compiled into the firmware).
bool sistema_trabalho_solto(trabalho_t fn, void* arg);
