# nome: Estresse
# cor: 0xF800
# periodo: 16
#
# Squeezes the whole system at once, to see how it behaves:
#   - TELA (screen):     hundreds of shapes per frame (as many as fit the deadline)
#   - CPU:               the Mandelbrot set computed line by line into a layer
#   - MEMORIA (memory):  fills Berry's memory up to 60% of the budget, then frees it
#   - CARTAO (SD card):  writes and re-reads a file non-stop
#   - REDE (network):    a download every 15 s (needs Wi-Fi)
#   - FUNDO (background): a heavy calculation in the background part every 500 ms
# The phases change every 20 s; the last one ("tudo junto", all together)
# turns them all on. A tap moves to the next phase. Every 10 s an
# "estresse:" line goes to the serial log with the numbers (to compare later).
#
# The load ADJUSTS itself: each part measures how long it took and increases
# or decreases its work to fit its slice of the frame. That way the app pushes
# as hard as it can without blowing the system's 2 s per-event deadline.

import math
import gc

# ---- phases ------------------------------------------------------------------
var FASES = ["Tela", "CPU", "Memoria", "Cartao", "Rede", "Tudo junto"]
var FASE_MS = 20000
var fase = 0
var fase_desde = 0

# Which phases each part is on in (the last, "Tudo junto", turns them all on).
def ligado(nome)
  return FASES[fase] == nome || fase == size(FASES) - 1
end

# ---- frame numbers -----------------------------------------------------------
var quadros = 0          # frames since the last summary
var pior_ms = 0.0        # the slowest frame since the last summary
var soma_ms = 0.0
var resumo_desde = 0
var fps = 0.0
var med_ms = 0.0
var pior_mostrado = 0.0
var erros = 0            # everything that failed (card, network, memory)
var ultimo_erro = ""

def erro(txt)
  erros += 1
  ultimo_erro = txt
  sistema.log("estresse: ERRO " + txt)
end

# ---- TELA (screen): random shapes ---------------------------------------------
var n_fig = 50           # how many shapes per frame (adjusted)
var FIG_ALVO_US = 25000  # the frame slice for the shapes

def figuras(x0, y0, l, a)
  var t0 = sistema.us()
  for i: 1 .. n_fig
    var x = x0 + math.rand() % l
    var y = y0 + math.rand() % a
    var c = tela.cor(math.rand() % 256, math.rand() % 256, math.rand() % 256)
    var tipo = i % 5
    if tipo == 0     tela.circulo(x, y, 4 + math.rand() % 30, c)
    elif tipo == 1   tela.retangulo(x, y, 8 + math.rand() % 60, 8 + math.rand() % 40, c)
    elif tipo == 2   tela.linha(x, y, x0 + math.rand() % l, y0 + math.rand() % a, c)
    elif tipo == 3   tela.triangulo(x, y, x + 30, y + 10, x + 10, y + 35, c)
    else             tela.texto("Tab5", x, y, 2, c)
    end
  end
  var gasto = sistema.us() - t0
  # Adjust: 10% more or less per frame, between 10 and 5000 shapes.
  if gasto < FIG_ALVO_US && n_fig < 5000 n_fig = int(n_fig * 1.1) + 1 end
  if gasto > FIG_ALVO_US && n_fig > 10   n_fig = int(n_fig * 0.9) end
end

# ---- CPU: Mandelbrot into a layer --------------------------------------------
var MAND = 240           # side of the image (px)
var mand = nil           # the layer (created in abrir)
var mand_linha = 0       # next line to compute
var mand_linhas = 2      # lines per frame (adjusted)
var mand_zoom = 1.0
var MAND_ALVO_US = 30000
var MAX_IT = 48

def mand_cor(it)
  if it >= MAX_IT return tela.PRETO end
  var k = it * 255 / MAX_IT
  return tela.cor(k, 255 - k, (k * 3) % 256)
end

def mandelbrot()
  var t0 = sistema.us()
  tela.alvo(mand)
  var escala = 3.0 / (MAND * mand_zoom)
  for j: 1 .. mand_linhas
    var py = mand_linha
    var ci = (py - MAND / 2) * escala
    for px: 0 .. MAND - 1
      var cr = (px - MAND * 0.7) * escala - 0.5 * (1 - 1 / mand_zoom)
      var zr = 0.0
      var zi = 0.0
      var it = 0
      while it < MAX_IT && zr * zr + zi * zi < 4.0
        var t = zr * zr - zi * zi + cr
        zi = 2 * zr * zi + ci
        zr = t
        it += 1
      end
      tela.pixel(px, py, mand_cor(it))
    end
    mand_linha += 1
    if mand_linha >= MAND
      mand_linha = 0
      mand_zoom *= 1.3                      # the next pass, closer in
      if mand_zoom > 40 mand_zoom = 1.0 end
    end
  end
  tela.alvo()
  var gasto = sistema.us() - t0
  if gasto < MAND_ALVO_US && mand_linhas < 60 mand_linhas += 1 end
  if gasto > MAND_ALVO_US && mand_linhas > 1  mand_linhas -= 1 end
