#include "tab5_power.hpp"
#include "esp_rom_sys.h"   // esp_rom_delay_us
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "tab5_marcos.hpp"
#include "tab5_sensores.hpp"
#include "tab5_tela.hpp"
#include "tab5_barramento.hpp"

#include "esp_log.h"
#include <math.h>
#include <stdlib.h>   // abs
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"   // esp_clk_cpu_freq()
#include <stdio.h>

// Needed to reach the DPI panel handle (see tab5_power_repouso).
#include "tab5_painel_dsi_interno.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


// THE VARIABLE CLOCK RANGE NEVER CROSSES THE PSRAM SPEED.
//
// On the P4, every automatic frequency change (DFS) that crosses
// CONFIG_SPIRAM_SPEED (200 MHz) also changes the PSRAM timing - and to do so
// IDF STALLS THE OTHER CORE (mspi_timing_change_speed_mode_cache_safe ->
// esp_ipc_isr_stall_other_cpu). If at that instant the other core is inside
// an interrupt that also wants to change frequency (esp_pm_impl_isr_hook ->
// do_switch, with interrupts masked), it does not honour the stall, the two
// wait on each other and the interrupt watchdog reboots the device. Caught in
// the core dump on 2026-10-21 (both cores in do_switch); it also explains
// the INT_WDT reboots with Music playing on 2026-10-17. With the range on
// only one side of the limit, DFS never changes the PSRAM speed: above it the
// clock stays fixed at the ceiling; below it (idle at 40 MHz) it keeps varying.
static void faixa_segura(int max_mhz, int* min_mhz)
{
#if CONFIG_SPIRAM
  if (max_mhz >= CONFIG_SPIRAM_SPEED && *min_mhz < CONFIG_SPIRAM_SPEED) { *min_mhz = max_mhz; }
#else
  (void)max_mhz; (void)min_mhz;
#endif
}

static const char *TAG = "power";

// ---------------------------------------------------------------------------
// Map of the Tab5's expanders (two PI4IOE5V6408 on the internal I2C).
// Taken from M5Unified itself: Power_Class.cpp, board_M5Tab5 branch.
//
// Expander 0 (0x43)                     Expander 1 (0x44)
//   7 HP_DET    (input)                   7 CHG_EN       (output, high)
//   6 CAM_RST   (output, high=active)     6 CHG_STAT     (input)
//   5 TP_RST    (output, high=touch on)   5 nCHG_QC_EN   (output)
//   4 LCD_RST   (output, high=panel on)   4 PWROFF_PULSE (output)
//   3 NC / Hat power on the Tab5X         3 USB5V_EN     (input w/ pull)
//   2 EXT5V_EN  (input w/ pull)           2 NC
//   1 SPK_EN    (output)                  1 NC
//   0 RF_PTH    (output)                  0 WLAN_PWR_EN  (output, high)
//
// A misleading detail: EXT5V_EN and USB5V_EN are NOT driven pins. They are
// inputs with an internal pull and the real enable is a resistor on the
// board; that is why M5Unified's setExtOutput() flips the PULL instead of
// writing a level. And that is why both start on without anyone asking.
// ---------------------------------------------------------------------------
static constexpr uint8_t IOE0_CAM_RST     = 6;
static constexpr uint8_t IOE0_TP_RST      = 5;
static constexpr uint8_t IOE1_WLAN_PWR_EN = 0;

// BMI270 on the internal bus. M5Unified tries 0x69 and 0x68.
static constexpr uint8_t BMI270_CHIP_ID   = 0x24;
static constexpr uint8_t BMI270_REG_ID    = 0x00;
static constexpr uint8_t BMI270_PWR_CONF  = 0x7C;
static constexpr uint8_t BMI270_PWR_CTRL  = 0x7D;

// ---------------------------------------------------------------------------

static bool s_imu_pedida = false;
bool tab5_power_imu_pedida(void) { return s_imu_pedida; }

void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_perfil_t& perfil)
{
  cfg.internal_imu = perfil.imu;
  s_imu_pedida     = perfil.imu;
  cfg.internal_mic = perfil.microfone;
  cfg.internal_spk = perfil.alto_falante;
  cfg.internal_rtc = perfil.rtc;

  // output_power=true makes M5.begin() call setExtOutput(true) with mask
  // 0xFF, which covers ext_PA (Grove) AND ext_USB (USB-A) at once. We turn it
  // on here only if EITHER is requested, and separate them in phase 2.
  cfg.output_power = (perfil.grove_5v || perfil.usb_a_5v);
}

// When the Tab5 last touched the Grove 5V / reapplied the profile
// (esp_timer in ms; 0 = never). Feeds the Mesh node's reset journal.
static volatile uint32_t s_grove_toque_ms = 0, s_aplicar_ms = 0;
static int8_t s_grove_hw = -1;   // last state requested of the rail (-1 = never)
// Only counts as "touched the rail" when the requested state CHANGES:
// reapplying the same value shows up separately, in tab5_power_aplicar_ms().
static void anotar_grove(bool ligar)
{
  if (s_grove_hw == (int8_t)ligar) { return; }
  s_grove_hw = (int8_t)ligar;
  s_grove_toque_ms = tab5_agora_ms() | 1u;
}
uint32_t tab5_power_grove_toque_ms(void) { return s_grove_toque_ms; }
uint32_t tab5_power_aplicar_ms(void)     { return s_aplicar_ms; }

void tab5_power_aplicar(const tab5_perfil_t& perfil)
{
  s_aplicar_ms = tab5_agora_ms() | 1u;
  anotar_grove(perfil.grove_5v);
  tab5_marco("power profile", perfil.cpu_min_mhz);
  // --- 5V rails, now one at a time ---------------------------------------
  // If output_power turned both on, here we turn off whichever was not requested.
  M5.Power.setExtOutput(perfil.grove_5v, m5::ext_PA);
  M5.Power.setExtOutput(perfil.usb_a_5v, m5::ext_USB);

  // --- camera -------------------------------------------------------------
  tab5_power_camera(perfil.camera);

  // --- radio (ESP32-C6) ---------------------------------------------------
  // We do not call tab5_power_radio(true) when it is already on: the rail
  // starts high and an unnecessary pulse would reset the C6.
  if (!perfil.radio) { tab5_power_radio(false); }

  // --- backlight ----------------------------------------------------------
  M5.Display.setBrightness(perfil.brilho);

  // --- CPU power management ----------------------------------------------
  if (perfil.pm_dinamico)
  {
#if CONFIG_PM_ENABLE
    // THE FLOOR MAY NEVER EXCEED THE CEILING - esp_pm_configure refuses with
    // ESP_ERR_INVALID_ARG if min > max, and WITHOUT A CLEAR WARNING: it is
    // exactly the bug 'energia repouso 20' revealed in ajustar_teto(), except
    // that here it is the path EVERY app uses on each opening, not a
    // diagnostic command. Today no app declares cpu_max_mhz below the default
    // cpu_min_mhz of 40, so the case never really happens - but the next app
    // wanting a low ceiling would fall into the same trap with no clue why.
    // Same fix: the floor follows the ceiling when the requested ceiling is
    // lower.
    int piso = (perfil.cpu_max_mhz < perfil.cpu_min_mhz) ? perfil.cpu_max_mhz
                                                          : perfil.cpu_min_mhz;
    faixa_segura(perfil.cpu_max_mhz, &piso);
    esp_pm_config_t pm = {
      .max_freq_mhz       = perfil.cpu_max_mhz,
      .min_freq_mhz       = piso,
      .light_sleep_enable = perfil.light_sleep_ocioso,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK)
    {
      // ESP_ERR_INVALID_ARG here is almost always an invalid frequency:
      // on P4 rev >= 3.0 only 400, 200, 100 and XTAL/N are valid.
      ESP_LOGW(TAG, "esp_pm_configure(%d/%d) failed: %s",
               perfil.cpu_max_mhz, piso, esp_err_to_name(err));
    }
    else
    {
      if (piso == perfil.cpu_max_mhz) {
        ESP_LOGI(TAG, "fixed clock: %d MHz (no DFS crossing the PSRAM speed); sleep when idle: %s",
                 piso, perfil.light_sleep_ocioso ? "yes" : "no");
      } else {
        ESP_LOGI(TAG, "variable clock: %d to %d MHz depending on load; sleep when idle: %s",
                 piso, perfil.cpu_max_mhz, perfil.light_sleep_ocioso ? "yes" : "no");
      }
    }
#else
    ESP_LOGW(TAG, "pm_dinamico requested, but CONFIG_PM_ENABLE is not on. "
                  "Uncomment the line in sdkconfig.defaults and rebuild.");
#endif
  }

  // Only the names of what stays ON: a row of "=0/=1" required knowing the
  // field order to read it.
  ESP_LOGI(TAG, "power profile: on [%s%s%s%s%s%s%s], brightness %d",
           perfil.imu ? " IMU" : "", perfil.microfone ? " microphone" : "",
           perfil.alto_falante ? " speaker" : "", perfil.camera ? " camera" : "",
           perfil.radio ? " radio" : "", perfil.grove_5v ? " Grove-5V" : "",
           perfil.usb_a_5v ? " USB-A-5V" : "", (int)perfil.brilho);
}

