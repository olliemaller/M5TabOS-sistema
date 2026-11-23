# nome: Bench
# cor: 0xFFE0
# periodo: 16
#
# Measures what each thing costs in a Berry app: Berry itself (loop, sum,
# text, list, system call) and each kind of screen drawing. One test per
# frame, each repeated N times; the result is the time PER OPERATION, with
# the empty-loop cost already removed. The table goes to the screen and to
# the serial log (for pasting).

import string

var N = 200
var testes = []     # [name, function] - the function does ONE operation
var res = []        # us per operation, in test order
var atual = 0
var vazio = 0.0     # us per empty-loop iteration
var area_y = 0      # where the test drawings land (below the table)

def t(nome, f) testes.push([nome, f]) end

# ---- Berry itself ------------------------------------------------------------
var x = 0
var txt = ""
t(idioma.escolher("laco vazio", "empty loop"), def (i) end)
t(idioma.escolher("soma em variavel", "add to variable"), def (i) x += i end)
t(idioma.escolher("chamada ao sistema", "system call"), def (i) tela.cor(1, 2, 3) end)
t(idioma.escolher("texto + str(i)", "text + str(i)"), def (i) txt = "valor " + str(i) end)
t(idioma.escolher("lista [1,2,3]", "list [1,2,3]"), def (i) var l = [1, 2, 3] end)
t(idioma.escolher("mapa {a:1}", "map {a:1}"), def (i) var m = {"a": 1} end)

# ---- the screen --------------------------------------------------------------
t(idioma.escolher("retangulo 1x1", "rectangle 1x1"), def (i) tela.retangulo(20, area_y, 1, 1, tela.VERMELHO) end)
t(idioma.escolher("retangulo 50x40", "rectangle 50x40"), def (i) tela.retangulo(20, area_y, 50, 40, tela.VERMELHO) end)
t(idioma.escolher("retangulo 300x28", "rectangle 300x28"), def (i) tela.retangulo(20, area_y, 300, 28, tela.VERDE) end)
t(idioma.escolher("retangulo vazado 50x40", "rectangle outline 50x40"), def (i) tela.retangulo(20, area_y, 50, 40, tela.AZUL, false) end)
t("pixel", def (i) tela.pixel(20 + i % 100, area_y, tela.BRANCO) end)
t(idioma.escolher("linha 100 px", "line 100 px"), def (i) tela.linha(20, area_y, 120, area_y + 37, tela.BRANCO) end)
t(idioma.escolher("circulo r24", "circle r24"), def (i) tela.circulo(60, area_y + 30, 24, tela.AMARELO) end)
t(idioma.escolher("circulo vazado r24", "circle outline r24"), def (i) tela.circulo(60, area_y + 30, 24, tela.AMARELO, false) end)
t(idioma.escolher("arredondado 120x80", "rounded 120x80"), def (i) tela.arredondado(20, area_y, 120, 80, 20, tela.VERDE) end)
t(idioma.escolher("triangulo", "triangle"), def (i) tela.triangulo(20, area_y + 60, 60, area_y, 100, area_y + 60, tela.LARANJA) end)
t(idioma.escolher("texto 20 letras", "text 20 chars"), def (i) tela.texto("Ola mundo do Berry!!", 20, area_y, 3) end)
t(idioma.escolher("texto 20 letras tam 5", "text 20 chars size 5"), def (i) tela.texto("Ola mundo do Berry!!", 20, area_y, 5) end)
t(idioma.escolher("medir 20 letras", "measure 20 chars"), def (i) tela.medir("Ola mundo do Berry!!", 3) end)
t(idioma.escolher("botao", "button"), def (i) tela.botao(20, area_y, 300, 70, "Botao") end)

# ---- layers ------------------------------------------------------------------
# Created once, on opening (allowed outside desenhar: a layer is an image).
var cam_p = tela.camada(300, 200)
var cam_l = tela.camada(tela.largura(), 200)
t(idioma.escolher("colar camada 300x200", "paste layer 300x200"), def (i) tela.colar(cam_p, 20, area_y - 80) end)
t(idioma.escolher("colar camada largura x200", "paste layer width x200"), def (i) tela.colar(cam_l, 0, area_y - 80) end)
t(idioma.escolher("colar pedaco 50x40", "paste piece 50x40"), def (i) tela.colar(cam_p, 20, area_y, 10, 10, 50, 40) end)
t(idioma.escolher("texto 20 letras na camada", "text 20 chars on layer"), def (i)
  tela.alvo(cam_p)
  tela.texto("Ola mundo do Berry!!", 4, 4, 3)
  tela.alvo()
end)

def medir(f, n)
  var t0 = sistema.us()
  for i: 0 .. n - 1 f(i) end
  return (sistema.us() - t0) * 1.0 / n
end

def desenhar(tudo)
  var l = tela.largura()
  area_y = tela.altura() - 120

  # One test per frame (each must fit within desenhar's 2 s deadline).
  if atual < size(testes)
    var tt = testes[atual]
    var n = atual < 6 ? N * 10 : N
    var us = medir(tt[1], n)
    if atual == 0 vazio = us end
    var liquido = atual == 0 ? us : us - vazio
    res.push(liquido)
    sistema.log(string.format("bench %-24s %8.1f us", tt[0], liquido))
    atual += 1
    if atual == size(testes) sistema.periodo(0) end
  end

  tela.limpar()
  tela.texto(idioma.escolher("Custo por operacao (us)", "Cost per operation (us)"), 24, 16, 3, tela.AMARELO)
  var y = 64
  for i: 0 .. size(res) - 1
    tela.texto(testes[i][0], 24, y, 2.5)
    tela.texto(string.format("%.1f", res[i]), l - 24, y, 2.5, tela.CIANO, tela.DIREITA)
    y += 34
  end
  if atual < size(testes)
    tela.texto(idioma.escolher("medindo ", "measuring ") + testes[atual][0] + "...", 24, y + 10, 2.5, tela.CINZA_CLARO)
  else
    tela.botao(24, y + 10, 300, 70, idioma.escolher("Repetir", "Repeat"), tela.CIANO)
  end
end

def tocar(px, py)
  if atual >= size(testes)
    res = []
    atual = 0
    sistema.periodo(16)
  end
end

