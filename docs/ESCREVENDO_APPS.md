# Writing apps for M5tabOS

This guide is the sum of the mistakes we have already made. Nearly every rule
here exists because something broke in a way that took hours to understand —
and the note says what, so you do not have to repeat it.

> **The API reference** (every function, what it does and which task it may be
> called from) is in [API.md](API.md). The [Example](../main/app_exemplo/app_exemplo.cpp)
> app uses each of them on a card: the quickest way to start is to copy its
> folder.

---

## 1. The minimum to exist

An app is **a folder inside `main/`, with the `app_` prefix**, and one or
more `.cpp` files (the prefix is there so you can tell apps apart from
`main/kernel/`, `main/hal/` and `main/services/` just by the folder name -
see the full tree at the end of this section). `CMakeLists.txt` globs
recursively: creating the folder is enough, there is nothing to register in the build.

```
main/
  app_meuapp/
    app_meuapp.cpp
```

At the end of the file, a descriptor:

```c
extern const app_t APP_MEUAPP = {
  .nome   = "MeuApp",
  .cor    = TEMA_APP_LARANJA,   // theme colour (tab5_tema.hpp), see section 2.5
  .perfil = { },              // the hardware IT needs switched on
  .usa_sd = false,
  .orientacao = ORI_EM_PE,    // or ORI_DEITADO, or ORI_SEGUE_IMU (section 9)
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,      // may be nullptr
  .ao_fechar = ao_fechar,     // may be nullptr
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,      // may be nullptr
  .periodo_ms = 0,
};
```

(The fields are Portuguese: `nome` = name, `cor` = colour, `perfil` = power
profile, `usa_sd` = uses the SD card, `orientacao` = orientation,
`piso_repouso_mhz` = minimum clock while idle, `ao_abrir`/`ao_fechar` = on
open/close, `desenhar` = draw, `ao_tocar` = on touch, `periodo_ms` = redraw period.)

> **`extern` is mandatory.** In C++, a `const` at file scope has **internal**
> linkage. Without the `extern`, `apps.cpp` cannot see the symbol and the
> error only shows up at link time, as `undefined reference` — far from the cause.

There is no list to edit: `main/CMakeLists.txt` generates the table from the
`app_*` folders, and the symbol name follows the folder name (`app_meuapp` →
`APP_MEUAPP`). To remove an app, delete its folder.

**The full system tree, to show where each thing lives:**

```
main/
  kernel/     core.cpp, shell.cpp, apps.cpp (generated), tarefa_app.*, pintura.*, app.h
              (the contract between apps and the system; who starts/stops services)
  hal/        tab5_tela, tab5_power, tab5_grove, tab5_teclado, tab5_sd,
              tab5_sensores, tab5_barramento, tab5_wifi, tab5_gps, nmea
              (pure hardware - none of it lives only because an app is open)
  services/   tab5_mesh, notificacao, ajustes, registro
              (state that must exist with the screen closed - see section 8)
  app_*/      each app, in its own folder with the app_ prefix
```

Dependency rule, from the bottom up: the HAL depends on neither services nor
apps; services do not depend on apps; the kernel does not depend on apps (only
the reverse - an app includes `app.h`, never the other way round). An app may
use the kernel, the HAL and services freely, but never another app's header.

---

## 2. The four drawing rules

This is the most important section of the document. **Most of the project's
visual bugs came from breaking one of these four.**

### 2.1. Never erase to redraw

M5GFX writes straight into the framebuffer the panel is scanning out. A
clearing `fillRect` followed by a `drawString` **shows up as a flicker**.

Write **over the top**, with a background colour and a **fixed width**:

```c
// WRONG - flickers
d.fillRect(x, y, 200, 30, TFT_BLACK);
d.drawString(texto, x, y);

// RIGHT - overwrites
snprintf(buf, sizeof(buf), "%-20.20s", texto);   // fixed width!
d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);          // background colour! (section 2.5)
d.drawString(buf, x, y);
```

If the content **shrank** (a list with fewer items), clear **only the strip
that is left over**, never the whole area.

### 2.2. Text state is GLOBAL, not your app's

`setTextDatum`, `setTextSize` and `setTextColor` apply to the whole display.
**Between two of your app's draws, the status bar draws** and leaves its own
datum behind (`middle_right`, for the battery percentage).

So: **set datum and size before every `drawString`**, not just on the
first pass.

```c
// WRONG - the text comes out anchored wherever the status bar left it
if (primeira_vez) { d.setTextDatum(top_left); }
d.drawString(buf, x, y);

// RIGHT
d.setTextSize(2);
d.setTextDatum(top_left);
d.drawString(buf, x, y);
```

If you write a button or box function that changes the style, **save and
restore it** — by value, because `getTextStyle()` returns a reference to the
internal member:

```c
const lgfx::TextStyle estilo = d.getTextStyle();   // a copy, not a reference
...draw...
d.setTextStyle(estilo);
```

### 2.3. Paint by difference, with `pintura.hpp`

```c
#include "pintura.hpp"

static pintura_t s_p;
static int  s_ult_valor;
static char s_ult_nome[24];

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);                    // decides whether this is a full pass

  if (pintura_completa(&s_p)) {
    ...titles, frames, everything that never changes...
  }

  if (pintura_mudou_i(&s_p, &s_ult_valor, valor)) {
    ...draw just the number...
  }
}
```

(`pintura` = painting: `pintura_iniciar` = start, `pintura_completa` = full
pass, `pintura_mudou_i` = "did this int change".)

Three things to get right:

**`pintura_iniciar()` checks by itself whether the system cleared the area.**
Without it (or with a hand-rolled `s_tudo` that forgets that check), **the
screen stays blank when coming back from idle** — the app thinks nothing
changed, but everything was erased.

**Do not use `||` between comparators.** `||` short-circuits and the later ones
are never called, leaving their memory stale — you get the classic "only
updates every other time":

