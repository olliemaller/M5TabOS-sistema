# nome: Ola Berry
# cor: 0xFD20
# avisos: volumes
#
# THE FIRST APP IN BERRY - a tour of what a script can do on M5tabOS.
# Each tab shows one possibility, in a few lines:
#
#   HOME     time, date, battery and network (the system provides them)
#   TOUCH    buttons, beep, a status-bar notice and a counter saved on the card
#   SENSOR   the accelerometer: a bubble that moves when the device tilts
#   DRAW     your finger painting on the screen
#   DISKS    the SD card and the USB flash drive (the arquivos module)
#
# To make your own: copy this file to /sdcard/apps/Berry/ under another
# name, change it, and it appears on the home screen. Language manual:
# https://berry.readthedocs.io - and every system function is in
# docs/BERRY_API.md.

import json

# ---- the app's state ---------------------------------------------------------
# Whatever sits OUTSIDE the functions runs once, on opening.
# In the language chosen in Settings: idioma.escolher(portuguese, english).
var ABAS = [idioma.escolher("INÍCIO", "HOME"), idioma.escolher("TOQUE", "TOUCH"), "SENSOR",
            idioma.escolher("DESENHO", "DRAW"), idioma.escolher("DISCOS", "DISKS")]
var aba = 0
var toques = 0        # saved on the card (TOUCH tab)
var pontos = []       # the drawing (DRAW tab): [x, y] of each point
var discos = []       # the DISKS tab

# Read what was saved last time: cartao.ler returns nil if it doesn't exist.
var salvo = cartao.ler("ola.json")
if salvo != nil
  var d = json.load(salvo)
  if d != nil && d.contains("toques") toques = d["toques"] end
end

def salvar()
  cartao.gravar("ola.json", json.dump({"toques": toques}))
end

# ---- the tabs at the top -----------------------------------------------------
var ABA_A = 80        # height of the row of tabs

def largura_aba() return tela.largura() / size(ABAS) end

def desenhar_abas()
  var l = largura_aba()
  for i: 0 .. size(ABAS) - 1
    tela.botao(i * l + 4, 8, l - 8, ABA_A - 8, ABAS[i], i == aba ? tela.LARANJA : tela.CINZA)
  end
end

# Each tab asks for the redraw rate it needs: the clock, once a second;
# the sensor bubble, 20 times a second; the rest, only when something changes
# (0) - when idle, the app uses no battery.
def trocar_aba(i)
  aba = i
  sistema.periodo(i == 0 ? 1000 : (i == 2 ? 50 : 0))
  if i == 4 discos = arquivos.volumes() end
end

# ---- HOME --------------------------------------------------------------------
def desenhar_inicio(y)
  tela.texto(idioma.escolher("Olá do Berry!", "Hello from Berry!"), 40, y, 5, tela.LARANJA)
  var h = sistema.hora()
  tela.texto(h != "" ? h : idioma.escolher("relógio sem acerto", "clock not set"), 40, y + 100, 6)
  tela.texto(sistema.data(), 40, y + 170, 3, tela.CINZA_CLARO)
  var e = sistema.energia()
  tela.texto(idioma.escolher("Bateria: ", "Battery: ") + str(e["pct"]) + "%" + (e["ma"] > 0 ? idioma.escolher("  (carregando)", "  (charging)") : ""), 40, y + 250, 3)
  var r = sistema.rede()
  tela.texto("Wi-Fi: " + (r["ip"] != "" ? r["ssid"] + "  " + r["ip"] : r["texto"]), 40, y + 300, 3)
  tela.texto(idioma.escolher("Toque nas abas acima para ver mais.", "Tap the tabs above to see more."), 40, y + 380, 2.5, tela.CINZA_CLARO)
end

# ---- TOUCH -------------------------------------------------------------------
def desenhar_toque(y)
  tela.texto(idioma.escolher("Toques: ", "Taps: ") + str(toques), 40, y, 5)
  tela.texto(idioma.escolher("(fica salvo no cartão: feche e abra de novo)", "(saved on the card: close and open again)"), 40, y + 70, 2.5, tela.CINZA_CLARO)
  tela.botao(40, y + 130, 300, 100, idioma.escolher("+1 e bip", "+1 and beep"), tela.VERDE)
  tela.botao(380, y + 130, 300, 100, idioma.escolher("Avisar", "Notify"), tela.AMARELO)
  tela.botao(40, y + 260, 300, 100, idioma.escolher("Zerar", "Reset"), tela.VERMELHO)
end