end

# ---- MEMORIA (memory): fill and free ------------------------------------------
var blocos = []          # each block is a large list
var BLOCO = 8192         # items per block
var mem_pico = 0
var mem_ciclos = 0       # how many times it filled and freed

def memoria()
  var m = sistema.memoria()
  if m["berry"] > mem_pico mem_pico = m["berry"] end
  # Frees once past 60% of the budget (or 300 blocks, about 2.4 MB: the
  # system's count can take a while to catch up).
  if m["berry"] > m["orcamento"] * 6 / 10 || size(blocos) >= 300
    blocos = []
    gc.collect()
    mem_ciclos += 1
    return
  end
  try
    var b = []
    b.resize(BLOCO)
    for i: 0 .. 63 b[i * 128] = i end   # actually touch the memory
    blocos.push(b)
  except .. as e, msg
    erro(idioma.escolher("memoria: ", "memory: ") + str(msg))
    blocos = []
    gc.collect()
  end
end

# ---- CARTAO (SD card): write and re-read ----------------------------------------
var ARQ = "estresse.txt"
var cart_bytes = 0       # how big the file should be
var cart_linhas = 0
var cart_leituras = 0
var cart_conferido_ms = 0

def cartao_trabalho(agora)
  var linha = format("%010d quadro %d fase %s figuras %d\n", agora, quadros, FASES[fase], n_fig)
  if cartao.anexar(ARQ, linha)
    cart_bytes += size(linha)
    cart_linhas += 1
  else
    erro(idioma.escolher("cartao: anexar falhou", "card: append failed"))
  end
  # Every 2 s, re-reads everything and checks the size.
  if agora - cart_conferido_ms > 2000
    cart_conferido_ms = agora
    var t = cartao.ler(ARQ)
    cart_leituras += 1
    if t == nil
      erro(idioma.escolher("cartao: ler devolveu nil", "card: read returned nil"))
    elif size(t) != cart_bytes
      erro(format(idioma.escolher("cartao: lido %d, esperado %d", "card: read %d, expected %d"), size(t), cart_bytes))
      cart_bytes = size(t)
    end
    # Close to the 64 KB read limit: start again.
    if cart_bytes > 48000
      if cartao.gravar(ARQ, "") cart_bytes = 0 else erro(idioma.escolher("cartao: zerar falhou", "card: reset failed")) end
    end
  end
end

# ---- REDE (network): a download every 15 s --------------------------------------
var URL = "http://example.com/"
var rede_ok = 0
var rede_falhas = 0
var rede_ms = 0          # how long the last one took
var rede_pedido_ms = -15000

def rede_trabalho(agora)
  var e = rede.estado()
  if e == "pronto" || e == "erro"
    var r = rede.resposta()
    if r != nil && r["status"] == 200
      rede_ok += 1
      rede_ms = r["ms"]
    else
      rede_falhas += 1
      erro(idioma.escolher("rede: ", "network: ") + (r != nil ? str(r["status"]) + " " + str(r["erro"]) : idioma.escolher("sem resposta", "no response")))
    end
  elif e == "parado" && agora - rede_pedido_ms > 15000
    rede_pedido_ms = agora
    if !rede.pedir(URL, 16384) erro(idioma.escolher("rede: pedido recusado", "network: request refused")) end
  end
end

# ---- FUNDO (background): heavy calculation outside desenhar ----------------------
# fundo() has a 200 ms deadline (if exceeded, the system closes the app): the
# calculation adjusts itself to take about 100 ms, half the deadline.
var fundo_vezes = 0
var fundo_pior_ms = 0.0
var fundo_n = 500        # iterations of the calculation (adjusted)
var FUNDO_ALVO_MS = 100.0

def fundo()
  var t0 = sistema.us()
  var s = 0.0
  for i: 1 .. fundo_n s += math.sqrt(i) * math.sin(i) end
  var ms = (sistema.us() - t0) / 1000.0
  fundo_vezes += 1
  if ms > fundo_pior_ms fundo_pior_ms = ms end
  if ms < FUNDO_ALVO_MS && fundo_n < 100000 fundo_n = int(fundo_n * 1.2) + 1 end
  if ms > FUNDO_ALVO_MS && fundo_n > 50     fundo_n = int(fundo_n * 0.8) end
  sistema.fundo_estado(idioma.escolher("estresse", "stress"), fundo_vezes)
end

# ---- what the system calls ---------------------------------------------------
def abrir()
  mand = tela.camada(MAND, MAND)
  fase_desde = sistema.ms()
  resumo_desde = fase_desde
  sistema.acordado(true)          # the screen doesn't switch off during the test
  sistema.fundo(true, 500)
  cartao.gravar(ARQ, "")
  sistema.log("estresse: comecou")
end

