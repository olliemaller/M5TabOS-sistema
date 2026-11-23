# M5tabOS — kernel and shell

Architecture document. It describes what **exists today**, what is **missing
before it can be called a kernel**, and a plan in stages. Each stage is
independent: you can stop at any of them and the system keeps standing.

Written after measuring. Where there is a number, it came from the code or from
`sdkconfig`, with the file and line. Where I did not measure, it says so.

---

## 1. Where we are

The system already has a kernel — it just is not gathered in one place.

**Already kernel, and in good shape:**

| piece | file | what it does |
|---|---|---|
| app life cycle | `tarefa_app.cpp` (84 l.) | creates and destroys each app's task |
| power | `tab5_power.cpp` (1248 l.) | profiles, INA226, idle, clock ceiling |
| notifications | `notificacao.cpp` | system service for apps and services: idempotent registration by name, state per source, 4 levels, a single highlight in the status bar - see ESCREVENDO_APPS.md §8.2 |
| preferences | `ajustes.cpp` (183 l.) | persistence in NVS (brightness, volume, wifi, grove, screen, clock, home) |
| background parts | `kernel/fundo.cpp` | app work that outlives the window: task, pill, summed profile, clean stop (1st client: Trilha, `app_trilha/trilha_fundo.cpp`) |
| persistent log | `tab5_log.cpp` (183 l.) | vprintf hook, buffer, daily flush to the SD card |
| music player | `services/tab5_musica.cpp` | service: folder, queue, MP3 (`minimp3` component)/WAV, streamed sound, spectrum. The Music app and the Berry module `musica` are just interface; it runs as a background part ("Música" - Music) and publishes to `kernel/midia` |
| disks | `services/tab5_volumes.cpp` | the SD card (`/sdcard`) and the USB flash drive (`/usb`) by absolute path: mount/unmount, available, `AVISO_VOLUMES` when one is inserted or removed |
| USB-A | `services/tab5_usb.cpp` | high-speed host + `usb_host_msc`: FAT flash drive at `/usb`, safe removal, notice in the status bar, light sleep held off while a device is plugged in |
| pendrive (SD card on the computer) | `services/tab5_pendrive.cpp` + the `tab5_sd_cru_*` ports of `hal/tab5_sd` | the service does not touch `sdmmc_*`: it only uses the sector ports of the lent card. Contiguous writes from the computer are gathered in a 64 KB buffer (flushed on: full, non-contiguous, READ10, SYNCHRONIZE CACHE, TUR, eject, 30 ms idle, end) and reads come from a 64 KB cache. Card failure: the HAL resets the SDMMC controller (`sdmmc_host_reset`, which the IDF driver does not do after a timeout, and which also used to take down the C6's Wi-Fi, slot 1), waits for the card and retries (up to 2 times); if it fails for good the session ends by itself and notifies |
| paint budget | `pintura.cpp` | limits repainting per frame |
| lazy lock | `tab5_mutex.hpp` | mutex created on demand - used by 4 modules that used to rewrite the same logic |
| drivers | `tab5_tela`, `tab5_sd`, `tab5_grove`, `tab5_teclado`, `tab5_camera` | one owner per peripheral |

**STAGE 1 DONE.** `sistema.cpp` reached 2457 lines doing
eight different things (schedules drawing, routes touch, draws the home screen,
draws the status bar, manages the app life cycle, applies power
policy, corrects the touch calibration, and — since earlier the
same day — marshals HAL calls between tasks for the camera) and
was split in two, **without changing a single line of logic** (the only
exception, documented in `kernel/internal.hpp`, is one call swapped for its
public equivalent):

| file | lines | responsibilities | items above |
|---|---|---|---|
| `kernel/core.cpp` | 1486 | the engine - state, touch and keyboard ISR, wake from light sleep, interface task, `sistema_iniciar` | 1, 5, 6, 8 |
| `kernel/core_toque.cpp` | 218 | GT911 watchdog and magnifier, continuous reading, sample fingers, test bench (simulate touch/gesture/open) | 1 |
| `kernel/core_radios.cpp` | 151 | Bluetooth, Mesh over the internet, Wi-Fi on/off, Grove and the Mesh path | 6 |
| `kernel/core_apps.cpp` | 307 | open/close app, SD mount in its own task, refused app | 1 |
| `kernel/core_energia.cpp` | 207 | screen kept on by request, power-saving contract, panel clock, profile | 6 |
| `kernel/core_som.cpp` | 315 | speaker idleness, USB pins, streamed sound, beep | 8 |
| `kernel/core_desenho.cpp` | 294 | usable area, measured passes, content drawing pass | 1 |
| `kernel/core_interno.hpp` | 98 | state and functions shared by the `core_*.cpp` files | — |
| `kernel/shell.cpp` | 280 | the look - geometry and touch correction, gestures, splash, touch routing | 2, 3, 4, 7 |
| `kernel/shell_barra.cpp` | 1027 | battery icon, status bar, time, bottom strip of the home screen, navigation button | 2, 3 |
| `kernel/shell_home.cpp` | 468 | home pages, icon order, move/remove icon, drawing the home screen | 2 |
| `kernel/shell_bloqueio.cpp` | 257 | lock screen | 4 |
| `kernel/shell_avisos.cpp` | 544 | notification centre and mini player | 7 |
| `kernel/shell_interno.hpp` | 57 | geometry, state and functions shared by the `shell_*.cpp` files | — |
| `kernel/internal.hpp` | 78 | the private bridge between the two - never included by an app | — |

`sistema.cpp` was renamed to `sistema.cpp.old` until the behaviour was
confirmed on hardware - once confirmed over several test sessions, the file was
removed for good. The ~24 references to "sistema.cpp" that
remained in comments in other files (pointing at code that now
lives in `core.cpp`/`shell.cpp`/`internal.hpp`) were fixed the same day -
the few that still mention "sistema.cpp" are genuine history (they describe
the file as it was BEFORE Stage 1), not forgotten pointers.

Item 8 (the camera bridge) stayed inside `core.cpp` because it is life
cycle/HAL, not drawing. The question that had been left open - "if a sound
needs to play from a background task, does the bridge pattern repeat?" - now
has an answer: **no.** Sound turned up as the third case
(while investigating why the `i2s_driver` PM lock was never released, see 2.2.1)
and called for a completely different shape from the bridge - see 1.1, below.
Only the two camera-specific copies still exist; the lesson was not "we should
have generalised the bridge", it was "not every shared resource wants to be
shared the same way".

### 1.1 Resources that sleep on their own — three shapes, not one

Catalogued while deciding what to do with the speaker. This is not
a new abstraction to build - it is the name for what **already exists**,
scattered through the code, and the right question before writing any new
background service: which of the three (or none) does this resource need?

| pattern | who uses it today | shape of use |
|---|---|---|
| **idle timeout** | speaker (`kernel/core_som.cpp`, `s_speaker_ocioso_desde`) | short bursts, nobody holds the resource open - it turns on by itself on first use, turns off by itself after a few quiet seconds |
| **loan count** | SD card (`tab5_sd_montar()`/`tab5_sd_desmontar()`, reference counted) | explicit session, several ask at the same time, only released when the last one gives it back |
| **single owner** | Grove (`tab5_grove_dono()`), camera bridge | only one consumer at a time makes sense, ownership passes from one to another explicitly |

There is a fourth answer, as valid as the three: **do nothing.** The
IMU (`tab5_imu_medir()`, 1×/s, awake or idle - `kernel/core.cpp`) deliberately
follows none of the three: one I²C transaction per second is cheap
enough not to be worth tracking "who is using it now". Generalising
too early would have forced that same complexity onto a place that does not
need it.

When Stage 2 brings real background services, the question
"which of these four" becomes part of each one's design - instead of inventing
a generic mechanism without a real fourth case to validate the shape.

---

## 2. What is missing before it is a kernel

### 2.1 There is no event queue — and that is what holds back battery life

Today **everything is polled**. The services raise flags (`s_sujo_dados`,
`s_msgs_sujo`, counters) and the interface reads those flags every so often:

```
PERIODO_UI    = 20 ms   sistema.cpp:48    in use
PERIODO_OCIOSO = 50 ms  sistema.cpp:58    3 s without touch
PERIODO_ZZZ   = 80 ms   sistema.cpp:75    screen off
```

Look at the last line. **With the screen off, the interface still wakes 12.5
times a second** to ask whether anything has happened. In the overwhelming
majority of cases, nothing has.

A task that wakes 12 times a second stops the processor from really
sleeping. It does not matter how short the work is: the cost is the *waking*.

The right shape is the inverse: the interface **blocks on a queue** and someone
wakes it when there is a reason.

> I checked one thing before writing this, because it would change the
> diagnosis: the touch controller has an interrupt pin, **GPIO 23**, and M5GFX
> already uses it to skip the I²C read when there is no finger
> (`Touch_GT911.cpp:174`). So idle polling today is cheaper than it looked.
> The waste is not in the I²C — it is in the waking. But it also means the pin
> is already configured and stage 2 below has something to build on.

### 2.2 The power-saving machinery exists and **is switched off**

This is the finding that matters most:

```
sdkconfig:2951        # CONFIG_FREERTOS_USE_TICKLESS_IDLE is not set
sdkconfig.defaults:88 # CONFIG_FREERTOS_USE_TICKLESS_IDLE=y     ← commented out
```

Without *tickless idle*, the FreeRTOS tick at 1000 Hz (`CONFIG_FREERTOS_HZ=1000`)
wakes both cores **a thousand times a second, forever**. And as a
consequence:

- `esp_pm_configure()` receives `light_sleep_enable` (`tab5_power.cpp:84`), but
  it **has no effect at all** — there is no idle window long enough.
- No profile ever asks for it: `light_sleep_ocioso` is `false` by default
  (`tab5_power.hpp:109`) and **nobody assigns another value**.

In plain terms: the light sleep support in `tab5_power` is **dead code today**.
We wrote the part and never flipped the switch.

### 2.2.1 The PM locks — and why the news is better than it looks

I went to check who, besides the tick, stops the processor from sleeping. In
the IDF all power saving goes through **PM locks**: a driver that needs a stable
clock creates a lock at initialisation and holds it. While any lock is alive,
the system does not drop into the mode it forbids — with no warning at all.

The MIPI-DSI panel holds **two**, and both are acquired at creation and
released only in the destructor (`esp_lcd_panel_dpi.c` 291→405,
`esp_lcd_mipi_dsi_bus.c` 72→151):

```
dsi_dpi   CPU_FREQ_MAX     pins the CPU at 360 MHz
dsi_phy   NO_LIGHT_SLEEP   forbids light sleep
```

This would be fatal — with the panel alive, neither tickless idle nor `esp_pm`
would change anything. **But it is already solved**: `DESMONTAR_DSI_NO_REPOUSO = true`
(`tab5_power.cpp`) destroys the panel when entering idle, and both locks go
with it. This was measured with `esp_pm_dump_locks()`, after
two cheaper attempts had been tested and rejected (`set_pattern` and
lowering the clock with the stream running — both are documented in the source).

In other words: **the hardest obstacle of stage 3 was knocked down months ago.**
Stage 3 is more promising than the section above makes it look.

**The remaining suspect** is the UART. The driver creates `ESP_PM_APB_FREQ_MAX`
while it is installed (`uart.c:1922`), and APB_FREQ_MAX **also** prevents light
sleep, because sleeping would stop the APB. The Meshtastic Grove keeps a UART
installed all the time.

That is the real tension of stage 3, and it is not technical, it is a design
one: **you cannot sleep and listen to the radio at the same time.** Either
Meshtastic loses messages while the tablet sleeps, or the tablet does not sleep.
There is a middle ground in hardware (waking on the UART start bit), but it is
an open design decision.

**Update — one new lock, found and fixed; another,
explained.**

Running `power locks 40` (`energia travas 40`) with the screen genuinely off
(confirmed by the log: `idle after 30 s`, `DSI torn down`, clock
`360 -> 40 MHz`) revealed a lock the list above did not predict: `i2s_driver`
(`APB_FREQ_MAX`), held from the first `tone()`/`playWav()` of any session,
**forever** - whether sound is playing or not. Cause: on the ESP32-P4 (external
ES8388 codec over "real" I2S, without the internal DAC of the classic ESP32
family), the M5Unified library only calls `i2s_stop()` in the idle branch of
`spk_task` inside an `#if M5UNIFIED_I2S_ADC_DAC` that is never compiled for this
chip (`managed_components/m5stack__m5unified/src/utility/Speaker_Class.cpp:615`).
Patching a vendored component is not worth it - it is lost at the next
update. The fix stayed on our side, through the public API:
`M5.Speaker.end()` called by an idle watchdog in `kernel/core_som.cpp`
(the first real use of the "idle timeout" pattern from section 1.1). Confirmed
with `power locks 40` again: the lock no longer appears in the list once the
idle timeout has passed.

The `rtos0` lock (`CPU_FREQ_MAX`, core 0) stays held even after that
fix - and that one is expected, not a bug. It is `esp_pm`'s own internal
lock (`s_rtos_lock_handle`, `pm_impl.c`), which is only released when that
core's IDLE task runs, and is retaken the instant any interrupt fires -
in other words, it is the thermometer of how truly idle core 0 is. With `log`
using 10.3% of CPU there (measured with `tasks`) plus the `esp_timer` of
`registro.cpp` waking every ~15 s, core 0 is never still for
long enough stretches. It is the same polling described in 2.1, now seen
from the lock's side instead of the wait period - it confirms, rather than
replaces, the reason Stage 2 comes before Stage 3.

### 2.3 Priorities and cores were decided one at a time

```
ui         prio 4   stack 10240   core 1       sistema.cpp:2455   pinned (the only pinned one)
gps        prio 3   stack 4096    any          tab5_gps.cpp:265
mesh       prio 3   stack 6144    any          tab5_mesh.cpp:1414
wifi       prio 3   stack 4096    any          tab5_wifi.cpp:550
cam        prio 3   stack 8192    any          tab5_camera.cpp:406   only while the Camera app is open
trilha     prio 2   stack 4096    any          tab5_trilha.cpp:197   service, survives the app closing
NMEAParse  prio 2   stack 4096    any          app_azimutal.cpp:367  only with Azimutal open
console    prio 1   stack 12288   any          console.cpp:2196
registro   prio 1   stack 4096    any          registro.cpp:271
log        prio 1   stack 6144    any          tab5_log.cpp:179
tarefas    prio 1   stack 4096    any          app_tarefas.cpp:240   only with the Tasks app open
sd_monta/
desmonta   prio 1   stack 4096    any          sistema.cpp:1361/1427/1505   short, once per app open
grove_sonda prio 1  stack 4096    any          sistema.cpp:1672      short, a single probe
```

Updated - the first version of the list had 5 lines; today it has 12. Every
new service (trilha, log, camera) slotted into the priorities already
established (3 for real-time radio/sensor, 2 for unhurried background work, 1
for kernel plumbing) without needing to invent a new band - the original
scheme withstood the growth.

By priority it is consistent: the interface (4) pre-empts the services (3), so
no service interrupts a frame halfway through. That is right.

What was **not** decided, was inherited: `tskNO_AFFINITY` on every service.
They float between the cores. Pinning them to core 0 would leave core 1 just
for the interface — which helps if we ever want to switch off a whole core
when idle.

**I will not do this now.** The gain is plausible and has not been measured, and
optimising without measuring is exactly the mistake that has already cost us
time here.

---

## 3. The measuring instrument already exists

Before touching anything, it is worth measuring — and we can measure today:

```
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y
```

The `tasks` command (`tarefas`) and the Tasks app already show **real CPU% per
task**, from the delta of `ulRunTimeCounter` — it is not an estimate.

There are **two** measurements, and they answer different questions.

**1. How much there is to gain** — let the screen turn off and run `tasks`. Look
at the **IDLE** line: that percentage is the ceiling of what light sleep can
save. If it is at 97%, there is a lot to gain. If it is at 60%, someone is
working in the dark and the fix is something else — find out who.

**2. Whether the way is clear** — `power locks 30` (`energia travas 30`, a new
command). It waits 30 s for the screen to turn off and only then dumps the PM
lock table with `esp_pm_dump_locks()`. Do not touch the screen while it waits.

What to read in the dump:

- **empty, or only released locks** → the way is clear, stage 3 is worth a try;
- **`dsi_dpi` or `dsi_phy` held** → the screen teardown did not happen, and
  that is what has to be investigated before anything else;
- **`uart_driver` held** → it is the Meshtastic Grove, and then the design
  decision in section 2.2.1 comes before the code.

The second measurement is the one that matters more. The first says how much
would be gained; the second says whether it can be gained at all.

---

## 4. The plan, in stages

### Stage 1 — a boundary without changing behaviour ✅ DONE

Split `sistema.cpp` in two, **without altering a single line of logic**:

```
kernel/core.cpp    drawing schedule, app life cycle, power policy
kernel/shell.cpp   home, status bar, navigation, touch routing, calibration
```

(Later, each of the two was split by subject, again without changing logic:
`core_*.cpp` with `core_interno.hpp` and `shell_*.cpp` with `shell_interno.hpp`,
in `kernel/`; the Settings app went to `app_config.cpp` +
`app_config_*.cpp` + `app_config_interno.hpp`, in `app_config/`. The table
above gives the line count of each kernel file.)

(Names in English, the standard OS terms: `core.cpp` is the heart of the
kernel, and "shell" is the visual
layer around the apps - see the big warning in `kernel/internal.hpp` for
the distinction from the command-line shell in `console.cpp`.)

Gain: zero in performance, a lot in being able to see - confirmed, the
final binary ended up ~96 bytes different from the previous one (only the extra
indirection of a few cross calls). A 2457-line file with eight
responsibilities and shared statics is where bugs live. The seam
between the two is `kernel/internal.hpp`: a few `extern` variables and some
functions that cross from one side to the other, never exposed to an app.

### Stage 2 — the event queue (this is where the gain is)

**FIRST SLICE DONE.** A single queue, created in
`sistema_iniciar()` and consumed at the end of `vTaskInterface()`:

```c
typedef enum {
  EV_TOQUE,     // from the GPIO 23 ISR - just "wake up and check M5.Touch again"
  EV_SERVICO,   // sistema_acordar_interface() - someone published something new
} evento_t;
```

The interface loop stopped being `vTaskDelay(periodo)` and became
`xQueueReceive(fila, &ev, periodo)` - the content/reason of the return is
deliberately ignored in this slice: the very next lines of the loop already
check `M5.Touch` and all the flags by themselves, exactly as they always
have. **No existing decision logic changed** - only the way of
waiting.

What changes in practice, measured and not just expected:

- **In use and idle with the screen on:** behaviour IDENTICAL to before -
  same `PERIODO_UI`/`PERIODO_OCIOSO`, just swapping `vTaskDelay` for
  `xQueueReceive` with the same value. The touch ISR stays **disabled** in those
  two states (see risk 2 below) - zero observable change.
- **Screen off (idle):** the ceiling stopped being a fixed `PERIODO_ZZZ=80ms` and
  became "how long until the next real deadline the loop already honoured" -
  the smallest of sensor sampling (1000ms), status bar/beep (`BARRA_MS`)
  and the Grove watchdog (`VIGIA_MS`, only when it actually advances). In
  practice the loop now wakes by itself about **1x/s instead of 12.5x/s** -
  a real ISR on the touch pin (GPIO 23, which had none until
  now - just a level read synchronously, see `Touch_GT911.cpp:174`)
  wakes the queue at the right moment if a finger touches before then.
- **A service publishing while the device sleeps:** `notificacao.cpp`
  became the first active producer (`sistema_acordar_interface()`, called
  from inside `notif_publicar()` only when something really changes - the same
  guard that already existed for `notif_versao()`). Mesh and Trilha get this for
  free, without knowing the queue exists: a message arriving with the screen
  off no longer waits for the next ~1 s deadline for the beep to sound.

**Left out of this slice, on purpose:**

- **Mesh/GPS/Trilha do not touch the queue directly.** They do not need to - all
  three already notify via `notif_publicar()`, and get the benefit above without
  changing a line of their own. Converting them directly would multiply the
  files touched with no additional gain.
- **`PERIODO_OCIOSO` (awake, no touch for 3 s) stays fixed at 25ms**,
  it did not become deadline-adaptive. It could be done with the same deadline
  arithmetic already written, but it would also require enabling/disabling the
  touch interrupt exactly at the boundaries of a drag in progress - the GT911
  pulses the pin every 5-20ms while a finger is on the screen, and the ISR
  enabled in that state would keep the queue from ever emptying, breaking the
  very rate limit that `PERIODO_UI`/`PERIODO_OCIOSO` exist to enforce. The gain
  there (40Hz→~16Hz) is small next to idle (12.5Hz→~1Hz) and was not worth the
  extra complexity for now.

**Two risks found while reviewing the design, before writing the first
line** (a planning agent validated the design and found both - neither is
hypothetical):

1. *Loop spinning without stopping.* The Grove watchdog deadline only advances
   when `ajustes_grove() && !s_grove_resondar`. Without that guard in the
   minimum calculation, Grove switched off (the normal configuration) or a
   re-probe in progress would leave that deadline stuck at zero forever - the
   queue would never block, and the core would spin at ~100% in exactly the
   state that should be sleeping. Fixed: the Grove deadline only enters the
   calculation when both conditions hold; otherwise it is excluded from the
   `min()`, not treated as "already due".
2. *The ISR cannot feed the same queue in every state.* See the
   "PERIODO_OCIOSO" item above - the interrupt starts disabled and is only
   enabled (`gpio_intr_enable`) the instant `tab5_power_repouso()`
   succeeds, and disabled (`gpio_intr_disable`) the instant
   `tab5_power_retomar()` is called.

**Still needed to call "Stage 2" complete** (the next slice, not decided
yet): convert Mesh/GPS/Trilha into direct producers if they ever
need to wake the interface for a reason that does not go through
`notif_publicar()`, and decide whether `PERIODO_OCIOSO` is worth the complexity
of fine-grained enable/disable during a drag.

### Stage 3 — tickless idle and light sleep

Turn on `CONFIG_FREERTOS_USE_TICKLESS_IDLE` and make the idle profile ask for
`light_sleep_ocioso = true`.

**It only makes sense after stage 2.** With the first slice done,
idle already wakes about 1x/s instead of 12.5x/s - the window to
try `CONFIG_FREERTOS_USE_TICKLESS_IDLE` now exists in a way it did not
before. It is still worth measuring again (`tasks`/`power locks`) before
flipping the switch: the event queue gain is theoretical until confirmed on
hardware, and the obstacles below (the Grove UART, esp_hosted SDIO) did not
change at all with this slice.

**The honest obstacles**, in order of how much they worry me:

1. **The Grove UART** (section 2.2.1). It is a design decision before it is
   code: sleeping costs Meshtastic messages. Run `power locks 30` to see
   whether it really shows up in the list.
2. **The esp_hosted SDIO.** An active link combined with light sleep is a
   delicate mix. It is worth saying that esp_hosted itself ships a ready-made
   example for this (`examples/power_save/host+cp/…`), which suggests it is an
   anticipated path and not virgin territory. But it cannot be tested with the
   C6 silent.
3. **The DSI panel.** Already solved — see 2.2.1. It stays here only to record
   that it was checked, not forgotten.

---

## 4b. Suspend: the hardware owner declares, the system decides (05/11/2026)

What suspend preserves (minimum clock, light sleep, the Wi-Fi receiver,
Wi-Fi against dozing, the screen on) comes from **a table in the kernel**
(`kernel/recursos`), from the union of the resources in use. The one that
declares is the **service that owns the hardware**, in its own on/off and under
its own name (`sistema_recurso(RECURSO_CAMERA, "navegador", true)`); C apps and
Berry scripts never hold off suspend - using the service is enough, and the
system shows who is using what (`power who` / `energia quem`,
`sistema.recursos()`). A shared hardware defect is fixed in the hardware layer
(`hal/tab5_dma.cpp`), never with an app hook. Details in
`docs/SUSPENSAO_2026-11-05.md`.

## 5. Summary in three lines

1. The kernel already exists, scattered, and is in reasonable shape; what does
   not exist is an **event queue**, and that is what holds back battery life.
2. `sistema.cpp` mixed core and shell in 2457 lines — Stage 1 has already
   separated them (see `kernel/core.cpp`/`kernel/shell.cpp`). It sped nothing up,
   but now it is safe to work on the seam for Stage 2.
3. Power saving is **written and switched off** (`sdkconfig:2951`) — but
   the hardest obstacle (the DSI panel locks) has already been knocked down
   . Before switching it on: `tasks` for the ceiling of the gain, and
   `power locks 30` to see whether the way is clear.

**Recommended order:** measure → ~~stage 2~~ → ~~stage 1~~ (done) → stage 3.
Stage 1 ended up landing before stage 2 (for convenience, not because of gain
priority) - it does not change the original reasoning: the real gain is still
in Stage 2, and it remains the next code step after measuring.

Stage 2 before 1 because the gain is there, and 1 is hygiene. If one day
we have to stop halfway, it is better to stop with an efficient system and
messy code than with tidy code and a system eating the battery.
