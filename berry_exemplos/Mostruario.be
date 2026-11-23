# nome: Mostruario
# cor: 0xF81F
# periodo: 16
#
# The Berry API showcase: one tab for each part of the system. It's the
# template for a new app - copy it, delete the tabs you don't need.
#
#   Draw      every shape, colours, text alignment, dt-based animation
#   Sensors   IMU (acceleration and gyro), battery, date and time, GPS
#   Keyboard  text field: the system's on-screen keyboard or the physical one
#   Finger    drawing with your finger (dedo() receives the drag)
#   Card      files in the app's folder: write, append, read, list
#   Network   Wi-Fi, ESP-NOW (send and receive), Mesh (status, messages, nodes)
#   (the camera has its own app: Camera.be)
#   System    beep, status-bar notice, screen kept on, period
#
# Physical keyboard: the left/right arrows switch tabs (outside the Keyboard tab).

import string
import math

var ABAS = [idioma.escolher("Desenho", "Draw"), idioma.escolher("Sensores", "Sensors"), idioma.escolher("Teclado", "Keyboard"), idioma.escolher("Dedo", "Finger"),
            idioma.escolher("Cartao", "Card"), idioma.escolher("Rede", "Network"), idioma.escolher("Sistema", "System")]
# Each tab's period: animation on every refresh, clock every second,
# or 0 (only redraws when something changes - tap, key).
var PERIODOS = [16, 16, 500, 0, 0, 1000, 1000]   # Keyboard: 500 blinks the cursor
var ABA_A = 70          # height of the tab bar
var M = 24              # margin
var aba = 0

# Fixed lists live HERE, created once: a list created inside desenhar()
# is created afresh every frame (~35 us + ~4 us per item).
var CORES = [tela.PRETO, tela.BRANCO, tela.VERMELHO, tela.VERDE, tela.AZUL, tela.AMARELO,
             tela.CIANO, tela.LARANJA, tela.CINZA, tela.CINZA_CLARO, tela.ROXO, tela.ROSA]
var PALETA = [tela.BRANCO, tela.AMARELO, tela.CIANO, tela.VERDE, tela.ROSA, tela.LARANJA]

# ---- each tab's state -------------------------------------------------------
# Draw: the bouncing ball, moving by time (px/s), not by frame.
var bx = 100.0
var by = 100.0
var vx = 420.0
var vy = 300.0

# Keyboard
var campo = ""
var linhas = []

# Finger: the drawing lives in a LAYER (a stored image). dedo() paints into it
# and notes the new piece; the frame only draws the new pieces on screen
# (sistema.acumular) - never the whole drawing again.
var DEDO_Y = 70           # where the painting area starts, below the tab's header
var pintura = nil         # the layer (created the first time the tab opens)
var novos = []            # pieces painted since the last frame: [x0, y0, x1, y1, colour]
var ultimo = nil          # the last point of the current stroke ([x, y]), nil = finger up
var pontos = 0
var cor_dedo = 0
var cabecalho_sujo = false   # the colour or count changed: redo only the header

# Card
var aberturas = 0
var notas = ""
var arquivos = []

# Network
var enviadas = 0

# System
var com_aviso = false
var acesa = false

# ---- helpers -----------------------------------------------------------------
# A function can only be called by code that comes AFTER it in the file.
def f2(v) return string.format("%.2f", v) end
def f1(v) return string.format("%.1f", v) end

# Three buttons side by side across whatever width there is.
def largura_botao(l) return (l - 2 * M - 40) / 3 end

def ler_cartao()
  var t = cartao.ler("notas.txt")
  notas = t != nil ? t : ""
  arquivos = cartao.lista()
end

def mudar_aba(i)
  if i < 0 i = size(ABAS) - 1 end
  if i >= size(ABAS) i = 0 end
  aba = i
  sistema.periodo(PERIODOS[i])
  if aba != 2 teclado.fechar() end
  if aba == 4 ler_cartao() end
  # Only the Finger tab accumulates: the others redraw everything every frame.
  sistema.acumular(aba == 3)
  if aba == 3 && pintura == nil
    pintura = tela.camada(tela.largura(), tela.altura() - ABA_A - DEDO_Y)
  end
