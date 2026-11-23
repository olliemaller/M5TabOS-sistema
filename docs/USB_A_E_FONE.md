# USB-A (host) and 3.5 mm headphone jack - analysis before implementing

Survey done on 2026-10-17, before any code. Sources: the
**official Tab5 schematic** (Tab5_Schematics_PDF, Altium, 2025-06-09 - read
page by page), the M5Unified 0.2.21 code we already use, the BSP
`espressif/m5stack_tab5` 1.3.x, the local ESP-IDF 5.5.5 and Espressif's
component registry. What is deduction (and not written in the schematic) is
marked **[to be confirmed in testing]**.

---

## 1. USB-A

### 1.1 The hardware (from the schematic)

| What | How it is connected |
|---|---|
| Data | J10 (USB-A) ← ESD0524P (D21) ← common-mode choke FT2 (SDMM0806H, 90 Ω) ← R113/R114 **33 Ω in series** ← `USB_HOST_DP/DM` ← P4 pins 49/50: **`USB2_OTG_D-/D+`** |
| Controller | the **OTG 2.0 High-Speed (480 Mbit/s)**, with its own UTMI PHY - dedicated pins, not GPIO |
| VBUS | `SYS_INT5V` → **MT9700** (U27, current-limited switch) → `SYS_USB5V` → J10. Enable = `USB5V_EN` (expander 1 / 0x44, bit 3) |
| Current limit | set by R84 = 5.1 kΩ on SET. The MT9700 datasheet only gives the I×R graph; from the curve it is around ~1 A **[to be confirmed by measuring]** |
| Fault signal | **does not exist** - the MT9700 has no flag pin. Overcurrent can only be noticed by the device disappearing from the bus (or via the INA226) |
| USB-C, for comparison | `USB_DEVICE_DP/DM` → GPIO24/25 (`USB1P1_0`), the console's full-speed PHY - the one the pendrive mode switches |

**Direct consequences:**

1. **USB-A is independent of everything that already exists.** A different
   controller, a different PHY, different pins. It does not clash with the
   console (USB-C), nor with the pendrive mode (which uses the *full-speed* OTG
   via USB-C), nor with the speaker's I2S: the GPIO26/27 problem (the "pendrive"
   notes) belongs to the full-speed PHY1, not this one. You can have the SD card
   on the computer **and** a flash drive in the USB-A port at the same time.
2. **Real high speed**, with a caveat: the 33 Ω series resistors + the choke
   sit in the path of a 480 Mbit/s signal (the PHY already has internal 45 Ω
   termination). It should work on a short trace, but it is the first thing to
   measure: enumerate an HS flash drive and check that it came up as HS and did
   not fall back to FS.
3. The IDF default is already right: `usb_host_config_t.peripheral_map = 0` on
   the P4 installs on the **HS** peripheral (BIT0) - the same as Espressif's
   BSP, which passes nothing.
4. **Chip revision.** The IDF's `usb_phy.c` enables the host's 15 kΩ
   pull-downs in software only on the P4 v3 (`!CONFIG_ESP32P4_SELECTS_REV_LESS_V3`);
   on v1 the controller itself takes care of it. We have both sdkconfig variants
   (`chip_v1`/`chip_v3`) - test on both.

### 1.2 The connector's 5 V

We already have the control in `tab5_power` (`perfil.usb_a_5v`,
`M5.Power.setExtOutput(.., m5::ext_USB)`).
Note the discrepancy: our map (taken from M5Unified) says `USB5V_EN`
is an **input with pull** (the real enable is a resistor on the board) and
M5Unified inverts the *pull*; Espressif's BSP programs the same pin as a
**push-pull output**. Both work; we stay with M5Unified, which owns
the expander here.

Rules for the service:
- USB-A 5 V **only on while the host service is active** (it draws on the
  battery even with nothing plugged in: the switch and whatever is in the
  connector).
