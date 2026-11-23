# M5tabOS Berry API quick reference

Everything a Berry app can call, on one page. The explanation of each part,
with examples, is in [BERRY.md](BERRY.md); the example apps are in
[`berry_exemplos/`](../berry_exemplos) ([`Mostruario.be`](../berry_exemplos/Mostruario.be)
uses the whole API).

The API names are Portuguese, as in the system (`tela` = screen, `cartao` =
card, `desenhar` = draw, `tocar` = tap); the meaning of each is given here.

This list is taken from the modules' registration tables
(`berry_modulos_registrar`, [`main/services/berry_apps.cpp`](../main/services/berry_apps.cpp);
each module lives in [`main/services/berry/`](../main/services/berry)).
A new function in a module has to go in both places: here and in BERRY.md.

## Learning the Berry language

| Where | What |
|---|---|
| <https://berry.readthedocs.io> | The language's **official manual**: types, `var`, `def`, `class`, `for`, `try`, lists, maps, standard modules |
| <https://github.com/berry-lang/berry> | The source code and the README with the syntax on one page |
| <https://tasmota.github.io/docs/Berry/> | A practical introduction with examples (from the Tasmota project). **Note:** the hardware modules there (`gpio`, `wire`, `tasmota`, `lv`...) **do not exist** here - on M5tabOS the hardware goes through the modules on this page |

### The language in 20 lines

```berry
var n = 0                       # variable (32-bit integer; real is float)
var name = "Tab5"               # text (UTF-8)
var l = [1, 2, 3]               # list:  l.push(4)  l[0]  size(l)
var m = {"a": 1, "b": 2}        # map:   m["c"] = 3  m.contains("a")  m.keys()

def double(x) return x * 2 end  # function (declare it BEFORE whoever calls it)

if n > 0 print("positive")
elif n == 0 print("zero")
else print("negative") end

for i: 0 .. 9 n += i end        # 0 to 9, inclusive
for x: l print(x) end           # each list item
while n > 0 n -= 1 end

var s = format("%d items, %.1f%%", 3, 12.5)   # like printf
try
  var v = json.load(cartao.ler("data.json"))
except .. as e, msg
  print("error: " + msg)
end

class Point var x, y def init(x, y) self.x = x self.y = y end end
```

## The functions the system calls in your script

All optional. Define only the ones you use.

| Function | When |
|---|---|
| `abrir()` | the app opened |
| `desenhar(tudo)` | time to paint (`tudo` = the area arrived cleared) |
| `tocar(x, y)` | a tap (click) |
| `dedo(x, y, pressionado)` | the finger on the screen, every 20 ms; `false` when lifted |
| `tecla(texto, codigo)` | a key from the physical or on-screen keyboard |
| `aviso(nome)` | something arrived from a service asked for in `# avisos:` |
| `fechar()` | the window is about to close |
| `fundo()` | the background part (with `sistema.fundo(true)`) |

**File header** (comments at the top):
`# nome:` (name) · `# cor: 0x07E0` (colour) · `# periodo: ms` (period) ·
`# avisos: espnow, mesh, gps, volumes, arquivos` (notices) ·
`# orientacao: em_pe | deitado | girar | fixo` (portrait | landscape | rotate | fixed) -
with the physical keyboard attached every app goes landscape, except `fixo`
(always portrait: the ones that use `camera`)

## `tela` - drawing

Only inside `desenhar()` (or with `tela.alvo(layer)`). Coordinates of the app area.

