// Console commands - BANCADA (BENCH): grove, i2c, camera, foto.
// (Split from console.cpp; the COMANDOS table and the help remain there.)

#include "tab5_bmp.hpp"
#include "tab5_camera.hpp"
#include "tab5_camera_ipa.h"
#include "hal/efuse_hal.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "tab5_grove.hpp"
#include "tab5_teclado.hpp"
#include "ajustes.h"
#include "internal.hpp"   // sistema_trocar_relogio_painel
#include "console/console_interno.hpp"

// ---------------------------------------------------------------------------
// i2c <sda> <scl>  - finds out who answers on a pair of pins
// ---------------------------------------------------------------------------
//
// It exists so as NOT to guess the protocol of new hardware. The Tab5's side
// connector exposes G49, G50, G0, G1 and EXT 5V; if the keyboard speaks I2C, its address
// appears here and the integration becomes trivial. If nothing answers in any
// combination, it is a sign it is not I2C - and then the next hypothesis is UART.
//
// Use the pair in BOTH directions: there is no way to tell from outside which is SDA and which is
// SCL, and swapped nothing answers.
//
//   i2c 49 50        i2c 50 49        i2c 0 1        i2c 1 0
//
// Run it from the home screen: the command touches the external bus, which is the
// same one Azimutal borrows as the GPS UART.
int cmd_i2c(int argc, char** argv)
{
  if (argc < 3) { printf("%s", tr("uso: i2c <sda> <scl>   (ex: i2c 49 50)\n")); return 1; }

  // THE WARNING BECAME A LOCK. Until now, "run it from the home screen" was just a
  // comment in the source - the command reconfigured the external bus from
  // under whoever was using it, without asking. Azimutal uses
  // this same bus as the GPS UART, and the ExtPort1 keyboard is also
  // on it: running this with either of the two open yanks the
  // bus from the owning task in the middle of a transaction.
  //
  // The CONSISTENCY worth having: a bench rule documented in a
  // comment is a rule that only holds while someone remembers to read the
  // comment. By making the command itself check, it always holds.
  const char* app = sistema_app_atual();
  if (app) {
    printf(tr("nao da para varrer o I2C com '%s' aberto - ele pode estar usando\n"
           "o mesmo barramento externo. Volte para a tela inicial e tente de novo.\n"), app);
    return 1;
  }

  const int sda = atoi(argv[1]);
  const int scl = atoi(argv[2]);

  // The physical keyboard is read by the UI task on Ex_I2C: reopening the
  // bus from under it would yank the read in the middle of a transaction.
  if (tab5_teclado_presente()) {
    printf("%s", tr("nao da para varrer o I2C com o teclado conectado - ele usa o\n"
                    "barramento externo. Desconecte o teclado e tente de novo.\n"));
    return 1;
  }

  // The EXTERNAL bus (Ex_I2C, the Grove one) is the only one we can reuse.
  // On the Tab5 the INTERNAL one (touch, IMU, INA226, expanders, RTC) is In_I2C
  // (I2C_NUM_1) and is owned by the UI task - touching it from here
  // would be the race the whole system avoids. That is why the port used is
  // always Ex_I2C's own, never a fixed one.
  const i2c_port_t porta = M5.Ex_I2C.getPort();
  if (porta == M5.In_I2C.getPort()) {
    printf("%s", tr("o barramento externo usa a mesma porta do interno - recusado\n"));
    return 1;
  }
  // Original port and pins: this is exactly what is restored at the end.
  const int sda_orig = M5.Ex_I2C.getSDA();
  const int scl_orig = M5.Ex_I2C.getSCL();

  M5.Ex_I2C.release();

  if (!M5.Ex_I2C.begin(porta, sda, scl))
  {
    printf(tr("nao consegui abrir I2C com SDA=%d SCL=%d\n"), sda, scl);
    M5.Ex_I2C.release();
    M5.Ex_I2C.begin(porta, sda_orig, scl_orig);
    return 1;
  }

  printf(tr("varrendo I2C em SDA=%d SCL=%d ...\n"), sda, scl);
  bool achou[128] = { };
  M5.Ex_I2C.scanID(achou);

  int n = 0;
  for (int a = 0; a < 128; ++a)
  {
    if (achou[a]) { printf(tr("  0x%02X responde\n"), (unsigned)a); ++n; }
  }
  if (n == 0) { printf("%s", tr("  ninguem respondeu\n")); }
  else        { printf(tr("  %d dispositivo(s)\n"), n); }

  // Returns the external bus to the previous port and pins.
  M5.Ex_I2C.release();
  M5.Ex_I2C.begin(porta, sda_orig, scl_orig);
  return 0;
}