```c
// WRONG
if (pintura_mudou_i(&s_p, &a, x) || pintura_mudou_i(&s_p, &b, y)) { ... }

// RIGHT
const bool m1 = pintura_mudou_i(&s_p, &a, x);
const bool m2 = pintura_mudou_i(&s_p, &b, y);
if (m1 || m2) { ... }
```

**The comparators are for deciding DRAWING, not I/O.** They return `true` on
every full pass — by design. If you use one to decide "re-read the SD card",
and the read asks for a repaint, you get an **infinite loop**. For I/O
decisions, compare by hand.

### 2.4. Large areas are cleared in STRIPS

When you really **need** to clear — you changed page, the layout changed —
do not `fillRect` the whole area. Use:

```c
#include "tab5_tela.hpp"

tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TFT_BLACK);
```

The reason is not aesthetic, it is **memory bandwidth**. The MIPI-DSI panel
pulls a continuous ~110 MB/s from PSRAM just to keep the image alive. A
full-screen `fillRect` writes ~1.8 MB to the same memory, as fast as the CPU
can. When the two bursts add up, the panel's FIFO runs dry — and the frame
comes out **blue**. The IDF driver describes the symptom in its own source
(`esp_lcd_panel_dpi.c:113`):

```c
// when an underrun happens, the LCD display may already becomes blue
ESP_DRAM_LOGE(TAG, "can't fetch data from external memory fast enough,
                    underrun happens");
```

`tab5_tela_limpar()` fills in strips and pauses between them. It costs a few
milliseconds on an operation that only happens when opening, closing or waking.

> This blue first appeared **when leaving an app for the home screen**, not when
> opening one. The home screen is almost entirely black, and a large black area
> is the fastest, most contiguous fill there is. Opening an app writes the same
> amount, but spread over dozens of small operations — and the burst never forms.

**For small rectangles — a button, a cell, a bar — the normal `fillRect` is
still the right choice.** A single strip would come out the same and still pay
for the loop.

**That includes "erase what is left over".** The pattern of clearing only the
strip the list no longer covers looks small, but when the list shrinks a lot it
becomes half the screen. That is why the blue came back: the system was clearing
in strips and the apps still had nine large `fillRect`s scattered about, each
able to become the burst on its own. If the rectangle **can** get large, use
`tab5_tela_limpar()`.

**If you see blue, look for `lcd.dsi.dpi` in the serial monitor.** The driver
logs the underrun at ERROR level. Without that line, the blue is something else.

---

### 2.5. Colours and sizes: the central theme (`tab5_tema.hpp`)

Since the 2026-11 redesign, **no app writes a colour by hand** (`0x18E3`,
`TFT_LIGHTGREY`, `color565(...)`). You ask for a colour by its **role**:

```c
#include "tab5_tema.hpp"

d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);                              // main text
d.fillRoundRect(x, y, l, a, TEMA_RAIO_G, TEMA_SUPERFICIE);           // card
d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);                       // caption inside the card
tab5_botao(d, x, y, l, a, "Salvar", TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE);
```

(`"Salvar"` is the translatable key; the English UI shows "Save".)

