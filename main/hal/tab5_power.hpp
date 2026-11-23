// M5Tab5 power profile.
//
// The idea: EVERYTHING starts off, and each project turns on strictly what it
// uses. The Tab5 comes out of M5.begin() with the camera out of reset, the
// IMU with its gyroscope at maximum performance, the radio powered and both
// 5V rails (Grove and USB-A) on via pull-ups on the expanders - none of it
// requested, and it drains the battery without showing up in the code.
//
// The profile is applied at TWO moments, and they are not interchangeable:
//
//   phase 1  tab5_power_config()   BEFORE M5.begin()
//            Touches the M5.config() flags. The peripheral is never even
//            initialised - it stays in its factory idle state, no I2C traffic.
//
//   phase 2  tab5_power_aplicar()  AFTER M5.begin()
//            Touches what M5Unified turns on unconditionally and does not
//            expose: camera reset, radio rail, brightness, CPU management.
//
// Typical use:
//
//   tab5_perfil_t perfil;          // everything off
//   perfil.radio  = true;          // this project prints over WiFi
//   perfil.brilho = 80;
//
//   auto cfg = M5.config();
//   tab5_power_config(cfg, perfil);
//   M5.begin(cfg);
//   tab5_power_aplicar(perfil);

#pragma once

#include "sdkconfig.h"

// MAXIMUM CPU CLOCK - depends on the P4 revision (sdkconfig.defaults.chip_*):
// 360 MHz on v1.x, 400 on v3.x. The steps esp_pm accepts are the PLL ones
// (maximum, half, quarter) and the divided 40 MHz crystal ones.
#define TAB5_CPU_MAX_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define TAB5_CPU_DEGRAUS "360 180 90 40 20 10 8 5 4 2 1"
#define TAB5_CPU_METADE  "180"
#else
#define TAB5_CPU_DEGRAUS "400 200 100 40 20 10 8 5 4 2 1"
#define TAB5_CPU_METADE  "200"
#endif

#include <M5Unified.h>
#include <stdint.h>

struct tab5_perfil_t
{
  // ---- phase 1: never even initialised -------------------------------------

  // BMI270 (accelerometer + gyroscope). WARNING: when on, M5Unified leaves it
  // with the gyro active and advanced power save OFF (PWR_CONF=0x00) - the
  // chip's worst-case consumption. The hal/tab5_imu service fixes that: with
  // nobody requesting the gyroscope, only the accelerometer, in power-save.
  bool imu           = false;

  // ES7210: the microphones. On = ADC codec powered.
  bool microfone     = false;

  // ES8388: the output codec + SPK_EN on expander 0.
  bool alto_falante  = false;

  // RX8130. Costs almost nothing in current (its own domain); turning it off
  // here saves boot time and I2C traffic, not battery.
  bool rtc           = true;

  // EXT5V_EN, expander 0 bit 2. Powers the Grove port.
  bool grove_5v      = false;

  // USB5V_EN, expander 1 bit 3. Powers the built-in USB-A.
  bool usb_a_5v      = false;

  // ---- phase 2: only possible with the board up ---------------------------

  // CAM_RST, expander 0 bit 6. M5Unified takes the camera out of reset at boot.
  bool camera        = false;

  // WLAN_PWR_EN, expander 1 bit 0. It is the whole ESP32-C6's rail: WiFi AND
  // BLE go through it. Cutting it is the biggest saving after the backlight.
  // READ the comment on tab5_power_radio() before turning it back on at run time.
  bool radio         = false;

  // 0..255. The backlight (PWM on GPIO22) is the board's biggest consumer
  // with the screen on, and the response is almost linear: 80 uses ~1/3 of 255.
  //
  // IN M5tabOS THIS FIELD DOES NOT BELONG TO THE APP. The system overwrites it
  // with the user's preference in sistema_impor_preferencias() - brightness
  // belongs to whoever holds the device, not the program on screen. An app
  // descriptor that sets brightness misleads the reader: the value never
  // reaches the hardware.
  //
  // The field still exists because tab5_power is SELF-CONTAINED and can be
  // copied into projects without an app system, where setting brightness
  // here makes sense.
  uint8_t brilho     = 64;

  // ---- phase 2: CPU power management --------------------------------------

