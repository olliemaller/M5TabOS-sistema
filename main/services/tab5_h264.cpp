// H264 - ver tab5_h264.hpp.
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

// Os quadros (DPB) moram na PSRAM. 128 bytes: a linha da cache da PSRAM -
// dois quadros nunca dividem uma linha (o PPA e a CPU nao se atrapalham).
#define H264_ALINHAMENTO_QUADRO  128
// Teto da PSRAM para os quadros. A 720p (1,4 MB por quadro) da para 4 quadros
// de referencia + o em decodificacao + os presos pelo video; em 360p, folgado.
#define H264_ORCAMENTO_QUADROS   (8u * 1024 * 1024)
// Quadros que o video pode segurar ao mesmo tempo (o na tela e o seguinte).
#define H264_PRESOS              2
// A arena: o maior tamanho util (1280 de largura, ou 640 com a janela de
// referencia). Na SRAM ela vem em ate 4 PEDACOS (a SRAM do P4 quase nunca tem
// um bloco livre desse tamanho); pedaco menor que o minimo nao serve para nada
// (as linhas de macrobloco tem ~25-55 KB cada). O decodificador se ajusta ao
// que recebe: menos linhas em paralelo e, se faltar, sem a janela.
#define H264_ARENA_MAX           (240 * 1024)
// Acima de 1280 (o 1080p do celular) as linhas de macrobloco crescem com a
// largura: a arena na PSRAM cresce junto (1920 = 360 KB).
#define H264_LADO_MAX            1920
static size_t arena_para(int lado)
{
  const size_t n = lado > 1280 ? (size_t)H264_ARENA_MAX * (size_t)lado / 1280 : H264_ARENA_MAX;
  return (n + 1023) & ~(size_t)1023;
}
#define H264_PEDACO_MIN          (24 * 1024)
#define H264_PEDACOS             (1 + H264_DEC_WORK_EXTRA)
// O piso nao basta: os pedacos levam os MAIORES blocos, e o que sobra pode
// ficar todo picado - a pilha do trabalhador, o pool DMA do Wi-Fi e qualquer
// buffer contiguo do sistema falhariam com 80 KB livres em migalhas. Este
// bloco e separado ANTES dos pedacos e devolvido depois: sempre sobra pelo
// menos um bloco contiguo desse tamanho.
#define H264_RESERVA_CONTIGUA    (24 * 1024)
// O trabalhador do nucleo 1: deblocking e gravacao das linhas. Abaixo da
// Musica (o som nao pode engasgar por causa da imagem).
#define H264_POS_PILHA           6144
#define H264_POS_PRIO            4
#define H264_POS_NUCLEO          1

struct tab5_h264 {
  h264_dec_t*       dec;
  uint8_t*          pedaco[H264_PEDACOS];      // a arena (pedaco[0] sempre existe)
  size_t            pedaco_bytes[H264_PEDACOS];
  size_t            arena_bytes;               // a soma
  size_t            sram_bytes;                // quanto dela e SRAM
  bool              na_sram;
  h264_dec_threads_t fios;
  tab5_h264_cfg_t   cfg;
  bool              ja_saiu_quadro;   // depois do 1o quadro, nao troca mais a arena
};