// ---------------------------------------------------------------------------

void tab5_power_radio(bool ligar)
{
  M5.getIOExpander(1).digitalWrite(IOE1_WLAN_PWR_EN, ligar);
  ESP_LOGI(TAG, "C6 power (WiFi/Bluetooth): %s", ligar ? "on" : "off");
}

static bool s_grove_ligado = false;

void tab5_power_grove(bool ligar)
{
  anotar_grove(ligar);
  M5.Power.setExtOutput(ligar, m5::ext_PA);
  s_grove_ligado = ligar;
  ESP_LOGI(TAG, "Grove port 5V: %s", ligar ? "on" : "off");
}

bool tab5_power_grove_ligado(void) { return s_grove_ligado; }

void tab5_power_usb_a(bool ligar)
{
  M5.Power.setExtOutput(ligar, m5::ext_USB);
  ESP_LOGI(TAG, "USB-A 5V: %s", ligar ? "on" : "off");
}

void tab5_power_camera(bool ligar)
{
  M5.getIOExpander(0).digitalWrite(IOE0_CAM_RST, ligar);
}

bool tab5_power_imu_giroscopio(bool ligar)
{
  // M5.Imu.sleep() does NOT work here: IMU_Base::sleep() is virtual returning
  // false and only the MPU6886 overrides it. On the BMI270 the call does nothing.
  for (uint8_t addr : { (uint8_t)0x69, (uint8_t)0x68 })
  {
    if (M5.In_I2C.readRegister8(addr, BMI270_REG_ID, 400000) != BMI270_CHIP_ID)
    {
      continue;
    }
    if (ligar) {
      // PWR_CONF first: with advanced power save on the chip wants ~450 us
      // between two writes (datasheet, 4.4) - leave it, wait, and only then
      // turn the gyroscope on (which takes ~45 ms to give a good reading).
      M5.In_I2C.writeRegister8(addr, BMI270_PWR_CONF, 0x00, 400000);
      esp_rom_delay_us(1000);
      // PWR_CTRL: bit3 temp | bit2 acc | bit1 gyr | bit0 aux.
      M5.In_I2C.writeRegister8(addr, BMI270_PWR_CTRL, 0x0E, 400000);
    } else {
      // Accelerometer only: the gyroscope is what eats current (hundreds of
      // uA against tens for the accelerometer). PWR_CONF bit0 adv_power_save |
      // bit1 fifo_self_wakeup - M5Unified leaves 0x00 (maximum performance always).
      M5.In_I2C.writeRegister8(addr, BMI270_PWR_CTRL, 0x04, 400000);
      esp_rom_delay_us(1000);
      M5.In_I2C.writeRegister8(addr, BMI270_PWR_CONF, 0x03, 400000);
    }
    ESP_LOGI(TAG, "IMU (BMI270, 0x%02x): gyroscope %s", addr, ligar ? "on" : "off (power-save)");
    return true;
  }
  ESP_LOGW(TAG, "BMI270 not found (was perfil.imu false?)");
  return false;
}

// ---------------------------------------------------------------------------

// Last measurement taken, kept for whoever CANNOT go to I2C.
static tab5_medida_t s_ultima = { 0, 0, 0, -1 };

tab5_medida_t tab5_power_ultima(void) { return s_ultima; }

// Charge percentage, our own - M5Unified's is no good here.
//
// Its getBatteryLevel() does LINEAR interpolation between 3300 and 4150 mV
// per cell (Power_Class.cpp:2341). Two measured problems:
//
// 1. The Li-Po curve is NOT linear. It is almost flat between 4.0 and 3.7 V
//    - about 70% of the capacity - and then plunges. With a straight line,
//    the first 30% drains slowly and the last 10% drops in seconds.
// 2. There is no load compensation. Voltage sags under load and rises on the
//    charger, and the whole range is only 800mV.
//
// The table below has the SHAPE of a Li-Po discharge, anchored at 4150mV per
// cell = 100%, which is this board's real peak: it measured 8.24 to 8.30V
// with the battery full, i.e. 4.12 to 4.15V per cell. The charger finishes
// below the standard 4.20V, and anchoring at 4.20 would make "full" read 95%.
//
// REPLACED by the REAL curve - the generic table above (12 points, a
// "Li-Po discharge shape" guessed by hand) was thrown away.
//
// WHAT CHANGED: we found in energia.csv a CONTINUOUS discharge, FROM A SINGLE
// SESSION, from 100% (charge tail ending, 8.309V) to almost dead (6.574V, the
// device went into a brownout reboot loop right after) - 21.5 hours without
// interruption, without splicing different sessions (the problem that
// spoiled the two previous attempts) and with the voltage already recorded
// as %.3f (the other problem, fixed in registro.cpp before this analysis).
//
// THE METHOD: real Coulomb counting. We integrate mA*dt (trapezium) from the
// first instant of real discharge to the end, which gives the capacity
// ACTUALLY consumed in this discharge - 1306.8 mAh, no longer an assumption.
// Each sample of that same discharge has voltage AND current measured at the
// SAME instant, so we can match "how much was really left" (Coulomb) with
// "what open-circuit voltage the device had" (V - I*R) point by point -
// without guessing any shape, without generalising from another battery.
//
// THE OLD TABLE'S ERROR, measured against this data: up to +30 POINTS in the
// middle of the range (it said 50% at a real 19.9%). Worse than the analysis
// had found (+25 points) - that came from spliced sessions with voltage
// quantisation, so it was still too optimistic about its own error.
//
// ON THE LABEL SAYING 2400 mAh: the physical battery has "2400mAh" printed on
// it, almost double the 1306.8 measured here. Checked BEFORE doubting the
// measurement: the INA226 shunt (Power_Class.cpp, expander block 0x43/0x44 -
// the same Tab5 addresses documented at the top of this file) is set to
// 0.005 ohm, the right value - it is not the driver reading current with the
// wrong constant. A typical Li-Po cell holds about 5-15% of its charge
// between 3.3V and 3.0V per cell (the range the brownout stopped us
// measuring here) - far too little to close a 45% gap. We stick with the
// MEASURED number: an inflated generic battery label is more common than real
// capacity hidden below the point where the device itself no longer runs.
//
// The internal R (0.268 ohm) is kept - it was used to compensate EVERY point
// of this new table, so any error in it is already "inside" the new curve,
// and re-measuring R would need clean current steps that this dataset does
// not isolate well (most steps here come from apps opening/closing, not a
// controlled test).
static const struct { int mv; int pct; } CURVA[] = {
  { 4155, 100 }, { 4076, 98 }, { 4063, 96 }, { 4053, 94 }, { 4049, 92 },
  { 4044,  90 }, { 4037, 88 }, { 4031, 86 }, { 4023, 84 }, { 4018, 82 },
  { 4014,  80 }, { 4009, 78 }, { 4005, 76 }, { 4001, 74 }, { 3995, 72 },
  { 3989,  70 }, { 3982, 68 }, { 3973, 66 }, { 3965, 64 }, { 3957, 62 },
  { 3950,  60 }, { 3945, 58 }, { 3938, 56 }, { 3933, 54 }, { 3928, 52 },
  { 3923,  50 }, { 3916, 48 }, { 3910, 46 }, { 3903, 44 }, { 3895, 42 },
  { 3886,  40 }, { 3879, 38 }, { 3868, 36 }, { 3863, 34 }, { 3856, 32 },
  { 3850,  30 }, { 3843, 28 }, { 3836, 26 }, { 3828, 24 }, { 3819, 22 },
  { 3810,  20 }, { 3801, 18 }, { 3794, 16 }, { 3786, 14 }, { 3778, 12 },
  { 3766,  10 }, { 3744,  8 }, { 3701,  6 }, { 3645,  4 }, { 3557,  2 },
  { 3302,   0 },
};