| Role | Use for |
|---|---|
| `TEMA_FUNDO` | the screen background |
| `TEMA_SUPERFICIE` / `_ALTA` | card, panel, list row / neutral button, field, card on a card |
| `TEMA_LINHA` | divider, thin border, grid |
| `TEMA_TEXTO` / `_2` / `_3` | main / secondary / disabled or hint text |
| `TEMA_DESTAQUE` (+ `_SUAVE`, `SOBRE_DESTAQUE`) | the **only** strong colour: primary button, selection, "on" |
| `TEMA_SUCESSO` / `ALERTA` / `PERIGO` / `INFO` | states (success / warning / danger / info) — only to communicate something, never decoration |
| `TEMA_APP_*` | 10 hues of equal lightness for categorising (and the app's `.cor`) |
| `TEMA_RAIO_*`, `TEMA_ESPACO_*`, `TEMA_LETRA_*` | corner radii, spacing and font heights |

**Set `.fundo_tema = true` in the `app_t`.** With it the system clears the
app's area with `TEMA_FUNDO` (without it, black, as before the redesign) and
`tab5_escrever`/`tab5_celula` draw text over that colour by themselves — the
area's background colour is `sistema_cor_fundo()`. Text with a `TEMA_FUNDO`
background inside a `TEMA_SUPERFICIE` card makes a box: pass the colour
**underneath**.

**Data colours are not theme colours:** a graph line, the X/Y/Z axes, an ANSI
colour, a satellite, an image pixel keep the colour that carries meaning.

In Berry it is the `tema` module (`tema.FUNDO`, `tema.RAIO_M`...).

## 3. Touch

The signature is `void ao_tocar(int x, int y)`, with coordinates **relative to
the app's area**. The area itself comes from `sistema_area()` — **never rebuild
it from constants**:

```c
// WRONG - the heights of the status bar and navigation strip are private to the system
const area_t a = { 0, 0, M5.Display.width(), M5.Display.height() - 64 - 96 };

// RIGHT
const area_t a = sistema_area();
```

**Do not do blocking I/O in the touch handler.** Reading the SD card, talking
to hardware or anything slow stalls the UI task **before any drawing**. Record
the intent and do the work in `desenhar()`:

```c
static bool s_precisa_ler = false;

static void ao_tocar(int x, int y) {
  if (...) { s_pagina++; s_precisa_ler = true; sistema_pedir_redesenho_completo(); }
}

static void desenhar(...) {
  if (s_precisa_ler) { ler_do_cartao(); s_precisa_ler = false; }
  ...
}
```

### Requesting a redraw

| call | when |
|---|---|
| `sistema_pedir_redesenho()` | a **value** changed |
| `sistema_pedir_redesenho_completo()` | the **screen** changed (page, selection, layout) |

The full version clears the area and goes through the double buffer, so it
**does not flicker**. The incremental one writes straight to the visible page —
right for a few pixels.

---

## 4. Power profile

The `perfil` says **what your app needs switched on**, never what others may
switch off. The system **adds** what it guarantees itself:

```c
.perfil = { .grove_5v = true },   // this app uses the Grove port
```

| field | who decides |
|---|---|
| `brilho` (brightness) | **the system** (the user's preference). Do not set it — it is ignored. |
| `radio` | **the system**, if the Wi-Fi service needs it |
| `grove_5v` | you **add** it; the user's setting can also switch it on |
| `imu`, `microfone`, `alto_falante`, `rtc` | phase 1: the union of all apps at boot |

> The rule exists because we broke it: every app declared `radio = false` (the
> default), so **opening any app cut power to the radio** and dropped Wi-Fi in
> the middle of a connection.

`piso_repouso_mhz` is the minimum clock frequency the app needs with the
screen off. `0` means "may drop to the minimum".

### Screen off: the power-saving contract

With the screen off the P4 **sleeps between one event and the next** (light
sleep). The software follows suit: anything with a periodic loop picks its
timeout by the level, instead of spinning at screen-on pace (`kernel/app.h`):

```c
// 1 s awake, 10 s with the screen off; an event in the queue wakes it at once
xQueueReceive(fila, &c, pdMS_TO_TICKS(sistema_ritmo_ms(1000, 10000)));
```

| function | what for |
|---|---|
| `sistema_economia()` | `ECONOMIA_ACORDADO` (awake) or `ECONOMIA_REPOUSO` (idle) right now |
| `sistema_ritmo_ms(acordado, repouso)` | the right timeout for the level |
| `sistema_economia_segurar(bit, sim)` | **prevents sleep** while you need a fast response (with your own bit, and release it afterwards) |

No throughput is lost: interrupts, queues and notifications wake it at once —
only the "check again just in case" gets spaced out. A connected Wi-Fi does not
hold it awake: the C6's SDIO does not wake the P4, so a 100 ms beat serves
whatever the C6 has gathered. Whatever asks for the receiver to be on holds it
(ESP-NOW chatting, CSI, sniffing). Sound playing, Grove with UART and the camera
hold it by themselves through their own drivers' locks. `power sleep`
(`energia sono`) in the console shows the state; measure it on battery with
`power trial` (`energia ensaio`).

### If your app changes a system preference

A settings app (like the Settings app) changes something that cuts across all
apps. To make it take effect **immediately**, do not touch the hardware:

```c
// WRONG - bypasses the "an app only adds" rule
tab5_power_grove(false);

// RIGHT - the system rebuilds the profile of whatever is on screen and applies it
ajustes_definir_grove(false);
sistema_reaplicar_perfil();
```

> This came from a bug: turning off the Grove 5V in Settings did not switch
> anything off — the rail only dropped when going back to the home screen. The
> app switched it on by hand with `tab5_power_grove(true)` and there was no
> matching call the other way, so "OFF" on screen and 5 V on the wire coexisted.
>
> The fix was not to call `tab5_power_grove(false)` as well. It was to **stop
> touching the hardware from the app**: the one that knows how to build the
> final state is the system, because only it knows the rule that an app adds
> and never subtracts.

---

## 5. Tasks

If the app needs background work, **use `tarefa_app`** (app task), never
`xTaskCreate` directly:

```c
static tarefa_app_t s_t;

static void meu_laco(tarefa_app_t* t) {
  while (tarefa_app_ativa(t)) {
    ...work...                          // with BOUNDED waits
  }
  // give back what you took here: fclose, xSemaphoreGive, etc.
}

static bool ao_abrir(void) {
  return tarefa_app_criar(&s_t, meu_laco, "meuapp", 4096, 3);
}

static void ao_fechar(void) {
  tarefa_app_encerrar(&s_t, 200);   // asks, waits up to 200ms, only then carries on
}
```

`tarefa_app_encerrar()` signals, aborts the pending block (`xTaskAbortDelay`)
and **waits for the task to confirm** before you destroy a queue or driver -
only once it has returned is it safe to free what the task was using. Killing
a task with `vTaskDelete` while it holds a semaphore or is inside a
`uart_read_bytes` leaves a resource hanging forever.

### More than one task in the same app

Yes, and it is not exotic — it is **one `tarefa_app_t` per task**, each with
its own body. For example, a GPS reader over UART:

```c
static tarefa_app_t s_t_leitura;   // reads the UART, nothing else
static tarefa_app_t s_t_parser;    // interprets what the reader published

static bool ao_abrir(void) {
  if (!tarefa_app_criar(&s_t_leitura, laco_leitura, "GPSRead",   4096, 3)) { return false; }
  if (!tarefa_app_criar(&s_t_parser,  laco_parser,  "NMEAParse", 4096, 2)) { return false; }
  return true;
}

static void ao_fechar(void) {
  tarefa_app_encerrar(&s_t_parser,  200);   // ORDER matters: the consumer
  tarefa_app_encerrar(&s_t_leitura, 200);   // first (otherwise the producer
}                                           // writes into a queue whose reader has gone)
```

Why two instead of one: **each task should do ONE thing**, at its own pace.
Reading the UART is blocking and cannot wait; interpreting what arrived can be
slower without delaying the next read — giving the two different priorities
(`3` and `2` above) makes that explicit. They communicate through a **queue**
(`xQueueCreate`), never through an unlocked global variable: it is the same
"whoever draws does not touch hardware" rule applied to tasks that talk to
each other.

The stack budget **adds up**: two 4096-byte tasks are 8KB, on top of the UI
(10240) and any system service already running. That is no reason not to
create the second task — it is a reason not to create a third "just because
you can": if two queues solve it, two tasks are enough.

---

## 6. SD card

The card may be **on the computer** (USB mass-storage mode, Settings > USB
(flash drive)). That is why writing and reading follow system rules — follow them and
your app does not need to know whether the card is in the Tab5 or not.

**Writing data: only through the three doors** (`hal/tab5_sd.hpp`). They mount
the card by themselves and, while it is on the computer, keep the data in the
internal flash journal, which reaches the card when it comes back, in the
right order:

```c
// append a line, without waiting (goes in batches; great for logs and history)
tab5_sd_anexar("MeuApp", "dados.csv", linha, "date,value\n");   // header if the file is empty

// append a block now (for code that already batches)
tab5_sd_acrescentar("MeuApp", "dados.bin", bloco, n);

// replace the whole file (never leaves half a file)
tab5_sd_substituir("MeuApp", "config.txt", texto, n);
```

`acrescentar` (append) and `substituir` (replace) block: call them from the
worker (`sistema_trabalho`), never from the UI task.

**Only rewrite what you have read.** If your app keeps a list on the card and
rewrites it whole with `tab5_sd_substituir`, it must have read the file first.
If there was no card at read time, do not rewrite: the empty list in RAM would
wipe out what was stored. (This is what the Wi-Fi networks, the Mesh NodeDB and
the deleted conversations do.)

**Reading:** `tab5_sd_abrir(app, arquivo, "r")` — it first writes whatever was
pending, so you always read the latest version. With the card on the computer
it returns `NULL`, as if the card were not there; treat that as "no data". It
distinguishes "file does not exist yet" (normal) from a real failure.

**Declare what the app needs:**

- `.usa_sd = true` — the system mounts the card **before** `ao_abrir()` and
  unmounts it **after** `ao_fechar()`. The app opens even without a card.
- `.exige_sd = true` — without the card the app has no purpose (browsing
  files, saving a photo). With the card on the computer, **the system does not
  open the app**: it shows the reason in its place and opens it by itself when
  the card comes back. And USB mass-storage mode does not start while the app
  is open. The app does not need to handle "the card vanished halfway through".

A **large** new file (a photo, received firmware) is the only case for writing
with `tab5_sd_abrir(..., "wb")` directly: it needs the real card, so whatever
produces it declares `.exige_sd = true`.

If you need to mount it yourself (to read in a task, for example), mounting is
reference-counted and **every mount needs its unmount, on every exit path** —
including the error ones.

```c
if (!tab5_sd_montar()) { return; }      // also false with the card on the computer
FILE* f = tab5_sd_abrir("MeuApp", "dados.csv", "r");
if (!f) { tab5_sd_desmontar(); return; }    // <- do not forget this one
...
fclose(f);
tab5_sd_desmontar();
```

---

## 7. Exclusive resources

Some resources fit **one user at a time** and have an arbiter in the system.
Always ask, and **respect a no**:

```c
if (!tab5_grove_tomar(GROVE_DONO_GPS)) {
  // Another owner. Open anyway if it makes sense, and EXPLAIN it on screen.
}
...
tab5_grove_soltar(GROVE_DONO_GPS);   // in ao_fechar()
```

(`tomar` = take, `soltar` = release, `DONO` = owner.)

> Refusing to open without explaining is the worst outcome: the user taps the
> icon and the device seems to do nothing.

---

## 8. App or service?

This is the most important design decision.

| | app | service |
|---|---|---|
| lives while | it is on screen | the system is on |
| examples | Files, Settings, Camera | Wi-Fi, Meshtastic, GPS, logging, keyboard |
| use when | the work **is** the screen | the state must exist with the screen closed |

If the feature needs to **notify** something with the app closed, it is a
service — and the app becomes just its **window**. That is how Meshtastic
announces a new message without being open - and it is exactly why the GPS
stopped being read directly by an app: the Mesh app needed to ask "where am I"
to work out the distance to a received location, and one app cannot see
another's private state. Promoted to a service, any app can ask - and the app
that displayed the GPS became just its window, exactly as Mesh already was for
Meshtastic.

A service lives in `main/services/` (or in `main/hal/` when what it exposes is
essentially a hardware reading, like the GPS - the line between the two is
"has business rules" vs "just delivers the raw data"), has its own task created
by the KERNEL (`main/kernel/`) - never by the app - and exposes reads by copy,
never a pointer to a structure the task rewrites. Whatever starts/stops because
the module on the Grove port changed is always the kernel (`kernel/core.cpp`),
the same way for Mesh and for GPS - an app never calls any service's
`_iniciar()`/`_parar()` (start/stop) directly.

**Ready-made parts for services — do not rewrite them:**

| you need | use | instead of |
|---|---|---|
| to stop your own task safely | `kernel/parada.hpp` (`parada_preparar` / `parada_pedida` / `parada_confirmar` / `parada_pedir`) | two flags + `for (...) vTaskDelay(10)` asking whether it has exited |
| to deliver an event to ONE listener | `hal/tab5_ouvinte.hpp` — removing the listener only returns after the in-flight call | a `volatile` pointer and a `vTaskDelay` "to give it time" before destroying the queue |
| to read/write Meshtastic protobuf | `services/mesh_pb.hpp` — a reader that never adds to the pointer, a writer that never runs past the end | `*p++ = 0x22` and a buffer "with room to spare" |
| "has the clock been set yet?" | `tab5_hora_epoch()` / `tab5_hora_valida()` (`services/tab5_hora.hpp`) | `time(nullptr) > 1600000000` |

The rule behind all four: **wait for an event, not for a timeout.** Whoever
needs to know that something finished receives a signal (a semaphore, a
count); `vTaskDelay` is for documented hardware timing (the datasheet asks for
X µs) and for yielding the CPU — never for "it should have had time by now".

### 8.1. App with a BACKGROUND PART: `kernel/fundo.h`

Between "app" and "service" there is a third case: the app whose work carries
on when the window closes - recording a track, sending the position over the
internet, a long calculation. **Do not** turn it into a hand-made service for
that: describe the background part (`fundo`) and let the kernel run it.

```c
static void meu_laco(fundo_t* f) {
    while (fundo_esperar(f, 5000)) {          // sleeps; false = asked to stop
        ...work with BOUNDED waits...
        fundo_estado(f, n, "42 sent");         // the text of the grey pill
    }
}

const fundo_def_t MEU_FUNDO = {
    .nome   = "GPS online",                    // task, notification and lists
    .corpo  = meu_laco,
    .pilha  = 4096,
    .perfil = { .radio = true },               // ADDED to the effective profile while it runs
    .ao_iniciar = preparar,                    // optional, on the UI task
    .ao_parar   = liberar,                     // optional, after the loop has exited
};

// in the window:
sistema_fundo_iniciar(&MEU_FUNDO);
sistema_fundo_parar(&MEU_FUNDO);               // waits for the loop to exit (up to 3 s)
bool rodando = sistema_fundo_ativo(&MEU_FUNDO);
```

What the system guarantees without you writing anything: its own task at
priority 1 (never stalls touch), the **grey pill** with the name while it runs
(nothing runs hidden), the **power profile added** to the screen's, a stop that
wakes the loop at once, and clean-up when the loop ends by itself. In the
console: `background` (`fundo`) lists them, `background stop <name>` stops one.

**The user can stop any background part.** Every background part appears in
the notification centre (tap the status bar), in the **In the background**
section, with an X that belongs to the system: the first tap shows "Stop?",
the second stops it. The app writes nothing to get this and cannot refuse. In
return: the loop exits right after the request, and everything that must
survive is saved in `ao_parar()` - it always runs, including when it was the
user who stopped it with the window closed. A background part that can sit
idle (paused music) shuts itself down after a while. The full contract is in
the header of `kernel/fundo.h`.

The loop rules:

- **Never wait forever.** The real wait is `fundo_esperar()`; everything else
  has a timeout, otherwise the stop request never arrives.
- **A calculation loop calls `fundo_ceder(f)` on every iteration** - without
  it the core stays busy, IDLE does not run, the watchdog fires and the device
  does not sleep.
- **The loop does not draw.** It keeps the state (protected) and the window
  reads it by copy.
- **The background part's memory belongs to the background part** - static,
  or allocated in `ao_iniciar()` and freed in `ao_parar()`. Never a buffer of
  the window: it closes whenever it likes.

The live example is the FUNDO (background) card in the Example app: a
stopwatch (`laco_fundo`, `EXEMPLO_FUNDO`) that keeps counting, with the pill in
the status bar, after the window closes.

**When the work is not the app's, it is the system's.** Music is the example:
the player is a service (`services/tab5_musica`) that runs as a background
part, and the Music app is just its interface - list, buttons, spectrum. If
your app needs something another app (or a Berry script) will also want, ask
for a service instead of writing the engine inside the window. See docs/API.md,
section 8.

### 8.2. Notifying the user: `services/notificacao.h`

Notifications are a **system service**: any service or app publishes, and the
status bar shows them without knowing who is who. Nobody touches the status
bar to announce anything.

```c
#include "notificacao.h"

static int s_notif = -1;

// Register: may be called on every start/open - it is idempotent by NAME
// (the same name returns the same slot, WITH the state it had). The name must
// be a literal/static.
s_notif = notif_registrar("My service");
notif_silenciar(s_notif, !minha_preferencia_de_som);   // optional, per source
notif_associar_app(s_notif, "MeuApp");   // optional: the app a tap opens

// Publish the CURRENT STATE (not an event). From any task.
notif_publicar(s_notif, 3, NOTIF_INFO, "3 new");
notif_limpar(s_notif);                                 // nothing to say
```

The rules, and why:

- **State, not event.** Publish "I have 3", never "1 more arrived".
  Republishing the same state costs nothing (the status bar only repaints when
  something changes).
- **The count is derived from your source of truth**, never kept by parallel
  arithmetic. Mesh kept a total separate from the conversations, and the two
  drifted apart: new messages vanished from the status bar. Recalculate from
  what you store, at publish time.
- **Choose the level by what it means:**

  | level | when | in the status bar |
  |---|---|---|
  | `NOTIF_ESTADO` | activity in progress ("recording track") | grey; only shows if nothing else has anything to say |
  | `NOTIF_INFO` | news that does not need you (channel traffic) | blue |
  | `NOTIF_ATENCAO` | addressed to you (direct message) | orange |
  | `NOTIF_URGENTE` | needs action (battery running out, card full) | red |

- **The status bar shows ONE source prominently**: the one with the highest
  level and, on a tie, the one that changed last, with **its summary** - so
  write a summary that explains itself ("1 on LongFast", not "1"). The other
  active ones become a **dot in the level's colour** beside it (whatever does
  not fit becomes "+N").
- **Notification centre:** tapping the status bar opens the list of ALL active
  sources (`notif_listar`), one row per app with name, summary and count.
  Tapping the row opens the associated app - by default the app with the
  **same name** as the source; if the name differs ("Meshtastic" → the "Mesh"
  app), call `notif_associar_app()`. The lock screen shows the same list (up
  to 4 rows).
- **Do not clear the notification on a restart.** Registering again no longer
  resets the source, and a service that stops and starts itself (changing
  transport, network dropping) must not call `notif_limpar()` if there is
  still something to read - the notification used to vanish without the user
  having opened anything. Clear it when the thing has been **seen**.
- **"Seen" only counts with the app visible.** With the screen locked or the
  notification centre open the app is *covered* (`sistema_app_encoberto()`): it
  receives no `ao_aviso`, touch or key - notifications wait until it is back in
  front. So marking as read inside `ao_aviso`/`desenhar` is safe.
- **The beep** sounds when the count of an unmuted source **goes up**. A
  progress counter (points, bytes) at a level above `ESTADO` would beep at
  every step - use `ESTADO`, or mute the source.
- **App vs service:** an app only runs while open, so it only notifies while
  it is on screen. A notification that must exist with the app closed is a
  service's job (see above). On closing, an app that published must call
  `notif_limpar()`.

---

## 9. Orientation: portrait, landscape or following the device

`.orientacao` chooses between the two screen states that exist in the system:

| value | screen | when |
|---|---|---|
| `ORI_EM_PE` (default) | 720×1280, rotation 0 | most apps, like the home screen |
| `ORI_DEITADO` | 1280×720, rotation 3 | fixed landscape, ExtPort1 (keyboard) at the bottom |
| `ORI_SEGUE_IMU` | one of the two, following the device | messaging apps (Mesh, ESP-NOW) |

Rotations 1 and 2 are never used: the screen does not flip "the other way
round". The system rotates on opening and returns to portrait on closing.
`sistema_area()` already returns the new dimensions.

**`ORI_SEGUE_IMU`** (`kernel/orientacao`): with the app open, the system reads
the IMU at 5 Hz and rotates when gravity sits clearly along one of the
screen's axes for ~0.6 s. A device lying on the table or held diagonally
changes nothing (dead zone), and after rotating there is a 1.5 s grace period.
With the physical keyboard docked, it stays in landscape. The rotation happens
between two frames, never with a finger on the screen, and is followed by a
full pass. The app must:

- declare `.perfil = { .imu = true }` (the IMU only exists if some app asks for it);
- **always** draw from the area it is given — the width changes while the app
  is open. Any measurement kept between frames must be redone (`kernel/lista`
  and `kernel/conversa` already do this by themselves);
- if it has different portrait and landscape layouts, switch them in
  `ao_girar` (on rotate, optional): it is called after the rotation takes
  effect and before the full pass.

Drawing and touch need the **same** geometry function (`area` → position), so
they agree in both orientations — Mesh uses `det_btn_l(a)`, `cfg_btn_l(a)` and
the `COLUNA`/`COLUNA_PE` grid of the node table.

If your app puts a large control near the bottom edge, lock navigation while
it is open — the Home button sits just below it. While locked, the navigation
strip belongs to the app: a touch there arrives on the last row of the area.

```c
sistema_travar_navegacao(true);    // on opening
sistema_travar_navegacao(false);   // on closing
```

The virtual keyboard already does this by itself
(`tecvirt_abrir`/`tecvirt_fechar`). The system unlocks it when any app closes,
as a safety net.

While locked, navigation also blocks leaving by another route: tapping the
status bar does not open the notification centre (one of its rows would open
another app), and `sistema_simular_abrir` for another app is refused.

For work the user watches without touching the screen (a long copy), the app
can keep the screen on — the inactivity idle does not happen:

```c
sistema_manter_acordado(SISTEMA_ACORDADO_APP, true);    // for as long as needed
sistema_manter_acordado(SISTEMA_ACORDADO_APP, false);   // the system also releases it when the app closes
```

### Virtual keyboard

To ask for text (password, PIN, message), use the system keyboard
(`tecvirt`) — the app neither draws keys nor handles fingers:

```c
static tecvirt_t s_kb;
tecvirt_abrir(&s_kb);                        // when editing starts
tecvirt_posicionar(&s_kb, a);                // in desenhar: the area (screen) at whose bottom it opens
tecvirt_pintar(&s_kb, d, completo);          // EVERY pass; with no change it draws nothing
n = tecvirt_nivel(&s_kb, acoes, 8);          // in ao_tocar_nivel: confirmed keys
ac = tecvirt_clique(&s_kb, xs, ys);          // in ao_tocar, if tecvirt_contem(&s_kb, xs, ys)
tecvirt_fechar(&s_kb);                       // when finished and in ao_fechar
```

The actions are the character (>0, the **codepoint** with shift already
applied — the second symbols page has the Portuguese accents),
`TECVIRT_APAGAR` (backspace), `TECVIRT_ENVIAR` (send) and `TECVIRT_FECHAR`
(close). Shift and page switching belong to the keyboard and never reach the
app. A field without a cursor (password, name) does not need to handle
characters or UTF-8 by hand:

```c
if (ac == TECVIRT_ENVIAR) { ... }
else if (ac == TECVIRT_FECHAR) { ... }
else { tecvirt_editar(buf, sizeof(buf), ac, so_ascii); }   // appends at the end / deletes the last one
```

Running text (a message) turns on automatic capitals with
`tecvirt_frase(&s_kb, true)` and tells the keyboard the text and cursor
whenever either changes: `tecvirt_contexto(&s_kb, texto, cursor)`. The system
conversation component already does this. Repaint only the field when the text
changes — never the whole page per letter. Example: the Wi-Fi password in
`app_config/app_config.cpp`.

The keyboard's height **depends on the orientation** (in portrait the keys are
taller, because there width is short and height is plentiful). To reserve space
above it, ask `tecvirt_altura(a)` with the same area as `tecvirt_posicionar` —
never store a number.

