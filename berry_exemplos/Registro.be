# nome: Registro
# cor: 0x2DFF
# periodo: 1000
#
# REGISTRO (LOGGER) - battery, current and GPS written to the card, WITH THE
# APP CLOSED. Same layout as Azimutal: the window shows live values, RECORD
# turns on the background part and it keeps recording after the window closes
# (a pill in the status bar shows how many samples so far). STOP turns it
# off. Works without GPS: its columns are just left empty.
#
# HOW THE BACKGROUND WORKS: sistema.fundo(true, ms) makes the system call
# fundo() every ms, with the window open or closed. It is the SAME virtual
# machine: the lists fundo() fills are the ones the window reads - no lock
# needed, because the two never run at the same time. Inside fundo() there is
# no screen, keyboard, camera or beep: only reading, writing to the card and
# notices. Each call has a 200 ms deadline.
#
# MEMORY: the samples live in two lists of integers, at most MAX - about
# 12 KB on the device (see sistema.memoria() in the Now tab). The real
# history goes to the card: /sdcard/apps/Berry/Registro/registro-AAAA-MM-DD.csv,
#   hora,bateria %,volts,mA,lat,lon,alt,satelites
# (time, battery %, volts, mA, lat, lon, alt, satellites), one line per
# sample. Without the clock set there is no date: it stays in memory only.

import string

var INTERVALO = 10000    # ms between samples
var MAX = 720            # samples in memory (2 h at one per 10 s); the oldest drop off
var BARRA_A = 96
var M = 24
var FUNDO_COR = 0x18E3
var COR_PCT = 0x2DFF
var COR_MA = 0xFD20

var pct = []             # battery % of each sample (-1 = no estimate)
var ma = []              # current of each sample (mA; + = charging)
var gravadas = 0         # lines written to the card in this recording
var arquivo = ""         # today's csv ("" = clock not set: memory only)
var avisou_baixa = false
var aba = 0              # 0 = Now, 1 = Graph
var bt_gravar = [0, 0, 0, 0]
var bt_abas = [[0, 0, 0, 0], [0, 0, 0, 0]]

# ---- the sample (background and window) --------------------------------------
def nome_do_dia()
  var d = sistema.data()          # "dd/mm/aaaa" (dd/mm/yyyy) or "" with no clock
  if d == "" return "" end
  var p = string.split(d, "/")
  return "registro-" + p[2] + "-" + p[1] + "-" + p[0] + ".csv"
end

def amostrar()
  var e = sistema.energia()
  pct.push(e["pct"])
  ma.push(int(e["ma"]))
  if size(pct) > MAX pct.remove(0) ma.remove(0) end
  return e
end

# THE BACKGROUND PART: one sample per call, every INTERVALO.
def fundo()
  var e = amostrar()
  if arquivo == "" arquivo = nome_do_dia() end   # the clock may have just been set
  if arquivo != ""
    var linha = string.format("%s,%d,%.3f,%d", sistema.hora(), e["pct"], e["volts"], int(e["ma"]))
    var g = sistema.gps()
    if g != nil && g["fix"]
      linha += string.format(",%.6f,%.6f,%.1f,%d", g["lat"], g["lon"], g["alt"], g["sats"])
    else
      linha += ",,,,"
    end
    if cartao.anexar(arquivo, linha + "\n") gravadas += 1 end
  end
  sistema.fundo_estado(str(gravadas) + idioma.escolher(" amostra(s)", " sample(s)"), gravadas)
  # Battery low and discharging: a notice in the status bar, once only.
  var p = e["pct"]
  if p >= 0 && p <= 15 && e["ma"] < 0
    if !avisou_baixa
      sistema.avisar(idioma.escolher("Bateria em ", "Battery at ") + str(p) + "%")
      avisou_baixa = true
    end
  elif p > 20
    if avisou_baixa sistema.avisar() end
    avisou_baixa = false
  end
end

def gravando() return sistema.fundo() end

def ligar_desligar()
  if gravando()
    sistema.fundo(false)
  else
    gravadas = 0
    arquivo = nome_do_dia()
    sistema.fundo(true, INTERVALO)
  end
end

# ---- the window ----------------------------------------------------------------
def cor_bat(p)
  if p < 0 return tela.CINZA end
  if p <= 15 return tela.VERMELHO end
  if p <= 40 return tela.AMARELO end
  return tela.VERDE
end

def barra()
  var L = tela.largura()
  tela.retangulo(0, 0, L, BARRA_A, FUNDO_COR)
  var x = M
  if L >= 900   # landscape: the title fits
    tela.texto("Registro", x, 30, 4.2)
    x += tela.medir("Registro", 4.2) + 30
  end
  var nomes = [idioma.escolher("Agora", "Now"), idioma.escolher("Gráfico", "Graph")]
  for i: 0 .. 1
    var l = tela.medir(nomes[i], 3) + 44
    tela.arredondado(x, 16, l, 64, 32, i == aba ? 0x2C7F : 0x3186)
    tela.texto(nomes[i], x + 22, 36, 3)
    bt_abas[i] = [x, 16, l, 64]
    x += l + 14
  end
  var on = gravando()
  var bl = 220
  bt_gravar = [L - M - bl, 12, bl, 72]
  tela.botao(bt_gravar[0], bt_gravar[1], bl, 72, on ? idioma.escolher("PARAR", "STOP") : idioma.escolher("GRAVAR", "RECORD"), on ? tela.VERMELHO : tela.VERDE)
end

def linha_valor(rotulo, valor, y, cor)
  tela.texto(rotulo, M, y, 2.6, tela.CINZA_CLARO)
  tela.texto(valor, M + 230, y - 6, 3.4, cor != nil ? cor : tela.BRANCO)
end