int tab5_power_pct(float volts, float mA)
{
  if (volts < 1.0f) { return -1; }          // no valid reading

  // At-rest voltage, corrected for current. mA is signed in our convention
  // (+ charging), and the single formula covers both cases.
  const float aberto = volts - (mA / 1000.0f) * TAB5_R_INTERNA;
  const int mv = (int)(aberto * 500.0f);    // per cell (2S), in mV

  if (mv >= CURVA[0].mv) { return 100; }
  const int n = sizeof(CURVA) / sizeof(CURVA[0]);
  if (mv <= CURVA[n-1].mv) { return 0; }

  for (int i = 1; i < n; ++i) {
    if (mv >= CURVA[i].mv) {
      // Interpolate within the segment - the curve is broken into short
      // straight lines, which is what makes it faithful where linear fails.
      const int dv = CURVA[i-1].mv - CURVA[i].mv;
      const int dp = CURVA[i-1].pct - CURVA[i].pct;
      return CURVA[i].pct + ((mv - CURVA[i].mv) * dp) / dv;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// CHARGE ESTIMATOR - the percentage the system shows.
// ---------------------------------------------------------------------------
// The curve above is good in only one situation: discharging, well after a
// recent charge, within the current range it was calibrated in. Reprocessing
// energia.csv (17 thousand samples, 48 discharge segments) against the charge
// counted in mAh:
//   - discharge from full: curve and count agree within +-3 points - OK;
//   - AFTER A PARTIAL CHARGE (unplugged midway): the curve said 89.8% with a
//     real 52.6% - +37 points - and took 5.5 h to tell the truth again. The
//     voltage under charge (and for hours afterwards) rises far beyond I*R:
//     it is chemical polarisation, not resistance. The old charge correction
//     still PINNED the number to the curve, so during charging it also
//     showed 94.6%;
//   - with the screen off (~30 mA, outside the calibrated range) the curve
//     reads high;
//   - in 2.7% of discharge minutes the % ROSE (up to +2.4 points) when the
//     device's load changed.
//
// THE ESTIMATOR, the same design as a mobile phone's battery gauge:
//   1. COUNTS CHARGE in both directions: soc += mA * dt / capacity;
//   2. THE CURVE ONLY CORRECTS, slowly (3 h time constant; 30 min below 15%,
//      where it is steep and reliable), and only when it is reliable:
//      discharge between 45 and 160 mA, and at least 1 h after the last charge;
//   3. END OF CHARGE (current falling with the voltage at the ceiling) = 100%;
//   4. THE NUMBER SHOWN DOES NOT RISE outside charging.
// Against the same file: mean error 1.8 -> 0.8 points, p95 9.9 -> 2.5,
// worst case 43.7 -> 4.3, and no rise during discharge.
//
// The starting point is the curve, at boot. On a software reboot (flashing,
// hang, watchdog) the battery has not changed: the value survives in RTC
// memory and the estimator carries on from where it was, instead of starting
// again from the voltage - which lies if the reboot came right after a charge.
static const float EST_RELAX_S      = 3600.0f;  // after a charge, the voltage lies for hours
static const float EST_TAU_H        = 3.0f;
static const float EST_TAU_BAIXO_H  = 0.5f;
static const float EST_I_MIN_MA     = 45.0f;    // current range of the curve's calibration
static const float EST_I_MAX_MA     = 160.0f;
static const uint32_t EST_MAGIA     = 0x50C0A7E1u;

// Current below this, with the charger on AND the voltage at the ceiling, is
// the same "finished" signal the charger itself uses (C/20 to C/10).
static const float TAB5_CARGA_TERMINO_MA = 100.0f;

// THIS CHARGER'S END OF CHARGE (IP2326), measured in the 2026-10-17
// energia.csv over five charges: constant voltage at 8.354-8.365 V, the
// current falls to ~200-220 mA and it CUTS OFF at once - the next sample is
// already -2 mA (the cable powers the device, the battery idle). It never
// passes the 100 mA above: "full" was only recognised by chance, and the
// screen sat at 92-99% with the battery full. The real signal is the DROP: it
// was in the final phase (voltage at the ceiling, current already falling)
// and stopped with the cable still plugged in.
static const float TAB5_CARGA_CV_V        = 8.34f;   // constant-voltage phase
static const float TAB5_CARGA_FINAL_MA    = 350.0f;  // ... already in the tail
static const float TAB5_CABO_PARADO_MA    = -8.0f;   // above this the cable is carrying the device (= tab5_bateria)
static const int64_t TAB5_CARGA_CORTE_US  = 30000000; // the drop between two readings (5 s), with margin

RTC_NOINIT_ATTR static struct { uint32_t magia; float soc; } s_rtc_soc;

static bool        s_est_ok      = false;
static float       s_soc         = 0.0f;     // the state, in % with fraction
static int64_t     s_est_us      = 0;
static int64_t     s_ult_carga_us = 0;
static int64_t     s_cauda_us    = 0;        // last reading in the charge tail (see TAB5_CARGA_CV_V)
static bool        s_ja_carregou = false;
static int         s_exibido     = -1;
static const char* s_est_fonte   = "-";
static const char* const FONTE_CURVA_BOOT = "curve at boot";

// RTC_NOINIT survives every reboot in which the chip did not lose power -
// flashing over USB (ESP_RST_USB/JTAG/EXT) included, which was the most
// common case and was being left out: boot started again from the curve with
// the cable plugged in.
static bool reinicio_de_software(void)
{
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: case ESP_RST_BROWNOUT: case ESP_RST_UNKNOWN: return false;
    default: return true;
  }
}

// Called from INSIDE tab5_power_medir() - once per measurement.
static int estimar_pct(float volts, float mA, int pct_curva)
{
  const int64_t agora = esp_timer_get_time();
  if (pct_curva < 0) { return s_exibido; }   // invalid reading: keep

  if (!s_est_ok) {
    s_soc = (float)pct_curva;
    s_est_fonte = FONTE_CURVA_BOOT;
    // The RTC value only counts if the chip did not lose power AND it is
    // still plausible - 20 points of slack cover the curve's error right
    // after a charge. WHILE CHARGING, the curve only errs UPWARDS (measured:
    // a real 46%, curve 98% at 700 mA): it cannot veto a value lower than its own.
    const bool carregando = mA > 20.0f;
    const float dif = s_rtc_soc.soc - (float)pct_curva;
    const bool plausivel = carregando ? (dif <= 20.0f) : (fabsf(dif) <= 20.0f);
    if (reinicio_de_software() && s_rtc_soc.magia == EST_MAGIA
     && s_rtc_soc.soc >= 0.0f && s_rtc_soc.soc <= 100.0f && plausivel) {
      s_soc = s_rtc_soc.soc;
      s_est_fonte = "carried over in RTC memory";
    }
    s_est_ok = true;
    s_est_us = agora;
    s_exibido = (int)lroundf(s_soc);
    s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
    return s_exibido;
  }

  // 1. COUNTING. dt from the clock, not the cadence. A gap of more than
  //    6 min is not continuity: restart from the curve.
  const float dt_h = (float)(agora - s_est_us) / 3.6e9f;
  s_est_us = agora;
  if (dt_h > 0.0f && dt_h < 0.1f) { s_soc += mA * dt_h / TAB5_CAPACIDADE_MAH * 100.0f; }
  else if (dt_h >= 0.1f)          { s_soc = (float)pct_curva; s_est_fonte = "curva (lacuna)"; }

  if (mA > 20.0f) { s_ult_carga_us = agora; s_ja_carregou = true; }
  if (mA > 20.0f && mA < TAB5_CARGA_FINAL_MA && volts >= TAB5_CARGA_CV_V) { s_cauda_us = agora; }

  // 3. END OF CHARGE: the pair of signals the charger uses to stop...
  if (mA > 0.0f && mA < TAB5_CARGA_TERMINO_MA && (int)(volts * 500.0f) >= (int)(CURVA[0].mv * 0.995f)) {
    s_soc = 100.0f;
  }
  // ... and this charger's cut-off: it was in the tail, now nothing goes in
  // and the cable is still there (without the cable the current would be the
  // consumption, well below -8 mA).
  bool cheia_agora = false;
  if (s_cauda_us && agora - s_cauda_us < TAB5_CARGA_CORTE_US
      && mA <= 20.0f && mA > TAB5_CABO_PARADO_MA) {
    s_soc = 100.0f;
    cheia_agora = true;
    s_cauda_us = 0;
    s_est_fonte = "end of charge";
  }

  // 2. CORRECTION FROM THE CURVE, slow, only where it is reliable.
  const float desde_carga_s = s_ja_carregou ? (float)(agora - s_ult_carga_us) / 1e6f : 1e9f;
  if (mA < -EST_I_MIN_MA && mA > -EST_I_MAX_MA && desde_carga_s > EST_RELAX_S && dt_h > 0.0f && dt_h < 0.1f) {
    const float tau = (pct_curva < 15) ? EST_TAU_BAIXO_H : EST_TAU_H;
    float k = dt_h / tau;
    if (k > 1.0f) { k = 1.0f; }
    s_soc += ((float)pct_curva - s_soc) * k;
  }

  if (s_soc < 0.0f)   { s_soc = 0.0f; }
  if (s_soc > 100.0f) { s_soc = 100.0f; }

  // 4. Outside charging, the number shown does not rise - except at end of
  //    charge, which happens precisely with the current already at zero.
  int r = (int)lroundf(s_soc);
  if (mA <= 20.0f && !cheia_agora && s_exibido >= 0 && r > s_exibido) { r = s_exibido; }
  s_exibido = r;

  s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
  return r;
}

bool tab5_power_soc_da_curva_no_boot(void) { return s_est_ok && s_est_fonte == FONTE_CURVA_BOOT; }

bool tab5_power_soc_semear(float soc, const char* fonte)
{
  if (!tab5_power_soc_da_curva_no_boot() || soc < 0.0f || soc > 100.0f) { return false; }
  s_soc = soc;
  s_exibido = (int)lroundf(soc);
  s_est_fonte = fonte;
  s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
  return true;
}

void tab5_power_soc_debug(float* soc, float* curva, const char** fonte, float* min_desde_carga)
{
  if (soc)   { *soc = s_soc; }
  if (curva) { *curva = (float)tab5_power_pct(s_ultima.volts, s_ultima.mA); }
  if (fonte) { *fonte = s_est_fonte; }
  if (min_desde_carga) {
    *min_desde_carga = s_ja_carregou ? (float)(esp_timer_get_time() - s_ult_carga_us) / 6e7f : -1.0f;
  }
}

// History ring. We keep a write index and a count instead of shifting the
// array: shifting 60 slots for every point would be pointless work, and a
// point arrives every 30 seconds for years on end.
static tab5_ponto_t s_hist[TAB5_HIST_N];
static int      s_hist_n = 0, s_hist_i = 0;
static uint32_t s_hist_ultimo = 0;

int tab5_power_historico(tab5_ponto_t* saida)
{
  if (!saida) { return 0; }
  // Unroll the ring into chronological order: whoever draws wants time
  // running left to right, not the buffer's internal order.
  const int comeco = (s_hist_n < TAB5_HIST_N) ? 0 : s_hist_i;
  for (int k = 0; k < s_hist_n; ++k) {
    saida[k] = s_hist[(comeco + k) % TAB5_HIST_N];
  }
  return s_hist_n;
}

tab5_medida_t tab5_power_medir(void)
{
  tab5_medida_t m;
  m.volts = M5.Power.Ina226.getBusVoltage();
  // Do NOT use M5.Power.getBatteryCurrent() here: it returns int32_t, and the
  // INA226's current LSB is 61uA (max_expected_current 2.0A / 32768). Any
  // reading below 1mA is truncated to 0 - which is what made the current show
  // as a fixed "0.0 mA". Reading the float directly keeps the resolution.
  // Sign: the shunt is wired so that charging reads negative, so we invert to
  // the convention (+) charging / (-) discharging.
  m.mA    = -1000.0f * M5.Power.Ina226.getShuntCurrent();
  // POWER = VOLTAGE x CURRENT, right here. The INA226's power register is
  // exactly that calculation done by the chip: reading it was one more I2C
  // transaction per second (awake and idle) for a number we already have.
  // Always positive, like the register.
  m.mW    = m.volts * fabsf(m.mA);
  // The % shown comes from the ESTIMATOR (mAh counting slowly corrected by
  // the curve) - see estimar_pct(). The curve alone is tab5_power_pct().
  m.pct   = estimar_pct(m.volts, m.mA, tab5_power_pct(m.volts, m.mA));
  s_ultima = m;          // publish for the other tasks

  // Sample the history every TAB5_HIST_SEG. Measurement happens at 1Hz;
  // keeping everything would give a 30s window instead of 30min.
  const uint32_t agora = (uint32_t)(esp_timer_get_time() / 1000000);
  float c;
  const bool tem_temp = tab5_sensores_temp(&c);

  // DO NOT START THE HISTORY WITHOUT TEMPERATURE - the temperature graph came
  // out with gaps.
  //
  // The FIRST call to this function happens at boot, via tab5_power_log
  // ("boot") in main.cpp - and at that instant tab5_sensores_iniciar() has
  // NOT YET run: it lives inside the UI task (vTaskInterface, see
  // kernel/core.cpp), which sistema_iniciar() only creates a few lines LATER,
  // and main.cpp calls the boot log BEFORE sistema_iniciar().
  // tab5_sensores_temp() returned false at that moment, and the
  // "s_hist_ultimo==0" condition (the SAME one guaranteeing the first
  // measurement always enters the ring) made that sample ALWAYS become the
  // OLDEST slot of the history, with temp stuck at -273 - and it stayed there
  // for 30 WHOLE minutes of uptime, every time the device rebooted. Since
  // development reboots the device all the time (gravar.sh), the temperature
  // graph ALWAYS seemed to be missing data at its oldest edge.
  //
  // The fix: if there is no FIRST history entry yet (s_hist_ultimo==0) and
  // the temperature is not yet available, this sample only keeps the live
  // MEASUREMENT (s_ultima, already done above) and does NOT start the history
  // clock - the next call to tab5_power_medir() (1s later, still at boot)
  // tries again, and should find the sensor ready. Nothing changes for
  // callers after boot: if tem_temp is false only 30 minutes into running
  // (the sensor really failing), the old behaviour (-273 in the slot) applies
  // again, because by then s_hist_ultimo!=0 and the normal condition takes over.
  if (s_hist_ultimo != 0 || tem_temp) {
    if (s_hist_ultimo == 0 || (agora - s_hist_ultimo) >= TAB5_HIST_SEG) {
      s_hist_ultimo = agora;
      s_hist[s_hist_i].volts = m.volts;
      s_hist[s_hist_i].mA    = m.mA;
      s_hist[s_hist_i].pct   = (int16_t)m.pct;
      // -273 is "not measured": impossible as a temperature, so the graph
      // recognises it without needing a validity field alongside.
      s_hist[s_hist_i].temp = tem_temp ? c : -273.0f;
      s_hist_i = (s_hist_i + 1) % TAB5_HIST_N;
      if (s_hist_n < TAB5_HIST_N) { ++s_hist_n; }
    }
  }

  return m;
}

int tab5_power_cpu_mhz(void)
{
  // esp_clk_cpu_freq() returns Hz and is updated by DFS itself on every
  // frequency change - which is why it is useful to watch PM working live.
  return esp_clk_cpu_freq() / 1000000;
}

void tab5_power_log(const char* etiqueta)
{
  tab5_medida_t m = tab5_power_medir();
  // Compact on purpose: with the log prefix ("I (1606) power: ") the previous
  // version went past 90 columns and wrapped onto two lines in the terminal,
  // which gets in the way of precisely whoever is comparing several
  // measurements in a row.
  ESP_LOGI(TAG, "%-8s %dMHz %.2fV %.1fmA %.0fmW %d%%",
           etiqueta, tab5_power_cpu_mhz(), m.volts, m.mA, m.mW, m.pct);
}

void tab5_power_travas(const char* etiqueta)
{
#if CONFIG_PM_ENABLE
  printf("\n--- PM locks (%s) ---\n", etiqueta);
  // stdout and not ESP_LOG on purpose: the dump comes already formatted over
  // several lines, and a log prefix on each would make the table unreadable.
  esp_pm_dump_locks(stdout);
  printf("--- fim (%s) ---\n\n", etiqueta);
#else
  (void)etiqueta;
#endif
}

// (tab5_power_mostrar is gone: the power panel is drawn by the Energia app
// itself. The HAL measures; the apps draw.)


// ---------------------------------------------------------------------------
// STANDBY MODE
// ---------------------------------------------------------------------------

// Access to Panel_DSI's protected members now lives in a single place,
// shared with tab5_tela.cpp - see the big notice in
// tab5_painel_dsi_interno.hpp (consolidated; before this there was a copy of
// the same trick in each file).
namespace {
  esp_lcd_panel_handle_t painel_dpi(void)
  {
    auto* p = tab5_painel_dsi_interno();
    return p ? p->pegar_handle() : nullptr;
  }

  bool s_em_repouso = false;
  bool s_desmontado = false; // DSI panel and bus destroyed (see desmontar_tela)
  bool s_so_luz = false;     // TDDI screen idle: only the light went off (see tab5_painel)
  int  s_cpu_normal = TAB5_CPU_MAX_MHZ;   // ceiling to restore, saved on entering idle
}

bool tab5_power_em_repouso(void) { return s_em_repouso; }

// ---- which screen (see tab5_power.hpp) ---------------------------------------
// The touch address separates the families unambiguously: the GT911 answers
// at 0x14 (or 0x5D) and the Sitronix TDDI touch at 0x55. Between the two ST
// ones, M5GFX does not keep the model anywhere public - but it programs each
// with its own pixel clock (ST7121 at 70 MHz, ST7123 at 80), read here before
// anyone changes it. An ST matching neither is still treated as TDDI: the
// name comes out "unknown", the behaviour is the conservative one.
static int8_t s_painel = -1;   // -1 = not detected yet

tab5_painel_t tab5_painel(void)
{
  if (s_painel >= 0) { return (tab5_painel_t)s_painel; }
  tab5_painel_t r = TAB5_PAINEL_DESCONHECIDO;
  auto* tp = M5.Display.touch();
  auto* p  = tab5_painel_dsi_interno();
  if (tp && p) {
    const uint8_t end = tp->config().i2c_addr;
    const int mhz = p->config_detail().dpi_freq_mhz;
    if (end == 0x14 || end == 0x5D) { r = TAB5_PAINEL_ILI9881C; }
    else if (mhz == 70)             { r = TAB5_PAINEL_ST7121; }
    else if (mhz == 80)             { r = TAB5_PAINEL_ST7123; }
    ESP_LOGI(TAG, "screen: %s (touch at 0x%02X, factory clock %d MHz)",
             r == TAB5_PAINEL_ILI9881C ? "ILI9881C + GT911" :
             r == TAB5_PAINEL_ST7123   ? "ST7123 (TDDI)" :
             r == TAB5_PAINEL_ST7121   ? "ST7121 (TDDI)" : "unknown", end, mhz);
  } else {
    ESP_LOGW(TAG, "screen: no panel or no touch - called before M5.begin()?");
  }
  s_painel = (int8_t)r;
  return r;
}

const char* tab5_painel_nome(void)
{
  switch (tab5_painel()) {
    case TAB5_PAINEL_ILI9881C: return "ILI9881C + GT911";
    case TAB5_PAINEL_ST7123:   return "ST7123";
    case TAB5_PAINEL_ST7121:   return "ST7121";
    default:                   return "desconhecida";
  }
}

bool tab5_painel_toque_gt911(void) { return tab5_painel() == TAB5_PAINEL_ILI9881C; }

// See tab5_power.hpp. The GT911 datasheet sequence for address 0x14: INT
// HIGH, RST low (>100 us), RST high with INT still high (>5 ms), wait for its
// firmware to come up (~50 ms) and hand INT back as an input. Only
// gpio_set_direction/level: gpio_config would wipe the interrupt type the
// kernel armed on the pin (the touch ISR).
static constexpr gpio_num_t TOQUE_INT = GPIO_NUM_23;   // GT911 INT (and address at reset)

bool tab5_toque_reiniciar(void)
{
  if (!tab5_painel_toque_gt911()) { return false; }
  auto& expansor = M5.getIOExpander(0);
  gpio_set_level(TOQUE_INT, 1);
  gpio_set_direction(TOQUE_INT, GPIO_MODE_INPUT_OUTPUT);   // drives INT and still reads it
  const bool ok = expansor.digitalWrite(IOE0_TP_RST, false);
  vTaskDelay(pdMS_TO_TICKS(10));
  expansor.digitalWrite(IOE0_TP_RST, true);
  vTaskDelay(pdMS_TO_TICKS(10));                             // INT high: address 0x14
  // INT SYNC (the driver's wakeup(), which boot does right after reset): INT
  // LOW ~58 ms, high 2 ms, and only then input. Without this step the GT911
  // comes back answering on I2C but reports NO touches at all - which is
  // what the first version of this reset did (14/11/2026): the idle screen
  // stopped waking with a finger until the device rebooted.
  gpio_set_level(TOQUE_INT, 0);
  vTaskDelay(pdMS_TO_TICKS(58));
  gpio_set_level(TOQUE_INT, 1);
  vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_direction(TOQUE_INT, GPIO_MODE_INPUT);            // the GT911 drives INT again
  gpio_set_pull_mode(TOQUE_INT, GPIO_PULLUP_ONLY);           // as the driver leaves it
  vTaskDelay(pdMS_TO_TICKS(50));                             // its firmware comes up
  ESP_LOGW(TAG, "touch: GT911 reset via the reset pin%s", ok ? "" : " (expander did NOT respond)");
  return ok;
}

// Deep idle (SLPIN + tearing down DSI) only where touch survives it.
static bool repouso_fundo(void) { return tab5_painel_toque_gt911(); }

// Adjusts only the clock ceiling. The floor stays at 40MHz (the "XTAL" the
// old comment talks about) - BUT ONLY WHEN THE REQUESTED CEILING IS AT LEAST
// THAT.
//
// BUG FOUND by the 'energia repouso' command testing a ceiling below 40 for
// the first time: esp_pm_configure REQUIRES min <= max, and the floor was
// fixed at 40 in this function - so "energia repouso 20" asked for
// max=20/min=40 and IDF itself refused with ESP_ERR_INVALID_ARG, always, for
// any ceiling below 40. Nobody had caught it because no real caller
// (tab5_power_repouso()/retomar(), always >=40 until now) ever asked for such
// a low ceiling - the console command was the first.
//
// When the requested ceiling is BELOW 40, the floor follows (min = max):
// there is no DFS range left to vary, the clock stays fixed at that value -
// which is exactly the intent of whoever is testing "my floor IS this number".
static int  s_teto_mhz = 0;      // the last ceiling requested of esp_pm (0 = none)
static bool s_sono_leve = false; // light sleep on right now (only on idle)

// THE FREQUENCIES THE P4 ACCEPTS: the divisions of the 360 MHz PLL and the
// 40 MHz crystal. Asking for another (200, from the Terminal, failed
// silently: ESP_ERR_INVALID_ARG and idle gave up) rounds up to the next valid
// one - never below what was asked.
static int frequencia_valida(int mhz)
{
  static const int VALIDAS[] = { 40, 90, 180, 360 };
  for (int f : VALIDAS) { if (mhz <= f) { return f; } }
  return 360;
}

static bool ajustar_teto(int mhz)
{
#if CONFIG_PM_ENABLE
  mhz = frequencia_valida(mhz);
  int piso = (mhz < 40) ? mhz : 40;
  faixa_segura(mhz, &piso);
  // Touching the ceiling turns light sleep off: whoever wants it back (the
  // kernel, on idle) calls tab5_power_sono_leve() afterwards.
  s_sono_leve = false;
  s_teto_mhz = mhz;
  esp_pm_config_t pm = { .max_freq_mhz = mhz, .min_freq_mhz = piso, .light_sleep_enable = false };
  esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_pm_configure(max=%d) failed: %s", mhz, esp_err_to_name(err));
    return false;
  }
  return true;
#else
  ESP_LOGW(TAG, "clock ceiling ignored: CONFIG_PM_ENABLE is not on");
  (void)mhz;
  return false;
#endif
}

bool tab5_power_sono_leve(bool ligar)
{
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  // Only with the screen TORN DOWN: with DSI up its lock prevents sleep
  // regardless, and in light-only idle (TDDI) the panel stays up.
  if (ligar && (!s_em_repouso || !s_desmontado || s_teto_mhz <= 0)) { return false; }
  if (ligar == s_sono_leve) { return true; }
  const int mhz  = s_teto_mhz > 0 ? s_teto_mhz : 40;
  int piso = (mhz < 40) ? mhz : 40;
  faixa_segura(mhz, &piso);
  esp_pm_config_t pm = { .max_freq_mhz = mhz, .min_freq_mhz = piso, .light_sleep_enable = ligar };
  const esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "light sleep %s: esp_pm_configure %s", ligar ? "on" : "off", esp_err_to_name(err));
    return false;
  }
  s_sono_leve = ligar;
  ESP_LOGI(TAG, "light sleep: %s", ligar ? "on (idle)" : "off");
  return true;
