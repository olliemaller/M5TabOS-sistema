// The M5tabOS "core": drawing schedule, app life cycle, power
// policy.
//
// -------------------------------------------------------------------------
// STAGE 1 of docs/ARQUITETURA.md - the ENGINE
// -------------------------------------------------------------------------
// Until now this lived inside kernel/sistema.cpp, together with the APPEARANCE
// (home, status bar, touch routing, calibration) - 2457 lines doing eight
// different things, sharing statics with no boundary.
// Split in two, without changing a line of logic (the only exception is
// documented in kernel/internal.hpp): this file decides WHAT runs and
// WHEN; kernel/shell.cpp decides how the screen LOOKS. The two talk through
// the little that kernel/internal.hpp exposes - never through a raw global
// outside it.
//
// ONE task alone owns the display and I2C, on purpose. LovyanGFX is not
// safe for concurrent access and M5Unified's I2C_Class has no mutex at
// all - two tasks on the same bus read the wrong register without any
// error. That is why apps do NOT draw from their own tasks: they produce data
// into queues and the desenhar() function is called from here.
//
// -------------------------------------------------------------------------
// SPLIT BY TOPIC (only moved between files, no logic changed)
// -------------------------------------------------------------------------
// This file kept the engine state, the touch and keyboard interrupts and the
// wake from light sleep, the UI task (vTaskInterface) and
// sistema_iniciar(). The rest went to kernel/core_*.cpp; what the parts
// share is in kernel/core_interno.hpp:
//   core_toque.cpp    GT911 watchdog and magnifier, continuous reading, fingers, bench (simulate touch/gesture/open)
//   core_radios.cpp   Bluetooth, Mesh over the internet, Wi-Fi on/off, Grove and the Mesh path
//   core_apps.cpp     open/close app, SD mount in its own task, refused app
//   core_energia.cpp  screen kept on by request, power-saving contract, panel clock, profile
//   core_som.cpp      speaker idling, USB pins, streamed sound, beep
//   core_desenho.cpp  usable area, measured passes, content drawing pass

#include "app.h"
#include "esp_attr.h"
#include "tab5_sd.hpp"
#include <atomic>
#include "core_interno.hpp"
#include "bloqueio_anim.hpp"
#include "ritmo.hpp"
#include "rolagem.hpp"
#include "tela_virtual.hpp"
#include "boot_anim.hpp"
#include "orientacao.hpp"
#include "compositor.hpp"
#include "trabalho.hpp"
#include "pool_tarefas.h"
#include "tab5_monitor.hpp"
#include "registro.h"
#include "tab5_log.hpp"
#include "tab5_camera.hpp"
#include "ajustes.h"
#include "tab5_ble.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_teclado.hpp"
#include "tab5_usb.hpp"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "grove.hpp"
#include "idioma.h"
#include "tab5_sensores.hpp"
#include "tab5_imu.hpp"
#include "tab5_hora.hpp"
#include "tab5_barramento.hpp"
#include "tab5_bateria.hpp"
#include "tab5_ensaio.hpp"
#include "tab5_wifi_cochilo.hpp"
#include "console.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

// ---------------------------------------------------------------------------
// WHERE THE UI IS - for when it freezes without leaving anything in the log.
// ---------------------------------------------------------------------------
// Each loop step leaves its name here (ETAPA) and each full turn is counted.
// The monitor (another task) reads this 1x/s: a loop stopped for over 3 s
// becomes an event with the NAME of the step it stopped at; and the console
// 'ui' command shows it on the spot, even with the screen frozen. 'dedo' says whether touch is still being read.
static volatile const char* s_etapa   = "boot";
static std::atomic<uint32_t>    s_voltas  = 0;
static volatile uint32_t    s_volta_ms = 0;   // when the last turn finished
static volatile uint32_t    s_dedo_ms  = 0;   // the last time there was a finger on the screen
#define ETAPA(nome) (s_etapa = (nome))

// ---------------------------------------------------------------------------
// THE SYSTEM TOUCH (normal touches that were not being recognised)
// ---------------------------------------------------------------------------
// Until now the click was M5Unified's wasClicked(), and it rejects two cases
// that are normal one-finger touches on this screen:
//   - the finger moved more than 8 px between down and up (becomes a "flick").
//     At 294 DPI that is 0.7 mm - the natural roll of a fingertip exceeds it;
//   - the finger stayed more than 500 ms (becomes a "hold"). An unhurried touch vanished.
// In both cases the touch simply did not happen, with no log at all.
//
// Now the recogniser belongs to the SYSTEM, on the LVGL model (CLICKED): down
// and up without dragging = click, of any duration. "Dragging" = moved away
// from the down point by more than TOQUE_TOLERANCIA_PX, or the system scroll
// moved (rolagem_foi_arraste). ~2 mm, well below any gesture - and the
// SAME limit as the lists (ROLAGEM_ARRASTE_MIN): one limit across the system.
#define TOQUE_TOLERANCIA_PX ROLAGEM_ARRASTE_MIN
bool     s_dedo_na_tela = false;   // OUR state: went down and has not come up yet
static int      s_toque_x0, s_toque_y0;   // where it went down
static int      s_toque_x, s_toque_y;     // where it is (keeps the release position)
static int      s_toque_desvio = 0;       // largest distance from the down point, px
static uint32_t s_toque_desde = 0;

// THE GT911 READ MUST NOT MISS THE RELEASE. The M5GFX driver only reads the chip
// when the interrupt pin (GPIO 23) is low; with it high, it returns the LAST
// data read. If the release notice slips between two loop turns, the stale
// data says "finger on screen" forever: no click, the screen never goes
// idle, and the next touch vanishes too - the screen seems frozen until a
// few touches later. So with a finger on the screen, the read is ALWAYS over
// I2C (pin_int = -1): the GT911 keeps each report until it is read, and the
// release report is read for certain. Without a finger, it goes back to
// waiting on the pin - an idle screen spends no I2C (and the ISR on the same pin wakes the loop on the next touch).
//
// AND THE PULSE MUST NOT BE LOST (symptom: the screen went idle with the
// finger in use, with CreazySnake open). The GT911 signals by a PULSE on the
// pin, and the driver only looks at the pin's LEVEL at M5.update() time. With
// the screen active (game at 20 ms, or anything pending) the interrupt was
// off, and the loop sampled the pin every 20 ms at a fixed phase: if the pulse
// always fell between two samples, NO new touch was seen - neither by the
// game nor by the idle countdown. Now the interrupt stays ON always and only
// NOTES the pulse (s_toque_pulso); the following read goes to I2C for certain.
// Waking the queue still happens only when idle/still (s_isr_acorda) - the reason
// it was born switched off (dragging filling the queue) still holds.
volatile bool s_toque_pulso = false;   // the ISR saw a pulse since the last read
static volatile bool s_isr_acorda  = false;   // the ISR also wakes the loop (idle / still)
std::atomic<uint32_t> s_pulsos  = 0;       // GT911 pulses since boot (touch watchdog)


static const char *TAG = "core";

// ---- geometry --------------------------------------------------------------
// ALTURA_BARRA/ALTURA_NAV lived here until Stage 1; now they come from
// kernel/internal.hpp because kernel/shell.cpp needs them too.

// GEOMETRY OF THE "INICIO" (HOME) BUTTON (the pill in the navigation strip) - moved
// to kernel/shell.cpp, together with desenhar_barra_nav() and tratar_toque(),
// which are what really use these constants now.

// (REPINTURA_MIN_MS, the 80ms brake between repaints, is gone:
// it was a BANDWIDTH limit for drawing on the visible page. With the compositor, everything
// is drawn on the virtual screen and goes to the panel at its frame rate.)
static uint32_t s_ultimo_teclado  = 0;
// Panel pixel clock, in MHz. See tab5_power_relogio_dpi() - it is the only
// thing that PERMANENTLY reduces the bandwidth the panel consumes from PSRAM,
// and therefore the margin left for everything else to draw without flicker.
//
// NO LONGER A CONSTANT: it became a user preference, saved in
// ajustes_relogio_painel() - the console ('tela relogio') and the Settings app save there,
// and boot applies whatever is stored, instead of always reverting to 60.

// ---- behaviour -------------------------------------------------------------
// Time without touch before idle. Comes from the user's SETTING, not a constant:
// it is the preference of whoever holds the device, like brightness. See ajustes.h for
// why the limits are 15 and 60 s.
static inline uint32_t ocioso_ms(void) { return (uint32_t)ajustes_tela_s() * 1000u; }
static const uint32_t PERIODO_UI  = 20;
// Keyboard open: the longest turn between two touch reads. Below the
// 128 ms after which the GT911 driver starts discarding the pending report (see
// ajustar_leitura_do_toque).
static const uint32_t TOQUE_FRESCO_MS = 100;

// (PERIODO_OCIOSO/OCIOSO_APOS_MS are gone. The awake loop was
// sampled at 25ms so as not to miss a quick touch - "the definitive cure is touch
// by INTERRUPT". It arrived: when still, the UI sleeps with the GPIO 23 interrupt
// enabled and wakes the instant it is touched. See the end of the loop.)

// How often to ask the keyboard whether there is a key.
//
// It used to be every cycle (20ms) = 50 I2C transactions per second, always. Human
// typing does not exceed ~10 keys per second; 60ms is imperceptible to the finger and
// cuts the keyboard chatter to a third.
static const uint32_t TECLADO_MS = 60;
// With a key that repeats while held (delete, arrow), read at the UI's pace:
// the repeat is 70 ms, and at 60 ms it would come out every second read.
static uint32_t teclado_ms(void) { return tab5_teclado_segurando() ? 20 : TECLADO_MS; }
// Loop period when idle. It was 200ms and that was TOO SLOW for touch: the
// M5.update() only sees the finger if it is on the screen at the sampling instant, and
// a 100ms touch fits entirely between two 200ms samples. Detecting by
// level does not help if there is no sample during the touch.
//
// At 80ms, a typical human touch (150ms or more) is always seen. TODAY it only applies
// when the touch ISR did not come up: with it, idle sleeps until the next
// real deadline (~1 s) and the touch wakes the loop via the interrupt. (Idle
// drops the CPU to the floor - 40 MHz, see tab5_power_repouso.)
static const uint32_t PERIODO_ZZZ = 80;