---

## 10. Touch scrolling

If your content does not fit the area, **do not shrink the font**. Declare the
total height and the system takes care of the rest:

```c
extern const app_t APP_X = {
  ...
  .altura_conteudo = 1240,   // taller than the area: scrolls
};
```

`0` (the default) means "does not scroll", and the app sees the visible area
as always.

With scrolling on, the system:

- follows the finger and scrolls, with a limit at both ends;
- **clips** drawing to the area — without that you would paint over the status
  bar without noticing;
- **adds the scroll offset to the `ao_tocar` coordinates**, in one place only;
- **swallows the tap** when the finger has moved more than 12 px, so the end of
  a scroll does not press whatever is underneath;
- draws the little indicator bar.

The important point is the third: **you compare the touch against the same
constants you used to draw.** The whole app lives in a coordinate system that
starts at 0 and goes up to `altura_conteudo` (content height), and never adds
any offset. The alternative — every app adding an offset to every `y` — is the
recipe for the app that forgets to add it in *one* place and ends up with a
button that responds wrongly after scrolling.

> The Settings app was born without a grid: elements were positioned by eye, and
> the screen-timeout value (x 264 to 528) ran over its two buttons (400 to 490
> and 500 to 590). The fix was not to touch those numbers — it was to give each
> section a known height and position everything by relative offset. Then it
> no longer fitted on the screen, and scrolling stopped being a problem.

