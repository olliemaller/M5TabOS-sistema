// APP: Exemplo (Example) - one card for each system API.
//
// It is the showcase and the template: each row of the table uses ONE system
// API (docs/API.md) the right way, with the minimum of code. To write a new
// app, copy this folder to main/app_<name>/, rename APP_EXEMPLO to
// APP_<NAME> and delete the cards you do not need.
//
// WHAT THIS FILE SHOWS, BESIDES EACH CARD:
//   - the golden rule: only the UI task draws. The app's task
//     (TAREFA card), the worker (TRABALHO), the background part (FUNDO) and the
//     console (CONSOLE) only change DATA and request a redraw;
//   - life cycle: everything ao_abrir() creates, ao_fechar() destroys;
//   - the system scrollable list (kernel/lista): each card is only repainted
//     when its value changes;
//   - app console commands: 'app exemplo contar 5'.
//   - the THEME: every UI colour comes from TEMA_* (tab5_tema.hpp), by ROLE
//     (background, text, accent...) and not by number; and the app turns on
//     '.fundo_tema = true' so the system clears the area with TEMA_FUNDO. That way
//     changing the system palette does not require touching any app.

#include "app.h"
#include "idioma.h"
#include "lista.hpp"
#include "tarefa_app.h"
#include "trabalho.hpp"
#include "fundo.h"
#include "notificacao.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"   // colours by role: use TEMA_*, never 0x.... nor TFT_*
#include "tab5_sd.hpp"
#include "tab5_hora.hpp"
#include "tab5_imu.hpp"
#include "tab5_gps.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "tab5_teclado.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include "tab5_arquivos.hpp"   // listing a folder belongs to the system
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
// Everything another task writes and the UI reads is atomic (a number) or
// goes through a queue. Text from another task is never read "half-way".

static lista_t s_lista;

// TAREFA (task): a counter the app's task produces and delivers via a queue of 1.
static tarefa_app_t  s_tarefa = { };
static QueueHandle_t s_fila   = nullptr;
static uint32_t      s_tarefa_n = 0;

// TOQUE (touch) and TECLADO (keyboard).
static int  s_toques = 0, s_toque_x = -1, s_toque_y = -1;
static char s_tecla[24] = "";

// TRABALHO (work): the worker writes the result, the UI only reads it in ao_aviso.
static std::atomic<int> s_trab_arquivos{-1};   // -1 = never ran
static bool s_trab_rodando = false;

// CARTAO (SD card).
static int s_linhas_gravadas = 0;

// NOTIFICACAO (notification).
static int  s_notif = -1;
static bool s_notif_ativa = false;

// ACORDADO (keep awake).
static bool s_acordado = false;

// CONSOLE: the 'app exemplo contar N' command adds here (another task).
static std::atomic<int> s_console_soma{0};

// FUNDO (background): a stopwatch that keeps going with the app closed.
static std::atomic<uint32_t> s_fundo_s{0};

// ---------------------------------------------------------------------------
// THE CARDS
// ---------------------------------------------------------------------------
// Each one: title, the API it uses, how to write the current value, and an
// optional button. The table below is the whole screen.
typedef struct {
  const char* titulo;
  const char* api;
  void (*valor)(char* out, size_t n);
  const char* botao;            // nullptr = no button
  void (*ao_botao)(void);
} cartao_t;

#define VALOR_MAX 64

// ---- SCREEN -----------------------------------------------------------------
static void v_tela(char* o, size_t n)
{
  const area_t a = sistema_area();
  snprintf(o, n, tr("area do app: %d x %d"), a.largura, a.altura);
}

// ---- TOUCH ------------------------------------------------------------------
static void v_toque(char* o, size_t n)
{
  if (s_toques == 0) { snprintf(o, n, tr("toque em qualquer cartao")); return; }
  snprintf(o, n, tr("%d toque(s), ultimo em %d,%d"), s_toques, s_toque_x, s_toque_y);
}

// ---- TASK -------------------------------------------------------------------
// The app's task does NOT draw: it produces, delivers via the queue and requests a redraw.
static void laco_tarefa(tarefa_app_t* t)
{
  uint32_t n = 0;
  while (tarefa_app_ativa(t)) {
    ++n;
    xQueueOverwrite(s_fila, &n);       // queue of 1: only the most recent matters
    sistema_pedir_redesenho();
    vTaskDelay(pdMS_TO_TICKS(1000));   // tarefa_app_encerrar wakes it from here
  }
}

