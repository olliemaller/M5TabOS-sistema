#include "tab5_pendrive.hpp"
#include "idioma.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "esp_private/usb_phy.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/usb_wrap_struct.h"
#include "tusb.h"
#include "tab5_donos.hpp"
#include "tab5_sd.hpp"
#include "tab5_power.hpp"
#include "app.h"            // sistema_app_exige_sd, sistema_manter_acordado, sistema_app_atual...
#include "ajustes.h"
#include "notificacao.h"
#include "tab5_log.hpp"
#include "pool_tarefas.h"
#include <atomic>

static const char* TAG = "pendrive";

// The lent card, seen only through the hal's sector gates (tab5_sd_cru_*):
// here we only keep "we have a card" and its size.
static bool               s_tem_cartao = false;
static uint32_t           s_setores = 0, s_tam = 512;
static usb_phy_handle_t   s_phy     = nullptr;
static TaskHandle_t       s_tarefa  = nullptr;
static SemaphoreHandle_t  s_parou   = nullptr;
static volatile bool      s_parar   = false;
static volatile bool      s_ligado  = false;
static volatile bool      s_ejetado = false;
static volatile bool      s_suspenso = false;
static esp_pm_lock_handle_t s_pm = nullptr;

// DIAGNOSTICS BY STAGE - where the computer stopped (see tab5_pendrive_usb_t):
// did it recognise the device? ask what it is? the size? read?
static std::atomic<uint32_t> s_n_config = 0;      // SET_CONFIGURATION (tud_mount_cb)
static std::atomic<uint32_t> s_n_inquiry = 0;     // INQUIRY
static std::atomic<uint32_t> s_n_capacidade = 0;  // READ CAPACITY / READ FORMAT CAPACITIES
static std::atomic<uint32_t> s_n_leituras = 0;    // READ10 chunks read from the card (up to 4 KB each)
static volatile uint8_t  s_op_recusado = 0;   // last unsupported SCSI command (0 = none)
static volatile uint32_t s_leitura_max_us = 0;// slowest card read
static uint32_t          s_ops_logados[8];    // refused commands (one bit per code)
static std::atomic<uint32_t> s_n_tur = 0;         // TEST UNIT READY
static std::atomic<uint32_t> s_n_sense = 0;       // REQUEST SENSE (the computer wants to know about an error)
static std::atomic<uint32_t> s_n_respostas = 0;   // commands (other than read/write) with status delivered
static volatile uint8_t  s_op_respondido = 0; // the last of them
static std::atomic<uint32_t> s_n_soltou = 0;      // deconfigured (bus reset, cable)
static uint32_t          s_ops_respondidos[8];// commands already answered (one bit per code)
static std::atomic<uint32_t> s_n_leituras_ok = 0; // READ10 commands with data and status delivered
// WHERE IT STOPPED, when it stops: the three states a read command goes through.
static std::atomic<uint32_t> s_n_cmd_leitura = 0; // READ10 commands received
static volatile bool     s_lendo = false;     // a READ10 started and the status has not gone out yet
static volatile bool     s_no_cartao = false; // inside a card read/write right now
static volatile bool     s_gravando = false;  // ...and it is a write (so the log says which)
static volatile uint32_t s_escrita_max_us = 0;// slowest card write
static std::atomic<uint32_t> s_n_escritas_lentas = 0; // writes of more than 100 ms
static volatile uint32_t s_cmd_setor = 0;     // first sector of the READ10 in progress
static volatile uint32_t s_ultimo_setor = 0;  // last chunk requested from the card
static volatile int64_t  s_evento_us = 0;     // last sign of life from the computer or the card
static volatile uint32_t s_erro_setor = 0;    // the card's last failure (for the log)
static volatile int      s_erro_cod = 0;
static volatile uint64_t  s_lidos = 0, s_gravados = 0;
static std::atomic<uint32_t>  s_erros = 0;
static volatile int64_t   s_ultimo_io_us = 0, s_ultima_escrita_us = 0;

// COALESCED WRITES and READ-AHEAD (see the "MSC" section). All this is only touched
// by the "pendrive" task (tud_task), so no lock; the counters are read
// by the session task only for the log.
#define BUF_BYTES 65536                      // 64 KB: 128 sectors of 512
#define OCIOSO_US 30000                      // buffer idle this long: goes to the card
static uint8_t*  s_buf_esc = nullptr;        // coalesced writes (PSRAM)
static uint32_t  s_esc_ini = 0, s_esc_n = 0; // first sector and how many are in the buffer
static uint8_t*  s_buf_ler = nullptr;        // read-ahead (PSRAM)
static uint32_t  s_ler_ini = 0, s_ler_n = 0; // likewise; s_ler_n = 0 = cache empty
static volatile bool s_cartao_falhou = false;// the card failed for good: no media, the session ends
static std::atomic<uint32_t> s_n_descargas = 0;      // writes to the card (each one coalesces several chunks)
static std::atomic<uint32_t> s_kb_descargas = 0;     // KB written by them
static std::atomic<uint32_t> s_n_cache_ok = 0;       // read chunks served from the cache
static std::atomic<uint32_t> s_n_cache_falta = 0;    // card reads (cache empty or another range)

// ---------------------------------------------------------------------------
// THE PHY: from the console to OTG, and back
// ---------------------------------------------------------------------------
// LP_SYS.usb_ctrl.sw_usb_phy_sel (hal/usb_wrap_ll.h):
//   false - JTAG on PHY 0 (the USB-C), OTG on PHY 1 (the chip default)
//   true  - OTG on PHY 0, JTAG on PHY 1
// JTAG is NOT turned off or reset: the console driver stays
// installed, with its interrupts as they were, and simply stops seeing the
// computer (to it, it is a pulled cable - log output that cannot go out is discarded,
// as with the device on battery). That is why the console comes back by itself.
static void phy_para_o_otg(void)
{
  usb_serial_jtag_ll_phy_enable_pad(false);   // releases the JTAG resistors on PHY 0
  usb_wrap_ll_phy_select(&USB_WRAP, 0);       // OTG -> PHY 0 (USB-C), JTAG -> PHY 1
}

// The state of the registers that decide whether the computer sees the console - in the
// CARD LOG, because it is precisely when the console does not come back that the cable
// shows nothing.
static void registro_usb(const char* quando)
{
  ESP_LOGI(TAG, "usb %s: selector sw=%u phy=%u | jtag conf0=%08lx (pad %u, override %u, pullup %u) "
                "int_ena=%08lx, computer sees: %s",
           quando, (unsigned)LP_SYS.usb_ctrl.sw_hw_usb_phy_sel, (unsigned)LP_SYS.usb_ctrl.sw_usb_phy_sel,
           (unsigned long)USB_SERIAL_JTAG.conf0.val, (unsigned)USB_SERIAL_JTAG.conf0.usb_pad_enable,
           (unsigned)USB_SERIAL_JTAG.conf0.pad_pull_override, (unsigned)USB_SERIAL_JTAG.conf0.dp_pullup,
           (unsigned long)USB_SERIAL_JTAG.int_ena.val, usb_serial_jtag_is_connected() ? "yes" : "no");
}

// The OTG pad must already be off (before usb_del_phy, which cuts
// the USB_WRAP clock - a peripheral register without a clock cannot be written).
//
// BACK TO THE BOOT STATE, not just "JTAG on PHY 0". After USB drive mode the
// Mac no longer saw the serial port - not even unplugging and replugging; only a restart.
// Two things were different from boot:
//   - the selector stayed in SOFTWARE mode (sw_hw_usb_phy_sel = 1); at boot
//     it is on the factory mapping;
//   - the console controller spent the whole session connected to the other PHY
//     (the speaker pins), with no computer, and was never reset.
// Now it is reset (a new cable as far as it is concerned), with the interrupts the
// console driver set up restored - the driver stays the same.
static void phy_para_o_console(void)
{
  registro_usb("antes de voltar");
  usb_serial_jtag_ll_phy_select(0);           // JTAG -> PHY 0 again
  LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;      // and the selector on the factory mapping, as at boot
  const uint32_t ints = USB_SERIAL_JTAG.int_ena.val;
  PERIPH_RCC_ATOMIC() { usb_serial_jtag_ll_reset_register(); }
  usb_serial_jtag_ll_phy_set_defaults();      // internal PHY, pad on
  USB_SERIAL_JTAG.conf0.pad_pull_override = 0;   // the D+ pull-up belongs to the controller
  usb_serial_jtag_ll_phy_enable_pad(true);
  USB_SERIAL_JTAG.int_ena.val = ints;
  registro_usb("console de volta");
}