def tocar_toque(x, y, y0)
  if tela.dentro(x, y, 40, y0 + 130, 300, 100)
    toques += 1
    sistema.bip(880, 60)
    salvar()
  elif tela.dentro(x, y, 380, y0 + 130, 300, 100)
    # A notice in the status bar - tapping it (in the notification centre) opens this app.
    sistema.avisar(idioma.escolher("Olá: ", "Hello: ") + str(toques) + idioma.escolher(" toques", " taps"))
  elif tela.dentro(x, y, 40, y0 + 260, 300, 100)
    toques = 0
    salvar()
    sistema.avisar()   # clears the notice
  end
end

# ---- SENSOR ------------------------------------------------------------------
def desenhar_sensor(y)
  var cx = tela.largura() / 2
  var cy = y + 260
  tela.circulo(cx, cy, 200, tela.CINZA, false)
  tela.circulo(cx, cy, 100, tela.CINZA, false)
  var g = sistema.imu()   # [x, y, z] in g, or nil
  if g == nil
    tela.texto(idioma.escolher("sem acelerômetro", "no accelerometer"), 40, y, 3, tela.VERMELHO)
    return
  end
  tela.circulo(cx + int(g[0] * -180), cy + int(g[1] * 180), 26, tela.VERDE)
  tela.texto(format("x %.2f   y %.2f   z %.2f", g[0], g[1], g[2]), 40, y + 500, 3)
  tela.texto(idioma.escolher("Incline o aparelho.", "Tilt the device."), 40, y, 3, tela.CINZA_CLARO)
end

# ---- DRAW --------------------------------------------------------------------
# dedo(x, y, pressionado) arrives every 20 ms while a finger is on the screen.
def desenhar_desenho(y)
  tela.texto(idioma.escolher("Desenhe com o dedo.", "Draw with your finger."), 40, y, 3, tela.CINZA_CLARO)
  for p: pontos tela.circulo(p[0], p[1], 7, tela.CIANO) end
  tela.botao(tela.largura() - 220, y - 6, 200, 70, idioma.escolher("Limpar", "Clear"), tela.VERMELHO)
end

def dedo(x, y, pressionado)
  if aba != 3 || !pressionado || y < ABA_A + 80 return end
  if size(pontos) < 1500 pontos.push([x, y]) end
end

# ---- DISKS -------------------------------------------------------------------
# The arquivos module sees the whole SD card and the USB flash drive; the app
# only changes its own folder (see docs/BERRY.md).
def desenhar_discos(y)
  if size(discos) == 0 tela.texto(idioma.escolher("nenhum disco", "no disks"), 40, y, 3) return end
  for d: discos
    tela.texto(d["tipo"] + (d["nome"] != "" ? "  " + d["nome"] : ""), 40, y, 4,
               d["disponivel"] ? tela.BRANCO : tela.CINZA)
    var itens = d["disponivel"] ? arquivos.lista(d["raiz"]) : nil
    tela.texto(itens != nil ? str(size(itens)) + idioma.escolher(" item(s) na raiz", " item(s) at the root") : idioma.escolher("indisponível", "unavailable"), 40, y + 55, 2.5, tela.CINZA_CLARO)
    y += 130
  end
  tela.texto(idioma.escolher("Espete um pendrive: ele aparece aqui sozinho.", "Plug in a flash drive: it shows up here by itself."), 40, y, 2.5, tela.CINZA_CLARO)
end

# With "# avisos: volumes" in the header, the system calls aviso() when a
# USB flash drive is plugged in or removed (and redraws afterwards).
def aviso(nome)
  if nome == "volumes" discos = arquivos.volumes() end
end

# ---- what the system calls ---------------------------------------------------
def desenhar(tudo)
  desenhar_abas()
  var y = ABA_A + 40
  if aba == 0 desenhar_inicio(y)
  elif aba == 1 desenhar_toque(y)
  elif aba == 2 desenhar_sensor(y)
  elif aba == 3 desenhar_desenho(y)
  else desenhar_discos(y)
  end
end

def tocar(x, y)
  if y < ABA_A
    var i = x / largura_aba()
    if i >= 0 && i < size(ABAS) trocar_aba(i) end
    return
  end
  var y0 = ABA_A + 40
  if aba == 1 tocar_toque(x, y, y0)
  elif aba == 3 && tela.dentro(x, y, tela.largura() - 220, y0 - 6, 200, 70) pontos = []
  end
end

def abrir()
  trocar_aba(aba)   # the rate of the tab it was left on
end

def fechar()
  sistema.avisar()
end