static void v_tarefa(char* o, size_t n)
{
  uint32_t v;
  if (s_fila && xQueueReceive(s_fila, &v, 0) == pdTRUE) { s_tarefa_n = v; }
  snprintf(o, n, tr("a tarefa contou %lu"), (unsigned long)s_tarefa_n);
}

// ---- WORK (system worker) ---------------------------------------------------
// Runs on the worker task, never on the UI: it may read the SD card freely.
// When it finishes the app receives AVISO_TRABALHO (declared in .avisos).
// Listing a folder belongs to the system (services/tab5_arquivos): the app does not
// open a directory itself - it only says what to do with each item.
static bool visita_contar(const tab5_arq_item_t*, void*) { return !sistema_trabalho_cancelado(); }

static void trab_contar(void*)
{
  int n = 0;
  char pasta[96];
  if (tab5_sd_montado() && tab5_sd_pasta("Exemplo", pasta, sizeof(pasta))) {
    n = tab5_arq_percorrer(pasta, visita_contar, nullptr);
  }
  s_trab_arquivos = n < 0 ? 0 : n;
}

static void v_trabalho(char* o, size_t n)
{
  const int a = s_trab_arquivos;
  if (s_trab_rodando) { snprintf(o, n, tr("contando...")); }
  else if (a < 0)     { snprintf(o, n, tr("conta os arquivos da pasta do app")); }
  else                { snprintf(o, n, tr("%d arquivo(s) em apps/Exemplo"), a); }
}

static void b_trabalho(void)
{
  if (!s_trab_rodando && sistema_trabalho(trab_contar, nullptr)) { s_trab_rodando = true; }
}

// ---- SD CARD ----------------------------------------------------------------
// tab5_sd_anexar does not wait: the system writes in batches, and keeps a journal if
// the card is lent to the computer.
static void v_cartao(char* o, size_t n)
{
  snprintf(o, n, s_linhas_gravadas ? tr("%d linha(s) em apps/Exemplo/diario.csv")
                                   : tr("grava uma linha no CSV do app"), s_linhas_gravadas);
}

static void b_cartao(void)
{
  char linha[48];
  snprintf(linha, sizeof(linha), "%lu,%d\n", (unsigned long)tab5_hora_epoch(), s_linhas_gravadas + 1);
  if (tab5_sd_anexar("Exemplo", "diario.csv", linha, "epoch,n\n")) { ++s_linhas_gravadas; }
}

// ---- TIME -------------------------------------------------------------------
static void v_hora(char* o, size_t n)
{
  const uint32_t e = tab5_hora_epoch();
  if (!e) { snprintf(o, n, tr("relogio ainda sem acerto")); return; }
  struct tm t;
  tab5_hora_local(e, &t);
  snprintf(o, n, "%02d/%02d/%04d %02d:%02d:%02d (%s)", t.tm_mday, t.tm_mon + 1,
           t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec, tab5_hora_fonte());
}

// ---- POWER ------------------------------------------------------------------
// tab5_power_ultima(): the reading the UI already took, without touching I2C.
static void v_energia(char* o, size_t n)
{
  const tab5_medida_t m = tab5_power_ultima();
  if (m.pct >= 0) { snprintf(o, n, "%d%%  %.2f V  %+.0f mA", m.pct, (double)m.volts, (double)m.mA); }
  else            { snprintf(o, n, "%.2f V  %+.0f mA", (double)m.volts, (double)m.mA); }
}

// ---- IMU --------------------------------------------------------------------
// Requires .perfil.imu = true (the system only turns the IMU on for those who ask).
static void v_imu(char* o, size_t n)
{
  tab5_imu_dados_t d;
  if (!tab5_imu_ultima(&d) || !d.valida) { snprintf(o, n, tr("IMU sem leitura")); return; }
  snprintf(o, n, "x %+.2f g  y %+.2f g  z %+.2f g", (double)d.ax, (double)d.ay, (double)d.az);
}