| Function | Returns |
|---|---|
| `tela.largura()` · `tela.altura()` | int (px): width · height |
| `tela.limpar([cor])` | - (clear) |
| `tela.texto(txt, x, y [, tam [, cor [, alinhamento]]])` | - (text, size, colour, alignment) |
| `tela.medir(txt [, tam])` | int (width in px) |
| `tela.cortar(txt, largura [, tam])` | the text cut with `...` |
| `tela.retangulo(x, y, l, a, cor [, cheio])` | - (rectangle, filled) |
| `tela.arredondado(x, y, l, a, raio, cor [, cheio])` | - (rounded rectangle) |
| `tela.circulo(x, y, r, cor [, cheio])` | - (circle) |
| `tela.triangulo(x0, y0, x1, y1, x2, y2, cor [, cheio])` | - (triangle) |
| `tela.linha(x0, y0, x1, y1, cor)` | - (line) |
| `tela.pixel(x, y, cor)` | - |
| `tela.botao(x, y, l, a, txt [, cor])` | - (button) |
| `tela.dentro(x, y, rx, ry, rl, ra)` | bool: is the point inside (works outside `desenhar` too) |
| `tela.cor(r, g, b)` | int (RGB565 colour) |
| `tela.camada(l, a)` | layer (a stored image, up to 8 per app) |
| `tela.alvo(c)` · `tela.alvo()` | draw into the layer / back to the screen |
| `tela.colar(c, x, y [, rx, ry, rl, ra])` | - (paste a layer) |
| `tela.liberar(c)` | - (free a layer) |

Constants: `tela.ESQUERDA` `CENTRO` `DIREITA` (left, centre, right) · `tela.PRETO`
`BRANCO` `VERMELHO` `VERDE` `AZUL` `AMARELO` `CIANO` `LARANJA` `CINZA`
`CINZA_CLARO` `ROXO` `ROSA` (black, white, red, green, blue, yellow, cyan,
orange, grey, light grey, purple, pink)

## `sistema` - time, power, sensors, notices

| Function | Returns |
|---|---|
| `sistema.redesenhar()` | - (ask for a redraw) |
| `sistema.periodo([ms])` | int (the current period) |
| `sistema.acumular([sim])` | bool |
| `sistema.dt()` | real (s since the previous frame) |
| `sistema.ms()` · `sistema.us()` | int (since boot) |
| `sistema.agora()` | int (UTC epoch, 0 with no clock) |
| `sistema.hora()` · `sistema.data()` | `"14:05:09"` · `"14/10/2026"` (or `""`) |
| `sistema.hora_de(epoch)` | `"14:05"` (or `"2:05pm"` with the 12-hour clock), in the local time zone with daylight saving |
| `sistema.bateria()` | int (%) |
| `sistema.energia()` | `{"pct", "volts", "ma"}` |
| `sistema.imu()` · `sistema.giro()` | `[x, y, z]` (g · degrees/s) or `nil` |
| `sistema.repouso()` | bool (screen off) |
| `sistema.recursos()` | `[{"recurso", "quem", "s"}]` - what the system is using and for whom (a camera the script turned on shows its name) |
| `sistema.gps()` | `{"fix", "lat", "lon", "alt", "sats"}` or `nil` |
| `sistema.wifi()` | text |
| `sistema.rede()` | `{"ssid", "ip", "rssi", "texto"}` |
| `sistema.bip(hz, ms)` | - (beep) |
| `sistema.avisar(txt [, nivel])` · `sistema.avisar()` | puts / removes a status-bar notice. `nivel`: `"info"` (nothing needs the user), `"atencao"` (attention, the default) or `"urgente"` (needs action); any other text counts as `"atencao"` |
| `sistema.acordado(sim)` | - (the screen does not turn off) |
| `sistema.log(txt)` | - (serial log) |
| `sistema.fundo(sim [, ms])` · `sistema.fundo()` | on/off · bool (background part) |
| `sistema.fundo_estado(txt [, n])` | - (the text of the background chip) |
| `sistema.memoria()` | `{"app", "berry", "orcamento"}` (bytes) |

## `teclado` - keyboard

| Function | Returns |
|---|---|
| `teclado.abrir([tipo])` · `teclado.fechar()` | - (field type: `"texto"` (default), `"ascii"`, `"senha"` (password), `"numero"`, `"busca"` (search), `"paragrafo"`, `"codigo"` (code), `"comando"`) |
| `teclado.aberto()` · `teclado.fisico()` | bool (open · physical keyboard attached) |

Constants: `teclado.ENTER` `APAGAR` (backspace) `ESC` `TAB` `ESQUERDA` `DIREITA` `CIMA` `BAIXO` (arrows) `DEL`

## `cartao` - the app's files

Only the app's folder: `/sdcard/apps/Berry/<app name>/`. File name only, no folder.

