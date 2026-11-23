// H264 - see tab5_h264.hpp.
#include "tab5_h264.hpp"
#include "tab5_volumes.hpp"
#include "tab5_hash.hpp"
#include "recursos.h"

#include <stdio.h>
#include <string.h>
#include <new>
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_clk_tree.h"
#include "esp_cpu.h"

static const char* TAG = "h264";

// The frames (DPB) live in PSRAM. 128 bytes: the PSRAM cache line -
// two frames never share a line (the PPA and the CPU don't get in each other's way).
#define H264_ALINHAMENTO_QUADRO  128
// PSRAM ceiling for the frames. At 720p (1.4 MB per frame) there's room for 4 reference
// frames + the one being decoded + those held by the video; at 360p, plenty.
#define H264_ORCAMENTO_QUADROS   (8u * 1024 * 1024)
// Frames the video can hold at the same time (the one on screen and the next).
#define H264_PRESOS              2
// The arena: the largest useful size (1280 wide, or 640 with the reference
// window). In SRAM it comes in up to 4 PIECES (the P4's SRAM almost never has
// a free block that size); a piece smaller than the minimum is no use at all
// (macroblock rows are ~25-55 KB each). The decoder adapts to
// what it gets: fewer rows in parallel and, if short, no window.
#define H264_ARENA_MAX           (240 * 1024)
// Above 1280 (the phone's 1080p) the macroblock rows grow with the
// width: the arena in PSRAM grows with it (1920 = 360 KB).
#define H264_LADO_MAX            1920
static size_t arena_para(int lado)
{
  const size_t n = lado > 1280 ? (size_t)H264_ARENA_MAX * (size_t)lado / 1280 : H264_ARENA_MAX;
  return (n + 1023) & ~(size_t)1023;
}
#define H264_PEDACO_MIN          (24 * 1024)
#define H264_PEDACOS             (1 + H264_DEC_WORK_EXTRA)
// The floor isn't enough: the pieces take the LARGEST blocks, and what is left may
// end up all fragmented - the worker's stack, Wi-Fi's DMA pool and any
// contiguous system buffer would fail with 80 KB free in crumbs. This
// block is set aside BEFORE the pieces and given back afterwards: there is always at
// least one contiguous block of that size left.
#define H264_RESERVA_CONTIGUA    (24 * 1024)
// The core 1 worker: deblocking and writing the rows. Below
// Musica (the sound must not stutter because of the picture).
#define H264_POS_PILHA           6144
#define H264_POS_PRIO            4
#define H264_POS_NUCLEO          1

struct tab5_h264 {
  h264_dec_t*       dec;
  uint8_t*          pedaco[H264_PEDACOS];      // the arena (pedaco[0] always exists)
  size_t            pedaco_bytes[H264_PEDACOS];
  size_t            arena_bytes;               // the sum
  size_t            sram_bytes;                // how much of it is SRAM
  bool              na_sram;
  h264_dec_threads_t fios;
  tab5_h264_cfg_t   cfg;
  bool              ja_saiu_quadro;   // after the 1st frame, no longer swaps the arena
};