// ---- GPS --------------------------------------------------------------------
static void v_gps(char* o, size_t n)
{
  if (!tab5_gps_no_ar()) { snprintf(o, n, tr("sem GPS no Grove")); return; }
  const nmea_posicao_t p = tab5_gps_posicao();
  if (p.qualidade == 0) { snprintf(o, n, tr("GPS sem fix (%u sat)"), (unsigned)p.sats_usados); return; }
  snprintf(o, n, "%.5f, %.5f  (%u sat)", p.lat, p.lon, (unsigned)p.sats_usados);
}

// ---- WIFI -------------------------------------------------------------------
static void v_wifi(char* o, size_t n)
{
  const char* ip = tab5_wifi_ip();
  if (ip && ip[0]) { snprintf(o, n, "%s  %s  %d dBm", tab5_wifi_ssid(), ip, (int)tab5_wifi_rssi()); }
  else             { snprintf(o, n, "%s", tr(tab5_wifi_descricao())); }
}

// ---- ESP-NOW ----------------------------------------------------------------
static void v_espnow(char* o, size_t n)
{
  if (!tab5_espnow_pronto()) { snprintf(o, n, tr("fora do ar (radio C6 de fabrica ou Wi-Fi desligado)")); return; }
  const int k = tab5_espnow_n();
  espnow_msg_t m;
  if (k > 0 && tab5_espnow_msg(k - 1, &m)) { snprintf(o, n, tr("canal %u, ultima: %.28s"), tab5_espnow_canal(), m.texto); }
  else                                     { snprintf(o, n, tr("canal %u, nada recebido"), tab5_espnow_canal()); }
}

static void b_espnow(void) { tab5_espnow_enviar("ola do Exemplo"); }

// ---- MESH -------------------------------------------------------------------
// Read only: an example app should not go chatting on a public channel. To
// send: tab5_mesh_enviar(0xFFFFFFFF, canal, texto) or to a node.
static void v_mesh(char* o, size_t n)
{
  if (!tab5_mesh_servico_ativo()) { snprintf(o, n, tr("servico desligado (Ajustes > Mesh)")); return; }
  snprintf(o, n, tr("%s, %d no(s), %d nao lida(s)"), tab5_mesh_no_ar() ? tr("no ar") : tr("mudo"),
           tab5_mesh_num_nos(), tab5_mesh_nao_lidas());
}

// ---- NOTIFICATION -----------------------------------------------------------
// State, not event: publishing again REPLACES. Quantity 0 clears it.
static void v_notif(char* o, size_t n)
{
  snprintf(o, n, s_notif_ativa ? tr("aviso na barra - toque para tirar") : tr("poe um aviso na barra de status"));
}

static void b_notif(void)
{
  s_notif_ativa = !s_notif_ativa;
  if (s_notif_ativa) { notif_publicar(s_notif, 1, NOTIF_ATENCAO, tr("aviso do Exemplo")); }
  else               { notif_limpar(s_notif); }
}

// ---- SOUND ------------------------------------------------------------------
// Requires .perfil.alto_falante = true. Always through sistema_beep (the cold codec
// would swallow a direct tone()).
static void v_som(char* o, size_t n) { snprintf(o, n, "sistema_beep(880 Hz, 120 ms)"); }
static void b_som(void) { sistema_beep(880.0f, 120); }

// ---- KEYBOARD ---------------------------------------------------------------
static void v_teclado(char* o, size_t n)
{
  if (!tab5_teclado_presente()) { snprintf(o, n, tr("encaixe o teclado no ExtPort1")); return; }
  snprintf(o, n, s_tecla[0] ? tr("ultima tecla: %s") : tr("digite algo"), s_tecla);
}

// ---- KEEP AWAKE -------------------------------------------------------------
static void v_acordado(char* o, size_t n)
{
  snprintf(o, n, s_acordado ? tr("tela nao apaga enquanto o app estiver aberto")
                            : tr("a tela apaga sozinha (padrao)"));
}

static void b_acordado(void)
{
  s_acordado = !s_acordado;
  sistema_manter_acordado(SISTEMA_ACORDADO_APP, s_acordado);   // released automatically on close
}

