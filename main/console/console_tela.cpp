// Console commands - TELA (SCREEN): frame measurement, tela, brilho, piscada, toque, tocar, abrir, ordem, ui.
// (Split from console.cpp; the COMANDOS table and the help remain there.)

#include "driver/ledc.h"
#include "tab5_bmp.hpp"
#include "tab5_hora.hpp"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "ajustes.h"
#include "tela_virtual.hpp"
#include "compositor.hpp"
#include "tab5_glifos.hpp"
#include "internal.hpp"   // sistema_trocar_relogio_painel
#include "tab5_barramento.hpp"
#include "tab5_tela.hpp"
#include "console/console_interno.hpp"

// ---------------------------------------------------------------------------
// 'tela medir' / 'tela' - THE COST OF THE FRAMES OVER A WINDOW
// ---------------------------------------------------------------------------
// The compositor only accumulates (comp_estat); the window is from here: 'tela medir' stores
// the current snapshot, and 'tela' shows the difference up to the moment of reading. The
// monitor has its own snapshot - neither clears the other's window.
static comp_estat_t s_medir_base = {};
static int64_t      s_medir_desde = 0;

static void tela_medir_marcar(void)
{
  comp_estat(&s_medir_base);
  s_medir_desde = esp_timer_get_time();
  printf("%s", tr("medida zerada - use o aparelho (role, abra apps) e rode 'tela'\n"));
}

static void tela_medir_mostrar(void)
{
  if (s_medir_desde == 0) { printf("%s", tr("\n  (rode 'tela medir' para abrir uma janela de medida)\n")); return; }
  comp_estat_t a;
  comp_estat(&a);
  const comp_estat_t& b = s_medir_base;
  const double seg = (double)(esp_timer_get_time() - s_medir_desde) / 1e6;
  const uint32_t q = a.quadros - b.quadros;
  // Each pixel copied by DMA is read (2 B) and written (2 B) in PSRAM. Hence
  // x4. "drawn" is what the CPU painted directly on the page (write only).
  const double mb_d  = (double)(a.px_quadro    - b.px_quadro)    * 2.0 / 1e6;
  const double mb_s  = (double)(a.px_sinc      - b.px_sinc)      * 4.0 / 1e6;
  const double mb_rd = (double)(a.px_rolar_dma - b.px_rolar_dma) * 4.0 / 1e6;
  auto ms_por = [&](uint64_t us) { return q ? (double)us / 1000.0 / q : 0.0; };
  printf(tr("\n--- quadros (ultimos %.1f s) ---\n"), seg);
  printf(tr("  quadros                   : %lu (%.1f q/s)  cheios(>=90%%): %lu  lista estourou: %lu\n"),
         (unsigned long)q, seg > 0 ? q / seg : 0.0,
         (unsigned long)(a.cheios - b.cheios), (unsigned long)(a.estouros - b.estouros));
  printf(tr("  desenhado direto na pagina: %.1f MB\n"), mb_d);
  printf(tr("  copia por DMA2D           : sincronia %.1f MB + rolagem %.1f MB = %.1f MB/s\n"),
         mb_s, mb_rd, seg > 0 ? (mb_s + mb_rd) / seg : 0.0);
  printf(tr("  rolagem (DMA ou CPU)      : %.2f ms/quadro\n"), ms_por(a.us_rolar - b.us_rolar));
  printf(tr("  por quadro (media)        : desenho %.2f ms, copia %.2f ms, espera da troca %.2f ms\n"),
         ms_por(a.us_desenho - b.us_desenho), ms_por(a.us_copia - b.us_copia),
         ms_por(a.us_espera - b.us_espera));
  {
    // The drawing split out: FULL pass (opening an app, home) and the rest
    // (scrolling, a value changed). Different problems, different remedies.
    const uint32_t nc = a.n_desenho_completo - b.n_desenho_completo;
    const uint32_t nt = a.n_desenho - b.n_desenho;
    const uint64_t uc = a.us_desenho_completo - b.us_desenho_completo;
    const uint64_t ut = a.us_desenho - b.us_desenho;
    const uint32_t np = nt - nc;
    printf(tr("  desenho por passada       : completa %.2f ms (%lu)  parcial %.2f ms (%lu)\n"),
           nc ? (double)uc / 1000.0 / nc : 0.0, (unsigned long)nc,
           np ? (double)(ut - uc) / 1000.0 / np : 0.0, (unsigned long)np);
  }
  {
    sistema_passada_t ps[24];
    const int n = sistema_passadas_completas(ps, 24);
    if (n > 0) {
      printf("%s", tr("  passada completa por app (desde o boot, ms medios):\n"));
      printf("    %-12s %5s %8s %8s %8s %8s %8s\n", tr("app"), tr("vezes"), tr("total"), tr("pior"), tr("barra"), tr("limpar"), tr("conteudo"));
      for (int i = 0; i < n; ++i) {
        const sistema_passada_t& p = ps[i];
        if (!p.n) { continue; }
        auto m = [&](uint64_t us) { return (double)us / 1000.0 / p.n; };
        printf("    %-12.12s %5lu %8.1f %8.1f %8.1f %8.1f %8.1f\n", p.nome ? p.nome : "?",
               (unsigned long)p.n, m(p.us), (double)p.max_us / 1000.0,
               m(p.us_barra), m(p.us_limpar), m(p.us_conteudo));
      }
    }
  }
  {
    glifos_estat_t g;
    tab5_glifos_estat(&g);
    const uint32_t tot = g.acertos + g.faltas + g.recusas;
    printf(tr("  cache de glifos (desde o boot): %lu%% da imagem guardada; %lu acertos, %lu faltas, %lu pelo caminho original\n"),
           (unsigned long)(tot ? (uint64_t)g.acertos * 100 / tot : 0),
           (unsigned long)g.acertos, (unsigned long)g.faltas, (unsigned long)g.recusas);
    printf(tr("                              %lu letras guardadas, %lu KB, recomecou %lu vez(es)\n"),
           (unsigned long)g.entradas, (unsigned long)(g.bytes / 1024), (unsigned long)g.limpezas);
    printf(tr("                              caminho original por motivo: espaco %lu, sem fundo %lu, invade %lu,\n"
           "                              destino %lu, escala %lu, outro %lu\n"),
           (unsigned long)g.rec_espaco, (unsigned long)g.rec_sem_fundo, (unsigned long)g.rec_invade,
           (unsigned long)g.rec_destino, (unsigned long)g.rec_escala, (unsigned long)g.rec_outro);
  }
  static const char* const FAIXA[COMP_FAIXAS] = { "1", "2", "3", "4+" };
  printf(tr("  volta com quadro (media)  : %.2f ms   em periodos do painel:"), ms_por(a.us_volta - b.us_volta));
  for (int f = 0; f < COMP_FAIXAS; ++f) {
    printf(" %s:%lu", FAIXA[f], (unsigned long)(a.voltas[f] - b.voltas[f]));
  }
  printf("\n");
}