---

## 11. Checklist before opening a PR

- [ ] folder `main/app_x/` with `extern const app_t APP_X` (the name follows the folder)
- [ ] No clearing `fillRect` on a periodic path
- [ ] Large areas cleared with `tab5_tela_limpar()`, not `fillRect` — including those that only *can* get large
- [ ] `setTextDatum`/`setTextSize` before every `drawString`
- [ ] Uses `pintura.hpp`, with no `||` between comparators
- [ ] `ao_tocar` uses `sistema_area()`, with no copied constants
- [ ] No blocking I/O in `ao_tocar`
- [ ] Tasks created with `tarefa_app_criar` and stopped in `ao_fechar`
- [ ] Every `tab5_sd_montar()` has an unmount on every path
- [ ] `.perfil` does not set `brilho`
- [ ] System preferences applied through `sistema_reaplicar_perfil()`, without touching the hardware
- [ ] Exclusive resources requested with `tomar()` and given back with `soltar()`
- [ ] Buttons/cells use `tab5_ui.hpp`, not a local copy
- [ ] Colours and corners only from `tab5_tema.hpp`, with `.fundo_tema = true` (section 2.5)
- [ ] Tested: open, close, go idle and **wake**

The last item catches more bugs than all the others put together.

## Ready-made components: do not reimplement buttons