// ---- BACKGROUND -------------------------------------------------------------
// The loop runs on a KERNEL task and keeps going with the app closed; while it
// runs, the status bar shows a badge with the summary. It draws nothing.
static void laco_fundo(fundo_t* f)
{
  s_fundo_s = 0;
  while (fundo_esperar(f, 1000)) {
    const uint32_t s = ++s_fundo_s;
    char r[NOTIF_RESUMO];
    snprintf(r, sizeof(r), "%lu s", (unsigned long)s);
    fundo_estado(f, (int)s, r);
    sistema_pedir_redesenho();         // only has an effect with the window open
  }
}

static bool fundo_sem_som(void) { return false; }

static const fundo_def_t EXEMPLO_FUNDO = {
  .nome  = "Exemplo",
  .corpo = laco_fundo,
  .pilha = 3072,
  .perfil = { },
  .ao_iniciar = nullptr,
  .ao_parar   = nullptr,
  .com_som    = fundo_sem_som,
};

static void v_fundo(char* o, size_t n)
{
  if (sistema_fundo_ativo(&EXEMPLO_FUNDO)) {
    snprintf(o, n, tr("cronometro: %lu s (feche o app e veja a barra)"), (unsigned long)s_fundo_s.load());
  } else {
    snprintf(o, n, tr("um cronometro que sobrevive ao app fechado"));
  }
}

static void b_fundo(void)
{
  if (sistema_fundo_ativo(&EXEMPLO_FUNDO)) { sistema_fundo_parar(&EXEMPLO_FUNDO); }
  else                                     { sistema_fundo_iniciar(&EXEMPLO_FUNDO); }
}

// ---- CONSOLE ----------------------------------------------------------------
static void v_console(char* o, size_t n)
{
  snprintf(o, n, tr("'app exemplo contar 5' na serial: soma = %d"), s_console_soma.load());
}

static const cartao_t CARTOES[] = {
  { "TELA",        "sistema_area, tab5_ui, pintura",       v_tela,     nullptr,   nullptr },
  { "TOQUE",       "ao_tocar(x, y) - na soltura",           v_toque,    nullptr,   nullptr },
  { "TAREFA",      "tarefa_app + fila",                     v_tarefa,   nullptr,   nullptr },
  { "TRABALHO",    "sistema_trabalho + AVISO_TRABALHO",     v_trabalho, "contar",  b_trabalho },
  { "CARTAO",      "tab5_sd_anexar",                        v_cartao,   "gravar",  b_cartao },
  { "HORA",        "tab5_hora_epoch + tab5_hora_local",     v_hora,     nullptr,   nullptr },
  { "ENERGIA",     "tab5_power_ultima",                     v_energia,  nullptr,   nullptr },
  { "IMU",         "tab5_imu_ultima (perfil.imu)",          v_imu,      nullptr,   nullptr },
  { "GPS",         "tab5_gps_posicao",                      v_gps,      nullptr,   nullptr },
  { "WIFI",        "tab5_wifi_ssid / _ip / _rssi",          v_wifi,     nullptr,   nullptr },
  { "ESP-NOW",     "tab5_espnow_enviar + AVISO_ESPNOW",     v_espnow,   "difundir", b_espnow },
  { "MESH",        "tab5_mesh_* + AVISO_MESH_*",            v_mesh,     nullptr,   nullptr },
  { "NOTIFICACAO", "notif_publicar / notif_limpar",         v_notif,    "avisar",  b_notif },
  { "SOM",         "sistema_beep (perfil.alto_falante)",    v_som,      "bip",     b_som },
  { "TECLADO",     "ao_teclar(texto, codigo, mod)",         v_teclado,  nullptr,   nullptr },
  { "ACORDADO",    "sistema_manter_acordado",               v_acordado, "trocar",  b_acordado },
  { "FUNDO",       "fundo.h: sistema_fundo_iniciar",        v_fundo,    "ligar",   b_fundo },
  { "CONSOLE",     "app_t.comandos",                        v_console,  nullptr,   nullptr },
};
#define N_CARTOES ((int)(sizeof(CARTOES) / sizeof(CARTOES[0])))

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
static const int CARTAO_A = 132;          // height of each card
static const int BOTAO_L = 170, BOTAO_A = 64;
static const int MARGEM = 16;

static char s_valor[N_CARTOES][VALOR_MAX];   // what is on screen now

static int cartao_altura(int, void*) { return CARTAO_A; }

