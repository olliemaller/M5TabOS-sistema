#include "tab5_quadro.hpp"

#include <string.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/ppa.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "soc/mipi_dsi_host_struct.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"   // esp_intr_dump (tab5_quadro_listar_irqs)
#include "riscv/rv_utils.h"   // RV_READ_CSR(mepc)
#include "esp_private/esp_clk.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_async_memcpy.h"
// Private to esp_lcd: the pure DMA2D the DPI driver itself uses. The header
// has no extern "C" (it is only included from C inside IDF).
extern "C" {
#include "esp_async_fbcpy.h"
}

static const char *TAG = "quadro";

// Physical dimensions of the Tab5 panel (Panel_ST7123: portrait, RGB565).
static const uint32_t FB_L = 720;
static const uint32_t FB_A = 1280;
static const uint32_t FB_BYTES = FB_L * FB_A * 2;

static esp_lcd_panel_handle_t s_painel = nullptr;
static SemaphoreHandle_t      s_troca  = nullptr;   // given at every end of frame
// THE COPY BETWEEN PAGES is done by PURE DMA2D (esp_async_fbcpy), not the PPA.
// Measured on the device with 'tela banco', panel scanning, 720x1024 RGB565:
//   PPA (motor SRM)  32,1 ms   92 MB/s
//   GDMA AXI         14,3 ms  206 MB/s
//   DMA2D puro       10,2 ms  288 MB/s
//   CPU (memcpy)     25,0 ms  118 MB/s
// The SRM exists to scale and rotate: it walks in macroblocks and reads PSRAM
// in short bursts. The DMA2D reads whole lines. It is the same engine the DPI
// driver uses when 'use_dma2d' is requested. The PPA is only a fallback, if
// the DMA2D does not come up.
static esp_async_fbcpy_handle_t s_dma2d = nullptr;
static SemaphoreHandle_t        s_dma2d_fim = nullptr;
static ppa_client_handle_t      s_ppa   = nullptr;
static std::atomic<uint32_t>      s_quadros = 0;

// ---------------------------------------------------------------------------
// UNDERRUN COUNTER
// ---------------------------------------------------------------------------
// The driver does not expose the event: its ISR clears the bit and just
// prints, with esp_rom_printf, straight from the interrupt. That ROM printf
// writes to BOTH its output channels - 1 (the console) and 2, which boot
// leaves empty (cpu_start.c: esp_rom_install_channel_putc(2, NULL)). Hanging
// a "reader" on channel 2 we see every character the ROM prints without
// chaining or replacing the console. It looks for the word "underrun" and
// counts.
//
// Runs inside an ISR: IRAM, no calls at all, just character comparison.
static const char     PALAVRA[] = "underrun";
static std::atomic<uint8_t>  s_casou = 0;
static std::atomic<uint32_t> s_underruns = 0;

static void IRAM_ATTR vigia_canal2(char c)
{
  if (c == PALAVRA[s_casou]) {
    ++s_casou;
    if (s_casou == sizeof(PALAVRA) - 1) { ++s_underruns; s_casou = 0; }
  } else {
    s_casou = (c == PALAVRA[0]) ? 1 : 0;
  }
}

