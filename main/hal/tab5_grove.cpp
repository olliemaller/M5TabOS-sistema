#include "tab5_grove.hpp"
#include "idioma.h"
#include "tab5_marcos.hpp"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#include "tab5_power.hpp"

static const char *TAG = "grove";

// Same pins as Azimutal - there is only one Grove.
#define GROVE_UART   UART_NUM_1
#define GROVE_RX     54
#define GROVE_TX     53

// Time for the module to boot after receiving power. The C6L has an ESP32-C6
// that boots the whole of Meshtastic; 1.2s is ample for it to reach the point
// of answering. Too short here makes the C6L look absent.
// Time for the module to boot AFTER receiving power.
//
// 1.2s was enough for a GPS, which starts talking almost at once. It is NOT
// enough for the Unit C6L: it runs the whole of Meshtastic on an ESP32-C6 and
// takes seconds. With the short wait it was rebooted on every probe and never
// got to answer - the probe said "nothing" with the module plugged in and
// working.
//
// If the rail is ALREADY on ("Grove always on" setting), there is no boot to
// wait for and the probe is immediate.
#define ESPERA_BOOT_MS   4000

// How long to listen at each baud. The GPS sends one sentence per second in
// the worst case, so 1.5s guarantees at least one.
#define ESCUTA_MS        1500

static grove_tipo_t s_tipo = GROVE_NADA;
static grove_dono_t s_dono = GROVE_DONO_NINGUEM;

const char* grove_dono_nome(grove_dono_t d)
{
  switch (d) {
    case GROVE_DONO_MESH: return N_("servico Meshtastic");
    case GROVE_DONO_GPS:  return "GPS";
    default:              return N_("ninguem");
  }
}

grove_dono_t tab5_grove_dono(void) { return s_dono; }

bool tab5_grove_tomar(grove_dono_t quem)
{
  if (s_dono != GROVE_DONO_NINGUEM && s_dono != quem) {
    ESP_LOGW(TAG, "Grove already belongs to %s - %s cannot take over",
             grove_dono_nome(s_dono), grove_dono_nome(quem));
    return false;
  }
  s_dono = quem;
  ESP_LOGI(TAG, "Grove now belongs to %s", grove_dono_nome(quem));
  return true;
}

void tab5_grove_soltar(grove_dono_t quem)
{
  // Only the owner releases. Without this guard, an app closing after having
  // FAILED to take it would free the Grove from whoever was really using it.
  if (s_dono != quem) { return; }
  s_dono = GROVE_DONO_NINGUEM;
  ESP_LOGI(TAG, "Grove released by %s", grove_dono_nome(quem));
}

const char* grove_nome(grove_tipo_t t)
{
  switch (t) {
    case GROVE_GPS:          return N_("GPS (NMEA)");
    case GROVE_MESHTASTIC:   return N_("Meshtastic (C6L)");
    case GROVE_DESCONHECIDO: return N_("desconhecido");
    default:                 return N_("nada");
  }
}

grove_tipo_t tab5_grove_tipo(void) { return s_tipo; }