// ---------------------------------------------------------------------------
// What the decoder asks of the system: memory and threads (tasks + semaphores).
// ---------------------------------------------------------------------------
static void* alocar_psram(void*, size_t n)
{
  void* p = heap_caps_aligned_alloc(H264_ALINHAMENTO_QUADRO, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {   // video too large for the PSRAM left: says how much was missing
    ESP_LOGW(TAG, "no PSRAM: asked for %u KB, free %u KB, largest block %u KB", (unsigned)(n / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  }
  return p;
}
static void liberar(void*, void* p) { heap_caps_free(p); }

static void* sem_criar(void*, uint32_t max, uint32_t ini) { return xSemaphoreCreateCounting(max, ini); }
static void  sem_apagar(void*, void* s) { vSemaphoreDelete((SemaphoreHandle_t)s); }
static void  sem_pegar(void*, void* s) { xSemaphoreTake((SemaphoreHandle_t)s, portMAX_DELAY); }
static void  sem_dar(void*, void* s) { xSemaphoreGive((SemaphoreHandle_t)s); }

typedef struct { void (*f)(void*); void* arg; } partida_t;

static void tarefa_pos(void* p)
{
  const partida_t partida = *(partida_t*)p;
  delete (partida_t*)p;
  partida.f(partida.arg);       // only returns when the decoder closes
  tab5_h264_encerrar_tarefa();
}

static bool fio_criar(void*, void (*f)(void*), void* arg)
{
  partida_t* p = new (std::nothrow) partida_t{ f, arg };
  if (!p) { return false; }
  if (tab5_h264_criar_tarefa(tarefa_pos, "h264_pos", H264_POS_PILHA, p, H264_POS_PRIO,
                             H264_POS_NUCLEO, nullptr) != pdPASS) {
    delete p;
    return false;
  }
  return true;
}

BaseType_t tab5_h264_criar_tarefa(TaskFunction_t f, const char* nome, uint32_t pilha, void* arg,
                                  UBaseType_t prio, BaseType_t nucleo, TaskHandle_t* h)
{
  // MALLOC_CAP_SIMD: internal, aligned stack - the context switch saves the
  // PIE registers on it.
  return xTaskCreatePinnedToCoreWithCaps(f, nome, pilha, arg, prio, h, nucleo,
                                         MALLOC_CAP_SIMD | MALLOC_CAP_INTERNAL);
}

void tab5_h264_encerrar_tarefa(void) { vTaskDeleteWithCaps(nullptr); }

// ---------------------------------------------------------------------------
// The arena: in SRAM only what leaves the floor free, in pieces; otherwise, PSRAM.
// ---------------------------------------------------------------------------
static void soltar_arena(tab5_h264_t* d)
{
  for (int i = 0; i < H264_PEDACOS; ++i) {
    heap_caps_free(d->pedaco[i]);
    d->pedaco[i] = nullptr;
    d->pedaco_bytes[i] = 0;
  }
  d->arena_bytes = 0;
}

// Takes pieces from SRAM, always the largest free block, until the arena is gathered or the
// floor allows no more. 'forcar' (bench) ignores the floor.
static bool arena_sram(tab5_h264_t* d, bool forcar, int max_pedacos)
{
  // MALLOC_CAP_SIMD: only the main SRAM (L2MEM). The low-power SRAM
  // (0x5010xxxx) is also "internal", but the PIE can't read from it: the worker
  // died with a load access fault on the first packing.
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD;
  void* reserva = heap_caps_malloc(H264_RESERVA_CONTIGUA, caps);
  if (!reserva) { return false; }   // not even that much contiguous: SRAM is already too tight
  for (int i = 0; i < max_pedacos && d->arena_bytes < H264_ARENA_MAX; ++i) {
    const size_t livre = heap_caps_get_free_size(caps);
    // The floor counts the reserve as free (it comes back soon).
    const size_t piso = H264_PISO_INTERNA > H264_RESERVA_CONTIGUA ? H264_PISO_INTERNA - H264_RESERVA_CONTIGUA : 0;
    // Forced (bench 'sram'): a smaller floor, never zero - with nothing the worker's
    // stack isn't even born.
    const size_t piso_aqui = forcar ? 32 * 1024 : piso;
    size_t pode = livre > piso_aqui ? livre - piso_aqui : 0;
    size_t n = heap_caps_get_largest_free_block(caps);
    if (n > pode) { n = pode; }
    if (n > H264_ARENA_MAX - d->arena_bytes) { n = H264_ARENA_MAX - d->arena_bytes; }
    n &= ~(size_t)(H264_DEC_WORK_ALIGNMENT - 1);
    // The alignment slack: the largest block doesn't hold n aligned + header.
    if (n > 64) { n -= 64; }
    if (n < H264_PEDACO_MIN) {
      ESP_LOGD(TAG, "SRAM: %d piece(s); free %u KB, largest block %u KB, floor leaves %u KB - stopping here",
               i, (unsigned)(livre / 1024), (unsigned)(heap_caps_get_largest_free_block(caps) / 1024), (unsigned)(pode / 1024));
      break;
    }
    d->pedaco[i] = (uint8_t*)heap_caps_aligned_alloc(H264_DEC_WORK_ALIGNMENT, n, caps);
    if (!d->pedaco[i]) { break; }
    d->pedaco_bytes[i] = n;
    d->arena_bytes += n;
  }
  heap_caps_free(reserva);
  return d->pedaco[0] != nullptr;
}

// AUTO (the normal case) = PSRAM. Measured on the Tab5 (2026-11-12, Wi-Fi and MQTT running):
// with a "hot" piece of SRAM in front, 480p was the same (4.9 fps) - the
// 256 KB L2 cache already serves the arena in PSRAM. And the SRAM left above the
// floor barely exceeds 20 KB: taking from it would only squeeze Wi-Fi, for no gain.
// SRAM only on the bench ('video h264 ... sram'), for measuring.
static bool reservar_arena(tab5_h264_t* d, tab5_h264_onde_t onde)
{
  if (onde == TAB5_H264_SRAM) {                   // bench: SRAM only, no disguise
    d->na_sram = arena_sram(d, true, H264_PEDACOS);
    d->sram_bytes = d->arena_bytes;
    return d->na_sram;
  }
  // AUTO is PSRAM: a single piece, the size of the whole arena.
  const size_t n = arena_para(d->cfg.max_l > d->cfg.max_a ? d->cfg.max_l : d->cfg.max_a);
  d->pedaco[0] = (uint8_t*)heap_caps_aligned_alloc(H264_DEC_WORK_ALIGNMENT, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!d->pedaco[0]) { return false; }
  d->pedaco_bytes[0] = n;
  d->arena_bytes = n;
  return true;
}

static uint32_t relogio_ciclos(void) { return (uint32_t)esp_cpu_get_cycle_count(); }

static bool montar(tab5_h264_t* d, tab5_h264_onde_t onde)
{
  if (!reservar_arena(d, onde)) { soltar_arena(d); return false; }
  d->fios = { sem_criar, sem_apagar, sem_pegar, sem_dar, fio_criar, nullptr };
  h264_dec_config_t c = { };
  c.alloc = alocar_psram;
  c.free = liberar;
  c.work = d->pedaco[0];
  c.work_bytes = d->pedaco_bytes[0];
  for (int i = 1; i < H264_PEDACOS; ++i) {
    c.work_extra[i - 1] = d->pedaco[i];
    c.work_extra_bytes[i - 1] = d->pedaco_bytes[i];
  }
  c.max_side = (uint32_t)(d->cfg.max_l > d->cfg.max_a ? d->cfg.max_l : d->cfg.max_a);
  c.max_mbs = (uint32_t)(((d->cfg.max_l + 15) / 16) * ((d->cfg.max_a + 15) / 16));
  c.frame_budget_bytes = H264_ORCAMENTO_QUADROS;
  c.held_pictures = d->cfg.presos ? d->cfg.presos : H264_PRESOS;
  c.threads = &d->fios;
  c.clock = relogio_ciclos;   // only used with H264_DEC_PROFILE (per-stage measurement)
  d->dec = h264_dec_create(&c);
  if (!d->dec) {
    ESP_LOGW(TAG, "decoder not created: pieces %u/%u/%u/%u KB, SRAM free %u KB",
             (unsigned)(d->pedaco_bytes[0] / 1024), (unsigned)(d->pedaco_bytes[1] / 1024),
             (unsigned)(d->pedaco_bytes[2] / 1024), (unsigned)(d->pedaco_bytes[3] / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    soltar_arena(d);
    return false;
  }
  int n = 0;
  while (n < H264_PEDACOS && d->pedaco[n]) { ++n; }
  ESP_LOGI(TAG, "opened: arena of %u KB (%u KB in SRAM, %d piece(s)), up to %dx%d",
           (unsigned)(d->arena_bytes / 1024), (unsigned)(d->sram_bytes / 1024), n, d->cfg.max_l, d->cfg.max_a);
  return true;
}

static void desmontar(tab5_h264_t* d)
{
  if (d->dec) { h264_dec_destroy(d->dec); d->dec = nullptr; }   // waits for the worker to exit
  soltar_arena(d);
}

tab5_h264_t* tab5_h264_abrir(const tab5_h264_cfg_t* cfg)
{
  if (!cfg || cfg->max_l < 16 || cfg->max_a < 16 || cfg->max_l > H264_LADO_MAX || cfg->max_a > H264_LADO_MAX) { return nullptr; }
  tab5_h264_t* d = (tab5_h264_t*)heap_caps_calloc(1, sizeof(tab5_h264_t), MALLOC_CAP_SPIRAM);
  if (!d) { return nullptr; }
  d->cfg = *cfg;
  if (!montar(d, cfg->onde)) { heap_caps_free(d); return nullptr; }
  return d;
}

void tab5_h264_fechar(tab5_h264_t* d)
{
  if (!d) { return; }
  desmontar(d);
  heap_caps_free(d);
}

h264_dec_result_t tab5_h264_decodificar(tab5_h264_t* d, const uint8_t* dados, size_t n,
                                        uint8_t nal_tam, int64_t marca)
{
  // The hybrid arena always has the full-size PSRAM piece: running out of
  // memory here means the video really is too big (the error says which).
  return h264_dec_decode(d->dec, dados, n, nal_tam, marca);
}

bool tab5_h264_quadro(tab5_h264_t* d, tab5_h264_quadro_t* q)
{
  h264_dec_picture_t p;
  if (!h264_dec_output(d->dec, &p)) { return false; }   // comes out HELD (h264_dec_release releases it)
  d->ja_saiu_quadro = true;
  const h264_dec_stream_info_t& i = p.info;
  q->px = p.packed;
  q->l_cod = i.coded_width; q->a_cod = i.coded_height;
  q->x = i.crop_left; q->y = i.crop_top; q->l = i.width; q->a = i.height;
  // Colour matrix: what the file says; if it doesn't, HD is BT.709 and the rest BT.601.
  q->bt709 = i.matrix_coefficients == 1 || (i.matrix_coefficients != 5 && i.matrix_coefficients != 6 && i.height >= 720);
  q->faixa_cheia = i.full_range;
  q->marca = p.tag;
  q->id = p.id;
  return true;
}

void tab5_h264_soltar(tab5_h264_t* d, uint8_t id) { h264_dec_release(d->dec, id); }
void tab5_h264_fim(tab5_h264_t* d) { h264_dec_drain(d->dec); }
void tab5_h264_recomecar(tab5_h264_t* d) { h264_dec_flush(d->dec); }
bool tab5_h264_descartavel(const uint8_t* dados, size_t n, uint8_t nal_tam) { return h264_dec_droppable(dados, n, nal_tam); }
const char* tab5_h264_erro(tab5_h264_t* d) { return h264_dec_error(d->dec); }
bool tab5_h264_na_sram(const tab5_h264_t* d) { return d->na_sram; }
size_t tab5_h264_sram_bytes(const tab5_h264_t* d) { return d->sram_bytes; }
size_t tab5_h264_arena_bytes(const tab5_h264_t* d) { return d->arena_bytes; }

// ---------------------------------------------------------------------------
// BENCH
// ---------------------------------------------------------------------------
typedef struct {
  const char*          caminho;
  tab5_h264_onde_t     onde;
  bool                 hash;
  tab5_h264_bancada_t* r;
  SemaphoreHandle_t    feito;
} bancada_t;

// A "00 00 01" from p onwards (or the end).
static const uint8_t* proximo_inicio(const uint8_t* p, const uint8_t* fim)
{
  for (; p + 3 <= fim; ++p) { if (p[0] == 0 && p[1] == 0 && p[2] == 1) { return p; } }
  return fim;
}

static void tirar_quadros(tab5_h264_t* d, bancada_t* b)
{
  tab5_h264_quadro_t q;
  while (tab5_h264_quadro(d, &q)) {
    if (b->hash) {
      // The same FNV-1a as the Mac test: the whole packed frame.
      const size_t bytes = (size_t)q.l_cod * 3 / 2 * q.a_cod;
      printf("[H264V] f=%u h=%08lx\n", (unsigned)b->r->quadros, (unsigned long)tab5_fnv1a(q.px, bytes));
    }
    b->r->l = q.l; b->r->a = q.a;
    ++b->r->quadros;
    tab5_h264_soltar(d, q.id);
  }
}

#define BANCADA_MAX  (8u * 1024 * 1024)   // the maximum file loaded on the bench

static void tarefa_bancada(void* p)
{
  bancada_t* b = (bancada_t*)p;
  tab5_h264_bancada_t* r = b->r;
  uint8_t* dados = nullptr;
  size_t n = 0;
  tab5_h264_t* d = nullptr;

  // 1. The file in PSRAM (the bench measures decoding only). At most
  //    BANCADA_MAX: an 18 MB file wholly in PSRAM left no room
  //    for the reference frames (the real player reads bit by bit).
  tab5_leitor_t* lt = tab5_leitor_abrir(b->caminho);
  if (!lt) { snprintf(r->erro, sizeof(r->erro), "could not open the file"); goto fim; }
  n = (size_t)tab5_leitor_tamanho(lt);
  if (n > BANCADA_MAX) { n = BANCADA_MAX; }
  dados = n ? (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : nullptr;
  if (!dados || tab5_leitor_ler(lt, dados, n) != (int)n) {
    snprintf(r->erro, sizeof(r->erro), dados ? "read failed" : "file empty or too big");
    tab5_leitor_fechar(lt);
    goto fim;
  }
  tab5_leitor_fechar(lt);
  // Truncated: ends before the last NAL start (00 00 01), so the last
  // packet isn't left half-done.
  if (n == BANCADA_MAX) {
    while (n > 3 && !(dados[n - 3] == 0 && dados[n - 2] == 0 && dados[n - 1] == 1)) { --n; }
    n = n > 3 ? n - 3 : 0;
  }

  {
    // 2. Decodes packet by packet: a packet ends where the first
    //    slice of the next frame begins (or an SEI/SPS/PPS/AUD after a picture).
    const tab5_h264_cfg_t cfg = { 1280, 1280, b->onde };
    d = tab5_h264_abrir(&cfg);
    if (!d) { snprintf(r->erro, sizeof(r->erro), "out of memory for the decoder"); goto fim; }
    const uint8_t* fim_dados = dados + n;
    const uint8_t* q = proximo_inicio(dados, fim_dados);
    const uint8_t* au = q;
    bool tem_imagem = false;
    uint32_t pacote = 0;
    // The clock at the maximum allowed during the measurement: without this lock DFS
    // leaves the CPU at the floor (40 MHz when idle) and the bench measures the floor.
    esp_pm_lock_handle_t trava = nullptr;
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "h264_banc", &trava) == ESP_OK) { esp_pm_lock_acquire(trava); }
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
      const uint8_t* nal = q < fim_dados ? q + 3 : fim_dados;
      bool fronteira = q >= fim_dados;
      if (!fronteira && tem_imagem && nal < fim_dados) {
        const uint8_t tipo = nal[0] & 0x1F;
        const bool primeira_fatia = (tipo == 1 || tipo == 5) && nal + 1 < fim_dados && (nal[1] & 0x80);
        fronteira = primeira_fatia || (tipo >= 6 && tipo <= 9);
      }
      if (fronteira && au < q) {
        const h264_dec_result_t res = tab5_h264_decodificar(d, au, (size_t)(q - au), 0, pacote++);
        if (res != H264_DEC_OK && res != H264_DEC_NO_PICTURE) {
          ++r->com_erro;
          if (res == H264_DEC_UNSUPPORTED || res == H264_DEC_NO_MEMORY) {
            const char* e = tab5_h264_erro(d);
            snprintf(r->erro, sizeof(r->erro), "%s", e ? e : "unsupported / out of memory");
            break;
          }
        }
        tirar_quadros(d, b);
        au = q;
        tem_imagem = false;
      }
      if (q >= fim_dados) { break; }
      const uint8_t tipo = nal[0] & 0x1F;
      if (tipo == 1 || tipo == 5) { tem_imagem = true; }
      q = proximo_inicio(nal, fim_dados);
    }
    tab5_h264_fim(d);
    tirar_quadros(d, b);
    r->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    uint32_t hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz);
    r->cpu_mhz = hz / 1000000;
    if (trava) { esp_pm_lock_release(trava); esp_pm_lock_delete(trava); }
#ifdef H264_DEC_PROFILE
    // Time per stage (cycles -> ms at cpu_mhz): where decoding spends its time.
    uint64_t prof[H264_PROF_COUNT];
    if (h264_dec_take_profile(d->dec, prof) && r->cpu_mhz) {
      static const char* const NOMES[H264_PROF_COUNT] = { "mc_luma", "mc_busca", "janela", "mc_croma", "intra",
                                                          "cavlc", "idct", "deblock", "empacota", "grava", "espera", "TOTAL" };
      for (int k = 0; k < H264_PROF_COUNT; ++k) {
        printf("  etapa %-9s %8llu ms\n", NOMES[k], (unsigned long long)(prof[k] / (r->cpu_mhz * 1000ull)));
      }
    }
#endif
    r->na_sram = tab5_h264_na_sram(d);
    r->sram_kb = (uint32_t)(tab5_h264_sram_bytes(d) / 1024);
    r->arena_kb = (uint32_t)(tab5_h264_arena_bytes(d) / 1024);
    r->interna_livre_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    r->interna_min_kb = (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    r->ok = r->quadros > 0 && !r->erro[0];
  }

fim:
  if (d) { tab5_h264_fechar(d); }
  heap_caps_free(dados);
  if (b->feito) {
    xSemaphoreGive(b->feito);
  } else {
    // In the background: nobody waits - the result goes to the log.
    ESP_LOGI(TAG, "bench: %s%s%lu frames %dx%d in %lu ms = %.1f frames/s | arena %lu KB (%lu in SRAM) | CPU %lu MHz",
             r->erro, r->erro[0] ? " | " : "", (unsigned long)r->quadros, r->l, r->a, (unsigned long)r->ms,
             r->ms ? (double)r->quadros * 1000.0 / (double)r->ms : 0.0, (unsigned long)r->arena_kb,
             (unsigned long)r->sram_kb, (unsigned long)r->cpu_mhz);
    sistema_recurso_soltar(RECURSO_TELA_ACESA, "h264");
    heap_caps_free((void*)b->caminho);
    heap_caps_free(r);
    heap_caps_free(b);
  }
  tab5_h264_encerrar_tarefa();
}

bool tab5_h264_bancada(const char* caminho, tab5_h264_onde_t onde, bool hash, tab5_h264_bancada_t* r)
{
  memset(r, 0, sizeof(*r));
  bancada_t b = { caminho, onde, hash, r, xSemaphoreCreateBinary() };
  if (!b.feito) { snprintf(r->erro, sizeof(r->erro), "out of memory"); return false; }
  // Screen on during the measurement: when idle the clock is limited (180 MHz)
  // and the bench would measure idle, not video (which plays with the screen on).
  sistema_recurso_usar(RECURSO_TELA_ACESA, "h264");
  // The same priority and core the video uses (core 0, below Musica).
  if (tab5_h264_criar_tarefa(tarefa_bancada, "h264_banc", 8192, &b, 4, 0, nullptr) != pdPASS) {
    vSemaphoreDelete(b.feito);
    sistema_recurso_soltar(RECURSO_TELA_ACESA, "h264");
    snprintf(r->erro, sizeof(r->erro), "out of memory for the task");
    return false;
  }
  xSemaphoreTake(b.feito, portMAX_DELAY);
  vSemaphoreDelete(b.feito);
  sistema_recurso_soltar(RECURSO_TELA_ACESA, "h264");
  return r->ok;
}

bool tab5_h264_bancada_fundo(const char* caminho, tab5_h264_onde_t onde)
{
  // All on the heap: the task frees it on finishing (nobody waits for it).
  bancada_t* b = (bancada_t*)heap_caps_calloc(1, sizeof(bancada_t), MALLOC_CAP_SPIRAM);
  tab5_h264_bancada_t* r = (tab5_h264_bancada_t*)heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM);
  char* c = (char*)heap_caps_malloc(strlen(caminho) + 1, MALLOC_CAP_SPIRAM);
  if (!b || !r || !c) { heap_caps_free(b); heap_caps_free(r); heap_caps_free(c); return false; }
  strcpy(c, caminho);
  *b = { c, onde, false, r, nullptr };
  sistema_recurso_usar(RECURSO_TELA_ACESA, "h264");
  if (tab5_h264_criar_tarefa(tarefa_bancada, "h264_banc", 8192, b, 4, 0, nullptr) != pdPASS) {
    sistema_recurso_soltar(RECURSO_TELA_ACESA, "h264");
    heap_caps_free(b); heap_caps_free(r); heap_caps_free(c);
    return false;
  }
  return true;
}