end

# Deletes the last whole character (UTF-8: bytes 10xxxxxx are continuations).
def sem_ultimo(s)
  var i = size(s) - 1
  while i > 0 && (string.byte(s[i]) & 0xC0) == 0x80 i -= 1 end
  if i <= 0 return "" end
  return s[0 .. i - 1]
end

# ---- abrir: what runs once -----------------------------------------------------
def abrir()
  # Persistence: how many times the app has opened, stored on the card.
  var t = cartao.ler("aberturas.txt")
  aberturas = (t != nil ? int(t) : 0) + 1
  cartao.gravar("aberturas.txt", str(aberturas))
  mudar_aba(0)
end

# ---- drawing each tab ----------------------------------------------------------
def desenhar_abas(l)
  var w = l / size(ABAS)
  for i: 0 .. size(ABAS) - 1
    var cor = i == aba ? tela.cor(40, 90, 160) : tela.cor(30, 30, 30)
    tela.arredondado(i * w + 4, 6, w - 8, ABA_A - 12, 12, cor)
    tela.texto(ABAS[i], i * w + w / 2, 20, 2.5, i == aba ? tela.BRANCO : tela.CINZA_CLARO, tela.CENTRO)
  end
end

def aba_desenho(l, a)
  var y = ABA_A + M
  tela.texto(idioma.escolher("Formas", "Shapes"), M, y, 3, tela.AMARELO)
  y += 50
  # 9 cells across whatever width there is (portrait or landscape).
  var c = (l - 2 * M) / 9
  var s = c - 16
  if s > 110 s = 110 end
  tela.retangulo(M, y, s, s, tela.VERMELHO)
  tela.retangulo(M + c, y, s, s, tela.VERMELHO, false)
  tela.arredondado(M + 2 * c, y, s, s, s / 5, tela.VERDE)
  tela.arredondado(M + 3 * c, y, s, s, s / 5, tela.VERDE, false)
  tela.circulo(M + 4 * c + s / 2, y + s / 2, s / 2, tela.AZUL)
  tela.circulo(M + 5 * c + s / 2, y + s / 2, s / 2, tela.AZUL, false)
  tela.triangulo(M + 6 * c, y + s, M + 6 * c + s / 2, y, M + 6 * c + s, y + s, tela.LARANJA)
  tela.triangulo(M + 7 * c, y + s, M + 7 * c + s / 2, y, M + 7 * c + s, y + s, tela.LARANJA, false)
  for i: 0 .. 60
    tela.pixel(M + 8 * c + (i * 7) % s, y + (i * 13) % s, tela.BRANCO)
  end
  y += s + 30

  # Palette: the ready-made colours and one made with tela.cor(r, g, b).
  var q = (l - 2 * M) / size(CORES)
  for i: 0 .. size(CORES) - 1
    tela.retangulo(M + i * q, y, q - 8, 40, CORES[i])
    tela.retangulo(M + i * q, y, q - 8, 40, tela.CINZA, false)
  end
  y += 50
  q = (l - 2 * M) / 32
  for i: 0 .. 31
    tela.retangulo(M + i * q, y, q, 30, tela.cor(i * 8, 255 - i * 8, 128))
  end
  y += 50

  # Alignment: the same x, three ways. tela.medir gives the width.
  var cx = l / 2
  tela.linha(cx, y, cx, y + 130, tela.CINZA)
  tela.texto(idioma.escolher("esquerda", "left"), cx, y, 3, tela.CIANO, tela.ESQUERDA)
  tela.texto(idioma.escolher("centro", "centre"), cx, y + 45, 3, tela.CIANO, tela.CENTRO)
  tela.texto(idioma.escolher("direita", "right"), cx, y + 90, 3, tela.CIANO, tela.DIREITA)
  var msg = idioma.escolher("medir: ", "measure: ") + str(tela.medir(idioma.escolher("centro", "centre"), 3)) + " px"
  tela.texto(msg, M, y + 45, 2, tela.CINZA_CLARO)
  y += 150

  # Time-based animation: dt is the real time between frames on screen.
  var caixa_a = a - y - M
  if caixa_a < 80 return end
  tela.retangulo(M, y, l - 2 * M, caixa_a, tela.CINZA, false)
  var dt = sistema.dt()
  if dt > 0.1 dt = 0.1 end       # after a pause, don't jump
  bx += vx * dt
  by += vy * dt
  var r = 24
  if bx < r            bx = r;            vx = -vx end
  if bx > l - 2*M - r  bx = l - 2*M - r;  vx = -vx end
  if by < r            by = r;            vy = -vy end
  if by > caixa_a - r  by = caixa_a - r;  vy = -vy end
  tela.circulo(M + int(bx), y + int(by), r, tela.AMARELO)
  tela.texto("dt " + f1(dt * 1000) + " ms", M + 12, y + 8, 2, tela.CINZA_CLARO)
