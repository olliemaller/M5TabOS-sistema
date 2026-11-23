# M5tabOS

An app-based operating system for the **M5Stack Tab5** (ESP32-P4 + ESP32-C6),
written in C++ on **ESP-IDF 5.5** and **FreeRTOS**, with its own interface on
M5GFX.

> This is an English fork of [cparadaa/M5TabOS-sistema](https://github.com/cparadaa/M5TabOS-sistema).
> The interface starts in English (Portuguese is still one tap away in
> Settings > Language), the documentation is in English, and the fork adds
> its own fixes and features - see [CHANGELOG.md](CHANGELOG.md). Code
> comments, logs and docs are in English; code identifiers and the Berry API
> keep their Portuguese names, as upstream.

Current version: see [version.txt](version.txt) and [CHANGELOG.md](CHANGELOG.md).

## What it has

**System**
- Home screen with icons, a status bar (Wi-Fi, Bluetooth, ESP-NOW, battery,
  alerts) and a navigation bar with the time.
- Double-buffered screen compositor: no tearing, no flicker.
- Standard components: scrolling list, chat component (bubbles, on-screen
  keyboard, scrolling), buttons and smooth fonts (Roboto at native sizes,
  with accents).
- Its own touch recognition, a touch watchdog and touch diagnostics on the
  console.
- Power: screen sleep, per-app profiles (each app declares the hardware it
  uses), battery measured by the built-in INA226.
- SD card: a single batched system writer for every app; daily logs on the
  card.
- Multitasking: apps with a background part (they keep going after the window
  closes), a system worker for slow reads, and an owner record for every task.
- Time zones by **region with automatic daylight saving** (Melbourne, Sydney,
  London, New York...) or a fixed offset, and a **12/24-hour clock**
  (Settings > Date and time, or the `timezone` console command).
- Serial console and Terminal app with the usual Linux commands (`help`) and
  the system ones (`help sistema`), in English and Portuguese
  (`status` = `estado`, `wifi scan` = `wifi varrer`).
- **PC access**: from a browser on the same Wi-Fi, with a password shown on
  the Tab5 - Terminal, Berry script editor, Camera, Mesh, and a **screenshot**
  of the Tab5 screen.

**Radio**
- **Wi-Fi** through the ESP32-C6 (esp_hosted, SDIO).
- **Bluetooth LE**: the NimBLE stack runs on the P4 and the controller is the
  C6's.
- **ESP-NOW**: runs on the C6, with its own firmware (folder `c6/`).
- **Meshtastic**: talks to a node (for example the Unit C6L) **over the Grove
  cable or Bluetooth**, or to the mesh **over the internet** (MQTT). With no
  server chosen, the internet mode now picks the server for your region from
  the time zone (Australia and New Zealand go to `msh/ANZ`).

**System apps**

| App | What for |
|---|---|
| Mesh | Meshtastic messages: chats, nodes, mesh status |
| ESP-NOW | Direct chat between ESP32 devices |
| Camera | Live view and BMP photos on the card |
| Files | Browse the card: text, CSV and images; a trash bin per disk |
| Music | Plays MP3, WAV and M4A from the `Music` folder of the card, with subfolders, in the background; animated spectrum; mini-player in the notification centre |
| Video | MJPEG and phone MP4 (H.264 + AAC) |
| Power | Battery, consumption and daily graphs |
| Tasks | FreeRTOS tasks, CPU and memory, grouped by owner |
| IMU | Accelerometer and gyroscope |
| Settings | Brightness, volume, Wi-Fi, Bluetooth, ESP-NOW, Meshtastic, display, time zone and more |
| Example | One card for each system API: the template for writing an app |

Any `main/app_<name>/` folder becomes an app on the home screen, with no
registration anywhere: your app lives next to these.

## Hardware

- **M5Stack Tab5**, with any of the three screens it has shipped with
  (ILI9881C + GT911, ST7123, ST7121). M5GFX detects it and the system adapts;
  the ST71xx screens have not been tested on a device yet (see *Known
  limitations*).
- **ESP32-P4 revision**: v1.x (the Tab5s sold so far) and v3.x need different
  binaries. `./gravar.sh` reads the device revision and builds the right one;
  by hand it is `idf.py build` (v1.x) or `idf.py -B build_v3 -DM5_CHIP=v3 build`
  (v3.x). The options for each are in `sdkconfig.defaults.chip_v1` and `_v3`.
- Optional:
  - microSD card in FAT32 or exFAT;
  - ExtPort1 keyboard;
  - GPS or Meshtastic node on Grove.

## Build and flash

1. Install **ESP-IDF 5.5** (5.5.5 is the tested version;
   [official guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/)).
2. Prepare the terminal and build:

   ```bash
   source ambiente.sh
   idf.py build
   ```

   `ambiente.sh` loads `$IDF_PATH/export.sh` (default `~/esp/esp-idf`) and sets
   `ESP_IDF_VERSION=5.5`, which the Wi-Fi component needs. For a non-standard
   setup, create `ambiente.local.sh`; it is not committed.

3. Flash with the Tab5 connected over USB-C:

   ```bash
   ./gravar.sh
   ```

   The script finds the port by itself (`/dev/cu.usbmodem*` on macOS,
   `/dev/ttyACM*` on Linux) and opens the serial monitor. To pick another
   port: `PORTA=/dev/xxx ./gravar.sh`.

### ESP32-C6 firmware (optional)

**The system works with the factory radio, without updating anything.** Wi-Fi,
internet, Meshtastic over Grove or the internet and every app run as they
are. Updating the C6 only adds three things:

| Feature | Factory radio | Updated radio |
|---|---|---|
| Wi-Fi and internet | yes | yes |
| Meshtastic over Grove or the internet | yes | yes |
| ESP-NOW | no | yes |
| Bluetooth (and Meshtastic over Bluetooth) | no | yes |
| Radio sniffing and CSI | no | yes |

Without the update, the screens for those features say "factory C6 radio:
optional update", and everything else carries on.

For those who want the three features: the firmware is in the `c6/` folder
(the same esp_hosted version as the system, 3.0.10). The C6 has no USB of its
own, so it is flashed through the board's programming connector with a serial
programmer (for example the M5Stack ESP32 Downloader):

1. **Tab5 switched off**; the programmer powers the C6.
2. Programmer connected to the computer through **USB-A**. With a USB-C to
   USB-C cable the programmer gets no power.
3. First of all, **back up the factory firmware**:

   ```bash
   esptool.py -p /dev/xxx read_flash 0 0x400000 backup_c6.bin
   ```

4. Flash:

   ```bash
   c6/gravar.sh
   ```

   The script finds the programmer's port by itself (`/dev/cu.usbserial-*` on
   macOS, `/dev/ttyUSB*` on Linux). To pick another: `PORTA=/dev/xxx c6/gravar.sh`.