// ---------------------------------------------------------------------------
// SCREEN
// ---------------------------------------------------------------------------

// BLUE FLICKER DIAGNOSTICS, and switches to compare without reflashing.
//
// RECOGNISED WORD OR NOTHING DONE - and this rule cost a measurement. An
// earlier version was "if it is not 'on', turn off": any second word that
// did not match became a SILENT priorizar_tela(false), and a measurement came out
// with the priority at "default" without anyone asking for it. A diagnostic command
// must not have a destructive path by default.
// BENCH: the screen as it is now, into a .bmp on the SD card (apps/Console).
// readRect returns RGB565 in LovyanGFX's order (bytes swapped); the filter
// undoes that in the copy that goes to the file.
static void bmp_desinverter(uint16_t* l, int n) { for (int i = 0; i < n; ++i) { l[i] = (uint16_t)((l[i] << 8) | (l[i] >> 8)); } }
static void tela_foto(const char* nome)
{
  const int w = M5.Display.width(), h = M5.Display.height();
  uint16_t* px = (uint16_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
  if (!px) { printf(tr("sem memoria para %dx%d\n"), w, h); return; }
  M5.Display.readRect(0, 0, w, h, px);
  const bool ok = tab5_bmp_salvar("Console", nome, px, w, h, bmp_desinverter);
  heap_caps_free(px);
  printf(ok ? tr("gravada: /sdcard/apps/Console/%s.bmp (%dx%d)\n") : tr("falhou ao gravar %s.bmp\n"), nome, w, h);
}

// BENCH: tap and open an app over the serial port (see sistema_simular_toque).
void c_tocar(int argc, char** argv)
{
  if (argc < 3) { mostrar_uso("tocar"); return; }
  sistema_simular_toque(atoi(argv[1]), atoi(argv[2]));
  printf(tr("toque em %s,%s\n"), argv[1], argv[2]);
}

bool nome_de_app(const char* nome, const char* s);   // further down

void c_abrir(int argc, char** argv)
{
  if (argc < 2) {
    for (int i = 0; i < SISTEMA_NUM_APPS; ++i) { printf("  %s\n", SISTEMA_APPS[i]->nome); }
    return;
  }
  if (eh(argv[1], "home")) { sistema_simular_abrir(nullptr); printf("%s", tr("voltando para a home\n")); return; }
  // The name may have a space ("abrir Camera Berry"): the words joined again.
  char nome[48] = "";
  for (int i = 1; i < argc; ++i) {
    if (i > 1) { strlcat(nome, " ", sizeof(nome)); }
    strlcat(nome, argv[i], sizeof(nome));
  }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (nome_de_app(SISTEMA_APPS[i]->nome, nome)) {
      sistema_simular_abrir(SISTEMA_APPS[i]);
      printf(tr("abrindo %s\n"), SISTEMA_APPS[i]->nome);
      return;
    }
  }
  printf(tr("nao ha app '%s' - 'abrir' lista\n"), nome);
}

