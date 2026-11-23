# Apps in Berry

An app in [Berry](https://berry.readthedocs.io) is **a `.be` file on the card**.
There is nothing to compile and no tools to install: write it in a text editor
and copy it to the card (through USB flash drive mode, the icon appears as soon
as you eject).

**All the functions on a single page, plus links for learning the language:
[BERRY_API.md](BERRY_API.md).**

The examples are in [`berry_exemplos/`](../berry_exemplos): [`Ola.be`](../berry_exemplos/Ola.be)
is the first step, and [`Mostruario.be`](../berry_exemplos/Mostruario.be) (showcase) uses
**the whole** API, one tab per part of the system. It is the template for a new
app. [`Registro.be`](../berry_exemplos/Registro.be) (logger) is the template for
an app that **keeps running with the window closed** (see
[Background part](#background-part)), and
[`Azimutal.be`](../berry_exemplos/Azimutal.be) is the system's Azimutal app
redone as a script (sky, satellites and background recording).

## Installing

The firmware ships with an example app, **Ola Berry** (`berry_exemplos/Ola.be`):
on the first boot with a card that does not yet have `apps/Berry/`, the system
writes it there. It shows, in tabs, what a script can do - time and network,
buttons and beep, a notice in the status bar, data saved to the card, the
accelerometer, finger drawing and the disks. Once deleted, it does not come
back.

1. Copy the `.be` file into the card's **`apps/Berry/`** folder. The folder is
   created automatically on the first boot with a card.
2. Apps are read at boot (up to 16) and **again when the card comes back from
   USB flash drive mode**: copy from the computer and eject, and the icon
   appears on the home screen without a restart. If you copied some other way
   (card in a reader), restart or use `berry reload` (`berry reler`) in the
   console.
3. The app appears on the home screen with the file name, or the one in the
   header.

Another way to install: in the **Files** app, tapping a `.be` file (from the
flash drive, for example) copies it to `apps/Berry/` and the icon appears
straight away.

## Removing

**Hold the app's icon** on the home screen (as if to move it) and **drag it to
the "Remove" strip** that appears at the top; let go and confirm. The
`.be` file and the app's data folder (`apps/Berry/<nome>/`) go to the **card's
Trash** - Files can restore them until it is emptied. System apps do not show
the strip. Deleting or moving the `.be` file in Files also removes the icon
straight away.

## The file

```berry
# nome: Meu App          (optional) the name under the icon
# cor: 0x07E0            (optional) the icon colour, RGB565
# periodo: 1000          (optional) redraw by itself every N ms
# avisos: espnow         (optional) redraw STRAIGHT AWAY when something arrives: espnow, mesh, gps, volumes, arquivos
# orientacao: em_pe      (optional) em_pe (portrait, the default), deitado (landscape) or girar (follows the device)

var contador = 0         # whatever is outside the functions runs once, on opening

def desenhar(tudo)
  tela.texto("Counter: " + str(contador), 40, 40, 4)
  tela.botao(40, 120, 300, 90, "+1")
end

def tocar(x, y)
  if tela.dentro(x, y, 40, 120, 300, 90) contador += 1 end
end
```

A function can only be called by code that comes **after** it in the file
(Berry rejects a name it has not seen yet): write the helpers at the top.

### The functions the system calls (all optional)

| function | when |
|---|---|
| `abrir()` | the app opened (after the code outside the functions) |
| `desenhar(tudo)` | time to paint. The area arrives **cleared** and the frame goes on screen in one go, without flicker: always draw the whole screen (`tudo` is `true`). With `sistema.acumular(true)` it stops arriving cleared - see [Layers and accumulate](#layers-and-accumulate) |
| `tocar(x, y)` | a tap, in the app area's coordinates (the same as for drawing). The system redraws after it |
| `dedo(x, y, pressionado)` | the finger **while** it is on the screen (dragging, drawing, games), every 20 ms; one last time with `pressionado = false` when it lifts. The system redraws after each one |
| `tecla(texto, codigo)` | a key, from the **physical or virtual** keyboard: `texto` is what was typed (UTF-8), or `""` and `codigo` says which special key (`teclado.ENTER`, `APAGAR`...) |
| `aviso(nome)` | a service asked for in `# avisos:` published something (`"espnow"`, `"mesh"`, `"gps"`, `"volumes"` - a flash drive was plugged in or removed, `"arquivos"` - the copy progressed or finished). The system already redraws by itself afterwards; the function is for whatever else the app wants to do (mark as read, play a beep) |
| `fechar()` | the window is about to close |
| `fundo()` | the **background part**: with `sistema.fundo(true)` the system calls it from time to time, with the window open **or closed** - see [Background part](#background-part) |

## Module `tela` (screen)

Coordinates relative to the app area (0,0 is the top corner, below the status
bar). The drawing functions only work **inside `desenhar()`**.

| function | what it does |
|---|---|
| `tela.largura()`, `tela.altura()` | the size of the area (width, height). With the virtual keyboard open, `altura()` already excludes it |
| `tela.texto(txt, x, y [, tam [, cor [, alinhamento]]])` | text; `tam` N = N×8 px high (default 3, accepts 2.5). `alinhamento` (alignment): `tela.ESQUERDA` (left, the default), `CENTRO` (centre) or `DIREITA` (right) - `x` is the start, middle or end of the text |
| `tela.medir(txt [, tam])` | the width in px that this text would take up |
| `tela.cortar(txt, largura [, tam])` | the text trimmed to fit in `largura` px, with `...` at the end when it was trimmed (never splits an accented character). For song, contact or file names |
| `tela.retangulo(x, y, l, a, cor [, cheio])` | rectangle (filled by default) |
| `tela.arredondado(x, y, l, a, raio, cor [, cheio])` | rounded-corner rectangle |
| `tela.circulo(x, y, r, cor [, cheio])` | circle |
| `tela.triangulo(x0, y0, x1, y1, x2, y2, cor [, cheio])` | triangle |
| `tela.linha(x0, y0, x1, y1, cor)` | line |
| `tela.pixel(x, y, cor)` | a single point |
| `tela.botao(x, y, l, a, txt [, cor])` | the system button |
| `tela.limpar([cor])` | paints the whole area |
| `tela.dentro(x, y, rx, ry, rl, ra)` | is the point inside the rectangle? (for `tocar`; works outside `desenhar`) |
| `tela.cor(r, g, b)` | a colour from 0–255 values |
| `tela.camada(l, a)` | creates a stored image, a layer (see below) |
| `tela.alvo(c)` / `tela.alvo()` | redirects all `tela` drawing into layer `c` / back to the screen |
| `tela.colar(c, x, y [, rx, ry, rl, ra])` | copies the layer (or the `rx, ry, rl, ra` piece of it) to the screen |
| `tela.liberar(c)` | deletes the layer and returns the memory |
| `tela.PRETO`, `BRANCO`, `VERMELHO`, `VERDE`, `AZUL`, `AMARELO`, `CIANO`, `LARANJA`, `CINZA`, `CINZA_CLARO`, `ROXO`, `ROSA` | ready-made colours (black, white, red, green, blue, yellow, cyan, orange, grey, light grey, purple, pink) |

## Module `sistema` (system)

| function | what it does |
|---|---|
| `sistema.redesenhar()` | asks for `desenhar` to be called again |
| `sistema.periodo([ms])` | changes the `# periodo:` while the app is open (0 = only on request); with no argument, returns the current one. E.g. 16 on an animated tab, 1000 on a clock tab |
| `sistema.acumular([sim])` | `true`: the frame does not arrive cleared, draw only what changed (see below) |
| `sistema.dt()` | seconds between this frame and the previous one **on screen**. For time-based animation: `x += velocidade * sistema.dt()` |
| `sistema.ms()` | milliseconds since boot (for measuring time) |
| `sistema.us()` | microseconds, for measuring short stretches (only the difference between two readings means anything) |
| `sistema.hora()` | `"14:05:09"` in the local time zone, or `""` if the clock has not been set |
| `sistema.hora_de(epoch)` | `"14:05"` in the local time zone, for the time of a message (`""` if 0) |
| `sistema.data()` | `"14/10/2026"`, or `""` |
| `sistema.agora()` | seconds since 1970 (UTC), or 0 |
| `sistema.bateria()` | charge in % |
| `sistema.energia()` | `{"pct", "volts", "ma"}`; `ma` > 0 = charging |
| `sistema.imu()` | `[ax, ay, az]` in g, or `nil`. An app that uses it gets a fresh reading every frame |
| `sistema.giro()` | `[gx, gy, gz]` in degrees/s, or `nil`. The gyroscope only stays on while some script is calling it (5 s without a call, it switches off and comes back on the next call) |
| `sistema.repouso()` | `true` with the screen off. Lets the background part space out its own rhythm: each `fundo()` wakes the device from light sleep |
| `sistema.gps()` | `nil` without a GPS on the Grove port; otherwise `{"fix", "lat", "lon", "alt", "sats"}` |
| `sistema.wifi()` | the Wi-Fi status as text |
| `sistema.rede()` | `{"ssid", "ip", "rssi", "texto"}`; empty `ip` = not connected |
| `sistema.bip(hz, ms)` | a beep |
| `sistema.avisar(txt [, nivel])` / `sistema.avisar()` | puts / removes a notice in the status bar. `nivel`: `"info"`, `"atencao"` (attention, the default) or `"urgente"` (urgent) |
| `sistema.acordado(sim)` | `true`: the screen does not go off while the app is open |
| `sistema.log(txt)` | writes to the serial log |
| `sistema.fundo(sim [, ms])` / `sistema.fundo()` | switches the [background part](#background-part) on / off (calls `fundo()` every `ms`, default 1000, minimum 100); with no argument, says whether it is running |
| `sistema.fundo_estado(txt [, n])` | the text of the pill in the status bar while the background part runs (`"42 samples"`) |
| `sistema.memoria()` | `{"app", "berry", "orcamento"}` in bytes: what **this** machine (Berry VM) uses, what all the Berry machines add up to, and their ceiling (budget) |

<a id="camadas-e-acumular"></a>

## Layers and accumulate

By default the app redraws **the whole screen** every frame: it is the simple
way, and on a screen of text and buttons it costs a few ms. But drawing costs
per pixel, and there are two situations where redrawing everything is waste:
**a small thing moving on a full screen**, and **a drawing that only grows**
(finger painting, a scrolling graph). For those:

**`sistema.acumular(true)`** - the frame stops arriving cleared: what you drew
stays on the screen. `desenhar(tudo)` now tells you what to do:

- `tudo = true`: the area **was** cleared (the app opened, the screen rotated,
  the keyboard opened, it came back from the lock screen, or you have just
  switched accumulate on) - draw everything;
- `tudo = false`: draw **only what changed** since the last frame, and erase
  whatever moved yourself (a black rectangle over it).

**Layers** - images stored in memory, of whatever size you like (up to 8 per
app, totalling 4 screens). After `tela.alvo(c)`, every `tela` function draws
into it - **in any event**, not just in `desenhar()`. `tela.alvo()` goes back
to the screen, and `tela.colar(c, x, y)` puts the layer on the screen. Use them
for what is expensive to draw and changes little (a game's background, a
graph) and to keep what cannot be redone (a painting).

```berry
var pintura = tela.camada(tela.largura(), 800)
var novos = []
sistema.acumular(true)

def dedo(x, y, pressionado)
  if !pressionado return end
  tela.alvo(pintura)                      # paint into the layer: it is kept
  tela.circulo(x, y, 6, tela.AMARELO)
  tela.alvo()
  novos.push([x, y])                      # and note it down for the next frame
end

def desenhar(tudo)
  if tudo
    tela.colar(pintura, 0, 0)             # the area was cleared: the whole painting
  else
    for p: novos tela.circulo(p[0], p[1], 6, tela.AMARELO) end   # only the new part
  end
  novos = []
end
```

The **Dedo** (finger) tab of [`Mostruario.be`](../berry_exemplos/Mostruario.be) is
this example in full. Pasting costs per pixel just like drawing (see
`Bench.be`): paste the whole layer only when `tudo`, and pieces
(`rx, ry, rl, ra`) to restore the background under something that moves.

## Module `teclado` (keyboard)

The virtual keyboard is **the system's own** (the same one used for the Wi-Fi
password): the app only opens and closes it. Keys arrive in
`tecla(texto, codigo)`, just like those from the physical keyboard - the same
code serves both.

| function | what it does |
|---|---|
| `teclado.abrir([tipo])`, `teclado.fechar()` | shows / hides the virtual keyboard, at the bottom of the area. While it is open, `tela.altura()` shrinks: draw down to that. The `tipo` says what the field is, as in the system apps: `"texto"` (text, the default), `"ascii"` (server, user name), `"senha"` (password), `"numero"` (digits only), `"busca"` (search), `"paragrafo"` (paragraph: capital at the start of a sentence and accents; the green key is a new line), `"codigo"` (code), `"comando"` (command). While open, calling it again only changes the type. The green key arrives as `teclado.ENTER` in all of them |
| `teclado.aberto()` | is it on screen? (its ✕ closes it by itself and sends `ESC`) |
| `teclado.fisico()` | is there a physical keyboard on ExtPort1? |
| `teclado.ENTER`, `APAGAR`, `ESC`, `TAB`, `ESQUERDA`, `DIREITA`, `CIMA`, `BAIXO`, `DEL` | the special key codes (`APAGAR` = backspace; left, right, up, down) |

```berry
def tecla(texto, codigo)
  if texto != ""                    campo += texto
  elif codigo == teclado.ENTER      enviar(campo)  campo = ""
  elif codigo == teclado.APAGAR     campo = sem_ultimo(campo)   # see Mostruario.be
  end
end
```

## Module `cartao` (card)

Files in the app's folder, **`/sdcard/apps/Berry/<nome do app>/`** (created
automatically), next to the `.be` files - everything Berry-related lives inside
`apps/Berry/`. Just the file name, no folder: the script cannot leave its own
folder. It is the only way in to files: Berry's `open()`, `load("arquivo")` and
`save()` open nothing in a script (they return an error) - it is the system
that opens the `.be` file, when it loads the app.

Nor can the script touch memory outside Berry: there is no `introspect`
module, `bytes` does not accept a pointer (`bytes(ptr, n)`) and attributes
starting with `.` (the internal ones) cannot be assigned.

| function | what it does |
|---|---|
| `cartao.ler(nome)` | the whole file as text (up to 64 KB), or `nil` if it does not exist |
| `cartao.gravar(nome, texto)` | replaces the contents; `true` if it was written |
| `cartao.anexar(nome, texto)` | appends at the end (a log, a CSV) |
| `cartao.existe(nome)` | does the file exist? |
| `cartao.lista()` | the names of the files in the folder |

To store structured data (lists, maps), use Berry's `json`:
`cartao.gravar("estado.json", json.dump(estado))` and `json.load(cartao.ler("estado.json"))`.

## Module `arquivos` (files)

The whole card and the USB flash drive, through the system's file engine.
**The rule: the script sees everything, but only changes its own folder**
(`/sdcard/apps/Berry/<nome do app>/`). Whatever is outside, it lists and
**copies in** - and reads after bringing it in. Nothing outside the folder is
written, deleted, moved or renamed by a script. System passwords and names
starting with `.` do not appear. Full example:
[`Importar.be`](../berry_exemplos/Importar.be).

| function | what it does |
|---|---|
| `arquivos.volumes()` | the disks present now: `[{"raiz", "tipo", "nome", "disponivel"}]` (`"/sdcard"`; `"/usb"` with a flash drive) |
| `arquivos.pasta()` | the absolute path of the app's folder |
| `arquivos.lista(pasta [, filtro])` | what is in **any** folder (absolute: `"/usb/Rock"`; relative: inside the app's folder, `""` = the folder itself): `[{"nome", "tipo", "pasta", "bytes", "data", "caminho"}]`, folders first. `tipo`: `"pasta"` (folder), `"musica"` (music), `"imagem"` (image), `"texto"` (text), `"berry"`, `"outro"` (other). `filtro`: `".mp3;.wav"`. `nil` = disk missing or a folder that will not open |
| `arquivos.copiar(origem [, subpasta])` | **brings in** a file or folder (or a list of them, up to 64) from any disk to the app's folder (or an existing subfolder of it). In the background: `true` = started; otherwise the text of the reason (`"já há uma cópia em andamento"`, a copy is already in progress) |
| `arquivos.estado()` | `{"fase", "pct", "itens", "feitos", "atual", "erro", "meu"}` - phase `"parado"` (idle), `"contando"` (counting), `"copiando"` (copying), `"feito"` (done), `"falhou"` (failed), `"cancelado"` (cancelled); `meu` = this app started it |
| `arquivos.cancelar()` | stops the copy this app started |
| `arquivos.ler(caminho)` | the contents as text (up to 64 KB) - **only inside the app's folder** (`"sub/x.txt"`) |
| `arquivos.apagar(caminho)`, `arquivos.renomear(caminho, nome)`, `arquivos.criar_pasta(nome)` | **only inside the app's folder**; delete moves to the card's Trash |

- **One copy at a time on the device** (including the Files app's): ask for
  `# avisos: arquivos` to find out when it has finished, and show `estado()`.
- The copy carries on with the app closed; progress also shows in the status
  bar.
- A relative path never leaves the app's folder: `".."`, a leading `"/"` and
  `"\\"` give `arquivos_error`.
- `cartao` is still the simple way for loose files in the app's folder
  (`cartao.gravar("estado.json", ...)`); `arquivos` adds subfolders and the
  rest of the disks.

## Module `espnow`

The radio, the history and the notification while the app is closed belong to
the system service: the app only displays and sends. [`EspNow.be`](../berry_exemplos/EspNow.be)
is the system's ESP-NOW app redone in Berry - the example of a real-world app.

| function | what it does |
|---|---|
| `espnow.estado()` | `{"codigo", "texto"}`: 3 = on air, 2 = starting, 1 = Wi-Fi never switched on, 4 = radio stopped, 0 = C6 without the firmware; `texto` says what to do |
| `espnow.pronto()` | is it on air? |
| `espnow.enviar(texto)` | broadcast on the channel (everyone hears it); `true` if it was queued |
| `espnow.enviar_para(mac, texto)` | direct to a MAC (`"AA:BB:CC:DD:EE:01"`); `false` if the MAC is invalid |
| `espnow.enviar_todos(texto [, mac])` | repeats the send on channels 1 to 13 (broadcast, or direct if `mac` is given) and returns to the previous channel. **Wi-Fi drops for a few seconds** and comes back by itself |
| `espnow.n()`, `espnow.msg(i)` | how many messages are stored and the one at index `i` (0 = the oldest): `{"texto", "minha", "quando", "de", "envio", "espressif"}`. `envio` (for mine): 1 sending, 2 left the antenna, 3 failed. `espressif`: arrived in the esp-now component's format (otherwise, plain text) |
| `espnow.total()` | only goes up with each message - compare it to know whether something arrived |
| `espnow.janela(sim)` | `true` with the conversation on screen (receiver on, incoming messages do not become a status bar notice); `false` on closing |
| `espnow.marcar_lidas()`, `espnow.nao_lidas()` | the unread counter (mark as read; unread count) |
| `espnow.canal()`, `espnow.canal_fixo()`, `espnow.fixar_canal(c)` | the channel in use; the fixed one (0 = follows the router); set it (0, or 1 to 13) |
| `espnow.mac()`, `espnow.vistos()`, `espnow.ultimo()` | my MAC, how many devices have been heard, ms since the last one received (-1 = nothing) |

## Module `mesh`

Read access, plus one write with limits - a script does not go off talking on
a public channel: `mesh.enviar` only sends a **direct** message to a node.

| function | what it does |
|---|---|
| `mesh.estado()` | `{"ativo", "no_ar", "nos", "nao_lidas"}` |
| `mesh.n_msgs()`, `mesh.msg(i)` | the recent messages (up to 8; `0` = the newest): `{"de", "nome", "texto", "quando", "canal", "direta", "snr", "mqtt", "minha"}`, or `nil` |
| `mesh.total()` | only goes up with each message - compare it to know whether something arrived (with `# avisos: mesh`) |
| `mesh.n_nos()`, `mesh.no(i)` | the known nodes (`0` = ours): `{"num", "id", "nome", "curto", "snr", "idade", "bateria", "saltos", "mqtt", "favorito"}`, plus `"lat"`/`"lon"` when it has sent a position |
| `mesh.meu()` | our node's number |
| `mesh.enviar(numero, texto)` | direct message to node `numero`; `true` = queued. Refused (`false`): number `0`, broadcast (`0xFFFFFFFF`) or our own node; empty text or more than 200 bytes; and more than one send every 10 s across the whole device |

## Module `camera`

The live preview and the photo, through the same service as the Camera app.
[`Camera.be`](../berry_exemplos/Camera.be) is the full example.

The preview **does not go through the script**: the camera writes each frame
straight to the screen, in a full-width strip. `desenhar()` keeps arriving as
usual; the preview strip simply is not cleared, and whatever you draw on it (a
crosshair, a grid) goes on top of the image. With the camera on, the app is
redrawn every frame (~30 per second): keep `desenhar()` light.

| function | what it does |
|---|---|
| `camera.ligar([y [, altura]])` | switches the preview on from `y` to `y + altura` (default: the whole area below `y`). The top and the height are rounded to multiples of 8. Only with the app in **portrait** (`# orientacao: em_pe`, the default) |
| `camera.desligar()` | switches it off (the system also switches it off by itself when the app closes and on idle, and back on when it wakes) |
| `camera.ligada()` | is the preview on? It comes on right after the event that asked for it: by the next `desenhar()` it is already true |
| `camera.faixa()` | `[y, altura]` of the live preview (already rounded), or `nil` |
| `camera.erro()` | why it did not start (`""` = no error) |
| `camera.foto([nome])` | takes the photo (1200×1600) and writes `<nome>.bmp` to the app's folder; with no name, `foto_0001.bmp`, `foto_0002.bmp`... `false` = camera off or one already in progress |
| `camera.foto_estado()` | `""`, `"capturando"` (capturing), `"gravando"` (saving), `"ok"` or `"falhou"` (failed) |
| `camera.ultima()` | the name of the last file written |

Plus Berry's standard library: `string`, `math`, `json`, `time`, `global`...

## Module `gnss`

The **satellites** the GPS receiver on the Grove port can see (the GSV
sentences) - what draws a sky like the one in
[`Azimutal.be`](../berry_exemplos/Azimutal.be). The position is still in
`sistema.gps()`.

| function | what it does |
|---|---|
| `gnss.ligar()` | starts receiving the constellation. `false` = the GPS already has another subscriber (only one at a time) |
| `gnss.desligar()`, `gnss.ligado()` | releases it / says whether it is receiving. Deleting the app's machine releases it automatically |
| `gnss.satelites()` | list of `{"sis", "prn", "elev", "azim", "snr", "pos", "idade"}`, or `nil` without a complete round. `pos` false = seen but no position (do not draw it); `idade` = seconds since the last observed position |
| `gnss.versao()` | changes with every new round |
| `gnss.tempo()` | date and time from the **GPS itself**, in UTC: `{"valida", "fix", "ano", "mes", "dia", "hora", "min", "seg"}` |

- **Call `gnss.satelites()` at least every 1-2 s** (a `# periodo: 1000`, or
  `sistema.fundo(true, 1000)`). It is the script that drains the GPS queue -
  it holds about 2 s; once full, whatever arrives later is lost and the
  snapshot goes stale.
- **To record, use only fresh positions** (small `idade`): when a GSV arrives
  without a position, the parser holds on to the last one - recording that
  would repeat the same point.

## Module `musica` (music)

The **system player** (`services/tab5_musica`) - the same one as the Music
app. The script is only the front end: the service finds the card's music
folder, lists it, plays **with the app closed**, shows what is playing in the
status bar and in the notification centre's mini player (with the X that
stops it), shuts itself down after 15 min paused and computes the spectrum.
Not a single line of audio code in the script. Full example:
[`Tocador.be`](../berry_exemplos/Tocador.be) (player).

| function | what it does |
|---|---|
| `musica.pasta()` | the name of the music folder (`"Music"`, `"Música"`...), or `nil` without a card |
| `musica.pendrive()` | the name of the USB flash drive on USB-A, or `nil` without one. Its songs: `musica.lista("/usb")`, `musica.tocar("/usb", i)` - the whole flash drive is the "folder" (subfolders are accepted: `"/usb/Rock"`) |
| `musica.lista([sub])` | the contents of the subfolder (`""` = the root): list of `{"nome", "titulo", "pasta", "caminho"}`, folders first. `titulo` = the name without the extension; `caminho` = what `playlist_por` and `duracao` take. `nil` = no card or the folder does not exist |
| `musica.tocar(sub, i)` | plays folder `sub` starting from item `i` of `lista(sub)` (it has to be music). `true` = it worked. Tapping the title in the mini player opens **this** app |
| `musica.pausar()` | play/pause |
| `musica.proxima()`, `musica.anterior()` | skip (next, previous). Previous after 3 s goes back to the start of the same track |
| `musica.ir_para(ms)` | seek |
| `musica.parar()` | switches the player off |
| `musica.estado()` | `{"ativo", "tocando", "titulo", "artista", "album", "pos", "dur", "idx", "n", "pasta", "playlist", "caminho", "aleatorio", "repetir", "erro", "kbps", "hz"}` - `pos`/`dur` in ms, of what is **coming out** of the speaker |
| `musica.espectro()` | the `musica.FAIXAS` (32) bars, 0..255, bass first, of the sound playing now; `nil` = nothing playing |
| `musica.volume([v])` | the system volume, 0..100 (the same as in Settings) |
| `musica.cor([c])` | the player's accent colour (`tela.cor`): the status bar pill and the mini player |
| `musica.aleatorio([b])` | shuffle the queue (the current track keeps playing); returns the resulting setting |
| `musica.repetir([r])` | `0` plays to the end and stops, `1` repeats the whole queue, `2` repeats the same track |
| `musica.playlists()` | `[{"nome", "id"}]` - `nome` for display, `id` for the functions below; `Favoritas` (Favourites) first |
| `musica.playlist(id)` | the tracks: `[{"caminho", "titulo"}]`; `nil` = does not exist |
| `musica.playlist_criar(nome)` | creates an empty one; returns the `id`, or `nil` (already exists, no card) |
| `musica.playlist_apagar(id)` | deletes the `.m3u` (the songs stay) |
| `musica.playlist_por(id, caminho)`, `musica.playlist_tirar(id, caminho)` | add / remove; `caminho` = the one from `lista()` or `playlist()`; `false` = it was already there / was not there |
| `musica.playlist_tocar(id, [i])` | plays the playlist from track `i` (without `i`: from the start, or a random one with shuffle on) |
| `musica.capa()` | the cover art file of what is playing (JPEG/PNG), or `nil` |
| `musica.duracao(caminho)` | how long it lasts, in ms, without playing it (reads only the header) |

- **Paths**: `sub` and `caminho` are relative to the card's music folder
  (`"Rock"`, `"Rock/x.mp3"`) or absolute on a disk (`"/usb/Rock/x.mp3"`, the
  flash drive). Both fit in the same playlist; a track on a flash drive that
  is not plugged in is skipped. To know when a flash drive is plugged in or
  removed, ask for `# avisos: volumes`.
- **Only one player on the device.** `musica.tocar` from an app replaces the
  queue of whoever was playing (including the Music app).
- **The spectrum is only computed while someone is reading it**: call
  `musica.espectro()` every frame (`sistema.periodo(33)` while playing). Once
  you stop reading - window closed, screen off - the service stops computing
  it by itself within 1 s.
- **No need for `sistema.fundo()`**: the music carries on with the window
  closed thanks to the service.

## Module `rede` (network)

Internet (HTTP and HTTPS) **without waiting**: `rede.pedir(url [, max_bytes])`
only starts the request (`true`/`false`; one request at a time across the
whole device) and the download runs in the background. In `desenhar()`, check
`rede.estado()` (`"parado"` (idle), `"baixando"` (downloading), `"pronto"`
(ready), `"erro"` (error)) and read `rede.resposta()` -
`{"status", "texto", "tipo", "final", "ms", "erro"}` - which releases the
result. Also: `rede.ip()` (`{"conectado", "ip", "mascara", "gateway", "dns1", "dns2", "ipv6", "mac", "nome"}`),
`rede.cancelar()` and `rede.codificar(texto)` (URL-encodes text). Body limit
64 KB by default, 256 KB at most. When the app closes, the request is
cancelled. Details and an example in [BERRY_API.md](BERRY_API.md).

## Module `ajustes` (settings)

The person's preferences. `ajustes.brilho([pct])` (brightness) and
`ajustes.volume([pct])` read and change (0 to 100); `ajustes.tela_segundos()`
(screen timeout), `ajustes.casa()` (home, `[lat, lon]` or `nil`) and
`ajustes.fuso()` (time zone, minutes from UTC) only read.

## Performance: what each thing costs

Measured on the Tab5 with [`Bench.be`](../berry_exemplos/Bench.be) (run it
yourself too: it shows the table on screen and writes it to the log). A smooth
frame at 60 Hz has **~16 ms**; the whole of `desenhar()` has to fit in that.

| operation | cost |
|---|---|
| arithmetic, variable, `if` | < 1 µs |
| calling the system (`tela.cor`, `sistema.ms`...) | ~4 µs |
| building text (`"x" + str(n)`) | ~15 µs |
| **creating** a list `[1,2,3]` / map `{"a":1}` | ~35 / ~23 µs |
| `tela.texto`, 20 characters (size 3 / size 5) | ~340 / ~710 µs |
| `tela.botao` | ~560 µs |
| rectangle 50×40 / 300×28 | ~36 / ~95 µs |
| circle r24, rounded corners 120×80 | ~90 / ~165 µs |
| 100 px line | ~60 µs |

What this means in practice:

- **Drawing costs per pixel**, not per call: one large rectangle costs more
  than ten small ones. Do not paint a background that is already black (the
  area arrives cleared).
- **Text is the most expensive drawing.** Some 15 texts per frame already come
  to ~4 ms. On an animated screen, write only what changes; a large fixed label
  is expensive every time.
- **Do not create lists and maps inside `desenhar()`**: whatever is fixed
  (colours, tables, names) goes outside the functions and is created once.
- **Use a period only where something animates.** `sistema.periodo(16)` on the
  screen that moves and `0` on the others: when idle, the app costs nothing
  (see `Mostruario.be`).
- `berry times` (`berry tempos`) in the console shows your app from the
  inside: how much of the frame is drawing, how much is script, and which
  `tela` function weighs the most.

<a id="parte-de-fundo"></a>

## Background part

An app can keep working with the window closed - recording, monitoring,
notifying - in the same way as the Azimutal and Trilha (track) apps in C:

```
def fundo()                       # one short round of the work
  amostras.push(sistema.bateria())
  cartao.anexar("log.csv", sistema.hora() + "," + str(sistema.bateria()) + "\n")
  sistema.fundo_estado(str(size(amostras)) + " samples")
end

def tocar(x, y)
  if tela.dentro(x, y, 40, 40, 300, 90)
    sistema.fundo(!sistema.fundo(), 10000)   # RECORD / STOP, every 10 s
  end
end
```

- **The same machine.** The window and the background part see the **same
  variables**: what `fundo()` stores, `desenhar()` shows. The two never run at
  the same time (a single task for all of Berry), so there is no lock to add.
- **Closing the window does not delete the machine** while the background part is
  running: `fechar()` is called, the layers are returned and garbage is
  collected straight away. On reopening, the code outside the functions does
  **not** run again - only `abrir()`: the state carries on. When the
  background part stops with the window closed, the machine is deleted.
- **In the background there is no window.** `tela`, layers, `teclado`,
  `camera`, `sistema.bip`, `acordado`, `periodo` and `acumular` raise the
  error `fundo_error`. `cartao`, `sistema.avisar`, `espnow`, `mesh` and the
  readings work.
- **Deadline of 200 ms** per `fundo()` call: while it runs, a touch on the
  screen waits. Do a short round and let the interval (`ms`) do the rest.
- **Always visible.** While it runs, a pill in the status bar shows the app's
  name (and the `fundo_estado`). It is stopped by the app itself
  (`sistema.fundo(false)`, also from inside `fundo()`), from the console
  (`background stop <nome>`, i.e. `fundo parar <nome>`) or by an error in the
  script.
- **Cost:** no new task - the loop is a slot in the system pool and Berry
  runs on its usual task. What remains is the machine (tens of KB in PSRAM). Up to
  4 background parts at the same time, counting the system's own.

## Limits and safety

- **Each app has its own machine.** It is created on opening and deleted on closing
  (or when the background part stops): one app cannot see another's
  variables, and closing returns all the memory (Berry uses the PSRAM, 32 MB).
- **Memory budget.** All the Berry machines together (apps, background parts,
  console) cannot exceed **4 MB**. Past that, the script stops with
  `memory_error` - a loop that fills a list does not eat the camera's and the
  system's memory. A typical app uses 10 to 50 KB (`sistema.memoria()` and
  `berry mem` show it). Layers (`tela.camada`) are separate, with their own
  limit.
- **Deadline.** Each call (`desenhar`, `tocar`...) gets 2 s; the code outside
  the functions gets 5 s. Past that, the script stops with the error
  `tempo_esgotado` (timed out) - which is **not** caught by `try/except`: a
  loop that swallows errors stops too.
- **An error does not bring the device down.** The message appears on the
  app's screen (with the line number for syntax errors); close it, fix the
  file and open it again.
- **A script is not C++.** It works well for screens, logic and control. Fast
  games or heavy processing are still C++ apps (see
  [ESCREVENDO_APPS.md](ESCREVENDO_APPS.md)).

## Checking on the Mac before copying

```
testes/checar_be.sh                  (all of berry_exemplos/)
testes/checar_be.sh MeuApp.be
```

With the same Berry as the firmware, it **compiles** each file (syntax,
undeclared names) and then **runs** it with mock modules
([`testes/berry_falso.be`](../testes/berry_falso.be)): opens the app, taps
every point of the top strip (the tabs) and a grid over the whole screen,
sends keys and drags the finger, in landscape and portrait. Wrong types
(`tela.texto(5, ...)`), an index outside the list, drawing outside
`desenhar()` show up with the file and line - on the Mac, not on the device.

## Testing from the console

The serial console runs Berry directly, on the same machine between commands:

```
berry 1+2*3
berry "def dobro(x) return x*2 end"
berry dobro(21)
berry run apps/Berry/Ola.be
berry reload                   (rereads the folder: new icons without a restart)
berry times                    (how long the script takes in each event)
berry times reset              (the same, and restarts the count)
berry mem                      (memory of each live machine, and the budget)
```

(The Portuguese forms `berry rodar`, `reler`, `tempos` and `tempos zerar` also
work.) `berry times` shows, for the open app, how many times each function ran
and its average and worst time (Berry only, not the system). Use it to find
what is heavy: a 3 ms `desenhar()` animates smoothly; a 30 ms one already
drops frames.