def pag_agora(y0)
  var e = sistema.energia()
  var p = e["pct"]
  tela.texto(p >= 0 ? str(p) + "%" : "--", M, y0 + 24, 12, cor_bat(p))
  var c = int(e["ma"])
  var y = y0 + 150
  linha_valor(idioma.escolher("Tensão", "Voltage"), string.format("%.2f V", e["volts"]), y)
  y += 56
  linha_valor(idioma.escolher("Corrente", "Current"), (c > 0 ? "+" : "") + str(c) + " mA" + (c > 0 ? idioma.escolher("  carregando", "  charging") : ""), y,
              c > 0 ? tela.VERDE : nil)
  y += 56
  var g = sistema.gps()
  if g == nil
    linha_valor("GPS", idioma.escolher("sem GPS no Grove", "no GPS on Grove"), y, tela.CINZA)
  elif !g["fix"]
    linha_valor("GPS", idioma.escolher("procurando (", "searching (") + str(g["sats"]) + " sat)", y, tela.AMARELO)
  else
    linha_valor("GPS", string.format("%.5f, %.5f  (%d sat)", g["lat"], g["lon"], g["sats"]), y)
  end
  y += 80
  # THE RECORDING
  if gravando()
    var onde = arquivo != "" ? arquivo : idioma.escolher("só na memória (relógio sem acerto)", "memory only (clock not set)")
    linha_valor(idioma.escolher("Gravando", "Recording"), str(gravadas) + idioma.escolher(" amostra(s), a cada ", " sample(s), every ") + str(INTERVALO / 1000) + " s", y, tela.VERMELHO)
    y += 56
    linha_valor(idioma.escolher("Arquivo", "File"), onde, y, tela.CINZA_CLARO)
    y += 56
    tela.texto(idioma.escolher("Pode fechar o app: a gravação continua (pastilha na barra).", "You can close the app: recording continues (pill in the status bar)."), M, y, 2.4, tela.CINZA)
  else
    linha_valor(idioma.escolher("Gravação", "Recording"), idioma.escolher("parada - toque em GRAVAR", "stopped - tap RECORD"), y, tela.CINZA)
  end
  y += 70
  # WHAT THIS APP COSTS: its own virtual machine and all of Berry's, against the ceiling.
  var m = sistema.memoria()
  linha_valor(idioma.escolher("Memória", "Memory"), string.format(idioma.escolher("%.1f KB (Berry: %.0f de %.0f KB)", "%.1f KB (Berry: %.0f of %.0f KB)"), m["app"] / 1024.0,
              m["berry"] / 1024.0, m["orcamento"] / 1024.0), y, tela.CINZA_CLARO)
end

def pag_grafico(y0)
  var L = tela.largura()
  var A = tela.altura()
  var n = size(pct)
  if n < 2
    tela.texto(idioma.escolher("Sem amostras ainda.", "No samples yet."), M, y0 + 40, 3.4)
    tela.texto(idioma.escolher("Toque em GRAVAR: uma amostra a cada ", "Tap RECORD: one sample every ") + str(INTERVALO / 1000) + " s.", M, y0 + 100, 2.6, tela.CINZA_CLARO)
    return
  end
  var gx = M + 70
  var gy = y0 + 30
  var gl = L - gx - M - 70
  var ga = A - gy - 90
  # The grid: 0, 25, 50, 75 and 100 % on the left.
  for k: 0 .. 4
    var yy = gy + ga - k * ga / 4
    tela.linha(gx, yy, gx + gl, yy, 0x3186)
    tela.texto(str(k * 25), M, yy - 10, 2.2, COR_PCT)
  end
  # The current on its own scale: the largest absolute sample, on the right.
  var mmax = 100
  for v: ma if v > mmax mmax = v elif -v > mmax mmax = -v end end
  tela.texto("+" + str(mmax), gx + gl + 8, gy - 10, 2.0, COR_MA)
  tela.texto("-" + str(mmax), gx + gl + 8, gy + ga - 10, 2.0, COR_MA)
  tela.linha(gx, gy + ga / 2, gx + gl, gy + ga / 2, 0x4208)   # 0 mA
  # At most one point every 2 px: extra samples don't make more line.
  var k = n
  if k > gl / 2 k = gl / 2 end
  var xa = nil var ya = nil var yb = nil
  for j: 0 .. k - 1
    var i = j * (n - 1) / (k - 1)
    var x = gx + j * gl / (k - 1)
    var p = pct[i] < 0 ? 0 : pct[i]
    var y1 = gy + ga - p * ga / 100
    var y2 = gy + ga / 2 - ma[i] * (ga / 2) / mmax
    if xa != nil
      tela.linha(xa, ya, x, y1, COR_PCT)
      tela.linha(xa, yb, x, y2, COR_MA)
    end
    xa = x ya = y1 yb = y2
  end
  var minutos = (n - 1) * INTERVALO / 60000
  tela.texto(str(n) + idioma.escolher(" amostras, últimos ", " samples, last ") + str(minutos) + " min", gx, gy + ga + 24, 2.4, tela.CINZA_CLARO)
  tela.texto(idioma.escolher("bateria %", "battery %"), gx + gl - 300, gy + ga + 24, 2.4, COR_PCT)
  tela.texto("mA", gx + gl - 60, gy + ga + 24, 2.4, COR_MA)
end

def desenhar(tudo)
  barra()
  if aba == 0 pag_agora(BARRA_A) else pag_grafico(BARRA_A) end
end

def tocar(x, y)
  var b = bt_gravar
  if tela.dentro(x, y, b[0], b[1], b[2], b[3])
    ligar_desligar()
    return
  end
  for i: 0 .. 1
    b = bt_abas[i]
    if tela.dentro(x, y, b[0], b[1], b[2], b[3]) aba = i end
  end
end