end

def barra(rotulo, v, maximo, x, y, w, cor)
  tela.texto(rotulo, x, y, 2.5)
  var meio = x + 90 + (w - 90) / 2
  tela.retangulo(x + 90, y, w - 90, 28, tela.cor(30, 30, 30))
  var p = v / maximo
  if p > 1 p = 1 end
  if p < -1 p = -1 end
  var n = int(p * (w - 90) / 2)
  if n >= 0 tela.retangulo(meio, y, n, 28, cor)
  else tela.retangulo(meio + n, y, -n, 28, cor) end
  tela.linha(meio, y, meio, y + 28, tela.BRANCO)
end

def aba_sensores(l, a)
  var y = ABA_A + M
  var g = sistema.imu()
  var gi = sistema.giro()
  var w = l / 2 - 2 * M
  tela.texto(idioma.escolher("Aceleracao (g)", "Acceleration (g)"), M, y, 3, tela.AMARELO)
  tela.texto(idioma.escolher("Giro (graus/s)", "Gyro (degrees/s)"), l / 2 + M, y, 3, tela.AMARELO)
  if g == nil
    tela.texto(idioma.escolher("IMU sem leitura", "IMU not reading"), M, y + 60, 3, tela.VERMELHO)
  else
    barra("x " + f2(g[0]), g[0], 1.5, M, y + 60, w, tela.VERMELHO)
    barra("y " + f2(g[1]), g[1], 1.5, M, y + 100, w, tela.VERDE)
    barra("z " + f2(g[2]), g[2], 1.5, M, y + 140, w, tela.AZUL)
    barra("x " + str(int(gi[0])), gi[0], 250, l / 2 + M, y + 60, w, tela.VERMELHO)
    barra("y " + str(int(gi[1])), gi[1], 250, l / 2 + M, y + 100, w, tela.VERDE)
    barra("z " + str(int(gi[2])), gi[2], 250, l / 2 + M, y + 140, w, tela.AZUL)
    # The spirit-level bubble.
    var cx = l / 2
    var cy = y + 330
    tela.circulo(cx, cy, 120, tela.CINZA, false)
    tela.circulo(cx, cy, 40, tela.CINZA, false)
    tela.circulo(cx + int(g[0] * -110), cy + int(g[1] * 110), 22, tela.VERDE)
  end

  var e = sistema.energia()
  var yb = y + 220
  tela.texto(idioma.escolher("Bateria", "Battery"), M, yb, 3, tela.AMARELO)
  tela.texto((e["pct"] >= 0 ? str(e["pct"]) + "%  " : "") + f2(e["volts"]) + " V", M, yb + 45, 3)
  tela.texto((e["ma"] > 0 ? idioma.escolher("carregando ", "charging ") : "") + str(int(e["ma"])) + " mA", M, yb + 85, 3)

  tela.texto(idioma.escolher("Relogio", "Clock"), M, yb + 150, 3, tela.AMARELO)
  var h = sistema.hora()
  tela.texto(h != "" ? sistema.data() + "  " + h : idioma.escolher("sem acerto", "not set"), M, yb + 195, 3)

  var xg = l - M - 380
  tela.texto("GPS", xg, yb, 3, tela.AMARELO)
  var p = sistema.gps()
  if p == nil
    tela.texto(idioma.escolher("sem GPS no Grove", "no GPS on Grove"), xg, yb + 45, 2.5, tela.CINZA_CLARO)
  elif !p["fix"]
    tela.texto(idioma.escolher("sem fix (", "no fix (") + str(p["sats"]) + " sat)", xg, yb + 45, 2.5)
  else
    tela.texto(string.format("%.5f", p["lat"]), xg, yb + 45, 2.5)
    tela.texto(string.format("%.5f", p["lon"]), xg, yb + 80, 2.5)
    tela.texto(str(int(p["alt"])) + " m, " + str(p["sats"]) + " sat", xg, yb + 115, 2.5)
  end
