// THE BERRY TASK - where ALL Berry code runs (console and apps).
//
// ONE task, with a 32 KB stack IN PSRAM. Berry's compiler and machine
// are recursive and pcall keeps a jmp_buf: the console's 8 KB (and the
// UI's) are not enough - calling a function defined earlier stopped the device.
// The stack in PSRAM does not use internal RAM; the price is that this task cannot
// write to flash (NVS): the cache is disabled during the write.
//
// THE MODEL: whoever needs to run Berry hands over a job and WAITS for it to
// finish (berry_rodar). Requests are queued one at a time, so
// a machine (bvm) is never used by two tasks at the same time. The
// UI also asks this way - and, while it waits, it does not draw: the drawing
// the script does via the Berry task competes with nobody.
//
// PRIORITY: idle, the task sits at 1; during a request it INHERITS that of
// whoever asked (the UI is 4) - whoever waits for it does not end up behind everything else.
// Whoever arrives with a higher priority while another job is running (the UI
// waiting for a fundo() to finish) also pushes: the job in progress finishes
// at the priority of whoever is waiting.
//
// TIME LIMIT: each job has a deadline. Past it, the machine stops the
// script with the error "tempo_esgotado" (the hook is called every ~1 million
// instructions) and the caller's pcall receives the error. The script CANNOT
// catch that error with try/except. An infinite loop in an app does not hang the screen.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern "C" {
#include "berry.h"
}

typedef void (*berry_trabalho_t)(void* ctx);

// Runs fn(ctx) on the Berry task and waits for it to finish. 'prazo_ms' = maximum
// time of Berry running inside it (0 = no limit). false = the task did not
// come up. From any task except Berry's own.
bool berry_rodar(berry_trabalho_t fn, void* ctx, uint32_t prazo_ms);

// A new machine, already with the time hook and the system modules (tela,
// sistema). Only from inside a job. nullptr = Berry's memory budget
// (berry_mem.h) is full.
bvm* berry_vm_nova(void);

// Past the deadline of the job in progress (berry_rodar): ends the script with
// tempo_esgotado (does not return). Called by the machine's hook and by the system
// functions that may take a while (tela, see so_janela in berry_interno.hpp).
void berry_prazo_conferir(bvm* vm);

// Compiles and runs 'codigo' (or the file, if 'arquivo') in vm. With 'mostrar',
// prints the value of an expression. Error: printed and returned in 'erro'.
// Only from inside a job.
bool berry_executar(bvm* vm, const char* codigo, bool arquivo, bool mostrar,
                    char* erro, size_t erro_tam);

// CALLING A SCRIPT FUNCTION, in two steps (Berry wants the function on the
// stack before the arguments):
//   if (berry_funcao(vm, "tocar")) {          // pushed the function
//     be_pushint(vm, x); be_pushint(vm, y);   // the arguments
//     berry_chamar(vm, 2, erro, sizeof erro);
//   }
// berry_funcao: false = the script did not define it (nothing pushed).
// berry_chamar: false = there was an error (printed and copied into 'erro'). Pops everything.
bool berry_funcao(bvm* vm, const char* nome);

// CHECK WITHOUT RUNNING (the Editor): compiles 'codigo' in a new machine, just to
// find syntax errors, and deletes the machine. false = there is an error: the message in
// 'erro' and the line (1 = the first) in 'linha' (0 = not given). Blocks until
// the Berry task replies: outside the UI.
bool berry_verificar(const char* codigo, size_t n, char* erro, size_t erro_tam, int* linha);
bool berry_chamar(bvm* vm, int argc, char* erro, size_t erro_tam);

// Free stack of the task, in bytes (diagnostics).
uint32_t berry_pilha_livre(void);

// HAND-OVER MEASUREMENT: from the request until the Berry task starts running (task
// switch). It should stay in the tens of us; if it rises, something is ahead of it.
typedef struct {
  uint32_t entregas;
  uint64_t espera_soma_us;
  uint32_t espera_max_us;
} berry_medida_t;
void berry_medida(berry_medida_t* m, bool zerar);
