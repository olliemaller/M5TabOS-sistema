// The Berry task - see berry_vm.hpp.

#include "berry_vm.hpp"
#include "idioma.h"
#include "berry_apps.hpp"
#include "berry_mem.h"
extern "C" {
#include "be_gc.h"   // be_gc_setsteprate (outside the public API)
#include "be_exec.h" // be_throw (likewise): the timeout error that except does not catch
void be_port_liberar_arquivos(int sim);   // components/berry/port/be_port.c
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "berry";

static const uint32_t BERRY_PILHA = 32 * 1024;

static TaskHandle_t      s_tarefa = nullptr;
static SemaphoreHandle_t s_vez    = nullptr;   // one request at a time (console and UI)
static SemaphoreHandle_t s_ir     = nullptr;   // there is work
static SemaphoreHandle_t s_fim    = nullptr;   // finished

static berry_trabalho_t  s_fn  = nullptr;
static void*             s_ctx = nullptr;
static volatile int64_t  s_prazo_us = 0;       // 0 = no limit
static volatile int64_t  s_entregue_us = 0;    // when the request was handed over (measurement)

// THE IDLE PRIORITY. When idle, the task sits right down at (1): the console is
// the main requester, and it is in no hurry. With a request, it rises to the priority
// of WHOEVER ASKED (see berry_rodar) - the UI (4) sits waiting, and
// a Berry app must not be trampled by Wi-Fi, ESP-NOW and the card (2-3)
// while the whole screen waits for it.
static const UBaseType_t PRIO_FOLGA = 1;

static berry_medida_t s_med = {};

// THE TIME HOOK. The machine calls it every 2^13 instructions (8192,
// BE_VM_OBSERVABILITY_SAMPLING = 14 in berry_conf.h); once past the deadline, the error
// rises through the script up to the caller's pcall - no state is left half-done.
// It used to be every ~500 thousand: a script that spends its time in system
// calls (drawing, maths) executes few instructions, and a fundo()'s 200 ms
// deadline became 500 ms each time for almost a minute (Estresse app,
// 2026-11-13). Calls to tela also check (berry_prazo_conferir, in
// so_janela): a single one may take a few ms.
//
// THE ERROR IS NOT A SCRIPT EXCEPTION. With be_raise, a 'try ... except ..'
// inside a loop swallowed tempo_esgotado and the loop carried on - and, with the deadline
// cleared after the first error, forever, with the UI waiting. Berry's except
// does not catch BE_EXIT (be_except_block_resume only handles
// BE_EXCEPTION): it goes straight through every try and only stops at the system's pcall.
// After it the deadline becomes a short grace period, not zero: whatever Berry the system still
// runs in this job (the fechar() of an app with an error) also comes to an end.
static const int64_t FOLGA_DEPOIS_US = 500 * 1000;

void berry_prazo_conferir(bvm* vm)
{
  if (!s_prazo_us) { return; }
  const int64_t agora = esp_timer_get_time();
  if (agora <= s_prazo_us) { return; }
  s_prazo_us = agora + FOLGA_DEPOIS_US;
  // The way be_raise leaves the message: just above the top, where pcall picks it up.
  be_pushstring(vm, "tempo_esgotado: the script ran too long (infinite loop?)");
  be_pop(vm, 1);
  be_throw(vm, BE_EXIT);
}

static void gancho(bvm* vm, int evento, ...)
{
  if (evento == BE_OBS_VM_HEARTBEAT) { berry_prazo_conferir(vm); }
}

static void vTaskBerry(void*)
{
  for (;;) {
    xSemaphoreTake(s_ir, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    const uint32_t espera = (uint32_t)(t0 - s_entregue_us);
    s_med.entregas++;
    s_med.espera_soma_us += espera;
    if (espera > s_med.espera_max_us) { s_med.espera_max_us = espera; }
    s_fn(s_ctx);
    s_prazo_us = 0;
    fflush(stdout);
    xSemaphoreGive(s_fim);
  }
}

static bool subir(void)
{
  if (s_tarefa) { return true; }
  s_vez = xSemaphoreCreateMutex();
  s_ir  = xSemaphoreCreateBinary();
  s_fim = xSemaphoreCreateBinary();
  if (!s_vez || !s_ir || !s_fim) { return false; }
  if (xTaskCreatePinnedToCoreWithCaps(vTaskBerry, "berry", BERRY_PILHA, nullptr, PRIO_FOLGA,
                                      &s_tarefa, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
    s_tarefa = nullptr;
    return false;
  }
  return true;
}

bool berry_rodar(berry_trabalho_t fn, void* ctx, uint32_t prazo_ms)
{
  if (!subir()) { ESP_LOGE(TAG, "the Berry task did not come up"); return false; }
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  // WHOEVER WAITS IN THE QUEUE PUSHES WHOEVER IS RUNNING. With a fundo() (priority
  // 1) in the middle, the UI (4) arriving sits waiting on s_vez - and the
  // background job, on the Berry task, would carry on at 1, behind Wi-Fi and the card.
  // Raise it now: the background finishes at the priority of whoever is waiting for it.
  if (prio > uxTaskPriorityGet(s_tarefa)) { vTaskPrioritySet(s_tarefa, prio); }
  xSemaphoreTake(s_vez, portMAX_DELAY);   // the s_vez queue moves by priority
  s_fn = fn;
  s_ctx = ctx;
  if (prio > uxTaskPriorityGet(s_tarefa)) { vTaskPrioritySet(s_tarefa, prio); }   // inherits: see PRIO_FOLGA
  s_entregue_us = esp_timer_get_time();
  s_prazo_us = prazo_ms ? s_entregue_us + (int64_t)prazo_ms * 1000 : 0;
  xSemaphoreGive(s_ir);
  xSemaphoreTake(s_fim, portMAX_DELAY);   // the deadline is guaranteed by the hook
  vTaskPrioritySet(s_tarefa, PRIO_FOLGA);   // idle, it goes back down (whoever waits raises it again)
  xSemaphoreGive(s_vez);
  return true;
}

bvm* berry_vm_nova(void)
{
  // The modules are built outside pcall: running out of memory there would abort the
  // device. Without the budget headroom, it does not even start (the caller shows the error).
  if (!berry_mem_cabe(BERRY_FOLGA_VM)) {
    ESP_LOGW(TAG, "Berry budget full: %u KB in use", (unsigned)(berry_mem_usada() / 1024));
    return nullptr;
  }
  bvm* vm = be_vm_new();
  be_set_obs_hook(vm, gancho);
  // THE GARBAGE COLLECTOR runs when the script's memory passes a limit, and
  // the next limit is what was left x (rate/100). The default (200) is for
  // a microcontroller with no memory: here there are 32 MB of PSRAM, and each collection is
  // a pause in the middle of a frame. 350 (the maximum) collects ~2.5x less often.
  be_gc_setsteprate(vm, 350);
  berry_modulos_registrar(vm);
  return vm;
}

// The error at the top of the stack: prints, copies and pops.
static void erro_do_topo(bvm* vm, char* erro, size_t erro_tam)
{
  const char* msg = be_tostring(vm, -1);
  printf("erro: %s\n", msg);
  if (erro && erro_tam) { snprintf(erro, erro_tam, "%s", msg); }
  be_pop(vm, 1);
}

bool berry_executar(bvm* vm, const char* codigo, bool arquivo, bool mostrar,
                    char* erro, size_t erro_tam)
{
  int res;
  if (arquivo) {
    // The only moment Berry opens a file (components/berry/port/
    // be_port.c): the system loading the .be. The running script does not open files.
    be_port_liberar_arquivos(1);
    res = be_loadfile(vm, codigo);
    be_port_liberar_arquivos(0);
  } else {
    // As in the REPL: tries it as an expression (to show the value); otherwise, a statement.
    res = BE_SYNTAX_ERROR;
    if (mostrar) {
      char expr[224];
      snprintf(expr, sizeof(expr), "return (%s)", codigo);
      res = be_loadstring(vm, expr);
      if (res != BE_OK) { be_pop(vm, 1); }
    }
    if (res != BE_OK) { res = be_loadstring(vm, codigo); }
  }
  if (res == BE_OK) { res = be_pcall(vm, 0); }
  const bool ok = (res == BE_OK);
  if (ok && mostrar && be_top(vm) > 0 && !be_isnil(vm, -1)) { printf("%s\n", be_tostring(vm, -1)); }
  if (!ok) { erro_do_topo(vm, erro, erro_tam); }
  be_pop(vm, be_top(vm));
  return ok;
}

typedef struct { const char* cod; size_t n; char* erro; size_t tam; int linha; bool ok; } verificar_t;

static void verificar_na_tarefa(void* ctx)
{
  verificar_t* v = (verificar_t*)ctx;
  bvm* vm = berry_vm_nova();
  if (!vm) { snprintf(v->erro, v->tam, tr("sem memoria para o Berry")); return; }
  if (be_loadbuffer(vm, "script", v->cod, v->n) == BE_OK) {
    v->ok = true;
  } else {
    // "script:12: unexpected symbol near 'end'" - the line comes after the name.
    // The line becomes a number, and the message keeps only what went wrong.
    const char* m = be_tostring(vm, -1);
    const char* p = m ? strchr(m, ':') : nullptr;
    const char* q = p ? strchr(p + 1, ':') : nullptr;
    if (p) { v->linha = atoi(p + 1); }
    const char* msg = (q && v->linha > 0) ? q + 1 : (m ? m : "erro");
    while (*msg == ' ') { ++msg; }
    snprintf(v->erro, v->tam, "%s", msg);
  }
  be_vm_delete(vm);
}

bool berry_verificar(const char* codigo, size_t n, char* erro, size_t erro_tam, int* linha)
{
  verificar_t v = { codigo, n, erro, erro_tam, 0, false };
  if (erro && erro_tam) { erro[0] = 0; }
  if (!berry_rodar(verificar_na_tarefa, &v, 5000)) {
    snprintf(erro, erro_tam, tr("a tarefa do Berry nao respondeu"));
    return false;
  }
  if (linha) { *linha = v.linha; }
  return v.ok;
}

bool berry_funcao(bvm* vm, const char* nome)
{
  if (be_getglobal(vm, nome) && be_isfunction(vm, -1)) { return true; }
  be_pop(vm, 1);
  return false;
}

bool berry_chamar(bvm* vm, int argc, char* erro, size_t erro_tam)
{
  const bool ok = be_pcall(vm, argc) == BE_OK;
  if (!ok) { erro_do_topo(vm, erro, erro_tam); }
  be_pop(vm, be_top(vm));
  return ok;
}

uint32_t berry_pilha_livre(void)
{
  return s_tarefa ? (uint32_t)uxTaskGetStackHighWaterMark(s_tarefa) : 0;
}

void berry_medida(berry_medida_t* m, bool zerar)
{
  if (m) { *m = s_med; }
  if (zerar) { s_med = berry_medida_t{}; }
}