// PAD WATCHDOG. ESP-IDF assumes the PHY on GPIO 26/27 always belongs to OTG
// (hal/esp32p4/.../gpio_ll.h): configuring either of those pins - the speaker's
// I2S uses both - writes usb_pad_enable = 0 and removes the D+ pull-up
// in the OTG's register, which is now on the USB-C. To the computer, the cable
// was pulled (without any reset), and the Tab5 kept waiting for the next command. The
// speaker is silenced before USB comes up (sistema_som_ceder_pinos); this
// catches anything else still touching those pins: reconnects and counts.
static std::atomic<uint32_t> s_n_pad_religado = 0;
static bool pad_conferir(void)
{
  usb_wrap_otg_conf_reg_t c;
  c.val = USB_WRAP.otg_conf.val;
  if (c.usb_pad_enable && !c.pad_pull_override) { return false; }
  c.usb_pad_enable = 1;
  c.pad_pull_override = 0;   // the pull-up goes back to the controller
  USB_WRAP.otg_conf.val = c.val;
  ++s_n_pad_religado;
  return true;
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
// Espressif's VID with the PID esp_tinyusb uses for "MSC only" (0x4000 with
// the MSC bit) - and NOT JTAG's 0x1001: the computer stores drivers by
// VID:PID, and the same pair for two different functions confuses Windows.
static const tusb_desc_device_t DESC_APARELHO = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = 0x00,   // the class comes in the interface (macOS prefers it this way)
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = 0x303A,
  .idProduct          = 0x4008,
  .bcdDevice          = 0x0100,
  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,
  .bNumConfigurations = 0x01,
};

#define EP_SAIDA   0x01
#define EP_ENTRADA 0x81
static const uint8_t DESC_CONFIG[] = {
  TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN,
                        TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
  TUD_MSC_DESCRIPTOR(0, 0, EP_SAIDA, EP_ENTRADA, 64),
};

// The SERIAL NUMBER follows the mass storage specification (Bulk-Only
// Transport, 4.1.1): at least 12 hexadecimal digits. macOS is the most
// demanding about this. It comes from the device's own MAC: unique and always the same,
// so the computer recognises the Tab5 from one time to the next.
static char s_serie[13] = "000000000000";
static const char* const TEXTOS[] = { "", "M5Stack", "M5tabOS SD card", s_serie };

extern "C" const uint8_t* tud_descriptor_device_cb(void) { return (const uint8_t*)&DESC_APARELHO; }
extern "C" const uint8_t* tud_descriptor_configuration_cb(uint8_t) { return DESC_CONFIG; }

extern "C" const uint16_t* tud_descriptor_string_cb(uint8_t indice, uint16_t)
{
  static uint16_t d[33];
  uint8_t n;
  if (indice == 0) {
    d[1] = 0x0409;   // English (US): the language every system accepts
    n = 1;
  } else {
    if (indice >= sizeof(TEXTOS) / sizeof(TEXTOS[0])) { return nullptr; }
    const char* t = TEXTOS[indice];
    n = (uint8_t)strnlen(t, 32);
    for (uint8_t i = 0; i < n; ++i) { d[1 + i] = (uint8_t)t[i]; }
  }
  d[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return d;
}

// ---------------------------------------------------------------------------
// MSC: the card in sectors
// ---------------------------------------------------------------------------
// Everything here runs on the "pendrive" task (tud_task), one call at a time - the
// raw card has only this user while it is lent.
static uint32_t setor_tam(void) { return s_tam; }

// Card offline as far as the computer is concerned: ejected, no card or failed for good.
static bool sem_midia(void) { return s_ejetado || !s_tem_cartao || s_cartao_falhou; }

// ---------------------------------------------------------------------------
// COALESCED WRITES AND READ-AHEAD
// ---------------------------------------------------------------------------
// TinyUSB delivers each command in 4 KB chunks (CFG_TUD_MSC_EP_BUFSIZE).
// Writing each chunk as an 8-sector write made the card slow
// (1.5 s writes) until one passed 5 s and locked up the controller. Now
// CONTIGUOUS chunks go into a 64 KB buffer and go out in a single write.
//
// WHEN IT FLUSHES (all on the same task, no race):
//   - the buffer filled up;
//   - a non-contiguous write arrived (flush and start another);
//   - a READ10 arrived (coherence: the computer reads what it has just written);
//   - SYNCHRONIZE CACHE, TEST UNIT READY, START STOP (ejetar);
//   - idleness: nothing new for OCIOSO_US (vTaskPendrive loop);
//   - before shutting TinyUSB down.
//
// THE WINDOW: TinyUSB 0.21 only signals each chunk (it does not say which is the last of a
// WRITE10), so the command status may go out before the data reaches the
// card - at most OCIOSO_US + one 20 ms round. It is the write cache of
// any USB drive; the computer covers it with SYNCHRONIZE CACHE and ejecting.
//
// No ESP_LOG here: it runs in the USB callbacks (see tud_msc_scsi_complete_cb).
static void cartao_falhou(uint32_t setor, esp_err_t err)
{
  s_erro_setor = setor;
  s_erro_cod = err;
  ++s_erros;
  s_cartao_falhou = true;   // the session sees it and ends (vTaskSessao)
}

// Writes the write buffer to the card, in a single write. false = failed for good.
static bool descarregar(void)
{
  // Only a REAL flush invalidates the read cache. An empty buffer does not
  // change anything on the card: invalidating here (as before) wiped the cache on every
  // chunk read - READ10 calls this function before reading - and every 4 KB requested
  // became 64 KB read from the card.
  if (!s_esc_n) { return true; }
  s_ler_n = 0;   // the card is going to change: what was in the cache may be stale
  if (s_cartao_falhou) { s_esc_n = 0; return false; }
  const uint32_t n = s_esc_n;
  const int64_t t0 = esp_timer_get_time();
  s_evento_us = t0;
  s_ultimo_setor = s_esc_ini;
  s_gravando = true;
  s_no_cartao = true;
  const esp_err_t err = tab5_sd_cru_gravar(s_esc_ini, s_buf_esc, n);
  s_no_cartao = false;
  s_gravando = false;
  s_esc_n = 0;
  s_evento_us = esp_timer_get_time();
  const uint32_t dt = (uint32_t)(s_evento_us - t0);
  if (dt > s_escrita_max_us) { s_escrita_max_us = dt; }
  if (dt > 100000) { ++s_n_escritas_lentas; }
  if (err != ESP_OK) { cartao_falhou(s_esc_ini, err); return false; }
  ++s_n_descargas;
  s_kb_descargas += n * s_tam / 1024;
  s_gravados = s_gravados + (uint64_t)n * s_tam;
  s_ultimo_io_us = s_evento_us;
  return true;
}

// Called by the task loop: whatever was left in the buffer goes out after OCIOSO_US.
static void descarregar_se_ocioso(void)
{
  if (s_esc_n && esp_timer_get_time() - s_ultima_escrita_us >= OCIOSO_US) { descarregar(); }
}

// The SENSE of a flush that failed or of the card being offline.
static void sense_sem_midia(uint8_t lun) { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); }

static void log_cartao(void);   // defined alongside the session log

static void buffers_soltar(void)
{
  heap_caps_free(s_buf_esc); s_buf_esc = nullptr;
  heap_caps_free(s_buf_ler); s_buf_ler = nullptr;
  s_esc_n = 0; s_ler_n = 0;
}

// Aligned to the cache line (128 on the P4): the card DMA reads and writes directly.
static bool buffers_alocar(void)
{
  s_buf_esc = (uint8_t*)heap_caps_aligned_alloc(128, BUF_BYTES, MALLOC_CAP_SPIRAM);
  s_buf_ler = (uint8_t*)heap_caps_aligned_alloc(128, BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_buf_esc || !s_buf_ler) { buffers_soltar(); return false; }
  s_esc_n = 0; s_ler_n = 0;
  return true;
}

