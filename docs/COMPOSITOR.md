# M5tabOS — the new screen foundation (LVGL-style compositor)

Decided. We will not adopt LVGL: we will build **our own**
foundation, on the same principles that make LVGL smooth, within our
context (M5GFX as the rasteriser, FreeRTOS, power profiles, Tab5).

The rule that guides everything from here on:

> **A screen problem is a SYSTEM problem.** If an app needs an extra
> `fillRect`, a breather, a special case so it does not flicker or
> tear, the foundation is wrong, not the app. No per-app bodges.

---

## 1. Why the current foundation cannot be fixed by patching

Today M5GFX writes **straight into the page the panel is scanning**. Everything
that exists around that is a defence against that decision:

| today's patch | against what |
|---|---|
| paint by difference, fixed width, `pintura_*` | erase-and-redraw shows up as a flash |
| `s_sujo_layout` × `s_sujo_dados`, `sistema_area_foi_limpa()` | the app does not know when the screen was cleared |
| save/restore `getTextStyle()` (rule no. 2) | global drawing state leaking between functions |
| `tab5_tela_limpar` in strips, `respirar`, `tab5_tela_gastou` | burst of writes to PSRAM → underrun → blue frame |
| `tab5_tela_rolar` + `rolagem_item_preservado` | reuse pixels to scroll more cheaply |
| `ROLAR_RESPIRO_US`, "write faster than the beam" | tearing in the image while scrolling |
| `REPINTURA_MIN_MS = 80` | too many repaints per touch |
| double buffer only for full repaints | black flash when switching apps |

Every line of that table was a real bug, fixed the right way **for the
foundation we had**. The problem is the foundation. The first attempt (double
buffer only in the messages window) showed the limit: the flip is a whole page,
and without knowing **everything** that changed in the frame (the text entry
bar, the LongFast icon), the other page goes stale at those spots and flickers.

---

## 2. The six rules (what LVGL does, and what we will do)

1. **Never write to the page the panel is scanning.** All drawing goes
   to the back page. The front one is only read by the DSI DMA.
2. **One GLOBAL list of dirty areas per frame.** Any change (a text,
   the battery, scrolling) *invalidates* a rectangle. After the flip, we copy
   **only those rectangles** to the new back page — that is what was missing
   from the first attempt, and it is what makes double buffering cheap: the
   cost is proportional to what changed, not to the screen.
3. **Flip at the frame boundary.** The IDF driver already does this (the
   `cur_fb_index` is only read when the DMA restarts, `esp_lcd_panel_dpi.c:75`).
   We wait for `on_frame_buf_complete` before touching the page that left.
4. **One frame per cycle.** Touch, new data and timers only *invalidate*. A
   frame loop gathers everything and draws once. Nothing is drawn "at the
   moment" of the event. With nothing dirty there is no frame — zero cost when
   still.
5. **Scrolling is clipped redrawing, not shifting pixels.** The list changes
   its offset, invalidates its own area, and the next frame redraws it on the
   back page. No fragments are possible, because nothing from before is reused.
6. **Fonts at the exact size, with anti-aliasing.** No fractional scaling of
   bitmap fonts (the `TAM_MSG 3.8`). Fonts generated at the size they are used.

---

## 3. What the hardware already offers (checked in the installed IDF 5.5.5)