#else
  (void)ligar;
  return false;
#endif
}

bool tab5_power_sono_leve_ligado(void) { return s_sono_leve; }

// See the big notice in tab5_power.hpp. It just reuses ajustar_teto() - the
// same path repouso()/retomar() already use - BUT without touching the
// panel, backlight or DSI stream at all: the screen stays exactly as it was,
// on and drawing. That is why it can test "slow panel + slow CPU, both
// AWAKE" - none of the other paths does that.
bool tab5_power_teto_cpu(int mhz)
{
  const int antes = tab5_power_cpu_mhz();
  const bool ok = ajustar_teto(mhz);
  ESP_LOGI(TAG, "CPU ceiling (screen awake): %d MHz -> %s",
           antes, ok ? "accepted" : "REFUSED (error log above)");
  return ok;
}

// Varies brightness smoothly between two values.
//
// Touches the PANEL directly, not M5.Display.setBrightness(), on purpose:
// LGFX keeps the "official" brightness in a _brightness member, and that is
// what wakeup() restores. If the fade used setBrightness(), the saved value
// would become 0 and the screen would never light up by itself on waking.
static void (*s_gancho_rampa)(void) = nullptr;
void tab5_power_gancho_rampa(void (*passo)(void)) { s_gancho_rampa = passo; }