| Function | Returns |
|---|---|
| `cartao.ler(nome)` | text (up to 64 KB) or `nil` (read) |
| `cartao.gravar(nome, texto)` | bool (write) |
| `cartao.anexar(nome, texto)` | bool (append) |
| `cartao.existe(nome)` | bool (exists) |
| `cartao.lista()` | list of names |

## `arquivos` - the disks (sees everything, changes only its own folder)

Outside the app folder: only list and **copy in**. Inside: everything.

| Function | Returns |
|---|---|
| `arquivos.volumes()` | `[{"raiz", "tipo", "nome", "disponivel"}]` |
| `arquivos.pasta()` | the path of the app folder |
| `arquivos.lista(pasta [, filtro])` | `[{"nome", "tipo", "pasta", "bytes", "data", "caminho"}]` or `nil` |
| `arquivos.copiar(origem_ou_lista [, subpasta])` | `true` (started) or the text of the reason |
| `arquivos.estado()` | `{"fase", "pct", "itens", "feitos", "atual", "erro", "meu"}` |
| `arquivos.cancelar()` | - |
| `arquivos.ler(caminho)` | text (up to 64 KB) - only in the app folder |
| `arquivos.apagar(c)` · `renomear(c, nome)` · `criar_pasta(nome)` | bool (delete · rename · make folder) - only in the app folder |

## `musica` - the system music player

| Function | Returns |
|---|---|
| `musica.pasta()` | name of the music folder or `nil` |
| `musica.pendrive()` | name of the USB-A flash drive or `nil` |
| `musica.lista([sub])` | `[{"nome", "titulo", "pasta", "caminho"}]` or `nil` |
| `musica.tocar(sub, i)` | bool (play) |
| `musica.pausar()` · `proxima()` · `anterior()` · `parar()` | - (pause · next · previous · stop) |
| `musica.ir_para(ms)` | - (seek) |
| `musica.estado()` | `{"ativo", "tocando", "titulo", "artista", "album", "pos", "dur", "idx", "n", "pasta", "playlist", "caminho", "aleatorio", "repetir", "erro", "kbps", "hz"}` |
| `musica.espectro()` | a list of `musica.FAIXAS` (32) values 0..255, or `nil` |
| `musica.volume([v])` | int 0..100 |
| `musica.cor([c])` | int |
| `musica.aleatorio([b])` | bool (shuffle) |
| `musica.repetir([r])` | int (0 no, 1 queue, 2 the same track) |
| `musica.playlists()` | `[{"nome", "id"}]` |
| `musica.playlist(id)` | `[{"caminho", "titulo"}]` or `nil` |
| `musica.playlist_criar(nome)` | id or `nil` |
| `musica.playlist_apagar(id)` | bool |
| `musica.playlist_por(id, caminho)` · `playlist_tirar(id, caminho)` | bool (add · remove) |
| `musica.playlist_tocar(id [, i])` | bool |
| `musica.capa()` | path of the JPEG/PNG cover or `nil` |
| `musica.duracao(caminho)` | int (ms) |

Paths: relative to the music folder (`"Rock/x.mp3"`) or absolute on a disk (`"/usb/Rock/x.mp3"`).

## `idioma` - the interface language

The user chooses it, in Settings > Language; the app only reads it.

| Function | Returns |
|---|---|
| `idioma.atual()` | the code: `"en"`, `"pt"`, `"fr"`... |
| `idioma.nome()` | the name in the language itself: `"English"`, `"Português"`... |
| `idioma.tr(texto)` | the system phrase in the current language (`"Cancelar"` → `"Cancel"`); without a translation, the same |
| `idioma.escolher(pt, en)` | the app's phrase: `pt` in Portuguese, `en` in any other language |

```berry
tela.botao(20, 20, 300, 80, idioma.escolher("Começar", "Start"), tela.VERDE)
```

## `transferencia` - copy and paste text

The same clipboard as the system: what is copied in Mesh, ESP-NOW or Notes
appears here, and the other way round. One text, up to 4 KB.

| Function | Returns |
|---|---|
| `transferencia.copiar(texto)` | `true` if stored |
| `transferencia.colar()` | the copied text (`""` = empty) |

## `rede` - internet (HTTP and HTTPS)

