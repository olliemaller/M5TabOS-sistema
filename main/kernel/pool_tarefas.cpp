#include "pool_tarefas.h"

#include <atomic>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   // pxTaskGetStackStart
#include "esp_log.h"

static const char* TAG = "pool";

// SLOT CLASSES. The real measured consumption (lowest free stack in actual use)
// rules here: serial services and apps fit in 4 KB; Mesh asks for 6 KB, and
// with the C6L on Grove it holds slot 0 all the time. That is why the LAST slot
// is also 6 KB: the Terminal command (the console's ones) runs in it. Measured
// on 27/10: of the five 4 KB ones, only one was really used.
// The total is ~28 KB and replaces what these same uses asked of the heap,
// in the worst case, as separate blocks.
#define VAGA_G  6144
#define VAGA_M  4096

#define PILHA_ESTATICA(nome, bytes) alignas(16) static StackType_t nome[(bytes) / sizeof(StackType_t)]
PILHA_ESTATICA(s_pilha0, VAGA_G);
PILHA_ESTATICA(s_pilha1, VAGA_M);
PILHA_ESTATICA(s_pilha2, VAGA_M);
PILHA_ESTATICA(s_pilha3, VAGA_M);
PILHA_ESTATICA(s_pilha4, VAGA_M);
PILHA_ESTATICA(s_pilha5, VAGA_G);

#define POOL_N 6

enum { LIVRE = 0, OCUPADA = 1 };

typedef struct {
  StaticTask_t      tcb;
  StaticSemaphore_t sem_buf;
  StackType_t*      pilha;
  uint32_t          bytes;
  TaskHandle_t      h;
  SemaphoreHandle_t acordar;
  std::atomic<int>  estado;
  volatile uint32_t geracao;
  pool_corpo_t      fn;
  void*             arg;
  char              nome[16];
} vaga_t;

static vaga_t s_v[POOL_N];
static bool   s_pronto = false;
static std::atomic<uint32_t> s_dinamicas{0};