// Clock ceiling when idle: FIXED, decided once on entering idle.
// Reconfiguring PM (esp_pm_configure) with light sleep active froze the Tab5
// (both cores stuck in a critical section, one coming from esp_pm_lock_release,
// caught via JTAG on 06/11/2026) when a download started with the screen already
// off. 180 is the highest below the PSRAM's 200 MHz (see faixa_segura).
static const int REPOUSO_TETO_MHZ = 180;
static const uint32_t BARRA_MS    = 2000;

// ---- state -----------------------------------------------------------------
const app_t* s_app = nullptr;      // NULL = home screen - extern in internal.hpp

// ---------------------------------------------------------------------------
// TOUCH SCROLLING
// ---------------------------------------------------------------------------
// Lives in the CORE, and not in each app, for the same reason the usable area does:
// it is geometry, and geometry repeated in eight places becomes eight ways to get it wrong.
//
// The app declares altura_conteudo and thinks no more about it. What shifts the drawing,
// clips to the area, converts touch coordinates and tells a drag from a
// click is the code below - now delegated to kernel/rolagem.hpp, the mechanism
// SHARED with any app that needs its own scrolling (Mesh,
// consolidated - see the big note in rolagem.hpp about WHY
// this stopped being this file's alone).
rolagem_t s_rol;   // zero-init is already VERTICAL/INICIO - extern in internal.hpp

// TWO levels of "dirty", and the distinction between them is what kills the flickering:
//
//   s_sujo_layout - the SCREEN CHANGED (app opened, app closed, woke from idle).
//                   Here it is worth clearing the area and redrawing everything.
//   s_sujo_dados  - only the NUMBERS changed. Redraw on top, WITHOUT clearing.
//
// Before there was a single flag, and every redraw began with a black fillRect over the
// whole area - 720x1120 pixels cleared and repainted every second. That is
// literally a black blink at the redraw rate.
// Since apps write with a background colour (setTextColor(frente, fundo)), the text
// paints its own rectangle and needs no prior clearing.
bool s_sujo_layout = true;   // extern in internal.hpp - shell.cpp writes it too
bool s_sujo_dados  = true;   // extern in internal.hpp

// LOCK SCREEN. Sits IN FRONT of whatever is open (home or app): the app
// stays alive underneath, it just neither draws nor receives touch while this is
// true. Turned on when waking from idle (if the setting is on) and
// turned off by the swipe up (tratar_gesto, shell.cpp).
bool s_bloqueado = false;

static uint32_t s_ultimo_toque   = 0;
uint32_t s_ultimo_desenho = 0;
static uint32_t s_ultima_barra   = 0;
static uint32_t s_acordou_em     = 0;   // the instant it FINISHED waking

// THE FINGER THAT WOKE THE SCREEN must not press anything.
//
// There was only a 500ms grace period counted from the START of waking. But
// waking itself eats that time: rebuilding the DSI, repainting the whole screen,
// waiting for the frame to go out and ramping the backlight. When that exceeds half a second -
// and it does, the log itself measures it -, the grace expires BEFORE the finger leaves the screen,
// the release becomes a click, and the device opens the app that was under the finger.
//
// Time was the wrong measure. What defines the problem is not "how long it took", it is
// "is it the SAME finger". This flag follows the press: born on waking and only dies
// when the finger leaves the screen. While it stands, no click gets through -
// no matter whether waking took 50ms or 5s.
bool s_dedo_do_despertar = false;
static uint32_t s_repouso_falhou = 0;   // instant of the last failed attempt
static uint32_t s_ultima_medida  = 0;
// POWER-SAVING CONTRACT (app.h): with the screen off the P4 sleeps between turns,
// and each turn is a wake-up. The same jobs, longer deadlines -
// a real event (touch, key, notice) still wakes it immediately.
#define MEDIDA_MS()  sistema_ritmo_ms(1000, 5000)       // INA226, sensors, time (the guard counts real time)
#define BARRA_RMS()  sistema_ritmo_ms(BARRA_MS, 5000)   // when idle: the notice beep
// The UI's longest wait per turn: the watchdog deadline minus 2 s (see
// esp_task_wdt_add, at the start of the task).
static const uint32_t UI_ESPERA_MAX_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000u - 2000u;

TaskHandle_t s_tarefa_interface = nullptr;   // captured at creation, see sistema_iniciar()



bool sistema_bloqueado(void) { return s_bloqueado; }

// EVENT QUEUE - STAGE 2 of docs/ARQUITETURA.md, first slice.
//
// Until now the main loop only knew "vTaskDelay(period) and check everything n
// times". That means waking 12.5x/s (PERIODO_ZZZ=80ms) EVEN WITH THE SCREEN
// OFF, just to ask "did anything happen?" - and the answer was almost always
// no. Measured (command "energia travas 40"): esp_pm's own "rtos0" PM
// lock stays held precisely because core 0 is never
// idle long enough for it to be released - see docs/ARQUITETURA.md,
// section 2.2.1.
//
// The change here is only in HOW the loop waits, never in WHAT it decides: every
// dirty flag, every pintura_mudou_i/f/s, every barra_mudou() stays
// EXACTLY the same - they are just checked at a smarter pace
// (the next real deadline, or a real touch interrupt) instead of
// a fixed number.
//
// ONLY IDLE uses the adaptive deadline (see the calculation at the end of the loop). Awake,
// the timeout passed to xQueueReceive() is the SAME PERIODO_UI/PERIODO_OCIOSO as
// always - the only gain there is swapping vTaskDelay() for a wait that would ALSO
// wake earlier if something arrived in the queue, without changing the observable cadence.
//
// THE ALARM CLOCK IS A TASK NOTIFICATION, no longer a queue. The queue
// carried an 'evento_t' (touch, service, key) that nobody read - the loop
// always re-checked everything anyway. For "wake task X" FreeRTOS has the
// direct notification: faster than the queue, no queue RAM, and no "queue
// full, notice thrown away" (it ACCUMULATES, and one ulTaskNotifyTake takes them all).
// Who notifies: the touch ISR (GPIO 23), the keyboard ISR (G50) and
// sistema_acordar_interface() (notifications, services, requests from other
// tasks).
//
// ON ITS OWN INDEX (1), not 0: index 0 is the default for everything that uses
// notifications underneath (FreeRTOS stream buffers, some drivers), and a
// notice of ours there could wake a wait that is not ours. With the
// old sdkconfig (only 1 entry) it falls back to 0 - works as before, without
// the protection. CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2 in
// sdkconfig.defaults.
#define UI_AVISO_IDX ((configTASK_NOTIFICATION_ARRAY_ENTRIES) > 1 ? 1 : 0)

static inline void ui_avisar_da_isr(void)
{
  if (!s_tarefa_interface) { return; }
  BaseType_t acordou = pdFALSE;
  vTaskNotifyGiveIndexedFromISR(s_tarefa_interface, UI_AVISO_IDX, &acordou);
  portYIELD_FROM_ISR(acordou);
}

// false = the touch ISR did not come up (for some rare reason - see
// sistema_iniciar()); idle goes back to the usual fixed PERIODO_ZZZ, the SAME
// behaviour the system had before this slice. It never makes anything worse, it just
// misses out on the adaptive ceiling.
static bool s_toque_isr_ok = false;

// TDDI SCREEN (ST7123/ST7121): the ISR is installed, but only takes effect after
// PROVING that the pin pulses - the driver for these touch panels does not even look at GPIO 23, and
// nobody has tested the pin on them. Until proven, s_toque_isr_ok stays false and the
// UI runs on the interrupt-free path (20 ms awake, PERIODO_ZZZ when
// idle), which works with any touch panel. Proof: 3 edges during ONE
// touch read over I2C. See validar_isr_tddi().
static bool s_toque_isr_instalada = false;
static bool s_tecla_isr_ok = false;          // the keyboard ISR (G50) came up - see isr_teclado

// THE INTERRUPT IS BORN OFF AND IS ONLY TURNED ON INSIDE IDLE - found while
// reviewing this design, before writing the first line: while a
// finger drags on the screen, the GT911 pulses this same pin on every new report
// (5-20ms, the controller's own refresh rate). If the interrupt
// were on the whole time feeding this queue, and that SAME queue
// also governed PERIODO_UI/PERIODO_OCIOSO, the queue would never be empty
// during a drag - xQueueReceive() would return almost instantly on every
// call, and the loop would stop honouring the hand-picked 20/25ms,
// running as fast as the loop body itself allowed. Hence:
// s_isr_acorda turns on only the instant tab5_power_repouso() succeeds (or
// with the screen still) and turns off when the screen again has reason to run at
// 20 ms. The interrupt itself stays on the whole time: it
// NOTES each pulse (s_toque_pulso, see ajustar_leitura_do_toque) - with it
// off, a touch whose pulse fell between two loop samples was lost.
//
// WARNING FOR THE FUTURE: this assumes nobody reconfigures GPIO 23 after
// boot. True today - checked, Touch_GT911::sleep()/
// wakeup() (which REUSE this same pin as an output) are never called
// anywhere in this project, only M5.Display.sleep()/wakeup() (which only touches the
// panel). If one day someone adds M5.Touch.sleep()/wakeup() to
// save power on the touch controller, that will reconfigure the pin
// underneath this ISR and break it silently - the symptom would be "idle
// stops gaining the adaptive ceiling and goes back to fixed PERIODO_ZZZ with no log
// explaining why". If that happens, start looking here.
// THE PIN PROOF ON A TDDI SCREEN (see s_toque_isr_instalada). Called on every
// awake turn; once proven, it does nothing more.
static void validar_isr_tddi(bool tocando)
{
  static uint32_t base = 0;
  static bool dedo = false;
  if (s_toque_isr_ok || !s_toque_isr_instalada) { return; }
  if (tocando && !dedo) { base = s_pulsos; }
  if (tocando && (s_pulsos - base) >= 3) {
    s_toque_isr_ok = true;
    ESP_LOGI(TAG, "touch on screen %s: the INT pin pulses - idle and still screen by interrupt",
             tab5_painel_nome());
  }
  dedo = tocando;
}