// ---------------------------------------------------------------------------
// END OF FRAME
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// DETECTORS THAT DO NOT DEPEND ON THE DRIVER'S LOG
// ---------------------------------------------------------------------------
// Blue flashes were seen with the naked eye WITHOUT any "underrun" in the log,
// even with the bridge mask cleared. That leaves three places where the image
// can fail without the bridge's underrun interrupt firing:
//
// 1) FRAME CLOCK. The panel DMA is NOT circular: this ISR (end of frame) is
//    what restarts it for the next frame (mipi_dsi_dma_trans_done_cb). If it
//    is late - interrupts disabled too long on that core - the next frame
//    starts with no data. We measure the interval between frame ends: the
//    smallest seen is the real period; an interval 50% longer is a frame
//    that was late. In CPU cycles (the lit panel holds CPU_FREQ_MAX, so the
//    clock does not change while it is up).
//
// 2) DSI HOST ERRORS (the Synopsys controller after the bridge). INT_ST0
//    carries physical-layer errors and error ACKs from the panel; INT_ST1
//    carries the host's own - including dpi_buff_pld_under (bit 19), the lack
//    of data in the HOST's buffer, which the bridge does not see. Both
//    registers clear on read and nobody else reads them (the IDF driver does
//    not use the host interrupts). Read HERE, in the ISR: the ISR only exists
//    with DSI on, so a powered-off peripheral is never read, and there is no
//    race with idle tearing the panel down.
//
// 3) THE UNDERRUN COLOUR - see tab5_quadro_iniciar.
static volatile int64_t  s_ult_us      = 0;   // in us (esp_timer), NOT cycles - see the ISR
static volatile uint32_t s_periodo_min = UINT32_MAX;   // us
static std::atomic<uint32_t> s_atrasos     = 0;            // since boot
static volatile uint32_t s_pior_atraso = 0;            // us, since the last read
static volatile uint32_t s_dsi_st0     = 0;            // OR accumulated since the last read
static volatile uint32_t s_dsi_st1     = 0;
static std::atomic<uint32_t> s_dsi_erros   = 0;
static volatile bool     s_primeiro_quadro = true;
// WHO WAS RUNNING when the late frame was finally serviced. The ISR
// interrupts precisely whoever held the core (interrupts disabled, a long
// critical section) - its name is the best clue. Both cores, because a cross
// wait (kernel lock, flash operation) holds the other one too.
static char              s_atr_t[2][16];
static volatile int      s_atr_nucleo = -1;   // core the ISR runs on (-1 = no delay yet)
static std::atomic<uint32_t> s_atr_seq = 0;       // incremented on every new record
static volatile int64_t  s_atr_us  = 0;       // uptime (us) at which the late frame was serviced
// WHERE the core was when the late ISR finally got in (mepc: the interrupt's
// return address). Inside IDLE = another ISR held the core before; inside
// vPortExitCritical/esp_cache/esp_pm = a critical section, and the address
// says which (addr2line on build/M5tabOS.elf).
static volatile uint32_t s_atr_pc  = 0;            // frames with some bit set, since boot

static bool IRAM_ATTR ao_fim_do_quadro(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*)
{
  ++s_quadros;   // single writer: this ISR

  // IN MICROSECONDS, NOT CYCLES. Cycles count at the CPU clock rate, and the
  // clock changes (360 -> 40 on idle): the monitor divided by the CURRENT
  // clock and a 30 ms delay came out as "273.9 ms (normal 138.8)" - exactly
  // 9x. esp_timer does not depend on the CPU clock.
  const int64_t agora = esp_timer_get_time();
  if (s_ult_us != 0) {
    const uint32_t intervalo = (uint32_t)(agora - s_ult_us);
    // Below 3 ms it is not a real frame (no panel here goes beyond ~70 Hz):
    // ignore it, so as not to poison the minimum period.
    if (intervalo < 3000u) {
    } else if (intervalo < s_periodo_min) {
      s_periodo_min = intervalo;
    } else if (intervalo > s_periodo_min + s_periodo_min / 2) {
      ++s_atrasos;
      if (intervalo > s_pior_atraso) { s_pior_atraso = intervalo; }
      for (int c = 0; c < 2; ++c) {
        const char* nm = pcTaskGetName(xTaskGetCurrentTaskHandleForCore(c));
        int k = 0;
        for (; nm && nm[k] && k < 15; ++k) { s_atr_t[c][k] = nm[k]; }
        s_atr_t[c][k] = 0;
      }
      s_atr_nucleo = xPortGetCoreID();
      s_atr_pc = (uint32_t)RV_READ_CSR(mepc);
      s_atr_us = agora;
      ++s_atr_seq;
    }
  }
  s_ult_us = agora;

  const uint32_t st0 = MIPI_DSI_HOST.int_st0.val;
  const uint32_t st1 = MIPI_DSI_HOST.int_st1.val;
  // Each panel's FIRST FRAME only clears: the registers hold what happened
  // during the init sequence (LP commands). On the device, every new panel
  // brought st0=0x40 (ACK bit 6 = "false control error" observed by the panel
  // during init) - always one, always there, never afterwards.
  if (s_primeiro_quadro) { s_primeiro_quadro = false; }
  else if (st0 | st1) { s_dsi_st0 |= st0; s_dsi_st1 |= st1; ++s_dsi_erros; }

  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR(s_troca, &acordou);
  return acordou == pdTRUE;
}

