# nome: ESP-NOW Berry
# cor: 0x07FF
# periodo: 1000
# avisos: espnow
#
# The system's ESP-NOW app, redone in Berry: proof that a real-world app
# fits in a script. Status bar (radio, channel, who has been heard), the
# conversation in bubbles, scrolling with your finger, and a field you type
# into with the system's on-screen keyboard or the physical one. The radio,
# the history and the notice while the app is closed belong to the system
# service - the script only displays and sends.
#
# '# avisos: espnow' makes the system redraw the app THE MOMENT a message
# arrives (and call aviso("espnow")); the 1 s period is only for the bar's
# "last N s ago".

import string

var BARRA_A = 88         # the top bar
var CAMPO_A = 96         # the bottom bar (field + Send)
var M = 16
var PAD = 14             # padding inside the bubble
var TAM = 2.6            # the message text size (see letra())
var LH = 30              # height of one line of it
var LETRAS = [2.6, 3.2, 3.8, 4.6]   # the bar's "Aa" button steps from one to the next
var nivel_letra = 0
var aa = [0, 0]          # where the "Aa" button was drawn (x0, x1)
var CANAIS = [0, 1, 6, 11]   # 0 = the router's; 1, 6 and 11 don't overlap
var CORES_MAC = []       # stable colour per device (filled in abrir)
var MAX_TXT = 250        # the most the radio carries in one message

var rascunho = ""
var rolagem = 0          # px the conversation has moved down to show older ones
var rol_max = 0
var dedo_y = nil         # conversation drag in progress
var pilula = [0, 0]      # where the channel pill was drawn (x0, x1)
var medidas = {}         # index -> [width, text, lines, bubble width] (see medida())

# ---- helpers ---------------------------------------------------------------------
def cor_do_mac(mac)
  var h = 0
  for i: 0 .. size(mac) - 1 h = (h * 31 + string.byte(mac[i])) % 9973 end
  return CORES_MAC[h % size(CORES_MAC)]
end

def sem_ultimo(s)
  var i = size(s) - 1
  while i > 0 && (string.byte(s[i]) & 0xC0) == 0x80 i -= 1 end
  if i <= 0 return "" end
  return s[0 .. i - 1]
end

# Wraps the text into lines that fit in 'larg' (only in desenhar: uses tela.medir).
def quebrar(txt, larg)
  var linhas = []
  var atual = ""
  for w: string.split(txt, " ")
    var tenta = atual == "" ? w : atual + " " + w
    if tela.medir(tenta, TAM) <= larg
      atual = tenta
    else
      if atual != "" linhas.push(atual) end
      # Word longer than the whole line: cut it.
      while size(w) > 1 && tela.medir(w, TAM) > larg
        var k = size(w) - 1
        while k > 1 && tela.medir(w[0 .. k - 1], TAM) > larg k -= 1 end
        linhas.push(w[0 .. k - 1])
        w = w[k .. size(w) - 1]
      end
      atual = w
    end
  end
  if atual != "" || size(linhas) == 0 linhas.push(atual) end
  return linhas
end

# Wrapping and bubble width are expensive (they measure word by word): they
# are cached by INDEX, checked against the text - comparing text allocates
# nothing, and if the history moves on (the oldest drops off), the
# measurement is redone automatically.
def medida(i, m, larg)
  var q = medidas.find(i)
  if q != nil && q[0] == larg && q[1] == m["texto"] return q end
  if size(medidas) > 300 medidas = {} end
  var linhas = quebrar(m["texto"], larg)
  var w = 0
  for t: linhas
    var x = tela.medir(t, TAM)
    if x > w w = x end
  end
  var bl = w + 2 * PAD
  if bl < 200 bl = 200 end
  q = [larg, m["texto"], linhas, bl]
  medidas[i] = q
  return q
end

def enviar()
  if rascunho == "" || !espnow.pronto() return end
  if espnow.enviar(rascunho)
    rascunho = ""
    rolagem = 0
  else
    sistema.bip(220, 120)   # queue full or radio off: the text stays in the field
  end
end

# The conversation's FONT SIZE: changes the text and line height together,
# and throws away the cached measurements (made with the old size).
def letra(n)
  if n < 0 || n >= size(LETRAS) n = 0 end
  nivel_letra = n
  TAM = LETRAS[n]
  LH = int(TAM * 8 + 9.5)
  medidas = {}
end

# ---- the life cycle ---------------------------------------------------------------
def abrir()
  var t = cartao.ler("letra.txt")   # last time's choice (app folder on the card)
  if t != nil letra(int(t)) end
  CORES_MAC = [tela.cor(135, 206, 235), tela.LARANJA, tela.cor(173, 255, 47), tela.ROSA,
               tela.cor(255, 215, 0), tela.CIANO, tela.cor(238, 130, 238), tela.CINZA_CLARO]
  espnow.janela(true)      # conversation in front: receiver on, no status-bar notice
  espnow.marcar_lidas()
