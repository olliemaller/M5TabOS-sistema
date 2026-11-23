# nome: Camera Berry
# cor: 0xFD20
# orientacao: fixo
#
# The Tab5 camera in Berry: the live preview and a photo saved to the card.
#
# The preview does NOT go through the script: the camera writes each frame
# straight to the screen (in the band requested in camera.ligar), and
# desenhar() only paints what is around it - plus whatever you want ON TOP of
# the preview (here, the rule-of-thirds grid). That's why the app stays smooth
# even with the script drawing every frame.
#
# The photo comes out at 1200x1600 and goes to the app's folder on the card
# (/sdcard/apps/Berry/Camera Berry/foto_0001.bmp...). The preview only works
# with the app in portrait - hence the '# orientacao: fixo' above (portrait
# even with the physical keyboard attached, which turns other apps landscape).

var BARRA = 170          # the bottom band: buttons and status
var M = 24
var grade = true
var fotos = 0            # taken since the app was opened
var estado_ant = ""      # so the beep sounds only once when the photo finishes

def abrir()
  # The preview fills the whole area except the bottom bar. It starts right
  # after abrir(): camera.ligada() is already valid on the first desenhar.
  camera.ligar(0, tela.altura() - BARRA)
  sistema.acordado(true)          # the screen doesn't switch off while the camera is open
end

def desenhar_grade(f, l)
  var y0 = f[0]
  var a = f[1]
  var c = tela.cor(255, 255, 255)
  tela.linha(l / 3, y0, l / 3, y0 + a, c)
  tela.linha(2 * l / 3, y0, 2 * l / 3, y0 + a, c)
  tela.linha(0, y0 + a / 3, l, y0 + a / 3, c)
  tela.linha(0, y0 + 2 * a / 3, l, y0 + 2 * a / 3, c)
end

def desenhar(tudo)
  var l = tela.largura()
  var a = tela.altura()
  var f = camera.faixa()          # [y, height] of the preview, or nil

  if f == nil
    tela.texto(camera.erro() != "" ? camera.erro() : idioma.escolher("ligando a camera...", "starting the camera..."), l / 2, (a - BARRA) / 2, 3,
               tela.CINZA_CLARO, tela.CENTRO)
  elif grade
    desenhar_grade(f, l)
  end

  # The bottom bar: PHOTO, GRID and what is happening.
  var y = a - BARRA + 16
  var e = camera.foto_estado()
  var ocupado = e == "capturando" || e == "gravando"
  tela.botao(M, y, l - 2 * M - 220, 90, ocupado ? idioma.escolher("SALVANDO...", "SAVING...") : idioma.escolher("FOTO", "PHOTO"),
             ocupado ? tela.CINZA : tela.LARANJA)
  tela.botao(l - M - 200, y, 200, 90, grade ? idioma.escolher("SEM GRADE", "NO GRID") : idioma.escolher("GRADE", "GRID"), tela.cor(50, 55, 65))

  var txt = ""
  if e == "ok"          txt = idioma.escolher("salva: ", "saved: ") + camera.ultima()
  elif e == "falhou"    txt = idioma.escolher("a foto falhou (cartao?)", "the photo failed (SD card?)")
  elif ocupado          txt = e + "..."
  else                  txt = str(fotos) + idioma.escolher(" foto(s) nesta abertura", " photo(s) this session")
  end
  tela.texto(txt, M, y + 108, 2.5, e == "falhou" ? tela.VERMELHO : tela.CINZA_CLARO)

  # The beep when the photo finishes saving (once).
  if e != estado_ant
    if e == "ok" sistema.bip(1760, 60) end
    estado_ant = e
  end
end

def tocar(x, y)
  var l = tela.largura()
  var a = tela.altura()
  var by = a - BARRA + 16
  if tela.dentro(x, y, M, by, l - 2 * M - 220, 90)
    if camera.foto()
      fotos += 1
      sistema.bip(1200, 30)        # the shutter click
    end
  elif tela.dentro(x, y, l - M - 200, by, 200, 90)
    grade = !grade
  end
end

def fechar()
  camera.desligar()               # the system also switches it off on close
end