// The list reserves 12 px on the right for the scroll bar: the width the
// card gets is this (the SAME sum in drawing and in touch).
static int largura_cartao(const area_t& a) { return a.largura - 12; }

// Where a card's button sits (x relative to the window, y relative to the card).
static int botao_x(int largura) { return largura - MARGEM - BOTAO_L - 8; }
static int botao_y(void)        { return (CARTAO_A - BOTAO_A) / 2; }

static void cartao_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void*)
{
  const cartao_t& c = CARTOES[i];
  const lgfx::TextStyle estilo = d.getTextStyle();   // copy: we restore it at the end
  // (the list has already painted the card background - do not clear again)
  d.drawFastHLine(x + MARGEM, y + CARTAO_A - 1, largura - 2 * MARGEM, TEMA_LINHA);   // divider: the "line" role

  d.setTextDatum(top_left);
  tab5_fonte_corpo_negrito(d);
  tab5_texto(d, 3.0f);
  // Text: always colour + the colour of the BACKGROUND it lands on (the list paints TEMA_FUNDO),
  // otherwise the new text leaves a box of another colour around it.
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(c.titulo, x + MARGEM, y + 14);

  tab5_fonte_corpo(d);
  tab5_texto(d, 2.2f);
  d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);   // hint: the most faded level
  d.drawString(c.api, x + MARGEM, y + 52);

  // The value: cut with "..." if it does not fit before the button.
  char v[VALOR_MAX];
  snprintf(v, sizeof(v), "%s", s_valor[i]);
  tab5_texto(d, 2.6f);
  const int lim = (c.botao ? botao_x(largura) : largura - MARGEM) - MARGEM - 8;
  tab5_cortar_com_reticencias(d, v, sizeof(v), lim);
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);   // supporting value: secondary
  d.drawString(v, x + MARGEM, y + 84);

  if (c.botao) {
    tab5_botao(d, x + botao_x(largura), y + botao_y(), BOTAO_L, BOTAO_A, c.botao, TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE);   // primary button: the only strong colour
  }
  d.setTextStyle(estilo);
}

// Recalculates each card's value and marks ONLY the ones that changed for repainting.
static void atualizar_valores(bool todos)
{
  for (int i = 0; i < N_CARTOES; ++i) {
    char v[VALOR_MAX];
    CARTOES[i].valor(v, sizeof(v));
    if (todos || strcmp(v, s_valor[i]) != 0) {
      memcpy(s_valor[i], v, sizeof(v));
      if (!todos) { lista_item_mudou(&s_lista, i); }
    }
  }
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const bool tudo = sistema_area_foi_limpa();
  atualizar_valores(tudo);
  lista_desenhar(&s_lista, d, a, TEMA_FUNDO, tudo);   // the same background .fundo_tema clears
}

// ---------------------------------------------------------------------------
// TOUCH, KEYBOARD, NOTICES
// ---------------------------------------------------------------------------
static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  int dy = 0;
  const int i = lista_item_tocado(&s_lista, x, y, &dy);
  if (i < 0) { return; }                        // outside the list, or end of a drag
  ++s_toques; s_toque_x = x; s_toque_y = y;
  const cartao_t& c = CARTOES[i];
  const area_t a = sistema_area();
  if (c.botao && tab5_dentro(x, dy, botao_x(largura_cartao(a)), botao_y(), BOTAO_L, BOTAO_A)) {
    c.ao_botao();
  }
  sistema_pedir_redesenho();
}

static void ao_teclar(const char* texto, uint8_t codigo, uint8_t)
{
  if (texto[0]) { snprintf(s_tecla, sizeof(s_tecla), "\"%s\"", texto); }
  else          { snprintf(s_tecla, sizeof(s_tecla), tr("codigo %u"), (unsigned)codigo); }
}

