// SYSTEM LOG ON THE SD CARD, ONE FILE PER DAY.
//
// -------------------------------------------------------------------------
// WHY
// -------------------------------------------------------------------------
// Until now the ONLY way to see the log was the serial monitor plugged into the
// computer - and a good share of this system's bugs (GPS, Mesh, the Grove in
// general) only show up away from the desk, "near the window" where the signal is better
// but the USB cable doesn't reach. Without a log, each incident has no evidence: the
// described symptom turns into guessing the cause by reading the code.
//
// -------------------------------------------------------------------------
// PER DAY, NOT PER BOOT, with the reason alongside
// -------------------------------------------------------------------------
// "per boot gets quite complicated, because we're booting all the time" - this system
// restarts all the time during development (each gravar.sh is a new
// boot). One file per boot would become an unreadable folder of files lasting
// seconds. Per DAY, using the RTC date (see tab5_rtc.hpp - its own
// battery, "almost never runs down"), several boots on the same day land in the
// SAME file, one after the other, and a "=== boot ... ===" marker separates
// the sessions without needing a new file for each one.
//
// -------------------------------------------------------------------------
// HOW IT CAPTURES: replaces ESP_LOG's vprintf, doesn't duplicate each call
// -------------------------------------------------------------------------
// tab5_log_iniciar() swaps the function ESP_LOG uses to print
// (esp_log_set_vprintf) for one that does TWO things: calls the
// ORIGINAL function first (the serial console stays identical to today, byte for
// byte) and sends a copy of the SAME line to a buffer in RAM. That way
// any ESP_LOGI/W/E/D from ANY task in the system (UI, GPS,
// Mesh, camera...) is captured automatically - no task needs to
// know this file exists.
//
// -------------------------------------------------------------------------
// WHY A RAM BUFFER BEFORE THE SD CARD
// -------------------------------------------------------------------------
// The function that replaces vprintf runs ON THE STACK OF WHOEVER CALLED THE LOG - it may
// be the UI task, the GPS, the camera, any of them. Writing to the SD card
// right there (mount, open, write, close, unmount - the same ~100ms
// sum that tab5_sd.hpp documents) would tie up ANY task in the system
// for the duration of an SD card write, every time it logged something. It was
// exactly the kind of bug the Grove probe had to leave the UI
// task to avoid repeating (see the big notice in kernel/core.cpp).
//
// Instead, the vprintf swap only FORMATS the line and puts it in a RAM queue
// (xStreamBuffer, which accepts several tasks writing at the same time without
// a manual lock) - an operation of a few microseconds, never blocks. The
// "cartao" task (the system writer, low priority) drains that
// queue from time to time, piggybacking (tab5_sd_carona), and ONLY THEN mounts the
// SD card and writes. If the SD card is
// absent, the lines wait in the buffer until it comes back (or are
// discarded if the buffer fills first - we would rather lose old log lines than
// lock up the system trying to keep every log forever).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Installs the capture and piggybacks on the "cartao" task for flushing. Call ONCE
// at boot, after tab5_sd_iniciar() (it uses tab5_sd_montar/abrir) - see
// kernel/core.cpp.
void tab5_log_iniciar(void);

// Flushes to the SD card within 1 s, as a warning would, without waiting for the
// 60 s batch. For the end of something whose log you want to read soon (the USB drive: the console
// comes back and whoever opens the serial port restarts the Tab5 - what was only in RAM
// would be lost). From any task.
void tab5_log_descarregar_logo(void);

// Status for the 'servicos' command and for anyone who wants to show the log is
// alive: how many bytes are waiting in RAM, how many lines have really been
// written, how many were lost (buffer full) and today's file name
// ("" if the capture hasn't tried to write even once yet).
void tab5_log_estado(size_t* bytes_pendentes, uint32_t* linhas_gravadas,
                      uint32_t* linhas_perdidas, char* arquivo, size_t arquivo_tam);

// THE LAST CRASH stored in flash (core dump): notes in the log the task, the
// PC, the RA and the code addresses on the stack. Boot calls it by itself after
// a panic or watchdog; the console ('travamento') calls it whenever.
// false = nothing stored ('no_console' = says so on the console).
bool tab5_log_travamento(bool no_console);
