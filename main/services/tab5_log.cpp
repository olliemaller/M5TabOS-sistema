#include "tab5_log.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include "tab5_sd.hpp"
#include "tab5_hora.hpp"   // tab5_hora_nome_do_dia
#include "tab5_heap_psram.hpp"   // clean restarts from the boot check

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"   // xStreamBufferCreateWithCaps
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"   // esp_reset_reason
#include "esp_core_dump.h"
#include "esp_rom_sys.h"    // esp_rom_get_reset_reason: the chip's raw reason
#include "esp_app_desc.h"   // esp_app_get_elf_sha256: is the core dump from this firmware?
#include "esp_heap_caps.h"
#include <time.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <atomic>

static const char* TAG = "log";

// 12KB - a few dozen log lines in a system with several services
// talking at the same time (GPS, Mesh, camera, UI) fit comfortably
// between two FLUSH_MS flushes. A single fixed block, created once at
// boot and never reallocated - doesn't fragment. It lives in PSRAM: a fixed 12KB in
// internal SRAM comes out of everyone else's stacks and DMA buffers. That's allowed, because
// nobody writes here with the cache disabled - the hook doesn't run in an ISR, and the
// flash-writing code logs through the ROM (ESP_EARLY_LOG), not through this
// vprintf.
#define LOG_BUF_TAM    (12 * 1024)

// How often the log (in the "cartao" task) flushes the buffer to the SD card,
// for INFORMATIONAL lines. Warnings and errors don't wait (see log_na_carona). It was 15s
// and the "sd: montado/desmontado" (mounted/unmounted) lines from each flush
// fed back into the buffer, mounting the SD card every 15s forever.
#define LOG_FLUSH_MS   60000   // was 15000 - see "EM LOTE" (IN BATCHES) in log_na_carona

// Each call to ESP_LOG's vprintf is ONE whole line already formatted (tag,
// level, message, millisecond counter, line break - esp_log_writev itself
// builds that string and only then calls this function). 200 bytes
// covers any normal line in this system; a longer line is cut,
// it doesn't overflow - see the vsnprintf below.
#define LOG_LINHA_TAM  200

static StreamBufferHandle_t s_buf      = nullptr;
static volatile bool        s_urgente  = false;   // a warning/error arrived: flush now
static vprintf_like_t       s_original = nullptr;

// ONE WRITER AT A TIME. The FreeRTOS stream buffer is made for
// ONE writer and ONE reader - the documentation says to serialise when there is more than
// one. And here there are many: every task that calls ESP_LOG writes (UI,
// WiFi, ESP-NOW, Mesh, esp_hosted's receive task...), and IDF's esp_log (version
// 1, ours) does NOT lock anything before calling this function. Two lines at the same
// time could scramble the buffer's index. A short wait, never infinite: if
// the lock doesn't come within 5 ms, the line goes to the console and just doesn't go to the SD card
// (counted as lost) - logging must never stall whoever is logging.
static SemaphoreHandle_t    s_escrita  = nullptr;

static uint32_t s_linhas_pendentes = 0;   // queued, not yet written
static uint32_t s_linhas_gravadas  = 0;   // confirmed on the SD card
static uint32_t s_linhas_perdidas  = 0;   // buffer full, line discarded
static char     s_arquivo_atual[24] = "";
static bool     s_boot_marcado     = false;

void tab5_log_estado(size_t* bytes_pendentes, uint32_t* linhas_gravadas,
                      uint32_t* linhas_perdidas, char* arquivo, size_t arquivo_tam)
{
  if (bytes_pendentes) { *bytes_pendentes = s_buf ? xStreamBufferBytesAvailable(s_buf) : 0; }
  if (linhas_gravadas) { *linhas_gravadas = s_linhas_gravadas; }
  if (linhas_perdidas) { *linhas_perdidas = s_linhas_perdidas; }
  if (arquivo)          { snprintf(arquivo, arquivo_tam, "%s", s_arquivo_atual); }
}

// THE FUNCTION THAT REPLACES ESP_LOG'S vprintf - see the big notice in the .hpp
// on why it only FORMATS AND QUEUES, never writes to the SD card here.
//
// va_copy BEFORE touching 'args': the original list is consumed by the
// FIRST formatting that uses it (here, the call to the original vprintf) -
// reusing it after that is undefined behaviour in several libcs. That is
// why we take a copy straight away and format OUR copy from
// it, never from the original 'args' after it has been used.
// THE COLOUR MATRIX REFUSED BY THE ISP - known noise, filtered.
//
// On the P4 v1.0 each CCM coefficient fits in 13 bits with 2 integer bits
// (isp_ll.h: ISP_LL_CCM_MATRIX_INT_BITS) - maximum +-4, fixed in silicon. On a
// sudden change of light the white balance asks for more than that, the driver
// refuses and the ISP KEEPS the last good matrix - which is precisely what keeps the
// colour right (clamping the matrix to fit made the image yellowish, tested
// and undone). The effect is only in the log: five red lines per refusal.
//
// The chain is recognised by its TEXT, not by its tag: 'isp_video',
// 'esp_video' and 'ISP' also carry real errors, which keep
// appearing. Only what comes right after (10 ms) a CCM "out of range"
// disappears. Each refusal becomes a counter; the log task prints a summary, at
// most one line per minute.
static volatile int64_t  s_ccm_cadeia_ate = 0;
static std::atomic<uint32_t> s_ccm_recusas    = 0;

