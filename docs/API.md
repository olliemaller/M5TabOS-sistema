# M5tabOS API

The reference for what the system offers an app: the function, what it does
and **which task it may be called from**. The [`Exemplo`](../main/app_exemplo/app_exemplo.cpp)
app (shown as "Example") exercises each of these areas on a card of its own.
Open it on the device and read the code alongside.

For *how to think* (the design rules, the reason behind each one, the mistakes
we have already made) the guide is [ESCREVENDO_APPS.md](ESCREVENDO_APPS.md)
(writing apps). This file is the *what*.

---

## Why go through the API

Nothing stops an app from talking to the hardware directly: `M5.Imu`, `Wire`,
`fopen`, `xTaskCreate`, a full-screen `M5.Display.fillRect`. It works on the
bench and breaks in use, far from the cause. Each API below exists because the
direct route has already broken like this:

| direct route | what happens | the API |
|---|---|---|
| reading a sensor over the internal I2C from another task | M5Unified's `I2C_Class` has no lock: two tasks interleave and read the **wrong register, with no error** | `tab5_imu_ultima`, `tab5_power_ultima` (a copy, no I2C) |
| drawing from an app task | LovyanGFX is not safe for concurrency: corrupted pointer, *exception* | change the data, call `sistema_pedir_redesenho()` |
| `fopen(..., "a")` to write | with the card lent to the computer (USB flash drive mode), it corrupts the FAT or loses the data | `tab5_sd_anexar` / `_acrescentar` / `_substituir` |
| `xTaskCreate` + `vTaskDelete` | a task killed while holding a semaphore leaves the resource stuck forever | `tarefa_app_criar` / `tarefa_app_encerrar` |
| `M5.Speaker.tone()` | the codec is powered off when idle; the first beep comes out **silent** | `sistema_beep` |
| a large `fillRect` | a burst on the PSRAM starves the panel of data and the screen **flashes blue** | `tab5_tela_limpar` |
| switching hardware on in `ao_abrir` with `tab5_power_*` | switches off under another app or stays on after closing | `app_t.perfil` (the system adds it up and undoes it) |

**The three-task rule**, which decides almost every "from" column below:

- **interface**: the only task that draws and the only one that talks on the
  internal I2C. It runs `ao_abrir` (on open), `ao_fechar` (on close),
  `desenhar` (draw), `ao_tocar*` (on touch), `ao_teclar` (on key) and
  `ao_aviso` (on notice). Nothing in it may block (card, network, long wait).
- **app tasks** (`tarefa_app`), **worker** (`sistema_trabalho`), **background**
  (`fundo.h`) and **console**: do the slow work, change data (atomically, via
  a queue or under a lock) and request a redraw.
- **system services** (Mesh, ESP-NOW, Wi-Fi, GPS...): have their own task and
  deliver by copy (`tab5_*_msg`, `tab5_*_posicao`) and by notice (`AVISO_*`).

In the **from** column: **UI** = only from the interface task; **any** = any
task; **off UI** = any task except the interface (it blocks).

---

## 1. The app: `kernel/app.h`

An app is a folder `main/app_<nome>/` that defines `extern const app_t APP_<NOME>`.
The build generates the list by itself (`main/CMakeLists.txt`). There is no
table to edit.

| field | what for |
|---|---|
| `nome`, `cor`, `icone` | identity on the home screen (name, colour, icon; `icone` is optional: draw your own) |
| `perfil` | the hardware **this** app needs switched on (`imu`, `alto_falante` = speaker, `microfone`, `camera`, `grove_5v`, `usb_a_5v`). Everything else stays off while it is on screen |
| `usa_sd` / `exige_sd` | the system mounts the card before `ao_abrir` / will not open the app without the card |
| `orientacao` | `ORI_EM_PE` (portrait), `ORI_DEITADO` (landscape), `ORI_SEGUE_IMU` (follows the IMU) |
| `ao_abrir` / `ao_fechar` | create / destroy **everything**. A closed app costs nothing |
| `ao_repouso` / `ao_retomar` | the app's own business before the screen goes off (save the open note). Hardware and suspend belong to the system: see `kernel/recursos.h` |
| `desenhar(tela, area)` | the only function that draws |
| `ao_tocar(x, y)` | tap (on release), coordinates relative to the area |
| `ao_tocar_nivel(x, y, pressionado)` | every cycle while the finger is on the screen (game, drag, list) |
| `ao_teclar(texto, codigo, mod)` | physical keyboard, already decoded (UTF-8) |
| `periodo_ms` / `periodo_agora` | periodic redraw (prefer 0 and notices) |
| `avisos` / `ao_aviso` | mask of `AVISO_*`: the service sends a notice, the app redraws straight away |
| `altura_conteudo` | taller than the area: the system scrolls the whole area |
| `ao_girar` | the screen rotated (`ORI_SEGUE_IMU`) |
| `comandos` | the app's console commands (section 12) |