`tab5_ui.hpp` exists because **four different apps** wrote the same button,
byte-for-byte identical - comment included - before anyone noticed. The comment
each copy carried told the same story: without saving and restoring the
`TextStyle`, a button's `middle_center` **leaks** into the next `drawString`
of whoever draws afterwards, pushing text off the screen. It is the same trap
that had already bitten `tab5_power_mostrar` - and it bit again in every copy,
because each one rediscovered the bug on its own instead of inheriting the fix.

```c
#include "tab5_ui.hpp"

tab5_botao(d, x, y, largura, altura, "OK", TEMA_DESTAQUE);
tab5_botao(d, x, y, largura, altura, "OK", TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE, 3);  // text colour and size, if you want them different from the default
```

For a "small label on top, large value below" panel - the same design as the
home screen (`GROVE`/`CARTÃO`, shown in English as GROVE/SD CARD) - use
`tab5_celula()` (cell) instead of drawing the three `drawString`s by hand:

```c
tab5_celula(d, centro_x, y0, "FIX", "GPS", TFT_WHITE, "HDOP 0.9 - 8 sats");
```

If you notice another piece of drawing repeated in two apps, that is a sign it
should live here too - not in a third copy.

## Clearing the screen: always ask, unconditionally

Call `tab5_tela_limpar()` whenever you want a clear area. **Do not** check
`sistema_area_foi_limpa()` to decide whether it is worth it — that decision is
not yours.