static bool ruido_da_ccm(const char* linha)
{
  const int64_t agora = esp_timer_get_time();
  if (strstr(linha, "ISP_CCM") && strstr(linha, "out of range")) {
    s_ccm_cadeia_ate = agora + 10000;
    ++s_ccm_recusas;
    return true;
  }
  if (agora > s_ccm_cadeia_ate) { return false; }
  return strstr(linha, "failed to configure CCM")   || strstr(linha, "failed to reconfigure CCM")
      || strstr(linha, "set_ext_ctrl=")             || strstr(linha, "ISP: failed to set CCM")
      || strstr(linha, "ISP: failed to set blue balance") || strstr(linha, "ISP: failed to set red balance");
}

static int log_vprintf(const char* fmt, va_list args)
{
  char linha[LOG_LINHA_TAM];
  va_list copia;
  va_copy(copia, args);
  int n = vsnprintf(linha, sizeof(linha), fmt, copia);
  va_end(copia);

  // Known CCM noise: neither on serial nor on the SD card (see above).
  if (n > 0 && ruido_da_ccm(linha)) { return n; }

  // ORIGINAL with the ORIGINAL arguments - the console/serial stays
  // identical to what it always was, nobody who already depends on it (gravar.sh
  // itself, whoever follows live) notices any difference.
  const int ret = s_original ? s_original(fmt, args) : vprintf(fmt, args);

  if (n > 0 && s_buf && s_escrita && !xPortInIsrContext()) {
    if ((size_t)n >= sizeof(linha)) { n = sizeof(linha) - 1; }   // truncated, not overflowed
    // WARNINGS AND ERRORS DON'T WAIT FOR THE BATCH: the line starts with "W (" / "E (" -
    // after the colour code, if any ("\033[0;33mW (").
    const char* c = linha;
    if (c[0] == '\033') { while (*c && *c != 'm') { ++c; } if (*c) { ++c; } }
    if ((c[0] == 'W' || c[0] == 'E') && c[1] == ' ' && c[2] == '(') { s_urgente = true; }
    // With the scheduler stopped we can't wait (FreeRTOS assert).
    const TickType_t espera = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? pdMS_TO_TICKS(5) : 0;
    if (xSemaphoreTake(s_escrita, espera) == pdTRUE) {
      const size_t enviado = xStreamBufferSend(s_buf, linha, (size_t)n, 0);   // 0 = never waits
      if (enviado < (size_t)n) { s_linhas_perdidas++; }
      else                     { s_linhas_pendentes++; }
      xSemaphoreGive(s_escrita);
    } else {
      s_linhas_perdidas++;
    }
  }
  return ret;
}

// LOCAL day in the name, like energy and monitor (tab5_hora_nome_do_dia); the lines
// and the boot marker stay in UTC.
static void nome_arquivo_hoje(char* out, size_t tam)
{
  tab5_hora_nome_do_dia((uint32_t)time(nullptr), "log_", ".txt", out, tam);
}