// LIGHT SLEEP: WHO WAKES THE P4. In light sleep the edge interrupt does not
// arrive; only the LEVEL "gpio wakeup" takes the chip out of sleep. On entering sleep
// the touch and keyboard pins become level wake-ups (the opposite of each one's
// idle level); on leaving they go back to edge. By level the
// interrupt would repeat while the pin stays active (the keyboard INT stays
// low until the queue is read) - so the ISR itself turns off on the first
// armed firing, with gpio_ll (inline, ISR-safe).
static volatile bool s_despertar_armado = false;
static bool s_toque_nivel_ocioso = true;
// G50 only wakes the chip by level while it is HIGH when arming. Low
// with nobody reading the queue (keyboard the probe did not find, another accessory on
// ExtPort1), level wake-up would pull the chip out of every light sleep immediately -
// idle with awake consumption, no log. That is what happened with the keyboard
// docked and the old "disabled" setting.
static bool s_tecla_despertar = false;

static void tecla_despertar(bool sim)
{
  if (sim) {
    if (!s_tecla_despertar) { gpio_wakeup_enable((gpio_num_t)TAB5_TECLADO_INT, GPIO_INTR_LOW_LEVEL); }
    gpio_intr_enable((gpio_num_t)TAB5_TECLADO_INT);   // the ISR turns itself off on the level firing
  } else if (s_tecla_despertar) {
    gpio_wakeup_disable((gpio_num_t)TAB5_TECLADO_INT);
    gpio_set_intr_type((gpio_num_t)TAB5_TECLADO_INT, GPIO_INTR_NEGEDGE);
    gpio_intr_enable((gpio_num_t)TAB5_TECLADO_INT);
  }
  s_tecla_despertar = sim;
}