The system clears the whole usable area before calling you for a full repaint,
and tells the display layer what it did. If your request is already satisfied,
it becomes a no-op further down. You write the intent; the kernel decides the
work.

This matters because clearing the same surface twice is ~3.2 MB written to
PSRAM in a burst, while the panel pulls 119 MB/s from it to stay alive. The
panel's FIFO runs dry and the frame comes out **blue**. It was a bug that showed
up in some random app, months later, far from the cause — and that three of
the eight apps avoided by remembering the rule, which is the very definition
of a rule in the wrong place.

`display` (`tela`) in the console shows how many clears were skipped.

Never use `d.fillRect()` for large areas: it writes everything at once, with no
pause. `tab5_tela_limpar()` writes in strips with a breather between them.

**The number, so "large" is not left vague:** a strip is 96 KB, i.e. ~49,000
pixels. A `fillRect` above that becomes a burst; below it, it fits in one strip
and `tab5_tela_limpar` changes nothing. In practice: screen width × more than
40 rows already exceeds the limit in portrait, and × more than 38 in landscape.

When in doubt, use `tab5_tela_limpar()`. It is never worse than `fillRect`.

## A loop that draws a lot

It needs nothing special. Since the compositor ([COMPOSITOR.md](COMPOSITOR.md))
the app draws on the back page and the system brings it to the screen by DMA,
between two panel scans: the write burst no longer competes with the panel's
reads. The old `tab5_tela_gastou()` remains as an empty function, only so as
not to break code that still calls it.

---

## Animation: `periodo_ms` and `sistema_quadro_dt()`

`periodo_ms` is the **requested** pace. The system rounds it to a whole number
of panel scans (the nearest to the request, never less than one) and starts the
frame just after a scan — so it always goes on air at the same phase, without
the judder of "every so many frames one stays on screen a period longer"
(`kernel/ritmo.hpp`). There is nothing to do to get this.

To **move** things, do not measure time yourself inside `desenhar()`: ask for
`sistema_quadro_dt()` (frame delta). It gives the time between the scans at
which this frame and the previous one **appear**, not between the moments they
were drawn — what appears moved exactly as much as it stayed on screen. Clamp
it your own way (after a pause it is large):

```c
float dt = sistema_quadro_dt();
if (dt > 0.1f) { dt = 0.1f; }
bola.x += bola.vx * dt;
```

And draw in small boxes as much as you like: many small objects per frame are
the compositor's problem, not yours — it merges whatever overlaps and copes
with dozens of areas without copying leftovers.

---

## The blue flicker: what is the system's and what is yours

It is worth knowing the cause, because it explains all the rules above.

The panel is MIPI-DSI in DPI mode. It **does not receive** the image: a DMA
reads the framebuffer from PSRAM line by line, forever, at the pixel clock
rate. If that DMA falls behind, the panel's FIFO runs dry and the frame comes
out **blue** — the IDF driver itself says so in `esp_lcd_panel_dpi.c:113`.

Two things delay the DMA, and **both belong to the system, not to you**:

| Cause | Where it was solved |
|---|---|
| The CPU competes with the panel for PSRAM | Priority in the AXI-ICM (`tab5_barramento`) and, on the ILI9881C screen, the panel clock at 60 MHz instead of 80 (`tab5_power_relogio_dpi`; ST71xx screens keep the factory clock) |
| The interrupt that restarts the DMA is disabled while writing to flash | `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y` |

All that is left on your side is **not making large bursts**, and the rules
for that are already in sections 2.1 and 2.4:

- clear with `tab5_tela_limpar()` — **always, unconditionally**; the kernel
  decides whether there is work to do.

### Ask for the RIGHT repaint

This is the most expensive mistake and the easiest to make:

```c
sistema_pedir_redesenho();            // a VALUE changed     -> cheap
sistema_pedir_redesenho_completo();   // the LAYOUT changed  -> ~4 MB of PSRAM
```

A full repaint erases the usable area (1.6 MB), redraws everything, syncs the
whole framebuffer (1.84 MB) and flips the page. **Asking for that to change one
letter inside a text field is the shortest road to the blue flicker** — and it
is exactly what the Mesh app used to do on every keystroke.

Rule of thumb: if what changed stays **in the same place and the same size**,
it is `sistema_pedir_redesenho()`. Only ask for the full one when something
moved, appeared or disappeared.

### Physical keyboard

The ExtPort1 keyboard arrives through the `app_t`'s `ao_teclar` (on key),
already translated by the driver (`hal/tab5_teclado`, HID mode with the
official table of the 70 keys). Text comes as UTF-8; special keys come in
`codigo` (code):

| code | key |
|---|---|
| `TAB5_TECLA_ENTER`, `_ESC`, `_TAB` | Enter, Esc, Tab |
| `TAB5_TECLA_APAGAR` / `_DEL` | Backspace (before the cursor) / Del (after) |
| `TAB5_TECLA_ESQUERDA`, `_DIREITA`, `_CIMA`, `_BAIXO` | arrows (left, right, up, down) |

Unknown code: ignore it. With Ctrl/Alt (`mod`) it is a shortcut, not text.
Accents come via dead keys, as in the US International layout: `'`+vowel = á,
`'`+c = ç, `~`+a = ã, `^`+e = ê, `` ` ``+a = à; `'`+space = `'`. A field that
only accepts ASCII (password, node name) passes the text through
`tab5_teclado_ascii()`, which undoes the composition (á → `'a`).

```c
static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  if (codigo == TAB5_TECLA_ENTER) { enviar(); return; }
  if (codigo == TAB5_TECLA_APAGAR) { apagar_ultimo(); return; }
  acrescentar(texto);            // empty when it was a special key
}
```

`tab5_teclado_presente()` tells you whether a keyboard is docked. If your app
has a virtual keyboard, use it so as not to steal half the screen — **but then
implement `ao_teclar`**, otherwise there is no way left to type.

---

## Scrolling lists: use `kernel/lista` (pattern, touch)

Do not write scrolling by hand, nor the "which item did the finger touch"
maths. Every list in the system (Mesh: messages, conversations and nodes;
Files; Tasks) uses the same part. The pattern is LVGL's: **an item is measured
once, when it changes, and the list stores the result.**

```c
static lista_t s_l;
static int  altura_de(int i, void* ctx);                  // reads from YOUR cache
static void desenhar_item(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void* ctx);

// on opening
lista_definir(&s_l, ROLAGEM_INICIO /* or ROLAGEM_FIM */, n, altura_de, desenhar_item, nullptr);
s_l.topo = 12;                          // optional: breathing room before the 1st item
// items changed (added, removed, re-measured)
lista_itens_mudaram(&s_l, n);
// ONE item changed value, same height (age, counter)
lista_item_mudou(&s_l, i);
// ao_tocar_nivel
if (lista_arrastar(&s_l, x, y, pressionado)) { sistema_pedir_redesenho(); }
// ao_tocar
int dy;
const int i = lista_item_tocado(&s_l, x, y, &dy);   // -1 = nothing; LISTA_ARRASTE = it was a scroll
// desenhar
lista_desenhar(&s_l, d, janela, cor_de_fundo, pintura_completa(&s_p));
// the list's screen went away (another page of the app, a file opened on top)
lista_esconder(&s_l);
```

Rules:

- **The list owns its window.** It clears, clips, scrolls, draws the scroll
  bar (8 px on the right — do not draw items there) and only repaints the
  exposed strip or the marked items. Do not call `tab5_tela_limpar` on its
  window.
- **An item draws only within its own height.** Decoration that rises above
  the item (a counter in the corner) needs breathing room inside the item; the
  list only repaints the item crossing the strip, and whatever sticks out of it
  is left without an owner.
- **A touch only counts where the list last drew.** It converts the area
  coordinates by itself, and the window already excludes whatever something
  else covers (keyboard, compose bar). On a page change, call `lista_esconder`
  on the lists that went away.
- **Frame and header stay outside the window.** Draw them on the full pass;
  the list never reaches them.
- **Never measure text inside `desenhar_item`**: measure when building your
  cache (see `construir_layout` in the Mesh app).

`kernel/rolagem` (scrolling) is the engine under the list and under whole-area
scrolling (`altura_conteudo`). Apps do not use `rolagem_*` directly.

## Messaging app: use `kernel/conversa`

Bubbles, compose bar, virtual and physical keyboard, scrolling and day
dividers are a system component (`conversa` = conversation) - do not copy the
Mesh app. The app answers three questions (how many messages, what message i
looks like, send this text) and passes drawing and touch through:

```c
static conversa_t s_c;  static pintura_t s_p;
ao_abrir:        conversa_abrir(&s_c, &s_p, contar, ler, enviar, nullptr);
ao_fechar:       conversa_fechar(&s_c);
ao_aviso:        conversa_mudou(&s_c);
desenhar:        pintura_iniciar(&s_p);
                 conversa_desenhar(&s_c, d, retangulo, pintura_completa(&s_p), pode_escrever, motivo);
ao_tocar / ao_tocar_nivel / ao_teclar: conversa_tocar / _tocar_nivel / _teclar
```

The rectangle goes down to the bottom of the app's area (the keyboard opens
over the lower part). A complete, short example: `app_espnow/app_espnow.cpp`.
The virtual keyboard lives in `kernel/teclado_virtual` (see section 9) and line
wrapping is `conversa_quebrar_linhas` - Mesh already uses both.