// ORDER OF THE ICONS ON THE HOME SCREEN - the same order that press-and-drag changes on screen.
static const app_t* app_por_nome(const char* nome)
{
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->nome, nome) == 0) { return SISTEMA_APPS[i]; }
  }
  return nullptr;
}

void c_ordem(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "padrao")) {
    sistema_home_ordem_padrao();
    printf("%s", tr("ordem de fabrica (apps.cpp) restaurada\n"));
  } else if (argc >= 3) {
    const app_t* app = app_por_nome(argv[1]);
    const int pos = atoi(argv[2]);
    if (!app) { printf(tr("nao ha app '%s' - 'ordem' lista\n"), argv[1]); return; }
    if (pos < 1 || pos > SISTEMA_NUM_APPS) { printf(tr("posicao de 1 a %d\n"), SISTEMA_NUM_APPS); return; }
    sistema_home_mover(app, pos - 1);
    printf(tr("'%s' agora na posicao %d\n"), app->nome, pos);
  } else if (argc == 2) {
    mostrar_uso("ordem");
    return;
  }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const app_t* app = sistema_home_app(i);
    if (app) { printf("  %2d  %s\n", i + 1, app->nome); }
  }
}

void c_tela(int argc, char** argv)
{
  if (argc >= 2) {
    const char* o = argv[1];
    if (eh(o, "foto")) {
      // Without a name, a dated one ("tela-2026-11-23-1905"): a second shot no
      // longer overwrites the first.
      char nome[40] = "tela";
      const uint32_t e = tab5_hora_epoch();
      if (e) {
        struct tm t;
        tab5_hora_local(e, &t);
        snprintf(nome, sizeof(nome), "tela-%04d-%02d-%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
      }
      tela_foto(argc >= 3 ? argv[2] : nome);
      return;
    }
    if (eh(o, "medir")) { tela_medir_marcar(); return; }
    if (eh(o, "banco")) { tab5_quadro_banco(); return; }
    if (eh(o, "irqs"))  { tab5_quadro_listar_irqs(); return; }
    if (eh(o, "atrasar")) {
      // FLICKER PROOF: holds a core and the user watches the screen. Flickers =
      // the panel's delayed ISR is the cause; does not flicker = the cause is something else.
      if (argc < 3) {
        printf("%s", tr("uso: tela atrasar <ms> [critica] [nucleo 0|1] [vezes]\n"
                        "   (ex.: tela atrasar 10 - 5 vezes, interrupcoes desligadas no nucleo 0)\n"));
        return;
      }
      const int ms = atoi(argv[2]);
      int i = 3;
      const bool critica = argc > i && eh(argv[i], "critica");
      if (critica) { ++i; }
      const int nucleo = argc > i ? atoi(argv[i++]) : 0;
      const int vezes  = argc > i ? atoi(argv[i++]) : 5;
      const uint32_t antes_u = tab5_quadro_underruns();
      const uint32_t antes_a = tab5_quadro_atrasos(nullptr, nullptr);
      printf(tr("segurando o nucleo %d por %d ms (%s), %d vez(es) - olhe a tela\n"),
             nucleo, ms, critica ? tr("secao critica") : tr("interrupcoes desligadas"), vezes);
      if (!tab5_quadro_prova_atraso(ms, critica, nucleo, vezes)) { printf("%s", tr("argumento invalido (ms 1-100, nucleo 0-1, vezes 1-20)\n")); return; }
      printf(tr("pronto: underrun +%lu, atrasados +%lu\n"),
             (unsigned long)(tab5_quadro_underruns() - antes_u), (unsigned long)(tab5_quadro_atrasos(nullptr, nullptr) - antes_a));
      return;
    }
    if (eh(o, "rolar")) {
      // Scrolling by DMA (from the front page) or by the CPU (within the back
      // one). A trail that disappears with 'cpu' is on the DMA path.
      extern bool g_tela_rolar_cpu;
      if (argc >= 3 && eh(argv[2], "cpu"))      { g_tela_rolar_cpu = true; }
      else if (argc >= 3 && eh(argv[2], "dma")) { g_tela_rolar_cpu = false; }
      else { printf("%s", tr("uso: tela rolar cpu|dma\n")); }
      printf(tr("rolagem pela %s\n"), g_tela_rolar_cpu ? tr("CPU") : tr("DMA (da frente)"));
      return;
    }
    if (eh(o, "relogio")) {
      // CHANGES THE PIXEL CLOCK AT RUN TIME. It is the only adjustment that reduces the
      // panel's demand instead of competing better - see tab5_power.cpp.
      if (argc < 3) { printf("%s", tr("uso: tela relogio <mhz>   (ex.: tela relogio 60)\n")); return; }
      const int mhz = atoi(argv[2]);
      const bool ok = sistema_trocar_relogio_painel(mhz);   // on the UI task
      // The rebuilt panel is born with a WHITE framebuffer (
      // esp_lcd_new_panel_dpi allocates with calloc) - without the redraw the screen
      // stayed black with the backlight on. SAVES ONLY AFTER it is accepted: if it
      // saved first and the panel refused, the next boot would try the same
      // bad value again.
      if (ok) {
        sistema_pedir_redesenho_completo();
        ajustes_definir_relogio_painel((uint8_t)tab5_power_relogio_dpi());   // the REAL value applied
      }
      printf(ok ? tr("painel remontado a %d MHz e gravado - vale no proximo boot tambem\n")
                : tr("nao subiu nesse relogio - voltei ao anterior (%d MHz)\n"), tab5_power_relogio_dpi());
    }
    else if (eh(o, "link")) {
      // DSI LINK SPEED, for testing only: does NOT save (restarting goes back to
      // M5GFX's). To compare the edge shimmer between 1040 and 730 Mbps.
      if (argc < 3) { printf(tr("link DSI: %d Mbps por via\nuso: tela link <mbps>   (ex.: tela link 730)\n"), tab5_power_link_dsi()); return; }
      const bool ok = sistema_trocar_link_painel(atoi(argv[2]));
      if (ok) { sistema_pedir_redesenho_completo(); }
      printf(ok ? tr("painel remontado a %d Mbps (nao gravado: reiniciar volta)\n")
                : tr("nao subiu - voltei ao anterior (%d Mbps)\n"), tab5_power_link_dsi());
    }
    else if (eh(o, "prioridade") && argc >= 3 && (eh(argv[2], "alta") || eh(argv[2], "on"))) {
      tab5_barramento_priorizar_tela(true);
    }
    else if (eh(o, "prioridade") && argc >= 3 && (eh(argv[2], "padrao") || eh(argv[2], "off"))) {
      tab5_barramento_priorizar_tela(false);
    }
    // Old forms, kept: 'tela on' / 'tela off'.
    else if (eh(o, "on")  || eh(o, "sim")) { tab5_barramento_priorizar_tela(true); }
    else if (eh(o, "off") || eh(o, "nao")) { tab5_barramento_priorizar_tela(false); }
    else {
      printf(tr("nao conheco 'tela %s' - nada foi alterado.\n"), o);
      mostrar_uso("tela");
      return;
    }
  }
  printf("%s", tr("--- tela ---\n"));
  printf(tr("  prioridade da tela no AXI : %s\n"),
         tab5_barramento_priorizado() ? tr("ALTA") : tr("padrao (como vem de fabrica)"));
  printf(tr("  limpezas dispensadas      : %lu\n"),
         (unsigned long)tab5_tela_limpezas_evitadas());
  {
    // The detectors from hal/tab5_quadro. No 'pior' (worst): reading resets it, and whoever
    // tracks the worst is the monitor, which turns it into an event.
    uint32_t periodo_us = 0;
    const uint32_t atrasos = tab5_quadro_atrasos(nullptr, &periodo_us);
    const uint32_t erros   = tab5_quadro_erros_dsi(nullptr, nullptr);
    printf(tr("  quadros varridos          : %lu (periodo %lu.%lu ms)\n"),
           (unsigned long)tab5_quadro_quadros(),
           (unsigned long)(periodo_us / 1000), (unsigned long)(periodo_us % 1000 / 100));
    printf(tr("  underrun / atrasados / DSI: %lu / %lu / %lu\n"),
           (unsigned long)tab5_quadro_underruns(), (unsigned long)atrasos, (unsigned long)erros);
    int nucleo = -1; char t0[16], t1[16];
    if (tab5_quadro_atraso_quem(&nucleo, t0, t1)) {
      printf(tr("  ultimo atraso             : ISR no nucleo %d; rodando c0=%s c1=%s\n"), nucleo, t0, t1);
      printf(tr("  onde o nucleo estava      : 0x%08lx (addr2line no .elf)\n"), (unsigned long)tab5_quadro_atraso_pc());
    }
  }

  // THE SUM ON SCREEN, and not just in the source comment. These are the numbers that
  // say whether changing the clock was worth it: MB/s is what the panel pulls from PSRAM non-
  // stop, and that is the value everything else in the system competes against.
  const int mhz = tab5_power_relogio_dpi();
  printf(tr("  tela                      : %s%s\n"), tab5_painel_nome(),
         tab5_painel_toque_gt911() ? "" : tr(" (relogio fixo, repouso so de luz)"));
  if (mhz > 0) {
    const double fps  = tab5_power_quadros_por_segundo();
    printf(tr("  relogio do painel         : %d MHz\n"), mhz);
    printf(tr("  taxa de quadros           : %.1f Hz\n"), fps);
    printf(tr("  leitura continua da PSRAM : %.1f MB/s\n"), fps * 720.0 * 1280.0 * 2.0 / 1e6);
  }

  tela_medir_mostrar();
  printf("%s", tr("\n  underrun > 0 = a piscada azul. 'help tela' lista as medidas e ajustes.\n"));
}

// BACKLIGHT TEST, without saving: find the lowest level that still lights and see
// whether the PWM frequency changes the floor. An LED driver with a minimum on-time
// does not obey a small duty at high frequency (at 44.1 kHz the period is
// 22.7 us) - below a certain point the brightness stops falling.
void c_brilho(int argc, char** argv)
{
  if (argc < 2) {
    printf(tr("brilho: %d (gravado %d)\n"), (int)M5.Display.getBrightness(), (int)ajustes_brilho());
    return;
  }
  int b = atoi(argv[1]);
  if (b < 1)   { b = 1; }
  if (b > 255) { b = 255; }
  if (argc >= 3) {
    const uint32_t hz = (uint32_t)atoi(argv[2]);
    // M5GFX channel 7 on the Tab5 -> timer (7 >> 1) & 3 = 3, low-speed mode.
    const esp_err_t e = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3, hz);
    printf(tr("PWM do backlight: %lu Hz (%s)\n"), (unsigned long)hz, esp_err_to_name(e));
  }
  M5.Display.setBrightness((uint8_t)b);
  printf(tr("brilho %d = duty %.2f%% (nao gravado; Ajustes ou reiniciar voltam ao gravado)\n"),
         b, (float)(((b * 257 + 64) >> 7)) * 100.0f / 512.0f);
}