  // Turns on IDF's DFS: the CPU drops by itself to cpu_min_mhz when no task
  // is ready. Requires CONFIG_PM_ENABLE=y in sdkconfig - without it
  // esp_pm_configure() returns ESP_ERR_NOT_SUPPORTED (that was Arduino's block).
  bool pm_dinamico   = true;

  // The Tab5's P4 is revision < 3.0 (see the SILICON REVISION block in
  // sdkconfig.defaults). In that family the valid frequencies are
  // 360, 180, 90 and XTAL/N (40, 20, 10, 8, 5, 4, 2, 1) - the 400/200/100
  // ladder belongs to revision >= 3.0 and does NOT apply here. Any other
  // value makes esp_pm_configure return ESP_ERR_INVALID_ARG.
  //
  // TESTED: lowering the ceiling to 180 WITH THE SCREEN ON (not just on
  // idle) failed in practice - back to 360. That is, even with the AXI-ICM
  // priority already active (tab5_barramento_priorizar_tela), the DSI panel
  // still needs the CPU at the ceiling to sustain the frame without underrun.
  // The PM lock it holds (dsi_dpi, CPU_FREQ_MAX) is not driver conservatism -
  // it is a real dependency of this hardware. There is no intermediate step
  // to try (300 does not exist on this ladder - only 360/180/90 come from the
  // PLL); the next candidate, if it is ever worth reopening this, is to
  // measure with a panel clock LOWER than 60MHz (less bandwidth demanded) and
  // see whether 180 becomes enough - not tried yet.
  int  cpu_max_mhz   = TAB5_CPU_MAX_MHZ;
  int  cpu_min_mhz   = 40;

  // Automatic light sleep on idle.
  //
  // DO NOT USE: with DSI up, sleep does not happen. Idle light sleep belongs
  // to the kernel (tab5_power_sono_leve), not the app profile.
  //
  // Turning on tickless is a decision based on measurement, not taste: with
  // FREERTOS_HZ=1000 the gain depends on there being idle windows of several
  // milliseconds, and the DPI panel may not even allow sleep. Measure with
  // tab5_power_travas() first.
  bool light_sleep_ocioso = false;
};

// ---- applying the profile -------------------------------------------------

// Phase 1. Call with the cfg from M5.config(), before M5.begin().
void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_perfil_t& perfil);

// Phase 2. Call right after M5.begin().
void tab5_power_aplicar(const tab5_perfil_t& perfil);

// ---- run-time controls ---------------------------------------------------

// Cuts or restores the ESP32-C6 rail.
//
// WARNING: turning it off takes down WiFi and BLE together and resets the C6.
// Turning it back on restores power, but the host stack (esp_hosted/esp_wifi)
// does NOT reconnect by itself - SDIO must be renegotiated. Until we write
// that sequence, treat turning it off as a one-way street within a boot:
// turn it off when done with the radio and go to sleep, or reboot to turn it
// back on.
void tab5_power_radio(bool ligar);


// Grove 5V rail (expander 0, PortA). Turning it on at run time is needed to
// PROBE what is plugged in before knowing which app will use it - the module
// needs power to introduce itself. See tab5_grove.
void tab5_power_grove(bool ligar);

// Is the rail on RIGHT NOW? Exists so tab5_grove.cpp can decide whether it
// must wait for the module to boot before probing - a HARDWARE STATE
// question, not a user preference (which is why it lives here and not in
// ajustes.h: HAL asks HAL, never a service - see docs/ESCREVENDO_APPS.md
// section 8).
bool tab5_power_grove_ligado(void);

// USB-A 5V rail (expander 1, USB5V_EN), on its own - without reapplying the
// whole profile. The decider is services/tab5_usb (idle); the rest of the
// time it follows the profile (usb_a_5v). From the UI task (I2C).
void tab5_power_usb_a(bool ligar);

// When the Tab5 last touched the Grove 5V / reapplied the profile
// (esp_timer in ms, 0 = never). Proves or rules out the Tab5 in a node reset.
uint32_t tab5_power_grove_toque_ms(void);
uint32_t tab5_power_aplicar_ms(void);

// CAM_RST. false = camera held in reset.
void tab5_power_camera(bool ligar);