// ---------------------------------------------------------------------------
// 'foto' - PROOF THAT THE CAMERA BECAME A SERVICE, in the SAME way
// that 'concorrencia'/'simulacao' above prove GPS/IMU: turns the camera on, takes
// ONE photo and turns it off again, entirely from OUTSIDE the Camera app - if this
// works with the Camera app CLOSED, and works again with it OPEN at the
// same time (see the reference count in tab5_camera.hpp), the
// independence between service and app is proven in hardware, not just on
// paper.
//
// The PHOTO comes out with the sensor's full frame (TAB5_CAM_FOTO_L x _A); this
// command has no preview.
void cmd_foto(void)
{

  // From the console task, directly: the sensor's SCCB goes through the internal
  // I2C lock (hal/tab5_sccb.cpp). The console shows up as using the camera in
  // the resources ('energia quem') while it is turned on by it.
  printf("%s", tr("ligando a camera...\n"));
  if (!tab5_camera_iniciar("console")) {
    printf("%s", tr("falhou - confira o log (sensor ausente?)\n"));
    return;
  }

  // The camera only CAPTURES (one frame, via PPA, on its task); writing happens here,
  // on the console task - see services/tab5_bmp and tab5_camera_pedir_quadro.
  uint16_t* foto = tab5_camera_quadro_alocar(TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A);
  bool ok = false;
  if (!foto) {
    printf("%s", tr("sem PSRAM para a foto\n"));
  } else if (!tab5_camera_pedir_quadro(foto, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A)) {
    printf("%s", tr("nao consegui pedir a foto (outra ainda pendente?)\n"));
  } else {
    bool pronta = false;
    for (int i = 0; i < 50 && !pronta; ++i) {    // 5 s: the first frame may take a while
      vTaskDelay(pdMS_TO_TICKS(100));
      pronta = tab5_camera_quadro_pronto(foto, &ok);
    }
    if (!pronta)  { printf("%s", tr("a camera nao entregou quadro em 5 s (confira o log)\n")); }
    else if (ok)  { ok = tab5_bmp_salvar("Console", "console", foto, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A);
                    printf("%s", ok ? tr("foto salva: /sdcard/apps/Console/console.bmp\n") : tr("a gravacao falhou (confira o log)\n")); }
    else          { printf("%s", tr("a captura falhou (confira o log)\n")); }
  }

  // ALWAYS releases the interest this command requested - even on error. If anyone
  // else (the Camera app, for example) is still using it, the reference count
  // keeps the sensor up; if this command was the only
  // one interested, it really turns off now.
  tab5_camera_parar("console");   // waits for the camera task - only then can the buffer go
  if (foto) { heap_caps_free(foto); }
}

