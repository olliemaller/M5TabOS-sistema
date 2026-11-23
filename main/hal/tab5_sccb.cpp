// THE CAMERA SCCB THROUGH THE SAME LOCK AS THE REST OF THE INTERNAL I2C.
//
// THE PROBLEM. The internal I2C bus (I2C_NUM_1: GT911 touch, INA226, IMU,
// RTC, expanders, camera sensor) has TWO drivers on top of the same
// controller:
//   - the M5GFX/M5Unified one, which pokes the registers directly and protects each
//     transaction with a per-port mutex (common.cpp, i2c_context_t::lock);
//   - the IDF one (i2c_master), used by the camera sensor's SCCB via esp_video.
// M5GFX itself warns that the two sides "are not serialised with each other - the
// application must not run them at the same time". And esp_video does: the automatic
// gain/exposure control (IPA: AGC, AEN) writes to the sensor DURING
// capture, in its own task, while the UI reads the touch panel. The "camera
// bridge" (kernel/core.cpp) only protected start/stop.
//
// THE SOLUTION. esp_video creates the sensor channel by calling sccb_new_i2c_io()
// (esp_video_init.c:255). The linker redirects that call here
// (-Wl,--wrap=sccb_new_i2c_io, main/CMakeLists.txt) and we hand back an SCCB
// channel whose operations are M5GFX transactions - which go through the
// port lock. From then on ALL internal I2C is serialised by ONE single lock, from
// any task, and no third-party component was edited.
//
// Every SCCB operation boils down to three forms (see the component's sccb_i2c.c):
// write, write-then-read, read. The register/value bytes come already assembled
// in the buffer by the caller; here we only transport them.

#include <stdlib.h>
#include <M5Unified.h>
#include "driver/i2c_master.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_io_interface.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "sccb";

static const int PORTA_INTERNA = 1;   // I2C_NUM_1 - see the numbering warning in tab5-sistema-apps

typedef struct {
  esp_sccb_io_t base;
  uint16_t      endereco;
  uint32_t      freq;
} sccb_lgfx_t;

static inline sccb_lgfx_t* de(esp_sccb_io_t* io) { return __containerof(io, sccb_lgfx_t, base); }

static esp_err_t escrever(esp_sccb_io_t* io, const uint8_t* w, size_t n, int)
{
  if (n > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = de(io);
  return lgfx::i2c::transactionWrite(PORTA_INTERNA, s->endereco, w, (uint8_t)n, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t escrever_ler(esp_sccb_io_t* io, const uint8_t* w, size_t wn, uint8_t* r, size_t rn, int)
{
  if (wn > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = de(io);
  return lgfx::i2c::transactionWriteRead(PORTA_INTERNA, s->endereco, w, (uint8_t)wn, r, rn, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t ler(esp_sccb_io_t* io, uint8_t* r, size_t rn, int)
{
  if (rn > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = de(io);
  return lgfx::i2c::transactionRead(PORTA_INTERNA, s->endereco, r, (uint8_t)rn, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t apagar(esp_sccb_io_t* io)
{
  heap_caps_free(de(io));
  return ESP_OK;
}

extern "C" esp_err_t __real_sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t* cfg,
                                            esp_sccb_io_handle_t* io);

extern "C" esp_err_t __wrap_sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t* cfg,
                                            esp_sccb_io_handle_t* io)
{
  if (!bus || !cfg || !io) { return ESP_ERR_INVALID_ARG; }

  // Only the INTERNAL bus is shared with M5GFX. Any other one takes
  // the component's normal path.
  i2c_master_bus_handle_t interno = nullptr;
  if (i2c_master_get_bus_handle((i2c_port_num_t)PORTA_INTERNA, &interno) != ESP_OK || bus != interno
   || cfg->dev_addr_length != I2C_ADDR_BIT_LEN_7) {
    return __real_sccb_new_i2c_io(bus, cfg, io);
  }

  auto* s = (sccb_lgfx_t*)heap_caps_calloc(1, sizeof(sccb_lgfx_t), MALLOC_CAP_DEFAULT);
  if (!s) { return ESP_ERR_NO_MEM; }
  s->endereco = cfg->device_address;
  s->freq     = cfg->scl_speed_hz ? cfg->scl_speed_hz : 400000;

  s->base.transmit_reg_a8v8   = escrever;
  s->base.transmit_reg_a16v8  = escrever;
  s->base.transmit_reg_a8v16  = escrever;
  s->base.transmit_reg_a16v16 = escrever;
  s->base.transmit_reg_a16v32 = escrever;
  s->base.transmit_reg_a16    = escrever;
  s->base.transmit_receive_reg_a8v8   = escrever_ler;
  s->base.transmit_receive_reg_a16v8  = escrever_ler;
  s->base.transmit_receive_reg_a8v16  = escrever_ler;
  s->base.transmit_receive_reg_a16v16 = escrever_ler;
  s->base.transmit_receive_reg_a16v32 = escrever_ler;
  s->base.transmit_receive_reg_a16    = escrever_ler;
  s->base.transmit_v16 = escrever;
  s->base.receive_v16  = ler;
  s->base.del          = apagar;

  *io = &s->base;
  ESP_LOGI(TAG, "sensor 0x%02X on internal I2C via the M5GFX lock (%lu Hz)",
           (unsigned)s->endereco, (unsigned long)s->freq);
  return ESP_OK;
}