5. Switch the Tab5 on. The boot log should show
   `host=3.0.10 coprocessor=3.0.10 (match)`.

To go back to the original, flash the backup with
`esptool.py write_flash 0 backup_c6.bin`, or use M5Stack's official tool.

What runs on the C6 (sniffing, CSI, FTM, 802.15.4), who decides its Wi-Fi
configuration and the checklist after flashing are in
[docs/RADIO_C6.md](docs/RADIO_C6.md).

## First use

On a new Tab5 (or one flashed with M5Burner) the system opens a short
assistant: language, time zone (pick your region for automatic daylight
saving), the Meshtastic mesh over the internet and Wi-Fi. Everything can be
changed later in Settings. To see the assistant again: the `welcome` console
command.

- **Wi-Fi:** in Settings > Wi-Fi, tap SEARCH NETWORKS and choose the
  network. The password is asked for on screen and stored **encrypted** on the
  card (`apps/Sistema/wifi.txt`), with a key derived from each device's chip.
  Several networks fit: at power-on the system connects to the known one with
  the strongest signal. You can also write the file by hand (pairs of lines:
  network and password, `-` for an open network; the password is encrypted at
  the next boot) or use the console: `wifi <network> <password>`.
- **Settings:** turn on Wi-Fi, Bluetooth and Grove 5 V, and choose how to talk
  to the Meshtastic node.