end

def aba_teclado(l, a)
  # With the keyboard open, tela.altura() already excludes it: 'a' is what's left.
  var y = ABA_A + M
  var fis = teclado.fisico()
  tela.texto(fis ? idioma.escolher("Teclado fisico encaixado - e so digitar", "Physical keyboard attached - just type") : idioma.escolher("Sem teclado fisico: use o virtual", "No physical keyboard: use the on-screen one"),
             M, y, 2.5, tela.CINZA_CLARO)
  tela.botao(l - M - 300, y - 10, 300, 64, teclado.aberto() ? idioma.escolher("Fechar teclado", "Close keyboard") : idioma.escolher("Abrir teclado", "Open keyboard"),
             tela.CIANO)
  y += 70
  tela.arredondado(M, y, l - 2 * M, 70, 12, tela.cor(25, 25, 40))
  var cursor = (sistema.ms() / 500) % 2 == 0 ? "|" : ""
  tela.texto(campo + cursor, M + 16, y + 18, 3.5)
  y += 90
  tela.texto(idioma.escolher("Enter manda a linha para a lista:", "Enter sends the line to the list:"), M, y, 2.5, tela.AMARELO)
  y += 40
  var cabe = (a - y) / 40
  var ini = size(linhas) > cabe ? size(linhas) - cabe : 0
  for i: ini .. size(linhas) - 1
    tela.texto(str(i + 1) + ". " + linhas[i], M, y, 3)
    y += 40
  end
end

def cabecalho_dedo(l)
  var y0 = ABA_A
  tela.retangulo(0, y0, l, DEDO_Y, tela.PRETO)   # when accumulating, what was there stays: clear it
  tela.texto(idioma.escolher("Desenhe com o dedo", "Draw with your finger"), M, y0 + 12, 2.5, tela.CINZA_CLARO)
  tela.botao(l - M - 200, y0 + 6, 200, 56, idioma.escolher("Limpar", "Clear"), tela.VERMELHO)
  tela.botao(l - M - 420, y0 + 6, 200, 56, idioma.escolher("Cor", "Colour"), tela.cor(80, 80, 80))
  tela.circulo(l - M - 470, y0 + 34, 22, PALETA[cor_dedo % size(PALETA)])
  tela.texto(str(pontos) + idioma.escolher(" pontos", " points"), M + 400, y0 + 12, 2.5, tela.CINZA)
end

# One piece of stroke, thick (3 lines): the same drawing on the layer and on screen.
def pedaco(s, dy)
  tela.linha(s[0], s[1] + dy, s[2], s[3] + dy, s[4])
  tela.linha(s[0] + 1, s[1] + dy, s[2] + 1, s[3] + dy, s[4])
  tela.linha(s[0], s[1] + dy + 1, s[2], s[3] + dy + 1, s[4])