// 'camera' - WHAT THE ISP IS DOING WITH COLOUR. Prints the state of our own
// IPA (hal/tab5_camera_ipa.c: AWB gains, the calibration matrix and the
// one sent to the ISP, the green a saturated white would have with each) and measures a
// whole frame: centre average, shadows average, fraction of clipped
// pixels and of magenta pixels. With the lens covered, the centre average is the
// black the sensor delivers - the direct test of the pedestal (tab5_camera.cpp,
// zerar_pedestal). 'camera reg' talks to the sensor while the Camera app is
// open, to try a register live.
void cmd_camera(int argc, char** argv)
{
  if (argc >= 3 && strcmp(argv[1], "reg") == 0) {
    const long reg   = strtol(argv[2], nullptr, 16);
    const int  valor = (argc >= 4) ? (int)strtol(argv[3], nullptr, 16) : -1;
    uint8_t lido = 0;
    if (!tab5_camera_reg((uint16_t)reg, valor, &lido)) {
      printf("%s", tr("camera desligada (abra o app Camera) ou o SCCB falhou\n"));
      return;
    }
    printf(tr("sensor 0x%04lx = 0x%02x\n"), reg, lido);
    return;
  }

  const unsigned rev = efuse_hal_chip_revision();
  printf(tr("P4 revisao v%u.%u - %s\n"), rev / 100, rev % 100,
         rev >= 300 ? tr("ISP com WBG e BLC (balanco e preto no hardware)")
                    : tr("ISP sem WBG nem BLC (balanco na CCM, preto pelo sensor)"));

  // Turned on by someone else (the Camera app, the browser): only observes - AE/AWB are already
  // settled.
  const bool ja_ligada = tab5_camera_ativa();
  if (!ja_ligada) {
    printf("%s", tr("ligando a camera e esperando o AE/AWB assentarem...\n"));
    if (!tab5_camera_iniciar("console")) { printf("%s", tr("falhou - confira o log\n")); return; }
    vTaskDelay(pdMS_TO_TICKS(2500));
  }

  tab5_cam_ipa_estado_t e;
  if (!tab5_camera_ipa_estado(&e)) {
    printf("%s", tr("o IPA proprio nao rodou nenhum quadro (fora do pipeline?)\n"));
  } else {
    printf(tr("quadros do ISP   : %lu\n"), (unsigned long)e.quadros);
    printf(tr("exposicao, ganho : %lu us, %.2fx\n"), (unsigned long)e.exposicao_us, (double)e.ganho_sensor);
    printf(tr("ganhos do AWB    : R %.2f  B %.2f\n"), (double)e.ganho_r, (double)e.ganho_b);
    printf("%s", tr("CCM calibracao   |  CCM no ISP\n"));
    for (int i = 0; i < 3; ++i) {
      printf("  %6.2f %6.2f %6.2f  |  %6.2f %6.2f %6.2f\n",
             (double)e.ccm_awb[i][0], (double)e.ccm_awb[i][1], (double)e.ccm_awb[i][2],
             (double)e.ccm_isp[i][0], (double)e.ccm_isp[i][1], (double)e.ccm_isp[i][2]);
    }
    printf(tr("verde do branco saturado: %.2f -> %.2f (1 = branco; abaixo de ~0,9 sai rosa)\n"),
           (double)e.g_branco_antes, (double)e.g_branco_depois);
    printf(tr("saturacao        : %lu (128 = 1,0)\n"), (unsigned long)e.saturacao);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) { total += e.hist[i]; }
    if (total) {
      printf("%s", tr("histograma do Y  :"));
      for (int i = 0; i < 16; ++i) { printf(" %lu", (unsigned long)(e.hist[i] * 100 / total)); }
      printf("%s", tr(" (%% por faixa de 16)\n"));
    }
  }
  uint8_t preto = 0;
  if (tab5_camera_reg(0x3908, -1, &preto)) { printf(tr("alvo do preto    : 0x%02x (fabrica 0x41)\n"), preto); }

  uint16_t* foto = tab5_camera_quadro_alocar(TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A);
  bool ok = false, pronta = false;
  if (foto && tab5_camera_pedir_quadro(foto, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A)) {
    for (int i = 0; i < 50 && !pronta; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); pronta = tab5_camera_quadro_pronto(foto, &ok); }
  }
  if (pronta && ok) {
    const int L = TAB5_CAM_FOTO_L, A = TAB5_CAM_FOTO_A;
    uint64_t c[3] = { }, sombra[3] = { };
    uint32_t n_c = 0, n_sombra = 0, n = 0, estouro = 0, magenta = 0;
    for (int y = 0; y < A; y += 2) {
      for (int x = 0; x < L; x += 2) {
        const uint16_t p = foto[y * L + x];
        const int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        ++n;
        if (r >= 248 && g >= 248 && b >= 248) { ++estouro; }
        const int menor_rb = r < b ? r : b;
        if (menor_rb >= 200 && g + 40 < menor_rb) { ++magenta; }
        if (r + g + b < 96) { sombra[0] += r; sombra[1] += g; sombra[2] += b; ++n_sombra; }
        if (abs(x - L / 2) < 100 && abs(y - A / 2) < 100) { c[0] += r; c[1] += g; c[2] += b; ++n_c; }
      }
    }
    printf(tr("centro (200x200) : R %llu G %llu B %llu\n"), c[0] / n_c, c[1] / n_c, c[2] / n_c);
    if (n_sombra) {
      printf(tr("sombras (%lu%%)    : R %llu G %llu B %llu (neutro = tres iguais)\n"),
             (unsigned long)(n_sombra * 100 / n), sombra[0] / n_sombra, sombra[1] / n_sombra, sombra[2] / n_sombra);
    }
    printf(tr("estourado        : %.2f%% do quadro\n"), estouro * 100.0 / n);
    printf(tr("magenta no branco: %.2f%% do quadro\n"), magenta * 100.0 / n);
  } else {
    printf("%s", tr("nao veio quadro para medir\n"));
  }

  if (!ja_ligada) { tab5_camera_parar("console"); }   // waits for the task: only then does the buffer go
  if (foto) { heap_caps_free(foto); }
}