// Opening an app does several things at once; each option does ONLY ONE, noting the
// time, so you can watch the screen and see which one flickers.
void c_piscada(int argc, char** argv)
{
  const char* o = (argc >= 2) ? argv[1] : "";
  const unsigned long t = (unsigned long)(esp_timer_get_time() / 1000);
  if (eh(o, "brilho")) {
    printf(tr("piscada: %lums - reescrevendo o MESMO brilho (%d)\n"), t, (int)M5.Display.getBrightness());
    M5.Display.setBrightness(M5.Display.getBrightness());
  } else if (eh(o, "c6")) {
    printf(tr("piscada: %lums - reescrevendo WLAN_PWR_EN=0 no expansor\n"), t);
    tab5_power_radio(false);
  } else if (eh(o, "perfil")) {
    printf(tr("piscada: %lums - reaplicando o perfil inteiro (trilhos + brilho + pm)\n"), t);
    sistema_reaplicar_perfil();
  } else if (eh(o, "pm")) {
    esp_pm_config_t c = { };
    esp_pm_get_configuration(&c);
    printf(tr("piscada: %lums - esp_pm_configure(%d..%d MHz) de novo\n"), t, c.min_freq_mhz, c.max_freq_mhz);
    esp_pm_configure(&c);
  } else if (eh(o, "grove")) {
    printf(tr("piscada: %lums - 5V do Grove OFF, 1s, ON (o C6L reinicia)\n"), t);
    M5.Power.setExtOutput(false, m5::ext_PA);
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf(tr("piscada: %lums - 5V do Grove ON agora\n"), (unsigned long)(esp_timer_get_time() / 1000));
    M5.Power.setExtOutput(true, m5::ext_PA);
  } else if (eh(o, "quadro")) {
    printf(tr("piscada: %lums - redesenho completo pelo compositor\n"), t);
    tela_virtual_invalidar_tudo();
    sistema_pedir_redesenho_completo();
  } else {
    mostrar_uso("piscada");
  }
}