The script **never waits for the network**: `rede.pedir()` only starts the
request and returns at once; the download runs in the background. Look at
`rede.estado()` in `desenhar()` (the app is redrawn when the request ends)
and read `rede.resposta()` when it is ready. One request at a time on the
whole device, and the answer is only for whoever asked. When the app closes,
the request is cancelled and an unread answer is thrown away.

| Function | Returns |
|---|---|
| `rede.ip()` | `{"conectado", "ip", "mascara", "gateway", "dns1", "dns2", "ipv6", "mac", "nome"}` or `nil` with no Wi-Fi interface |
| `rede.pedir(url [, max_bytes])` | bool - `true` = started. `url` starts with `http://` or `https://` (up to 255 characters; redirects are followed). `max_bytes`: 64 KB by default, at most 256 KB (anything beyond is cut). `false` = a request is already running (from any app), there is an unread answer from ANOTHER app, or the URL is invalid. An unread answer of the app itself is discarded |
| `rede.estado()` | `"parado"` (idle), `"baixando"` (downloading), `"pronto"` (ready) or `"erro"` |
| `rede.resposta()` | `{"status", "texto", "tipo", "final", "ms", "erro"}` when `"pronto"` or `"erro"`, and **frees** the result (the state goes back to `"parado"`); `nil` with no answer. `status` 0 = did not connect (`erro` says why); `tipo` = Content-Type; `final` = the URL after redirects |
| `rede.cancelar()` | - (gives up the request, or throws away the unread answer) |
| `rede.codificar(texto)` | the text ready for a URL (`"a b&c"` → `"a+b%26c"`) |

```berry
# nome: Weather
var temp = ""

def tocar(x, y)                      # a tap starts the request
  var q = rede.codificar("Melbourne")
  if !rede.pedir("https://wttr.in/" + q + "?format=3") temp = "busy" end
end

def desenhar(tudo)
  var e = rede.estado()
  if e == "baixando"
    temp = "fetching..."
  elif e == "pronto" || e == "erro"
    var r = rede.resposta()          # reads and frees
    temp = e == "pronto" ? r["texto"] : r["erro"]
  end
  tela.texto(temp, 20, 20, 3, tela.BRANCO)
end
```

## `ajustes` - the user's preferences

The script may change brightness and volume (as the top bar does); the rest is read only.

| Function | Returns |
|---|---|
| `ajustes.brilho([pct])` | int 0..100 - with an argument, applies at once and saves. On the eye's scale, the same as the Settings bar; 0 does not turn the screen off (there is a minimum) |
| `ajustes.volume([pct])` | int 0..100 - the same as `musica.volume` |
| `ajustes.tela_segundos()` | int - seconds without a touch until the screen turns off |
| `ajustes.casa()` | `[lat, lon]` or `nil` if the user has not set a home |
| `ajustes.fuso()` | int - minutes from UTC in effect now, daylight saving included (`600` = UTC+10) |
| `ajustes.relogio_12h()` | bool - the user wants the 12-hour clock |

## `camera`

| Function | Returns |
|---|---|
| `camera.ligar([y [, altura]])` · `camera.desligar()` | bool · - (on · off) |
| `camera.ligada()` | bool |
| `camera.faixa()` | `[y, altura]` or `nil` |
| `camera.erro()` | text |
| `camera.foto([nome])` | bool (saves a `.bmp` in the app folder) |
| `camera.foto_estado()` | `""`, `"capturando"`, `"gravando"`, `"ok"`, `"falhou"` |
| `camera.ultima()` | name of the last file |

## `espnow`

| Function | Returns |
|---|---|
| `espnow.estado()` | `{"codigo", "texto"}` |
| `espnow.pronto()` | bool |
| `espnow.enviar(texto)` | bool (send) |
| `espnow.enviar_para(mac, texto)` | bool - straight to a MAC (`"AA:BB:CC:DD:EE:01"`) |
| `espnow.enviar_todos(texto [, mac])` | bool - on every channel (1 to 13); without `mac`, broadcast. Wi-Fi drops for a few seconds and comes back by itself |
| `espnow.n()` · `espnow.msg(i)` | int · `{"texto", "minha", "quando", "de", "envio", "espressif"}` |
| `espnow.total()` | int (goes up with each message) |
| `espnow.janela(sim)` | - |
| `espnow.nao_lidas()` · `espnow.marcar_lidas()` | int · - (unread · mark read) |
| `espnow.canal()` · `canal_fixo()` · `fixar_canal(c)` | int |
| `espnow.mac()` · `vistos()` · `ultimo()` | text · int · int (ms) |