---

## 2. Redraw and geometry: `kernel/app.h`

| function | from | what it does |
|---|---|---|
| `sistema_pedir_redesenho()` | any | a **value** changed: redraw on the next cycle |
| `sistema_pedir_redesenho_completo()` | UI | the **screen** changed (page, layout): clear and repaint without flicker |
| `sistema_area_foi_limpa()` | UI, in `desenhar` | `true` = the area arrived clean (opening, back from idle): repaint everything |
| `sistema_area()` | UI | the app's area; **never** recalculate it from status bar constants |
| `sistema_quadro_dt()` | UI, in `desenhar` | seconds this frame represents (time-based animation) |
| `sistema_avisar(AVISO_*)` | any | publishes a notice (service → apps that declared it) |
| `sistema_acordar_interface()` | any | wakes the interface out of its idle wait now |
| `sistema_superficie(r, &s)` / `_mudou(r)` | UI | RGB565 buffer for anything that writes an image by DMA (camera) |
| `sistema_app_atual()`, `sistema_bloqueado()` | any | who is on screen; lock screen in front |

Existing notices: `AVISO_MESH_MSG`, `AVISO_MESH_ENVIO`, `AVISO_MESH_NOS`,
`AVISO_GPS`, `AVISO_TRILHA`, `AVISO_TRABALHO`, `AVISO_ESPNOW`.

> **Example:** every card calls `sistema_pedir_redesenho()` after changing
> data; `.avisos` declares `AVISO_TRABALHO | AVISO_ESPNOW | ...`.

---

## 3. Drawing

### `tab5_ui.hpp`: ready-made components

| function | what it does |
|---|---|
| `tab5_fonte_corpo(d)`, `tab5_fonte_corpo_negrito(d)`, `tab5_titulo(d, px)` | smooth fonts (Roboto with accented characters) |
| `tab5_texto(d, n)` | size N (height N×8 px) at the nearest native height |
| `tab5_botao(d, x, y, l, a, txt, cor)` | the system button; saves and restores the text style |
| `tab5_celula(d, cx, y, rotulo, valor, cor, sub)` | small label + large value |
| `tab5_dentro(x, y, rx, ry, rl, ra)` | touch hit test on a rectangle |
| `tab5_cortar_ao_caber` / `_com_reticencias` | trims UTF-8 text until it fits in N px (optionally with an ellipsis) |
| `tab5_linha_grossa(d, ...)` | smooth line that respects the clip rectangle (use instead of `drawWideLine`) |
| `tab5_sprite_novo` / `tab5_sprite_soltar` | sprite always in PSRAM (or `nullptr`) |
| `SIS_FUNDO`, `SIS_LINHA`, `SIS_BORDA` | the system palette (background, line, border; `app.h`) |

### Painting by difference: `kernel/pintura.hpp`

`pintura_iniciar(&p)` at the start of `desenhar`; `pintura_completa(&p)` for
what never changes; `pintura_mudou_i/_s(&p, &memoria, agora)` for each value.
Do not chain comparisons with `||` (see the guide, 2.3).

### Clearing: `hal/tab5_tela.hpp`

`tab5_tela_limpar(x, y, l, a, cor)` (UI): clears in strips, without the blue
flash. Use it for any area that **may** exceed ~40 lines of the screen.

### Lists: `kernel/lista.hpp`

The system's scrolling list: measures each item once, scrolls by shifting
pixels and repaints only the item that changed (`lista_item_mudou`). The whole
Example app is one list.