static void esmaecer(uint8_t de, uint8_t para, int passos = 16, int ms = 12)
{
  auto* p = M5.Display.panel();
  if (!p) { return; }
  for (int i = 1; i <= passos; ++i) {
    const int b = (int)de + ((int)para - (int)de) * i / passos;
    p->setBrightness((uint8_t)b);
    if (s_gancho_rampa) { s_gancho_rampa(); }
    vTaskDelay(pdMS_TO_TICKS(ms));
  }
  p->setBrightness(para);
}

// TWO PATHS THAT DID NOT WORK - on record, so nobody repeats them.
//
// 1. Stopping the DSI stream with esp_lcd_dpi_panel_set_pattern() on idle
//    (and MIPI_DSI_PATTERN_NONE on retomar). Measured: after NONE the DSI
//    bridge sometimes does not resume feeding the panel - screen lit, pale,
//    no content, for up to 15 minutes, with the wake log complete and
//    normal. set_pattern was made to show a test pattern, not to pause.
//
// 2. Lowering the clock with the stream running. It underruns on every
//    frame, and the driver logs with ESP_DRAM_LOGE (esp_rom_printf, ignores
//    esp_log_level_set): ~6.5 ms of blocking UART inside an interrupt every
//    frame. And the driver itself warns that underrun can leave the screen
//    blue with no way back.
//
// What works is what is below: tearing down the panel and the bus.