// THE BMI270 GYROSCOPE: on (performance) or only the accelerometer in
// power-save. UI task only (internal I2C). The decider is the hal/tab5_imu
// service (tab5_imu_giroscopio), not the app.
bool tab5_power_imu_giroscopio(bool ligar);

// The boot profile asked for the IMU (cfg.internal_imu). For whoever needs to
// know whether a missing IMU is a choice or a failure - see tab5_imu_medir().
bool tab5_power_imu_pedida(void);

// ---- short history, for the graph -----------------------------------------
//
// It lives HERE, not in the app, because tab5_power_medir() runs once a
// second from the UI task EVEN ON IDLE. That way the history fills up with
// the app closed and the graph is already full on opening - if it lived in
// the app, it would start empty every time.
//
// 60 points every 30s = 30 minutes. Costs 60 * 12 = 720 bytes.
#define TAB5_HIST_N     60
#define TAB5_HIST_SEG   30

// Temperature lives here and not in its own ring because it is the SAME
// instant: both are read on the same 1s tick and drawn on the same time axis.
// Two separate rings would mean the work of matching them up later, and the
// chance of them drifting apart. It is -273 when there was no reading.
typedef struct { float volts; float mA; float temp; int16_t pct; } tab5_ponto_t;   // pct = the system % at that instant

// Copies the history in chronological order (oldest first) into 'saida',
// which must have TAB5_HIST_N slots. Returns how many points there are.
int tab5_power_historico(tab5_ponto_t* saida);

// Percentage FROM THE VOLTAGE CURVE alone (voltage corrected for current).
// It is NOT what the system shows: the system % comes from the estimator in
// tab5_power_medir() (mAh counting + the curve as a slow correction). Exposed
// for diagnostics and for reprocessing old files.
int tab5_power_pct(float volts, float mA);

// Estimator state, for the console: % with fraction, what the curve would
// say now, where the estimator started from, and minutes since the last
// charge (-1 = none this boot).
void tab5_power_soc_debug(float* soc, float* curva, const char** fonte, float* min_desde_carga);

// EXTERNAL SEED: the charge saved by the persister (services/tab5_bateria).
// Only applies if the estimator started from the curve at boot (not from the
// RTC) - and the caller decides when the curve is not to be trusted (booting
// while charging). true = used.
bool tab5_power_soc_semear(float soc, const char* fonte);
// true while the estimator started from the curve this boot and nobody has seeded it.
bool tab5_power_soc_da_curva_no_boot(void);

// ---- measurement (built-in INA226, 5 mOhm shunt on the battery bus) ------

// INTERNAL RESISTANCE of the assembly (battery + wiring + shunt), in ohms.
//
// Used to estimate the AT-REST voltage from the voltage measured under load:
//     V_open = V_measured - I * R      (signed I: + charging, - discharging)
//
// The formula works in both directions, which is why there is only one:
// discharging, the voltage sags and the correction ADDS; charging, the
// voltage is inflated by the charger and the correction SUBTRACTS.
//
// MEASURED, no longer estimated, from the real energia.csv:
//
//   39 current steps larger than 150mA in a 10.3h discharge
//   R = dV/dI  ->  median 0.268 ohm, range 0.179..0.312
//
// It was 0.15 by estimate, almost half the real value - so the I*R correction
// was smaller than it should have been and the percentage dropped when the
// screen came on, which is exactly the symptom this constant exists to remove.
//
// Large steps on purpose: voltage is read at 1.25mV resolution, and at a 20mA
// difference dV is lost in the noise. It is the idle transitions that give a
// clean step.
#define TAB5_R_INTERNA 0.268f

// Measured battery capacity, in mAh - REPLACED by the analysis of the 21.5h
// discharge WITHOUT A SPLICE found in energia.csv (the 1448mAh one came from
// two sessions stitched together + voltage with only 2 decimals; see the big
// comment next to CURVA[] in tab5_power.cpp for the method and for why the
// battery label saying "2400mAh" does not match this).
// Used by the charge estimator (tab5_power.cpp, estimar_pct) and exposed
// here, in one place, so the console can show "what % of capacity" without
// keeping a second copy of the same number.
#define TAB5_CAPACIDADE_MAH 1306.8f