```c
lista_definir(&l, ROLAGEM_INICIO, n, altura_de, desenhar_item, ctx);   // ao_abrir
lista_desenhar(&l, d, area, TFT_BLACK, sistema_area_foi_limpa());       // desenhar
if (lista_arrastar(&l, x, y, pressionado)) sistema_pedir_redesenho();   // ao_tocar_nivel
int i = lista_item_tocado(&l, x, y, &dy);   // ao_tocar; <0 = nothing or end of a drag
```

### Conversation and virtual keyboard: `kernel/conversa.hpp`, `kernel/teclado_virtual.hpp`

`conversa_t` is the ready-made messaging screen (bubbles, history, compose bar,
keyboard); the app only says how to count, read and send. `tecvirt_*` is the
on-screen keyboard for anyone who needs just that. The Mesh and ESP-NOW apps
use both. Holding a bubble copies the message; holding the compose bar (or
Ctrl+V) pastes.

### Text editor: `kernel/editor.hpp`

`editor_t` is multi-line text with a cursor (used by the Notes app): wraps by
word and by line, scrolls with the finger, virtual and physical keyboard,
hold or Ctrl+V to paste. The buffer belongs to the app (PSRAM); the editor
signals through `mudou` when there is something to save.

### Clipboard: `services/transferencia.h`

A single text, as on mobile phones, of up to 4 KB (`TRANSF_MAX`). The PSRAM
buffer only exists after the first copy. Any task may call it.

| function | what it does |
|---|---|
| `transf_copiar(texto)` | stores it (replaces the previous one) |
| `transf_colar(out, cap)` | the copied text; returns the bytes (0 = empty) |
| `transf_vazia()` | nothing copied yet |

The clipboard has no task and no timer: it is a buffer with a lock, called by
whoever copies or pastes.
| `sistema_recado("Copiado")` | (`kernel/app.h`, UI) the confirmation in the status bar for ~2 s ("Copied") |

---

### Language: `services/idioma.h`

Every piece of text that appears on screen goes through
`tr("texto em português")` (the Portuguese source text); text stored in a table
gets `N_("...")` where it is defined and `tr()` where it is drawn. Console
help and messages follow the interface language too (commands are accepted in
both English and Portuguese); the serial log (`ESP_LOGx`) is in English.

Each language is a `.po` file (the gettext format, which Poedit opens):
`idiomas/en.po` is built into the firmware; any other `.po` copied to
`/sdcard/apps/Sistema/idiomas/` appears in Settings > Language on the next
boot, with nothing to compile (an `en.po` there replaces the built-in one).

```bash
ferramentas/idiomas.py                       # gathers the tr() calls from the code and updates idiomas/*.po
ferramentas/idiomas.py novo fr "Français"    # creates idiomas/fr.po with everything to translate
```

---

## 4. Touch and keyboard: `kernel/app.h`, `hal/tab5_teclado.hpp`

| function | from | what it does |
|---|---|---|
| `sistema_toques(out, max)` | UI, in `ao_tocar_nivel` | every finger in the sample (multi-touch) |
| `sistema_consumir_toque()` | UI | the touch was already handled through `ao_tocar_nivel`: the release does not become a tap |
| `toque_longo(&t, x, y, pressionado)` | UI, in `ao_tocar_nivel` | `kernel/toque_longo.hpp`: true once when the finger stays still for 550 ms (long press) |
| `sistema_travar_navegacao(b)` | UI | the navigation strip is handed to the app (virtual keyboard) |
| `tab5_teclado_presente()` | any | physical keyboard attached |
| `tab5_teclado_ascii(txt, out, n)` | any | text without accents, for ASCII protocols |

Keys arrive through `ao_teclar`. Special codes: `TAB5_TECLA_ENTER`,
`_APAGAR` (backspace), `_DEL`, `_ESC`, `_TAB`, `_ESQUERDA` (left), `_DIREITA`
(right), `_CIMA` (up), `_BAIXO` (down); modifiers `TAB5_TECLA_CTRL/SHIFT/ALT`.

---

## 5. Work off the interface

