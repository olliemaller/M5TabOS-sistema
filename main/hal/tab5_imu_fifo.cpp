#include "tab5_imu_fifo.hpp"
#include <M5Unified.h>
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "imu_fifo";

// HEADER MODE: each frame starts with a byte saying what follows (gyro,
// accelerometer or both). The gyroscope runs at 200 Hz and the accelerometer
// at 100 by the chip's default, so frames alternate "gyro only" and "gyro +
// accel" - the header is what lets us read that without guessing.
#define BMI_CHIP_ID     0x24
#define BMI_REG_ID      0x00
#define BMI_FIFO_LEN    0x24
#define BMI_FIFO_DATA   0x26
#define BMI_ACC_RANGE   0x41
#define BMI_GYR_CONF    0x42
#define BMI_GYR_RANGE   0x43
#define BMI_FIFO_CFG0   0x48
#define BMI_FIFO_CFG1   0x49
#define BMI_PWR_CONF    0x7C
#define BMI_PWR_CTRL    0x7D
#define BMI_CMD         0x7E
#define BMI_FIFO_FLUSH  0xB0
#define BMI_I2C_HZ      400000
// FIFO_CONFIG_1: bit7 gyro, bit6 accelerometer, bit4 header.
#define BMI_FIFO_LIGADO 0xD0
// The chip's queue is 2 KB: a buffer of the same size reads it all at once,
// and no frame gets cut in half.
#define FIFO_BUF        2048
// GYR_CONF bit6: the gyroscope's low-NOISE mode (the default is low power).
// Costs a few tenths of a mA, only with the FIFO on.
#define BMI_GYR_RUIDO   0x40

static uint8_t  s_bmi = 0;                     // I2C address; 0 = off
static uint8_t  s_fifo_cfg1_antes = 0;
static uint8_t* s_fifo_buf = nullptr;          // internal RAM (I2C copies from there)
static float    s_gyr_lsb = 16.384f;           // counts per degree/s
static float    s_acc_lsb = 4096.0f;           // counts per g
static float    s_gyr_dt  = 0.005f;
static float    s_gyr_dt_nom = 0.005f;         // what the register says
static uint8_t  s_gyr_conf_antes = 0;
// THE REAL RATE. The BMI270's internal clock is not the P4's: if it runs 2%
// slower than nominal, each sample is worth 2% more time and EVERY rotation
// comes out 2% short - 7 degrees per turn. Counting samples against
// esp_timer for a few seconds, the true step comes out with an error of
// hundredths of a per cent.
static int64_t  s_odr_t0 = 0;
static uint32_t s_odr_n = 0;
#define ODR_MEDIR_S   4.0f
#define ODR_TOL       0.06f
// Order within the "gyro + accel" frame. The datasheet says gyro first;
// stationary calibration confirms it (the accelerometer reads ~1 g, the gyro
// ~0) and corrects it if the chip disagrees - getting this wrong would
// silently swap rotation for gravity.
static int      s_ordem_votos = 0;
static bool     s_acel_primeiro = false;
static tab5_imu_fifo_diag_t s_dg;              // counters (UI task only)

bool tab5_imu_fifo_ligado(void) { return s_bmi != 0; }

