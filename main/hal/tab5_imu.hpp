// IMU (BMI270) - shared reading, in the SAME spirit as tab5_power.hpp
// (medir() vs ultima()) and tab5_sensores.hpp (cache instead of a queue).
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS - found while reviewing "ready for more than one
// consumer", the same exercise that fixed tab5_gps.cpp.
// -------------------------------------------------------------------------
// app_imu.cpp always read the BMI270 directly (M5.Imu.update()/getAccel/
// getGyro), and that was SAFE - but not because anyone had thought about
// concurrency: it was because the only caller is the app's own desenhar(),
// which only runs in the UI task, the ONLY one allowed to talk on the
// internal I2C bus (rule documented in tab5_power.hpp: "M5Unified's
// I2C_Class has no mutex and two interleaving tasks read the wrong register
// without any error"). No other app could ask "what is the device's tilt
// right now" without duplicating that code AND risking bus access from the
// wrong task.
//
// THE SOLUTION, EXACTLY AS FOR POWER AND TEMPERATURE: one function that
// really MEASURES (UI task only) and another that READS THE CACHE (any task,
// mutex-protected - see the notice about tab5_gps.cpp that prompted this).
//
// -------------------------------------------------------------------------
// DIFFERENT RATES ON PURPOSE - this is NOT just "copy the INA226 pattern"
// -------------------------------------------------------------------------
// tab5_power_medir()/tab5_sensores_medir() run once a second because power
// and temperature change slowly - 1Hz suits any consumer, including whoever
// "owns" them. The IMU is different: app_imu.cpp itself needs a reading EVERY
// FRAME so the level indicator does not look frozen - sampling only once a
// second would make worse the ONLY screen that really uses this today.
//
// So app_imu.cpp reads via tab5_imu_amostra() (below), in the same task,
// every frame - it is safe (same task) and fast. tab5_imu_medir() runs in
// parallel, once a second, feeding ONLY the cache the OTHER apps will read -
// "is the device upright or lying flat", not "the bubble level must be
// smooth". A consumer needing a higher rate than that in future needs the
// SAME discussion this file documents, not just a bigger number.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool  valida;   // false until the first measurement (or if the IMU never came up)
  float ax, ay, az;   // g (acceleration, gravity included)
  float gx, gy, gz;   // degrees/s (gyroscope)
} tab5_imu_dados_t;

// REALLY reads the BMI270 and updates the cache. ONLY FROM THE UI TASK -
// same rule as INA226/temperature/keyboard, see the big notice above.
// Silent no-op if M5.Imu.isEnabled() is false (perfil.imu=false at this
// app's boot, or no app needing the IMU has run yet).
void tab5_imu_medir(void);

// On idle the measurement does not run, but the GYROSCOPE still has to
// follow the request: an expired Berry request (5 s) or one withdrawn as the
// screen turned off left the gyroscope on for the whole idle period (hundreds
// of uA). Just applies the requested state - I2C only when the state changes.
// ONLY FROM THE UI TASK.
void tab5_imu_repouso(void);

// The LATEST reading, without touching I2C - what any task should use.
// Returns false (with 'valida'=false in '*saida') if the IMU never measured.
bool tab5_imu_ultima(tab5_imu_dados_t* saida);

// Is the IMU up and responding? (does not touch I2C)
bool tab5_imu_disponivel(void);

// The MOST RECENT sample straight from the chip (acceleration in g, rotation
// in degrees/s), every screen frame. ONLY FROM THE UI TASK. Returns false if
// the IMU is unavailable or there is no new sample since the last call.
bool tab5_imu_amostra(float a[3], float g[3]);

// ---------------------------------------------------------------------------
// THE GYROSCOPE ONLY WHEN SOMEONE READS IT
// ---------------------------------------------------------------------------
// M5Unified starts the BMI270 with the gyroscope on and no power saving (the
// chip's worst consumption) and it stayed like that for the whole session.
// Screen rotation (kernel/orientacao) only uses the ACCELEROMETER: with no
// request, the system leaves only that on, in power-save. Whoever reads the
// gyroscope requests it with their own bit and releases it afterwards.
//
// From ANY task (an atomic OR): the write to the chip happens in the UI task,
// on the next tab5_imu_medir() - up to 1 s later. Whoever reads the sensor
// directly (the IMU app) calls tab5_imu_medir() right after requesting.
#define TAB5_GIRO_APP_IMU (1u << 0)   // IMU app (reads directly, every frame)
#define TAB5_GIRO_RADAR   (1u << 1)   // Radar app
#define TAB5_GIRO_BERRY   (1u << 2)   // sistema.giro() - see tab5_imu_giroscopio_berry
void tab5_imu_giroscopio(uint32_t quem, bool sim);

// The Berry request is TIME-LIMITED: a script does not say when it stops
// reading. Each sistema.giro() renews it; 5 s without a call, it lapses.
void tab5_imu_giroscopio_berry(void);