struct tab5_medida_t
{
  float volts;   // bus (2S Li-Po, ~7.4V nominal)
  float mA;      // (+) charging / (-) discharging
  float mW;      // power on the battery bus
  int   pct;     // estimated charge 0..100 (negative = the model cannot tell)
};

// WARNING - CONCURRENCY: these reads go to the INA226 over the internal I2C,
// and M5Unified's I2C_Class has NO mutex at all (checked in the source). If
// another task uses the same bus at the same time - and M5.update() does, to
// read touch and buttons - the two transactions interleave and you read the
// WRONG REGISTER, with no error at all, just meaningless numbers. Call
// everything here from the SAME task that calls M5.update(), or protect the
// bus with a mutex of your own covering both.
tab5_medida_t tab5_power_medir(void);

// The LAST measurement taken, without touching I2C. It is what other tasks
// should use.
//
// Project rule: only the UI task talks on the I2C bus, because M5Unified's
// I2C_Class has no mutex and two interleaving tasks read the wrong register
// without any error. Anything that is not the UI reads from here.
// The UI updates this value every second, awake or idle.
tab5_medida_t tab5_power_ultima(void);

// CPU operating frequency AT THIS INSTANT, in MHz. With DFS on (pm_dinamico)
// it changes at run time, so read it on every update instead of storing the
// value once at boot.
int tab5_power_cpu_mhz(void);

// Prints a log line with the current measurement, tagged with the given
// label. Use before/after each cut to know how much it was worth.
void tab5_power_log(const char* etiqueta);


// Lists the active PM LOCKS on the console.
//
// It exists to settle by measurement, not deduction, the question holding up
// all of this project's power saving: WHO pins the CPU at maximum? Until now
// the answer was inferred (the DPI panel pulls from PSRAM non-stop, so it
// must be that). esp_pm_dump_locks() gives each lock's name and how many
// times it was acquired - if the panel shows up with ESP_PM_CPU_FREQ_MAX, it
// is proven; if another owner shows up, we were hunting the wrong target.
//
// Calling it at different points (boot, with an app open, on idle) and
// comparing the lists shows what lets go and what never does.
void tab5_power_travas(const char* etiqueta);

// ---- standby mode (screen off + low clock) ---------------------------------
//
// THE WHY: lowering the clock with the panel active does NOT work - it was
// measured and DSI started underrunning on every frame. The DPI panel streams
// the framebuffer from PSRAM continuously, and at 180MHz the bus cannot feed
// it in time. M5.Display.sleep() does NOT solve that: M5GFX's setSleep() only
// sends the DCS 0x10 (SLPIN) command to the panel; the DPI controller keeps
// pulling ~1.8MB per frame from PSRAM for a panel that ignores it.
//
// The way out is to stop the stream itself with
// esp_lcd_dpi_panel_set_pattern(): the IDF source says "stop the DSI bridge
// from generating the DPI stream" when the pattern is not NONE. With the
// fetch stopped, no underrun is possible any more and the clock can drop.
//
// Note there are TWO stacked gains, and the first is probably worth more:
// stopping the stream removes continuous PSRAM traffic that exists today
// even with the screen "asleep"; lowering the clock is the bonus only
// available afterwards.
//
// -------------------------------------------------------------------------
// MEASURED with tab5_power_travas() - read before touching this
// -------------------------------------------------------------------------
// esp_pm_dump_locks() at boot returns:
//
//     dsi_dpi   CPU_FREQ_MAX     active 1
//     dsi_phy   NO_LIGHT_SLEEP   active 1
//
// It confirms the deduction about the clock: what pins the CPU is the panel,
// not M5Unified or the silicon revision. But it adds a NEW fact and brought
// with it a limit we did not know about:
//
// 1. The clock lock does NOT stop us lowering the ceiling.
//    ESP_PM_CPU_FREQ_MAX pins the CPU at max_freq_mhz, and that value is ours
//    - which is why idle with ajustar_teto(90) works. What breaks at 90MHz is
//    the underrun, not the lock. The strategy above still holds.
//
// 2. LIGHT SLEEP IS IMPOSSIBLE while the DSI bus exists, and no set_pattern
//    changes that. Verified in the IDF source: in esp_lcd_panel_dpi.c the
//    lock is acquired at panel CREATION (line 291) and only released inside
//    the destructor, along with free() (lines 405-406); in
//    esp_lcd_mipi_dsi_bus.c it is the same (72 -> 151). set_pattern() has not
//    a single reference to pm_lock.
//
// Practical consequence: with DSI up, turning on
// CONFIG_FREERTOS_USE_TICKLESS_IDLE would give ZERO. The way forward was to
// DESTROY panel and bus when the screen turns off and recreate them on wake
// (desmontar_tela/montar_tela) - done. Since 2026-10-16 tickless is on and
// idle really sleeps: tab5_power_sono_leve(), called by the kernel after
// idle, with touch/keyboard armed to wake it (see "SONO LEVE: QUEM ACORDA O
// P4" in kernel/core.cpp).