// Undoes an idle that failed midway, WITHOUT lighting up abruptly. Using
// M5.Display.wakeup() here was a mistake: it restores brightness at once.
static void desfazer_repouso(void)
{
  auto* p = M5.Display.panel();
  if (p) { p->setSleep(false); }
  vTaskDelay(pdMS_TO_TICKS(120));
  tab5_power_acender();
}

// ---------------------------------------------------------------------------
// TEARING DOWN AND REBUILDING THE SCREEN
// ---------------------------------------------------------------------------
//
// The path the two keys above could not open. Measured with
// esp_pm_dump_locks(): the panel holds TWO PM locks, and neither is released
// by set_pattern() or by sleep():
//
//     dsi_dpi   CPU_FREQ_MAX     <- pins the CPU at 360MHz
//     dsi_phy   NO_LIGHT_SLEEP   <- forbids light sleep
//
// In the IDF source both are acquired at CREATION and released only in the
// DESTRUCTOR (esp_lcd_panel_dpi.c 291 -> 405; esp_lcd_mipi_dsi_bus.c 72 -> 151).
// So: to release them, destroy. There is no middle ground.
//
// Destroying solves all three problems at once, which is why it is worth
// the risk:
//   - with no DPI stream there is no underrun, so the clock can really drop;
//   - dsi_dpi released, the clock ceiling takes effect;
//   - dsi_phy released, light sleep is no longer impossible.
//
// INVARIANT THAT MUST NOT BE BROKEN: between tear-down and rebuild, NOBODY may
// draw. The framebuffer belongs to the DPI panel
// (esp_lcd_dpi_panel_get_frame_buffer in Panel_DSI.cpp:143) and is freed
// along with it - drawing there is writing into memory returned to the heap.
// In this system that holds because only the UI task draws, and it is the
// one calling repouso()/retomar(); its idle loop does not touch the display.

static bool desmontar_tela(void)
{
  auto* p = tab5_painel_dsi_interno();
  if (p == nullptr) { return false; }
  auto* bus = p->getBusDSI();
  if (bus == nullptr) { return false; }

  // the double buffer dies FIRST: its line array points into the
  // framebuffer that esp_lcd_panel_del() is about to free.
  tab5_tela_encerrar();

  // Order: panel before bus. The DPI panel was created FROM the bus;
  // deleting the bus first would leave the panel referencing something that
  // no longer exists.
  esp_lcd_panel_handle_t dpi = p->pegar_handle();
  if (dpi != nullptr)
  {
    esp_err_t err = esp_lcd_panel_del(dpi);   // releases dsi_dpi, frees the framebuffer
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "tear-down: esp_lcd_panel_del failed: %s", esp_err_to_name(err));
      return false;
    }
    p->zerar_handle();
  }

  // The lineArray is allocated by Panel_DSI::init with heap_alloc_dma and
  // never freed by anyone. Besides the leak, IT is the guard that makes init
  // return false on the second call ("if (_lines_buffer != nullptr) return
  // false"). Without clearing it, rebuilding is impossible.
  uint8_t** lines = p->pegar_lines();
  if (lines != nullptr) { lgfx::heap_free(lines); p->zerar_lines(); }

  // The buffer was already freed with the panel; leaving the old pointer
  // would be a dangling one waiting for the first careless draw.
  p->zerar_buffer();

  // M5GFX's release() deletes the DBI IO, deletes the DSI bus (releasing
  // dsi_phy) and gives back the LDO channel powering the PHY. A complete
  // teardown, done by the library itself - we need not imitate anything.
  bus->release();
  return true;
}

static bool montar_tela(void)
{
  auto* p = tab5_painel_dsi_interno();
  if (p == nullptr) { return false; }
  auto* bus = p->getBusDSI();
  if (bus == nullptr) { return false; }

  // WARNING, WHOEVER CALLS THIS: THE LIGHT COMES BACK OFF.
  //
  // Panel_Device::init() inside does _light->init(0) (Panel_Device.cpp:67).
  // That is, building the panel zeroes the backlight PWM - and LGFX's
  // _brightness does NOT follow, because it lives in LGFX_Device and not in
  // the panel (LGFXBase.hpp:1519). Result: M5.Display.getBrightness() keeps
  // answering 80 with the lamp at 0. The screen is on, drawing correctly, and
  // invisible.
  //
  // On the idle path that is ON PURPOSE and must not change: retomar() hands
  // the panel back in the dark precisely so the caller can repaint before
  // lighting up, and only then call tab5_power_acender(). Anyone using
  // montar_tela() for another reason must light up themselves - which is
  // exactly what was missing in the first version of the clock change, and
  // the screen came up black until the first idle.
  //
  // Silence LEDC during the rebuild, and ONLY during it.
  //
  // Panel_Device::init() does _light->init(0) (Panel_Device.cpp:67), i.e. it
  // reconfigures the backlight PWM. GPIO 22 has been reserved since boot, so
  // the driver warns "GPIO 22 is not usable, maybe conflict with others" EVERY
  // time the screen wakes. It is benign - brightness keeps working, the
  // acender() right after proves it - but repeated on every touch it becomes
  // noise that hides real warnings. Restored at the end: outside this window
  // an LEDC warning means something again.
  esp_log_level_set("ledc", ESP_LOG_ERROR);

  // Redundant on purpose: Panel_Device::init() inside also calls
  // _bus->init() (line 74), and Bus_DSI::init() has a guard against creating
  // twice. We call it here so a bus failure is reported as a BUS failure, and
  // not as a generic "Panel_DSI::init failed".
  if (!bus->init())
  {
    ESP_LOGE(TAG, "rebuild: Bus_DSI::init failed");
    esp_log_level_set("ledc", ESP_LOG_WARN);
    return false;
  }
  // use_reset=true: sends SWRESET and re-runs the panel's initialisation
  // list. Since the bus has just been created, the panel must be
  // reprogrammed from scratch - no state can be assumed.
  const bool ok = p->init(true);
  esp_log_level_set("ledc", ESP_LOG_WARN);
  if (!ok)
  {
    ESP_LOGE(TAG, "rebuild: Panel_DSI::init failed");
    return false;
  }
  return true;
}