bool tab5_quadro_iniciar(esp_lcd_panel_handle_t painel)
{
  if (painel == nullptr) { return false; }
  if (painel == s_painel) { return true; }

  if (s_troca == nullptr) {
    // Binary and not counting: all that matters is "has there been an end
    // of frame since I armed", never how many.
    s_troca = xSemaphoreCreateBinary();
    esp_rom_install_channel_putc(2, vigia_canal2);
    // Self-test: the same function the driver's ISR uses. If the watcher
    // does not count this, a zero underrun count means nothing.
    esp_rom_printf("frame: underrun detector test (expected line, not an error)\n");
    if (s_underruns == 1) { s_underruns = 0; ESP_LOGI(TAG, "flash (underrun) detector tested and working"); }
    else { ESP_LOGE(TAG, "flash (underrun) detector does NOT work - the underrun counter is meaningless"); }
  }
  if (s_dma2d == nullptr && s_ppa == nullptr) {
    esp_async_fbcpy_config_t fc = { };
    s_dma2d_fim = xSemaphoreCreateBinary();
    if (s_dma2d_fim == nullptr || esp_async_fbcpy_install(&fc, &s_dma2d) != ESP_OK) {
      ESP_LOGW(TAG, "DMA2D unavailable - copying between pages via the PPA (~3x slower)");
      s_dma2d = nullptr;
    }
  }
  if (s_dma2d == nullptr && s_ppa == nullptr) {
    ppa_client_config_t cfg = { };
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;   // every copy is blocking
    if (ppa_register_client(&cfg, &s_ppa) != ESP_OK) {
      ESP_LOGE(TAG, "PPA unavailable - copying between pages disabled");
      s_ppa = nullptr;
    }
  }

  esp_lcd_dpi_panel_event_callbacks_t cbs = { };
  cbs.on_frame_buf_complete = ao_fim_do_quadro;
  if (esp_lcd_dpi_panel_register_event_callbacks(painel, &cbs, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "could not register the end-of-frame callback");
    return false;
  }
  // UNDERRUN UNMASKED. The IDF driver programs the DSI bridge NOT to
  // interrupt on underrun while the line count is below h_size
  // (esp_lcd_panel_dpi.c:343 - on the Tab5, 720 of 1280 lines). Result: lack
  // of data in the TOP part of the screen turned the image blue SILENTLY -
  // without the driver's log and without our watcher. That is how a blue
  // flash seen with the naked eye in the Energia app left no trace at all.
  // Clearing it, every underrun becomes an interrupt, a log and a monitor event.
  mipi_dsi_brg_ll_set_underrun_discard_count(&MIPI_DSI_BRIDGE, 0);

  // THE UNDERRUN COLOUR. The BLUE of the flash has an owner: the bridge's
  // DPI_RSV_DPI_DATA register, "the pixel sent to the host when the bridge
  // FIFO runs dry", whose default 0x3FFF (30 bits, B in the low 10) is pure
  // blue. We change it to MAGENTA, which no system screen uses over a large area:
  //   - flashed MAGENTA -> it is the bridge without data, even if the
  //                        interrupt does not say so;
  //   - flashed BLUE    -> it is NOT the bridge: the blue comes from
  //                        elsewhere (host, panel), and the investigation
  //                        moves to another layer.
  // It is diagnostic: once the cause is found, this colour gets decided again.
  MIPI_DSI_BRIDGE.dpi_rsv_dpi_data.dpi_rsv_data = (0x3FFu << 20) | 0x3FFu;
  mipi_dsi_brg_ll_update_dpi_config(&MIPI_DSI_BRIDGE);

  // The frame clock restarts: a new panel may have a different period.
  s_ult_us      = 0;
  s_periodo_min = UINT32_MAX;
  s_primeiro_quadro = true;

  s_painel = painel;
  ESP_LOGI(TAG, "panel frame counter up");
  return true;
}

// Idle destroys the panel and creates another - and the new one may be born
// at the SAME address as the old. Without forgetting here, iniciar() would
// think it had already registered and the end-of-frame callback would never
// arrive again (found as: "painel 0 Hz" after the first idle).
void tab5_quadro_encerrar(void) { s_painel = nullptr; }

// WHY ARM AFTER REQUESTING THE SWAP, AND NOT BEFORE.
//
// The swap request only changes an index; what reads it is the DMA ISR, at
// the end of the frame. If I cleared the semaphore BEFORE the request, an end
// of frame landing between the clear and the request would give the
// semaphore with the OLD page still being scanned - and I would write to it
// thinking it was free. Clearing AFTERWARDS, any signal I pick up is from a
// restart that has already read the new index. The worst case costs one extra
// frame of waiting; never a tear.
void tab5_quadro_armar_espera(void)
{
  if (s_troca) { xSemaphoreTake(s_troca, 0); }
}

bool tab5_quadro_esperar_troca(uint32_t prazo_ms)
{
  if (s_troca == nullptr) { return false; }
  return xSemaphoreTake(s_troca, pdMS_TO_TICKS(prazo_ms)) == pdTRUE;
}