// ---------------------------------------------------------------------------
// BENCH
// ---------------------------------------------------------------------------

void c_grove(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "trocar")) {
    // I SWAPPED THE MODULE ON THE CABLE. Stops the service, returns the port, probes again
    // and brings up whatever fits. Done by the system, on the UI task - from here
    // it is not safe to touch another task's UART.
    if (sistema_grove_redetectar()) {
      printf("%s", tr("pedido feito - a interface sonda no proximo ciclo (uns 4s).\n"
             "rode 'grove' depois para ver o que achou.\n"));
    } else {
      printf("%s", tr("um app esta usando o Grove agora - feche-o e tente de novo\n"));
    }
    return;
  }
  // 'grove cru <baud>', or the old form 'grove <baud>': dumps the raw
  // bytes, to LOOK instead of deducing.
  const char* baud = nullptr;
  if (argc >= 3 && eh(argv[1], "cru"))          { baud = argv[2]; }
  else if (argc >= 2 && atoi(argv[1]) > 0)      { baud = argv[1]; }
  if (baud) { tab5_grove_despejar(atoi(baud), 2500); return; }
  if (argc >= 2) { mostrar_uso("grove"); return; }

  // ONLY REPORTS - does not probe. Probing from here was a silent bug: the probe writes
  // and reads on the UART, and with the Meshtastic service up the port is ITS. Two
  // tasks on the same UART scramble the stream and the link dropped because of the
  // command that was only meant to diagnose.
  const grove_dono_t dono = tab5_grove_dono();
  printf(tr("Grove: %s\n"), grove_nome(tab5_grove_tipo()));
  printf(tr("  energia : %s\n"), ajustes_grove() ? tr("ligada") : tr("desligada"));
  printf(tr("  porta   : %s\n"), (dono == GROVE_DONO_NINGUEM) ? tr("livre") : grove_dono_nome(dono));
  if (sistema_grove_sondando()) { printf("%s", tr("  sondagem em andamento...\n")); }
  printf("%s", tr("\ntrocou o modulo do cabo? use 'grove trocar'\n"));
}