bool tab5_imu_fifo_ligar(tab5_imu_fifo_info_t* info)
{
  s_bmi = 0;
  for (uint8_t addr : { (uint8_t)0x68, (uint8_t)0x69 }) {
    if (M5.In_I2C.readRegister8(addr, BMI_REG_ID, BMI_I2C_HZ) == BMI_CHIP_ID) { s_bmi = addr; break; }
  }
  if (!s_bmi) { return false; }

  s_fifo_buf = (uint8_t*)heap_caps_malloc(FIFO_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_fifo_buf) { s_bmi = 0; return false; }

  // Gyro and accelerometer on (someone may have left only the accelerometer
  // on to save power - see tab5_power_imu_so_acelerometro).
  const uint8_t pwr = M5.In_I2C.readRegister8(s_bmi, BMI_PWR_CTRL, BMI_I2C_HZ);
  if ((pwr & 0x06) != 0x06) { M5.In_I2C.writeRegister8(s_bmi, BMI_PWR_CTRL, pwr | 0x06, BMI_I2C_HZ); }
  // Advanced power save (PWR_CONF bit0) off: with it the chip may doze
  // between samples. M5Unified already leaves it 0; this just makes sure.
  const uint8_t pconf = M5.In_I2C.readRegister8(s_bmi, BMI_PWR_CONF, BMI_I2C_HZ);
  if (pconf & 0x01) {
    M5.In_I2C.writeRegister8(s_bmi, BMI_PWR_CONF, pconf & ~0x01, BMI_I2C_HZ);
    vTaskDelay(pdMS_TO_TICKS(2));   // the datasheet asks for 450 us before the next write
  }

  // Ranges and rate: read from the chip, not assumed - M5Unified does not
  // change them today, but if it does the heading stays right.
  const uint8_t ar = M5.In_I2C.readRegister8(s_bmi, BMI_ACC_RANGE, BMI_I2C_HZ) & 0x03;
  const uint8_t gr = M5.In_I2C.readRegister8(s_bmi, BMI_GYR_RANGE, BMI_I2C_HZ) & 0x07;
  s_gyr_conf_antes = M5.In_I2C.readRegister8(s_bmi, BMI_GYR_CONF, BMI_I2C_HZ);
  if (!(s_gyr_conf_antes & BMI_GYR_RUIDO)) {
    M5.In_I2C.writeRegister8(s_bmi, BMI_GYR_CONF, s_gyr_conf_antes | BMI_GYR_RUIDO, BMI_I2C_HZ);
  }
  const uint8_t gc = s_gyr_conf_antes & 0x0F;
  s_acc_lsb = (float)(16384 >> ar);                          // 2/4/8/16 g
  s_gyr_lsb = 16.384f * (float)(1 << (gr > 4 ? 4 : gr));     // 2000/1000/500/250/125 dps
  const float odr = 100.0f * powf(2.0f, (float)gc - 8.0f);   // code 8 = 100 Hz, 9 = 200 Hz
  s_gyr_dt = (odr >= 25.0f && odr <= 3200.0f) ? 1.0f / odr : 0.005f;
  s_gyr_dt_nom = s_gyr_dt;
  s_odr_t0 = 0;
  s_odr_n = 0;
  memset(&s_dg, 0, sizeof(s_dg));

  s_fifo_cfg1_antes = M5.In_I2C.readRegister8(s_bmi, BMI_FIFO_CFG1, BMI_I2C_HZ);
  M5.In_I2C.writeRegister8(s_bmi, BMI_FIFO_CFG0, 0x00, BMI_I2C_HZ);   // streaming queue, no clock
  M5.In_I2C.writeRegister8(s_bmi, BMI_FIFO_CFG1, BMI_FIFO_LIGADO, BMI_I2C_HZ);
  M5.In_I2C.writeRegister8(s_bmi, BMI_CMD, BMI_FIFO_FLUSH, BMI_I2C_HZ);
  s_ordem_votos = 0;
  s_acel_primeiro = false;
  ESP_LOGI(TAG, "BMI270 FIFO (0x%02x): gyro %.0f Hz, %.1f LSB/dps, accel %.0f LSB/g", s_bmi,
           (double)(1.0f / s_gyr_dt), (double)s_gyr_lsb, (double)s_acc_lsb);
  if (info) {
    info->gyr_hz  = 1.0f / s_gyr_dt;
    info->gyr_lsb = s_gyr_lsb;
    info->acc_lsb = s_acc_lsb;
  }
  return true;
}