bool tab5_power_repouso(int cpu_mhz)
{
  if (s_em_repouso) { return true; }

  if (painel_dpi() == nullptr) {
    ESP_LOGE(TAG, "idle: could not find the DPI panel handle");
    return false;
  }

  // 1. Fade out SMOOTHLY.
  tab5_marco("idle: dimming", 0);
  esmaecer(M5.Display.getBrightness(), 0);

  // TDDI SCREEN (ST7123/ST7121): stop here, with just the light off. Panel
  // awake and DSI stream up - touch, which lives in the screen chip, keeps
  // scanning and wakes the screen as usual. It costs the high clock of a
  // running panel; that is the price of not risking a device that no longer
  // wakes. See tab5_painel().
  if (!repouso_fundo()) {
    s_so_luz = true;
    s_em_repouso = true;
    ESP_LOGI(TAG, "idle: light off (screen %s: panel stays awake for touch)",
             tab5_painel_nome());
    return true;
  }

  // 2. Put the panel to sleep (brightness is already zero).
  tab5_marco("idle: SLPIN", 0);
  M5.Display.sleep();
  M5.Display.waitDisplay();         // waits for the BUS, not the panel
  vTaskDelay(pdMS_TO_TICKS(200));   // SLPIN takes up to ~120ms to take effect

  s_cpu_normal = tab5_power_cpu_mhz();

  // 3. Tear down panel and bus. That is what releases the PM locks - see the
  //    big block above. After this there IS NO FRAMEBUFFER: nobody may draw
  //    until retomar()'s montar_tela().
  const uint32_t t0 = tab5_agora_ms();
  if (!desmontar_tela()) {
    ESP_LOGE(TAG, "idle: tear-down failed - going back to the lit state");
    desfazer_repouso();
    return false;
  }
  s_desmontado = true;
  ESP_LOGI(TAG, "idle: DSI torn down in %lu ms", (unsigned long)(tab5_agora_ms() - t0));

  // 4. Lower the clock - with no panel there is no underrun, so it is allowed.
  if (!ajustar_teto(cpu_mhz)) {
    ESP_LOGE(TAG, "idle: could not lower the clock to %d MHz", cpu_mhz);
    montar_tela();
    s_desmontado = false;
    desfazer_repouso();
    return false;
  }

  s_em_repouso = true;
  ESP_LOGI(TAG, "idle: DSI torn down, clock %d -> %d MHz", s_cpu_normal, tab5_power_cpu_mhz());
  // THE PROOF. If the tear-down did what it promises, dsi_dpi and dsi_phy
  // vanish from the lock list - and then the clock really dropping is no
  // longer wishful thinking. ONCE PER BOOT: 'energia travas' shows it on request.
  static bool s_provou = false;
  if (!s_provou) { s_provou = true; tab5_power_travas("primeiro repouso"); }
  return true;
}

