# nome: Importar
# cor: 0x5D1F
# avisos: arquivos, volumes
#
# The 'arquivos' module: browse the SD card and the USB flash drive and BRING
# a file into the app's folder. The system's rule: the script sees the whole
# disks, but only changes its own folder (apps/Berry/Importar/) - anything
# outside it, the script copies in and then reads.
#
# Tabs: DISKS (browse and tap a file to bring it in) and MINE (what has
# already been brought in; tapping a text file shows its beginning).

import string

var aba = 0            # 0 = disks, 1 = mine
var pasta = ""         # "" = the list of disks; otherwise, the open path
var itens = []         # what the screen shows
var pagina = 0
var texto = nil        # the text open in the MINE tab (nil = the list)
var msg = ""

var LIN = 70           # height of one list row
var TOPO = 200         # where the list starts

def por_pagina() return (tela.altura() - TOPO - 110) / LIN end

def carregar()
  pagina = 0
  texto = nil
  if aba == 1
    itens = arquivos.lista("")
  elif pasta == ""
    itens = []
    for v: arquivos.volumes()
      if v["disponivel"]
        itens.push({"nome": v["tipo"] + (v["nome"] != "" ? "  " + v["nome"] : ""),
                    "pasta": true, "caminho": v["raiz"], "tipo": "pasta", "bytes": 0})
      end
    end
  else
    itens = arquivos.lista(pasta)
    if itens == nil itens = [] msg = idioma.escolher("a pasta não abriu", "the folder did not open") end
  end
end

def subir()
  var i = size(pasta) - 1
  while i > 0 && pasta[i] != "/" i -= 1 end
  if pasta == "/sdcard" || pasta == "/usb" pasta = "" else pasta = pasta[0 .. i - 1] end
  carregar()
end

def tamanho(b)
  if b >= 1048576 return format("%.1f MB", b / 1048576.0) end
  if b >= 1024 return format("%.1f KB", b / 1024.0) end
  return str(b) + " B"
end

def desenhar(tudo)
  var L = tela.largura()
  tela.botao(20, 16, 220, 70, idioma.escolher("DISCOS", "DISKS"), aba == 0 ? tela.AZUL : tela.CINZA)
  tela.botao(260, 16, 220, 70, idioma.escolher("MEUS", "MINE"), aba == 1 ? tela.AZUL : tela.CINZA)
  if (aba == 0 && pasta != "") || texto != nil tela.botao(L - 200, 16, 180, 70, idioma.escolher("< Voltar", "< Back")) end

  # The copy's progress (the system copies in the background).
  var e = arquivos.estado()
  var linha = msg
  if e["meu"] && e["fase"] == "copiando" linha = idioma.escolher("trazendo ", "bringing in ") + e["atual"] + "  " + str(e["pct"]) + "%"
  elif e["meu"] && e["fase"] == "feito"  linha = idioma.escolher("pronto: está em MEUS", "done: it is in MINE")
  elif e["meu"] && e["fase"] == "falhou" linha = idioma.escolher("falhou: ", "failed: ") + e["erro"]
  end
  var titulo = aba == 1 ? "apps/Berry/Importar" : (pasta == "" ? idioma.escolher("Discos", "Disks") : pasta)
  tela.texto(tela.cortar(titulo, L - 40, 3), 20, 110, 3)
  tela.texto(tela.cortar(linha, L - 40, 2.5), 20, 152, 2.5, tela.CINZA_CLARO)

  if texto != nil
    var y = TOPO
    for l: string.split(texto, "\n")
      if y > tela.altura() - 60 break end
      tela.texto(tela.cortar(l, L - 40, 2.5), 20, y, 2.5)
      y += 34
    end
    return
  end

  var n = por_pagina()
  var y = TOPO
  for i: pagina * n .. pagina * n + n - 1
    if i >= size(itens) break end
    var it = itens[i]
    var cor = it["pasta"] ? tela.AMARELO : (it["tipo"] == "texto" ? tela.CIANO : tela.BRANCO)
    tela.texto(tela.cortar((it["pasta"] ? "[ ] " : "") + it["nome"], L - 220, 3), 20, y, 3, cor)
    if !it["pasta"] tela.texto(tamanho(it["bytes"]), L - 20, y, 2.5, tela.CINZA_CLARO, tela.DIREITA) end
    y += LIN
  end
  if size(itens) == 0 tela.texto(idioma.escolher("(vazio)", "(empty)"), 20, TOPO, 3, tela.CINZA) end
  if size(itens) > n
    var by = tela.altura() - 100
    tela.botao(20, by, 220, 80, idioma.escolher("ANTES", "PREV"), pagina > 0 ? tela.AZUL : tela.CINZA)
    tela.botao(260, by, 220, 80, idioma.escolher("DEPOIS", "NEXT"), (pagina + 1) * n < size(itens) ? tela.AZUL : tela.CINZA)
  end
end

def tocar(x, y)
  var L = tela.largura()
  if tela.dentro(x, y, 20, 16, 220, 70) aba = 0 msg = "" carregar() return end
  if tela.dentro(x, y, 260, 16, 220, 70) aba = 1 msg = "" carregar() return end
  if tela.dentro(x, y, L - 200, 16, 180, 70)
    if texto != nil texto = nil elif aba == 0 && pasta != "" subir() end
    return
  end
  var n = por_pagina()
  if size(itens) > n && y >= tela.altura() - 100
    if x < 240 && pagina > 0 pagina -= 1 end
    if x >= 260 && x < 480 && (pagina + 1) * n < size(itens) pagina += 1 end
    return
  end
  if texto != nil || y < TOPO return end
  var i = pagina * n + (y - TOPO) / LIN
  if i >= size(itens) return end
  var it = itens[i]
  if aba == 1
    if it["tipo"] == "texto"
      texto = arquivos.ler(it["nome"])
      if texto == nil texto = idioma.escolher("(não consegui ler)", "(could not read it)") end
    end
  elif it["pasta"]
    pasta = it["caminho"]
    carregar()
  else
    var r = arquivos.copiar(it["caminho"])
    msg = r == true ? idioma.escolher("trazendo ", "bringing in ") + it["nome"] + "..." : idioma.escolher("não deu: ", "could not: ") + r
  end
end

# The copy progressed or finished, or a disk was plugged in/removed: the
# system already redraws; here we only refresh the list.
def aviso(nome)
  if nome == "volumes" && aba == 0 pasta = "" carregar() end
  if nome == "arquivos" && aba == 1 && texto == nil carregar() end
end

carregar()