// The services notify (AVISO_*) and the app redraws at once, without polling
// every second. AVISO_TRABALHO is the end of sistema_trabalho().
static void ao_aviso(uint32_t avisos)
{
  if (avisos & AVISO_TRABALHO) { s_trab_rodando = false; }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// LIFE CYCLE
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  s_fila = xQueueCreate(1, sizeof(uint32_t));
  if (!s_fila) { return false; }
  if (!tarefa_app_criar(&s_tarefa, laco_tarefa, "exemplo", 3072, 2)) {
    vQueueDelete(s_fila);
    s_fila = nullptr;
    return false;
  }
  s_notif = notif_registrar("Exemplo");    // idempotent: the same slot on every open
  notif_associar_app(s_notif, "Exemplo");  // tapping the notification opens this app
  tab5_espnow_janela(true);                // ESP-NOW receiver on while the app is on screen
  lista_definir(&s_lista, ROLAGEM_INICIO, N_CARTOES, cartao_altura, cartao_desenhar, nullptr);
  return true;
}

static void ao_fechar(void)
{
  // The task EXITS before its queue is destroyed: encerrar waits.
  tarefa_app_encerrar(&s_tarefa, 200);
  if (s_fila) { vQueueDelete(s_fila); s_fila = nullptr; }
  tab5_espnow_janela(false);
  // The notification and the background part continue on purpose: they outlive the window.
  s_acordado = false;                      // the system has already released the request
  s_trab_rodando = false;                  // pending work was discarded
}

// ---------------------------------------------------------------------------
// CONSOLE: 'app exemplo contar [n]' and 'app exemplo zerar'
// ---------------------------------------------------------------------------
// They run on the console task: they only touch atomic data and request a redraw.
static void cmd_contar(int argc, char** argv)
{
  const int n = argc >= 2 ? atoi(argv[1]) : 1;
  printf("soma = %d\n", s_console_soma += n);
  sistema_pedir_redesenho();
}

static void cmd_zerar(int, char**)
{
  s_console_soma = 0;
  printf("soma zerada\n");
  sistema_pedir_redesenho();
}

static void cmd_fundo(int, char**)
{
  b_fundo();
  printf("cronometro em fundo: %s\n", sistema_fundo_ativo(&EXEMPLO_FUNDO) ? "ligado" : "desligado");
}

static const app_comando_t COMANDOS[] = {
  { "contar", "[n]\tsoma n (padrao 1) no cartao CONSOLE", cmd_contar, false },
  { "fundo",  "\tliga/desliga o cronometro em fundo (o cartao FUNDO)", cmd_fundo, false },
  { "zerar",  "\tzera a soma (so com o app aberto)",      cmd_zerar,  true },
  { nullptr },
};

// ---------------------------------------------------------------------------
// ICON: dashed square with a "+" - an app template.
// ---------------------------------------------------------------------------
static void icone_exemplo(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t traco = TFT_CYAN;
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(20, 50, 55));
  const int m = (int)(0.24f * lado), L = lado - 2 * m, passo = L / 7, e = lado / 40;
  for (int i = 0; i < 7; i += 2) {
    d.fillRect(x + m + i * passo, y + m,             passo, e, traco);
    d.fillRect(x + m + i * passo, y + m + L,         passo, e, traco);
    d.fillRect(x + m,             y + m + i * passo, e, passo, traco);
    d.fillRect(x + m + L,         y + m + i * passo, e, passo, traco);
  }
  const int g = lado / 28;
  d.fillRect(x + lado / 2 - L / 6, y + lado / 2 - g, L / 3, 2 * g, traco);
  d.fillRect(x + lado / 2 - g, y + lado / 2 - L / 6, 2 * g, L / 3, traco);
}

// 'extern' is MANDATORY: in C++ a file-scope const has internal linkage, and
// apps.cpp would not see the symbol (the error only shows up at link time).
extern const app_t APP_EXEMPLO = {
  .nome  = "Exemplo",
  .cor   = TFT_CYAN,
  // Only what the cards use. The rest stays off while the app is on
  // screen - that is what stops the device powering what nobody uses.
  .perfil = { .imu = true, .alto_falante = true },
  .usa_sd = true,               // mounts the card before ao_abrir (TRABALHO reads the folder)
  .orientacao = ORI_EM_PE,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_ms = 500,            // time, power and IMU change by themselves
  .avisos = AVISO_TRABALHO | AVISO_ESPNOW | AVISO_MESH_MSG | AVISO_MESH_NOS | AVISO_GPS,
  .ao_aviso = ao_aviso,
  .icone = icone_exemplo,
  .comandos = COMANDOS,
  .fundo_tema = true,           // the system clears the area with TEMA_FUNDO (use TEMA_FUNDO for text)
};