- **Two framebuffers** already allocated (`num_fbs = 2` in M5GFX's Panel_DSI),
  720×1280 RGB565 = 1.84 MB each, in PSRAM.
- **Hardware flip**: `esp_lcd_panel_draw_bitmap()` with a pointer inside a
  framebuffer only swaps the index (`esp_lcd_panel_dpi.c:506-524`).
- **Frame-complete notification**: `esp_lcd_dpi_panel_register_event_callbacks()`
  with `on_frame_buf_complete` (and `on_vsync`).
- **PPA** (`esp_driver_ppa`): `ppa_do_fill`, `ppa_do_blend`,
  `ppa_do_scale_rotate_mirror` (with scale 1 = rectangle copy with stride).
  Copies and fills via DMA2D, without the CPU.
- **Priority on the AXI bus** (`hal/axi_icm_ll.h`): QoS per master,
  including the DW_GDMA, which is the DMA that feeds the DSI. The driver itself
  points to this as the remedy for underrun (`esp_lcd_panel_dpi.c:113`). It is
  the **root cause** of the blue frame: today the CPU and the panel compete for
  PSRAM on an equal footing; with QoS, the panel's read wins.

---

## 4. The architecture

```
  events (touch, data, timers)
        │  only invalidate
        ▼
 ┌──────────────────────┐
 │  OBJECT TREE         │  screens, labels, buttons, lists, images, canvas
 │  (kernel/ui_*)       │  changing a property = invalidating its own area
 └─────────┬────────────┘
           │ dirty areas (global list, merged)
           ▼
 ┌──────────────────────┐
 │  COMPOSITOR          │  1 frame per cycle, in the interface task
 │  (kernel/quadro)     │  for each dirty area → slices in internal SRAM
 └─────────┬────────────┘
           │ finished slice (SRAM) → copy via DMA2D/PPA
           ▼
 ┌──────────────────────┐
 │  FRAME HAL           │  back page ← slices
 │  (hal/tab5_quadro)   │  flip at the frame boundary
 └─────────┬────────────┘  waits for on_frame_buf_complete
           │               syncs the dirty areas into the page that left
           ▼
        DSI panel (reads the front page, with priority on AXI)
```

### 4.1 Render in SRAM, not in PSRAM

This is the bandwidth gain that ends the fight against the blue frame. Today a
message bubble writes to PSRAM three times: the background, the bubble on top
and the text on top of the bubble. With slices:

- the compositor borrows a **slice** of internal SRAM (e.g. 720 × 32 lines ×
  2 B = 46 KB; two slices to alternate);
- it draws into it everything that falls within that strip of the dirty area,
  in layer order (overlap happens in SRAM, which is fast and does not compete
  with the panel);
- it sends the finished slice to the back page **once**, via DMA2D, while
  the CPU is already drawing the next slice in the other one.

PSRAM then sees each final pixel **once**, in well-behaved DMA bursts,
instead of N CPU writes. Add AXI QoS, and the strips, the
breathers and `tab5_tela_gastou` no longer have any reason to exist.

The rasteriser is still M5GFX: each slice is an `LGFX_Sprite` with
`setBuffer()` pointing into SRAM (checked: `LGFX_Sprite.hpp:155`). The
drawing context carries the slice offset, and the objects draw
in their own coordinates without knowing it.

### 4.2 The object tree (retained mode)

Today the app is "immediate": the system calls `desenhar()` (draw) and the app
paints everything, keeping the drawing rules in its head. In the new foundation
the app **declares** the screen once (`ao_abrir`, on open) and afterwards only
**changes properties**:

```c
ui_obj_t* tela  = ui_tela_do_app();
ui_obj_t* bat   = ui_rotulo(tela, 16, 16, "Battery: --");
ui_obj_t* lista = ui_lista(tela, ...);         // scrolling, inertia, clipping: the system's job

// when the data changes (in the interface task):
ui_rotulo_texto(bat, "Battery: 87%");            // invalidates only the label's rectangle
```

Each object knows its own area, so invalidation is automatic. Style
(colour, font, radius, border) belongs to the object, not global state: rule
no. 2 disappears. Fixed width and painting by difference disappear: the
compositor redraws the whole rectangle in SRAM, with background, without
flickering.

Planned objects: `tela` (screen), `painel` (rectangle with radius and border),
`rotulo` (label), `botao` (button), `imagem` (PNG/emoji), `lista` (scrollable and
**virtualised**: only the visible items exist — essential for the Mesh
history), `canvas` (free drawing for Azimutal, Camera and Cobra).

### 4.3 Input

Touch goes to the system, which finds the object under the finger and delivers
events (pressed, released, clicked, dragged, long press). The list handles
dragging by itself (threshold, inertia on release, braking at the ends) and
swallows the click of whoever was scrolling. The per-app
`ao_tocar`/`ao_tocar_nivel`/`rolagem_foi_arraste` goes away.

### 4.4 Power

With no dirty area, there is no frame: the interface task sleeps on the event
queue as it does today. The frame loop only runs at 60 Hz while there is
animation or a finger on the screen; a clock that changes once a minute costs
one rectangle a minute. The per-app power profiles do not change.

### 4.5 Migration without a big bang

An app not yet ported runs inside a `canvas` object with its own `M5Canvas`
in PSRAM: the old `desenhar()` paints into it as always, and the
compositor copies only the dirty part. That keeps the whole system working
during the changeover, and it is the path for Cobra (the boundary with the game
tab: the app contract changes on this side, the game ports whenever its tab
wants to).

---

## 5. Stages

Each stage builds, is flashed and is tested on the device before the next.

| # | stage | delivers | how we know it worked |
|---|---|---|---|
| 0 | **measure** | underrun counter, frame time, free SRAM | baseline numbers to compare against |
| 1 | **frame HAL** | `hal/tab5_quadro`: AXI QoS, flip at the boundary, `on_frame_buf_complete`, sync areas, copy via PPA | test screen from the console: animated rectangle + synthetic scrolling, zero underrun, zero tearing |
| 2 | **compositor** | dirty-area list, merging, slices in SRAM, frame loop | the same test screen going through the compositor |
| 3 | **objects + input** | tree, styles, `rotulo/botao/painel/imagem/lista/canvas`, touch, list with inertia; smooth fonts at the exact size | home and status bar rewritten on the new foundation |
| 4 | **Mesh** | messages as a virtualised `lista` of bubbles | the scrolling that distorts today, smooth, with no screen code in the app |
| 5 | **other apps** | one at a time; unported ones run in the bridge `canvas` | each app tested on its own |
| 6 | **delete the legacy** | `pintura.*`, the strips/breathers of `tab5_tela`, `rolagem_deslocar`, `REPINTURA_MIN_MS`, drawing rules no. 1–4 | the apps' screen code becomes pure declaration |

---

## 6. Open questions (to be measured, not guessed)

- **Free internal SRAM** with WiFi/esp_hosted on: decides the slice
  size (stage 0).
- **QoS level** of the DW_GDMA that eliminates underrun without starving the CPU
  (stage 1).
- **PPA for the slice → page copy**, or `draw_bitmap` with `use_dma2d`: the
  second is simpler, the first allows filling and blending in
  hardware. Decide by measuring (stage 1).
- **Fonts**: smooth VLW (which M5GFX already loads) generated at the sizes in
  use by a script, as `gerar_emoji.py` already does for the emoji (stage 3).

---

## 7. Diary

**Stages 0 and 1 in the code, awaiting testing on the device.**
- `hal/tab5_quadro.{hpp,cpp}`: DW-GDMA QoS on AXI (on/off), end-of-frame
  notification (`on_frame_buf_complete`), rectangle copy between pages
  via the PPA, underrun counter (a watcher on channel 2 of the ROM output, which
  boot leaves free — it counts without replacing the console).
- `hal/tab5_tela`: `tab5_tela_igualar_paginas()`, `tab5_tela_apresentar()`
  (syncs the cache only for the areas → requests the swap → waits for the end
  of the frame → copies the areas into the page that left),
  `tab5_tela_escrever_na_frente()`.
- `app_quadro` (throwaway): the same animation in LEGACY and NEW, with QoS
  ON/OFF, measuring fps, draw time, swap time, worst frame and
  underrun.

**Result of stage 1 on the device.**
- Synchronised swap **works**: "sem troca: 0" (no swap: 0) in every NEW run.
- Drawing the whole window straight into PSRAM costs **~34 ms** (legacy and new
  the same): a ceiling of ~29 fps. That is the argument for stage 2.
- NEW at 16–17 fps because it waited for the swap right after requesting it and
  copied the whole window that the next frame would redraw anyway. Fixed in
  stage 2 (wait and sync deferred to the start of the next frame).
- **Maximum QoS on the DW-GDMA makes it WORSE**: with the panel at 79 MHz the
  screen flashed blue non-stop (observed in use). It stays OFF; it does not go
  into the foundation until it is understood and measured. The 35 Hz in one run
  was the panel clock lowered in Settings, not a defect.
- Pending: confirm whether the underrun watcher really counts (self-test at
  boot) — it showed 0 during what may have been the blue run.

**Stage 2 in the code (`kernel/compositor.{hpp,cpp}`).** List of
dirty areas with merging (LVGL's rule), two slices in internal SRAM, assembly
with M5GFX in screen coordinates (sprite with a pulled-back pointer), slice →
page copy via the asynchronous PPA, deferred sync that skips what is going to
be redrawn, swap requested without waiting. Test: COMP ROLAR / COMP QUADRADO
(scroll / square).

**Fixes after the blue flashes.**
- The test app's "QoS" DUPLICATED `hal/tab5_barramento` (already on at
  boot: arbitration 8 + ARQOS 8). The test's "QoS OFF" = a mixed state; "QoS ON"
  = ARQOS 15, which flashed blue non-stop. Duplicate removed; the test button
  now really switches the original module on/off.
- L2 cache line: 64B → 128B (see the notes on the causes of the blue). The main
  suspect for the lack of bandwidth.
- Panel clock limited to 24..80 MHz (Settings and the console clamp instead of
  refusing); the step of 5 now reaches 80.

**Stages 1–2 validated on the device; stage 3a (the bridge) in the code.**
Test result: COMP ROLAR "very smooth, no tearing or fragments";
COMP QUADRADO at 63.5 fps (the panel's ceiling) with 2.4 ms; no blue at 80 MHz.

Stage 3a — **the whole system goes through the compositor**:
- `kernel/tela_virtual`: a 720×1280 canvas in PSRAM that the panel never reads,
  with a **tracking panel** (a subclass of `Panel_Sprite`) that records every
  rectangle touched by any M5GFX primitive, already converted to
  physical coordinates (the same rotation arithmetic as `Panel_Sprite`, so
  Mesh in landscape works).
- The kernel (home, bars, apps) draws into the virtual screen; at the end of
  each loop pass, `comp_quadro_imagem()` takes only the recorded areas to the
  back page via the PPA and requests the swap. On waking: whole screen + wait
  for the swap before turning the backlight on.
- `tab5_tela_limpar/rolar` write to the target (virtual screen) and **with no
  breather**.
- Panel recreated (idle, clock) → the compositor notices from the `tab5_tela`
  generation and invalidates the whole screen.
- Azimutal: `pushSprite(px,py)` wrote into the canvas's PARENT (M5.Display) —
  it went around the side; now `pushSprite(&d, ...)`, without slicing.
- Frame app: only ROLAR / QUADRADO / QoS.

Legacy left without a job but still present (goes in stage 6): the old double
buffer (`para_tras/mostrar`), `REPINTURA_MIN_MS`, `rolagem_deslocar`,
`pintura_*`, "already cleared" marks.

**Stage 3a validated; legacy
clean-up; first standard component.**
- `hal/tab5_tela` 811 → 275 lines: out went the old double buffer, strips,
  breathers, `gastou` (now a no-op only because of Cobra), cache
  synchronisation in strips. `REPINTURA_MIN_MS` and `rolagem_em_arraste` left
  the kernel; the 70 ms "settling" on wake left `tab5_power`; `comp_encerrar`
  left the compositor.
- **`kernel/lista`**: standard scrollable list (cached height per item, exposed
  strip, scroll bar, compensation for a new item in a list anchored at the end).
- **Mesh/messages** migrated to the list, with a **bubble cache**
  (`construir_layout`): each message is measured once per history load,
  not on every scroll step (before: line wrapping, emoji, 2×gmtime and the
  service mutex, for ALL messages, every frame). Dragging no longer
  forces the whole window.
- Pending in Mesh: Chats and Nodes still on the old scrolling (migrate to the
  list); a new message still triggers a full repaint of the whole app.

**Fixes + audit.**
- Mesh: the send time was being saved as 0 (it became 31/12/1969 in the -3
  time zone); now the system UTC when valid (`MESH_EPOCH_VALIDO`), and a
  message without a date does not open a divider. AVISO (alert) and ENV (send)
  buttons with `TAB5_BOTAO_TEXTO_GRANDE`.
- On-screen keyboard: native 24pt bold font (was 12pt × 1.5 = jagged);
  the gap between keys goes to the nearest key; the key is confirmed on finger
  RELEASE (before, only through M5Unified's click, which discards a touch with
  a drag or a long press), with the click as a safety net for a touch that is
  too quick.
- Structural audit (see the session's reply): the UI wakes at 40–50 Hz even
  when still (M5.update → touch I2C); Mesh reads the history from the SD IN THE
  INTERFACE TASK on every new message; internal I2C can only be used by the UI
  (no mutex in M5Unified) and that forces bridges; apps poll via `periodo_ms`
  instead of receiving an event.

**The four structural fixes + protobuf review.**
1. **Notices** (`sistema_avisar`, `AVISO_*`, `app_t.avisos/ao_aviso/periodo_agora`):
   a service says what changed, the app redraws immediately. Mesh: no period on
   messages (only Nodes/Mesh, which show age, have 1 s).
2. **System worker** (`kernel/trabalho`): slow work outside the
   interface; result delivered by `AVISO_TRABALHO`; closing an app waits for the
   work in progress and discards the queue. Mesh (history and card, with two
   buffers) and Files (line count and page) migrated.
3. **The interface sleeps when still**: finger or pending work → 20 ms with the
   touch interrupt disabled; still → interrupt enabled and sleep until the
   next real deadline. `sistema_pedir_redesenho` from another task wakes the
   interface.
4. **Internal I2C under a single lock**: M5GFX already locks the port per
   transaction; the camera's SCCB (IDF driver) does not.
   `-Wl,--wrap=sccb_new_i2c_io` hands esp_video an SCCB made of M5GFX
   transactions (`hal/tab5_sccb.cpp`). Camera bridge removed. This also covers
   automatic exposure control (IPA/AGC), which wrote to the sensor during
   capture without a lock.
- Keyboard: debounce on release (2 samples without a finger, 80 ms between letters).
- **Protobuf checked against `meshtastic/src/mesh/generated/.../mesh.pb.h`**:
  `MeshPacket.via_mqtt` is field 14 and `hop_start` is 15 (the code read 13 and 14);
  `QueueStatus.mesh_packet_id` is a varint (it was read as fixed32). The rest of
  the decoder and the whole encoder (ToRadio, heartbeat, want_config) match.

**Tasks: measurement became a service.** `services/tab5_monitor`
measures all the time (1 Hz), keeps peaks/minimums with time, a 5 min history
and trigger events; it saves events and a per-minute summary to the card. The
app is only the window and, with the compositor, simply redraws everything
every second.

**Power app reviewed.** The panel reads the kernel's measurement (no 2nd
I2C read), native 18pt/12pt fonts, "HOJE" ("TODAY": the day's file, via the
worker, 0–24h axis, a gap where it was off) in place of the open-ended
history, signed current + zero line, power in W, the day's integrated
consumption (mAh used/charged, Wh). Per-region redraw. Log:
`Sistema/energia/energia-AAAA-MM-DD.csv` (ENE3, real time), batches of 5;
`tarefas.csv` is gone (the monitor covers it). The monitor gained an UNDERRUN
trigger with the app open.

**Review of patches and the list pattern.**
- `kernel/lista` became the ONLY form of scrollable list: it gained touch
  (`lista_arrastar`, `lista_item_tocado` with `LISTA_ARRASTE`), updating
  a single item (`lista_item_mudou`), a breather at the top and `lista_esconder`.
  Migrated: Mesh (messages, chats, nodes), Files, Tasks, Azimutal
  (days) and Trilha (sessions — the ANTERIOR/PRÓXIMA (previous/next) pages are
  gone). No app calls `rolagem_*` directly; `kernel/rolagem` is the engine of
  the list and the kernel.
- Limits that hid the NEW files (readdir returns them in creation
  order): Files 24→512 (sorted, folders first, files newest first),
  Azimutal 24→400 days, Trilha 60→1000 sessions — all in PSRAM.
- Compositor: the slices-in-SRAM path is gone (only the test app used it, and it
  held 90 KB of internal SRAM from boot) and so is the "equalise pages" on every
  new panel (1.8 MB of extra copying in PSRAM at boot and on wake).
  `tab5_quadro_copiar` is the only copy (blocking).
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` + `EXT_RAM_BSS_ATTR` on the
  large statics (Mesh nodes, the log drain, monitor events...):
  ~45 KB less in internal SRAM.
- Removed: the Quadro (Frame) app (the test bench, it served its purpose for
  stage 6), `tab5_power_tela` (turned the panel off outside of idle),
  `notif_esta_silenciada`, `pintura_mudou_f`, `rolagem_item_preservado`.
- Blue flash: the bridge's underrun colour = MAGENTA (diagnostic), the frame
  clock and DSI host errors become monitor events and show up in `display`
  (`tela`).

**Cost audit; steps 0 and 1.**
The pixel still goes through PSRAM 5 times (CPU into the virtual screen, PPA to
the back page, sync in the next frame); scrolling still shifts with the
CPU (`copyRect` on the virtual screen). Plan: 0 measure → 1 pacing + merging →
2 direct mode (draw on the back page, no virtual screen) with scrolling
via the PPA from front to back → 3 Radar without sprites, Tasks by difference →
4 adaptive refresh rate (a measured experiment).
- **Measurement** (`comp_estat`): frames, frame and sync pixels, PPA
  time, swap wait, drawing and the whole loop pass (histogram by
  band: <4, <8, <12, <16, <33, >=33 ms), CPU scrolling. Console:
  `display measure` (`tela medir`) opens the window, `display` shows it.
  Monitor: `quadros-AAAA-MM-DD.csv` per minute (only minutes with a frame).
- **Pacing:** in fast mode the pass that presented waits for the swap and
  starts again immediately, instead of sleeping 20 ms after the work (~30 fps
  with judder against the ~63 Hz panel).
- **Merging:** 32 areas; a full list merges only the pair with the least waste
  (before, it became one bounding rectangle, sometimes the whole screen).
- **First measurement (scrolling, panel at 47.4 Hz):** pass with a frame,
  average 120 ms, 157 of 159 above 33 ms. The PPA copy was 88 ms per frame
  (~32 MB/s in use): the bottleneck, not the shortcut.
- **`display bench`** (`tela banco`, 720x1024, panel scanning): PPA 92 MB/s,
  pure DMA2D (`esp_async_fbcpy`) 288 MB/s, AXI GDMA 206 MB/s, CPU 118 MB/s. The
  copy between pages moved to DMA2D (PPA only as a fallback). Destination
  cache: write back + invalidate, because "invalidate only" requires 128 B
  boundaries.
- **Second measurement (copy via DMA2D):** copy 88 → 11.4 ms per frame, pass
  120 → 40.8 ms. Drawing (26.9 ms, CPU scrolling included) became the biggest
  cost.

**Step 2: direct mode (LVGL's "direct mode" with two pages).**
- The virtual screen is gone (−1.84 MB of PSRAM). The tracker writes to the back
  page; the first write of the pass OPENS the frame (`comp_abrir`: waits for the
  previous swap and syncs front → back), `comp_apresentar` writes back the
  cache of the drawn lines and requests the swap. Page swap only via the
  sprite's pointer (`setBuffer` would reset the window in the middle of
  writing).
- Scrolling: `copyRect` becomes a DMA2D copy from the FRONT (shifted) to the
  back, when the source was not drawn in this frame; before opening, the window
  is promised (`comp_cobrir`) and the sync does not copy it needlessly.
  Otherwise, CPU.
- New panel (boot, clock, wake) = pages with nothing of ours:
  `comp_painel_novo()` makes the core redraw everything. Drawing without a panel
  falls into a bin allocated only if it happens (with a log line).
- Camera: its own DMA surface, allocated on the first request and given back
  when the app closes; taken to the page via DMA2D in `desenhar`.
- Changing the panel clock from the console now runs in the interface task
  (`sistema_trocar_relogio_painel`): destroying the pages from another task in
  the middle of a drawing pass would mean writing to freed memory.
- Pass histogram in PERIODS of the measured panel (1, 2, 3, 4+), not in fixed
  ms. New CSV: `quadros2-AAAA-MM-DD.csv`.
- **Third measurement (direct mode):** pass 40.8 → 23.0 ms, 75% of frames
  within a single scan; drawing 26.9 → 14.0; scrolling 6.9 → 2.2 ms (DMA). But
  the sync became 2/3 of the copying (336 MB against 154 for scrolling): the
  apps draw the header BEFORE the list, the frame opened and synced the
  whole window, and only then did the scrolling overwrite it.
- **Lazy per-area sync:** on opening, the areas of the previous frame
  stay pending; each one comes from the front only when something writes to or
  READS it (`comp_preparar`, called by the tracker, including in `readRect`),
  and the rest when presenting. A DMA copy that covers a whole pending area
  (scrolling, surface) discards it without copying.
- Drawing measured separately: full pass × partial.
- **Fourth measurement (many app openings):** full pass 123.5 ms,
  partial 14.0 ms - the cost is now opening an app. Sync still high: merging
  combines the header and the list window into a single pending rectangle, and
  touching the header brought in the whole window.
- **Clipped pending areas:** only the intersection with what is about to be
  touched comes from the front; the rest stays pending in up to 4 pieces.
  `fillRect` is an opaque write: it discards the pending area it covers instead
  of copying it (clearing the usable area when an app opens no longer copies
  1.6 MB just to paint over it).
- Log `desenho completo de <app>: N ms` (full draw of <app>) above 40 ms, to
  find out who is heavy.
- **Full pass per app** (table in `display`): "limpar" (clear) 20-25 ms in every
  app (CPU writing 1.6 MB of black); "conteúdo" (content) follows the volume of
  text (Camera 6 ms, Files 40, home 57, Mesh 55, Settings 77, Tasks 116).
- **Hardware fill:** `fillRect` of 16 K px or more goes to the PPA's FILL
  engine (write only; the driver writes back + invalidates the lines' cache
  first). Self-check on the first colour with differing bytes; if it does not
  match, it switches off and falls back to the CPU. `display bench` compares
  PPA-fill × CPU-fill.
- **Measurement with PPA-fill:** "limpar" 20-25 → ~8 ms in every app (bench
  run while idle, at 40 MHz: only the ratio counts - PPA-fill 3.6x the CPU).
  Files with a worst case of 2.3 s: the photo (1.4 MB) was read from the card
  line by line INSIDE desenhar(), and reread on every full redraw. Now the
  worker reads it once into a buffer in PSRAM (already in screen order) and
  desenhar() only paints; the buffer is released when going back to the list or
  closing.
- **Measurement with the screen on:** PPA-fill 4.2 ms against CPU-fill 14.1 ms
  (720x1024). Files: worst 2,271 → 92 ms. Average full pass 61 ms;
  what remains is text (Settings 61, ESP-NOW 55, home 54 of "content").
- **Why text is expensive:** M5GFX's `VLWfont::drawChar` does a 1x1
  `writeFillRect` per anti-aliased edge pixel, plus one per background
  run - ~200 calls per 30 px letter, each one going through the tracker.
- **Glyph cache** (`tab5_glifos`, `FonteRapida` in place of `VLWfont`):
  the letter is drawn once by M5GFX itself into an RGB565 scratch buffer and
  kept by (font, letter, colour, background); afterwards, one `pushImage` per
  letter. Only text with a background, size 1, a 16-bit target and a letter
  that does not overlap the previous one - the rest follows the original path.
  512 KB of PSRAM, starts again from scratch if it fills up. Statistics in
  `display`.
- **Page epoch** (`g_tab5_tela_epoca`): each write's path checks "does the page
  still exist?" with a single comparison, instead of querying the
  panel for every pixel.
- **Measurement with a 1 MB arena:** glyph cache 79% (97% not counting spaces,
  which are already cheap); refusals: space 3202, overlap 435, other 75; it did
  not restart. Full pass 40 ms, partial 4.5, loop pass 17.9.
- **Tasks per field (step C):** the window erased the whole area and
  redrew everything on every sample (1 Hz). Now each line of text is a
  field that remembers what is on the screen and only repaints when it changes;
  the table, per row (text signature + `lista_item_mudou`); the graph, its
  interior, once per sample; titles, frame and button only on the full pass.