// Flushes whatever is in the buffer to TODAY's file. Called only by the
// "cartao" task (log_na_carona), never by whoever is logging.
//
// Through the tab5_sd_acrescentar gateway (hal/tab5_sd.hpp): with the SD card lent
// to the computer, the batch goes to the journal in flash and reaches the file when it
// comes back - the log of a USB-drive session isn't lost.
static void descarregar(void)
{
  if (!s_buf || xStreamBufferIsEmpty(s_buf)) { return; }

  // IS THERE SOMEWHERE TO WRITE? The SD card (held until the end of the batch, so it doesn't
  // vanish midway) or, with it lent out, the journal. With neither it does NOT
  // DISCARD: the lines stay in the buffer, waiting for the SD card to come back (or
  // giving way to newer ones if it fills first, the same policy
  // xStreamBufferSend already applies).
  const bool montou = tab5_sd_montar();
  if (!montou && !tab5_sd_emprestado()) { return; }

  char nome[24];
  nome_arquivo_hoje(nome, sizeof(nome));

  // BOOT MARKER, once per session - the same reasoning as "BOOT" in
  // registro.cpp/energia.csv: since several boots on the same day land in the SAME
  // file (see the big notice in the .hpp about "per day, not per boot"), without
  // this it would be impossible to know where a restart separates two sessions. The
  // "(N)" on each ESP_LOG line is just milliseconds since THIS boot - the
  // marker ties that local clock to a real date/time.
  if (!s_boot_marcado) {
    const time_t agora = time(nullptr);
    struct tm t = { };
    gmtime_r(&agora, &t);
    char marca[80];
    const int n = snprintf(marca, sizeof(marca), "=== boot %04d-%02d-%02dT%02d:%02d:%02dZ, uptime %llums ===\n",
                           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                           (unsigned long long)(esp_timer_get_time() / 1000));
    if (tab5_sd_acrescentar(TAB5_SD_LOGS, nome, marca, (size_t)n)) { s_boot_marcado = true; }
  }

  // Drains in blocks, without allocating per line, into a static block the size of the
  // whole buffer (12KB, in PSRAM - never on the flusher's stack).
  EXT_RAM_BSS_ATTR static uint8_t bloco[LOG_BUF_TAM];
  size_t total = 0;
  size_t n;
  while ((n = xStreamBufferReceive(s_buf, bloco, sizeof(bloco), 0)) > 0) {
    if (!tab5_sd_acrescentar(TAB5_SD_LOGS, nome, bloco, n)) { s_linhas_perdidas++; break; }
    total += n;
  }
  if (montou) { tab5_sd_desmontar(); }

  if (total > 0) {
    snprintf(s_arquivo_atual, sizeof(s_arquivo_atual), "%s", nome);
    s_linhas_gravadas  += s_linhas_pendentes;
    s_linhas_pendentes  = 0;
  }
}

// IN BATCHES, PIGGYBACKING ON THE "cartao" TASK (tab5_sd_carona) - the log has no
// task of its own: "cartao" already wakes every 1 s and is already what writes to the SD card.
//   - informational lines wait for the LOG_FLUSH_MS batch (60s);
//   - a WARNING or ERROR goes to the SD card within 1s (it is what you want to read after a
//     crash);
//   - a buffer past 3/4 flushes immediately, so as not to lose lines.
// Combined with tab5_sd's deferred unmount, nearby writes (log, monitor,
// energy) share a single mount.
static void log_na_carona(void)
{
  static int64_t  desde_us      = esp_timer_get_time();
  static int64_t  ccm_resumo_us = desde_us;
  static uint32_t ccm_contadas  = 0;
  const int64_t agora = esp_timer_get_time();

  // The summary of the filtered CCM refusals (see ruido_da_ccm): one line, at
  // most once a minute, only when there was a new refusal.
  const uint32_t recusas = s_ccm_recusas;
  if (recusas != ccm_contadas && agora - ccm_resumo_us >= 60000000LL) {
    ESP_LOGI(TAG, "camera: %lu colour adjustment(s) beyond the chip's limit were ignored (expected, not an error)",
             (unsigned long)(recusas - ccm_contadas));
    ccm_contadas = recusas;
    ccm_resumo_us = agora;
  }
  const bool cheio = s_buf && xStreamBufferBytesAvailable(s_buf) > (LOG_BUF_TAM * 3) / 4;
  if (s_urgente || cheio || agora - desde_us >= (int64_t)LOG_FLUSH_MS * 1000) {
    s_urgente = false;
    desde_us = agora;
    descarregar();
  }
}

void tab5_log_descarregar_logo(void) { s_urgente = true; }