static bool abrir(int baud)
{
  // Silence the UART driver during the probe.
  //
  // M5.Ex_I2C is initialised at boot on the Grove pins (54/53) and RESERVES
  // the GPIOs. When tab5_teclado points that bus at ExtPort1, M5GFX's
  // release() frees the bus but does not give back the reservation - it is
  // left dangling. uart_set_pin then warns "GPIO 53 is not usable" and
  // configures the pin anyway (the probe works: we measured 209 bytes of NMEA
  // with the warning showing). It is known noise, silenced only here.
  esp_log_level_set("uart", ESP_LOG_ERROR);

  uart_config_t cfg = { };
  cfg.baud_rate = baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  tab5_marco("grove: UART at", baud);   // installs the driver - and its PM lock
  if (uart_driver_install(GROVE_UART, 2048, 512, 0, NULL, 0) != ESP_OK) {
    esp_log_level_set("uart", ESP_LOG_WARN); return false;
  }
  if (uart_param_config(GROVE_UART, &cfg) != ESP_OK) {
    uart_driver_delete(GROVE_UART); esp_log_level_set("uart", ESP_LOG_WARN); return false;
  }
  // Mind the order: uart_set_pin takes TX before RX.
  if (uart_set_pin(GROVE_UART, GROVE_TX, GROVE_RX,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    uart_driver_delete(GROVE_UART);
    esp_log_level_set("uart", ESP_LOG_WARN);
    return false;
  }
  return true;
}

static void fechar(void)
{
  uart_driver_delete(GROVE_UART);
  esp_log_level_set("uart", ESP_LOG_WARN);   // outside the probe the warning applies again
}

// Reads whatever arrives in the window and returns how many bytes it got.
static int escutar(uint8_t* buf, int tam, int ms)
{
  int n = 0;
  const TickType_t fim = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
  while (n < tam && xTaskGetTickCount() < fim)
  {
    const int r = uart_read_bytes(GROVE_UART, buf + n, tam - n, pdMS_TO_TICKS(100));
    if (r > 0) { n += r; }
  }
  return n;
}

// ---------------------------------------------------------------------------
// Meshtastic probe (115200, active)
// ---------------------------------------------------------------------------
// Asks Meshtastic to introduce itself. Called with the UART ALREADY open.
//
// This is mandatory: in PROTO mode the radio does NOT talk by itself, it only
// answers. Passive listening returns zero bytes even with everything working
// - which is what fooled me when using the raw dump to test the C6L.
//
// Sending these bytes to a GPS is harmless: it ignores what it does not
// understand.
static void pedir_apresentacao(void)
{
  // Meshtastic's StreamAPI asks the client to wake the port with 32 START1
  // bytes before the first request - the radio may be in low-power mode on
  // serial and lose the first bytes.
  uint8_t acorda[32];
  memset(acorda, 0x94, sizeof(acorda));
  uart_write_bytes(GROVE_UART, acorda, sizeof(acorda));
  vTaskDelay(pdMS_TO_TICKS(120));

  // ToRadio{ want_config_id = 0x2A }  -- raw protobuf, assembled by hand:
  //   field 3 (want_config_id), varint  -> tag 0x18, value 0x2A
  // Framed: 0x94 0xC3 <len_hi> <len_lo> <payload>
  const uint8_t quadro[] = { 0x94, 0xC3, 0x00, 0x02, 0x18, 0x2A };
  uart_write_bytes(GROVE_UART, quadro, sizeof(quadro));
}

static bool sondar_meshtastic(void)
{
  if (!abrir(115200)) { return false; }
  pedir_apresentacao();

  uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ESCUTA_MS);
  fechar();

  ESP_LOGI(TAG, "probe at 115200 baud (Meshtastic?): %d byte(s) received", n);
  for (int i = 0; i + 1 < n; ++i)
  {
    if (buf[i] == 0x94 && buf[i+1] == 0xC3) { return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------
// GPS probe (9600, passive)
// ---------------------------------------------------------------------------
static bool sondar_gps(int* bytes_vistos)
{
  if (!abrir(9600)) { return false; }
  uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ESCUTA_MS);
  fechar();

  *bytes_vistos = n;
  ESP_LOGI(TAG, "probe at 9600 baud (GPS?): %d byte(s) received", n);

  // NMEA signature: '$' followed by two capital letters (GP, GL, GN, GA...).
  // Requiring the letters avoids matching a '$' that fell in binary garbage.
  for (int i = 0; i + 2 < n; ++i)
  {
    if (buf[i] == '$' && buf[i+1] >= 'A' && buf[i+1] <= 'Z'
                      && buf[i+2] >= 'A' && buf[i+2] <= 'Z') { return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------

void tab5_grove_despejar(int baud, int ms)
{
  // ASK BEFORE TURNING ON - otherwise the answer would always be "yes" (the
  // tab5_power_grove(true) just below). Real hardware state, not the
  // preference (see the comment in tab5_power_grove_ligado()).
  const bool ja_ligado = tab5_power_grove_ligado();
  tab5_power_grove(true);
  // If the rail has just come up, the module is booting: wait. See the
  // comment on ESPERA_BOOT_MS - that is what made the C6L look mute.
  vTaskDelay(pdMS_TO_TICKS(ja_ligado ? 300 : ESPERA_BOOT_MS));

  if (!abrir(baud)) { printf(tr("nao consegui abrir a UART a %d\n"), baud); return; }

  // ALWAYS ask for the introduction. For Meshtastic it is mandatory (it does
  // not talk by itself); for a GPS it is harmless. Without this the dump lies:
  // it shows zero bytes on a module that is perfectly fine, just silent.
  pedir_apresentacao();

  static uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ms);
  fechar();

  printf("--- %d bytes a %d baud ---\n", n, baud);
  for (int i = 0; i < n; i += 16)
  {
    printf("%04d  ", i);
    for (int j = 0; j < 16; ++j) {
      if (i + j < n) { printf("%02X ", buf[i+j]); } else { printf("   "); }
    }
    printf(" |");
    for (int j = 0; j < 16 && i + j < n; ++j) {
      const uint8_t c = buf[i+j];
      printf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    printf("|\n");
  }
  printf("--- fim ---\n");
}

// IS ANYONE ON THE CABLE? One GPIO read, and that is all.
//
// The real probe costs 3 SECONDS - brings up the UART, listens 1.5s at 9600,
// changes baud, asks for an introduction and listens another 1.5s at 115200.
// That cannot run in a loop: it is what made detection a boot-time decision,
// made once and never revisited.
//
// But identifying WHO is there is expensive; noticing that SOMEONE arrived is
// cheap. The data line gives it away: in UART, idle is HIGH, and every module
// holds its TX at that level even when saying nothing. With no module, the
// pin floats.
//
// So: internal pull-down on and one read.
//   module present  -> its TX (push-pull) beats the pull-down  -> HIGH
//   nothing on cable -> only the pull-down holds the pin       -> LOW
//
// Cost: one gpio_config and one gpio_get_level. At 0.5Hz it is statistical
// noise next to anything the screen does.
//
// ONLY WITH THE PORT FREE. With the UART installed the pin belongs to its
// driver, and reconfiguring underneath would break the reading of whoever is
// using it.
bool tab5_grove_presenca(void)
{
  gpio_config_t c = { };
  c.pin_bit_mask = (1ULL << GROVE_RX);
  c.mode         = GPIO_MODE_INPUT;
  c.pull_up_en   = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&c);

  // The pull-down is weak (tens of kohm) and must discharge the pin and cable
  // capacitance. Without this pause the first read gets the OLD level - and a
  // just-removed module would still look present.
  esp_rom_delay_us(500);

  return gpio_get_level((gpio_num_t)GROVE_RX) != 0;
}

// Forgets what was detected. For when the cable goes empty: there is nothing
// to identify, so no probe is paid for to conclude "nothing".
void tab5_grove_esquecer(void) { s_tipo = GROVE_NADA; }

grove_tipo_t tab5_grove_detectar(void)
{
  ESP_LOGI(TAG, "probing the Grove...");

  // Power first: without the 5V no module introduces itself, and the probe
  // would say "nothing" with the device plugged in - the worst kind of wrong
  // answer.
  //
  // HARDWARE STATE, not the "keep always on" preference - the two used to
  // coincide, but they are not the same question (the rail may be on now for
  // another reason even with the preference off). Fixed together with the
  // layer audit - HAL asks HAL, never a service (see docs/ESCREVENDO_APPS.md
  // section 8).
  if (tab5_power_grove_ligado()) {
    ESP_LOGI(TAG, "rail was already on - probing without waiting for boot");
  } else {
    tab5_power_grove(true);
    ESP_LOGI(TAG, "turned the rail on now - waiting %dms for the module to boot", ESPERA_BOOT_MS);
    vTaskDelay(pdMS_TO_TICKS(ESPERA_BOOT_MS));
  }

  // Meshtastic BEFORE GPS. Reason: its probe writes to the line, and writing
  // to a GPS is harmless (it ignores it), whereas the reverse does not hold -
  // a silent Meshtastic would pass for "nothing" if we only listened.
  if (sondar_meshtastic()) { s_tipo = GROVE_MESHTASTIC; }
  else
  {
    int vistos = 0;
    if (sondar_gps(&vistos))  { s_tipo = GROVE_GPS; }
    else if (vistos > 0)      { s_tipo = GROVE_DESCONHECIDO; }
    else                      { s_tipo = GROVE_NADA; }
  }

  ESP_LOGI(TAG, "Grove: %s", grove_nome(s_tipo));
  return s_tipo;
}