end

def fechar()
  espnow.janela(false)
end

def aviso(nome)
  espnow.marcar_lidas()    # with the conversation open, what arrives is born read
end

# ---- drawing ---------------------------------------------------------------------
def desenhar_barra(l, est)
  var fundo = tela.cor(13, 17, 23)
  tela.retangulo(0, 0, l, BARRA_A, fundo)
  var cy = BARRA_A / 2
  var cod = est["codigo"]
  tela.circulo(34, cy, 15, cod == 3 ? tela.cor(173, 255, 47) : cod == 2 ? tela.LARANJA : tela.CINZA)
  tela.texto("ESP-NOW", 64, cy - 15, 3.6)
  var x = 64 + tela.medir("ESP-NOW", 3.6) + 24

  # The channel pill is a button: orange = fixed channel, blue = the router's.
  var fixo = espnow.canal_fixo()
  var canal = fixo ? "ch " + str(fixo) : cod == 3 ? "ch " + str(espnow.canal()) + " auto" : "ch auto"
  var pl = tela.medir(canal, 3) + 40
  tela.arredondado(x, cy - 33, pl, 66, 33, fixo ? tela.cor(120, 70, 10) : tela.cor(38, 60, 90))
  tela.texto(canal, x + 20, cy - 12, 3)
  pilula = [x, x + pl]
  x += pl + 14

  # "Aa": the conversation font size (the button's letters grow with it).
  var al = 76
  tela.arredondado(x, cy - 33, al, 66, 33, tela.cor(38, 60, 90))
  tela.texto("Aa", x + al / 2, cy - int((2.4 + 0.4 * nivel_letra) * 4), 2.4 + 0.4 * nivel_letra, tela.BRANCO, tela.CENTRO)
  aa = [x, x + al]
  x += al + 20

  # The rest, shrunk until it fits.
  var info
  if cod == 3
    var ms = espnow.ultimo()
    var ult = ms < 0 ? idioma.escolher("nada ouvido", "nothing heard") : ms < 60000 ? idioma.escolher("ultimo ", "last ") + str(ms / 1000) + "s" : idioma.escolher("ultimo ", "last ") + str(ms / 60000) + "min"
    info = str(espnow.vistos()) + idioma.escolher(" ouvido(s)  ", " heard  ") + ult
    if l > 1000 info = espnow.mac() + "   " + info end
  else
    info = cod == 2 ? idioma.escolher("ligando...", "starting...") : cod == 4 ? idioma.escolher("radio parado", "radio stopped") : idioma.escolher("fora do ar", "off air")
  end
  var tam = 3.0
  while tam > 1.8 && tela.medir(info, tam) > l - M - x tam -= 0.2 end
  tela.texto(info, x, cy - int(tam * 4), tam, tela.CINZA_CLARO)
  tela.linha(0, BARRA_A - 1, l, BARRA_A - 1, tela.cor(38, 42, 50))
end

def desenhar_campo(l, a)
  var y = a - CAMPO_A
  tela.retangulo(0, y, l, CAMPO_A, tela.cor(13, 17, 23))
  tela.linha(0, y, l, y, tela.cor(38, 42, 50))
  var bl = 170
  var fl = l - 3 * M - bl
  tela.arredondado(M, y + 14, fl, CAMPO_A - 28, 14, tela.cor(30, 33, 40))
  if rascunho == ""
    tela.texto(teclado.aberto() || teclado.fisico() ? idioma.escolher("escreva...", "type...") : idioma.escolher("toque para escrever", "tap to type"),
               M + 18, y + 33, 2.8, tela.CINZA)
  else
    # Shows the END of the draft when it doesn't fit (the cursor is there).
    var t = rascunho
    while size(t) > 1 && tela.medir(t + "|", 2.8) > fl - 36 t = t[1 .. size(t) - 1] end
    tela.texto(t + "|", M + 18, y + 33, 2.8)
  end
  tela.botao(l - M - bl, y + 14, bl, CAMPO_A - 28, idioma.escolher("Enviar", "Send"),
             rascunho != "" && espnow.pronto() ? tela.CIANO : tela.CINZA)
end