// ARMED BEFORE SWITCHING TO LEVEL. It was the other way round: gpio_wakeup_enable switches the
// pin to LEVEL interrupt immediately, and if the level was already active
// (the finger on the screen while it was turning off, a GT911 pulse landing right on the
// read of s_toque_nivel_ocioso) the ISR fired with s_despertar_armado
// still false - it did not turn itself off, and the level fired it again, endlessly, on the
// same core as this task. Armed first, the worst case is one pointless
// firing (rearmar_despertar() turns it back on).
static void armar_despertar(void)
{
  s_despertar_armado = true;
  if (s_toque_isr_instalada) {
    s_toque_nivel_ocioso = gpio_get_level(GPIO_NUM_23) != 0;
    gpio_wakeup_enable(GPIO_NUM_23, s_toque_nivel_ocioso ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  }
  if (s_tecla_isr_ok) { tecla_despertar(gpio_get_level((gpio_num_t)TAB5_TECLADO_INT) != 0); }
  esp_sleep_enable_gpio_wakeup();
}

// A firing that woke nobody (the level read when arming was already stale)
// left the touch interrupt turned off by the ISR itself: the next touch
// would only be seen at the loop deadline (seconds). With the pin back at its idle
// level, turn it back on - and touch wakes immediately again.
static void rearmar_despertar(void)
{
  if (!s_despertar_armado) { return; }
  if (s_toque_isr_instalada) {
    if ((gpio_get_level(GPIO_NUM_23) != 0) == s_toque_nivel_ocioso) {
      gpio_intr_enable(GPIO_NUM_23);
    } else {
      // PIN STUCK ACTIVE: the GT911 only releases INT when someone reads the
      // report, and when idle it is only read on a pulse - which is exactly what is
      // off. Seen on 26/10 (log): locked screen that touch would not
      // light up but a key would. Force the read on the next turn; it releases the
      // pin and the turn after turns the interrupt back on.
      s_toque_pulso = true;
    }
  }
  // G50 follows the current level: the queue was already checked this turn
  // (tab5_teclado_pendente), so low here means a pin nobody releases.
  if (s_tecla_isr_ok) { tecla_despertar(gpio_get_level((gpio_num_t)TAB5_TECLADO_INT) != 0); }
}

static void desarmar_despertar(void)
{
  if (!s_despertar_armado) { return; }
  s_despertar_armado = false;
  if (s_toque_isr_instalada) {
    gpio_wakeup_disable(GPIO_NUM_23);
    gpio_set_intr_type(GPIO_NUM_23, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(GPIO_NUM_23);
  }
  if (s_tecla_isr_ok) { tecla_despertar(false); }
}

static void IRAM_ATTR isr_toque(void*)
{
  if (s_despertar_armado) { gpio_ll_intr_disable(&GPIO, GPIO_NUM_23); }   // level: once only
  s_toque_pulso = true;          // always: the next read goes to I2C
  ++s_pulsos;
  if (!s_isr_acorda) { return; } // screen active: the loop already runs at 20 ms, do not wake needlessly
  ui_avisar_da_isr();
}

// PHYSICAL KEYBOARD WAKES THE SCREEN. The keyboard INT (G50 on ExtPort1, active
// low) goes down on each event and only goes up when the queue is read (the driver clears
// INT_STAT on each read) - one edge per key, never a burst, so it
// can stay on permanently, unlike the touch one. It only wakes the queue in the
// same case as the touch one (s_isr_acorda: idle or still screen); awake, the
// periodic read already picks up the key. With no keyboard docked the pull-up holds
// the pin high and it never fires.

static void IRAM_ATTR isr_teclado(void*)
{
  if (s_despertar_armado) { gpio_ll_intr_disable(&GPIO, TAB5_TECLADO_INT); }   // level: once only
  if (!s_isr_acorda) { return; }
  ui_avisar_da_isr();
}

// Wakes the UI NOW, without waiting for the next periodic deadline - called
// by whoever published something worth noticing immediately (see
// notificacao.cpp and the camera bridge further down). Safe to call from
// ANY task: it only puts an event on the queue (never blocks, 0 wait) -
// if the queue is full (which would require 4 notices without the UI running
// a single loop turn, practically impossible), the event is dropped without
// error, because the loop itself will wake for another reason soon.
// The pending notices. Atomic because any task publishes and only the
// UI consumes - exchange(0) takes everything at once without losing what arrives
// in between.
static std::atomic<uint32_t> s_avisos{0};

void sistema_avisar(uint32_t avisos)
{
  s_avisos.fetch_or(avisos, std::memory_order_relaxed);
  sistema_acordar_interface();
}

// The period of the app open NOW (0 = only wakes on a notice or touch).
uint32_t periodo_do_app(void)
{
  // Locked: the only "app" that draws is the background animation (~25 Hz) with the
  // clock on top (see desenhar_bloqueio).
  if (s_bloqueado) { return BQ_ANIM_PERIODO_MS; }
  if (!app_vivo()) { return 0; }
  return s_app->periodo_agora ? s_app->periodo_agora() : s_app->periodo_ms;
}


void sistema_acordar_interface(void)
{
  if (!s_tarefa_interface) { return; }
  xTaskNotifyGiveIndexed(s_tarefa_interface, UI_AVISO_IDX);
}


// FROM OUTSIDE THE UI, WAKE IT. When still, the UI sleeps
// until the next deadline (see the end of the loop) - without this, a request from an app
// task would wait up to 1s. From INSIDE the UI it only marks: waking itself
// on every draw would make the loop spin without ever sleeping.
static inline void acordar_se_de_fora(void)
{
  if (xTaskGetCurrentTaskHandle() != s_tarefa_interface) { sistema_acordar_interface(); }
}

void sistema_pedir_redesenho(void) { s_sujo_dados = true; acordar_se_de_fora(); }

// FULL redraw: the system clears the area and the app repaints everything (the pass
// reaches it as full - pintura_completa). The ordinary pedir_redesenho() is
// incremental: the app redraws only what changed. Both go through the virtual
// screen and the compositor; the difference is only how much the app repaints.
//
// Rule: a value changed, use pedir_redesenho(). The SCREEN changed, use this.
void sistema_pedir_redesenho_completo(void) { s_sujo_layout = true; acordar_se_de_fora(); }


const char* sistema_app_atual(void) { return s_app ? s_app->nome : nullptr; }
bool sistema_na_interface(void) { return s_tarefa_interface && xTaskGetCurrentTaskHandle() == s_tarefa_interface; }
// ---------------------------------------------------------------------------
// UI task
// ---------------------------------------------------------------------------
static void vTaskInterface(void *pvParameters)
{
  (void)pvParameters;
  s_ultimo_toque = tab5_agora_ms();

  // ON THE REAL WATCHDOG, and not just the idle task's one.
  //
  // The system watchdog (see sdkconfig, ESP_TASK_WDT) by default only
  // watches the two IDLE tasks - it catches a task that hangs the whole
  // CORE (never yields the CPU, not even for idle to run), but does NOT catch a
  // real DEADLOCK: this task blocked forever on a mutex or
  // semaphore that nobody will ever give back, while the rest of the system
  // keeps running normally outside. Without subscribing, a hang
  // like that would leave the screen frozen forever, with NO sign of it in the
  // log - the device would look "alive" (idle CPU available) and be
  // deaf and dumb inside.
  //
  // The longest period this loop waits is the idle pace (5 s, see
  // MEDIDA_MS), and the wait is capped at UI_ESPERA_MAX_MS - 2 s below the
  // watchdog deadline (CONFIG_ESP_TASK_WDT_TIMEOUT_S, 10 s). With the old 5 s
  // deadline, idle's 5 s wait tripped the watchdog on every turn. If one day
  // the loop does not come back here within that deadline, the
  // watchdog restarts the device - worse than frozen, better than hung
  // forever and invisible.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "could not subscribe the UI to the watchdog");
  }

  // WiFi only comes up here, and not in app_main, for a concrete reason: turning on
  // the C6 rail is a write to the I2C expander, and this task owns I2C.
  // Also, coming up after the UI, the screen is already drawn when the
  // radio starts negotiating - the user sees "connecting" instead of waiting.
  if (ajustes_wifi()) { tab5_wifi_ligar(); }

  // BLUETOOTH. Same reason as WiFi for living here: turning it on needs the
  // C6 rail (I2C). Only the user's button turns it on (see sistema_bluetooth);
  // with Mesh over Bluetooth its service comes up together - it does not depend on
  // Grove, so it waits for no probing.
  if (ajustes_bluetooth()) { bt_aplicar(true); }

  // ExtPort1 keyboard. Here, and not in app_main, because it is I2C - and this task
  // owns the bus. With no keyboard docked it just notes it and carries on; there is no
  // enable setting - the HAL finds the keyboard by itself (tab5_teclado.cpp).
  tab5_teclado_iniciar();

  // P4 temperature sensor. It is not I2C (it is an internal ADC), but it starts here
  // with the others so boot has a single place where sensors come up.
  tab5_sensores_iniciar();

  // SCREEN PRIORITY ON THE BUS. Before any app draws: it is the
  // arbitration that decides whether the blue flash can happen or not, and it
  // does not depend on anything the apps do.
  tab5_barramento_priorizar_tela(true);

  // THE VIRTUAL SCREEN (docs/COMPOSITOR.md, stage 3a). Before the first draw: from
  // here on, home, bars and apps draw on it, and only the compositor
  // writes to the panel pages.
  tela_virtual_iniciar();

  // THE SYSTEM WORKER (kernel/trabalho.hpp): whatever is slow leaves
  // this task.
  trabalho_iniciar();

  // PANEL CLOCK. The other half of the same fight: priority decides who
  // wins the contest, this REDUCES the contest. M5GFX programs 80MHz for this
  // panel - 64 frames per second and 118.5 MB/s read from PSRAM non-stop. At
  // 60MHz it is 48 frames and 88.9 MB/s: a quarter less permanent demand,
  // and no app needs to know this exists.
  //
  // Here, before the first draw: the change tears down and rebuilds the panel, and
  // doing that with the screen already in use would blank the content for a moment.
  if (tab5_painel_toque_gt911()) { tab5_power_relogio_dpi((int)ajustes_relogio_painel()); }   // TDDI: keeps the factory value

#ifdef TAB5_MOSTRAR_ANIM_BOOT
  // ONLY NOW: the panel already has its final clock and will not be torn down again
  // this boot (a clock change would blank the screen mid-animation), and the
  // virtual screen already exists to draw through.
  boot_anim_mostrar();
#endif

  // Who is on Grove? The watcher (kernel/grove.cpp) probes a moment later,
  // with home already drawn - probing takes seconds and no app depends
  // on it to open.
  grove_iniciar();

  for (;;) {
    // MEASUREMENT (kernel/compositor.hpp, comp_estat): the turn starts here, on
    // waking. If it ends in a frame, the whole cost goes to the histogram.
    const int64_t volta_t0 = esp_timer_get_time();
    ETAPA("touch (M5.update, I2C)");
    // Navigation locked = keyboard (or capture) reading touch by level: the
    // I2C on every turn, so the driver never discards the finger going down.
    ajustar_leitura_do_toque(s_nav_travada);
    M5.update();
    const uint32_t t = tab5_agora_ms();

    atender_pedido_de_relogio();

    // NEW PANEL (boot, clock change): in direct mode the pages are the
    // ONLY copy of the screen - a new panel is born with nothing of ours, and the only way
    // to get the screen back is to redraw it entirely. (Waking from idle
    // handles its own case further down, before turning on the light.)
    if (!tab5_power_em_repouso() && comp_painel_novo()) { s_sujo_layout = true; }

    // The background parts (kernel/fundo.h): whoever finished on their own is cleaned up, the
    // stop requested by the console happens, and the effective profile is reapplied
    // when the set changed. Awake or idle - the background does not sleep.
    ETAPA("background parts");
    fundo_manter();
    ETAPA("Grove watch / opening with SD");

    // APP OPENING WITH SD PENDING: has the background task (vTaskMontarSDApp)
    // finished since the last cycle? Complete it now, with the screen already showing
    // "abrindo..." (opening...) for a few frames - see abrir_app() and concluir_abertura_do_sd().
    if (s_app_carregando && !s_sd_montando) {
      concluir_abertura_do_sd();
    }
    // THE CARD CAME BACK with the refusal screen open: open the app for real.
    if (s_app_recusado && !tab5_sd_emprestado() && !s_bloqueado) {
      const app_t* ap = s_app_recusado;
      fechar_app();
      abrir_app(ap);
    }

    // One INA226 read per second, AWAKE OR IDLE. This task is
    // the only one that may go to I2C, so it is what feeds the cache that the
    // power log consumes. Without this, the log would only have data while
    // the status bar was being drawn - that is, nothing when idle.
    if ((t - s_ultima_medida) >= MEDIDA_MS()) {
      s_ultima_medida = t;
      ETAPA("medidas (I2C: energia, sensores, IMU, hora)");
      tab5_power_medir();
      // Battery empty: warn and power off before the brownout (services/tab5_bateria).
      tab5_bateria_vigiar();
      tab5_ensaio_vigiar();   // only does anything when 'energia ensaio' was requested
      tab5_wifi_cochilo_vigiar();   // Wi-Fi off with the screen off (services/tab5_wifi_cochilo)
      // Same tick: both feed the same bar, and a single cadence avoids
      // it changing by halves at different instants.
      tab5_sensores_medir();
      // IMU, 1x/s - only for the CACHE that other apps (not app_imu, which reads
      // directly and faster - see the big note in tab5_imu.hpp about why
      // the cadences differ on purpose) can query without
      // touching the internal I2C bus.
      // NOT WHEN IDLE: nobody reads this cache with the screen off (whoever
      // follows the IMU - kernel/orientacao - measures by itself every 200 ms, and only with an
      // app open). It was one I2C transaction per second, every second, for
      // nothing; awake, it keeps serving the console.
      // When idle only the gyroscope follows the request (tab5_imu_repouso).
      if (!tab5_power_em_repouso()) { tab5_imu_medir(); } else { tab5_imu_repouso(); }
      // Time: GPS/Mesh as the source, RTC (internal I2C) as the destination - same
      // rule that only the UI touches the bus. It only actually writes
      // every ~10min (see RESSINCRONIZAR_MS); the 1x/s call is only to
      // notice a new source as soon as it appears.
      tab5_hora_sincronizar();
      vigiar_mesh_internet(t);
    }

    // Physical keyboard: every TECLADO_MS. When there is no key it is ONE
    // I2C transaction (just the event counter), cheap enough for the awake loop.
    //
    // THE TRANSLATION IS THE DRIVER'S (hal/tab5_teclado): it knows the keyboard - the
    // HID mode, the table of 70 keys, the dead keys for accents - and
    // delivers the event ready-made. Here it is only handed to the app. Up to 4 keys per
    // turn: typing fast, more than one fits in the interval, and one per turn
    // would leave the text lagging behind the fingers.
    if (!tab5_power_em_repouso() && (t - s_ultimo_teclado) >= teclado_ms()) {
      s_ultimo_teclado = t;
      ETAPA("teclado (I2C)");
      tab5_tecla_t ev;
      for (int k = 0; k < 4 && tab5_teclado_ler(&ev); ++k) {
        s_ultimo_toque = t;          // typing counts as use: do not turn the screen off
        if (s_bloqueado) {
          // SCREEN LOCKED: the keyboard does not talk to the app behind it - only
          // Enter, which unlocks (the counterpart of the swipe up). Before this
          // guard, whatever was typed on the lock screen ended up in the conversation.
          if (ev.codigo == TAB5_TECLA_ENTER) {
            ESP_LOGI(TAG, "unlocked by the keyboard (Enter)");
            desbloquear_tela();
            tab5_teclado_descartar();   // the rest of the burst does not go to the app
            break;
          }
          continue;
        }
        if (s_painel_avisos) {
          // NOTIFICATION CENTRE OPEN: the app is hidden and does not listen. Esc
          // closes the centre; the rest is swallowed (no typing blind).
          if (ev.codigo == TAB5_TECLA_ESC) { painel_avisos_abrir(false); }
          continue;
        }
        if (app_vivo() && s_app->ao_teclar) {
          s_app->ao_teclar(ev.texto, ev.codigo, ev.mod);
          sistema_pedir_redesenho();
        } else {
          ESP_LOGI(TAG, "key: \"%s\" code=%u%s%s%s (no app listening)",
                   ev.texto, (unsigned)ev.codigo,
                   (ev.mod & TAB5_TECLA_CTRL)  ? " +CTRL"  : "",
                   (ev.mod & TAB5_TECLA_SHIFT) ? " +SHIFT" : "",
                   (ev.mod & TAB5_TECLA_ALT)   ? " +ALT"   : "");
        }
      }
    }

    // When idle, detect touch by LEVEL (is there a finger on the screen?) and not by edge.
    // wasClicked() fires on RELEASE, and the idle loop runs every 200ms:
    // a quick touch starts and ends within the same interval, M5.update()
    // never sees the transition, and the touch is lost. That is why you sometimes
    // had to touch two or three times to wake it.
    ETAPA("touch: decide");
    const bool tocando = (M5.Touch.getCount() > 0);
    if (tocando) { s_dedo_ms = t; }
    const auto toque = M5.Touch.getDetail();
    if (!tab5_power_em_repouso()) { ETAPA("touch: watchdog"); vigiar_toque(t, tocando); lupa_toque(t, tocando); validar_isr_tddi(tocando); }
    vigiar_toque_repouso(t, tocando, tab5_power_em_repouso());

    // KEY WHEN IDLE: the keyboard queue is checked on every idle turn
    // (the G50 pulse wakes the turn immediately; without it, the ~1 s deadline). One
    // I2C transaction per turn with a keyboard docked; without one, just the G50
    // level - when low, a freshly docked keyboard is probed and the key wakes.
    bool tecla_acorda = false;
    if (tab5_power_em_repouso()) {
      tecla_acorda = tab5_teclado_pendente();
    }

    // The wake request only applies to the idle period in which it arrived: awake, it
    // is discarded - left over, it would wake the NEXT idle period right on entry.
    const bool pedido_acordar = s_pedir_acordar;
    s_pedir_acordar = false;
    if (tab5_power_em_repouso()) {
      const bool sim_acorda = s_sim_x >= 0 || s_sim_abrir || s_sim_fechar || pedido_acordar;
      if (tocando || tecla_acorda || sim_acorda) {
        s_sim_x = -1;   // the request that woke it does not become a click, like the finger
        s_ultimo_toque = t;
        // What woke it does not count as use: the finger does not become a click
        // (s_dedo_do_despertar), and the key does not become a letter (discarded).
        if (tocando) { s_dedo_do_despertar = true; }
        if (tecla_acorda) { tab5_teclado_descartar(); }
        ESP_LOGI(TAG, "waking (%s): app=%s", tocando ? "touch" : "keyboard", s_app ? s_app->nome : "home");
        // TURN THE ISR OFF HERE, before anything else - see the big note next to
        // s_toque_isr_ok. Awake, a drag would pulse this pin every
        // 5-20ms and the queue would never be empty if the interrupt stayed
        // on.
        s_isr_acorda = false;
        // The pins go back to edge BEFORE resuming (which also turns off light
        // sleep, via the clock ceiling): awake, touch pulses every 5-20 ms.
        desarmar_despertar();
        ETAPA("repouso: acordando");
        if (tab5_power_retomar()) {
          tab5_usb_repouso(false);   // the USB-A 5V comes back: the flash drive mounts again
          // TURN BACK ON WHAT THE APP RELEASED IN ao_repouso() (e.g. the camera) - only
          // after the panel is back up, so as not to reopen a
          // DMA consumer before the panel channel has been rebuilt.
          // See the big note in kernel/app.h next to ao_repouso/ao_retomar.
          // The camera that idle paused comes back first (the app already finds it running).
          tab5_camera_despausar(app_vivo());
          if (app_vivo() && s_app->ao_retomar) {
            s_app->ao_retomar();
          }
          // retomar() brings the panel back on air with the LIGHT STILL OFF and the
          // pages black - and in direct mode they were the only copy of the screen.
          // FULL redraw straight onto the back page, and only after the swap is
          // CONFIRMED does the light come up: the first thing the eye sees is already the
          // right content. comp_painel_novo() is consumed here so the top
          // of the loop does not request a second full redraw.
          const uint32_t t0 = tab5_agora_ms();
          bloqueio_ao_ligar_tela();
          comp_painel_novo();
          desenhar_barra_status(tela_virtual(), true);
          desenhar_conteudo(true, true);
          comp_apresentar();
          comp_esperar_troca();
          ESP_LOGI(TAG, "waking: redraw took %lu ms",
                   (unsigned long)(tab5_agora_ms() - t0));

          // The brightness ramp blocks this task; the lock screen animation
          // advances one frame per step so it does not freeze.
          if (s_bloqueado) { tab5_power_gancho_rampa(animar_na_rampa); }
          tab5_power_acender();
          tab5_power_gancho_rampa(nullptr);

          // Marked HERE, after everything is ready. The time grace became a
          // secondary safety net (the flag above is the main one), and now it counts
          // from the moment the screen became usable - not from when
          // waking started.
          s_acordou_em = tab5_agora_ms();

          // SAFETY NET, NOW ONLY WHEN IT IS NEEDED.
          //
          // It was born for the washed-out screen: in that design the panel was
          // RESYNCHRONISED and the first frame after that could come out bad,
          // so a second redraw 20ms later fixed it.
          //
          // Today idle TEARS DOWN the panel and resume creates a new one. With
          // double buffering, what went to the screen is a whole frame painted from
          // scratch and published in one go - there is no "dubious first frame"
          // to fix. What was left was the COST: a second full
          // redraw (on home that is the eight icons, 578KB) fired at the
          // VISIBLE page exactly while the light ramps up. The worst-placed
          // burst there was, right at the instant the eye is looking.
          //
          // Without double buffering it is another story: there the drawing was done on
          // the screen itself, piece by piece, and a second pass still makes sense.
          // (The "safety net" of the second redraw died with the
          // compositor: the frame that lights up is always whole.)
        } else {
          // retomar() already logs the reason. Here we only record that the screen stayed
          // off on purpose, so the log tells the whole story.
          ESP_LOGE(TAG, "waking: retomar() failed - screen stays off");
        }
      }

      if (tab5_power_em_repouso()) { rearmar_despertar(); }

      // Notification beep + turning off idle I2S, ALSO when idle -
      // found, see the big note in
      // atualizar_beep_e_ociosidade_do_som() further up the file. Same
      // 2s step the bar uses when awake (s_ultima_barra is
      // shared between the two branches: they are mutually exclusive, never
      // run in the same cycle, so there is no contention over it).
      if ((t - s_ultima_barra) >= BARRA_RMS()) {
        s_ultima_barra = t;
        atualizar_beep_e_ociosidade_do_som(t);
        // The flash drive music finished with the screen off: now the 5V drops.
        if (tab5_power_em_repouso()) { tab5_usb_repouso(true); }
      }
    } else {
      if (tocando) { s_ultimo_toque = t; }

      // Touch by LEVEL, for whoever asked: delivered every cycle (20 ms), with the
      // finger still on the screen. It is what wasClicked() does not give - it only speaks on
      // release, and a game cannot wait for the finger to lift to turn the snake.
      // The rising edge is up to the app; here we only count what is there now.
      //
      // The 500 ms grace after waking applies equally: the finger that lit the
      // screen must not become a move.
      if (app_vivo() && s_app->ao_tocar_nivel
       && !sistema_app_encoberto() && !s_dedo_do_despertar && (t - s_acordou_em) > 500) {
        const area_t a = area_util();
        // SAME CORRECTION AND SAME STRIP AS THE CLICK (shell.cpp, tratar_toque):
        // TOQUE_DY applied here too, and with navigation locked the bottom strip
        // belongs to the app - a point in it is delivered on the last line of the area.
        const int nav_y0 = M5.Display.height() - ALTURA_NAV;
        auto y_do_app = [&](int y_cru, bool* vale) {
          int y = y_cru - TOQUE_DY;
          *vale = y >= ALTURA_BARRA && (y < nav_y0 || s_nav_travada);
          if (y >= nav_y0) { y = nav_y0 - 1; }
          return y;
        };
        bool dentro = false;
        const int ty = y_do_app(toque.y, &dentro);
        dentro = dentro && tocando;
        // ONLY THE FINGERS ON THE SCREEN NOW. M5Unified's getCount() counts a finger
        // that has already lifted for TWO more samples (states touch_end and then
        // none), and getDetail(i) indexes the raw report of the last read -
        // beyond its point count, the entries are stale. Read through
        // getCount, the keyboard saw the key "still pressed" ~40 ms after the
        // finger left: every letter lagged two turns, and the next finger that
        // came down in that window with the same id (the GT911 reuses the lowest
        // free one) looked like the same finger sliding - the previous key vanished.
        // Here: the 5 slots, only the pressed ones, each id once.
        s_n_toques = 0;
        for (int i = 0; i < (int)m5::Touch_Class::TOUCH_MAX_POINTS && s_n_toques < SISTEMA_TOQUES_MAX; ++i) {
          const auto& dd = M5.Touch.getDetail(i);
          if (!dd.isPressed()) { continue; }
          bool repetido = false;
          for (int k = 0; k < s_n_toques; ++k) { if (s_toques[k].id == dd.id) { repetido = true; break; } }
          if (repetido) { continue; }
          bool vale = false;
          const int y = y_do_app(dd.y, &vale);
          if (!vale) { continue; }
          s_toques[s_n_toques].id = dd.id;
          s_toques[s_n_toques].x  = (int16_t)(dd.x - a.x);
          s_toques[s_n_toques].y  = (int16_t)(y - a.y + s_rol.atual);
          ++s_n_toques;
        }
        // SCROLL ADDED HERE TOO. ao_tocar() already did this and this one did
        // not - the asymmetry did not show because no scrolling app used
        // level touch. Leaving it so would plant a bug for the first one
        // that did: the two touch callbacks MUST speak the same coordinate
        // language, otherwise the app gets one right and the other wrong.
        s_app->ao_tocar_nivel(dentro ? toque.x - a.x : -1,
                              dentro ? ty - a.y + s_rol.atual : -1,
                              dentro);
      }

      // DRAG: accumulates while the finger is in the usable area. All the
      // drag/distance/sign maths now lives in
      // kernel/rolagem.hpp - see the big note there about why this
      // stopped being this file's alone.
      {
        const area_t ar = area_util();
        s_rol.max = limite_do_app(ar);
        const bool na_area = tocando
                          && toque.y >= ALTURA_BARRA
                          && toque.y <  (M5.Display.height() - ALTURA_NAV);
        // Do NOT mark s_sujo_layout here. Pure scrolling has its own,
        // cheaper path (see the repaint block in the main loop): it shifts
        // the already-drawn pixels instead of clearing the whole area and redrawing
        // everything again on each touch sample.
        rolagem_arrastar(&s_rol, toque.y, na_area && !sistema_app_encoberto() && s_rol.max > 0);
      }

      // THE SYSTEM CLICK - see TOQUE_TOLERANCIA_PX at the top of the file.
      if (toque.isPressed()) {
        if (!s_dedo_na_tela) {
          s_dedo_na_tela = true;
          s_toque_x0 = toque.x; s_toque_y0 = toque.y;
          s_toque_desvio = 0;
          s_toque_desde = t;
        }
        const int dx = abs(toque.x - s_toque_x0), dy = abs(toque.y - s_toque_y0);
        if (dx > s_toque_desvio) { s_toque_desvio = dx; }
        if (dy > s_toque_desvio) { s_toque_desvio = dy; }
        s_toque_x = toque.x; s_toque_y = toque.y;
        // HOLDING A HOME ICON lifts it for moving (shell.cpp). With an
        // icon lifted the touch is its own: the release becomes neither a click nor a gesture.
        if (!s_app && !s_app_carregando && !sistema_app_encoberto() && !s_dedo_do_despertar && (t - s_acordou_em) > 500
         && home_segurar(s_toque_x, s_toque_y, t, t - s_toque_desde, s_toque_desvio)) {
          s_toque_consumido = true;
        }
      } else if (s_dedo_na_tela) {
        s_dedo_na_tela = false;
        s_ultimo_toque = t;
        home_soltar();   // saves the order, if an icon was moved
        // A DRAG IS NOT A CLICK. Without this guard, finishing a scroll would press
        // whatever was under the finger - and the finger always ends on top of something.
        // rolagem_foi_arraste() is called on EVERY release: it also
        // resets the measurement for the next touch.
        const bool rolou   = rolagem_foi_arraste(&s_rol);
        const bool arrasto = rolou || s_toque_desvio > TOQUE_TOLERANCIA_PX;
        // Two guards, and the first is the one that solves it: is it the same finger that
        // woke the screen? The second catches the accidental touch right after.
        const bool valido = !arrasto && !s_dedo_do_despertar && (t - s_acordou_em) > 500
                         && !s_toque_consumido;
        if (diag_toque_ligado()) {
          ESP_LOGI(TAG, "touch: %lu ms, deviation %d px -> %s", (unsigned long)(t - s_toque_desde),
                   s_toque_desvio, valido ? "CLICK" : rolou ? "scroll" : arrasto ? "drag"
                                         : s_toque_consumido ? "consumed (level)" : "wake/grace");
        }
        // GESTURE (sideways drag on home, swipe up on the lock screen). A
        // vertical app-scroll drag does not get here because the app only
        // sees gestures with home open; the finger that woke the screen never counts.
        if (arrasto && !s_dedo_do_despertar && (t - s_acordou_em) > 500 && !s_toque_consumido) {
          tratar_gesto(s_toque_x - s_toque_x0, s_toque_y - s_toque_y0, s_toque_y0);
        }
        if (valido) {
          ETAPA("handling touch (open/close app, ao_tocar)");
          tratar_toque(s_toque_x, s_toque_y);
        }
      }

      // BENCH: the console's request, on the right task.
      // With navigation locked, switching apps from outside is refused too:
      // the lock belongs to the open app, not just the INICIO (home) button.
      // WITH THE APP STILL "ABRINDO..." (mounting the card) the requests WAIT for the
      // next turn - the same rule as touch (tratar_toque swallows). Serving
      // now would call ao_fechar() on an app whose ao_abrir() never ran, and
      // then concluir_abertura_do_sd() would open an app that already left the screen.
      if (s_sim_fechar && !s_app_carregando) {
        s_sim_fechar = false; s_ultimo_toque = t;
        if (s_app && s_nav_travada) { ESP_LOGW(TAG, "close refused: '%s' locked navigation", s_app->nome); }
        else if (s_app)             { fechar_app(); }
      }
      if (const app_t* ap = (s_app_carregando ? nullptr : s_sim_abrir)) {
        s_sim_abrir = nullptr; s_ultimo_toque = t;
        if (s_app && s_nav_travada) {
          ESP_LOGW(TAG, "open '%s' refused: '%s' locked navigation", ap->nome, s_app->nome);
        } else {
          if (s_app) { fechar_app(); }
          abrir_app(ap);
        }
      }
      // EXTERNAL APPS RELOADED (the card came back from the flash drive): swap the icons
      // only on the home screen and with no finger on it - no icon lifted midway.
      if (!s_app && !s_app_carregando && !s_dedo_na_tela && sistema_apps_de_fora_atender()) {
        home_ordem_carregar();   // the saved order; new ones go at the end
        sistema_pedir_redesenho_completo();
      }
      if (s_simg_pedido && !s_dedo_na_tela) {
        s_simg_pedido = false;
        s_ultimo_toque = t;
        ESP_LOGI(TAG, "simulated drag %d,%d -> %d,%d", s_simg[0], s_simg[1], s_simg[2], s_simg[3]);
        tratar_gesto(s_simg[2] - s_simg[0], s_simg[3] - s_simg[1], s_simg[1]);
      }
      if (s_sim_x >= 0 && !s_dedo_na_tela) {
        const int sx = s_sim_x, sy = s_sim_y;
        s_sim_x = -1;
        s_ultimo_toque = t;
        ESP_LOGI(TAG, "simulated touch at %d,%d", sx, sy);
        tratar_toque(sx, sy);
      }

      // End of touch: the consumed mark does not carry over to the next one. Also
      // clears the touchless mark of the recogniser in progress (the level saw a
      // finger that the click was not tracking).
      if (!s_dedo_na_tela) { s_toque_consumido = false; }

      // The waking finger only stops counting when it LEAVES the screen. After the
      // click block, on purpose: on the release pass the recogniser
      // still needs to see the flag standing.
      if (!tocando) { s_dedo_do_despertar = false; }
    }

    if (!tab5_power_em_repouso()) {
      // The failed attempt has a 5s grace. Without it, an idle attempt that
      // fails midway undoes everything, returns false, and 20ms later the idle
      // condition is still true and it tries AGAIN - a loop of
      // turning off and on, which is what makes the bars flicker.
      const bool pode_tentar = (s_repouso_falhou == 0) || ((t - s_repouso_falhou) > 5000);

      // A keep-screen-on request has just been released: the deadline restarts now.
      static uint32_t acordado_antes = 0;
      const uint32_t acordado = s_acordado.load();
      if (acordado_antes && !acordado) { s_ultimo_toque = t; }
      acordado_antes = acordado;
      if ((t - s_ultimo_toque) > ocioso_ms() && pode_tentar && !acordado) {
        // Say the CONFIGURED time too: it is the only proof, in the log, that the
        // Settings value really is arriving here.
        ESP_LOGI(TAG, "idle after %u s without touch (Settings value)",
                 (unsigned)ajustes_tela_s());
        // TESTED: 20MHz broke the Mesh UART (the service went back
        // to showing "sem modulo" (no module) even with the C6L plugged in and answering before).
        // Back to 40. The next idea - low floor ONLY when tab5_grove_dono()
        // is GROVE_DONO_NINGUEM, high when Mesh/GPS is up - still
        // needs an isolated test (see conversation) before going in
        // here: we still need to know how low the internal I2C (touch/INA226, which run
        // the WHOLE idle period, with Grove empty or not) tolerates going.
        // TELL THE APP BEFORE TEARING DOWN THE SCREEN - see the big note in
        // kernel/app.h next to ao_repouso/ao_retomar (the camera+idle
        // crash). Called here, with the panel still up, so the
        // app releases hardware (DMA/etc) that cannot coexist with the panel
        // being destroyed and recreated by tab5_power_repouso() just below.
        ETAPA("repouso: entrando");
        // The lock screen background (~1.6 MB in PSRAM) need not sleep
        // with it: waking draws and paints another one.
        if (s_bloqueado) { bq_anim_soltar(); }
        // THE OPEN APP'S CAMERA STOPS WITH THE SCREEN OFF - system rule
        // (hal/tab5_camera, tab5_camera_pausar): nobody sees the preview, and
        // left on it would hold 180 MHz. Applies to C and Berry apps without any
        // hook; the browser (another consumer) keeps filming.
        if (app_vivo()) { tab5_camera_pausar(s_app->nome); }
        if (app_vivo() && s_app->ao_repouso) {
          s_app->ao_repouso();
        }
        // THE CLOCK CEILING is fixed (REPOUSO_TETO_MHZ), decided only here, on
        // entering idle; when idle, DFS drops to 40 MHz just the same.
        // What forbids light sleep is the resources in use (kernel/recursos).
        {
          char r[160];
          recursos_resumo(r, sizeof(r));
          ESP_LOGI(TAG, "idle: resources in use: %s (clock ceiling %d MHz)", r, REPOUSO_TETO_MHZ);
        }
        if (!tab5_power_repouso(REPOUSO_TETO_MHZ)) {
          s_repouso_falhou = t;
          s_sujo_layout = true;      // the screen came back: redraw what was there
          // tab5_power_repouso() FAILED - the panel was NOT torn down, so
          // the app stays up with the screen on and the hardware it just
          // released in ao_repouso() (lines above) has to come back NOW, otherwise
          // it stays off until the next idle attempt succeeds.
          tab5_camera_despausar(app_vivo());
          if (app_vivo() && s_app->ao_retomar) {
            s_app->ao_retomar();
          }
        } else {
          s_repouso_falhou = 0;
          // USB-A with nobody using it is not left powered in the dark (a
          // plugged-in device would hold off light sleep) - services/tab5_usb.
          tab5_usb_repouso(true);
          // TURN THE ISR ON HERE - only now, with the screen actually off, is it safe
          // to let touch wake the queue without disturbing PERIODO_UI/OCIOSO
          // (see the big note next to s_toque_isr_ok). Clearing the notification prevents
          // a leftover event from BEFORE sleeping from causing a spurious
          // wake one cycle after entering here.
          if (s_toque_isr_ok) {
            ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, 0);   // clears whatever was left over
            s_isr_acorda = true;
          }
          // LIGHT SLEEP (power-saving contract, app.h): only with touch proven by
          // interrupt - without it, nobody would take the chip out of sleep on a touch.
          if (s_toque_isr_ok && ajustes_sono_leve()) {
            armar_despertar();
            if (!tab5_power_sono_leve(true)) { desarmar_despertar(); }
          }
        }
      } else {
        // THE APP CLOCK is the frame pace (kernel/ritmo): the requested
        // period, in whole panel scans.
        const uint32_t per = periodo_do_app();
        if (per && ritmo_falta_ms() == 0) { s_sujo_dados = true; }

        // NOTICES: what the services said changed since the last turn.
        // Only what the app declared it wants reaches it; the rest is discarded
        // (home updates the services line immediately, on its own).
        //
        // COVERED (lock screen or notification centre), NOTHING IS DELIVERED: the
        // notices are kept until the app becomes visible again. The app behind the lock screen is NOT visible, and
        // the ao_aviso of whoever shows a conversation (ESP-NOW) marks everything as read
        // - it was the notice that vanished from the bar without anyone opening the app: it arrived
        // with the screen off, waking locked it, and the app "read" it from behind.
        // The open centre has aged (a notice arrived or was read): again.
        if (painel_avisos_mudou()) { s_sujo_layout = true; }
        else if (painel_avisos_dados_mudou()) { s_sujo_dados = true; }
        const uint32_t av = sistema_app_encoberto() ? 0 : s_avisos.exchange(0, std::memory_order_relaxed);
        if (av) {
          if (app_vivo()) {
            const uint32_t dele = av & s_app->avisos;
            if (dele) {
              if (s_app->ao_aviso) { s_app->ao_aviso(dele); }
              else                 { s_sujo_dados = true; }
            }
          } else if (!s_app) {
            atualizar_linha_servicos();
          }
        }

        // SCREEN ROTATION (kernel/orientacao): the IMU for apps that follow it, and
        // the physical keyboard docked/removed for everyone (home included). Rotates
        // BETWEEN frames, here, before drawing: the full pass that comes
        // right below already goes out in the new orientation. Never with a finger on the screen (nor
        // right after one): a drag would become a touch somewhere else, and the
        // release would land on a button that was not there when the finger went down.
        ETAPA("orientacao (IMU, I2C)");
        // On home too: the physical keyboard lays home down (orientacao.hpp).
        const bool pode_girar = (app_vivo() || !s_app) && !s_bloqueado && !s_painel_avisos
                             && !tocando && !s_dedo_na_tela && (t - s_dedo_ms) > 300;
        if (orientacao_vigiar(t, pode_girar)) {
          rolagem_zerar(&s_rol);            // the scroll was measured at the other height
          tela_virtual_invalidar_tudo();
          if (app_vivo() && s_app->ao_girar) { s_app->ao_girar(); }
          s_sujo_layout = true;             // bar, navigation and app: everything again
        }

        ETAPA("desenho (app / home)");
        const int64_t desenho_t0 = esp_timer_get_time();
        const bool desenho_completo = s_sujo_layout;
        comp_marcar_desenho();
        uint32_t fase_barra = 0, fase_conteudo = 0;
        if (s_sujo_layout) {
          const int64_t f0 = esp_timer_get_time();
          desenhar_barra_status(tela_virtual(), true);
          const int64_t f1 = esp_timer_get_time();
          desenhar_conteudo(true, true);
          fase_barra    = (uint32_t)(f1 - f0);
          fase_conteudo = (uint32_t)(esp_timer_get_time() - f1);
          // The visible page now reflects exactly s_rol.atual - any
          // scroll pending from before this full redraw has gone out with it.
          rolagem_sincronizar(&s_rol);
        } else if (rolagem_pendente(&s_rol)) {
          // SCROLLING: the position changed, the VALUE of nothing changed. Instead of the
          // path above (clear ~1.65MB + swap pages), we shift
          // the ALREADY CORRECT pixels on the screen (tab5_tela_rolar - memcpy
          // in strips, clearing nothing) and only then call the app to
          // draw on top - skipping the clearing burst and the page
          // swap, but NOT the "app redraws everything".
          //
          // completo_para_apps=true (even without limpar_area) is intentional:
          // the strip the shift has just exposed was NEVER painted
          // AT THIS scroll position before - if each app only looked at the
          // per-field dirty-tracking (pintura_mudou_i/f/s), static
          // structure that only draws when "it is a full pass" (title,
          // help, button - see app_config) would never fill that strip.
          // It was exactly the bug seen live: scrolling down looked fine
          // (the new strip is thin, it went unnoticed), scrolling back up
          // exposed at once the trail of never-painted strips.
          const area_t a = area_util();
          const rolagem_passada_t p = rolagem_deslocar(&s_rol, a, TFT_BLACK);
          if (p.deslocou) {
            desenhar_conteudo(false, true, &p.exposta);
          } else {
            // Drag too fast for the interval between calls (delta
            // larger than the usable area itself): there is nothing to reuse, fall
            // back to the normal full redraw on the next cycle.
            s_sujo_layout = true;
          }
        } else if (s_sujo_dados && !s_sujo_layout) {
          desenhar_conteudo(false, false);
        }

        // The bar is only repainted when something IN IT changed - the percentage, the
        // charge state or the app. Repainting every two seconds for nothing was
        // another source of flicker, just in a smaller strip.
        if ((t - s_ultima_barra) >= BARRA_MS || recado_pendente()) {
          s_ultima_barra = t;
          if (barra_mudou()) {
            tela_virtual().startWrite();
            desenhar_barra_status(tela_virtual(), false);
            tela_virtual().endWrite();
          }

          // Notification beep + turning off idle I2S - see the big note
          // in atualizar_beep_e_ociosidade_do_som(), further up the file.
          atualizar_beep_e_ociosidade_do_som(t);

          // The home services line, on the same tick and by the same rule -
          // see atualizar_linha_servicos() (kernel/shell.cpp) for the "only
          // redraw if the text changed".
          atualizar_linha_servicos();
        }
        if (comp_tem_sujeira()) {
          const uint32_t us = (uint32_t)(esp_timer_get_time() - desenho_t0);
          const uint32_t cpu = comp_contar_desenho(us, desenho_completo);
          // WHO WEIGHS. The full pass averaged 90-130 ms - the average does not say
          // whose. The per-app (and per-phase) table comes out in the console's 'tela'.
          if (desenho_completo && fase_conteudo) {
            const uint32_t limpar = s_fase_limpar_us;
            anotar_passada(s_bloqueado ? "bloqueio" : (s_app ? s_app->nome : "home"), cpu,
                           fase_barra, limpar, fase_conteudo > limpar ? fase_conteudo - limpar : 0);
          }
        }
      }
    }

    // Adaptive period: fast during use, more spaced out when still.
    //
    // The ceiling is "how long until the next REAL deadline of this loop" - it does not
    // invent any deadline, it just stops waking needlessly between them. The usual
    // deadlines (sensors and bar) go in here, once, for both states.
    uint32_t prazo = UI_ESPERA_MAX_MS;
    auto menor = [&](uint32_t intervalo, uint32_t desde) {
      const uint32_t dec = t - desde;
      const uint32_t falta = (dec < intervalo) ? (intervalo - dec) : 0;
      if (falta < prazo) { prazo = falta; }
    };
    menor(MEDIDA_MS(), s_ultima_medida);   // sensors
    menor(BARRA_RMS(), s_ultima_barra);    // status bar (when idle, the notice beep)
    uint32_t periodo;
    if (tab5_power_em_repouso()) {
      periodo = s_toque_isr_ok ? prazo : PERIODO_ZZZ;   // without the touch ISR, the usual fixed pace
    }
    else {
      // AWAKE: FAST ONLY WHEN THERE IS A REASON. Until now the loop
      // ran every 20-25ms whenever the screen was on - 40 to 50
      // turns per second, each reading touch over I2C, with the device
      // sitting on the table. Now:
      //   - finger on the screen (or within the last 500ms) or pending work: 20ms,
      //     with the touch interrupt OFF (during a drag it would pulse
      //     every 5-20ms and only get in the way);
      //   - still: the interrupt goes ON and the loop sleeps until the next
      //     real deadline. A touch, a service notice or a redraw request from
      //     another task wakes it immediately (the wait on the notification, further down).
      const bool dedo = (t - s_ultimo_toque) < 500;
      const bool pendente = s_sujo_layout || s_sujo_dados || rolagem_pendente(&s_rol)
                         || comp_tem_sujeira() || s_app_carregando || s_sd_montando;
      if (dedo || pendente || !s_toque_isr_ok) {
        s_isr_acorda = false;
        periodo = PERIODO_UI;
      } else {
        if (prazo > 1000) { prazo = 1000; }
        menor(ocioso_ms(), s_ultimo_toque);              // time to turn the screen off
        // (the app clock goes in below, after the PERIODO_UI floor)
        if (s_speaker_ocioso_desde) { menor(SPEAKER_OCIOSO_MS, s_speaker_ocioso_desde); }
        if (tab5_teclado_presente()) { menor(teclado_ms(), s_ultimo_teclado); }   // the physical keyboard is polled
        { const uint32_t falta = orientacao_prazo_ms(t); if (falta < prazo) { prazo = falta; } }   // rotation watcher (only with an app that follows the IMU)
        if (s_nav_travada && prazo > TOQUE_FRESCO_MS) { prazo = TOQUE_FRESCO_MS; }   // keyboard: touch always fresh
        if (prazo < PERIODO_UI) { prazo = PERIODO_UI; }
        s_isr_acorda = s_toque_isr_ok;
        periodo = prazo;
      }
      // THE APP CLOCK breaks through the PERIODO_UI floor: the next frame lands
      // right after a scan, and that moment may be less than
      // 20 ms away (and with the finger on the screen too - dragging must not stall an
      // animation).
      if (periodo_do_app()) {
        uint32_t falta = ritmo_falta_ms();
        if (falta < 1) { falta = 1; }
        if (falta < periodo) { periodo = falta; }
      }
    }

    // NEVER BEYOND THE WATCHDOG: the wait plus the turn fit within its deadline, with
    // slack for light sleep waking late. A new pace (sistema_ritmo_ms)
    // that exceeds it is cut here, and does not turn into a restart.
    if (periodo > UI_ESPERA_MAX_MS) { periodo = UI_ESPERA_MAX_MS; }

    // ONE FRAME PER TURN (docs/COMPOSITOR.md, rule 4). Everything this turn
    // drew is already on the back page (direct mode), and the tracker noted
    // where. Here the swap is requested - without waiting: what waits is the next
    // frame, when it opens. A turn without drawing = nothing to do.
    bool apresentou = false;
    if (!tab5_power_em_repouso() && comp_tem_sujeira()) {
      ETAPA("compositor (page swap)");
      apresentou = comp_apresentar();
      if (apresentou) { comp_contar_volta((uint32_t)(esp_timer_get_time() - volta_t0)); }
    }

    // PROOF OF LIFE. A full loop turn finished - the screen was handled,
    // the apps had their turn, nothing hung. It is the signal the watchdog
    // subscribed up top expects; without it on every turn, the 10 s deadline expires
    // and the device restarts even while working fine.
    esp_task_wdt_reset();
    ++s_voltas;   // single writer: this task
    s_volta_ms = tab5_agora_ms();
    ETAPA("esperando evento");

    // The wait on the notification (ulTaskNotifyTake) in place of the usual vTaskDelay() - wakes at the SAME
    // deadline calculated above OR earlier, if something arrives in the queue (a real
    // touch when idle, or sistema_acordar_interface() from a service). The
    // CONTENT and the reason for returning are ignored on purpose: the
    // first lines of the NEXT cycle already check M5.Touch and all the
    // flags by themselves, exactly as they always have.
    if (apresentou && periodo == PERIODO_UI) {
      // AT THE PANEL'S PACE. With the finger on the screen (or pending work) the
      // turn slept 20 ms AFTER finishing: real period = 20 ms + the
      // drawing, ~30 fps against a ~63 Hz panel. The two paces do not
      // match, and every so often a frame stays on screen for two periods -
      // scrolling judders even without any tearing.
      //
      // Here the turn that presented waits for the SWAP (the panel started scanning
      // the new frame) and restarts immediately: touch read right after the swap,
      // new frame ready for the next one. The wait is not extra - the next
      // frame would have had to wait for it anyway (comp_quadro_imagem,
      // step 1). Applies ONLY in fast mode: when still, the UI keeps
      // sleeping until the next deadline, as always.
      comp_esperar_troca();
      ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, 0);   // consumes a notice that has already arrived
    } else {
      ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, pdMS_TO_TICKS(periodo));
    }
    // On every turn, not on the 2 s step: the flash drive waits for this answer
    // to turn on USB (see sistema_som_ceder_pinos).
    som_atender_reserva();
    som_atender_fluxo();
  }
}