void tab5_log_iniciar(void)
{
  // 1 "trigger" byte - we don't use a blocking xStreamBufferReceive
  s_buf = xStreamBufferCreateWithCaps(LOG_BUF_TAM, 1, MALLOC_CAP_SPIRAM);
  s_escrita = xSemaphoreCreateMutex();              // BEFORE the hook: it only writes with the lock
  if (!s_buf || !s_escrita) {
    ESP_LOGE(TAG, "no RAM for the log buffer - SD card capture disabled");
    return;
  }

  // Keeps the CURRENT function before swapping - normally IDF's default
  // vprintf(), but we don't assume: whatever is installed keeps receiving
  // ALL the log, only now through our function.
  s_original = esp_log_set_vprintf(log_vprintf);

  // No task of its own: descarregar() runs in the "cartao" task (see
  // log_na_carona). The 12KB drain block is 'static' in PSRAM, off the stack.
  tab5_sd_carona(log_na_carona);

  ESP_LOGI(TAG, "capture running: buffer %dKB, flush every %lus, /sdcard/apps/Sistema/logs/log_AAAA-MM-DD.txt",
           LOG_BUF_TAM / 1024, (unsigned long)(LOG_FLUSH_MS / 1000));

  // WHY THE DEVICE RESTARTED. A crash loses the lines that were still
  // in the buffer (up to 60 s of informational log) - without this line, a
  // restart in the middle of something just looks like "the log ended there".
  const esp_reset_reason_t r = esp_reset_reason();
  const char* motivo = "outro";
  switch (r) {
    case ESP_RST_POWERON:   motivo = "powered on"; break;
    case ESP_RST_SW:        motivo = "reinicio pedido (esp_restart)"; break;
    case ESP_RST_PANIC:     motivo = "TRAVOU (panic/excecao)"; break;
    case ESP_RST_INT_WDT:   motivo = "CRASHED (interrupt watchdog)"; break;
    case ESP_RST_TASK_WDT:  motivo = "CRASHED (task watchdog)"; break;
    case ESP_RST_WDT:       motivo = "TRAVOU (outro watchdog)"; break;
    case ESP_RST_BROWNOUT:  motivo = "brownout"; break;
    case ESP_RST_DEEPSLEEP: motivo = "woke from deep sleep"; break;
    default: break;
  }
  // THE CHIP'S RAW REASON (soc_reset_reason_t, soc/reset_reasons.h) goes along too:
  // on this board the "other watchdog" (code 7) also appears when the device
  // comes back via the button after being switched off (seen 06/11/2026: after the battery
  // guard switched off at 0%). Only the raw one tells them apart.
  const int cru = (int)esp_rom_get_reset_reason(0);
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
    ESP_LOGW(TAG, "boot: the last restart was %s (code %d, chip %d)", motivo, (int)r, cru);
    tab5_log_travamento(false);
  } else {
    ESP_LOGI(TAG, "boot: the last restart was %s (code %d, chip %d)", motivo, (int)r, cru);
  }
  if (const uint32_t n = tab5_heap_psram_reinicios()) {
    ESP_LOGW(TAG, "boot: PSRAM heap corrupted before M5.begin - %lu clean restart(s) in a row",
             (unsigned long)n);
  }
}

// THE SNAPSHOT OF THE LAST CRASH (core dump in flash, see partitions.csv). The
// task, where it was (PC) and who called (RA): with the .elf of the same version,
//   riscv32-esp-elf-addr2line -pfiaC -e build/M5tabOS.elf <PC> <RA>
// gives the line. A watchdog that locked up BOTH cores doesn't get to write anything.
bool tab5_log_travamento(bool no_console)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  if (esp_core_dump_image_check() != ESP_OK) {
    if (no_console) { printf("no crash stored in flash\n"); }
    return false;
  }
  esp_core_dump_summary_t* r = (esp_core_dump_summary_t*)heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_SPIRAM);
  if (!r) { return false; }
  const bool ok = esp_core_dump_get_summary(r) == ESP_OK;
  if (ok) {
    char sha[APP_ELF_SHA256_SZ + 1];
    memcpy(sha, r->app_elf_sha256, APP_ELF_SHA256_SZ);
    sha[APP_ELF_SHA256_SZ] = '\0';
    // FROM THIS FIRMWARE? The snapshot stays in flash until another crash replaces it, and
    // flashing new firmware doesn't erase it: without this, a crash from weeks
    // ago looks like today's on every boot.
    char meu[APP_ELF_SHA256_SZ + 1] = "";
    esp_app_get_elf_sha256(meu, sizeof(meu));
    const bool deste = strncmp(meu, sha, 8) == 0;
    // Warning (W): goes to the SD card immediately, without waiting for the 60 s flush.
    ESP_LOGW(TAG, "crash: task '%s', PC 0x%08lx, RA 0x%08lx, SP 0x%08lx, mcause %lu, mtval 0x%08lx (elf %.8s, %s)",
             r->exc_task, (unsigned long)r->exc_pc, (unsigned long)r->ex_info.ra, (unsigned long)r->ex_info.sp,
             (unsigned long)r->ex_info.mcause, (unsigned long)r->ex_info.mtval, sha,
             deste ? "from THIS firmware" : "from an earlier firmware");
    // The code addresses that were on the stack: almost always the call
    // chain (0x4xxxxxxx), in the order they appear.
    char linha[160];
    int k = 0, n = 0;
    for (uint32_t i = 0; i + 4 <= r->exc_bt_info.dump_size && n < 16; i += 4) {
      uint32_t v;
      memcpy(&v, r->exc_bt_info.stackdump + i, 4);
      if (v < 0x40000000u || v >= 0x44000000u) { continue; }
      k += snprintf(linha + k, sizeof(linha) - (size_t)k, " 0x%08lx", (unsigned long)v);
      ++n;
      if (k > (int)sizeof(linha) - 14) {
        ESP_LOGW(TAG, "crash: stack%s", linha);
        k = 0;
      }
    }
    if (k) { ESP_LOGW(TAG, "crash: stack%s", linha); }
  }
  heap_caps_free(r);
  return ok;
#else
  if (no_console) { printf("core dump disabled in sdkconfig\n"); }
  return false;
#endif
}
