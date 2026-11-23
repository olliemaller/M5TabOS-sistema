// IMU (BMI270) - the chip's FIFO, for whoever needs ALL the gyroscope
// samples and not just the last one (today: the Radar app, which integrates
// the heading).
//
// WHY THIS LIVES IN THE HAL: reading the FIFO means talking to the chip over
// the internal I2C (registers, frame headers, the chip's clock). That is
// driver work, and driver work does not belong in an app - the app just
// receives the finished samples.
//
// M5Unified only reads the data registers (the LATEST sample). The FIFO is a
// ~2 KB queue inside the chip: every gyroscope sample goes in, and the reader
// gets them all, in order. Turning the FIFO on changes nothing for those
// reading the registers (tab5_imu_medir, tab5_imu_amostra): they are
// independent paths.
//
// ALL the functions below are UI TASK only - the internal I2C rule (see
// tab5_imu.hpp and tab5_power.hpp).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  float gyr_hz;             // nominal gyroscope rate
  float gyr_lsb, acc_lsb;   // counts per degree/s and per g
} tab5_imu_fifo_info_t;

// Turns the FIFO on (gyro + accelerometer, with headers). 'info' may be
// nullptr. false = no chip or no memory: the caller uses tab5_imu_amostra().
bool tab5_imu_fifo_ligar(tab5_imu_fifo_info_t* info);

// Returns the chip to its previous state and frees the buffer.
void tab5_imu_fifo_desligar(void);

bool tab5_imu_fifo_ligado(void);

// One gyro sample (degrees/s) and, when it came along, the accelerometer (g;
// nullptr if not). 'dt' is the step (s) and 't_us' its timestamp (esp_timer).
typedef void (*tab5_imu_fifo_cb)(const float g[3], const float* a, float dt, int64_t t_us, void* ctx);

// Reads what the chip's queue has gathered and calls 'cb' for each gyro
// sample, in order. 'parado' = the app knows the device is still
// (calibrating): only then does the driver check the gyro/accelerometer order
// within the frame (the accelerometer is the one reading ~1 g). Chip drops
// and the real gyroscope rate (the BMI270's clock is not the P4's) are
// handled in here.
void tab5_imu_fifo_ler(bool parado, tab5_imu_fifo_cb cb, void* ctx);

// Diagnostic counters.
typedef struct {
  uint32_t lidas, bytes, cortes, vazios, ctrl[4], desconhecidos, cheios;
  uint32_t pulos;           // times the chip dropped samples; CUMULATIVE, never reset
  bool     acel_primeiro;   // order of the "gyro + accel" frame on this chip
  float    dt;              // current gyroscope step (s)
  uint8_t  quadro[21];      // start of the last gyro frame read
  int      quadro_tam;      // real size of that frame (what does not fit in quadro[] is left out)
} tab5_imu_fifo_diag_t;

// Copies the counters. 'zerar' restarts the window ones (except 'pulos').
void tab5_imu_fifo_diag(tab5_imu_fifo_diag_t* d, bool zerar);
