// TERMINAL - the device's command session, for the screen (Terminal app) and
// for anyone else who wants it (Berry, SSH tomorrow).
//
// WHAT IT IS. The commands ARE those of the serial console (console.cpp): 'help', 'ls',
// 'cd', 'cat', 'ping', 'wifi', 'ble', 'berry', 'mem', 'tarefas'... a single
// table, with the help alongside. The console gained the idea of a SESSION (the
// current folder and the typist's Ctrl+C - console_sessao_t); this service is the
// screen's session. No duplicated commands: whatever goes into the console appears here, and vice versa.
//
// HOW THE OUTPUT GETS HERE. In ESP-IDF stdout is per task. The task that
// runs the commands swaps its own for a FILE* (funopen) that stores the text as
// lines - the commands keep using plain printf, unaware.
//
// RUNS IN THE BACKGROUND. Each line runs in a BACKGROUND PART of the system (kernel/
// fundo.h): closing the app does not stop the command. Anything over 3 s gets
// the "Terminal" pill (with the command) and the X in the control centre interrupts
// it; short ones do not flash in the status bar. With no command there is no task
// at all: the background part exits when the queue drains and the pool slot goes back to sleep.
//
// MEMORY. The lines (TERM_LINHAS x 128 B, ~51 KB) live in PSRAM and only
// exist after the first opening; they stay alive with the app closed (coming back
// shows what was there). Queue, history and state: PSRAM too. In internal
// RAM, only the lock and the stack borrowed from the pool while a command runs.
//
// WHO CALLS: any task - the screen (Terminal app) and the PC's browser
// (services/tab5_terminal_web) send lines to the SAME session. The command
// runs in a background part that the kernel switches on in the interface
// (sistema_fundo_pedir).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TERM_LINHA_MAX  126   // bytes of ONE stored line; a longer one continues on the next
#define TERM_LINHAS     400   // the ones kept on screen; the oldest goes
#define TERM_ENTRADA    192   // one typed line (the console's limit)

typedef enum {
  TERM_SAIDA = 0,    // what the command wrote
  TERM_COMANDO,      // the typed line, with the prompt
  TERM_AVISO,        // the terminal itself ("^C", "na fila")
} term_tipo_t;

// Prepares the session (the lines in PSRAM and the greeting). false = out of memory.
bool tab5_terminal_abrir(void);

// Runs the line (or queues it, if another command is running). false = queue full.
bool tab5_terminal_enviar(const char* linha);
// Ctrl+C: the running command stops at the next check (console_interrompido).
void tab5_terminal_interromper(void);
bool tab5_terminal_ocupado(void);          // running or with a queue
void tab5_terminal_limpar(void);           // empty screen (the 'clear' command does the same)

// READING, for drawing. versao changes on every new text; i = 0 is the oldest.
uint32_t tab5_terminal_versao(void);
int      tab5_terminal_linhas(void);
// The ABSOLUTE NUMBER of line i = 0 (lines are numbered since boot; the
// oldest leaving the ring and 'clear' make this number go up). A remote
// reader (the browser) asks only for what is past what it already has.
uint32_t tab5_terminal_primeira(void);
// The line with ABSOLUTE number 'abs' (always the same line, even as the ring
// moves; only the last one still grows). -1 = already left the ring or does not exist yet.
int      tab5_terminal_linha_abs(uint32_t abs, char* out, size_t n, term_tipo_t* tipo);
int      tab5_terminal_tamanho(int i);     // bytes of line i (0 if none)
int      tab5_terminal_linha(int i, char* out, size_t n, term_tipo_t* tipo);   // -1 = none
void     tab5_terminal_prompt(char* out, size_t n);   // "/sdcard/Música $ ", or the command's question
// The running command is reading a line (console_ler_linha): whatever is
// typed belongs to it. 'oculto' = password (the screen masks it).
bool     tab5_terminal_lendo(bool* oculto);
// What was typed before: k = 0 is the last. false = there are not that many.
bool     tab5_terminal_historico(int k, char* out, size_t n);