void tab5_imu_fifo_desligar(void)
{
  if (s_bmi) {
    M5.In_I2C.writeRegister8(s_bmi, BMI_FIFO_CFG1, s_fifo_cfg1_antes, BMI_I2C_HZ);
    M5.In_I2C.writeRegister8(s_bmi, BMI_GYR_CONF, s_gyr_conf_antes, BMI_I2C_HZ);
    M5.In_I2C.writeRegister8(s_bmi, BMI_CMD, BMI_FIFO_FLUSH, BMI_I2C_HZ);
  }
  s_bmi = 0;
  if (s_fifo_buf) { heap_caps_free(s_fifo_buf); s_fifo_buf = nullptr; }
}

static inline void tripla(const uint8_t* p, float out[3], float escala)
{
  for (int i = 0; i < 3; ++i) { out[i] = (float)(int16_t)(p[2 * i] | (p[2 * i + 1] << 8)) / escala; }
}

// Checks the gyro/accel order in a frame with both, only when still
// (calibrating): whichever reads ~1 g is the accelerometer. 5 consecutive
// votes against the datasheet swap it.
static void votar_ordem(const float g[3], const float a[3])
{
  const float ng = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) * s_gyr_lsb / s_acc_lsb;
  const float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (na > 0.7f && na < 1.3f && ng < 0.2f) {
    if (s_ordem_votos < 5) { ++s_ordem_votos; }
  } else if (ng > 0.7f && ng < 1.3f && na < 0.2f) {
    if (--s_ordem_votos <= -5) {
      s_acel_primeiro = !s_acel_primeiro;
      s_ordem_votos = 0;
      ESP_LOGW(TAG, "FIFO: accelerometer before gyro on this chip - corrected");
    }
  }
}

// Walks the frames. With a null 'cb' it only counts the gyro samples (to know
// each one's timestamp: the last is "now", each earlier one a step before).
// Returns how many there were, or -1 if it found a byte it does not understand.
static int fifo_percorrer(const uint8_t* b, int n, bool parado, tab5_imu_fifo_cb cb, void* ctx,
                          int64_t t_fim, int total)
{
  const bool aplicar = cb != nullptr;
  int i = 0, k = 0;
  while (i < n) {
    const uint8_t h = b[i] & 0xFC;             // the low 2 bits are interrupt markers
    if (h == 0x80) { if (aplicar) { s_dg.vazios += 1; } break; }   // queue empty (read past the end)
    if ((h & 0xC0) == 0x80) {                  // data frame: aux? gyro? accel?
      const int  aux   = (h & 0x10) ? 8 : 0;
      const bool tem_g = (h & 0x08) != 0;
      const bool tem_a = (h & 0x04) != 0;
      const int  tam   = 1 + aux + (tem_g ? 6 : 0) + (tem_a ? 6 : 0);
      if (i + tam > n) { if (aplicar) { ++s_dg.cortes; } break; }   // truncated: the chip repeats it on the next read
      if (tem_g) {
        if (aplicar) {
          const uint8_t* p1 = b + i + 1 + aux;
          const bool troca = tem_a && s_acel_primeiro;
          float g[3], a[3];
          s_dg.quadro_tam = tam;
          memcpy(s_dg.quadro, b + i, tam < (int)sizeof(s_dg.quadro) ? tam : (int)sizeof(s_dg.quadro));
          tripla(troca ? p1 + 6 : p1, g, s_gyr_lsb);
          if (tem_a) {
            tripla(troca ? p1 : p1 + 6, a, s_acc_lsb);
            if (parado) { votar_ordem(g, a); }
          }
          const int64_t t = t_fim - (int64_t)((float)(total - 1 - k) * s_gyr_dt * 1e6f);
          cb(g, tem_a ? a : nullptr, s_gyr_dt, t, ctx);
        }
        ++k;
      }
      i += tam;
      continue;
    }
    // Control frames: skip (1), clock (3), configuration change (4),
    // dropped sample (1).
    int resto = -1;
    switch (h) {
      case 0x40: resto = 1; if (aplicar) { ++s_dg.pulos; } break;   // skip: the queue filled up
      case 0x44: resto = 3; break;
      case 0x48: resto = 4; break;
      case 0x50: resto = 1; break;
      default: break;
    }
    if (aplicar && resto >= 0) { ++s_dg.ctrl[h == 0x40 ? 0 : h == 0x44 ? 1 : h == 0x48 ? 2 : 3]; }
    if (resto < 0) { return -1; }
    i += 1 + resto;
  }
  return k;
}