// WHAT THE DISK IS, and the response size. TinyUSB answers INQUIRY with
// 36 bytes; if the computer asked for more (some systems ask for 255), it sends
// the 36 and STALLs the data pipe - the computer has to unstall it
// (CLEAR_FEATURE) before it can read the status. It is the first disk command to
// go down that path, and the log stopped exactly there: the computer
// asked and never got to the size. The transport specification (BOT
// 6.7, case 5) leaves the other way out: pad with zeros up to the requested size.
// The true length goes in the header itself (additional_length), so
// the zeros are just padding. Request of 36, response of 36 - nothing changes.
#define RESPOSTA_CHEIA 255
static void inquiry_contar(void) { ++s_n_inquiry; }

static void inquiry_textos(uint8_t vendor[8], uint8_t produto[16], uint8_t rev[4])
{
  memset(vendor, ' ', 8);   memcpy(vendor, "M5Stack", 7);
  memset(produto, ' ', 16); memcpy(produto, "Tab5 SD card", 12);
  memset(rev, ' ', 4);      memcpy(rev, "1.0", 3);
}

extern "C" uint32_t tud_msc_inquiry2_cb(uint8_t, scsi_inquiry_resp_t* r, uint32_t bufsize)
{
  inquiry_contar();
  inquiry_textos(r->vendor_id, r->product_id, r->product_rev);
  const uint32_t n = bufsize < RESPOSTA_CHEIA ? bufsize : RESPOSTA_CHEIA;
  if (n <= sizeof(*r)) { return sizeof(*r); }
  memset((uint8_t*)r + sizeof(*r), 0, n - sizeof(*r));
  return n;   // TinyUSB trims to what the computer asked for
}

// The old version: only called if the one above does not exist in the library.
extern "C" void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t produto[16], uint8_t rev[4])
{
  inquiry_contar();
  inquiry_textos(vendor, produto, rev);
}

// REQUEST SENSE, the same case: 18 bytes, and the computer asks for more (Linux
// asks for 96). TinyUSB has already built the 18 in 'buf'; the rest is zero.
extern "C" int32_t tud_msc_request_sense_cb(uint8_t, void* buf, uint16_t bufsize)
{
  ++s_n_sense;
  const uint16_t n = bufsize < RESPOSTA_CHEIA ? bufsize : RESPOSTA_CHEIA;
  if (n <= sizeof(scsi_sense_fixed_resp_t)) { return sizeof(scsi_sense_fixed_resp_t); }
  memset((uint8_t*)buf + sizeof(scsi_sense_fixed_resp_t), 0, n - sizeof(scsi_sense_fixed_resp_t));
  return n;
}

// A COMMAND'S STATUS REACHED THE COMPUTER (TinyUSB only calls this when
// the whole response has gone out). It is what separates "the computer did not ask" from
// "it asked and the answer did not arrive".
//
// NO LOGGING IN HERE, nor in the other TinyUSB callbacks: ESP_LOG goes down
// to the console (USB-Serial-JTAG, with no computer during USB drive mode - up to
// 50 ms of waiting), through the stdout locks and the USB task's stack, in the
// middle of a transfer. The callbacks only count; the session task writes to the log,
// looking at the counters (see registrar_eventos).
extern "C" void tud_msc_scsi_complete_cb(uint8_t, const uint8_t cmd[16])
{
  const uint8_t op = cmd[0];
  ++s_n_respostas;
  s_op_respondido = op;
  s_ops_respondidos[op >> 5] |= 1u << (op & 31);
  s_evento_us = esp_timer_get_time();
}

extern "C" void tud_msc_read10_complete_cb(uint8_t)
{
  ++s_n_leituras_ok;
  s_lendo = false;
  s_evento_us = esp_timer_get_time();
}

extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
  ++s_n_tur;
  s_evento_us = esp_timer_get_time();
  // Ejected: "no media" - the computer stops sending commands to the disk.
  // Takes the chance to empty the write buffer (the computer asks this
  // whenever it is idle).
  if (!sem_midia()) { descarregar(); }
  if (sem_midia()) {
    sense_sem_midia(lun);
    return false;
  }
  return true;
}

extern "C" void tud_msc_capacity_cb(uint8_t, uint32_t* n_blocos, uint16_t* bloco)
{
  ++s_n_capacidade;
  *n_blocos = s_tem_cartao ? s_setores : 0;
  *bloco    = (uint16_t)setor_tam();
}

extern "C" bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool ejetar)
{
  // "Eject" in Finder/Explorer/Linux arrives here as STOP + eject - the signal
  // that the computer has finished and has nothing more cached for the disk.
  if (ejetar) {
    // Flush BEFORE accepting: once ejected nothing more goes out of the buffer.
    if (!start && !descarregar()) {
      tud_msc_set_sense(0, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
      return false;
    }
    s_ejetado = !start;
  }
  return true;
}

extern "C" bool tud_msc_prevent_allow_medium_removal_cb(uint8_t, uint8_t, uint8_t) { return true; }

extern "C" bool tud_msc_is_writable_cb(uint8_t) { return !s_ejetado; }

extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buf, uint32_t tam)
{
  const uint32_t st = setor_tam();
  if (sem_midia() || offset % st || tam < st) {
    sense_sem_midia(lun);
    return -1;
  }
  const uint32_t n = tam / st;
  const uint32_t setor = lba + offset / st;
  if (setor + n > s_setores) {   // beyond the end of the card
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
    return -1;
  }
  if (!s_lendo) {   // first chunk of a new command
    s_lendo = true;
    ++s_n_cmd_leitura;
    s_cmd_setor = lba;
  }
  ++s_n_leituras;
  s_ultimo_setor = setor;
  // Coherence: whatever the computer wrote that is still in the buffer goes first.
  if (!descarregar()) {
    sense_sem_midia(lun);
    return -1;
  }
  // READ-AHEAD: outside the cache, read 64 KB from the requested sector (up to the
  // end of the card) in a single read; the next chunks come from there.
  if (!(s_ler_n && setor >= s_ler_ini && setor + n <= s_ler_ini + s_ler_n)) {
    uint32_t cnt = BUF_BYTES / st;
    if (cnt > s_setores - setor) { cnt = s_setores - setor; }
    const int64_t t0 = esp_timer_get_time();
    s_evento_us = t0;
    s_no_cartao = true;
    const esp_err_t lido = tab5_sd_cru_ler(setor, s_buf_ler, cnt);
    s_no_cartao = false;
    const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    s_evento_us = esp_timer_get_time();
    if (dt > s_leitura_max_us) { s_leitura_max_us = dt; }
    if (lido != ESP_OK) {
      s_ler_n = 0;
      cartao_falhou(setor, lido);
      tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // read error
      return -1;
    }
    s_ler_ini = setor;
    s_ler_n = cnt;
    s_lidos = s_lidos + (uint64_t)cnt * st;
    ++s_n_cache_falta;
  } else {
    ++s_n_cache_ok;
  }
  memcpy(buf, s_buf_ler + (size_t)(setor - s_ler_ini) * st, (size_t)n * st);
  s_ultimo_io_us = esp_timer_get_time();
  return (int32_t)(n * st);
}

extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t tam)
{
  const uint32_t st = setor_tam();
  if (sem_midia() || offset % st || tam < st) {
    sense_sem_midia(lun);
    return -1;
  }
  // Whole sectors only: whatever is left over (never, with the buffer a multiple of
  // 512) TinyUSB delivers again on the next call.
  const uint32_t n = tam / st;
  const uint32_t setor = lba + offset / st;
  if (setor + n > s_setores) {   // beyond the end of the card
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
    return -1;
  }
  const uint32_t cap = BUF_BYTES / st;
  // Not contiguous with what is already in the buffer, or does not fit: flush first.
  if (s_esc_n && (setor != s_esc_ini + s_esc_n || s_esc_n + n > cap) && !descarregar()) {
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);   // write error
    return -1;
  }
  if (!s_esc_n) { s_esc_ini = setor; }
  memcpy(s_buf_esc + (size_t)s_esc_n * st, buf, (size_t)n * st);
  s_esc_n += n;
  const int64_t agora = esp_timer_get_time();
  s_evento_us = agora;
  s_ultimo_io_us = agora;
  s_ultima_escrita_us = agora;
  if (s_esc_n >= cap && !descarregar()) {   // full
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
    return -1;
  }
  return (int32_t)(n * st);
}

extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t cmd[16], void* buf, uint16_t tam)
{
  switch (cmd[0]) {
    case 0x35:   // SYNCHRONIZE CACHE (10): the computer wants what it wrote on the card
      if (!descarregar()) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
        return -1;
      }
      return 0;
    case 0x5A: { // MODE SENSE (10). macOS asks for it (TinyUSB only has
                 // the 6-byte one) to know whether the disk accepts writes. An 8-byte
                 // header, no block descriptor: data length (6), medium
                 // type 0, write-protect bit.
      const uint8_t r[8] = { 0, 6, 0, (uint8_t)(s_ejetado ? 0x80 : 0x00), 0, 0, 0, 0 };
      const uint16_t k = tam < sizeof(r) ? tam : (uint16_t)sizeof(r);
      memcpy(buf, r, k);
      return k;
    }
    default: {
      // Refused: it is recorded (once per command) - it is the prime suspect
      // when a system will not mount the disk.
      const uint8_t op = cmd[0];
      s_op_recusado = op;
      s_ops_logados[op >> 5] |= 1u << (op & 31);
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      return -1;
    }
  }
}

// "Mount" in TinyUSB is the computer CONFIGURING the device (enumeration done) -
// it is not the disk being opened yet: that comes later, with INQUIRY, size and reads.
extern "C" void tud_mount_cb(void)
{
  s_suspenso = false;
  ++s_n_config;
  s_lendo = false;
  s_evento_us = esp_timer_get_time();
}

// It is NOT an eject. Deconfiguring also happens in a recovery reset by the
// computer in the middle of a copy - giving the card back then would pull the disk
// out from under it. Without mounting, the screen just stops asking for confirmation to exit.
extern "C" void tud_umount_cb(void)
{
  ++s_n_soltou;
  s_lendo = false;   // bus reset: the command in progress died with it
}
extern "C" void tud_suspend_cb(bool)  { s_suspenso = true; }
extern "C" void tud_resume_cb(void)   { s_suspenso = false; }

// ---------------------------------------------------------------------------
// The task and the life cycle
// ---------------------------------------------------------------------------
// TINYUSB IS BORN AND DIES INSIDE ITS OWN TASK, pinned to one core.
// tusb_init allocates the USB interrupt on the caller's core; the
// library protects the transfer state with a lock that, on the same
// core, disables interrupts - but the interrupt handler (writing
// and reading the FIFO, finishing transfers) does not take that lock. With the
// interrupt on one core and the task migrating to the other, both touch the
// same endpoint at the same time, and the transfer hangs with no error at all -
// at a different point each time. Before, tusb_init ran on the session
// task (no fixed core) and this task had no core either. It is what
// Espressif's esp_tinyusb does (tinyusb_task.c): init in the task, pinned task.
// Core 1, the same esp_tinyusb uses by default (0 is left to WiFi).
#define PENDRIVE_NUCLEO 1
static SemaphoreHandle_t s_iniciou = nullptr;
static volatile bool     s_init_ok = false;

static void vTaskPendrive(void*)
{
  tusb_rhport_init_t ini = { };
  ini.role  = TUSB_ROLE_DEVICE;
  ini.speed = TUSB_SPEED_FULL;
  s_init_ok = tusb_init(0, &ini);
  xSemaphoreGive(s_iniciou);
  if (s_init_ok) {
    while (!s_parar) {
      tud_task_ext(20, false);   // wakes every 20 ms to check for the stop request
      descarregar_se_ocioso();   // a coalesced write left idle goes to the card
    }
    descarregar();               // whatever the reason for exiting: nothing stays in the buffer
    tusb_deinit(0);              // on the same core the interrupt was born on
  }
  tab5_dono_esquecer(xTaskGetCurrentTaskHandle());
  xSemaphoreGive(s_parou);
  vTaskDelete(nullptr);
}

// Does the controller have DMA? (GHWCFG2.OtgArch = 2). Without it TinyUSB falls back to
// slave mode - only so the log says which of the two is in use.
static const char* usb_modo(void)
{
  const uint32_t ghwcfg2 = *(volatile const uint32_t*)(0x50040000UL + 0x48);
  return ((ghwcfg2 >> 3) & 3) == 2 ? "DMA" : "escravo";
}