end

# tudo = true: the area arrived clear - header and the whole painting (pasted
# from the layer). tudo = false: only what changed since the last frame.
def aba_dedo(l, a, tudo)
  var topo = ABA_A + DEDO_Y
  if tudo || cabecalho_sujo
    cabecalho_dedo(l)
    cabecalho_sujo = false
  end
  if tudo
    tela.colar(pintura, 0, topo)
  else
    for s: novos pedaco(s, topo) end
  end
  novos = []
end

def aba_cartao(l, a)
  var y = ABA_A + M
  tela.texto(idioma.escolher("Este app abriu ", "This app has opened ") + str(aberturas) + idioma.escolher(" vez(es) - contado em aberturas.txt", " time(s) - counted in aberturas.txt"), M, y, 2.5)
  y += 50
  var bw = largura_botao(l)
  tela.botao(M, y, bw, 70, idioma.escolher("Anexar linha", "Append line"), tela.VERDE)
  tela.botao(M + bw + 20, y, bw, 70, idioma.escolher("Apagar notas", "Delete notes"), tela.VERMELHO)
  tela.botao(M + 2 * (bw + 20), y, bw, 70, idioma.escolher("Reler", "Re-read"), tela.CIANO)
  y += 100
  tela.texto(idioma.escolher("Arquivos em apps/Mostruario/:", "Files in apps/Mostruario/:"), M, y, 2.5, tela.AMARELO)
  var nomes = ""
  for n: arquivos nomes += n + "   " end
  tela.texto(nomes != "" ? nomes : idioma.escolher("(nenhum)", "(none)"), M, y + 40, 2.5)
  y += 100
  tela.texto("notas.txt:", M, y, 2.5, tela.AMARELO)
  y += 40
  var ls = string.split(notas, "\n")
  var cabe = (a - y) / 36
  var mostradas = []
  for s: ls if s != "" mostradas.push(s) end end
  var ini = size(mostradas) > cabe ? size(mostradas) - cabe : 0
  for i: ini .. size(mostradas) - 1
    tela.texto(mostradas[i], M, y, 2.5)
    y += 36
  end
end

def aba_rede(l, a)
  var y = ABA_A + M
  var r = sistema.rede()
  tela.texto("Wi-Fi", M, y, 3, tela.AMARELO)
  if r["ip"] != ""
    tela.texto(r["ssid"] + "   " + r["ip"] + "   " + str(r["rssi"]) + " dBm", M, y + 45, 2.5)
  else
    tela.texto(r["texto"], M, y + 45, 2.5)
  end
  y += 110

  tela.texto("ESP-NOW", M, y, 3, tela.AMARELO)
  if espnow.pronto()
    tela.texto(idioma.escolher("canal ", "channel ") + str(espnow.canal()) + ", " + str(espnow.total()) + idioma.escolher(" recebida(s)", " received"),
               M + 180, y + 4, 2.5)
    tela.botao(l - M - 300, y - 10, 300, 64, idioma.escolher("Enviar ola", "Send hello"), tela.VERDE)
    var n = espnow.n()
    var yy = y + 60
    var ini = n > 5 ? n - 5 : 0
    for i: ini .. n - 1
      var m = espnow.msg(i)
      tela.texto((m["minha"] ? idioma.escolher("eu: ", "me: ") : m["de"] + ": ") + m["texto"], M, yy, 2.5,
                 m["minha"] ? tela.CIANO : tela.BRANCO)
      yy += 36
    end
    if n == 0 tela.texto(idioma.escolher("nada recebido ainda", "nothing received yet"), M, yy, 2.5, tela.CINZA_CLARO) end
  else
    tela.texto(idioma.escolher("fora do ar (Wi-Fi desligado ou radio C6 de fabrica)", "off air (Wi-Fi off or factory C6 radio)"), M + 180, y + 4, 2.5,
               tela.CINZA_CLARO)
  end
  y += 260

  tela.texto("Mesh", M, y, 3, tela.AMARELO)
  var h = mesh.estado()
  if h["ativo"]
    tela.texto((h["no_ar"] ? idioma.escolher("no ar", "on air") : idioma.escolher("mudo", "muted")) + ", " + str(h["nos"]) + idioma.escolher(" no(s), ", " node(s), ") +
               str(h["nao_lidas"]) + idioma.escolher(" nao lida(s)", " unread"), M + 120, y + 4, 2.5)
    # The recent messages (0 = the newest) and the most recent node: read only.
    var yy = y + 50
    for i: 0 .. 1
      var m = mesh.msg(i)
      if m == nil break end
      tela.texto(sistema.hora_de(m["quando"]) + "  " + (m["nome"] != "" ? m["nome"] : "?") + ": " + m["texto"],
                 M, yy, 2.5, m["direta"] ? tela.CIANO : tela.BRANCO)
      yy += 36
    end
    var no = mesh.no(1)               # 0 is ours; 1 the most recently heard
    if no != nil
      tela.texto(idioma.escolher("ultimo ouvido: ", "last heard: ") + no["nome"] + " (" + (no["mqtt"] ? "internet" : str(no["saltos"]) + idioma.escolher(" salto(s)", " hop(s)")) + ")",
                 M, yy, 2.5, tela.CINZA_CLARO)
    end
  else
    tela.texto(idioma.escolher("servico desligado (Ajustes > Mesh)", "service off (Settings > Mesh)"), M + 120, y + 4, 2.5, tela.CINZA_CLARO)
  end