void tab5_imu_fifo_ler(bool parado, tab5_imu_fifo_cb cb, void* ctx)
{
  if (!s_bmi || !cb) { return; }
  const int64_t agora = esp_timer_get_time();
  uint8_t l[2];
  if (!M5.In_I2C.readRegister(s_bmi, BMI_FIFO_LEN, l, 2, BMI_I2C_HZ)) { return; }
  int n = (l[0] | (l[1] << 8)) & 0x3FFF;
  if (n == 0) { return; }
  // More than the buffer only happens after the UI has stalled ~0.5 s: read
  // what fits now, the rest on the next call (this batch's timestamps come
  // out early, the integrated rotation does not).
  const bool cheio = n > FIFO_BUF;
  if (cheio) { n = FIFO_BUF; }
  ++s_dg.lidas;
  s_dg.bytes += (uint32_t)n;
  if (cheio) { ++s_dg.cheios; }
  if (!M5.In_I2C.readRegister(s_bmi, BMI_FIFO_DATA, s_fifo_buf, n, BMI_I2C_HZ)) { s_odr_t0 = 0; return; }
  const int total = fifo_percorrer(s_fifo_buf, n, parado, nullptr, nullptr, 0, 0);
  if (total < 0) {
    ESP_LOGW(TAG, "FIFO: unknown frame - flushing");
    ++s_dg.desconhecidos;
    M5.In_I2C.writeRegister8(s_bmi, BMI_CMD, BMI_FIFO_FLUSH, BMI_I2C_HZ);
    s_odr_t0 = 0;
    return;
  }
  const uint32_t pulos = s_dg.pulos;
  fifo_percorrer(s_fifo_buf, n, parado, cb, ctx, agora, total);
  if (s_dg.pulos != pulos) {
    ESP_LOGW(TAG, "FIFO: the chip dropped samples (did the UI stall?) - rotation lost");
  }

  // The real rate: samples since t0 over the P4's time. Any abnormal read
  // (queue full, skip) restarts the count.
  if (cheio || s_dg.pulos != pulos || s_odr_t0 == 0) {
    s_odr_t0 = agora;
    s_odr_n = 0;
    return;
  }
  s_odr_n += (uint32_t)total;
  const float dec = (float)(agora - s_odr_t0) * 1e-6f;
  if (dec >= ODR_MEDIR_S && s_odr_n > 0) {
    const float dt = dec / (float)s_odr_n;
    if (fabsf(dt / s_gyr_dt_nom - 1.0f) < ODR_TOL) {
      if (fabsf(dt / s_gyr_dt - 1.0f) > 0.002f) {
        ESP_LOGI(TAG, "real gyro rate %.2f Hz (nominal %.0f)", (double)(1.0f / dt),
                 (double)(1.0f / s_gyr_dt_nom));
      }
      s_gyr_dt = dt;
    }
  }
}

void tab5_imu_fifo_diag(tab5_imu_fifo_diag_t* d, bool zerar)
{
  if (!d) { return; }
  *d = s_dg;
  d->acel_primeiro = s_acel_primeiro;
  d->dt = s_gyr_dt;
  if (zerar) {
    const uint32_t pulos = s_dg.pulos;   // cumulative: not reset
    memset(&s_dg, 0, sizeof(s_dg));
    s_dg.pulos = pulos;
  }
}