| API | when | from |
|---|---|---|
| `tarefa_app_criar(&t, corpo, nome, pilha, prio)` / `tarefa_app_encerrar(&t, ms)` (`kernel/tarefa_app.h`) | a continuous loop while the app is open | UI (`ao_abrir`/`ao_fechar`) |
| `sistema_trabalho(fn, arg)` (`kernel/trabalho.hpp`) | one slow operation (read a file, build a list); completion arrives as `AVISO_TRABALHO` | any |
| `sistema_trabalho_cancelado()` | inside the job: the app has closed, stop | worker |
| `sistema_trabalho_solto(fn, arg)` | seconds of work that must not hold up closing (saving a photo); `arg` belongs to it | any |
| `sistema_fundo_iniciar/parar/ativo(&def)` (`kernel/fundo.h`) | work that **carries on with the app closed** (recording a track, stopwatch) | UI |
| `fundo_esperar(f, ms)`, `fundo_ceder(f)`, `fundo_estado(f, n, txt)` | inside the background loop | background |

Loop rules (all three): never wait with `portMAX_DELAY` (the stop request has
to get through); do not draw; use your own memory, never the window's.

> **Example:** the TAREFA cards (task + queue of 1), TRABALHO (counts files and
> receives `AVISO_TRABALHO`) and FUNDO (a stopwatch that shows in the status
> bar with the app closed).

---

## 6. SD card: `hal/tab5_sd.hpp`

App folder: `/sdcard/apps/<Nome>/`, created automatically.

| function | from | what it does |
|---|---|---|
| `tab5_sd_anexar(app, arq, linha, cabecalho)` | any | appends a line **without waiting** (batched writes) |
| `tab5_sd_acrescentar(app, arq, dados, n)` | off UI | appends a block now |
| `tab5_sd_substituir(app, arq, dados, n, flags)` | off UI | replaces the whole file (never leaves half a file); `TAB5_SD_SIGILOSO` (confidential) wipes the old one |
| `tab5_sd_apagar(app, arq)` | off UI | deletes the file from the app's folder (same lock; refuses while the card is lent out) |
| `tab5_sd_abrir(app, arq, "r")` | off UI | **read** (and large new files, with `exige_sd`) |
| `tab5_sd_pasta(app, buf, n)` | off UI | folder path |
| `tab5_sd_montado()`, `tab5_sd_espaco(&t, &l)` | any | status (mounted; total and free space) |

The three write paths keep the data in the flash journal while the card is on
the computer and deliver it, in order, when it comes back. **Only rewrite what
you have read.**

> **Example:** the CARTÃO (card) card (`tab5_sd_anexar` with a CSV header).

### Disks and files on any disk: `services/tab5_volumes.hpp`, `services/tab5_arquivos.hpp`

For anything outside the app's own folder - music, the USB flash drive,
copying between disks - everything goes by **absolute path** (`/sdcard/...`,
`/usb/...`). The service holds the disk while it works: the caller mounts
nothing.

| function | from | what it does |
|---|---|---|
| `tab5_volumes_listar(v, max)` | any | the disks present now (card always; flash drive when plugged in) |
| `tab5_volume_ejetar(raiz, espera_ms)` | off UI | unmounts the flash drive so it can be removed safely |
| `tab5_arq_listar(pasta, itens, max, &total, ordem, filtro)` | off UI | what the folder shows (folders first, hidden items left out) |
| `tab5_arq_percorrer(pasta, visita, ctx)` | off UI | every entry, with no table and no limit (no size/date) - **do not use `opendir`** |
| `tab5_arq_maior_numero(pasta, "foto_%04lu.bmp")` | off UI | the highest number already written: gives the next numbered file |
| `tab5_arq_visivel/gravavel/mexivel(caminho)` | any | the rules: hidden, read-only (`/sdcard/apps`, except `apps/Berry`), free to change |
| `tab5_arq_criar_pasta`, `tab5_arq_renomear`, `tab5_arq_apagar` | off UI | quick; delete moves to `<raiz>/.Lixeira` (the Trash) |
| `tab5_arq_copiar(origens, n, destino, mover)` | off UI | **starts** a background job; progress in `tab5_arq_trabalho()` and `AVISO_ARQUIVOS` |
| `tab5_arq_area_por / tab5_arq_colar` | any / off UI | the system file clipboard (Copy, Cut, Paste) |
| `tab5_arq_esvaziar_lixeira(raiz)`, `tab5_arq_cancelar()` | off UI / any | empty the Trash; cancel |