// Enters idle: turns the screen off, stops the video stream and lowers the
// clock ceiling. Returns false if any step fails (and in that case undoes
// what it did).
//
// cpu_mhz: valid steps on this silicon revision are 360, 180, 90 and XTAL/N.
//
// WARNING: do not enter idle with the radio on without testing. The C6's SDIO
// driver has its own bandwidth requirements, and since the PM lock pins at
// MAXIMUM, lowering the ceiling lowers the clock for it too.
bool tab5_power_repouso(int cpu_mhz = 90);

// Leaves idle, IN THIS ORDER: raises the clock ceiling, CONFIRMS the
// frequency really went up, only then restarts the video stream and wakes
// the screen.
//
// The confirmation is the interlock: restarting the stream still at a low
// clock would bring back the underrun storm. If the frequency does not rise
// in time, this function returns false and LEAVES THE SCREEN OFF on purpose -
// failing dark is better than failing with a tearing image.
bool tab5_power_retomar(void);

// Ramps the screen up to the previous brightness.
//
// SEPARATE from retomar() on purpose: retomar() brings the panel back but
// LEAVES THE LIGHT OFF, so the caller can redraw the screen before any pixel
// is visible. Without that separation, the light comes up showing the old
// framebuffer or the garbage from the DSI bridge resync - the "artefact" on
// waking.
//
//   tab5_power_retomar();   // restores clock, stream and panel, screen still dark
//   ...redraw everything...
//   tab5_power_acender();   // now it is fine
void tab5_power_acender(void);

// Hook called at each step of acender's brightness ramp (the ramp blocks the
// task for ~300 ms; without the hook, anything animated on screen freezes for
// that time). Runs in the UI task. nullptr = none.
void tab5_power_gancho_rampa(void (*passo)(void));

// ---------------------------------------------------------------------------
// WHICH SCREEN THIS TAB5 HAS
// ---------------------------------------------------------------------------
// The Tab5 has shipped with three screens (all 720x1280 MIPI-DSI, same board):
//   ILI9881C + GT911 - the launch (2025): panel and touch on separate chips;
//   ST7123           - since Oct/2025: panel and touch on the SAME chip (TDDI);
//   ST7121           - since Apr/2026: another TDDI from the same family.
// M5GFX detects it by itself in M5.begin() and sets up the right driver;
// ordinary drawing and touch (M5.Display, M5.Touch) work the same on all
// three. What does NOT work the same is what this system does underneath
// M5GFX, and each such point asks here first:
//   - the touch watchdog reads GT911 registers directly (kernel/core.cpp);
//   - deep idle (SLPIN + tearing down DSI) - on a TDDI touch is scanned
//     together with the image, so with the panel asleep touch probably dies,
//     and touch is what wakes the screen. On the ST ones idle only turns the
//     light off;
//   - the pixel clock (tab5_power_relogio_dpi): the porches M5GFX uses on
//     the ST ones put the touch scan in the vertical blanking, and changing
//     the clock changes that gap. On the ST ones it stays at factory default.
// The ST paths were written without a device to test on: they are the
// CONSERVATIVE ones (what M5GFX already does by itself), never a new guess.
//
// Detected once, on the first call - which must be AFTER M5.begin() and
// BEFORE any clock change (main.cpp calls it right after begin). Any task
// after that.
typedef enum {
  TAB5_PAINEL_DESCONHECIDO = 0,
  TAB5_PAINEL_ILI9881C,
  TAB5_PAINEL_ST7123,
  TAB5_PAINEL_ST7121,
} tab5_painel_t;
tab5_painel_t tab5_painel(void);
const char*   tab5_painel_nome(void);
// true only on the ILI9881C: touch is a separate GT911 (direct watchdog,
// GPIO 23 pulse confirmed, deep idle and adjustable clock validated on it).
bool          tab5_painel_toque_gt911(void);