void sistema_iniciar(void)
{
  //                      body            name  stack param prio handle kernel
  // ajustes_iniciar() was already called in app_main, BEFORE M5.begin(), so the
  // screen is born at the right brightness instead of flashing the default and correcting later.
  pool_iniciar();         // fixed tasks for services that turn on/off and for apps - BEFORE everything
  tab5_mem_marco("slot pool");
  tab5_sd_iniciar();      // creates the card mutex BEFORE there is concurrency
  tab5_mem_marco("SD card (task)");
  tab5_log_iniciar();     // AFTER the SD (uses tab5_sd_montar/abrir) - keeps the logs on the card
  tab5_mem_marco("log (task+buffer)");
  idioma_iniciar();       // the .po files (built-in and from the card) - before the first UI text
  tab5_mem_marco("languages");
  tab5_monitor_iniciar(); // measures CPU/memory all the time and records events (services/tab5_monitor)
  tab5_mem_marco("monitor (task)");
  tab5_wifi_iniciar();    // creates the queue and the task; does NOT turn on the radio
  tab5_mem_marco("wifi (task, no radio)");
  tab5_espnow_iniciar();  // waits for our firmware on the C6 (peer_data) and turns on ESP-NOW
  tab5_mem_marco("espnow (task)");
  tab5_ble_iniciar();     // creates the task; does NOT turn on (see the UI loop)
  tab5_mem_marco("ble (task, no radio)");
  console_iniciar();      // commands over the serial monitor (help)
  tab5_mem_marco("console (task)");
  sistema_apps_de_fora_iniciar();   // the apps on the card (kernel/app.h) - BEFORE home builds the order
  tab5_mem_marco("external apps");

  // TOUCH ISR - see the big note next to UI_AVISO_IDX,
  // further up the file. It must exist BEFORE the UI task
  // comes up (it consumes the queue from the first loop turn) and AFTER
  // M5.begin() (main.cpp) - that is what configures GPIO 23 as an input for
  // the GT911 driver; this ISR only ADDS an interrupt type to that
  // pin, never changes direction/pull.
  {
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
      // INVALID_STATE = another module already installed the global ISR service
      // before we got here (the most likely candidate is the bring-up of
      // WiFi/C6 by esp_hosted, which does the same in eh_host_port_gpio.c) -
      // normal and safe, we just need SOMEONE to have installed it, no matter
      // who.
      // On the GT911 the driver itself already left the pin as an input. On TDDI
      // nobody configured it: input with pull-up (open-drain interrupt line,
      // active low - with no finger it idles high).
      if (!tab5_painel_toque_gt911()) {
        gpio_set_direction(GPIO_NUM_23, GPIO_MODE_INPUT);
        gpio_set_pull_mode(GPIO_NUM_23, GPIO_PULLUP_ONLY);
      }
      if (gpio_set_intr_type(GPIO_NUM_23, GPIO_INTR_ANYEDGE) == ESP_OK &&
          gpio_isr_handler_add(GPIO_NUM_23, isr_toque, nullptr) == ESP_OK) {
        // ON PERMANENTLY: it notes every pulse (s_toque_pulso). What decides whether
        // it also wakes the loop is s_isr_acorda (idle / still screen).
        s_toque_isr_instalada = true;
        s_toque_isr_ok = tab5_painel_toque_gt911();
      }
    }
  }
  if (!s_toque_isr_instalada) {
    ESP_LOGW(TAG, "touch ISR did not come up - idle uses the usual fixed PERIODO_ZZZ");
  } else if (!s_toque_isr_ok) {
    ESP_LOGI(TAG, "touch on screen %s: interrupt only after proving the pin (validar_isr_tddi)",
             tab5_painel_nome());
  }
  // KEYBOARD ISR (see isr_teclado). G50 belongs to nobody else: input with
  // pull-up, falling edge. If it does not come up, the keyboard still wakes the screen -
  // only at the next idle deadline, by polling the queue (up to 5 s with the
  // touch ISR ok - sistema_ritmo_ms -, or 80 ms without it).
  {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << TAB5_TECLADO_INT;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    c.intr_type = GPIO_INTR_NEGEDGE;
    s_tecla_isr_ok = gpio_config(&c) == ESP_OK
                  && gpio_isr_handler_add((gpio_num_t)TAB5_TECLADO_INT, isr_teclado, nullptr) == ESP_OK;
    if (!s_tecla_isr_ok) { ESP_LOGW(TAG, "keyboard ISR (G%d) did not come up - it wakes the screen via the 1 s poll", TAB5_TECLADO_INT); }
  }

  // Handle CAPTURED, not NULL - sistema_camera_iniciar()/parar() (see the
  // big note in app.h) need to know whether their caller ALREADY IS this
  // task, so as not to bridge to themselves (deadlock).
  home_ordem_carregar();   // the user's icon order (NVS), before the first draw
  // FIRST USE: the welcome wizard opens by itself (kernel/boas_vindas).
  // Only on a device that has never run the system - see ajustes_config_inicial().
  if (!ajustes_config_inicial()) {
    extern const app_t APP_BOAS_VINDAS;
    sistema_simular_abrir(&APP_BOAS_VINDAS);
  }
  // Boot is also "the screen turned on" - the same rule as waking. Here, before
  // the UI task exists: the first draw already comes out locked.
  bloqueio_ao_ligar_tela();
  s_relogio_feito = xSemaphoreCreateBinary();   // before the UI: it gives, the console waits
  s_som_parou     = xSemaphoreCreateBinary();   // likewise: it gives, the flash drive waits
  s_fluxo_pronto  = xSemaphoreCreateBinary();   // likewise: it gives, the music background waits
  som_configurar();   // before any M5.Speaker.begin()
  xTaskCreatePinnedToCore(vTaskInterface, "ui", 10240, NULL,   4,  &s_tarefa_interface, 1);
  registro_iniciar();
  tab5_mem_marco("ui + log (tasks)");
  ESP_LOGI(TAG, "UI up, %d app(s) registered", SISTEMA_NUM_APPS);
}


// See ETAPA at the top of the file. Read from another task, without a lock: loose
// words, and a value one step behind does not change the diagnosis.
void sistema_diag_interface(sistema_diag_t* d)
{
  const uint32_t agora = tab5_agora_ms();
  d->etapa          = (const char*)s_etapa;
  d->voltas         = s_voltas;
  d->ms_desde_volta = s_volta_ms ? agora - s_volta_ms : 0;
  d->ms_desde_dedo  = s_dedo_ms ? agora - s_dedo_ms : UINT32_MAX;
  d->toque_pulsos   = s_pulsos;
  d->toque_falhas   = s_vig_falhas;
  d->toque_presos   = s_vig_presos;
  d->toque_surdos   = s_vig_surdos;
  d->toque_reinicios = s_vig_reinicios;
  d->toque_status   = s_vig_status;
  d->repouso        = tab5_power_em_repouso();
  d->app            = s_app ? s_app->nome : nullptr;
}
