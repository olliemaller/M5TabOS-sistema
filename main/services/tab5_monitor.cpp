#include "tab5_monitor.hpp"
#include "tab5_donos.hpp"
#include "tab5_marcos.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM

#include <string.h>
#include "pool_tarefas.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_memory_utils.h"             // esp_ptr_external_ram
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "tab5_sd.hpp"
#include "tab5_hora.hpp"
#include "tab5_quadro.hpp"
#include "compositor.hpp"   // panel underrun counter
#include "app.h"            // sistema_app_atual

static const char* TAG = "monitor";
static const char* PASTA = "Tarefas";

// Compositor snapshot at the last recorded minute (quadros-*.csv).
static comp_estat_t s_qd_ant = {};
static uint32_t     s_qd_amostras = 0;

// ---------------------------------------------------------------------------
// THE TRIGGERS - small and cheap, all EDGE-triggered
// ---------------------------------------------------------------------------
// Each fires ONCE on crossing the limit and only re-arms after coming back with
// margin (hysteresis) - memory oscillating around the limit does not turn into a
// flood of events.
static const uint32_t RAM_BAIXA      = 48 * 1024;    // free internal RAM below this
static const uint32_t RAM_FOLGA      = 16 * 1024;
static const uint32_t BLOCO_BAIXO    = 16 * 1024;    // largest internal block below this (fragmentation)
static const uint32_t BLOCO_FOLGA    =  8 * 1024;
static const uint32_t PSRAM_BAIXA    = 2 * 1024 * 1024;
static const uint32_t PSRAM_FOLGA    = 512 * 1024;
static const uint32_t NOVO_MIN_PASSO =  4 * 1024;    // new low-RAM record, in 4 KB steps
static const uint32_t QUEDA_BRUSCA   = 32 * 1024;    // internal RAM dropped this much in 1 s
static const uint32_t PILHA_BAIXA    = 512;          // bytes left on a task's stack
static const float    CPU_TAREFA_ALTA = 60.0f, CPU_TAREFA_OK = 40.0f;
static const int      CPU_TAREFA_SEG  = 3;           // consecutive seconds above
static const float    CPU_TOTAL_ALTA  = 90.0f, CPU_TOTAL_OK = 70.0f;
static const int      CPU_TOTAL_SEG   = 5;
static const uint32_t ACOMODAR_S      = 20;          // boot allocates all sorts: records only after this

// The kernel's own short-lived tasks: being born and vanishing is their job,
// not news (one per app opening).
static bool efemera(const char* n)
{
  return strcmp(n, "sd_monta") == 0 || strcmp(n, "sd_desmonta") == 0;
}

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_mutex = nullptr;
EXT_RAM_BSS_ATTR static mon_foto_t s_foto;
static bool              s_tem_foto = false;

// In PSRAM (3.6 KB): read 1x/s, only by the Tasks app - internal RAM is for stacks and DMA.
EXT_RAM_BSS_ATTR static float    s_h_cpu[MON_HIST_N];
EXT_RAM_BSS_ATTR static uint32_t s_h_ram[MON_HIST_N], s_h_psram[MON_HIST_N];
static int      s_h_n = 0, s_h_i = 0;

EXT_RAM_BSS_ATTR static mon_evento_t s_ev[MON_EVENTOS_N];
static int          s_ev_n = 0, s_ev_i = 0;
static uint32_t     s_ev_total = 0;

// Lines waiting for the card (written once per round, not on every event).
#define PENDENTES_N 16
EXT_RAM_BSS_ATTR static char s_pend[PENDENTES_N][112];   // likewise (1.8 KB)
static int  s_n_pend = 0;

// Memory of the sampling task (only it touches this).
typedef struct { UBaseType_t numero; uint64_t tempo; int seg_alta; bool alerta_cpu; bool alerta_pilha; } anterior_t;
static anterior_t    s_ant[MON_MAX_TAREFAS];
static int           s_n_ant = 0;
static TaskStatus_t* s_status = nullptr;

static bool     s_ram_alerta = false, s_bloco_alerta = false, s_psram_alerta = false, s_cpu_alerta = false;
static int      s_cpu_seg_alta = 0;
static uint32_t s_ram_recorde_avisado = UINT32_MAX;
static uint32_t s_ram_anterior = 0;
static uint32_t s_underruns_ant = 0;
static uint32_t s_atrasos_ant = 0;
static int64_t  s_foto_pior_us = 0;   // the worst duration of uxTaskGetSystemState
static uint32_t s_dsi_erros_ant = 0;

// MINIMUM STACK PER TASK NAME, to size stacks with real data.
// A task's high water mark vanishes when it dies (sd_monta, grove_sonda,
// app tasks), and the Tasks app only shows who is alive NOW. This table
// keeps the lowest value ever seen per name and goes to the card every 10 min.
#define PILHAS_N        48
#define PILHAS_PERIODO  600
EXT_RAM_BSS_ATTR static char s_pilha_nome[PILHAS_N][MON_NOME];
static uint32_t s_pilha_min[PILHAS_N];
static int      s_pilha_n = 0;
static uint32_t s_pilha_gravada_em = 0;
EXT_RAM_BSS_ATTR static char s_pilha_linha[PILHAS_N * (MON_NOME + 10) + 32];