def fechar()
  sistema.fundo(false)
  sistema.acordado(false)
  blocos = []
  gc.collect()
  if mand != nil tela.liberar(mand) mand = nil end
  sistema.log(format("estresse: fim - %d erro(s), ultimo: %s", erros, ultimo_erro))
end

var primeiro = true      # the first desenhar notes it in the log (the window ran)

def resumo(agora)
  var dur = (agora - resumo_desde) / 1000.0
  fps = quadros / dur
  med_ms = soma_ms / (quadros > 0 ? quadros : 1)
  pior_mostrado = pior_ms
  var m = sistema.memoria()
  var en = sistema.energia()
  sistema.log(format("estresse: fase %s | %.1f qps, quadro %.1f ms (pior %.0f) | figuras %d, mandel %d lin | " +
                     "berry %d KB (pico %d) de %d | cartao %d lin %d leit | rede %d ok %d falha | " +
                     "fundo %d x%d voltas (pior %.0f ms) | %.0f mA %d%% | erros %d",
                     FASES[fase], fps, med_ms, pior_ms, n_fig, mand_linhas,
                     m["berry"] / 1024, mem_pico / 1024, m["orcamento"] / 1024, cart_linhas, cart_leituras,
                     rede_ok, rede_falhas, fundo_vezes, fundo_n, fundo_pior_ms, en["ma"], en["pct"], erros))
  quadros = 0
  soma_ms = 0.0
  pior_ms = 0.0
  resumo_desde = agora
end

def desenhar(tudo)
  var t0 = sistema.us()
  var agora = sistema.ms()
  if primeiro sistema.log("estresse: primeiro quadro") primeiro = false end
  var l = tela.largura()
  var a = tela.altura()

  if agora - fase_desde > FASE_MS
    fase = (fase + 1) % size(FASES)
    fase_desde = agora
    sistema.log("estresse: fase " + FASES[fase])
  end

  tela.limpar(tela.PRETO)
  var topo = 200                       # the numbers panel sits at the top
  if ligado("Tela")    figuras(0, topo, l, a - topo) end
  if ligado("CPU")     mandelbrot() end
  if mand != nil       tela.colar(mand, l - MAND - 10, topo + 10) end
  if ligado("Memoria") memoria() end
  if ligado("Cartao")  cartao_trabalho(agora) end
  if ligado("Rede")    rede_trabalho(agora) end

  # Panel.
  var m = sistema.memoria()
  var en = sistema.energia()
  var resta = (FASE_MS - (agora - fase_desde)) / 1000
  tela.retangulo(0, 0, l, topo - 6, tela.cor(20, 20, 40))
  tela.texto(format(idioma.escolher("ESTRESSE  fase %d/%d: %s  (%ds)", "STRESS  phase %d/%d: %s  (%ds)"), fase + 1, size(FASES), FASES[fase], resta),
             12, 8, 3, tela.AMARELO)
  tela.texto(format(idioma.escolher("%.1f quadros/s   quadro %.1f ms   pior %.0f ms", "%.1f frames/s   frame %.1f ms   worst %.0f ms"), fps, med_ms, pior_mostrado),
             12, 48, 2.5, tela.BRANCO)
  tela.texto(format(idioma.escolher("figuras %d   Mandelbrot %d lin/quadro   fundo %d (pior %.0f ms)", "shapes %d   Mandelbrot %d lines/frame   background %d (worst %.0f ms)"),
                    n_fig, mand_linhas, fundo_vezes, fundo_pior_ms), 12, 80, 2, tela.CIANO)
  tela.texto(format(idioma.escolher("Berry %d KB (pico %d) de %d KB   enchidas %d", "Berry %d KB (peak %d) of %d KB   fills %d"), m["berry"] / 1024,
                    mem_pico / 1024, m["orcamento"] / 1024, mem_ciclos), 12, 108, 2, tela.CIANO)
  tela.texto(format(idioma.escolher("cartao %d linhas %d leituras   rede %d ok %d falhas (%d ms)", "card %d lines %d reads   network %d ok %d failed (%d ms)"),
                    cart_linhas, cart_leituras, rede_ok, rede_falhas, rede_ms), 12, 136, 2, tela.CIANO)
  tela.texto(format(idioma.escolher("%.0f mA  %d%%   erros %d %s", "%.0f mA  %d%%   errors %d %s"), en["ma"], en["pct"], erros, ultimo_erro),
             12, 164, 2, erros > 0 ? tela.VERMELHO : tela.VERDE)

  # Frame accounting.
  var ms = (sistema.us() - t0) / 1000.0
  quadros += 1
  soma_ms += ms
  if ms > pior_ms pior_ms = ms end
  if agora - resumo_desde >= 10000 resumo(agora) end
  sistema.redesenhar()
end

def tocar(x, y)
  fase = (fase + 1) % size(FASES)
  fase_desde = sistema.ms()
  sistema.log("estresse: fase " + FASES[fase] + " (toque)")
end