- **Meshtastic over Bluetooth:** in Settings > Meshtastic, choose BLUETOOTH.
  - On the first connection the node asks for the **PIN**. The tested default
    is 123456, the one for devices without a screen.
  - If the node refuses, the keyboard opens by itself to type the PIN it shows.
- **Time zone:** Settings > Date and time. Pick a region and daylight saving
  changes by itself; the manual offset is a fixed UTC offset in 30-minute
  steps. The same page switches between the 24-hour and 12-hour clock. On the
  console: `timezone list`, `timezone <n>`, `timezone 12h`.

## Writing apps

The API reference (screen, touch, tasks, card, notifications, power, sensors,
Wi-Fi, ESP-NOW, Meshtastic, camera, console) is in [docs/API.md](docs/API.md);
the guide to designing an app, with the mistakes already made, is in
[docs/ESCREVENDO_APPS.md](docs/ESCREVENDO_APPS.md). To create an app:

1. copy `main/app_exemplo/` to `main/app_<name>/`;
2. replace `APP_EXEMPLO` with `APP_<NAME>` (the name follows the folder);
3. declare its power profile and delete the cards you do not use.

An app can also answer serial console commands (`app <app> <command>`) by
declaring `.comandos` in its `app_t`.

Every visible string goes through `tr("...")` (the key is the Portuguese
text, gettext style); `ferramentas/idiomas.py` collects them into
`idiomas/M5tabOS.pot` and updates `idiomas/en.po`, which is built into the
firmware. Any other `.po` copied to `/sdcard/apps/Sistema/idiomas/` appears in
Settings > Language at the next boot.

Apps can also be written in **Berry** scripts, without compiling: see
[docs/BERRY.md](docs/BERRY.md) and [docs/BERRY_API.md](docs/BERRY_API.md),
and the examples in `berry_exemplos/`.

The kernel architecture is in [docs/ARQUITETURA.md](docs/ARQUITETURA.md), and
the screen base in [docs/COMPOSITOR.md](docs/COMPOSITOR.md).

## Layout

```
main/kernel/      core: UI loop, compositor, components (list, chat, keyboard)
main/hal/         hardware: screen, touch, power, card, Wi-Fi, Bluetooth, GPS, camera, IMU
main/services/    services: settings, notifications, log, Meshtastic, ESP-NOW, time zones, music player
main/app_*/       apps
c6/               ESP32-C6 coprocessor firmware (ESP-NOW + esp_hosted)
compartilhado/    message contract between the P4 and the C6
ferramentas/      generators (fonts, emoji, hardware table) and utilities
components/       Berry and the other bundled libraries
berry_exemplos/   example Berry apps (copy to apps/Berry/ on the card)
variantes/        what changes in each firmware (full, system, m5burner, berry)
idiomas/          translations (.pot template and en.po)
docs/             documentation (API, architecture, Berry, variants...)
```

## Known limitations

- The ST7123 and ST7121 screens and the ESP32-P4 v3.x build and take the
  conservative path (sleep only turns off the backlight, factory panel clock),
  but have not been tested on a device yet. Reports are welcome.
- Code identifiers, the Berry API names, the dated analysis notes in `docs/`
  and the changelog entries up to 0.9.1 are in Portuguese, as upstream.
- The battery curve (`hal/tab5_power.hpp`) was calibrated on one unit
  (1306.8 mAh measured). Other batteries may need recalibration.
- Bluetooth is BLE only: the C6 does not do Bluetooth Classic.
- Wi-Fi and ESP-NOW share a single-channel radio. With the router connected,
  ESP-NOW stays on its channel.

## Licence

[MIT](LICENSE). Third-party components have their own licences: see
[docs/TERCEIROS.md](docs/TERCEIROS.md).