// The minute's memory (internal RAM fragmentation).
static uint32_t s_mm_ram_min = UINT32_MAX, s_mm_bloco_min = UINT32_MAX, s_mm_psram_min = UINT32_MAX;
static uint32_t s_mm_blocos_max = 0, s_mm_ocupadas_max = 0;
static float    s_mm_frag_max = 0;
static int      s_mm_amostras = 0;

// The minute's summary.
static float    s_min_cpu_max = 0;
static uint32_t s_min_ram = UINT32_MAX, s_min_psram = UINT32_MAX, s_min_bloco = UINT32_MAX;
static int      s_min_amostras = 0;

// ---- internal RAM statement (see tab5_monitor.hpp) ----
// No lock: the index is reserved with an atomic increment, so two
// simultaneous milestones land in different slots. When full, new milestones are
// discarded - what matters is boot and the first start of each radio.
static mem_marco_t s_marcos[MEM_MARCOS_N];
static uint32_t    s_marcos_n = 0;
static uint32_t    s_marco_livre_ant = 0;

void tab5_mem_marco(const char* etapa)
{
  multi_heap_info_t hi;
  heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
  const uint32_t livre = (uint32_t)hi.total_free_bytes;
  const uint32_t ant   = __atomic_exchange_n(&s_marco_livre_ant, livre, __ATOMIC_RELAXED);
  const int32_t  delta = ant ? (int32_t)livre - (int32_t)ant : 0;

  ESP_LOGI("mem", "%-26s free RAM %6lu B (%+7ld)  largest block %6lu B  minimum %6lu B",
           etapa, (unsigned long)livre, (long)delta, (unsigned long)hi.largest_free_block,
           (unsigned long)hi.minimum_free_bytes);

  const uint32_t i = __atomic_fetch_add(&s_marcos_n, 1, __ATOMIC_RELAXED);
  if (i >= MEM_MARCOS_N) { return; }
  mem_marco_t& m = s_marcos[i];
  m.ms          = tab5_agora_ms();
  m.livre       = livre;
  m.maior_bloco = (uint32_t)hi.largest_free_block;
  m.minimo      = (uint32_t)hi.minimum_free_bytes;
  m.delta       = delta;
  snprintf(m.etapa, sizeof(m.etapa), "%s", etapa);
}

int tab5_mem_extrato(mem_marco_t* out, int max)
{
  uint32_t n = __atomic_load_n(&s_marcos_n, __ATOMIC_RELAXED);
  if (n > MEM_MARCOS_N) { n = MEM_MARCOS_N; }
  if ((int)n > max)     { n = (uint32_t)max; }
  memcpy(out, s_marcos, n * sizeof(mem_marco_t));
  return (int)n;
}

static inline uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

static uint32_t epoch_valido(void) { return tab5_hora_epoch(); }

static void nome_do_dia(const char* prefixo, char* out, size_t tam)
{
  char pre[24];
  snprintf(pre, sizeof(pre), "%s-", prefixo);
  tab5_hora_nome_do_dia(epoch_valido(), pre, ".csv", out, tam);
}

