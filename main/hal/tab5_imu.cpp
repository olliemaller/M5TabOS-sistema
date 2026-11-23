#include "tab5_imu.hpp"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "tab5_mutex.hpp"
#include <M5Unified.h>
#include "utility/imu/BMI270_Class.hpp"   // DEFAULT_ADDRESS (chip_responde)
#include "tab5_power.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "imu";

static tab5_imu_dados_t s_cache = { };

// Lazily created, never destroyed - same template as services/notificacao.cpp
// and tab5_gps.cpp: first use always comes before any real contention (boot
// calls tab5_imu_medir() before any app can read).
static Trava s_trava;

static inline void travar(void)   { s_trava.travar(); }
static inline void destravar(void) { s_trava.destravar(); }

// THE IMU THAT DID NOT COME UP AT BOOT. M5.begin() starts the BMI270 only
// once, and that start loads an 8 KB firmware into the chip over I2C; now
// and then it fails (the chip still coming out of reset, a lost transaction)
// and the IMU stayed down for the whole session - the app showed "IMU nao
// disponivel neste boot" until a reboot. Here the system retries a few times,
// spaced out: each attempt occupies the bus for about 200 ms.
#define IMU_TENTATIVAS   5
#define IMU_ESPERA_MS 3000

// THE IMU THAT STOPPED AFTER COMING UP: this many consecutive measurements
// without a new sample (1 per second) count as "stopped responding" and
// trigger the same retry. The attempt limit is per episode: it resets when
// the IMU responds again.
#define IMU_FALHAS_SEGUIDAS 5
static int s_tentativas = 0;
static int s_falhas = 0;

// Does the BMI270 respond on I2C? Reads CHIP_ID (0x00) at the driver's
// default address (BMI270_Class::DEFAULT_ADDRESS). Only to tell "no new
// sample" from "mute chip" - see tab5_imu_medir.
static bool chip_responde(void)
{
  uint8_t id = 0;
  return M5.In_I2C.readRegister(m5::BMI270_Class::DEFAULT_ADDRESS, 0x00, &id, 1, 400000) && id != 0;
}

static bool subir_de_novo(void)
{
  static uint32_t s_proxima_ms = 0;
  if (!tab5_power_imu_pedida() || s_tentativas >= IMU_TENTATIVAS) { return false; }
  const uint32_t agora = tab5_agora_ms();
  if ((int32_t)(agora - s_proxima_ms) < 0) { return false; }
  s_proxima_ms = agora + IMU_ESPERA_MS;
  ++s_tentativas;
  const bool ok = M5.Imu.begin(&M5.In_I2C, M5.getBoard());
  if (ok) { ESP_LOGI(TAG, "IMU came up on attempt %d after boot", s_tentativas); }
  else    { ESP_LOGW(TAG, "IMU still down (attempt %d of %d)", s_tentativas, IMU_TENTATIVAS); }
  return ok;
}

// THE GYROSCOPE (tab5_imu.hpp): the requests (bits) and the chip's state.
// -1 = unknown - boot and every M5.Imu.begin() leave the chip in performance
// mode with the gyroscope on, so the first measurement always applies what
// was requested.
#define GIRO_PRAZO_BERRY_MS 5000
static uint32_t          s_giro_pedidos = 0;
static uint32_t          s_giro_berry_ms = 0;
static int               s_giro_no_chip = -1;

void tab5_imu_giroscopio(uint32_t quem, bool sim)
{
  if (sim) { __atomic_fetch_or(&s_giro_pedidos, quem, __ATOMIC_ACQ_REL); }
  else     { __atomic_fetch_and(&s_giro_pedidos, ~quem, __ATOMIC_ACQ_REL); }
}

void tab5_imu_giroscopio_berry(void)
{
  __atomic_store_n(&s_giro_berry_ms, tab5_agora_ms(), __ATOMIC_RELEASE);
  tab5_imu_giroscopio(TAB5_GIRO_BERRY, true);
}

// In the UI task, before measuring: an expired Berry request lapses, and the
// chip moves to the requested state if it is not already in it.
static void aplicar_giroscopio(void)
{
  uint32_t p = __atomic_load_n(&s_giro_pedidos, __ATOMIC_ACQUIRE);
  if ((p & TAB5_GIRO_BERRY)
   && tab5_agora_ms() - __atomic_load_n(&s_giro_berry_ms, __ATOMIC_ACQUIRE) > GIRO_PRAZO_BERRY_MS) {
    tab5_imu_giroscopio(TAB5_GIRO_BERRY, false);
    p &= ~TAB5_GIRO_BERRY;
  }
  const int quer = p ? 1 : 0;
  if (quer == s_giro_no_chip) { return; }
  if (tab5_power_imu_giroscopio(quer)) { s_giro_no_chip = quer; }
}

void tab5_imu_medir(void)
{
  // IMU down (did not come up at boot) or mute after coming up: retry
  // begin(), spaced out and with a per-episode limit.
  if (!M5.Imu.isEnabled() || s_falhas >= IMU_FALHAS_SEGUIDAS) {
    if (subir_de_novo()) { s_giro_no_chip = -1; }   // begin() put the chip back in performance mode
    else if (!M5.Imu.isEnabled()) { return; }
  }
  aplicar_giroscopio();

  // update() == 0 means "no new sample" - and the driver ALSO returns that
  // when the I2C read fails (and keeps the last sample). Not every zero can
  // be treated as a failure: an app reading the IMU quickly (app_imu,
  // orientacao) may have just consumed the sample. So, on zero, ask for
  // CHIP_ID (register 0x00): answered = just no new sample, the cache from
  // 1 s ago still stands; no answer = real failure, and the cache becomes
  // invalid and zeroed instead of repeating an old value as if current.
  if (M5.Imu.update() == 0 && !chip_responde()) {
    if (++s_falhas == IMU_FALHAS_SEGUIDAS) {
      ESP_LOGW(TAG, "IMU stopped responding - restarting");   // once per episode
    }
    const tab5_imu_dados_t invalido = { };
    travar();
    s_cache = invalido;
    destravar();
    return;
  }
  if (s_falhas > 0) { ESP_LOGI(TAG, "IMU responding again"); }
  s_falhas = 0;
  s_tentativas = 0;   // the episode is over: a new one gets fresh attempts

  tab5_imu_dados_t d = { };
  d.valida = true;
  M5.Imu.getAccel(&d.ax, &d.ay, &d.az);
  if (s_giro_no_chip == 1) { M5.Imu.getGyro(&d.gx, &d.gy, &d.gz); }   // off: 0, not a stale value

  travar();
  s_cache = d;
  destravar();
}

void tab5_imu_repouso(void)
{
  if (M5.Imu.isEnabled()) { aplicar_giroscopio(); }
}

bool tab5_imu_ultima(tab5_imu_dados_t* saida)
{
  if (!saida) { return false; }
  travar();
  *saida = s_cache;
  destravar();
  return saida->valida;
}

bool tab5_imu_disponivel(void)
{
  return M5.Imu.isEnabled();
}

bool tab5_imu_amostra(float a[3], float g[3])
{
  if (!M5.Imu.isEnabled()) { return false; }
  if (M5.Imu.update() == 0) { return false; }   // no new sample
  M5.Imu.getAccel(&a[0], &a[1], &a[2]);
  M5.Imu.getGyro(&g[0], &g[1], &g[2]);
  return true;
}