Copying carries on with the app closed and the screen off, shows `Copying 45%`
in the status bar, and never leaves a half-written file (flash drive pulled
out, cancelled, disk full: the file in progress is deleted). Console: `files`
(`arquivos`), `usb eject` (`usb ejetar`).

---

## 7. Notifications: `services/notificacao.h`

State, not a queue: each source has **one** current state, and publishing again
replaces it.

```c
int id = notif_registrar("MeuApp");                 // idempotent by name
notif_associar_app(id, "MeuApp");                   // tapping the notification opens the app
notif_publicar(id, 3, NOTIF_ATENCAO, "3 alertas");  // any task ("3 alerts")
notif_limpar(id);                                   // = publish 0
```

Levels: `NOTIF_ESTADO` (activity), `NOTIF_INFO`, `NOTIF_ATENCAO` (addressed to
you), `NOTIF_URGENTE`. The status bar is coloured by the worst active source.

---

## 8. Power, sound and display

| function | from | what it does |
|---|---|---|
| `app_t.perfil` | (declaration) | what the app needs switched on; the system combines it with the user's preference |
| `sistema_reaplicar_perfil()` | UI | a preference changed: applies straight away |
| `sistema_manter_acordado(SISTEMA_ACORDADO_APP, b)` | any | the screen does not go off (the system releases it when the app closes) |
| `tab5_power_ultima()` | any | battery: volts, mA, mW, % (a copy, no I2C) |
| `sistema_beep(hz, ms)` | UI | beep (needs `perfil.alto_falante`) |
| `sistema_som_fluxo_abrir/livre/tocar/calar/fechar` | any except UI | sound in blocks (music): the interface powers the codec, the task feeds it; see below |
| `tab5_painel()`, `tab5_painel_nome()` | any | which of the Tab5's three screens (ILI9881C, ST7123, ST7121) |
| `tab5_power_cpu_mhz()` | any | current clock (360 MHz on P4 v1.x, 400 on v3.x) |

> **Example:** the ENERGIA (power), SOM (sound) and ACORDADO (awake) cards.

**The system player: `services/tab5_musica.hpp`.** Music is a service, not an
app: the card's music folder (`tab5_musica_pasta`), listing
(`tab5_musica_listar`), playing a folder starting from an item
(`tab5_musica_tocar(rel, idx, app)`), the controls, the status and the spectrum
(`tab5_musica_espectro`, computed only while someone is reading it). It runs as
a kernel background part ("Música", shown as "Music"): status bar, mini player
and the X that stops it come for free. The Music app and the Berry `musica`
module are two front ends to it - neither of them decodes anything or reads
the card. Callable from any task; the controls only flag a request. Also part
of the service: **shuffle and repeat** (`tab5_musica_definir_aleatorio`,
`tab5_musica_definir_repetir` - off, the whole queue, the same track; kept in
the settings), **playlists** as `.m3u` in the `Playlists` subfolder
(`tab5_musica_playlists`, `_playlist_criar`, `_playlist_itens`,
`_playlist_por`, `_playlist_tirar`, `_playlist_apagar`,
`tab5_musica_tocar_playlist`; `Favoritas` (Favourites) is the heart one), a
file's **duration** without playing it (`tab5_musica_duracao`, header only;
remembered) and the **cover art** of what is playing (`tab5_musica_capa`: the
one in the ID3 tag, the folder's cover/folder.jpg or, with Wi-Fi, an iTunes
search by artist and title - stored in `Music/.capas/`). Every `rel`/`caminho`
also accepts an ABSOLUTE path on a disk (`"/usb/Rock/x.mp3"`): the USB-A flash
drive plays directly, and the queue and playlists mix the two. Formats: MP3,
WAV and AAC in `.m4a`/`.mp4` (the audio of a video).

