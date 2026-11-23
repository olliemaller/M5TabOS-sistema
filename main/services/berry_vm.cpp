// A tarefa do Berry - ver berry_vm.hpp.

#include "berry_vm.hpp"
#include "idioma.h"
#include "berry_apps.hpp"
#include "berry_mem.h"
extern "C" {
#include "be_gc.h"   // be_gc_setsteprate (fora da API publica)
#include "be_exec.h" // be_throw (idem): o erro de tempo que o except nao pega
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
static SemaphoreHandle_t s_vez    = nullptr;   // um pedido por vez (console e interface)
static SemaphoreHandle_t s_ir     = nullptr;   // ha trabalho
static SemaphoreHandle_t s_fim    = nullptr;   // terminou

static berry_trabalho_t  s_fn  = nullptr;
static void*             s_ctx = nullptr;
static volatile int64_t  s_prazo_us = 0;       // 0 = sem limite
static volatile int64_t  s_entregue_us = 0;    // quando o pedido foi entregue (medida)

// A PRIORIDADE DE FOLGA. Parada, a tarefa fica la embaixo (1): o console e
// quem mais pede, e nao tem pressa. Com um pedido, ela sobe para a prioridade
// de QUEM PEDIU (ver berry_rodar) - a interface (4) fica parada esperando, e
// um app Berry nao pode ser atropelado pelo Wi-Fi, ESP-NOW e cartao (2-3)
// enquanto a tela inteira espera por ele.
static const UBaseType_t PRIO_FOLGA = 1;

static berry_medida_t s_med = {};

// O GANCHO DE TEMPO. A maquina chama a cada 2^13 instrucoes (8192,
// BE_VM_OBSERVABILITY_SAMPLING = 14 no berry_conf.h); passou do prazo, o erro
// sobe pelo script ate o pcall de quem chamou - nenhum estado fica pela metade.
// Antes era a cada ~500 mil: um script que passa o tempo em chamadas ao
// sistema (desenho, contas) faz poucas instrucoes, e o prazo de 200 ms de um
// fundo() virava 500 ms de cada vez por quase um minuto (app Estresse,
// 2026-11-13). As chamadas a tela tambem conferem (berry_prazo_conferir, em
// so_janela): uma so pode levar alguns ms.
//
// O ERRO NAO E UMA EXCECAO DO SCRIPT. Com be_raise, um 'try ... except ..'
// dentro de um laco engolia o tempo_esgotado e o laco seguia - e, com o prazo
// zerado depois do primeiro erro, para sempre, com a interface esperando. O
// BE_EXIT o except do Berry nao pega (be_except_block_resume so trata
// BE_EXCEPTION): ele atravessa todo try e so para no pcall do sistema.
// Depois dele o prazo vira uma folga curta, nao zero: o que o sistema ainda
// rodar de Berry neste trabalho (o fechar() de um app com erro) tambem tem fim.
static const int64_t FOLGA_DEPOIS_US = 500 * 1000;

void berry_prazo_conferir(bvm* vm)
{
  if (!s_prazo_us) { return; }
  const int64_t agora = esp_timer_get_time();
  if (agora <= s_prazo_us) { return; }
  s_prazo_us = agora + FOLGA_DEPOIS_US;
  // Como o be_raise deixa a mensagem: logo acima do topo, onde o pcall a pega.
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
  if (!subir()) { ESP_LOGE(TAG, "a tarefa do Berry nao subiu"); return false; }
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  // QUEM ESPERA NA FILA EMPURRA QUEM ESTA RODANDO. Com um fundo() (prioridade
  // 1) no meio, a interface (4) que chega fica parada em s_vez - e o trabalho
  // do fundo, na tarefa do Berry, seguiria na 1, atras do Wi-Fi e do cartao.
  // Sobe ja: o fundo termina na prioridade de quem espera por ele.
  if (prio > uxTaskPriorityGet(s_tarefa)) { vTaskPrioritySet(s_tarefa, prio); }
  xSemaphoreTake(s_vez, portMAX_DELAY);   // a fila de s_vez anda por prioridade
  s_fn = fn;
  s_ctx = ctx;
  if (prio > uxTaskPriorityGet(s_tarefa)) { vTaskPrioritySet(s_tarefa, prio); }   // herda: ver PRIO_FOLGA
  s_entregue_us = esp_timer_get_time();
  s_prazo_us = prazo_ms ? s_entregue_us + (int64_t)prazo_ms * 1000 : 0;
  xSemaphoreGive(s_ir);
  xSemaphoreTake(s_fim, portMAX_DELAY);   // o prazo e garantido pelo gancho
  vTaskPrioritySet(s_tarefa, PRIO_FOLGA);   // parada, volta para baixo (quem esperar sobe de novo)
  xSemaphoreGive(s_vez);
  return true;
}

bvm* berry_vm_nova(void)
{
  // Os modulos sao montados fora de pcall: faltar memoria ali abortaria o
  // aparelho. Sem a folga no orcamento, nem comeca (quem chama mostra o erro).
  if (!berry_mem_cabe(BERRY_FOLGA_VM)) {
    ESP_LOGW(TAG, "orcamento do Berry cheio: %u KB em uso", (unsigned)(berry_mem_usada() / 1024));
    return nullptr;
  }
  bvm* vm = be_vm_new();
  be_set_obs_hook(vm, gancho);
  // O COLETOR DE LIXO roda quando a memoria do script passa de um limite, e
  // o limite seguinte e o que sobrou x (taxa/100). O padrao (200) e para
  // microcontrolador sem memoria: aqui ha 32 MB de PSRAM, e cada coleta e
  // uma pausa no meio de um quadro. 350 (o maximo) coleta ~2,5x menos vezes.
  be_gc_setsteprate(vm, 350);
  berry_modulos_registrar(vm);
  return vm;
}

// O erro do topo da pilha: imprime, copia e desempilha.
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
    // O unico momento em que o Berry abre arquivo (components/berry/port/
    // be_port.c): o sistema carregando o .be. O script, rodando, nao abre.
    be_port_liberar_arquivos(1);
    res = be_loadfile(vm, codigo);
    be_port_liberar_arquivos(0);
  } else {
    // Como no REPL: tenta como expressao (para mostrar o valor); senao, comando.
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
    // "script:12: unexpected symbol near 'end'" - a linha vem depois do nome.
    // A linha vira numero, e a mensagem fica so com o que deu errado.
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