## `mesh` (Meshtastic, reading and direct messages)

| Function | Returns |
|---|---|
| `mesh.estado()` | `{"ativo", "no_ar", "nos", "nao_lidas"}` |
| `mesh.n_msgs()` · `mesh.msg(i)` | int · `{"de", "nome", "texto", "quando", "canal", "direta", "snr", "mqtt", "minha"}` |
| `mesh.total()` | int |
| `mesh.n_nos()` · `mesh.no(i)` | int · `{"num", "id", "nome", "curto", "snr", "idade", "bateria", "saltos", "mqtt", "favorito"[, "lat", "lon"]}` |
| `mesh.meu()` | int (our node) |
| `mesh.enviar(numero, texto)` | bool - a **direct** message to one node (the `num` of `mesh.no(i)`); `true` = it joined the send queue |

`mesh.enviar` has limits, so a script cannot go chatting on a public channel:
only a direct message to **one** node (never broadcast); it refuses (`false`)
the number `0`, `0xFFFFFFFF` (broadcast) and our own node, empty text or text
over 200 bytes, and allows **at most one every 10 s on the whole device**
(across all apps).

The Berry integer here is 32-bit signed: a node number above 2147483647 shows
up **negative** in `mesh.no(i)["num"]` and `mesh.meu()` (broadcast
`0xFFFFFFFF` is `-1`). Use the number as it came; written out
(`4294967295`) Berry cuts it to 2147483647, which is another node.

## `gnss` - GPS satellites on Grove

| Function | Returns |
|---|---|
| `gnss.ligar()` · `desligar()` · `ligado()` | bool |
| `gnss.satelites()` | `[{"sis", "prn", "elev", "azim", "snr", "pos", "idade"}]` or `nil` |
| `gnss.versao()` | int (changes every round) |
| `gnss.tempo()` | `{"valida", "fix", "ano", "mes", "dia", "hora", "min", "seg"}` (UTC) |

## Berry standard library available

| Module / function | What for |
|---|---|
| `print`, `str`, `int`, `real`, `bool`, `number`, `size`, `type`, `classname`, `isinstance`, `format`, `assert`, `compile` | base functions |
| `import string` | `format`, `find`, `split`, `replace`, `count`, `toupper`, `tolower`, `startswith`, `endswith`, `byte`, `char`, `hex`, `tr`, `escape` |
| `import math` | `sin` `cos` `tan` `atan2` `sqrt` `pow` `exp` `log` `log10` `abs` `floor` `ceil` `round` `min` `max` `deg` `rad` `rand` `srand` `pi` ... |
| `import json` | `json.load(text)` · `json.dump(value)` |
| `import time` | `time.time()` · `time.dump(t)` · `time.clock()` |
| `import global` | read/create global variables by name |
| `import gc` | `gc.collect()` · `gc.allocated()` |
| `import strict` | forces variables to be declared with `var` |

Lists: `push` `pop` `insert` `remove` `find` `size` `resize` `clear` `concat` `reverse` `copy` `keys`.
Maps: `find` `contains` `insert` `remove` `size` `keys`.

**Not available here:** `open()`, `load("file")`, `save()` (files only through
the `cartao` module) and the `os` module. Do not use `input()`: nobody types
on an app's console (it returns `""`). There is no `introspect` module either,
nor `bytes` pointing at an address (`bytes(ptr, n)`).

## Limits

- Memory: all Berry machines together get at most **4 MB**; beyond that, `memory_error`.
- Time: **2 s** per event, **5 s** to open, **200 ms** per `fundo()`; beyond that, `tempo_esgotado` (timeout; not caught by `try/except`).
- Up to 16 apps in `apps/Berry/`, 8 layers per app.
- Check on a computer before copying: `testes/checar_be.sh MyApp.be`.