// ---------------------------------------------------------------------------
// O que o decodificador pede ao sistema: memoria e fios (tarefas + semaforos).
// ---------------------------------------------------------------------------
static void* alocar_psram(void*, size_t n)
{
  void* p = heap_caps_aligned_alloc(H264_ALINHAMENTO_QUADRO, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {   // video grande demais para a PSRAM que sobrou: diz quanto faltou
    ESP_LOGW(TAG, "sem PSRAM: pediu %u KB, livre %u KB, maior bloco %u KB", (unsigned)(n / 1024),
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
  partida.f(partida.arg);       // so volta quando o decodificador fecha
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
  // MALLOC_CAP_SIMD: pilha interna e alinhada - a troca de contexto guarda os
  // registradores da PIE nela.
  return xTaskCreatePinnedToCoreWithCaps(f, nome, pilha, arg, prio, h, nucleo,
                                         MALLOC_CAP_SIMD | MALLOC_CAP_INTERNAL);
}

void tab5_h264_encerrar_tarefa(void) { vTaskDeleteWithCaps(nullptr); }

// ---------------------------------------------------------------------------
// A arena: na SRAM so o que deixar o piso livre, em pedacos; senao, PSRAM.
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

// Pega pedacos da SRAM, sempre o maior bloco livre, ate juntar a arena ou o
// piso nao deixar mais. 'forcar' (bancada) ignora o piso.
static bool arena_sram(tab5_h264_t* d, bool forcar, int max_pedacos)
{
  // MALLOC_CAP_SIMD: so a SRAM principal (L2MEM). A SRAM de baixo consumo
  // (0x5010xxxx) tambem e "interna", mas a PIE nao le dela: o trabalhador
  // morria com load access fault no primeiro empacotamento.
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD;
  void* reserva = heap_caps_malloc(H264_RESERVA_CONTIGUA, caps);
  if (!reserva) { return false; }   // nem isso contiguo: a SRAM ja esta apertada demais
  for (int i = 0; i < max_pedacos && d->arena_bytes < H264_ARENA_MAX; ++i) {
    const size_t livre = heap_caps_get_free_size(caps);
    // O piso conta a reserva como livre (ela volta logo).
    const size_t piso = H264_PISO_INTERNA > H264_RESERVA_CONTIGUA ? H264_PISO_INTERNA - H264_RESERVA_CONTIGUA : 0;
    // Forcado (bancada 'sram'): piso menor, nunca zero - sem nada a pilha do
    // trabalhador nem nasce.
    const size_t piso_aqui = forcar ? 32 * 1024 : piso;
    size_t pode = livre > piso_aqui ? livre - piso_aqui : 0;
    size_t n = heap_caps_get_largest_free_block(caps);
    if (n > pode) { n = pode; }
    if (n > H264_ARENA_MAX - d->arena_bytes) { n = H264_ARENA_MAX - d->arena_bytes; }
    n &= ~(size_t)(H264_DEC_WORK_ALIGNMENT - 1);
    // A folga do alinhamento: o maior bloco nao comporta n alinhado + cabecalho.
    if (n > 64) { n -= 64; }
    if (n < H264_PEDACO_MIN) {
      ESP_LOGD(TAG, "SRAM: %d pedaco(s); livre %u KB, maior bloco %u KB, piso deixa %u KB - para aqui",
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

// AUTO (o normal) = PSRAM. Medido no Tab5 (2026-11-12, Wi-Fi e MQTT no ar):
// com um pedaco "quente" de SRAM na frente o 480p ficou igual (4,9 qps) - a
// cache L2 de 256 KB ja atende a arena na PSRAM. E a SRAM que sobra acima do
// piso mal passa de 20 KB: tirar dela so apertaria o Wi-Fi, sem ganho.
// SRAM so na bancada ('video h264 ... sram'), para medir.
static bool reservar_arena(tab5_h264_t* d, tab5_h264_onde_t onde)
{
  if (onde == TAB5_H264_SRAM) {                   // bancada: so SRAM, sem disfarce
    d->na_sram = arena_sram(d, true, H264_PEDACOS);
    d->sram_bytes = d->arena_bytes;
    return d->na_sram;
  }
  // AUTO e PSRAM: um pedaco so, do tamanho da arena inteira.
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
  c.clock = relogio_ciclos;   // so usado com H264_DEC_PROFILE (medicao por etapa)
  d->dec = h264_dec_create(&c);
  if (!d->dec) {
    ESP_LOGW(TAG, "decodificador nao criou: pedacos %u/%u/%u/%u KB, SRAM livre %u KB",
             (unsigned)(d->pedaco_bytes[0] / 1024), (unsigned)(d->pedaco_bytes[1] / 1024),
             (unsigned)(d->pedaco_bytes[2] / 1024), (unsigned)(d->pedaco_bytes[3] / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    soltar_arena(d);
    return false;
  }
  int n = 0;
  while (n < H264_PEDACOS && d->pedaco[n]) { ++n; }
  ESP_LOGI(TAG, "aberto: arena de %u KB (%u KB na SRAM, %d pedaco(s)), ate %dx%d",
           (unsigned)(d->arena_bytes / 1024), (unsigned)(d->sram_bytes / 1024), n, d->cfg.max_l, d->cfg.max_a);
  return true;
}

static void desmontar(tab5_h264_t* d)
{
  if (d->dec) { h264_dec_destroy(d->dec); d->dec = nullptr; }   // espera o trabalhador sair
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
  // A arena hibrida sempre tem o pedaco de PSRAM do tamanho inteiro: falta de
  // memoria aqui e video grande demais de verdade (o erro diz qual).
  return h264_dec_decode(d->dec, dados, n, nal_tam, marca);
}

bool tab5_h264_quadro(tab5_h264_t* d, tab5_h264_quadro_t* q)
{
  h264_dec_picture_t p;
  if (!h264_dec_output(d->dec, &p)) { return false; }   // sai PRESO (h264_dec_release solta)
  d->ja_saiu_quadro = true;
  const h264_dec_stream_info_t& i = p.info;
  q->px = p.packed;
  q->l_cod = i.coded_width; q->a_cod = i.coded_height;
  q->x = i.crop_left; q->y = i.crop_top; q->l = i.width; q->a = i.height;
  // Matriz de cor: o que o arquivo diz; sem dizer, HD e BT.709 e o resto BT.601.
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
// BANCADA
// ---------------------------------------------------------------------------
typedef struct {
  const char*          caminho;
  tab5_h264_onde_t     onde;
  bool                 hash;
  tab5_h264_bancada_t* r;
  SemaphoreHandle_t    feito;
} bancada_t;

// Um "00 00 01" a partir de p (ou o fim).
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
      // O mesmo FNV-1a do teste do Mac: o quadro empacotado inteiro.
      const size_t bytes = (size_t)q.l_cod * 3 / 2 * q.a_cod;
      printf("[H264V] f=%u h=%08lx\n", (unsigned)b->r->quadros, (unsigned long)tab5_fnv1a(q.px, bytes));
    }
    b->r->l = q.l; b->r->a = q.a;
    ++b->r->quadros;
    tab5_h264_soltar(d, q.id);
  }
}

#define BANCADA_MAX  (8u * 1024 * 1024)   // o maximo do arquivo carregado na bancada

static void tarefa_bancada(void* p)
{
  bancada_t* b = (bancada_t*)p;
  tab5_h264_bancada_t* r = b->r;
  uint8_t* dados = nullptr;
  size_t n = 0;
  tab5_h264_t* d = nullptr;

  // 1. O arquivo na PSRAM (a bancada mede so a decodificacao). No maximo
  //    BANCADA_MAX: um arquivo de 18 MB inteiro na PSRAM nao deixava lugar
  //    para os quadros de referencia (o player de verdade le aos poucos).
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
  // Cortado: termina antes do ultimo inicio de NAL (00 00 01), para o ultimo
  // pacote nao ficar pela metade.
  if (n == BANCADA_MAX) {
    while (n > 3 && !(dados[n - 3] == 0 && dados[n - 2] == 0 && dados[n - 1] == 1)) { --n; }
    n = n > 3 ? n - 3 : 0;
  }

  {
    // 2. Decodifica pacote a pacote: um pacote termina onde comeca a primeira
    //    fatia do quadro seguinte (ou um SEI/SPS/PPS/AUD depois de imagem).
    const tab5_h264_cfg_t cfg = { 1280, 1280, b->onde };
    d = tab5_h264_abrir(&cfg);
    if (!d) { snprintf(r->erro, sizeof(r->erro), "out of memory for the decoder"); goto fim; }
    const uint8_t* fim_dados = dados + n;
    const uint8_t* q = proximo_inicio(dados, fim_dados);
    const uint8_t* au = q;
    bool tem_imagem = false;
    uint32_t pacote = 0;
    // O clock no maximo permitido durante a medida: sem esta trava o DFS
    // deixa a CPU no piso (40 MHz em repouso) e a bancada mede o piso.
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
    // Tempo por etapa (ciclos -> ms a cpu_mhz): onde a decodificacao gasta.
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
    // Em segundo plano: ninguem espera - o resultado vai para o log.
    ESP_LOGI(TAG, "bancada: %s%s%lu quadros %dx%d em %lu ms = %.1f quadros/s | arena %lu KB (%lu na SRAM) | CPU %lu MHz",
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
  // A tela acesa durante a medida: em repouso o clock fica limitado (180 MHz)
  // e a bancada mediria o repouso, nao o video (que toca com a tela acesa).
  sistema_recurso_usar(RECURSO_TELA_ACESA, "h264");
  // A mesma prioridade e nucleo que o video usa (nucleo 0, abaixo da Musica).
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
  // Tudo no heap: a tarefa solta ao terminar (ninguem espera por ela).
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