- With the Tab5 on battery, the budget is that of the internal 5 V converter;
  power-hungry devices (2.5" external HDD, a hub with lots on it) will hit the
  MT9700's limit. A self-powered hub is the way to go for that.

### 1.3 Libraries

| Stack | Status on the P4 | Verdict |
|---|---|---|
| **IDF USB Host Library** (`components/usb`, already in IDF 5.5.5) | HS on the P4, optional hubs (`CONFIG_USB_HOST_HUBS_SUPPORTED`, off today), multi-hub | **use this one** |
| `espressif/usb` 1.6.0 (the same library, as a component) | requires IDF ≥ 5.5.3 (we have 5.5.5). Brings what the built-in one lacks: **suspends the port before light sleep** (1.5.0), `DEV_REMOVED` event, `fsls_only` (force FS if HS has signal problems, 1.6.0), dual host on the P4 | switch to it **if** light sleep or the HS signal calls for it |
| TinyUSB (`espressif/tinyusb` 0.21) | "Only Device part ... is supported" - no host | not suitable (stays for pendrive mode only) |
| CherryUSB 1.6.1 | HS host, hubs, many classes | a second USB stack in the firmware, with no gain Espressif's does not give; discarded |

Class drivers (all from Espressif, all with P4 support; on IDF 5.x they use the built-in `usb`):

| Component | Version | What for in M5tabOS |
|---|---|---|
| `usb_host_msc` | 1.3.0 | **flash drive on USB-A** → mounted in VFS/FAT (`/usb`), Files and Music read from it. Only transparent BOT/SCSI (every ordinary flash drive). Block 512-4096 |
| `usb_host_hid` | 1.2.1 | **keyboard and mouse**. Delivers the raw report (+ shortcuts for Boot Protocol). The layout (ABNT2) is ours - it slots into `hal/tab5_teclado` |
| `usb_host_cdc_acm` + `usb_host_vcp` (CH34x 2.2.1, CP210x 2.2.0, FTDI 2.1.1) | 2.4.1 | USB serial: Meshtastic node/radio on the cable, USB GPS, Arduino |
| `usb_host_uac` | 1.5.0 | USB headset/DAC. **UAC 1.0 only** (most cheap headsets and adaptors are UAC1; "hi-fi" UAC2 DACs do not work) |
| `usb_host_uvc` | 2.6.0 (we have 2.4.2 indirectly, via esp_video) | webcam. `esp_video` already has the path ready: `CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE` becomes a `/dev/videoN` for the Camera app |

Versions: pin with `==` in `main/idf_component.yml`, like the rest (see the
warning at the top of it: clean build when changing a component).

### 1.4 How to put it together for the best performance

Architecture (house rule: engine in `services/`, apps only interface, one Berry module per service):

```
services/tab5_usb_host.{hpp,cpp}   engine: install/uninstall, usb_lib task,
                                   connection events, 5 V, power
  ├─ MSC class   → mounts /usb, notifies "flash drive connected" (notificacao)
  ├─ HID class   → injects into tab5_teclado / pointer
  └─ CDC class   → byte stream for whoever asks (Mesh, GPS)
apps (Files, Music, Settings) and Berry 'usb' → only ask and display
```

Performance and robustness points:

1. **Tasks.** One `usb_lib` task (`usb_host_lib_handle_events`) + the class
   driver's. Priority above the UI, below audio. Pin the core according to
   `reserva_nucleo` (do not compete with the screen's core).
2. **Memory.** The DWC's DMA buffers sit in internal RAM by default - and
   internal RAM is the tightest resource (see the RAM audit). There is
   `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` (P4 only) with a "small
   performance loss". Measure both; the most likely outcome is moving to PSRAM,
   as esp_hosted did.
3. **Reading files (MSC).** The lesson from the SD card applies here:
   POSIX `read()` with large blocks, not 4 KB `fread`. The component itself
   says the per-file cache (`setvbuf`) stops helping above
   16 KB. Measure HS MB/s with the same `ensaio` (trial) as the SD.
4. **FIFO.** `CONFIG_USB_HOST_HW_BUFFER_BIAS_*`: *Balanced* (current) for MSC;
   *Bias IN* if the main use is a webcam/audio input.
5. **Hot-plug.** Pulling the flash drive with a file open: the service has to
   unmount the VFS and notify the apps (just like "the card went to the
   computer" in pendrive mode/Music).
6. **Power/light sleep.** With the host active, light sleep takes the bus down
   (the same kind of problem as the Mesh UART). Either the service holds a PM
   lock while a device is connected, or we use `espressif/usb` ≥ 1.5, which
   suspends the port by itself. With nothing connected: uninstall the host and
   cut the 5 V.
7. **Hub.** Enabling `CONFIG_USB_HOST_HUBS_SUPPORTED` (costs a little RAM)
   allows keyboard + flash drive together. MSC supports up to 4 drives on the P4.

Suggested order: **MSC → HID → CDC/VCP → UVC → UAC**.

---

## 2. 3.5 mm headphone jack

### 2.1 The hardware (from the schematic)

```
ES8388 LOUT1 ─ 22µF ─ HPOUT_L ─ 22Ω ─ J5 pin 6 (L)    ──┐
ES8388 ROUT1 ─ 22µF ─ HPOUT_R ─ 22Ω ─ J5 pin 3 (R)      │ PJ-342, 4 poles (TRRS)
AUDIO_VDD ─ 3.3k ─ HPMIC ─────────── J5 pin 5 (mic)    │
                     └─ 1µF ─ ES7210 MIC4P             │
AUDIO_VDD ─ 100k ─┬─ J5 pin 7 (L switch contact)       │
                  └─ 10k ─ HP_DET → expander 0, bit 7 ──┘
HPOUT_L ─ 0Ω ─ AEC_P ─ 1µF ─ ES7210 MIC3P   (reference for echo cancellation)
HPOUT_L ─ 100nF/68k ─ NS4150B (U15) ─ speaker;  SPK_EN = CTRL (expander 0, bit 1)
ES8388 LOUT2/ROUT2: not connected
```

What this means:

1. **Headphones and speaker come out of the same pair of pins (LOUT1/ROUT1).**
   There is no hardware switching: plugging in headphones does **not** mute the
   speaker. Muting is the software's job, switching off `SPK_EN` when `HP_DET`
   reports the plug.
2. **The speaker is mono and only gets the LEFT channel.** Today M5Unified is
   set to `stereo = false` (the same signal on both channels), so nothing is
   lost. When switching to stereo for headphones, **with the speaker active
   (L+R)/2 must be mixed into the left** - otherwise the speaker loses everything
   that only exists on the right.
3. **`HP_DET`** - with no plug the pin 7 switch touches L (5.1k to
   ground) and the signal is low (~0.16 V); with a plug the switch opens and the
   100k pulls it up to 3.3 V. So **high = headphones plugged in [to be confirmed
   in testing]**. The expander's `nINT` does not go to the P4: it is read by
   polling (I2C, in the interface task, which owns the bus - e.g. every
   300-500 ms, only with sound active or the screen on).
4. **The jack has a headset microphone** (CTIA, biased via 3.3k) on the
   ES7210's **MIC4**. MIC1/MIC2 are the board's two microphones, **MIC3 is a copy
   of the left output** (AEC reference). Only the ES7210's SDOUT1 goes to the
   P4 - to read MIC3/MIC4 the ES7210 has to switch to 4-channel TDM. M5Unified
   today reads only MIC1/2 and leaves MIC3/4 at gain 0.
5. **Bass.** The 22 µF coupling capacitor + 22 Ω form a high-pass filter
   with the headphones: ~134 Hz for 32 Ω headphones, ~190 Hz for 16 Ω. Ordinary
   earphones will sound **bass-less**. It is not a defect in our code; it can be
   compensated with a *low-shelf* (biquad) in software, only for headphones (the
   speaker cannot reproduce bass and would distort). Into a line input
   (amplifier, active speaker ~10 kΩ) the cut-off drops to ~1 Hz - full sound.
6. **LOUT2/ROUT2 are not connected**, but M5Unified turns all four on
   (`reg 4 = 0x3C`). `0x30` turns on only LOUT1/ROUT1 - a small saving, but
   correct.

### 2.2 Libraries

| Option | Pros | Cons |
|---|---|---|
| **M5Unified `Speaker`** (today) | already integrated (beeps, Music streaming, switching off idle I2S, handing pins over to pendrive mode); channel mixer | `sample_rate` fixed at 48 kHz with its own linear resampling (MP3 is 44.1 kHz → everything is resampled); `magnification = 4` (×4 digital gain → clipping on loud music); software volume (loses resolution); knows nothing about headphones |
| **`esp_codec_dev` 1.6.2** + the IDF `i2s_std` I2S driver | volume **in the codec** (0.5 dB per step, without losing bits), native sample rate per file, the same path as the official BSP | means changing the owner of I2S0 and the ES8388 - the microphone (ES7210) shares the same I2S bus (common clock), so it would have to migrate too; redoing what `sistema_som_*` already solves. LOUT1/2 selection and headphone detection are **not** in the driver |
| ESP-ADF | complete pipeline | heavy, brings a lot we already have (decoder, SD) |

**Recommendation: stay on M5Unified and fix things on top of it, in phases.**
`sistema_som_*` in `kernel/core.cpp` already isolates everyone from M5Unified -
switching engines, if it ever happens, stays hidden there.

### 2.3 How to put it together for the best performance

1. **Detection (`HP_DET`) in the system**, not in an app: the kernel reads bit 7
   of expander 0, generates the "headphones plugged/unplugged" event and:
   - plugged → ramp the volume down, switch `SPK_EN` off, apply the headphone
     volume, stereo;
   - unplugged → **pause Music** (the behaviour every phone has), back to the
     speaker volume, mono, `SPK_EN` on.
2. **Two volumes stored in the settings** (speaker and headphones), applied
   **in the ES8388** (registers 46/47, `LOUT1VOL/ROUT1VOL`) and not in the PCM -
   the PCM stays at gain 1 and keeps its 16 bits. Low initial headphone volume,
   with a ceiling (hearing safety; today M5Unified leaves the output at +4.5 dB,
   `0x21`).
3. **Stereo and native rate for headphones.** With headphones:
   `spk_cfg.stereo = true` and `sample_rate` = the file's (44 100) when Music
   opens the stream - eliminating resampling. System beeps keep working (the
   M5Unified mixer converts the rest). Reopening the I2S for this change only
   between tracks, never in the middle.
4. **Digital gain.** Revisit `magnification = 4` for the music stream (it is gain
   meant for the small speaker); on headphones, 1.
5. **Bass on headphones**: *low-shelf* biquad (~120 Hz, +6 to +9 dB) in the Music
   loop, only with headphones, with *headroom* (drop 3-6 dB first so as not to
   clip). Negligible cost on the P4.
6. **Headset microphone**: a later phase - ES7210 in TDM, select MIC4
   when `HP_DET` reports (and the MIC3 reference opens the way to
   echo cancellation with esp-sr, if there is ever calling/voice).
7. **Pops**: the ES8388 already has "click free" on (`reg 28 = 0x08`); switching
   `SPK_EN` has to happen with the DAC muted (M5Unified itself does it that way
   in `end()`).
8. **No conflict with USB-A.** The I2S pins (GPIO26/27) only conflict with
   the full-speed PHY (pendrive mode) - already handled by
   `sistema_som_ceder_pinos`.

---

## 3. What has already been done (phases 0 and 1 - 2026-10-17)

- **FAT in UTF-8** (`CONFIG_FATFS_API_ENCODING_UTF_8`): names arrive and
  open as the computer wrote them ("Canção.mp3", not `CANCAO~1.MP3`). Music
  understands old `.m3u` files in CP437.
- **`services/tab5_volumes`**: the disks by absolute path (`/sdcard`,
  `/usb`) - mount/unmount, available, version, `AVISO_VOLUMES`.
- **`services/tab5_usb`**: HS host (`peripheral_map = 0`), `usb_host_msc`
  1.3.0 pinned, flash drive at `/usb`, removal with up to 3 s for readers to
  let go, "USB-A" notice in the status bar (opens Music), light sleep held off
  (`ECONOMIA_SEGURA_USB`) with a device plugged in, DMA buffers in PSRAM.
  Setting `usb_a` (on by default) → `usb_a_5v` in the profile (kernel/core.cpp).
- **Music**: the flash drive at the top of Folders; `/usb/...` paths in listing,
  playing, duration, cover art (cached on the card), playlists and Favourites.
  The loop holds only the disk the queue is using.
- **Berry**: `musica.pendrive()`, `musica.lista("/usb")`, `# avisos: volumes`.
- **Console**: `usb`, `usb read <file>` (`usb ler`), `usb space` (`usb espaco`),
  `usb on|off` (`usb ligar|desligar`).

Left for the next phase: the file service (copy/move/delete with
trash, clipboard, "open with" by extension) and the Files app
browsing both disks - and then "copy music from the flash drive to the
card" falls out of the same operation.

## 4. Test plan (when the Tab5 is back on the cable)

**Flash drive playing (phase 1)**
- [ ] Boot with the flash drive already plugged in: `usb` shows the device, the
      speed and the mounted disk; the status bar shows "Pendrive <nome>" (USB
      drive <name>).
- [ ] Plug it in with the Tab5 on (screen on and idle).
- [ ] Music > Folders: the flash drive row at the top; enter, subfolders, back.
- [ ] Play from the flash drive; the cover art (tag and the folder's cover.jpg);
      duration.
- [ ] Yank it out while playing: the music stops within ~3 s, the list goes back
      to the root, nothing locks up (`usb` with 0 holding).
- [ ] Name with an accent ("Canção.mp3") on the card and on the flash drive.
- [ ] Favourite a track from the flash drive; remove the flash drive; the
      playlist skips the track; put it back, it plays.
- [ ] exFAT flash drive (64 GB as shipped): mounts and plays. NTFS flash drive:
      notice "use FAT32/exFAT", nothing locks up.
- [ ] exFAT SD card (64 GB SDXC): mounts at boot; pendrive mode keeps working.
- [ ] `usb read` on a large MP3: KB/s with DMA in PSRAM.
- [ ] Pendrive mode (card on the computer) while playing from USB-A: the sound
      carries on.
- [ ] Idle consumption with and without the flash drive (`power trial` /
      `energia ensaio`).

**Headphones and USB-A in general**

Measurements before writing the service - each one answers a question above.

**Headphones**
- [ ] Read bit 7 of expander 0 with and without a plug → confirms the `HP_DET` polarity.
- [ ] 3-pole (TRS) and 4-pole (TRRS) plug: does `HP_DET` detect both?
- [ ] Tone only on L, only on R → confirms stereo on headphones and that the speaker only plays L.
- [ ] `SPK_EN` off with headphones: speaker silent, headphones playing.
- [ ] 40 Hz-2 kHz sweep on headphones: check the estimated bass loss.
- [ ] Volume in the ES8388 (regs 46/47): usable range and noise floor at low volume.

**USB-A**
- [ ] 5 V at the connector with `usb_a_5v` on/off (and idle current via the INA226).
- [ ] HS flash drive: does it enumerate as **high speed**? (if it falls back to FS, look at the signal/`fsls_only`).
- [ ] Read and write MB/s, DMA buffers in internal RAM × PSRAM.
- [ ] USB keyboard and mouse (boot protocol); simple hub.
- [ ] Flash drive on USB-A **together** with pendrive mode on USB-C.
- [ ] Overcurrent: a power-hungry device → what the MT9700 does and how the host notices.
- [ ] Repeat the basics with the chip_v1 and chip_v3 `sdkconfig`.

## References

- Schematic: https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf (p. 1: P4 and expanders; p. 3: audio; p. 4: USB connectors; p. 5: MT9700)
- Tab5 page: https://docs.m5stack.com/en/core/Tab5
- Espressif BSP: https://components.espressif.com/components/espressif/m5stack_tab5
- USB host classes: https://github.com/espressif/esp-usb/tree/master/host/class
- `espressif/usb`: https://components.espressif.com/components/espressif/usb
- `esp_codec_dev`: https://components.espressif.com/components/espressif/esp_codec_dev
