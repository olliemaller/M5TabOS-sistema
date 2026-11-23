# nome: Teste Quadros
# cor: 0x07FF
# periodo: 16
#
# Speed test: tells "drawing is slow" apart from "the IMU is slow".
#
# - YELLOW ball: moves by itself from side to side, driven by the clock
#   (sistema.ms). It doesn't depend on the IMU. If it moves in jumps, the
#   problem is the frame cadence, not the sensor.
# - GREEN ball: the IMU (tilt the device).
# - Numbers: frames per second, the time desenhar() takes in Berry, the
#   longest gap between frames in the last second, and how many NEW IMU
#   readings arrived per second (if it's well below the frame rate, the
#   sensor is returning the same value over and over).

var t0 = sistema.ms()       # start of the animation
var ultimo = t0              # the previous frame
var janela = t0              # start of the 1 s measurement window
var quadros = 0
var novas = 0
var pior = 0
var ant_ax = nil
var fps = 0
var imu_s = 0
var pior_s = 0
var custo = 0

def desenhar(tudo)
  var ini = sistema.ms()
  var l = tela.largura()

  # Measurements, totalled every second.
  var dt = ini - ultimo
  ultimo = ini
  if dt > pior pior = dt end
  quadros += 1
  if ini - janela >= 1000
    fps = quadros
    imu_s = novas
    pior_s = pior
    quadros = 0
    novas = 0
    pior = 0
    janela = ini
  end

  # The independent ball: goes there and back in 2 s, driven by the clock.
  var m = 60
  var curso = l - 2 * m
  var fase = ((ini - t0) % 4000) / 2000.0     # 0..2
  if fase > 1 fase = 2 - fase end
  tela.linha(m, 150, l - m, 150, tela.CINZA)
  tela.circulo(m + int(curso * fase), 150, 30, tela.AMARELO)

  # The IMU.
  var g = sistema.imu()
  var cx = l / 2
  var cy = 430
  tela.circulo(cx, cy, 180, tela.CINZA, false)
  tela.linha(cx - 180, cy, cx + 180, cy, tela.CINZA)
  tela.linha(cx, cy - 180, cx, cy + 180, tela.CINZA)
  if g != nil
    if ant_ax == nil || g[0] != ant_ax novas += 1 end
    ant_ax = g[0]
    tela.circulo(cx + int(g[0] * -170), cy + int(g[1] * 170), 26, tela.VERDE)
  else
    tela.texto(idioma.escolher("IMU sem leitura", "IMU not reading"), cx - 150, cy - 12, 3, tela.VERMELHO)
  end

  tela.texto(idioma.escolher("quadros/s: ", "frames/s: ") + str(fps) + idioma.escolher("   pior intervalo: ", "   worst gap: ") + str(pior_s) + " ms", 30, 20, 3)
  tela.texto("desenhar(): " + str(custo) + idioma.escolher(" ms   IMU novas/s: ", " ms   new IMU/s: ") + str(imu_s), 30, 60, 3)

  custo = sistema.ms() - ini
end