// Lowest free stack by run name.
#define PILHAS_N 24
static pool_pilha_t s_pilhas[PILHAS_N];
static int          s_pilhas_n = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void anotar_pilha(const char* nome, uint32_t livre)
{
  taskENTER_CRITICAL(&s_mux);
  int k = 0;
  while (k < s_pilhas_n && strncmp(s_pilhas[k].nome, nome, sizeof(s_pilhas[k].nome)) != 0) { ++k; }
  if (k < PILHAS_N) {
    if (k == s_pilhas_n) {
      snprintf(s_pilhas[k].nome, sizeof(s_pilhas[k].nome), "%s", nome);
      s_pilhas[k].livre_min = livre;
      ++s_pilhas_n;
    } else if (livre < s_pilhas[k].livre_min) {
      s_pilhas[k].livre_min = livre;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
}

// Repaints the FREE part of the stack with the 0xA5 pattern, so that the high water mark
// of the next run measures only its own.
//
// WHERE THE STACK STARTS: it is NOT the start of the buffer (v->pilha). On the P4 the IDF keeps
// at the foot of the buffer the task's coprocessor context area (FPU and PIE),
// and only after it does the stack FreeRTOS knows about (pxStack) start. In the
// first 32 bytes of pxStack, aligned to 32, sits the end-of-stack watchpoint
// (FREERTOS_WATCHPOINT_END_OF_STACK): any write there brings the system down
// with "Breakpoint" - on purpose, it is what catches a stack overflow immediately.
// Repainting from "buffer + 128" erased the FPU/PIE context and hit the
// watchpoint (Guru Meditation when closing the Camera). That is why the start comes from
// FreeRTOS itself, and the watchpoint and canary are skipped with room to spare.
static void __attribute__((noinline)) repintar(void)
{
  const uintptr_t ini_pilha = (uintptr_t)pxTaskGetStackStart(nullptr);
  uint8_t* base = (uint8_t*)(((ini_pilha + 31u) & ~(uintptr_t)31u) + 64u);   // watchpoint (32 B) + canary, with room to spare
  uint8_t* topo = (uint8_t*)__builtin_frame_address(0) - 512;
  if (topo > base) { memset(base, 0xA5, (size_t)(topo - base)); }
}

static void vTaskVaga(void* a)
{
  vaga_t* v = (vaga_t*)a;
  for (;;) {
    // xTaskAbortDelay may wake here with no work: go back to sleep.
    if (xSemaphoreTake(v->acordar, portMAX_DELAY) != pdTRUE) { continue; }
    pool_corpo_t fn = v->fn;
    if (!fn) { continue; }
    ulTaskNotifyTake(pdTRUE, 0);   // notice left over from the previous run

    fn(v->arg);

    tab5_dono_esquecer(v->h);
    anotar_pilha(v->nome, (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
    ulTaskNotifyTake(pdTRUE, 0);
    repintar();
    v->fn = nullptr;
    v->estado.store(LIVRE, std::memory_order_release);
  }
}

void pool_iniciar(void)
{
  if (s_pronto) { return; }
  StackType_t* pilhas[POOL_N] = { s_pilha0, s_pilha1, s_pilha2, s_pilha3, s_pilha4, s_pilha5 };
  const uint32_t tam[POOL_N]  = { VAGA_G, VAGA_M, VAGA_M, VAGA_M, VAGA_M, VAGA_G };
  uint32_t fixos = 0;
  for (int i = 0; i < POOL_N; ++i) {
    vaga_t* v = &s_v[i];
    v->pilha = pilhas[i];
    v->bytes = tam[i];
    v->estado.store(LIVRE);
    v->acordar = xSemaphoreCreateBinaryStatic(&v->sem_buf);
    char nome[8];
    snprintf(nome, sizeof(nome), "pool%d", i);
    v->h = xTaskCreateStaticPinnedToCore(vTaskVaga, nome, v->bytes, v, 1,
                                         v->pilha, &v->tcb, tskNO_AFFINITY);
    fixos += v->bytes + (uint32_t)sizeof(StaticTask_t) + (uint32_t)sizeof(StaticSemaphore_t);
  }
  s_pronto = true;
  ESP_LOGI(TAG, "%d fixed tasks, %lu B reserved (static stacks and TCBs)",
           POOL_N, (unsigned long)fixos);
}

// Run in an ordinary task, when there is no slot.
typedef struct { pool_corpo_t fn; void* arg; } dinamica_t;
static void vTaskDinamica(void* a)
{
  dinamica_t d = *(dinamica_t*)a;
  free(a);
  d.fn(d.arg);
  tab5_dono_esquecer(xTaskGetCurrentTaskHandle());
  vTaskDelete(nullptr);
}

bool pool_criar(pool_id_t* id, pool_corpo_t fn, void* arg, const char* nome,
                uint32_t pilha, UBaseType_t prio, dono_tipo_t tipo, const char* dono)
{
  if (id) { *id = (pool_id_t)POOL_ID_VAZIO; }
  if (!fn) { return false; }
  if (!s_pronto) { pool_iniciar(); }

  // The smallest free slot that fits: Mesh does not take the 6 KB one when it fits in 4.
  // The order is 1..N-1 and then 0: the 4 KB ones first, and the 6 KB ones (the
  // last and 0) only when the smaller ones have run out or do not fit.
  vaga_t* esc = nullptr;
  for (int k = 1; k <= POOL_N && !esc; ++k) {
    vaga_t* v = &s_v[k % POOL_N];
    if (v->bytes < pilha) { continue; }
    int esperado = LIVRE;
    if (v->estado.compare_exchange_strong(esperado, OCUPADA, std::memory_order_acq_rel)) { esc = v; }
  }

  if (esc) {
    snprintf(esc->nome, sizeof(esc->nome), "%s", nome ? nome : "?");
    esc->fn  = fn;
    esc->arg = arg;
    esc->geracao = esc->geracao + 1;
    if (dono) { tab5_dono_registrar(esc->h, tipo, dono); }
    if (id) { id->idx = (int8_t)(esc - s_v); id->geracao = esc->geracao; id->handle = esc->h; }
    vTaskPrioritySet(esc->h, prio);
    xSemaphoreGive(esc->acordar);
    return true;
  }

  s_dinamicas.fetch_add(1);
  // A request larger than the largest slot (e.g. the Terminal, 8 KB) is the requester's
  // choice, not a lack of slots: only warn when the slots that would fit have run out.
  if (pilha <= VAGA_G) {
    ESP_LOGW(TAG, "no slot for '%s' (%lu B) - ordinary task", nome ? nome : "?", (unsigned long)pilha);
  }
  dinamica_t* d = (dinamica_t*)malloc(sizeof(dinamica_t));
  if (!d) { return false; }
  d->fn = fn; d->arg = arg;
  TaskHandle_t h = nullptr;
  if (xTaskCreatePinnedToCore(vTaskDinamica, nome ? nome : "tarefa", pilha, d, prio, &h,
                              tskNO_AFFINITY) != pdPASS) {
    free(d);
    return false;
  }
  if (dono) { tab5_dono_registrar(h, tipo, dono); }
  if (id) { id->handle = h; }
  return true;
}

void pool_acordar(const pool_id_t* id)
{
  if (!id) { return; }
  if (id->idx >= 0 && id->idx < POOL_N) {
    vaga_t* v = &s_v[id->idx];
    if (v->geracao == id->geracao && v->estado.load(std::memory_order_acquire) == OCUPADA) {
      xTaskAbortDelay(v->h);
    }
  } else if (id->handle) {
    xTaskAbortDelay(id->handle);
  }
}

void pool_estado(pool_estado_t* out)
{
  memset(out, 0, sizeof(*out));
  out->vagas = POOL_N;
  for (int i = 0; i < POOL_N; ++i) {
    if (s_v[i].estado.load() == OCUPADA) { ++out->ocupadas; }
    out->bytes_fixos += s_v[i].bytes + (uint32_t)sizeof(StaticTask_t) + (uint32_t)sizeof(StaticSemaphore_t);
  }
  out->dinamicas = s_dinamicas.load();
}

int pool_pilhas(pool_pilha_t* out, int max)
{
  taskENTER_CRITICAL(&s_mux);
  const int n = s_pilhas_n < max ? s_pilhas_n : max;
  memcpy(out, s_pilhas, sizeof(pool_pilha_t) * n);
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

void pool_pilhas_zerar(void)
{
  taskENTER_CRITICAL(&s_mux);
  s_pilhas_n = 0;
  taskEXIT_CRITICAL(&s_mux);
}