// Turns the diagnostics on/off. Does not read the panel from here: I2C belongs to the UI
// task, and two tasks on the same bus read the wrong register without
// raising an error. Here we only set the flag; whoever measures is whoever prints.
void c_toque(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "regs")) {
    sistema_pedir_regs_toque();
    printf(tr("raio-x do toque pedido - veja o log ('toque: regs')\n"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "reiniciar")) {
    sistema_pedir_reinicio_toque();
    printf(tr("reinicio do toque pedido - veja o log ('toque: GT911 reiniciado')\n"));
    return;
  }
  const bool ligar = !(argc >= 2 && eh(argv[1], "off"));
  sistema_diag_toque(ligar);
  printf(ligar ? tr("toque cada botao e veja onde o dedo cai.\n"
                 "compare o 'app (x,y)' com a posicao esperada do botao.\n"
                 "para desligar: toque off\n")
               : tr("diagnostico de toque desligado\n"));
}

// Where the UI is NOW - works with the screen frozen: the console is
// another task. Run it twice: 'voltas' unchanged = loop stuck at that step.
void c_ui(int, char**)
{
  sistema_diag_t d;
  sistema_diag_interface(&d);
  printf(tr("interface: %s\n  voltas %lu, ultima ha %lu ms, %s%s\n  dedo lido ha %ld ms\n"),
         d.etapa, (unsigned long)d.voltas, (unsigned long)d.ms_desde_volta,
         d.app ? d.app : tr("home"), d.repouso ? tr(" (repouso)") : "",
         d.ms_desde_dedo == UINT32_MAX ? -1L : (long)d.ms_desde_dedo);
  printf(tr("toque: %lu pulsos, %lu falhas de I2C, %lu presos, %lu s surdos, status GT911 %02X\n"),
         (unsigned long)d.toque_pulsos, (unsigned long)d.toque_falhas, (unsigned long)d.toque_presos,
         (unsigned long)d.toque_surdos, (unsigned)d.toque_status);
  printf(tr("toque: GT911 reiniciado %lu vez(es) (mudo: pulsava sem relatorio)\n"),
         (unsigned long)d.toque_reinicios);
}