bool tab5_power_retomar(void)
{
  if (!s_em_repouso) { return true; }

  // TDDI screen idle: nothing was turned off but the light, and the light is
  // turned on by tab5_power_acender(), after the redraw.
  if (s_so_luz) {
    s_so_luz = false;
    s_em_repouso = false;
    ESP_LOGI(TAG, "resume: panel was already up (light-only idle)");
    return true;
  }

  // 1. Raise the clock BEFORE anything visible, and CONFIRM it: montar_tela()
  //    reprograms the panel and creates the DPI stream, and doing that at
  //    90MHz would mean starting already short of PSRAM bandwidth.
  ajustar_teto(s_cpu_normal);
  int mhz = 0;
  for (int i = 0; i < 20; ++i) {          // up to 100ms, checking every 5ms
    mhz = tab5_power_cpu_mhz();
    if (mhz >= s_cpu_normal) { break; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (mhz < s_cpu_normal) {
    // Fail safe: do not light up. Better dark than tearing.
    ESP_LOGE(TAG, "resume: clock stayed at %d MHz (expected %d) - screen stays off",
             mhz, s_cpu_normal);
    return false;
  }

  // 2. Rebuild panel and bus. A freshly created panel starts awake (init(true)
  //    sends SWRESET and re-runs initialisation) and with the LIGHT OFF: the
  //    caller redraws in the dark and only then calls tab5_power_acender().
  const uint32_t t0 = tab5_agora_ms();
  bool ok = montar_tela();
  if (!ok) {
    // A second chance: the first failure may be an LDO or a PHY that needed
    // more time to settle after the release.
    ESP_LOGE(TAG, "resume: rebuild failed - trying once more");
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = montar_tela();
  }
  if (!ok) {
    // Without a panel the device is a dark brick and no code path brings it
    // back. Rebooting is the least bad option: settings are in NVS.
    ESP_LOGE(TAG, "resume: rebuild failed TWICE - rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));   // let the UART drain before the reset
    esp_restart();
  }
  s_desmontado = false;

  // THE AXI PRIORITY, REASSERTED. The ICM registers do not belong to the
  // panel and should not be touched by its destruction - but "should not" is
  // not a guarantee written down anywhere, and the cost is four register
  // writes against waking up at the factory default, which is exactly the
  // blue-flash condition.
  tab5_barramento_priorizar_tela(tab5_barramento_priorizado());

  s_em_repouso = false;
  ESP_LOGI(TAG, "resume: DSI rebuilt in %lu ms, clock at %d MHz",
           (unsigned long)(tab5_agora_ms() - t0), mhz);
  return true;
}

// ---------------------------------------------------------------------------
// PANEL PIXEL CLOCK - the permanent lever against the blue flash
// ---------------------------------------------------------------------------
// THE ARITHMETIC, which is the whole argument.
//
// The panel is 720x1280 and M5GFX programs it with porches of 140+40+40
// horizontally and 20+4+20 vertically (M5GFX.cpp:3214, ILI9881C branch). So
// each frame costs, in pixel cycles:
//
//   total width   = 720 + 220 = 940
//   total height  = 1280 +  44 = 1324
//   cycles/frame  = 940 * 1324 = 1,244,560
//
// At 80MHz that gives 64.3 frames per second. Each frame reads
// 720*1280*2 bytes = 1.84MB from PSRAM. That is: the panel pulls 118.5 MB/s
// from PSRAM ALL THE TIME, and nothing the code does reduces it - it is the
// hardware scanning.
//
// At 60MHz it is 48.2 frames per second and 88.9 MB/s: TWENTY-FIVE PER CENT
// less, for ever, on every transition, in every app, without anyone having
// to remember anything. And the per-line slack grows too: the active line
// goes from 9.0us to 12.0us and the FIFO refill interval from 2.75us to 3.67us.
//
// What is lost: 64Hz becomes 48Hz. In this interface - lists, buttons, a
// grid snake game - nothing needs 64 frames per second.
//
// WHY 60 AND NOT ANOTHER NUMBER: it is the default of M5GFX's own Panel_DSI
// (Panel_DSI.hpp:47, dpi_freq_mhz = 60). The 80 comes from a specific M5GFX
// line for this panel; the porches are counted in PIXELS and scale with the
// clock, so the geometry stays valid.
//
// HONESTY: I could not measure this on the bench. That is why there are two
// safety nets: if the rebuild fails, we revert by ourselves to the old
// value; and the console changes the value at run time ('tela relogio 80'),
// without reflashing.
static uint16_t s_dpi_mhz_pedido = 0;   // 0 = never touched

int tab5_power_relogio_dpi(void)
{
  auto* p = tab5_painel_dsi_interno();
  if (p == nullptr) { return 0; }
  return (int)p->config_detail().dpi_freq_mhz;
}

double tab5_power_quadros_por_segundo(void)
{
  auto* p = tab5_painel_dsi_interno();
  if (p == nullptr) { return 0; }
  const auto d = p->config_detail();
  const double l = p->largura() + d.hsync_back_porch + d.hsync_pulse_width + d.hsync_front_porch;
  const double a = p->altura()  + d.vsync_back_porch + d.vsync_pulse_width + d.vsync_front_porch;
  return (l > 0 && a > 0) ? d.dpi_freq_mhz * 1e6 / (l * a) : 0;
}

// The DPI panel clock comes from 240 MHz / an INTEGER divider (mipi_dsi_hal.c
// uses roundf), so only these "round" values exist (there are also 34.3 and
// 26.7, left out of the table). Asking for 65 really gave 60. Measured on the
// Tab5 (07/11/2026): a real 60 underran (blue screen); a real 80, none.
static const int RELOGIOS_PAINEL[] = { 80, 60, 48, 40, 30, 24 };
static const int N_RELOGIOS_PAINEL = (int)(sizeof(RELOGIOS_PAINEL) / sizeof(RELOGIOS_PAINEL[0]));

// The table value CLOSEST to the request (out of range takes the end; a tie
// goes to the larger).
static int relogio_aproximado(int mhz)
{
  int melhor = RELOGIOS_PAINEL[0];
  for (int i = 1; i < N_RELOGIOS_PAINEL; i++)
  {
    if (abs(RELOGIOS_PAINEL[i] - mhz) < abs(melhor - mhz)) { melhor = RELOGIOS_PAINEL[i]; }
  }
  return melhor;
}

int tab5_power_relogio_vizinho(int atual, int passo)
{
  int i = 0;   // position of the table value closest to the current one
  for (int k = 1; k < N_RELOGIOS_PAINEL; k++)
  {
    if (abs(RELOGIOS_PAINEL[k] - atual) < abs(RELOGIOS_PAINEL[i] - atual)) { i = k; }
  }
  // The table runs from largest to smallest: "up" is the previous index.
  if (passo > 0 && i > 0)                      { i--; }
  else if (passo < 0 && i < N_RELOGIOS_PAINEL - 1) { i++; }
  return RELOGIOS_PAINEL[i];
}

bool tab5_power_relogio_dpi(int mhz)
{
  // Any request becomes the closest real value (see the table above).
  mhz = relogio_aproximado(mhz);

  // TDDI screen: the clock stays at factory default (see tab5_painel). Asking
  // for the value already set is a success; anything else is refused
  // without touching anything.
  if (!tab5_painel_toque_gt911()) {
    if (mhz == tab5_power_relogio_dpi()) { return true; }
    ESP_LOGW(TAG, "panel clock: fixed at %d MHz on the %s screen",
             tab5_power_relogio_dpi(), tab5_painel_nome());
    return false;
  }

  // FINDING: calling this with the screen IDLE is dangerous and had no guard
  // at all. In deep idle the panel is already torn down - _disp_panel_handle
  // and _lines_buffer zeroed -, but the Panel_DSI OBJECT is still alive (only
  // the internal pointers die), so tab5_painel_dsi_interno() below returns
  // non-null and the code carried on. trocar() would call desmontar_tela()
  // and bus->release() A SECOND TIME on a bus already released - M5GFX's
  // Bus_DSI does not promise to be idempotent to a repeated release(), and
  // that is the most likely recipe for the hang observed when testing 'tela
  // relogio'/'energia teto' with the screen asleep. The way out: refuse here,
  // screen by screen, and hand control back to the caller - the same policy
  // as kernel/core.cpp of only touching the panel once awake.
  if (s_em_repouso) {
    ESP_LOGE(TAG, "panel clock: the screen is idle (panel torn down) - "
                  "wake it first and try again");
    return false;
  }

  auto* p = tab5_painel_dsi_interno();
  if (p == nullptr) { return false; }

  const uint16_t antigo = p->config_detail().dpi_freq_mhz;
  if (antigo == (uint16_t)mhz) { return true; }

  // The path is the SAME as idle: tear down and rebuild. It already runs on
  // every wake, so it is not a new path - it is the most exercised one in the
  // module. Reusing it rather than inventing another keeps a single way of
  // recreating the panel, and so a single place to fix when it breaks.
  // THE LIGHT COMES BACK TOO, and lives INSIDE the lambda on purpose.
  //
  // There are two paths that rebuild here: the new clock and the return to
  // the old one when it does not come up. Lighting up only on the success
  // path, a panel refusing 60MHz would go back to 80 working and INVISIBLE -
  // the worst possible outcome, because the log would say "back to the
  // previous one" and the screen would stay black. Every successful rebuild
  // lights up, without exception.
  //
  // No risk of showing garbage: esp_lcd_new_panel_dpi allocates the
  // framebuffers with heap_caps_calloc (esp_lcd_panel_dpi.c:238), so the new
  // panel starts scanning BLACK.
  auto trocar = [&](uint16_t v) -> bool {
    auto det = p->config_detail();
    det.dpi_freq_mhz = v;
    p->config_detail(det);
    if (!desmontar_tela()) { return false; }
    if (!montar_tela())    { return false; }
    // Rewriting the same number is what pushes the brightness to the hardware
    // again - see the notice at the top of montar_tela().
    M5.Display.setBrightness(M5.Display.getBrightness());
    return true;
  };

  ESP_LOGI(TAG, "panel clock: %u -> %d MHz", (unsigned)antigo, mhz);
  if (!trocar((uint16_t)mhz))
  {
    // REVERTS BY ITSELF. A panel that does not come up would leave the device
    // blind, and the user with no way of finding out why - the new value is
    // precisely the one I cannot test here.
    ESP_LOGE(TAG, "the panel did not come up at %d MHz - going back to %u", mhz, (unsigned)antigo);
    if (!trocar(antigo)) {
      ESP_LOGE(TAG, "the panel did not come back even at the old value - rebooting");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    return false;
  }

  s_dpi_mhz_pedido = (uint16_t)mhz;

  // The pages died along with the old panel (desmontar_tela calls
  // tab5_tela_encerrar). The compositor notices from the generation and
  // sends the whole screen to the new panel - nothing to do here.
  //
  // THE AXI PRIORITY, however, does need reasserting. The ICM registers
  // survive the panel's destruction, but relying on that is relying on a
  // detail that was never promised. It costs four writes.
  tab5_barramento_priorizar_tela(tab5_barramento_priorizado());
  return true;
}

// DSI LINK SPEED (Mbps per lane) - FOR TESTING ONLY, not saved. M5GFX 0.2.32
// runs the ILI9881C at 1040 Mbps, the value for the new screens (ST7123); a
// report on M5GFX (issue 334) cites 730 Mbps for the ILI9881C and slight
// flicker when using one screen's setting on the other. This path allows
// comparing without rebuilding: the same tear-down/rebuild as the clock, with
// automatic revert.
int tab5_power_link_dsi(void)
{
  auto* p = tab5_painel_dsi_interno();
  auto* bus = p ? p->getBusDSI() : nullptr;
  return bus ? (int)bus->config().lane_mbps : 0;
}

bool tab5_power_link_dsi(int mbps)
{
  if (mbps < 500)  { mbps = 500; }
  if (mbps > 1500) { mbps = 1500; }
  if (s_em_repouso) {
    ESP_LOGE(TAG, "panel link: the screen is idle - wake it first");
    return false;
  }
  auto* p = tab5_painel_dsi_interno();
  auto* bus = p ? p->getBusDSI() : nullptr;
  if (bus == nullptr) { return false; }
  const uint16_t antigo = bus->config().lane_mbps;
  if (antigo == (uint16_t)mbps) { return true; }
  auto trocar = [&](uint16_t v) -> bool {
    if (!desmontar_tela()) { return false; }   // release the bus: init reads the new config
    auto cfg = bus->config();
    cfg.lane_mbps = v;
    bus->config(cfg);
    if (!montar_tela()) { return false; }
    M5.Display.setBrightness(M5.Display.getBrightness());
    return true;
  };
  ESP_LOGI(TAG, "panel link: %u -> %d Mbps", (unsigned)antigo, mbps);
  if (!trocar((uint16_t)mbps)) {
    ESP_LOGE(TAG, "the panel did not come up at %d Mbps - going back to %u", mbps, (unsigned)antigo);
    if (!trocar(antigo)) {
      ESP_LOGE(TAG, "the panel did not come back even at the old value - rebooting");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    return false;
  }
  tab5_barramento_priorizar_tela(tab5_barramento_priorizado());
  return true;
}

void tab5_power_acender(void)
{
  uint8_t alvo = M5.Display.getBrightness();

  // SAFETY NET: if the brightness saved by LGFX is zero, lighting up with it
  // would leave the screen on and dark for ever - the panel showing content
  // with no light at all, the "washed-out off" look. Never accept zero here.
  if (alvo == 0) {
    ESP_LOGW(TAG, "light up: saved brightness was 0 - using 80 as the floor");
    alvo = 80;
    M5.Display.setBrightness(alvo);   // also corrects the saved value
  }

  // COLD START. With the backlight coming from zero, a very low duty does not
  // light up: the LED driver needs a minimum pulse to leave its idle state,
  // and once lit it copes with a much lower duty (which is why the Settings
  // slider showed light at minimum and waking came back black). When the
  // target is low, first rise to a safe level, hold for a moment and only
  // then go down to the target - the eye sees a brightness settling, not a
  // dead screen.
  static const uint8_t PISO_PARTIDA = 28;
  if (alvo < PISO_PARTIDA) {
    ESP_LOGI(TAG, "light up: cold start %d -> %d -> %d", 0, (int)PISO_PARTIDA, (int)alvo);
    esmaecer(0, PISO_PARTIDA, 8, 8);
    vTaskDelay(pdMS_TO_TICKS(80));
    esmaecer(PISO_PARTIDA, alvo, 12, 12);
    // AND FINISH VIA THE SETTINGS SLIDER'S PATH. Seen on the device: with the
    // saved brightness at minimum, the screen woke bright, and only obeyed
    // after moving the slider (which applies via M5.Display.setBrightness).
    // The ramp writes straight to the panel; the last write goes the same way
    // as the slider, a moment later, so the value that sticks is exactly what
    // the slider would set.
    vTaskDelay(pdMS_TO_TICKS(60));
    M5.Display.setBrightness(alvo);
    return;
  }

  ESP_LOGI(TAG, "light up: ramp 0 -> %d", (int)alvo);
  esmaecer(0, alvo);
}

// tab5_power_diagnostico() (read raw INA226 registers) was removed; it was
// created for the battery calibration investigation, which has closed (see
// the memory record "tab5-bateria-
// calibracao"), and nobody called it any more (neither console nor any app).
