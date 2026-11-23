// Serial console - a handful of commands typed through the monitor.
//
// It was born of a concrete need: changing the WiFi network meant turning the
// device off, taking out the SD card, taking it to the PC, editing, and putting it all back.
// For one line of text. Now it is a command.
//
// It works inside `idf.py monitor` itself - it forwards what you type
// to the serial port. Type "help" to see the commands.
//
// It does not replace the SD card on the PC for large files; it is for one-off adjustments.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Creates the console task. Call after tab5_sd_iniciar().
void console_iniciar(void);

// COMMAND SESSION - who is typing: the serial port (the console's own) or the
// Terminal on screen (services/tab5_terminal). Each has its own current folder
// and its own way of interrupting. The command runs on the caller's task and
// writes to THAT task's stdout (in ESP-IDF stdout is per task): the
// Terminal swaps its own for a FILE* that stores the lines.
typedef struct {
  char cwd[160];                 // current folder; "/sdcard" at the start, "/" = the disks
  bool (*interrompido)(void);    // Ctrl+C / stop requested; nullptr = never
  // One line from whoever is typing (see console_ler_linha). nullptr = does not know how.
  int  (*ler_linha)(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms);
  // There is a REAL TERMINAL in front (the Terminal app, the /terminal/ page):
  // 'ssh user@host' opens the shared SSH session (services/tab5_ssh) and
  // returns at once - the screen becomes the remote terminal. Without it (the serial port), ssh
  // is the line-by-line shell.
  bool interativo;
} console_sessao_t;

// Runs ONE line as the serial console would ('help', 'ls', 'ping'...), on the
// caller's task. The serial port and the Terminal run at the same time, each
// with its own session (found by the task). The SAME command on both at the same
// instant shares its own state (ping counts in a single variable):
// rare, and the worst case is a wrong number on screen. 'linha' is copied.
void console_executar(console_sessao_t* sessao, const char* linha);

// A long command (ping, large listing, test loop) asks this on each
// iteration: true = the session asked to stop.
bool console_interrompido(void);

// A COMMAND THAT ASKS (the ssh password, the remote shell's lines): one line
// from whoever is typing in this session. 'pergunta' appears first (on the serial port) or on the
// command line (in the Terminal); 'oculto' = password (no echo, masked).
// Waits up to espera_ms: 1 = the line came; 0 = not yet (call again - what
// was typed so far is kept); -1 = interrupted or the session does not
// know how to ask.
int console_ler_linha(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms);