// CLOCK AT MAXIMUM while the computer is talking: with a variable
// clock, each pause between packets drops the CPU to the floor and the next
// request from the computer waits for it to ramp up. With the bus STOPPED (cable pulled
// without ejecting, computer asleep) the lock is released - see the session loop: then the
// Tab5 is on battery and there is no request to serve quickly.
static bool s_clock_max = false;
static void clock_maximo(bool sim)
{
  if (sim == s_clock_max) { return; }
  if (!s_pm && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pendrive", &s_pm) != ESP_OK) { s_pm = nullptr; }
  if (!s_pm) { return; }
  if (sim) { esp_pm_lock_acquire(s_pm); } else { esp_pm_lock_release(s_pm); }
  s_clock_max = sim;
}

static bool usb_ligar(void)
{
  if (s_ligado) { return true; }
  if (!s_parou) { s_parou = xSemaphoreCreateBinary(); }
  if (!s_iniciou) { s_iniciou = xSemaphoreCreateBinary(); }
  if (!s_parou || !s_iniciou) { return false; }

  // The card through the hal gates; the 64 KB buffers (write and read) in PSRAM.
  if (!tab5_sd_cru_info(&s_setores, &s_tam) || !s_tam) { return false; }
  if (!buffers_alocar()) { ESP_LOGE(TAG, "out of memory for the USB drive buffers"); return false; }
  s_tem_cartao = true;
  s_cartao_falhou = false;
  s_n_descargas = 0; s_kb_descargas = 0; s_n_cache_ok = 0; s_n_cache_falta = 0;
  s_ejetado = false; s_suspenso = false; s_parar = false;
  s_lidos = 0; s_gravados = 0; s_erros = 0;
  s_n_config = 0; s_n_inquiry = 0; s_n_capacidade = 0; s_n_leituras = 0; s_n_leituras_ok = 0;
  s_op_recusado = 0; s_leitura_max_us = 0;
  memset(s_ops_logados, 0, sizeof(s_ops_logados));
  s_n_tur = 0; s_n_sense = 0; s_n_respostas = 0; s_op_respondido = 0; s_n_soltou = 0;
  memset(s_ops_respondidos, 0, sizeof(s_ops_respondidos));
  s_erro_setor = 0; s_erro_cod = 0;
  s_n_cmd_leitura = 0; s_lendo = false; s_no_cartao = false;
  s_cmd_setor = 0; s_ultimo_setor = 0; s_evento_us = 0;
  s_n_pad_religado = 0;
  s_gravando = false; s_escrita_max_us = 0; s_n_escritas_lentas = 0;
  uint8_t mac[6] = { };
  esp_read_mac(mac, ESP_MAC_BASE);
  snprintf(s_serie, sizeof(s_serie), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  s_ultimo_io_us = 0; s_ultima_escrita_us = 0;

  ESP_LOGI(TAG, "turning on: the console leaves the USB-C until USB drive mode ends");
  vTaskDelay(pdMS_TO_TICKS(50));   // lets the line above reach the computer

  phy_para_o_otg();
  static const usb_phy_otg_io_conf_t io = USB_PHY_SELF_POWERED_DEVICE(-1);
  usb_phy_config_t cfg = { };
  cfg.controller  = USB_PHY_CTRL_OTG;
  cfg.target      = USB_PHY_TARGET_INT;      // the full-speed PHY (now 0)
  cfg.otg_mode    = USB_OTG_MODE_DEVICE;
  cfg.otg_speed   = USB_PHY_SPEED_FULL;      // without this IDF switches to the high-speed PHY
  cfg.otg_io_conf = &io;
  if (usb_new_phy(&cfg, &s_phy) != ESP_OK) {
    ESP_LOGE(TAG, "OTG PHY refused");
    s_phy = nullptr;
    phy_para_o_console();
    s_tem_cartao = false; buffers_soltar();
    return false;
  }

  // HIGH PRIORITY, above the interface: on mounting, the computer reads the whole FAT
  // table at once, and macOS gives up (I/O error, the disk does not even
  // appear) if it takes too long. A screen-drawing round ahead of every
  // request was pure delay. The task almost always sleeps waiting for USB.
  // The stack is TinyUSB's own plus the call chain down to the card driver.
  xSemaphoreTake(s_iniciou, 0);   // no leftover from a previous time
  xTaskCreatePinnedToCore(vTaskPendrive, "pendrive", 4096, nullptr, configMAX_PRIORITIES - 2, &s_tarefa, PENDRIVE_NUCLEO);
  if (!s_tarefa) {
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
    usb_del_phy(s_phy); s_phy = nullptr;
    phy_para_o_console();
    s_tem_cartao = false; buffers_soltar();
    return false;
  }
  xSemaphoreTake(s_iniciou, portMAX_DELAY);
  if (!s_init_ok) {
    ESP_LOGE(TAG, "TinyUSB did not start");
    xSemaphoreTake(s_parou, portMAX_DELAY);   // the task is already exiting
    s_tarefa = nullptr;
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
    usb_del_phy(s_phy); s_phy = nullptr;
    phy_para_o_console();
    s_tem_cartao = false; buffers_soltar();
    return false;
  }
  tab5_dono_registrar(s_tarefa, DONO_SERVICO, "Pendrive");
  clock_maximo(true);
  tud_connect();
  s_ligado = true;
  ESP_LOGI(TAG, "USB up: mode %s, core %d", usb_modo(), PENDRIVE_NUCLEO);
  return true;
}

static void usb_desligar(void)
{
  if (!s_ligado) { return; }
  tud_disconnect();                       // the computer sees the disk go
  vTaskDelay(pdMS_TO_TICKS(30));
  s_parar = true;
  // The task finishes the command in progress (at most one 4 KB read/write),
  // shuts TinyUSB down and exits; only after that does USB stop touching
  // the card.
  //
  // WAIT WITHOUT A TIMEOUT. Carrying on after a timeout would release the PHY from under
  // TinyUSB still running and, straight after (tab5_sd_devolver), free the
  // raw card the task may be using in the middle of an
  // sdmmc_read/write_sectors - memory freed while in use and the Tab5's FAT
  // mounting on top of a computer write. Every card command
  // has its own timeout in the driver: the task always comes back.
  while (xSemaphoreTake(s_parou, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGW(TAG, "the USB task has not stopped yet (%s) - waiting",
             s_no_cartao ? (s_gravando ? "card write" : "card read") : "USB");
  }
  s_tarefa = nullptr;
  usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);   // still with the USB_WRAP clock
  if (s_phy) { usb_del_phy(s_phy); s_phy = nullptr; }
  phy_para_o_console();
  s_tem_cartao = false;
  buffers_soltar();
  s_ligado = false;
  clock_maximo(false);
  vTaskDelay(pdMS_TO_TICKS(50));
  log_cartao();
  ESP_LOGI(TAG, "off: console back (%llu KB read from the card, %llu KB written, %u errors)",
           (unsigned long long)(s_lidos / 1024), (unsigned long long)(s_gravados / 1024),
           (unsigned)s_erros);
}

void tab5_pendrive_usb(tab5_pendrive_usb_t* e)
{
  const int64_t agora = esp_timer_get_time();
  e->ligado   = s_ligado;
  e->montado  = s_ligado && tud_mounted();
  e->suspenso = s_ligado && s_suspenso;
  e->ejetado  = s_ejetado;
  e->ativo    = s_ultimo_io_us && (agora - s_ultimo_io_us) < 2000000;
  e->lidos    = s_lidos;
  e->gravados = s_gravados;
  e->erros    = s_erros;
  e->ms_sem_escrita = s_ultima_escrita_us ? (uint32_t)((agora - s_ultima_escrita_us) / 1000) : UINT32_MAX;
  e->n_config     = s_n_config;
  e->n_inquiry    = s_n_inquiry;
  e->n_capacidade = s_n_capacidade;
  e->n_leituras   = s_n_leituras;
  e->op_recusado  = s_op_recusado;
  e->leitura_max_ms = s_leitura_max_us / 1000;
  e->n_tur        = s_n_tur;
  e->n_sense      = s_n_sense;
  e->n_respostas  = s_n_respostas;
  e->op_respondido = s_op_respondido;
  e->n_soltou     = s_n_soltou;
  e->n_leituras_ok = s_n_leituras_ok;
  e->n_cmd_leitura = s_n_cmd_leitura;
  e->ultimo_setor  = s_ultimo_setor;
  e->parado_ms = s_evento_us ? (uint32_t)((agora - s_evento_us) / 1000) : 0;
  e->onde = !s_evento_us ? PENDRIVE_ESPERA_NADA
          : s_no_cartao  ? PENDRIVE_ESPERA_CARTAO
          : s_lendo      ? PENDRIVE_ESPERA_USB
          :                PENDRIVE_ESPERA_COMPUTADOR;
}

bool tab5_pendrive_pc_presente(void)
{
  // With USB drive mode on JTAG sees nothing - OTG is what answers.
  if (s_ligado) { return tud_connected() && !s_suspenso; }
  return usb_serial_jtag_is_connected();
}

void tab5_pendrive_console_reanunciar(void)
{
  if (s_ligado || usb_serial_jtag_is_connected()) { return; }
  const usb_serial_jtag_pull_override_vals_t solto = { .dp_pu = false, .dm_pu = false, .dp_pd = false, .dm_pd = false };
  usb_serial_jtag_ll_phy_enable_pull_override(&solto);   // D+ without pull-up: "cable out"
  vTaskDelay(pdMS_TO_TICKS(150));
  usb_serial_jtag_ll_phy_disable_pull_override();        // the controller reconnects: "cable back"
  ESP_LOGI(TAG, "console re-announced to the computer (the cable arrived with the chip asleep)");
}

// ===========================================================================
// THE SESSION - lend, USB, give back (see the .hpp)
// ===========================================================================
static volatile tab5_pendrive_fase_t   s_fase = PENDRIVE_PARADO;
static volatile tab5_pendrive_motivo_t s_motivo = PENDRIVE_OK;
static volatile bool s_pedido_devolver = false;
static volatile bool s_devolvido_por_ejecao = false;
// The last session ended with the disk MOUNTED on the computer, without ejecting: the
// card's FAT was left marked "not unmounted". Next time macOS
// (and Windows) checks the whole card before showing the disk - over full-speed
// USB, on a big card, that takes a while, and nothing appears until it finishes.
static volatile bool s_saiu_sem_ejetar = false;
static volatile bool s_oferta = false;     // computer connected, waiting for the person to decide
static volatile bool s_resultado_aviso = false;   // conflict/pending item not yet seen in Settings > USB
static int           s_notif = -1;
static portMUX_TYPE  s_mux_sessao = portMUX_INITIALIZER_UNLOCKED;

static void avisar(int quantidade, notif_nivel_t nivel, const char* txt)
{
  if (s_notif >= 0) { notif_publicar(s_notif, quantidade, nivel, txt); }
}

// The reason in words, for the log and the console (the screen has its own texts in
// Settings > USB). Declared in the .hpp.
const char* tab5_pendrive_motivo_texto(tab5_pendrive_motivo_t m)
{
  switch (m) {
    case PENDRIVE_RECUSA_APP:        return tr("um app aberto esta usando o cartao");
    case PENDRIVE_RECUSA_EM_USO:     return tr("cartao em uso por um servico alem do prazo");
    case PENDRIVE_RECUSA_SEM_CARTAO: return tr("sem cartao, ou ele nao respondeu");
    case PENDRIVE_RECUSA_SEM_DIARIO: return "falta a particao reserva (diario)";
    case PENDRIVE_RECUSA_USB:        return tr("o USB nao subiu");
    case PENDRIVE_OK:                return tr("nenhum (ligou, ou nunca tentou)");
    default:                         return "?";
  }
}

// An open app requires the card, or has it mounted for itself: the system does not pull
// the card from under whoever holds it (app.h, usa_sd/exige_sd).
static bool app_com_cartao(void) { return sistema_app_exige_sd() || sistema_app_segura_sd(); }

// Ends the session with the given reason (USB drive mode never turned on). Goes to the
// log: without this, a refusal only showed up by asking the console.
static void recusou(tab5_pendrive_motivo_t m)
{
  const char* app = sistema_app_atual();
  if (m == PENDRIVE_RECUSA_APP && app) {
    ESP_LOGW(TAG, "USB drive refused: %s ('%s')", tab5_pendrive_motivo_texto(m), app);
  } else {
    ESP_LOGW(TAG, "USB drive refused: %s", tab5_pendrive_motivo_texto(m));
  }
  s_motivo = m;
  s_fase = PENDRIVE_PARADO;
  avisar(0, NOTIF_INFO, nullptr);
}

// THE SESSION LOG: each computer step once (recognised, asked what it
// is, the size, read, each command answered or refused, card error,
// eject) and a summary when something changes - every 3 s until the first read
// delivered, every 10 s afterwards. "leit 12/11" = 12 read commands
// received, 11 with data and status delivered. And, when everything stops for
// more than 3 s, WHERE it stopped (see tab5_pendrive_espera_t).
typedef struct {
  uint32_t config, inquiry, capacidade, leituras, erros, soltou, marca;
  uint32_t ops_ok[8], ops_recusa[8];
  bool     ejetado;
  bool     suspenso;
  int64_t  resumo_us;
  bool     parada_contada;
} passos_t;

// Coalesced writes, read cache and card recoveries (tab5_sd_cru_estat).
static void log_cartao(void)
{
  tab5_sd_cru_estat_t c;
  tab5_sd_cru_estat(&c);
  const uint32_t nd = s_n_descargas;
  ESP_LOGI(TAG, "card: %lu flushes (average %lu KB), reads: %lu from cache / %lu from card, "
           "recoveries %lu, failures %lu, slowest %lu ms",
           (unsigned long)nd, (unsigned long)(nd ? s_kb_descargas / nd : 0),
           (unsigned long)s_n_cache_ok, (unsigned long)s_n_cache_falta,
           (unsigned long)c.recuperacoes, (unsigned long)c.falhas, (unsigned long)c.max_ms);
}

static void registrar_eventos(passos_t* d, const tab5_pendrive_usb_t* u)
{
  if (u->n_config > d->config) {
    ESP_LOGI(TAG, "computer recognised the USB device%s", d->config ? " (again)" : "");
    d->config = u->n_config;
  }
  if (u->n_inquiry && !d->inquiry) { ESP_LOGI(TAG, "computer asked what the disk is (INQUIRY)"); }
  d->inquiry = u->n_inquiry;
  if (u->n_capacidade && !d->capacidade) {
    ESP_LOGI(TAG, "computer asked for the size: %lu sectors of %u",
             s_tem_cartao ? (unsigned long)s_setores : 0UL, (unsigned)setor_tam());
  }
  d->capacidade = u->n_capacidade;
  if (u->n_leituras && !d->leituras) { ESP_LOGI(TAG, "computer started reading the disk"); }
  d->leituras = u->n_leituras;
  for (int i = 0; i < 8; ++i) {
    const uint32_t novos_ok = s_ops_respondidos[i] & ~d->ops_ok[i];
    const uint32_t novas_recusas = s_ops_logados[i] & ~d->ops_recusa[i];
    for (int b = 0; b < 32; ++b) {
      if (novos_ok & (1u << b))      { ESP_LOGI(TAG, "response to SCSI command 0x%02X delivered", i * 32 + b); }
      if (novas_recusas & (1u << b)) { ESP_LOGW(TAG, "SCSI command 0x%02X not supported (refused)", i * 32 + b); }
    }
    d->ops_ok[i] |= novos_ok;
    d->ops_recusa[i] |= novas_recusas;
  }
  if (u->erros > d->erros) {
    ESP_LOGE(TAG, "card failed %lu time(s), the last at sector %lu: %s", (unsigned long)(u->erros - d->erros),
             (unsigned long)s_erro_setor, esp_err_to_name(s_erro_cod));
    d->erros = u->erros;
  }
  if (u->n_soltou > d->soltou) { ESP_LOGI(TAG, "computer released the disk"); d->soltou = u->n_soltou; }
  if (u->suspenso != d->suspenso) {
    // No SOF from the computer for 3 ms: cable out, computer asleep - or the
    // computer stopped seeing the Tab5 (see pad_conferir).
    ESP_LOGI(TAG, "bus %s", u->suspenso ? "stopped (suspended)" : "back");
    d->suspenso = u->suspenso;
  }
  if (u->ejetado != d->ejetado) {
    ESP_LOGI(TAG, "%s by the computer", u->ejetado ? "ejected" : "loaded");
    d->ejetado = u->ejetado;
  }

  const uint32_t marca = u->n_config + u->n_inquiry + u->n_tur + u->n_sense + u->n_capacidade
                       + u->n_respostas + u->n_soltou + u->op_recusado + u->n_leituras
                       + u->n_leituras_ok + u->n_cmd_leitura + u->erros + (uint32_t)(u->gravados >> 9);
  const int64_t intervalo = u->n_leituras_ok ? 10000000 : 3000000;
  const int64_t agora = esp_timer_get_time();
  if (marca != d->marca && agora - d->resumo_us > intervalo) {
    d->marca = marca;
    d->resumo_us = agora;
    // Deliberately short: the log cuts lines at 200 characters.
    ESP_LOGI(TAG, "USB: cfg %lu inq %lu pronto %lu sense %lu tam %lu resp %lu (0x%02X) solt %lu"
             " | leit %lu/%lu = %lu KB, grav %lu KB, erros %lu, max %lu/%lu ms (%lu lentas)",
             (unsigned long)u->n_config, (unsigned long)u->n_inquiry, (unsigned long)u->n_tur,
             (unsigned long)u->n_sense, (unsigned long)u->n_capacidade, (unsigned long)u->n_respostas,
             (unsigned)u->op_respondido, (unsigned long)u->n_soltou,
             (unsigned long)u->n_cmd_leitura, (unsigned long)u->n_leituras_ok, (unsigned long)(u->lidos / 1024),
             (unsigned long)(u->gravados / 1024), (unsigned long)u->erros, (unsigned long)u->leitura_max_ms,
             (unsigned long)(s_escrita_max_us / 1000), (unsigned long)s_n_escritas_lentas);
    log_cartao();
  }

  // STOPPED: nothing from the computer or the card for more than 3 s, after the
  // computer recognised the device. One line per stop.
  if (u->n_config && u->parado_ms >= 3000) {
    if (!d->parada_contada) {
      d->parada_contada = true;
      switch (u->onde) {
        case PENDRIVE_ESPERA_CARTAO:
          ESP_LOGW(TAG, "STOPPED for %lu s: stuck on the CARD (%s of sector %lu did not return)",
                   (unsigned long)(u->parado_ms / 1000), s_gravando ? "write" : "read",
                   (unsigned long)u->ultimo_setor);
          break;
        case PENDRIVE_ESPERA_USB:
          ESP_LOGW(TAG, "STOPPED for %lu s: stuck on USB - read from sector %lu started, the card "
                   "delivered up to %lu and the status did not go out (%lu/%lu commands)",
                   (unsigned long)(u->parado_ms / 1000), (unsigned long)s_cmd_setor,
                   (unsigned long)u->ultimo_setor, (unsigned long)u->n_leituras_ok, (unsigned long)u->n_cmd_leitura);
          break;
        default: {
          // Was the Tab5 LISTENING? The OUT endpoint (where the next command
          // arrives) has to be enabled (EPENA) and not STALLed.
          // Disarmed or stalled = the computer sent and the Tab5 did not receive.
          const uint32_t doepctl1 = *(volatile const uint32_t*)(0x50040000UL + 0xB20);
          const uint32_t diepctl1 = *(volatile const uint32_t*)(0x50040000UL + 0x920);
          ESP_LOGW(TAG, "STOPPED for %lu s: nothing new from the COMPUTER - everything delivered (%lu/%lu reads, "
                   "last 0x%02X, %lu KB)",
                   (unsigned long)(u->parado_ms / 1000), (unsigned long)u->n_leituras_ok,
                   (unsigned long)u->n_cmd_leitura, (unsigned)u->op_respondido, (unsigned long)(u->lidos / 1024));
          ESP_LOGW(TAG, "  listening for the next command? %s%s (out %08lX, in %08lX%s)",
                   (doepctl1 & (1u << 31)) ? "yes" : "NO (disarmed)",
                   (doepctl1 & (1u << 21)) ? ", out STALLED" : "",
                   (unsigned long)doepctl1, (unsigned long)diepctl1,
                   (diepctl1 & (1u << 21)) ? ", in STALLED" : "");
          break;
        }
      }
    }
  } else if (u->parado_ms < 3000 && d->parada_contada) {
    d->parada_contada = false;
    ESP_LOGI(TAG, "moving again");
  }
}

// Suspended for longer than this = nobody on the other side (see the loop below). The
// bus suspends by itself after 3 ms without SOF; a computer that just
// blips (port power change) comes back well before this.
#define BARRAMENTO_PARADO_US 2000000
// On and no computer has configured the device by now: there is nobody.
#define ESPERA_PC_US         10000000

static void vTaskSessao(void*)
{
  // 1. The card. An app that requires it, or has it mounted for itself, cannot
  //    be open: the system does not pull the card from under it. Refuse on the
  //    spot, without the lending wait.
  if (app_com_cartao()) { recusou(PENDRIVE_RECUSA_APP); return; }
  switch (tab5_sd_emprestar(3000)) {
    case SD_EMPRESTADO:             break;
    // An app may have opened during the wait: the right reason is its.
    case SD_EMPRESTIMO_EM_USO:
      recusou(app_com_cartao() ? PENDRIVE_RECUSA_APP : PENDRIVE_RECUSA_EM_USO);
      return;
    case SD_EMPRESTIMO_SEM_DIARIO:  recusou(PENDRIVE_RECUSA_SEM_DIARIO); return;
    default:                        recusou(PENDRIVE_RECUSA_SEM_CARTAO); return;
  }
  // 2. USB. First, the speaker leaves the pins ESP-IDF confuses with
  //    the OTG's (see pad_conferir) - off now, BEFORE USB comes up, and
  //    muted until the end. Failed: the card comes back at once, nothing changed for anyone.
  if (!sistema_som_ceder_pinos(3000)) {
    ESP_LOGW(TAG, "the interface did not silence the speaker within 3 s - carrying on (the pad watchdog covers it)");
  }
  if (!usb_ligar()) {
    sistema_som_devolver_pinos();
    tab5_sd_devolver();
    recusou(PENDRIVE_RECUSA_USB);
    return;
  }
  s_fase = PENDRIVE_ATIVO;
  avisar(1, NOTIF_INFO, "Cartão no computador");

  // 3. While the computer has the card. Exits when it ejects (nothing
  //    more cached on that side) or when the person asks for it back (the screen
  //    asks for confirmation first, if it is still mounted).
  tab5_sd_resumo_t r0;
  tab5_sd_resumo(&r0);
  bool avisou_cheio = false;
  // THE PATH TO THE DISK APPEARING, in the log (which goes to the journal and reaches the
  // card on return) - written HERE, never in the USB callbacks.
  passos_t d = { };
  int64_t suspenso_desde_us = 0;
  bool    economia = false;
  bool    cartao_falhou_sessao = false;
  const int64_t inicio_us = esp_timer_get_time();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    if (pad_conferir()) {
      ESP_LOGE(TAG, "the OTG USB pad was turned off externally (pins 26/27) - reconnected (%lu time(s))",
               (unsigned long)s_n_pad_religado);
    }
    tab5_pendrive_usb_t u;
    tab5_pendrive_usb(&u);
    registrar_eventos(&d, &u);
    // THE JOURNAL FILLING UP: over 90%, or it has already lost writes - the person
    // needs to know so they give the card back before losing more.
    tab5_sd_resumo_t rr;
    tab5_sd_resumo(&rr);
    const bool cheio = rr.perdidas > r0.perdidas
                    || (rr.diario_total && (uint64_t)rr.diario_usado * 10 >= (uint64_t)rr.diario_total * 9);
    if (cheio != avisou_cheio) {
      avisou_cheio = cheio;
      if (cheio) { avisar(1, NOTIF_ATENCAO, tr("Memória do pendrive cheia")); }
      else       { avisar(1, NOTIF_INFO, "Cartão no computador"); }
    }
    // BUS STOPPED for more than BARRAMENTO_PARADO_US: cable pulled without
    // ejecting, or computer asleep. TinyUSB stays "mounted" (suspending
    // does not deconfigure, and without a VBUS sensor a pulled cable is just that) - without
    // this rule the screen stayed on and the CPU at maximum clock FOREVER, on
    // battery, waiting for a computer that is no longer there. The session carries on
    // (the computer's cache may hold something from the card: only ejecting or the person
    // gives it back); only what uses power goes, and comes back when the bus
    // comes back.
    // The same for USB drive mode on WITHOUT a computer (from the screen, no cable):
    // after ESPERA_PC_US without the computer configuring the device, the clock
    // drops. Enumeration does not need it; when the computer configures, the
    // clock goes up on this round (200 ms), before the heavy FAT read.
    const int64_t agora = esp_timer_get_time();
    if (!u.suspenso)             { suspenso_desde_us = 0; }
    else if (!suspenso_desde_us) { suspenso_desde_us = agora; }
    const bool parado = suspenso_desde_us && agora - suspenso_desde_us >= BARRAMENTO_PARADO_US;
    const bool sem_pc = !u.montado && agora - inicio_us >= ESPERA_PC_US;
    const bool economia_agora = parado || sem_pc;
    if (economia_agora != economia) {
      economia = economia_agora;
      ESP_LOGI(TAG, "%s", !economia ? "computer talking: CPU at maximum"
                        : parado    ? "bus stopped: screen and CPU released until the computer comes back"
                        :             "no computer configuring the disk: CPU released until one arrives");
    }
    clock_maximo(!economia);
    // Disk mounted: screen on - idle mode would lower the clock mid-copy,
    // and the person needs to see the notice that the card is out.
    sistema_recurso(RECURSO_TELA_ACESA, "pendrive", u.montado && !u.ejetado && !parado);
    // THE CARD FAILED FOR GOOD (even after the hal's recoveries): the
    // computer already gets "no media" and the session ends here, instead of staying
    // stuck forever. Also applies to a deferred flush that failed: the
    // command had already been confirmed to the computer.
    if (s_cartao_falhou) {
      ESP_LOGE(TAG, "card failed in USB drive mode (sector %lu: %s) - ending the session",
               (unsigned long)s_erro_setor, esp_err_to_name(s_erro_cod));
      log_cartao();
      cartao_falhou_sessao = true;
      s_devolvido_por_ejecao = false;
      s_saiu_sem_ejetar = u.montado;
      break;
    }
    if (u.ejetado || s_pedido_devolver) {
      s_devolvido_por_ejecao = u.ejetado;
      s_saiu_sem_ejetar = !u.ejetado && u.montado;
      break;
    }
  }

  // 4. Give back: USB goes FIRST (the computer stops touching the card) and
  //    only then does the card return to the Tab5. The journal is applied right here (the
  //    write barrier waits for it to finish) so the result is the current one.
  s_fase = PENDRIVE_DEVOLVENDO;
  avisar(1, NOTIF_INFO, "Devolvendo o cartão...");
  usb_desligar();
  sistema_som_devolver_pinos();
  sistema_recurso(RECURSO_TELA_ACESA, "pendrive", false);
  tab5_sd_devolver();
  tab5_sd_gravar_pendentes();   // applies the journal now (and waits)

  tab5_sd_resumo_t r;
  tab5_sd_resumo(&r);
  if (cartao_falhou_sessao) {
    avisar(1, NOTIF_ATENCAO, tr("Cartão falhou no pendrive"));   // stays until the person sees it in Settings > USB
    s_resultado_aviso = true;
  } else if (r.conflitos) {
    char t[NOTIF_RESUMO];
    snprintf(t, sizeof(t), tr("%u arquivo(s) em .tab5"), (unsigned)r.conflitos);
    avisar((int)r.conflitos, NOTIF_ATENCAO, t);   // stays until the person sees it in Settings > USB
    s_resultado_aviso = true;
  } else if (r.no_diario) {
    avisar((int)r.no_diario, NOTIF_ATENCAO, tr("Gravações esperando cartão"));   // fits in NOTIF_RESUMO
    s_resultado_aviso = true;
  } else {
    avisar(0, NOTIF_INFO, nullptr);
  }
  s_motivo = PENDRIVE_OK;
  s_pedido_devolver = false;
  s_fase = PENDRIVE_PARADO;
  ESP_LOGI(TAG, "USB drive mode ended (%s): %u journal write(s) applied, %u conflict(s)",
           s_devolvido_por_ejecao ? "ejected on the computer" : "given back on the Tab5",
           (unsigned)r.aplicadas, (unsigned)r.conflitos);
  // The computer may have added or removed apps on the card: the home screen follows.
  sistema_apps_de_fora_reler();
  // The computer should have seen the console again by now (goes to the card log).
  vTaskDelay(pdMS_TO_TICKS(3000));
  registro_usb("3 s depois");
  tab5_log_descarregar_logo();
}

bool tab5_pendrive_iniciar(void)
{
  taskENTER_CRITICAL(&s_mux_sessao);
  const bool livre = (s_fase == PENDRIVE_PARADO);
  if (livre) { s_fase = PENDRIVE_PREPARANDO; }
  taskEXIT_CRITICAL(&s_mux_sessao);
  if (!livre) { return false; }

  s_motivo = PENDRIVE_OK;
  s_pedido_devolver = false;
  s_devolvido_por_ejecao = false;
  s_oferta = false;
  s_resultado_aviso = false;
  if (app_com_cartao()) { recusou(PENDRIVE_RECUSA_APP); return false; }
  avisar(1, NOTIF_INFO, "Preparando o pendrive...");
  // Stack of the "cartao" task: lending snapshots the card (walks the folders)
  // and f_mount goes deep. Pool slot G (fixed 6 KB): one session at a time,
  // without creating and destroying 6 KB of SRAM on every trip to the computer.
  if (!pool_criar(nullptr, vTaskSessao, nullptr, "pendrive_sessao", 6144, 3)) {
    recusou(PENDRIVE_RECUSA_USB);
    return false;
  }
  return true;
}

// Also applies while PREPARANDO: the request is kept and the session gives back as soon as
// USB comes up (or never uses it, if refused first) - discarding silently
// left the card on the computer after the person had asked for it back.
void tab5_pendrive_devolver(void)
{
  const tab5_pendrive_fase_t f = s_fase;
  if (f == PENDRIVE_ATIVO || f == PENDRIVE_PREPARANDO) { s_pedido_devolver = true; }
}

void tab5_pendrive_estado(tab5_pendrive_estado_t* e)
{
  tab5_pendrive_usb(&e->usb);
  e->fase = s_fase;
  e->motivo = s_motivo;
  e->por_ejecao = s_devolvido_por_ejecao;
  e->sem_ejetar = s_saiu_sem_ejetar;
  e->oferta = s_oferta;
}

bool tab5_pendrive_pede_atencao(void) { return s_fase != PENDRIVE_PARADO || s_oferta || s_resultado_aviso; }

void tab5_pendrive_resultado_visto(void)
{
  if (!s_resultado_aviso || s_fase != PENDRIVE_PARADO) { return; }
  s_resultado_aviso = false;
  avisar(0, NOTIF_INFO, nullptr);
}

// ---------------------------------------------------------------------------
// THE WATCH: computer connected to the cable
// ---------------------------------------------------------------------------
// Every 250 ms (esp_timer). On the EDGE "no computer -> computer", with
// the setting on: on the home screen, unlocked, USB drive mode turns on by itself; with
// an app open or the screen locked, it only notifies (the notice opens Settings > USB).
// A GRACE PERIOD, in which the edge does not count:
//   - at boot (5 s): the computer already connected is the one that just flashed the
//     firmware - its console reappears right after the restart, and stealing the
//     cable would take away the serial monitor;
//   - after a USB drive session (4 s): the console returns to the USB-C and the computer
//     "reappears" without the cable having left.
// Before, the rule was "only after 3 s with the cable out" (checking once per
// second): unplugging and replugging the cable faster than that did nothing - and it is
// the natural way to mount again after ejecting.
// With USB drive mode on JTAG sees nothing: the watch does not look.
#define VIGIA_US          250000
#define VIGIA_LENTA_US    1000000   // setting off (see the end of vigiar)
#define VIGIA_SEM_CABO_US 5000000   // no cable on the USB-C (see the end of vigiar)
#define CARENCIA_BOOT_US  5000000
#define CARENCIA_FIM_US   4000000
static esp_timer_handle_t s_vigia = nullptr;
static uint64_t s_vigia_periodo = VIGIA_US;
static bool    s_viu = false, s_antes = false;
static int64_t s_carencia_ate_us = 0;
static int64_t s_ausente_desde_us = 0;

static void vigiar(void*)
{
  const int64_t agora = esp_timer_get_time();
  if (s_fase != PENDRIVE_PARADO || tab5_sd_emprestado()) {
    s_antes = true;
    s_carencia_ate_us = agora + CARENCIA_FIM_US;   // counts from the end of the session
    return;
  }

  const bool pc = tab5_pendrive_pc_presente();
  if (!s_viu) { s_viu = true; s_antes = pc; s_carencia_ate_us = agora + CARENCIA_BOOT_US; return; }

  if (s_oferta && !ajustes_pendrive()) {   // the setting went off: so does the offer
    s_oferta = false;
    avisar(0, NOTIF_INFO, nullptr);
  }
  if (pc) {
    s_ausente_desde_us = 0;
    if (!s_antes && agora >= s_carencia_ate_us && ajustes_pendrive()) {
      const bool trancado = sistema_bloqueado() || (tab5_power_em_repouso() && ajustes_bloqueio());
      if (!trancado && !sistema_app_atual()) {
        ESP_LOGI(TAG, "computer on USB-C: turning USB drive mode on");
        tab5_pendrive_iniciar();
      } else {
        s_oferta = true;
        avisar(1, NOTIF_INFO, tr("Computador no USB: toque"));
      }
    }
  } else {
    if (!s_ausente_desde_us) { s_ausente_desde_us = agora; }
    // The offer only drops with the cable really out, not on a bus hiccup.
    if (s_oferta && agora - s_ausente_desde_us >= 1000000) { s_oferta = false; avisar(0, NOTIF_INFO, nullptr); }
  }
  s_antes = pc;

  // THE PACE FOLLOWS THE SETTING. With "turn on by itself" off (the factory default) the
  // edge turns nothing on - the watch only tracks the cable so as not to confuse a
  // computer that WAS ALREADY connected with one that has just arrived when the
  // setting is turned on. For that 1 s is enough; 250 ms only when the edge counts.
  // NO CABLE (the battery service sees it from the current and declares RECURSO_ENERGIA_EXTERNA)
  // there is no computer to see: 5 s, just to notice the cable arriving - a
  // self-starting USB drive may take a few seconds longer. Before it was 1 to
  // 4 wake-ups per second, on battery and with the screen off.
  const bool cabo = sistema_recurso_em_uso(RECURSO_ENERGIA_EXTERNA);
  const uint64_t periodo = !cabo && !s_oferta ? VIGIA_SEM_CABO_US
                         : (ajustes_pendrive() || s_oferta) ? VIGIA_US : VIGIA_LENTA_US;
  if (periodo != s_vigia_periodo) {
    s_vigia_periodo = periodo;
    esp_timer_restart(s_vigia, periodo);
  }
}

void tab5_pendrive_servico_iniciar(void)
{
  if (s_vigia) { return; }
  s_notif = notif_registrar("Pendrive");
  notif_associar_app(s_notif, "Ajustes");   // the notice opens Settings (on the USB page)
  const esp_timer_create_args_t a = { .callback = vigiar, .arg = nullptr,
                                      .dispatch_method = ESP_TIMER_TASK, .name = "pendrive_vigia",
                                      .skip_unhandled_events = true };
  if (esp_timer_create(&a, &s_vigia) == ESP_OK) { esp_timer_start_periodic(s_vigia, VIGIA_US); }
}
