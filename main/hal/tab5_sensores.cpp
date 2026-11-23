#include "tab5_sensores.hpp"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char* TAG = "sensores";

static temperature_sensor_handle_t s_tsens = nullptr;
static float s_celsius = 0.0f;
static bool  s_valida  = false;
// AVERAGE SINCE BOOT, of the RAW reading (the filter above is for the eye, not for
// the maths). Double: at 1 sample per second, a float would lose half-degree
// resolution within a few days of uptime.
static double   s_soma = 0.0;
static uint32_t s_n    = 0;

void tab5_sensores_iniciar(void)
{
  // Range 10..80 C. The ESP32 sensor switches internal scale according to the range
  // requested, and too wide a range loses resolution. This covers everything from a
  // cold room to the chip working hard, which is the whole span this device
  // sees in practice.
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);

  esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "temperature_sensor_install failed: %s", esp_err_to_name(err));
    s_tsens = nullptr;
    return;
  }
  err = temperature_sensor_enable(s_tsens);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "temperature_sensor_enable failed: %s", esp_err_to_name(err));
    temperature_sensor_uninstall(s_tsens);
    s_tsens = nullptr;
    return;
  }
  ESP_LOGI(TAG, "P4 temperature sensor up");
}

void tab5_sensores_medir(void)
{
  if (!s_tsens) { return; }
  float c = 0.0f;
  if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
    // GENTLE FILTER. The raw reading jumps half a degree between samples, and a number
    // on the status bar that keeps flickering forces the eye to chase it.
    // Simple moving average: 7/8 of the previous and 1/8 of the new - follows a
    // real change within a few seconds and ignores the noise.
    s_celsius = s_valida ? (s_celsius * 0.875f + c * 0.125f) : c;
    s_valida  = true;
    s_soma += c;
    ++s_n;
  }
}

bool tab5_sensores_temp(float* celsius)
{
  if (!s_valida) { return false; }
  if (celsius) { *celsius = s_celsius; }
  return true;
}

bool tab5_sensores_temp_media(float* celsius)
{
  if (!s_n) { return false; }
  if (celsius) { *celsius = (float)(s_soma / s_n); }
  return true;
}