# The conversation from the bottom up: the newest sits against the field;
# scrolling moves the whole lot down to show older ones. Anything that goes
# over the bar is covered by it, as it is drawn afterwards. Stops as soon as
# it goes off the top: what is above the screen is not even fetched.
def desenhar_conversa(l, topo, base, est)
  var n = espnow.n()
  if n == 0
    var txt = est["texto"] != "" ? est["texto"] : idioma.escolher("nada por aqui ainda - escreva algo", "nothing here yet - write something")
    tela.texto(txt, l / 2, (topo + base) / 2 - 12, 2.6, tela.CINZA, tela.CENTRO)
    rol_max = 0
    return
  end
  var bw = l * 78 / 100
  var y = base - M + rolagem
  var i = n - 1
  var m = espnow.msg(i)
  while m != nil
    # The previous one is used twice: for this one's label and, on the next
    # pass, as the current message - only one fetch per message.
    var ant = i > 0 ? espnow.msg(i - 1) : nil
    var q = medida(i, m, bw - 2 * PAD)
    var linhas = q[2]
    var bl = q[3]
    # Sender label only when it changes (a run doesn't repeat the name).
    var rotulo = ""
    if !m["minha"] && (ant == nil || ant["minha"] || ant["de"] != m["de"])
      rotulo = m["de"][12 .. 13] + m["de"][15 .. 16]
    end
    var h = 2 * PAD + size(linhas) * LH + 28 + (rotulo != "" ? 30 : 0)
    y -= h
    if y < base
      var x = m["minha"] ? l - M - bl : M
      tela.arredondado(x, y, bl, h, 18, m["minha"] ? tela.cor(30, 70, 130) : tela.cor(45, 45, 52))
      var ty = y + PAD
      if rotulo != ""
        tela.texto(rotulo, x + PAD, ty, 2.4, cor_do_mac(m["de"]))
        ty += 30
      end
      for t: linhas
        tela.texto(t, x + PAD, ty, TAM)
        ty += LH
      end
      # The time and, below it: the send status (mine) or the dialect (received ones).
      var meta = sistema.hora_de(m["quando"])
      var cor_meta = tela.CINZA_CLARO
      if m["minha"]
        var e = m["envio"]
        meta += e == 1 ? idioma.escolher("  enviando", "  sending") : e == 2 ? idioma.escolher("  enviada", "  sent") : e == 3 ? idioma.escolher("  FALHOU", "  FAILED") : ""
        if e == 3 cor_meta = tela.VERMELHO end
      else
        meta += m["espressif"] ? "  esp-now Espressif" : idioma.escolher("  texto puro", "  plain text")
      end
      tela.texto(meta, x + bl - PAD, ty + 2, 2, cor_meta, tela.DIREITA)
    end
    y -= 12
    if y < topo break end   # gone off the top: the rest isn't visible
    m = ant
    i -= 1
  end
  # How far it can scroll: if it reached the oldest, we know exactly; if it
  # stopped earlier, there is more above - the sum settles once scrolling
  # gets there.
  if m == nil || i <= 0
    rol_max = rolagem + (topo + M - y > 0 ? topo + M - y : 0)
  else
    rol_max = rolagem + 100000
  end
end

def desenhar(tudo)
  var l = tela.largura()
  var a = tela.altura()       # with the keyboard open, already excluding it
  var est = espnow.estado()
  if rolagem > rol_max rolagem = rol_max end
  desenhar_conversa(l, BARRA_A, a - CAMPO_A, est)
  desenhar_barra(l, est)
  desenhar_campo(l, a)
end

# ---- tap, finger and keys ----------------------------------------------------------
def tocar(x, y)
  var l = tela.largura()
  var a = tela.altura()
  if y < BARRA_A
    if x >= aa[0] && x < aa[1]
      letra((nivel_letra + 1) % size(LETRAS))
      cartao.gravar("letra.txt", str(nivel_letra))
    elif x >= pilula[0] && x < pilula[1]
      # Router -> 1 -> 6 -> 11 -> router.
      var k = CANAIS.find(espnow.canal_fixo())
      espnow.fixar_canal(CANAIS[k == nil ? 0 : (k + 1) % size(CANAIS)])
    end
    return
  end
  if y >= a - CAMPO_A
    if x >= l - M - 170
      enviar()
    elif !teclado.fisico()
      if teclado.aberto() teclado.fechar() else teclado.abrir() end
    end
  end
end

# Dragging the conversation: up shows newer ones, down shows older ones.
def dedo(x, y, pressionado)
  if !pressionado
    dedo_y = nil
    return
  end
  if y < BARRA_A || y >= tela.altura() - CAMPO_A return end
  if dedo_y != nil
    rolagem += y - dedo_y
    if rolagem < 0 rolagem = 0 end
    if rolagem > rol_max rolagem = rol_max end
  end
  dedo_y = y
end

def tecla(texto, codigo)
  if texto != ""
    if size(rascunho) + size(texto) <= MAX_TXT rascunho += texto end
  elif codigo == teclado.APAGAR
    rascunho = sem_ultimo(rascunho)
  elif codigo == teclado.ENTER
    enviar()
  elif codigo == teclado.ESC
    teclado.fechar()
  elif codigo == teclado.CIMA
    rolagem += 3 * LH
  elif codigo == teclado.BAIXO
    rolagem -= 3 * LH
    if rolagem < 0 rolagem = 0 end
  end
end