end

def aba_sistema(l, a)
  var y = ABA_A + M
  var bw = largura_botao(l)
  tela.botao(M, y, bw, 80, idioma.escolher("Bip", "Beep"), tela.AMARELO)
  tela.botao(M + bw + 20, y, bw, 80, com_aviso ? idioma.escolher("Tirar aviso", "Clear notice") : idioma.escolher("Aviso na barra", "Status-bar notice"), tela.LARANJA)
  tela.botao(M + 2 * (bw + 20), y, bw, 80, acesa ? idioma.escolher("Deixar apagar", "Allow screen off") : idioma.escolher("Tela acesa", "Keep screen on"), tela.CIANO)
  y += 120
  tela.texto(idioma.escolher("ligado ha ", "up for ") + str(sistema.ms() / 1000) + " s", M, y, 3)
  tela.texto(idioma.escolher("periodo desta aba: ", "this tab's period: ") + str(sistema.periodo()) + " ms", M, y + 45, 3)
  tela.texto(idioma.escolher("area livre: ", "free area: ") + str(l) + " x " + str(a) + " px", M, y + 90, 3)
  tela.texto(idioma.escolher("tela ", "screen ") + (acesa ? idioma.escolher("acesa enquanto o app estiver aberto", "stays on while the app is open") : idioma.escolher("apaga sozinha (padrao)", "switches off by itself (default)")),
             M, y + 135, 3)
  tela.texto(idioma.escolher("teclado fisico: ", "physical keyboard: ") + (teclado.fisico() ? idioma.escolher("sim", "yes") : idioma.escolher("nao", "no")), M, y + 180, 3)
end

def desenhar(tudo)
  var l = tela.largura()
  var a = tela.altura()
  # The tabs only change when the area has been cleared (tudo); outside the
  # Finger tab the app doesn't accumulate, so tudo is always true.
  if tudo desenhar_abas(l) end
  if   aba == 0 aba_desenho(l, a)
  elif aba == 1 aba_sensores(l, a)
  elif aba == 2 aba_teclado(l, a)
  elif aba == 3 aba_dedo(l, a, tudo)
  elif aba == 4 aba_cartao(l, a)
  elif aba == 5 aba_rede(l, a)
  else          aba_sistema(l, a)
  end