// Framebuffer to framebuffer (or virtual screen to framebuffer - all three are
// 720x1280 RGB565): the rectangle goes to the SAME place in the destination.
// Blocking: the compositor only requests the page swap with everything copied.
static bool IRAM_ATTR dma2d_terminou(esp_async_fbcpy_handle_t, esp_async_fbcpy_event_data_t*, void* arg)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &acordou);
  return acordou == pdTRUE;
}

// THE CACHE, which the DMA cannot see. Whole lines of the rectangle:
//  - SOURCE: write back to memory what the CPU wrote that is still only in
//    the cache;
//  - DESTINATION: write back AND invalidate, so the CPU does not later read a
//    stale copy left in the cache. "Invalidate only" (M2C) requires edges on
//    the 128 B cache line, and an arbitrary rectangle does not fall on them -
//    IDF refuses (that was the error in the first 'tela banco'). Writing back
//    before invalidating is safe with any edge: nothing dirty is lost.
static void cache_das_linhas(const void* base, const tab5_ret_t& r, int flags)
{
  uint8_t* p = (uint8_t*)base + (size_t)r.y * FB_L * 2;
  esp_cache_msync(p, (size_t)r.a * FB_L * 2, flags | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool copiar_dma2d(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy)
{
  esp_async_fbcpy_trans_desc_t t = { };
  t.src_buffer = origem;
  t.dst_buffer = destino;
  t.src_buffer_size_x = FB_L; t.src_buffer_size_y = FB_A;
  t.dst_buffer_size_x = FB_L; t.dst_buffer_size_y = FB_A;
  t.src_offset_x = ox;  t.src_offset_y = oy;
  t.dst_offset_x = r.x; t.dst_offset_y = r.y;
  t.copy_size_x = r.l;  t.copy_size_y = r.a;
  t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565);

  const tab5_ret_t ro = { (int16_t)ox, (int16_t)oy, r.l, r.a };
  cache_das_linhas(origem,  ro, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  cache_das_linhas(destino, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  xSemaphoreTake(s_dma2d_fim, 0);   // no stale signal counts as this one
  if (esp_async_fbcpy(s_dma2d, &t, dma2d_terminou, s_dma2d_fim) != ESP_OK) {
    ESP_LOGE(TAG, "copy %dx%d at (%d,%d) refused by the DMA2D", r.l, r.a, r.x, r.y);
    return false;
  }
  // A full screen takes ~13 ms; 100 ms is just the net against a hung DMA.
  if (xSemaphoreTake(s_dma2d_fim, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGE(TAG, "copy %dx%d at (%d,%d) did not finish within 100 ms", r.l, r.a, r.x, r.y);
    return false;
  }
  return true;
}

bool tab5_quadro_copiar_de(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy)
{
  if (destino == nullptr || origem == nullptr || destino == origem) { return false; }
  if (r.l <= 0 || r.a <= 0) { return true; }
  if (ox < 0 || oy < 0 || ox + r.l > (int)FB_L || oy + r.a > (int)FB_A) { return false; }
  if (s_dma2d) { return copiar_dma2d(destino, r, origem, ox, oy); }
  // The fallback PPA only handles the no-offset case.
  if (ox != r.x || oy != r.y) { return false; }
  return tab5_quadro_copiar(destino, origem, r);
}

bool tab5_quadro_copiar(void* destino, const void* origem, const tab5_ret_t& r)
{
  if (destino == nullptr || origem == nullptr) { return false; }
  if (r.l <= 0 || r.a <= 0) { return true; }
  if (s_dma2d) { return copiar_dma2d(destino, r, origem, r.x, r.y); }
  if (s_ppa == nullptr) { return false; }

  ppa_srm_oper_config_t op = { };
  op.in.buffer         = origem;
  op.in.pic_w          = FB_L;
  op.in.pic_h          = FB_A;
  op.in.block_w        = r.l;
  op.in.block_h        = r.a;
  op.in.block_offset_x = r.x;
  op.in.block_offset_y = r.y;
  op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

  op.out.buffer         = destino;
  op.out.buffer_size    = FB_BYTES;
  op.out.pic_w          = FB_L;
  op.out.pic_h          = FB_A;
  op.out.block_offset_x = r.x;
  op.out.block_offset_y = r.y;
  op.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  op.scale_x = 1.0f;
  op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;

  const esp_err_t e = ppa_do_scale_rotate_mirror(s_ppa, &op);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "copy %dx%d at (%d,%d) failed: %s", r.l, r.a, r.x, r.y, esp_err_to_name(e));
    return false;
  }
  return true;
}

uint32_t tab5_quadro_underruns(void) { return s_underruns; }

uint32_t tab5_quadro_atrasos(uint32_t* pior_us, uint32_t* periodo_us)
{
  if (pior_us)    { *pior_us = s_pior_atraso; s_pior_atraso = 0; }
  if (periodo_us) { *periodo_us = (s_periodo_min != UINT32_MAX) ? s_periodo_min : 0; }
  return s_atrasos;
}

int64_t tab5_quadro_atraso_quando(void) { return s_atr_us; }
uint32_t tab5_quadro_atraso_pc(void) { return s_atr_pc; }

void tab5_quadro_listar_irqs(void) { esp_intr_dump(stdout); }

// DELAY PROOF: holds core 'nucleo' for 'ms', either with interrupts disabled
// (portDISABLE_INTERRUPTS: nothing gets in, like a long high-level ISR) or in
// a critical section (portENTER_CRITICAL: masks levels 1-3 - those of the
// ordinary ISRs, the panel's included). 'vezes' repetitions with 1 s between
// them, to give time to watch the screen. If the screen flashes, the panel
// ISR delay is the cause of the flash; if not, the cause is something else.
struct prova_t { int ms, vezes; bool critica; SemaphoreHandle_t fim; };
static void tarefa_prova(void* arg)
{
  prova_t* p = (prova_t*)arg;
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  for (int i = 0; i < p->vezes; ++i) {
    if (p->critica) {
      portENTER_CRITICAL(&mux);
      esp_rom_delay_us((uint32_t)p->ms * 1000u);
      portEXIT_CRITICAL(&mux);
    } else {
      portDISABLE_INTERRUPTS();
      esp_rom_delay_us((uint32_t)p->ms * 1000u);
      portENABLE_INTERRUPTS();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  xSemaphoreGive(p->fim);
  vTaskDelete(nullptr);
}

bool tab5_quadro_prova_atraso(int ms, bool critica, int nucleo, int vezes)
{
  // 100 ms ceiling: the interrupt watchdog (800 ms) stays well away.
  if (ms < 1 || ms > 100 || nucleo < 0 || nucleo > 1 || vezes < 1 || vezes > 20) { return false; }
  prova_t p = { ms, vezes, critica, xSemaphoreCreateBinary() };
  if (!p.fim) { return false; }
  // Stack in internal RAM (the task disables interrupts: no PSRAM).
  if (xTaskCreatePinnedToCore(tarefa_prova, "prova", 2048, &p, 5, nullptr, nucleo) != pdPASS) {
    vSemaphoreDelete(p.fim);
    return false;
  }
  xSemaphoreTake(p.fim, portMAX_DELAY);
  vSemaphoreDelete(p.fim);
  return true;
}

int64_t tab5_quadro_ultimo_fim_us(void) { return s_ult_us; }

uint32_t tab5_quadro_atraso_quem(int* nucleo_isr, char* t0, char* t1)
{
  if (nucleo_isr) { *nucleo_isr = s_atr_nucleo; }
  if (t0) { memcpy(t0, s_atr_t[0], 16); t0[15] = 0; }
  if (t1) { memcpy(t1, s_atr_t[1], 16); t1[15] = 0; }
  return s_atr_seq;
}

uint32_t tab5_quadro_erros_dsi(uint32_t* st0, uint32_t* st1)
{
  if (st0) { *st0 = s_dsi_st0; s_dsi_st0 = 0; }
  if (st1) { *st1 = s_dsi_st1; s_dsi_st1 = 0; }
  return s_dsi_erros;
}
uint32_t tab5_quadro_quadros(void)   { return s_quadros; }

// Peeks at the ISR counter every tick (1 ms): notices the end of frame at
// most 1 ms later - plenty of slack before the next one (~15 ms).
bool tab5_quadro_esperar_fim(uint32_t prazo_ms)
{
  const uint32_t q0 = s_quadros;
  const int64_t limite = esp_timer_get_time() + (int64_t)prazo_ms * 1000;
  while (s_quadros == q0) {
    if (esp_timer_get_time() >= limite) { return false; }
    vTaskDelay(1);
  }
  return true;
}

// ---------------------------------------------------------------------------
// HARDWARE FILL
// ---------------------------------------------------------------------------
// The per-phase measurement of the full pass showed "clear" costing 20-25 ms
// in EVERY app: the CPU writing 1.6 MB of black. The PPA has a write-only fill
// engine - it reads nothing, just sends the colour via the DMA2D.
//
// The cache is the driver's job: before writing, it writes back AND
// invalidates the destination lines (ppa_fill.c, C2M|INVALIDATE) - whatever
// the CPU drew alongside, on the same lines, goes to memory first and
// nothing is lost.
static ppa_client_handle_t s_ppa_fill = nullptr;
static int8_t              s_fill_ok  = -1;   // -1 not checked, 0 disabled, 1 checked

bool tab5_quadro_preencher(void* destino, const tab5_ret_t& r, uint16_t cor)
{
  if (s_fill_ok == 0 || destino == nullptr || r.l <= 0 || r.a <= 0) { return false; }
  if (s_ppa_fill == nullptr) {
    ppa_client_config_t cfg = { };
    cfg.oper_type = PPA_OPERATION_FILL;
    cfg.max_pending_trans_num = 1;
    if (ppa_register_client(&cfg, &s_ppa_fill) != ESP_OK) {
      ESP_LOGW(TAG, "fill PPA unavailable - clearing by CPU");
      s_ppa_fill = nullptr;
      s_fill_ok = 0;
      return false;
    }
  }

  ppa_fill_oper_config_t op = { };
  op.out.buffer         = destino;
  op.out.buffer_size    = FB_BYTES;
  op.out.pic_w          = FB_L;
  op.out.pic_h          = FB_A;
  op.out.block_offset_x = r.x;
  op.out.block_offset_y = r.y;
  op.out.fill_cm        = PPA_FILL_COLOR_MODE_RGB565;
  op.fill_block_w       = r.l;
  op.fill_block_h       = r.a;
  // RGB565 -> ARGB8888 repeating the high bits in the low ones: the PPA cuts
  // back to 565 and arrives at exactly the same colour.
  const uint8_t r5 = (cor >> 11) & 0x1F, g6 = (cor >> 5) & 0x3F, b5 = cor & 0x1F;
  op.fill_argb_color.a = 0xFF;
  op.fill_argb_color.r = (uint8_t)((r5 << 3) | (r5 >> 2));
  op.fill_argb_color.g = (uint8_t)((g6 << 2) | (g6 >> 4));
  op.fill_argb_color.b = (uint8_t)((b5 << 3) | (b5 >> 2));
  op.mode = PPA_TRANS_MODE_BLOCKING;

  if (ppa_do_fill(s_ppa_fill, &op) != ESP_OK) { return false; }

  // Only a colour with its two bytes DIFFERENT reveals swapped byte order -
  // black (0x0000), which is almost every clear, would pass either way.
  if (s_fill_ok < 0 && (cor >> 8) != (cor & 0xFF)) {
    // ONE-OFF CHECK. The driver invalidated those lines' cache before the
    // DMA and the CPU has not touched them since: this read comes from memory.
    const uint16_t lido = ((const uint16_t*)destino)[(size_t)r.y * FB_L + r.x];
    if (lido != cor) {
      ESP_LOGE(TAG, "PPA fill wrote %04x instead of %04x - disabled, clearing by CPU",
               (unsigned)lido, (unsigned)cor);
      s_fill_ok = 0;
      return false;   // the caller paints over it by CPU
    }
    s_fill_ok = 1;
    ESP_LOGI(TAG, "hardware screen clearing (PPA) tested and working");
  }
  return true;
}

// ---------------------------------------------------------------------------
// COPY TEST BENCH
// ---------------------------------------------------------------------------
// The compositor measurement showed the PPA taking ~88 ms per frame
// (~32 MB/s counting read and write). The PPA's SRM engine is made to SCALE
// and ROTATE: it works in 16/32-pixel macroblocks, and on PSRAM that becomes
// short 32-64 byte bursts jumping from line to line. This bench puts the PPA
// side by side with engines that copy whole lines, on the same rectangle,
// with the panel scanning - no datasheet numbers.
namespace {

struct banco_t {
  uint16_t* src = nullptr;
  uint16_t* dst = nullptr;
  SemaphoreHandle_t fim = nullptr;
};

double mbps(uint32_t px, int64_t us) { return us > 0 ? (double)px * 4.0 / (double)us : 0.0; }

bool IRAM_ATTR fbcpy_fim(esp_async_fbcpy_handle_t, esp_async_fbcpy_event_data_t*, void* arg)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &acordou);
  return acordou == pdTRUE;
}

bool IRAM_ATTR gdma_fim(async_memcpy_handle_t, async_memcpy_event_t*, void* arg)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &acordou);
  return acordou == pdTRUE;
}

// L2 cache line = 128 B: the DMA needs the destination edges on it.
void sincronizar_linhas(void* base, const tab5_ret_t& r, int dir)
{
  uint8_t* p = (uint8_t*)base + (size_t)r.y * FB_L * 2;
  esp_cache_msync(p, (size_t)r.a * FB_L * 2, dir | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

int64_t por_ppa(banco_t& b, ppa_client_handle_t cli, const tab5_ret_t& r)
{
  ppa_srm_oper_config_t op = { };
  op.in.buffer = b.src; op.in.pic_w = FB_L; op.in.pic_h = FB_A;
  op.in.block_w = r.l; op.in.block_h = r.a; op.in.block_offset_x = r.x; op.in.block_offset_y = r.y;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = b.dst; op.out.buffer_size = FB_BYTES; op.out.pic_w = FB_L; op.out.pic_h = FB_A;
  op.out.block_offset_x = r.x; op.out.block_offset_y = r.y; op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0; op.scale_x = 1.0f; op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  const int64_t t0 = esp_timer_get_time();
  if (ppa_do_scale_rotate_mirror(cli, &op) != ESP_OK) { return -1; }
  return esp_timer_get_time() - t0;
}

int64_t por_dma2d(banco_t& b, esp_async_fbcpy_handle_t h, const tab5_ret_t& r)
{
  esp_async_fbcpy_trans_desc_t t = { };
  t.src_buffer = b.src; t.dst_buffer = b.dst;
  t.src_buffer_size_x = FB_L; t.src_buffer_size_y = FB_A;
  t.dst_buffer_size_x = FB_L; t.dst_buffer_size_y = FB_A;
  t.src_offset_x = r.x; t.src_offset_y = r.y; t.dst_offset_x = r.x; t.dst_offset_y = r.y;
  t.copy_size_x = r.l; t.copy_size_y = r.a;
  t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565);
  const int64_t t0 = esp_timer_get_time();
  sincronizar_linhas(b.src, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  sincronizar_linhas(b.dst, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (esp_async_fbcpy(h, &t, fbcpy_fim, b.fim) != ESP_OK) { return -1; }
  if (xSemaphoreTake(b.fim, pdMS_TO_TICKS(500)) != pdTRUE) { return -2; }
  return esp_timer_get_time() - t0;
}

int64_t por_gdma(banco_t& b, async_memcpy_handle_t h, const tab5_ret_t& r)
{
  // Full-width rectangles only: then it is a CONTIGUOUS stretch of memory.
  uint8_t* s = (uint8_t*)b.src + (size_t)r.y * FB_L * 2;
  uint8_t* d = (uint8_t*)b.dst + (size_t)r.y * FB_L * 2;
  const size_t n = (size_t)r.a * FB_L * 2;
  const int64_t t0 = esp_timer_get_time();
  if (esp_async_memcpy(h, d, s, n, gdma_fim, b.fim) != ESP_OK) { return -1; }
  if (xSemaphoreTake(b.fim, pdMS_TO_TICKS(500)) != pdTRUE) { return -2; }
  return esp_timer_get_time() - t0;
}

int64_t por_cpu(banco_t& b, const tab5_ret_t& r)
{
  const int64_t t0 = esp_timer_get_time();
  const size_t lb = (size_t)r.l * 2;
  for (int y = r.y; y < r.y + r.a; ++y) {
    memcpy(b.dst + (size_t)y * FB_L + r.x, b.src + (size_t)y * FB_L + r.x, lb);
  }
  // The panel (DMA) reads from memory, not the cache: a CPU write only
  // counts once written back. It goes into the total.
  sincronizar_linhas(b.dst, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  return esp_timer_get_time() - t0;
}

void mostrar(const char* motor, const char* qual, uint32_t px, int64_t us)
{
  if (us < 0) { printf("  %-8s %-14s : failed (%lld)\n", motor, qual, (long long)us); return; }
  printf("  %-8s %-14s : %7.2f ms  %6.1f MB/s\n", motor, qual, (double)us / 1000.0, mbps(px, us));
}

} // namespace

void tab5_quadro_banco(void)
{
  banco_t b;
  b.src = (uint16_t*)heap_caps_aligned_calloc(128, 1, FB_BYTES, MALLOC_CAP_SPIRAM);
  b.dst = (uint16_t*)heap_caps_aligned_calloc(128, 1, FB_BYTES, MALLOC_CAP_SPIRAM);
  b.fim = xSemaphoreCreateBinary();
  if (!b.src || !b.dst || !b.fim) {
    printf("bench: out of memory\n");
  } else {
    for (uint32_t i = 0; i < FB_L * FB_A; ++i) { b.src[i] = (uint16_t)(i * 2654435761u >> 16); }
    esp_cache_msync(b.src, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // OUR OWN client: the compositor's stays free for the UI.
    ppa_client_handle_t cli = nullptr;
    ppa_client_config_t pc = { };
    pc.oper_type = PPA_OPERATION_SRM;
    pc.max_pending_trans_num = 1;
    if (ppa_register_client(&pc, &cli) != ESP_OK) { cli = nullptr; }

    esp_async_fbcpy_handle_t fb = nullptr;
    esp_async_fbcpy_config_t fc = { };
    if (esp_async_fbcpy_install(&fc, &fb) != ESP_OK) { fb = nullptr; }

    async_memcpy_handle_t gd = nullptr;
    async_memcpy_config_t gc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    gc.backlog = 4;
    gc.dma_burst_size = 64;
    if (esp_async_memcpy_install_gdma_axi(&gc, &gd) != ESP_OK) { gd = nullptr; }

    // Full width (the scrolling and full-screen case) and half width (the
    // common dirty-area case, with a line stride).
    const tab5_ret_t cheio = { 0, 128, (int16_t)FB_L, 1024 };
    const tab5_ret_t meio  = { 0, 128, (int16_t)(FB_L / 2), 1024 };
    const uint32_t px_cheio = (uint32_t)cheio.l * cheio.a;
    const uint32_t px_meio  = (uint32_t)meio.l * meio.a;
    const int REP = 3;

    printf("--- copy bench (PSRAM -> PSRAM, panel scanning) ---\n");
    auto rodar = [&](const char* motor, const char* qual, uint32_t px, auto f) {
      int64_t soma = 0;
      for (int i = 0; i < REP; ++i) {
        const int64_t us = f();
        if (us < 0) { mostrar(motor, qual, px, us); return; }
        soma += us;
      }
      mostrar(motor, qual, px, soma / REP);
    };
    if (cli) {
      rodar("PPA", "720x1024", px_cheio, [&] { return por_ppa(b, cli, cheio); });
      rodar("PPA", "360x1024", px_meio,  [&] { return por_ppa(b, cli, meio); });
    }
    if (fb) {
      rodar("DMA2D", "720x1024", px_cheio, [&] { return por_dma2d(b, fb, cheio); });
      rodar("DMA2D", "360x1024", px_meio,  [&] { return por_dma2d(b, fb, meio); });
    }
    if (gd) {
      rodar("GDMA", "720x1024", px_cheio, [&] { return por_gdma(b, gd, cheio); });
    }
    // FILL: write only (2 B per pixel - the MB/s below counts x4 like the
    // copies, so compare the ms).
    rodar("PPA-fill", "720x1024", px_cheio, [&] {
      const int64_t t0 = esp_timer_get_time();
      if (!tab5_quadro_preencher(b.dst, cheio, 0x0000)) { return (int64_t)-1; }
      return esp_timer_get_time() - t0;
    });
    rodar("CPU-fill", "720x1024", px_cheio, [&] {
      const int64_t t0 = esp_timer_get_time();
      for (int y = cheio.y; y < cheio.y + cheio.a; ++y) { memset(b.dst + (size_t)y * FB_L, 0, FB_L * 2); }
      sincronizar_linhas(b.dst, cheio, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      return esp_timer_get_time() - t0;
    });
    rodar("CPU", "720x1024", px_cheio, [&] { return por_cpu(b, cheio); });
    rodar("CPU", "360x1024", px_meio,  [&] { return por_cpu(b, meio); });

    // Did the copy come out right? A cheap check: the last engine was the
    // CPU, and the CPU does not err - check the DMA2D separately.
    if (fb) {
      memset(b.dst, 0, FB_BYTES);
      esp_cache_msync(b.dst, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      por_dma2d(b, fb, cheio);
      const bool ok = memcmp(b.dst + (size_t)cheio.y * FB_L, b.src + (size_t)cheio.y * FB_L,
                             (size_t)cheio.a * FB_L * 2) == 0;
      printf("  DMA2D copiou certo        : %s\n", ok ? "sim" : "NAO");
    }

    if (gd)  { esp_async_memcpy_uninstall(gd); }
    if (fb)  { esp_async_fbcpy_uninstall(fb); }
    if (cli) { ppa_unregister_client(cli); }
  }
  if (b.fim) { vSemaphoreDelete(b.fim); }
  heap_caps_free(b.src);
  heap_caps_free(b.dst);
}