// GT911 RESET via the reset pin (TP_RST, expander 0x43 output 5), the same
// sequence as at boot: INT high during reset (it stays at address 0x14,
// which the driver already uses). For the "mute" GT911: seen on 13-14/11/2026,
// it started pulsing INT ~10x/s WITHOUT ever having a report ready, and only
// came back with the device rebooted - on idle the screen would no longer
// wake. Takes ~130 ms. UI task only (the owner of internal I2C and touch).
// false = not a GT911, or the expander did not respond.
bool          tab5_toque_reiniciar(void);

// ---------------------------------------------------------------------------
// PANEL PIXEL CLOCK
// ---------------------------------------------------------------------------
// The DPI panel reads the framebuffer from PSRAM non-stop, at this clock's
// rate. At 80MHz (what M5GFX programs for the ILI9881C) that is 64 frames
// per second and 118.5 MB/s of CONTINUOUS reading. At 60MHz it is 48 frames
// and 88.9 MB/s.
//
// That 25% less is the only thing that permanently reduces the panel's
// demand - everything else in the system (sliced clearing, avoiding clearing
// twice, spacing bursts) works on the OTHER side of the contest. See the full
// explanation in tab5_power.cpp, alongside the implementation.
//
// Changing the clock TEARS DOWN AND REBUILDS the panel - the same path as
// waking. Returns false (and reverts by itself to the previous value) if the
// panel does not come up.
bool tab5_power_relogio_dpi(int mhz);

// Current clock, in MHz; 0 if there is no panel.
int  tab5_power_relogio_dpi(void);

// DSI link speed (Mbps per lane), FOR TESTING ONLY (not saved). Rebuilds the
// panel like the clock does; reverts by itself if it does not come up. In
// the UI task.
bool tab5_power_link_dsi(int mbps);
int  tab5_power_link_dsi(void);

// Frames per second at the current clock, from the porches M5GFX programmed
// on THIS screen (each has its own). 0 if there is no panel.
double tab5_power_quadros_por_segundo(void);

// Next REAL clock in the table (80, 60, 48, 40, 30, 24): up with passo > 0,
// down with passo < 0. Stops at the ends. The app uses this for the plus/minus
// buttons; tab5_power_relogio_dpi(mhz) approximates any request.
int tab5_power_relogio_vizinho(int atual, int passo);

// ---------------------------------------------------------------------------
// CPU CEILING WITH THE SCREEN AWAKE - diagnostics
// ---------------------------------------------------------------------------
// Changes ONLY the CPU clock ceiling, at run time, WITHOUT entering idle and
// WITHOUT tearing down the panel - unlike tab5_power_repouso(), which turns
// the screen off completely. It exists to test one specific question: does a
// lower panel clock (see tab5_power_relogio_dpi above) make room for a lower
// CPU ceiling, with the screen ON and drawing normally, without underrun?
//
// Nobody had tested that combination yet - only "low CPU with the panel at
// the usual clock" (underran) and "low CPU with the panel torn down" (idle,
// which always worked because there is no panel left to underrun). This
// function allows the third case: both ON and both slow, at the same time.
//
// Valid values on this silicon revision: 360, 180, 90 (PLL) or XTAL/N -
// 40, 20, 10, 8, 5, 4, 2, 1. Returns false and changes nothing if IDF refuses.
bool tab5_power_teto_cpu(int mhz);

// true while idle.
bool tab5_power_em_repouso(void);

// LIGHT SLEEP (automatic light sleep on idle) - only on idle with the screen
// TORN DOWN. The kernel decides (it arms the wake pins: touch and keyboard);
// retomar/adjusting the ceiling turns it off by itself. false = could not
// (not idle, light-only idle, PM or tickless missing from sdkconfig).
bool tab5_power_sono_leve(bool ligar);
bool tab5_power_sono_leve_ligado(void);