end

# ---- tap ---------------------------------------------------------------------
def tocar(x, y)
  var l = tela.largura()
  if y < ABA_A
    mudar_aba(x / (l / size(ABAS)))
    return
  end
  var y0 = ABA_A + M
  if aba == 2
    if tela.dentro(x, y, l - M - 300, y0 - 10, 300, 64)
      if teclado.aberto() teclado.fechar() else teclado.abrir() end
    end
  elif aba == 3
    if tela.dentro(x, y, l - M - 200, ABA_A + 6, 200, 56)
      tela.alvo(pintura)
      tela.limpar()
      tela.alvo()
      novos = []
      pontos = 0
      sistema.acumular(false)   # the whole painting changed: one clean frame (tudo = true)
      sistema.acumular(true)
    elif tela.dentro(x, y, l - M - 420, ABA_A + 6, 200, 56)
      cor_dedo += 1
      cabecalho_sujo = true
    end
  elif aba == 4
    var yb = y0 + 50
    var bw = largura_botao(l)
    if tela.dentro(x, y, M, yb, bw, 70)
      var h = sistema.hora()
      cartao.anexar("notas.txt", (h != "" ? h : str(sistema.ms())) + idioma.escolher(" - toque em anexar\n", " - tapped append\n"))
      ler_cartao()
    elif tela.dentro(x, y, M + bw + 20, yb, bw, 70)
      cartao.gravar("notas.txt", "")
      ler_cartao()
    elif tela.dentro(x, y, M + 2 * (bw + 20), yb, bw, 70)
      ler_cartao()
    end
  elif aba == 5
    if espnow.pronto() && tela.dentro(x, y, l - M - 300, y0 + 100, 300, 64)
      enviadas += 1
      espnow.enviar(idioma.escolher("ola do Berry #", "hello from Berry #") + str(enviadas))
    end
  elif aba == 6
    var bw = largura_botao(l)
    if tela.dentro(x, y, M, y0, bw, 80)
      sistema.bip(880, 120)
    elif tela.dentro(x, y, M + bw + 20, y0, bw, 80)
      com_aviso = !com_aviso
      if com_aviso sistema.avisar(idioma.escolher("aviso do Mostruario", "notice from Mostruario")) else sistema.avisar() end
    elif tela.dentro(x, y, M + 2 * (bw + 20), y0, bw, 80)
      acesa = !acesa
      sistema.acordado(acesa)
    end
  end
end

# The finger by level: called while the finger is on the screen (and once on
# lifting, with pressionado = false). Only the Finger tab uses it.
def dedo(x, y, pressionado)
  if aba != 3 return end
  var topo = ABA_A + DEDO_Y
  if !pressionado || y < topo
    ultimo = nil
    return
  end
  var p = [x, y - topo]               # in the layer's coordinates
  var de = ultimo != nil ? ultimo : p
  var s = [de[0], de[1], p[0], p[1], PALETA[cor_dedo % size(PALETA)]]
  # Paints into the layer (it stays stored) and notes it for the next frame.
  tela.alvo(pintura)
  pedaco(s, 0)
  tela.alvo()
  novos.push(s)
  ultimo = p
  pontos += 1
  if pontos % 20 == 0 cabecalho_sujo = true end   # the count, without redoing it on every point
end

# ---- keys: the on-screen and physical keyboards both arrive here the same way -
def tecla(texto, codigo)
  if aba == 2
    if texto != ""
      campo += texto
    elif codigo == teclado.APAGAR
      campo = sem_ultimo(campo)
    elif codigo == teclado.ENTER
      if campo != "" linhas.push(campo) end
      campo = ""
    elif codigo == teclado.ESC
      teclado.fechar()
    end
    return
  end
  if codigo == teclado.ESQUERDA mudar_aba(aba - 1)
  elif codigo == teclado.DIREITA mudar_aba(aba + 1)
  end
end

def fechar()
  sistema.avisar()
end