static void hora_texto(char* out, size_t tam)
{
  const uint32_t e = epoch_valido();
  if (!e) { snprintf(out, tam, "--:--:--"); return; }
  struct tm lt;
  tab5_hora_local(e, &lt);
  snprintf(out, tam, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

// ---------------------------------------------------------------------------
// EVENT: memory + log + card queue
// ---------------------------------------------------------------------------
static void evento(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void evento(const char* fmt, ...)
{
  mon_evento_t e;
  e.uptime_s = uptime_s();
  e.epoch    = epoch_valido();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.texto, sizeof(e.texto), fmt, ap);
  va_end(ap);

  ESP_LOGW(TAG, "%s", e.texto);

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_ev[s_ev_i] = e;
  s_ev_i = (s_ev_i + 1) % MON_EVENTOS_N;
  if (s_ev_n < MON_EVENTOS_N) { ++s_ev_n; }
  ++s_ev_total;
  xSemaphoreGive(s_mutex);

  if (s_n_pend < PENDENTES_N) {
    char h[12];
    hora_texto(h, sizeof(h));
    snprintf(s_pend[s_n_pend++], sizeof(s_pend[0]), "%s,%lu,%s\n",
             h, (unsigned long)e.uptime_s, e.texto);
  }
}

// Through the system writer (hal/tab5_sd.hpp) - hands over and carries on.
static void anexar(const char* prefixo, const char* cabecalho, const char* linhas, int n, size_t passo)
{
  if (n <= 0) { return; }
  char nome[40];
  nome_do_dia(prefixo, nome, sizeof(nome));
  for (int i = 0; i < n; ++i) { tab5_sd_anexar(PASTA, nome, linhas + i * passo, cabecalho); }
}

// ---------------------------------------------------------------------------
// THE ONE-SECOND SAMPLE
// ---------------------------------------------------------------------------
static anterior_t* anterior_de(UBaseType_t numero)
{
  for (int i = 0; i < s_n_ant; ++i) { if (s_ant[i].numero == numero) { return &s_ant[i]; } }
  return nullptr;
}

static void ler_mem(mon_mem_t* m, const mon_mem_t* velho, uint32_t caps, uint32_t agora)
{
  m->livre       = (uint32_t)heap_caps_get_free_size(caps);
  m->total       = (uint32_t)heap_caps_get_total_size(caps);
  m->maior_bloco = (uint32_t)heap_caps_get_largest_free_block(caps);
  m->min_livre    = velho->min_livre;    m->min_livre_em = velho->min_livre_em;
  m->min_bloco    = velho->min_bloco;    m->min_bloco_em = velho->min_bloco_em;
  if (m->livre < m->min_livre)       { m->min_livre = m->livre;       m->min_livre_em = agora; }
  if (m->maior_bloco < m->min_bloco) { m->min_bloco = m->maior_bloco; m->min_bloco_em = agora; }
}

static void anotar_pilha(const char* nome, uint32_t livre)
{
  int k = 0;
  while (k < s_pilha_n && strncmp(s_pilha_nome[k], nome, MON_NOME) != 0) { ++k; }
  if (k == s_pilha_n) {
    if (s_pilha_n >= PILHAS_N) { return; }
    snprintf(s_pilha_nome[k], MON_NOME, "%s", nome);
    s_pilha_min[k] = livre;
    ++s_pilha_n;
  } else if (livre < s_pilha_min[k]) {
    s_pilha_min[k] = livre;
  }
}

// A pool slot with a run inside: the useful name is the OWNER's ("Mesh"), not
// the slot's ("pool2").
static const char* nome_efetivo(const TaskStatus_t& st)
{
  const char* n = st.pcTaskName ? st.pcTaskName : "?";
  if (strncmp(n, "pool", 4) == 0) {
    const char* dono = nullptr;
    tab5_dono_de(st.xHandle, &dono);
    if (dono && dono[0]) { return dono; }
  }
  return n;
}

static void amostrar(bool primeira)
{
  const uint32_t agora = uptime_s();
  // uxTaskGetSystemState runs ENTIRELY in a critical section (on IDF SMP:
  // interrupts off on this core), measuring each task's free stack
  // in there - ~0.8 ms with the CPU at 360 MHz. That is more than the
  // ~0.5 ms slack the panel DMA has between frames: landing in it loses a
  // whole frame. IT WAS THE CAUSE of the "FRAME ATRASADO" (late frame) with both cores
  // IDLE - this loop runs at a fixed 1 s phase, and all the
  // delays had the same fraction of a second. That is why the snapshot is only taken
  // right AFTER an end of frame (tab5_quadro_esperar_fim).
  // IN IDLE MODE THE PANEL IS TORN DOWN: there is no end of frame to wait for
  // (nor a frame to lose). The wait only ended by timeout - 40 wake-ups
  // of 1 tick, every second, with the screen off.
  if (!tab5_power_em_repouso()) { tab5_quadro_esperar_fim(40); }
  const int64_t t_foto = esp_timer_get_time();
  const UBaseType_t n = uxTaskGetSystemState(s_status, MON_MAX_TAREFAS, nullptr);
  const int64_t us_foto = esp_timer_get_time() - t_foto;
  tab5_marco("monitor: task snapshot us", (int32_t)us_foto);
  if (us_foto > s_foto_pior_us) {
    s_foto_pior_us = us_foto;
    // Above 5 ms not even the window between frames covers it (~14 ms, but the real
    // limit depends on where the wait woke up) - worth knowing.
    if (us_foto > 5000 && !tab5_power_em_repouso()) {
      evento("foto das tarefas segurou o nucleo %d por %lld us", (int)xPortGetCoreID(), (long long)us_foto);
    }
  }
  if (n == 0) {
    // More tasks than slots: FreeRTOS fills in nothing. Warn once, instead
    // of the snapshot silently stopping.
    static bool avisou = false;
    if (!avisou) {
      avisou = true;
      evento("monitor cego: %u tarefas, so cabem %d", (unsigned)uxTaskGetNumberOfTasks(), MON_MAX_TAREFAS);
    }
    return;
  }

  uint64_t deltas[MON_MAX_TAREFAS];
  uint64_t total = 0;
  for (UBaseType_t i = 0; i < n; ++i) {
    const anterior_t* a = anterior_de(s_status[i].xTaskNumber);
    const uint64_t atual = (uint64_t)s_status[i].ulRunTimeCounter;
    const uint64_t antes = a ? a->tempo : atual;   // new task: starts from zero
    deltas[i] = (atual >= antes) ? (atual - antes) : atual;
    total += deltas[i];
  }

  // Static: ~1.7 KB each, and only this task uses them - on the stack, together with the
  // card's fprintf, they would squeeze its 6 KB.
  EXT_RAM_BSS_ATTR static mon_foto_t nova, velha;
  memset(&nova, 0, sizeof(nova));
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  velha = s_foto;
  xSemaphoreGive(s_mutex);

  nova.uptime_s = agora;
  nova.n = (int)n;
  nova.foto_us      = (uint32_t)us_foto;
  nova.foto_pior_us = (uint32_t)s_foto_pior_us;
  float idle = 0;
  for (UBaseType_t i = 0; i < n; ++i) {
    mon_tarefa_t& t = nova.t[i];
    snprintf(t.nome, sizeof(t.nome), "%s", nome_efetivo(s_status[i]));
    t.numero      = s_status[i].xTaskNumber;
    t.prioridade  = s_status[i].uxCurrentPriority;
    t.pilha_livre = (uint32_t)s_status[i].usStackHighWaterMark * sizeof(StackType_t);
    // WHERE THE STACK LIVES: just the start address, without reading anything from the task (it
    // may have died after the snapshot). The SIZE cannot be got from here safely:
    // FreeRTOS does not keep it, and the snapshot that would have it is only valid in a panic.
    t.pilha_psram = esp_ptr_external_ram(s_status[i].pxStackBase);
    const char* dono = nullptr;
    t.tipo = (uint8_t)tab5_dono_de(s_status[i].xHandle, &dono);
    snprintf(t.dono, sizeof(t.dono), "%s", dono ? dono : "");
#if ( configTASKLIST_INCLUDE_COREID == 1 )
    t.core = s_status[i].xCoreID;
#else
    t.core = tskNO_AFFINITY;
#endif
    t.pct = total ? (100.0f * (float)deltas[i] / (float)total) : 0.0f;
    if (strncmp(t.nome, "IDLE", 4) == 0) { idle += t.pct; }

    // This task's peak comes from the previous snapshot (if it already existed).
    t.pct_pico = t.pct; t.pct_pico_em = agora;
    for (int k = 0; k < velha.n; ++k) {
      if (velha.t[k].numero == t.numero && velha.t[k].pct_pico > t.pct) {
        t.pct_pico = velha.t[k].pct_pico; t.pct_pico_em = velha.t[k].pct_pico_em;
      }
    }
  }
  // Stable order on screen: by xTaskNumber (creation order).
  for (int i = 1; i < nova.n; ++i) {
    mon_tarefa_t c = nova.t[i]; int j = i - 1;
    while (j >= 0 && nova.t[j].numero > c.numero) { nova.t[j + 1] = nova.t[j]; --j; }
    nova.t[j + 1] = c;
  }

  // Minimum stack per name (only this task touches the table). Pool slots
  // go in under the RUN's name (further down); here only ordinary tasks
  // and slots with a known owner.
  for (int i = 0; i < nova.n; ++i) {
    const mon_tarefa_t& t = nova.t[i];
    if (strncmp(t.nome, "IDLE", 4) == 0 || strncmp(t.nome, "pool", 4) == 0) { continue; }
    anotar_pilha(t.nome, t.pilha_livre);
  }
  {
    pool_pilha_t pp[24];
    const int np = pool_pilhas(pp, 24);
    for (int i = 0; i < np; ++i) { anotar_pilha(pp[i].nome, pp[i].livre_min); }
  }

  nova.cpu = 100.0f - idle;
  if (nova.cpu < 0) { nova.cpu = 0; }
  if (nova.cpu > 100) { nova.cpu = 100; }
  nova.cpu_pico = velha.cpu_pico; nova.cpu_pico_em = velha.cpu_pico_em;
  if (primeira || nova.cpu > nova.cpu_pico) { nova.cpu_pico = nova.cpu; nova.cpu_pico_em = agora; }

  if (primeira) {
    nova.ram.min_livre = nova.ram.min_bloco = UINT32_MAX;
    nova.psram.min_livre = nova.psram.min_bloco = UINT32_MAX;
    ler_mem(&nova.ram,   &nova.ram,   MALLOC_CAP_INTERNAL, agora);
    ler_mem(&nova.psram, &nova.psram, MALLOC_CAP_SPIRAM,   agora);
  } else {
    ler_mem(&nova.ram,   &velha.ram,   MALLOC_CAP_INTERNAL, agora);
    ler_mem(&nova.psram, &velha.psram, MALLOC_CAP_SPIRAM,   agora);
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_foto = nova;
  s_tem_foto = true;
  s_h_cpu[s_h_i] = nova.cpu; s_h_ram[s_h_i] = nova.ram.livre; s_h_psram[s_h_i] = nova.psram.livre;
  s_h_i = (s_h_i + 1) % MON_HIST_N;
  if (s_h_n < MON_HIST_N) { ++s_h_n; }
  xSemaphoreGive(s_mutex);

  // ---- triggers -----------------------------------------------------------
  if (!primeira) {
    const mon_mem_t& r = nova.ram;
    if (!s_ram_alerta && r.livre < RAM_BAIXA) {
      s_ram_alerta = true;
      evento("RAM baixa: %luK livres (maior bloco %luK)", (unsigned long)(r.livre / 1024), (unsigned long)(r.maior_bloco / 1024));
    } else if (s_ram_alerta && r.livre > RAM_BAIXA + RAM_FOLGA) {
      s_ram_alerta = false;
      evento("RAM normalizada: %luK livres", (unsigned long)(r.livre / 1024));
    }
    if (!s_bloco_alerta && r.maior_bloco < BLOCO_BAIXO) {
      s_bloco_alerta = true;
      evento("RAM fragmentada: maior bloco %luK (livre %luK)", (unsigned long)(r.maior_bloco / 1024), (unsigned long)(r.livre / 1024));
    } else if (s_bloco_alerta && r.maior_bloco > BLOCO_BAIXO + BLOCO_FOLGA) {
      s_bloco_alerta = false;
    }
    if (!s_psram_alerta && nova.psram.livre < PSRAM_BAIXA) {
      s_psram_alerta = true;
      evento("PSRAM baixa: %luK livres", (unsigned long)(nova.psram.livre / 1024));
    } else if (s_psram_alerta && nova.psram.livre > PSRAM_BAIXA + PSRAM_FOLGA) {
      s_psram_alerta = false;
    }
    if (agora >= ACOMODAR_S && r.livre + NOVO_MIN_PASSO <= s_ram_recorde_avisado) {
      if (s_ram_recorde_avisado != UINT32_MAX) {
        evento("new RAM low: %luK free", (unsigned long)(r.livre / 1024));
      }
      s_ram_recorde_avisado = r.livre;
    }
    if (s_ram_anterior > r.livre + QUEDA_BRUSCA) {
      evento("RAM caiu %luK em 1s (%luK -> %luK)", (unsigned long)((s_ram_anterior - r.livre) / 1024),
             (unsigned long)(s_ram_anterior / 1024), (unsigned long)(r.livre / 1024));
    }
    if (nova.cpu > CPU_TOTAL_ALTA) {
      if (++s_cpu_seg_alta >= CPU_TOTAL_SEG && !s_cpu_alerta) {
        s_cpu_alerta = true;
        evento("CPU above %.0f%% for %ds", (double)CPU_TOTAL_ALTA, s_cpu_seg_alta);
      }
    } else {
      s_cpu_seg_alta = 0;
      if (s_cpu_alerta && nova.cpu < CPU_TOTAL_OK) { s_cpu_alerta = false; }
    }
  }
  s_ram_anterior = nova.ram.livre;

  // THE BLUE FRAME BECOMES AN EVENT. The underrun watcher
  // (hal/tab5_quadro) counts each time the panel ran out of data; here each
  // increase becomes an event with the APP THAT WAS OPEN - the blue flash stops
  // being anecdote and gets a time and place.
  const uint32_t und = tab5_quadro_underruns();
  if (!primeira && und > s_underruns_ant) {
    const char* app = sistema_app_atual();
    evento("panel UNDERRUN (+%lu) with %s open", (unsigned long)(und - s_underruns_ant),
           app ? app : "a home");
  }
  s_underruns_ant = und;

  // HAS THE INTERFACE STOPPED? (screen frozen, no touch, nothing in the log). Its loop counts rounds and leaves the name of the step it is on
  // (kernel/core.cpp, ETAPA). Stopped for more than 3 s: an event with the step - just
  // once, and another when it comes back. The task watchdog catches the BLOCKED
  // loop; this also catches a loop that stopped going round for some other reason.
  {
    static bool     s_presa = false;
    static uint32_t s_presa_desde = 0;
    sistema_diag_t d;
    sistema_diag_interface(&d);
    // In idle mode the loop deliberately waits up to the power-saving contract's
    // timeout (5-10 s, app.h): 3 s there was a false alarm - and right on waking,
    // the last round is still the one from before sleep.
    const bool dormindo = d.repouso || (d.etapa && strncmp(d.etapa, "repouso", 7) == 0);
    const uint32_t limite = dormindo ? 12000 : 3000;
    if (!s_presa && d.voltas > 0 && d.ms_desde_volta > limite) {
      s_presa = true;
      s_presa_desde = d.voltas;
      evento("INTERFACE PRESA ha %lus em: %s", (unsigned long)(d.ms_desde_volta / 1000), d.etapa);
      evento("  with %s%s; finger read %lds ago", d.app ? d.app : "a home",
             d.repouso ? " (repouso)" : "",
             d.ms_desde_dedo == UINT32_MAX ? -1L : (long)(d.ms_desde_dedo / 1000));
    } else if (s_presa && d.voltas != s_presa_desde) {
      s_presa = false;
      evento("UI came back (stalled in: %s)", d.etapa);
    }
  }

  // The detectors that do not depend on the driver log (see hal/tab5_quadro):
  // a late frame (DMA restarted late) and a DSI host error.
  {
    uint32_t pior_us = 0, periodo_us = 0;
    const uint32_t atr = tab5_quadro_atrasos(&pior_us, &periodo_us);
    if (!primeira && atr > s_atrasos_ant) {
      const char* app = sistema_app_atual();
      evento("LATE FRAME (+%lu) worst %lu.%lums (normal %lu.%lums) with %s",
             (unsigned long)(atr - s_atrasos_ant),
             (unsigned long)(pior_us / 1000), (unsigned long)(pior_us % 1000 / 100),
             (unsigned long)(periodo_us / 1000), (unsigned long)(periodo_us % 1000 / 100),
             app ? app : "a home");
      int nucleo = -1; char t0[16], t1[16];
      tab5_quadro_atraso_quem(&nucleo, t0, t1);
      evento("  delay: ISR on core %d; running c0=%s c1=%s; pc 0x%08lx", nucleo, t0, t1,
             (unsigned long)tab5_quadro_atraso_pc());
      // WHAT HAPPENED NEARBY: how long before the delay the card actually mounted/
      // unmounted and the last frame went out (and what size).
      const int64_t t = tab5_quadro_atraso_quando();
      int64_t mont = 0, desm = 0, qd = 0; uint32_t px = 0;
      tab5_sd_ultimas_transicoes(&mont, &desm);
      comp_ultimo_quadro(&qd, &px);
      auto ms = [&](int64_t x) { return x ? (long)((t - x) / 1000) : -1L; };
      evento("  em %ld.%03lds: sd +%ldms -%ldms, quadro %ldms antes (%luKpx)",
             (long)(t / 1000000), (long)(t / 1000 % 1000), ms(mont), ms(desm), ms(qd),
             (unsigned long)(px / 1000));
      // THE MILESTONES OF THE PREVIOUS 300 ms (hal/tab5_marcos): CPU clock
      // changes (and with them the PSRAM bus readjustment), idle mode, Grove
      // probing... Straight to the log, not via evento(), because the line is long.
      tab5_marco_t m[12];
      const int nm = tab5_marcos_entre(t - 300000, t + 2000, m, 12);
      if (nm == 0) {
        ESP_LOGW(TAG, "  milestones: none in the 300 ms before");
      } else {
        char linha[400];
        int k = snprintf(linha, sizeof(linha), "  marcos:");
        for (int i = 0; i < nm && k < (int)sizeof(linha) - 48; ++i) {
          k += snprintf(linha + k, sizeof(linha) - k, " [%+ldms %s %ld]",
                        (long)((m[i].us - t) / 1000), m[i].o_que, (long)m[i].valor);
        }
        ESP_LOGW(TAG, "%s", linha);
      }
    }
    s_atrasos_ant = atr;

    uint32_t st0 = 0, st1 = 0;
    const uint32_t err = tab5_quadro_erros_dsi(&st0, &st1);
    if (!primeira && err > s_dsi_erros_ant) {
      const char* app = sistema_app_atual();
      evento("DSI host: error st0=%08lx st1=%08lx (+%lu frames) with %s",
             (unsigned long)st0, (unsigned long)st1, (unsigned long)(err - s_dsi_erros_ant),
             app ? app : "a home");
    }
    s_dsi_erros_ant = err;
  }

  // Per task: CPU stuck high, stack nearly exhausted, birth. The previous state
  // of each is rebuilt here - those that vanished are left out.
  static anterior_t novo_ant[MON_MAX_TAREFAS];
  for (UBaseType_t i = 0; i < n; ++i) {
    const char* nome = nome_efetivo(s_status[i]);
    const anterior_t* a = anterior_de(s_status[i].xTaskNumber);
    anterior_t& b = novo_ant[i];
    b.numero       = s_status[i].xTaskNumber;
    b.tempo        = (uint64_t)s_status[i].ulRunTimeCounter;
    b.seg_alta     = a ? a->seg_alta : 0;
    b.alerta_cpu   = a ? a->alerta_cpu : false;
    b.alerta_pilha = a ? a->alerta_pilha : false;

    if (!a && !primeira && !efemera(nome)) { evento("tarefa nova: %s", nome); }

    const float pct = total ? (100.0f * (float)deltas[i] / (float)total) : 0.0f;
    if (strncmp(nome, "IDLE", 4) != 0) {
      if (pct > CPU_TAREFA_ALTA) {
        if (++b.seg_alta >= CPU_TAREFA_SEG && !b.alerta_cpu) {
          b.alerta_cpu = true;
          evento("%s at %.0f%% CPU for %ds", nome, (double)pct, b.seg_alta);
        }
      } else {
        b.seg_alta = 0;
        if (pct < CPU_TAREFA_OK) { b.alerta_cpu = false; }
      }
    }
    const uint32_t pilha = (uint32_t)s_status[i].usStackHighWaterMark * sizeof(StackType_t);
    if (!b.alerta_pilha && pilha < PILHA_BAIXA) {
      b.alerta_pilha = true;
      evento("stack of %s nearly full: %luB free", nome, (unsigned long)pilha);
    }
  }
  if (!primeira) {
    for (int k = 0; k < s_n_ant; ++k) {
      bool viva = false;
      for (UBaseType_t i = 0; i < n; ++i) { if (novo_ant[i].numero == s_ant[k].numero) { viva = true; break; } }
      if (!viva) {
        const char* nome = "?";
        for (int j = 0; j < velha.n; ++j) { if (velha.t[j].numero == s_ant[k].numero) { nome = velha.t[j].nome; } }
        if (!efemera(nome)) { evento("tarefa encerrada: %s", nome); }
      }
    }
  }
  memcpy(s_ant, novo_ant, sizeof(anterior_t) * n);
  s_n_ant = (int)n;

  // ---- the minute's memory: fragmentation becomes a number ----------------
  // frag % = 1 - (largest block / free): 0 = all in one piece; the higher,
  // the more chopped up the internal heap. 'blocos' = separate free chunks.
  {
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    pool_estado_t pe;
    pool_estado(&pe);
    const uint32_t livre = (uint32_t)hi.total_free_bytes;
    const float frag = livre ? 100.0f * (1.0f - (float)hi.largest_free_block / (float)livre) : 0.0f;
    if (livre < s_mm_ram_min)                      { s_mm_ram_min = livre; }
    if (hi.largest_free_block < s_mm_bloco_min)    { s_mm_bloco_min = (uint32_t)hi.largest_free_block; }
    if (nova.psram.livre < s_mm_psram_min)         { s_mm_psram_min = nova.psram.livre; }
    if (hi.free_blocks > s_mm_blocos_max)          { s_mm_blocos_max = (uint32_t)hi.free_blocks; }
    if (frag > s_mm_frag_max)                      { s_mm_frag_max = frag; }
    if ((uint32_t)pe.ocupadas > s_mm_ocupadas_max) { s_mm_ocupadas_max = (uint32_t)pe.ocupadas; }
    if (++s_mm_amostras >= 60) {
      char h[12], linha[112];
      hora_texto(h, sizeof(h));
      snprintf(linha, sizeof(linha), "%s,%lu,%lu,%lu,%lu,%.0f,%lu,%lu,%lu\n", h, (unsigned long)agora,
               (unsigned long)(s_mm_ram_min / 1024), (unsigned long)(s_mm_bloco_min / 1024),
               (unsigned long)s_mm_blocos_max, (double)s_mm_frag_max,
               (unsigned long)(s_mm_psram_min / 1024), (unsigned long)s_mm_ocupadas_max,
               (unsigned long)pe.dinamicas);
      anexar("memoria",
             "hora,uptime_s,ram_livre_min_kb,maior_bloco_min_kb,blocos_livres_max,frag_max_pct,psram_livre_min_kb,vagas_ocupadas_max,vagas_dinamicas\n",
             linha, 1, sizeof(linha));
      s_mm_ram_min = s_mm_bloco_min = s_mm_psram_min = UINT32_MAX;
      s_mm_blocos_max = s_mm_ocupadas_max = 0; s_mm_frag_max = 0; s_mm_amostras = 0;
    }
  }

  // ---- the minute's frames: the cost of the interface ---------------------
  // Only writes a minute that HAD frames: an idle screen costs no card writes.
  // Copy MB = read + write in PSRAM (x4 per pixel); drawn = write
  // only (x2); ms = average per frame. p1..p4mais = interface rounds that
  // ended in a frame, by how many panel periods they took - above
  // 1 the frame missed its turn.
  if (++s_qd_amostras >= 60) {
    s_qd_amostras = 0;
    comp_estat_t a;
    comp_estat(&a);
    const comp_estat_t& b = s_qd_ant;
    const uint32_t q = a.quadros - b.quadros;
    if (q > 0) {
      auto ms = [&](uint64_t us) { return (double)us / 1000.0 / q; };
      char h[12], linha[200];
      hora_texto(h, sizeof(h));
      snprintf(linha, sizeof(linha),
               "%s,%lu,%lu,%lu,%lu,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%lu,%lu,%lu\n",
               h, (unsigned long)agora, (unsigned long)q,
               (unsigned long)(a.cheios - b.cheios), (unsigned long)(a.estouros - b.estouros),
               (double)(a.px_quadro - b.px_quadro) * 2.0 / 1e6,
               (double)(a.px_sinc - b.px_sinc) * 4.0 / 1e6,
               (double)(a.px_rolar_dma - b.px_rolar_dma) * 4.0 / 1e6,
               ms(a.us_desenho - b.us_desenho), ms(a.us_copia - b.us_copia),
               ms(a.us_espera - b.us_espera), ms(a.us_rolar - b.us_rolar),
               ms(a.us_volta - b.us_volta),
               (unsigned long)(a.voltas[0] - b.voltas[0]), (unsigned long)(a.voltas[1] - b.voltas[1]),
               (unsigned long)(a.voltas[2] - b.voltas[2]), (unsigned long)(a.voltas[3] - b.voltas[3]));
      // The format changed with direct mode: a new file ("quadros2"), so as
      // not to mix columns with the day's one.
      anexar("quadros2",
             "hora,uptime_s,quadros,cheios,estouros,mb_desenhado,mb_sinc,mb_rolagem_dma,"
             "ms_desenho,ms_copia,ms_espera,ms_rolagem,ms_volta,p1,p2,p3,p4mais\n",
             linha, 1, sizeof(linha));
    }
    s_qd_ant = a;
  }

  // ---- the minute's summary -----------------------------------------------
  if (nova.cpu > s_min_cpu_max)          { s_min_cpu_max = nova.cpu; }
  if (nova.ram.livre < s_min_ram)        { s_min_ram = nova.ram.livre; }
  if (nova.psram.livre < s_min_psram)    { s_min_psram = nova.psram.livre; }
  if (nova.ram.maior_bloco < s_min_bloco){ s_min_bloco = nova.ram.maior_bloco; }
  if (++s_min_amostras >= 60) {
    char h[12], linha[112];
    hora_texto(h, sizeof(h));
    snprintf(linha, sizeof(linha), "%s,%lu,%.0f,%lu,%lu,%lu,%d\n", h, (unsigned long)agora,
             (double)s_min_cpu_max, (unsigned long)(s_min_ram / 1024),
             (unsigned long)(s_min_psram / 1024), (unsigned long)(s_min_bloco / 1024), nova.n);
    anexar("resumo", "hora,uptime_s,cpu_max_pct,ram_min_kb,psram_min_kb,bloco_min_kb,tarefas\n",
           linha, 1, sizeof(linha));
    s_min_cpu_max = 0; s_min_ram = s_min_psram = s_min_bloco = UINT32_MAX; s_min_amostras = 0;
  }

  // Minimum stack per task: one line every 10 min, "nome=livreB" separated
  // by ';'. The file is short (144 lines/day) and can be compared with the
  // declared size of each task.
  if (agora - s_pilha_gravada_em >= PILHAS_PERIODO && s_pilha_n > 0) {
    s_pilha_gravada_em = agora;
    char h[12];
    hora_texto(h, sizeof(h));
    int o = snprintf(s_pilha_linha, sizeof(s_pilha_linha), "%s,%lu,", h, (unsigned long)agora);
    for (int k = 0; k < s_pilha_n && o < (int)sizeof(s_pilha_linha) - (MON_NOME + 12); ++k) {
      o += snprintf(s_pilha_linha + o, sizeof(s_pilha_linha) - o, "%s%s=%lu",
                    k ? ";" : "", s_pilha_nome[k], (unsigned long)s_pilha_min[k]);
    }
    snprintf(s_pilha_linha + o, sizeof(s_pilha_linha) - o, "\n");
    anexar("pilhas", "hora,uptime_s,tarefa=pilha_livre_min_bytes\n", s_pilha_linha, 1, sizeof(s_pilha_linha));
  }

  // Pending events go to the card in one go.
  if (s_n_pend > 0) {
    anexar("eventos", "hora,uptime_s,evento\n", &s_pend[0][0], s_n_pend, sizeof(s_pend[0]));
    s_n_pend = 0;
  }
}

static volatile bool s_zerar_pedido = false;
static void zerar_agora(void);

static void vTaskMonitor(void*)
{
  amostrar(true);
  TickType_t marca = xTaskGetTickCount();
  for (;;) {
    // SCREEN OFF: one sample every 10 s, and the task SLEEPS the 10 s - before,
    // it woke every second just to skip nine (each wake-up pulls the P4 out
    // of light sleep). The task snapshot is a critical section (~1 ms, each task's
    // stack read byte by byte) and nobody looks at the Tasks app in the
    // dark; RAM/CPU alarms arrive up to 10 s later. On waking, it goes back
    // to 1 s on the next round - up to 10 s later (each window's CPU comes from
    // deltas, valid for any interval).
    vTaskDelayUntil(&marca, pdMS_TO_TICKS(tab5_power_em_repouso() ? 10000 : 1000));
    if (s_zerar_pedido) { s_zerar_pedido = false; zerar_agora(); }
    amostrar(false);
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void tab5_monitor_iniciar(void)
{
  if (s_mutex) { return; }
#if ( configGENERATE_RUN_TIME_STATS != 1 )
  ESP_LOGE(TAG, "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS disabled - no per-task CPU");
#endif
  s_mutex  = xSemaphoreCreateMutex();
  // In PSRAM: only the monitor task reads this, and uxTaskGetSystemState
  // suspends the scheduler, it does not turn the cache off.
  s_status = (TaskStatus_t*)heap_caps_malloc(MON_MAX_TAREFAS * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
  if (!s_mutex || !s_status) { ESP_LOGE(TAG, "out of memory"); return; }
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskMonitor, "monitor", 6144, nullptr, 1, &h, tskNO_AFFINITY);
    tab5_dono_registrar(h, DONO_SERVICO, "Monitor");   // see hal/tab5_donos.hpp
  }
}

bool tab5_monitor_foto(mon_foto_t* out)
{
  if (!s_mutex || !out) { return false; }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const bool ok = s_tem_foto;
  if (ok) { *out = s_foto; }
  xSemaphoreGive(s_mutex);
  return ok;
}

int tab5_monitor_historico(float* cpu, uint32_t* ram, uint32_t* psram, int max)
{
  if (!s_mutex) { return 0; }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const int n = (s_h_n < max) ? s_h_n : max;
  const int inicio = (s_h_i - n + MON_HIST_N) % MON_HIST_N;
  for (int k = 0; k < n; ++k) {
    const int i = (inicio + k) % MON_HIST_N;
    if (cpu)   { cpu[k]   = s_h_cpu[i]; }
    if (ram)   { ram[k]   = s_h_ram[i]; }
    if (psram) { psram[k] = s_h_psram[i]; }
  }
  xSemaphoreGive(s_mutex);
  return n;
}

int tab5_monitor_eventos(mon_evento_t* out, int max)
{
  if (!s_mutex) { return 0; }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const int n = (s_ev_n < max) ? s_ev_n : max;
  for (int k = 0; k < n; ++k) {
    out[k] = s_ev[(s_ev_i - 1 - k + MON_EVENTOS_N) % MON_EVENTOS_N];
  }
  xSemaphoreGive(s_mutex);
  return n;
}

uint32_t tab5_monitor_total_eventos(void) { return s_ev_total; }

// Called by the INTERFACE: it only asks. The monitor task does the reset, at the
// start of the next sample - the trigger state and the card queue are
// its, and touching them from another task would be a race.
void tab5_monitor_zerar_picos(void) { s_zerar_pedido = true; }

bool tab5_monitor_programa(uint32_t* usado, uint32_t* total)
{
  const esp_partition_t* p = esp_ota_get_running_partition();
  if (!p) { return false; }
  if (usado) {
    esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t meta;
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK) { return false; }
    *usado = meta.image_len;
  }
  if (total) { *total = (uint32_t)p->size; }
  return true;
}

static void zerar_agora(void)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const uint32_t agora = uptime_s();
  s_foto.cpu_pico = s_foto.cpu; s_foto.cpu_pico_em = agora;
  for (int i = 0; i < s_foto.n; ++i) { s_foto.t[i].pct_pico = s_foto.t[i].pct; s_foto.t[i].pct_pico_em = agora; }
  s_foto.ram.min_livre   = s_foto.ram.livre;         s_foto.ram.min_livre_em = agora;
  s_foto.ram.min_bloco   = s_foto.ram.maior_bloco;   s_foto.ram.min_bloco_em = agora;
  s_foto.psram.min_livre = s_foto.psram.livre;       s_foto.psram.min_livre_em = agora;
  s_foto.psram.min_bloco = s_foto.psram.maior_bloco; s_foto.psram.min_bloco_em = agora;
  xSemaphoreGive(s_mutex);
  s_pilha_n = 0;
  pool_pilhas_zerar();
  s_ram_recorde_avisado = UINT32_MAX;
  evento("picos zerados pelo usuario");
}