**The video player: `services/tab5_video.hpp`.** `.mp4/.m4v/.mov` with H.264
(embedded AAC audio, played by Music) and MJPEG `.avi` (audio from the `.mp3`
of the same name): `tab5_video_abrir`, the controls, the status,
`tab5_video_pintar` (scales through the PPA onto the app's surface) and
`TAB5_VIDEO_EXTENSOES` (the filter for listing). Underneath:
`services/tab5_h264` (the H.264 decoder, with the `video h264` bench) and
`services/tab5_midia` (the bridge between the `components/medf` libraries -
container readers - and `components/audf` - audio decoders - and the system's
file reader).

**The disks: `services/tab5_volumes.hpp`.** Card (`/sdcard`) and USB flash
drive on USB-A (`/usb`, `services/tab5_usb`) by absolute path:
`tab5_volumes_listar`, `tab5_volume_montar(caminho)`/`_desmontar` (hold the
disk the path lives on), `tab5_volume_disponivel` (for anything that holds it
a long time: once it turns `false`, let go - card lent out or flash drive
pulled out). A disk arrived or left: `AVISO_VOLUMES` (Berry:
`# avisos: volumes`).

**Streamed sound** (used by the system player, `services/tab5_musica.cpp`):
`sistema_som_fluxo_abrir(ms)` asks the interface to switch the speaker on and
waits for the codec to warm up; then the task (the background part) queues
16-bit PCM blocks with `sistema_som_fluxo_tocar(pcm, quadros, hz, estereo)`
whenever `sistema_som_fluxo_livre()`. The speaker holds two blocks (the one
playing and the next): use **three** rotating buffers, in PSRAM. `tocar`
returns `false` when the stream was closed underneath it (USB flash drive mode
took the pins) - stop. `calar` (mute) discards the queue (pause, track
change); `fechar` releases the speaker. The system beep plays over the music,
on another channel.

**When reading a large file from the card, use `open`/`read`, not `fread`.**
Measured on the Tab5, reading 256 KB: `fread` with `setvbuf(_IONBF)` 52 KB/s
(newlib reads one byte per call into FAT), plain `fread` 572 KB/s, `read()`
4.6 MB/s - the whole request goes to FatFs, which DMAs several sectors at
once.

**Now playing: `kernel/midia.h`.** Whatever plays media takes the slot
(`midia_assumir(fonte, app, &controles)`: the background part that plays and
the app the mini player opens) and publishes title, playing/paused, position
and duration (`midia_publicar`, from any task). The system shows a lilac pill
in the status bar (alongside the notifications pill) and the mini player at
the foot of the notification centre; the controls (`tocar_pausar`, `proxima`,
`anterior`, `ir_para`) run on the interface and must only flag a request.
`midia_definir_cor` changes the colour of the pill and the mini player.

**Writing to the settings (NVS) disables the cache, and without the cache the
PSRAM disappears**: from a task whose stack is in PSRAM (Berry's) that brings
the device down. The `ajustes_definir_*` functions callable from any task
write through the deferred path (on the `esp_timer` task); `ajustes` refuses,
with an error log, a direct write coming from a stack in PSRAM.

---

## 9. Sensors and time

| function | from | what it does |
|---|---|---|
| `tab5_imu_ultima(&d)` (`hal/tab5_imu.hpp`) | any | acceleration (g) and rotation (°/s); needs `perfil.imu` |
| `tab5_gps_no_ar()`, `tab5_gps_posicao()`, `tab5_gps_tempo()` (`hal/tab5_gps.hpp`) | any | GPS on the Grove port; lat/lon are zero without a fix |
| `tab5_gps_assinar(fila)` | any | receives every new sentence on a queue of your own |
| `tab5_sensores_temp(&c)` (`hal/tab5_sensores.hpp`) | any | internal temperature |
| `tab5_hora_epoch()` (`services/tab5_hora.hpp`) | any | now in UTC, or 0 if the clock has not been set |
| `tab5_hora_local(epoch, &tm)` | any | for **display** (the time zone the person chose, `ajustes_fuso_min()`); always store UTC |
| `tab5_hora_fonte()` | any | where the time came from (GPS, network, mesh, RTC) |
| `tab5_agora_ms()` | any | monotonic clock for measuring intervals (compare by difference) |

> **Example:** the HORA (time), IMU and GPS cards.

---

## 10. Network

### Wi-Fi: `hal/tab5_wifi.hpp`

| function | from | what it does |
|---|---|---|
| `tab5_wifi_estado()`, `tab5_wifi_descricao()` | any | status, and text ready for the screen |
| `tab5_wifi_ssid()`, `tab5_wifi_ip()`, `tab5_wifi_rssi()` | any | the current connection |
| `tab5_wifi_ligar()` / `_desligar()` | UI | normally the user does this in Settings, not the app |
| `tab5_wifi_procurar()`, `tab5_wifi_vista(i, &v)` | any | scan the networks around |
| `tab5_wifi_gravar_credenciais(ssid, senha)` | off UI | saves a network (password encrypted) |

With Wi-Fi up, the rest is ordinary ESP-IDF (`esp_http_client`, sockets, MQTT).

### ESP-NOW: `services/tab5_espnow.hpp`

Needs our own firmware on the C6 (`c6/`, see [RADIO_C6.md](RADIO_C6.md)).

| function | from | what it does |
|---|---|---|
| `tab5_espnow_pronto()`, `tab5_espnow_canal()` | any | on air, and on which channel |
| `tab5_espnow_enviar(texto)` | any | broadcast; does not block |
| `tab5_espnow_enviar_para(mac, texto)` | any | direct (unicast); does not block |
| `tab5_espnow_enviar_todos_canais(mac_ou_null, texto)` | any | channels 1-13; Wi-Fi drops for a few seconds |
| `tab5_espnow_n()`, `tab5_espnow_msg(i, &m)` | any | history in RAM (a copy) |
| `tab5_espnow_janela(b)` | UI | the window opened/closed: receiver on while the app is on screen |

Notice: `AVISO_ESPNOW`.

### Mesh (Meshtastic): `services/tab5_mesh.hpp`

See [MESHTASTIC.md](MESHTASTIC.md). A system service: the radio, the nodes and
the history live in it, and the app is only a window.

| function | from | what it does |
|---|---|---|
| `tab5_mesh_servico_ativo()`, `tab5_mesh_no_ar()` | any | status |
| `tab5_mesh_num_nos()`, `tab5_mesh_no(i, &no)` | any | known nodes (a copy) |
| `tab5_mesh_num_nos_busca(txt)`, `tab5_mesh_no_busca(txt, i, &no)` | any | the same, only those matching the long name/short name/`!id` (ignoring accents and case) |
| `tab5_mesh_num_msgs()`, `tab5_mesh_msg(i, &m)` | any | messages (0 = most recent) |
| `tab5_mesh_nao_lidas()` | any | unread |
| `tab5_mesh_enviar(para, canal, texto)` | any | `para = 0xFFFFFFFF` broadcasts on the channel; anything else = direct |
| `tab5_mesh_estado_envio(id, &erro)` | any | ACK, failure |

Notices: `AVISO_MESH_MSG`, `AVISO_MESH_ENVIO`, `AVISO_MESH_NOS`.

Internet mode servers (`services/tab5_mesh_mqtt.hpp`): the list belongs to
the person. `mesh_mqtt_n_servidores()` / `mesh_mqtt_servidor(i, &s)` (0 =
automatic), `mesh_mqtt_salvar_servidor(i, &s)` (i = 0: new),
`mesh_mqtt_apagar_servidor(i)`, `mesh_mqtt_modelo(i)` to pre-fill a new one.
The choice is `ajustes_mqtt_servidor()`; changing it requires restarting the
service (`sistema_mesh_reiniciar()`). Changing the link: `sistema_mesh_via(via)`
(the Bluetooth link switches Bluetooth on as well).

### Bluetooth: `hal/tab5_ble.hpp`

`tab5_ble_pronto()`, `tab5_ble_varrer(ms)`, `tab5_ble_visto(i, &v)`. The user
switches it on and off (`sistema_bluetooth`).

**Switching a service on** is always a system call (`kernel/app.h`), which
saves the choice and applies it: `sistema_bluetooth`, `sistema_wifi`,
`sistema_grove`, `tab5_usb_ligar`. The app does not save the setting and
drive the hardware on its own.

> **Example:** the WIFI, ESP-NOW (with a broadcast button) and MESH cards
> (read-only: an example app should not talk on a public channel).

---

## 11. Camera: `hal/tab5_camera.hpp`

| function | from | what it does |
|---|---|---|
| `tab5_camera_iniciar(quem)` / `tab5_camera_parar(quem)` | UI | on/off (reference counted: several users coexist); `quem` (who) appears in the CAMERA resource |
| `tab5_camera_definir_destino(&d)` | UI | where each preview frame is written (use `sistema_superficie`) |
| `tab5_camera_pedir_quadro(buf, l, a)`, `tab5_camera_quadro_pronto(buf, &ok)` | any | one frame at the requested size (the photo: `TAB5_CAM_FOTO_L` x `_A`) |

The app does not deal with idle: with the screen off the system pauses the open
app's camera and restarts it on wake, with the same destination
(`tab5_camera_pausar`, called by the kernel). A photo being captured at that
moment comes back with `ok = false`. The Camera app is the complete reference.

---

## 12. App console: `app_t.comandos`

The serial console (`idf.py monitor`, type `help`) belongs to the system, but
an app can offer its own commands. They appear in an **APPS** group of
`help system` (`help sistema`) (and in `help app`) and are always called with
the app's name in front, so they never collide:

```
app exemplo contar 5
```

```c
static void cmd_contar(int argc, char** argv) { ... }   // argv[0] = "contar"

static const app_comando_t COMANDOS[] = {
  { "contar", "[n]\tadds n to the counter", cmd_contar, false },
  { "zerar",  "\tresets it (only with the app open)", cmd_zerar, true },
  { nullptr },
};
// in the app_t:  .comandos = COMANDOS,
```

The command runs on the **task of whoever typed it**: the serial console's, or
the Terminal's background part. The rules of any task that is not the
interface apply (do not draw, change data atomically or under a lock, request
a redraw). Write with `printf`: stdout belongs to the task, and the Terminal
keeps what comes out. A long loop checks `console_interrompido()` (Ctrl+C in
the Terminal). Relative paths start from the session's current folder (`cd`).
`so_aberto = true` makes the system refuse the command while the app is
closed, and is the right choice for every command that touches state created
in `ao_abrir`.

### Network tools: `services/tab5_rede.hpp`

The device's address (`tab5_rede_ip`), DNS, the ARP table and the local
network scan, traceroute, TCP port test and HTTP/HTTPS (`tab5_rede_http`
delivers the body in pieces; `tab5_rede_http_texto` gathers it into a string
in PSRAM). Everything blocks: off the interface. The long loops take a
`parar()` (stop) callback (the Terminal passes `console_interrompido`).

### SSH: `services/tab5_ssh.hpp`

`tab5_ssh_rodar()` connects, authenticates by password, runs a command (or the
shell line by line) and disconnects, delivering the output through a callback.
The password and the shell lines come through `ler_linha` (the console passes
`console_ler_linha`).

### Terminal: `services/tab5_terminal.hpp`

The on-screen command session (the Terminal app). The commands **are** the
console's (`console.cpp`, a single table, with the help alongside); the
console has sessions (`console_sessao_t`: the current folder and the Ctrl+C of
whoever is typing), and the Terminal is one of them.
`tab5_terminal_enviar(linha)` runs on a background part (it carries on with
the app closed; the pill shows the command, the X interrupts it);
`tab5_terminal_linha(i, ...)` reads what came out. The lines (~51 KB) live in
PSRAM and only exist after the first opening. A new command goes into the
console's `COMANDOS` and appears in both places.

---

## 13. Hardware: what differs between devices

The same binary runs on all **three screens** of the Tab5 (M5GFX detects
which). The system adjusts what differs by itself: touch watchdog, idle and
panel clock (see `tab5_painel()` in `hal/tab5_power.hpp`). An app that draws
through `M5.Display` and reads touch through `ao_tocar` does not need to know
which one it is.

The **chip** is another story: the ESP32-P4 v1.x and v3.x need different
binaries (`sdkconfig.defaults.chip_v1` / `_v3`). `./gravar.sh` (flash script)
reads the device's revision and picks. To build by hand:

```bash
idf.py -B build_v3 -DM5_CHIP=v3 build
```

App code should not depend on the revision. If you really must, use
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` (v1.x) from `sdkconfig.h`.
