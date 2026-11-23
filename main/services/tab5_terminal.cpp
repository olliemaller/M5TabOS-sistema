// TERMINAL - see tab5_terminal.hpp.
#include "idioma.h"
#include "tab5_terminal.hpp"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "console.hpp"
#include "fundo.h"
#include "recursos.h"

static const char* TAG = "terminal";

// ---------------------------------------------------------------------------
// STATE. All under s_mx (a single small lock: the copies are one line each).
// ---------------------------------------------------------------------------
typedef struct { uint8_t n; uint8_t tipo; char t[TERM_LINHA_MAX]; } linha_t;   // 128 B

static SemaphoreHandle_t s_mx  = nullptr;
static linha_t*          s_lin = nullptr;     // PSRAM, TERM_LINHAS, a ring
static int               s_ini = 0, s_qtd = 0;
static bool              s_aberta = false;    // the last line is still receiving text (no '\n')
static uint32_t          s_total = 0;         // lines created since boot (the absolute numbering)
static std::atomic<uint32_t> s_versao{0};

#define FILA_N 4
#define HIST_N 16
EXT_RAM_BSS_ATTR static char s_fila[FILA_N][TERM_ENTRADA];
EXT_RAM_BSS_ATTR static char s_hist[HIST_N][TERM_ENTRADA];
static int  s_fila_ini = 0, s_fila_n = 0;
static int  s_hist_ini = 0, s_hist_n = 0;
static bool s_rodando = false;                // the background body is in its loop

static std::atomic<bool> s_parar{false};
static fundo_t*          s_f = nullptr;       // the background part running now (only the body writes)
static FILE*             s_saida = nullptr;   // the commands task's stdout

static bool interrompido(void) { return s_parar.load() || (s_f && !fundo_rodando(s_f)); }
EXT_RAM_BSS_ATTR static console_sessao_t s_sessao;

// THE COMMAND THAT READS (console_ler_linha): while 's_lendo', the typed line
// belongs to it - it goes into the 's_resp' box instead of becoming a new queued command.
static SemaphoreHandle_t s_resp_sem = nullptr;
static bool  s_lendo = false, s_oculto = false, s_tem_resp = false;
EXT_RAM_BSS_ATTR static char s_pergunta[80];
EXT_RAM_BSS_ATTR static char s_resp[TERM_ENTRADA];

static void travar(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void destravar(void) { xSemaphoreGive(s_mx); }

// ---------------------------------------------------------------------------
// THE LINES
// ---------------------------------------------------------------------------
static linha_t* nova_linha(term_tipo_t tipo)
{
  int i;
  if (s_qtd < TERM_LINHAS) { i = (s_ini + s_qtd++) % TERM_LINHAS; }
  else                     { i = s_ini; s_ini = (s_ini + 1) % TERM_LINHAS; }   // the oldest goes
  s_lin[i].n = 0;
  s_lin[i].tipo = (uint8_t)tipo;
  s_aberta = true;
  ++s_total;
  return &s_lin[i];
}

static linha_t* ultima(void) { return &s_lin[(s_ini + s_qtd - 1) % TERM_LINHAS]; }

// One byte of what the command wrote. Understands '\n', '\t', the ESC[2J of 'clear'
// and skips the rest of the ANSI sequences (colour, cursor) - nowhere to use them here.
static uint8_t s_esc = 0;            // 0 = text, 1 = saw ESC, 2 = inside ESC[...
static char    s_esc_par[8];
static uint8_t s_esc_n = 0;

static void limpar_trancado(void) { s_ini = s_qtd = 0; s_aberta = false; }

static void por_byte(uint8_t c, term_tipo_t tipo)
{
  if (s_esc == 1) { s_esc = (c == '[') ? 2 : 0; s_esc_n = 0; return; }
  if (s_esc == 2) {
    if (c >= 0x40 && c <= 0x7E) {   // the sequence's final letter
      s_esc = 0;
      if (c == 'J' && s_esc_n == 1 && s_esc_par[0] == '2') { limpar_trancado(); }
    } else if (s_esc_n < sizeof(s_esc_par)) {
      s_esc_par[s_esc_n++] = (char)c;
    }
    return;
  }
  if (c == 0x1B) { s_esc = 1; return; }
  if (c == '\n') {
    if (!s_aberta) { nova_linha(tipo); }   // an empty line is still a line
    s_aberta = false;
    return;
  }
  if (c == '\t') {
    for (int k = 0; k < 8; ++k) {
      por_byte(' ', tipo);
      if (s_aberta && ultima()->n % 8 == 0) { break; }
    }
    return;
  }
  if (c < 0x20 || c == 0x7F) { return; }
  linha_t* l = s_aberta ? ultima() : nova_linha(tipo);
  // Full: continue on the next, without cutting a UTF-8 character in the middle (the
  // continuation byte, 10xxxxxx, still fits in the 4-byte slack).
  const bool comeco = c < 0x80 || c >= 0xC0;
  if (l->n >= TERM_LINHA_MAX || (comeco && l->n >= TERM_LINHA_MAX - 4)) { l = nova_linha(tipo); }
  l->t[l->n++] = (char)c;
}

static void escrever(const char* b, int n, term_tipo_t tipo)
{
  if (!s_lin || n <= 0) { return; }
  travar();
  for (int i = 0; i < n; ++i) { por_byte((uint8_t)b[i], tipo); }
  destravar();
  ++s_versao;
}

static void aviso(const char* t) { escrever(t, (int)strlen(t), TERM_AVISO); }

// The commands task's stdout.
static int ao_escrever(void*, const char* b, int n) { escrever(b, n, TERM_SAIDA); return n; }

// ---------------------------------------------------------------------------
// THE BACKGROUND PART: takes lines from the queue and runs them until the queue runs dry
// ---------------------------------------------------------------------------
static bool tirar(char* linha)
{
  travar();
  const bool tem = s_fila_n > 0;
  if (tem) {
    memcpy(linha, s_fila[s_fila_ini], TERM_ENTRADA);
    s_fila_ini = (s_fila_ini + 1) % FILA_N;
    --s_fila_n;
  }
  destravar();
  return tem;
}

static void corpo(fundo_t* f)
{
  s_f = f;
  FILE* const antes_out = stdout;
  FILE* const antes_err = stderr;
  stdout = s_saida;
  stderr = s_saida;
  char linha[TERM_ENTRADA];
  // A command running with the screen off: the TLS handshake (an HTTPS)
  // overruns the timeout at 40 MHz - the PROCESSING policy holds 200.
  sistema_recurso_usar(RECURSO_PROCESSAMENTO, "Terminal");
  // Queue dry, exit straight away: the pool slot goes back to sleep, and the next
  // command starts another background part (cheap - the task already exists in the pool). The
  // 'sobrou' is checked under the lock, together with s_rodando = false: a line
  // arriving at that instant is either seen here, or starts a new background part.
  for (;;) {
    if (!tirar(linha)) {
      travar();
      const bool sobrou = s_fila_n > 0;
      if (!sobrou) { s_rodando = false; }
      destravar();
      if (!sobrou) { break; }
      continue;
    }
    s_parar = false;
    fundo_estado(f, 1, linha);   // the pill's summary, if it appears
    console_executar(&s_sessao, linha);
    travar();
    s_lendo = s_tem_resp = false;
    s_pergunta[0] = 0;
    memset(s_resp, 0, sizeof(s_resp));
    destravar();
    ++s_versao;
    if (s_parar.exchange(false)) { aviso("^C\n"); }
  }
  sistema_recurso_soltar(RECURSO_PROCESSAMENTO, "Terminal");
  fflush(stdout);
  stdout = antes_out;
  stderr = antes_err;
  s_f = nullptr;
}

// The Terminal session asks via the screen's bar (console_ler_linha).
static int ler_linha(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms)
{
  travar();
  const bool mudou = !s_lendo || s_oculto != oculto || strcmp(s_pergunta, pergunta ? pergunta : "") != 0;
  s_lendo = true;
  s_oculto = oculto;
  snprintf(s_pergunta, sizeof(s_pergunta), "%s", pergunta ? pergunta : "");
  destravar();
  if (mudou) { ++s_versao; }   // the bar swaps the prompt
  for (uint32_t t = 0;; t += 100) {
    travar();
    const bool tem = s_tem_resp;
    if (tem) { snprintf(out, n, "%s", s_resp); s_tem_resp = false; memset(s_resp, 0, sizeof(s_resp)); }
    destravar();
    if (tem) { return 1; }
    if (interrompido()) { return -1; }
    if (t >= espera_ms) { return 0; }
    xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(100));
  }
}

static bool sem_som(void) { return false; }

static const fundo_def_t FUNDO_TERMINAL = {
  .nome = "Terminal",
  .corpo = corpo,
  // THE SAME STACK AS THE CONSOLE (8 KB, console.cpp): the Terminal runs the same
  // commands. With 6 KB the monitor recorded only 256 B free (07/11/2026) - one
  // step from overflowing. Bigger than the pool slots: becomes an ordinary task
  // while there are commands queued and gives the RAM back when done. Stack in internal
  // RAM - a command that writes to NVS (wifi, ble) cannot run with its stack
  // in PSRAM.
  .pilha = 8192,
  .perfil = { },
  .ao_iniciar = nullptr,
  .ao_parar = nullptr,
  .com_som = sem_som,
  // A short command (ls, cd, help) does not flash in the status bar; anything over 3 s
  // (ping, copy, berry) appears, with the X to stop it.
  .aviso_ms = 3000,
};

// ---------------------------------------------------------------------------
// API
// API
bool tab5_terminal_abrir(void)
{
  if (!s_mx) {
    s_mx = xSemaphoreCreateMutex();
    s_resp_sem = xSemaphoreCreateBinary();
    if (!s_mx || !s_resp_sem) { return false; }
    snprintf(s_sessao.cwd, sizeof(s_sessao.cwd), "/sdcard");
    s_sessao.interrompido = interrompido;
    s_sessao.ler_linha = ler_linha;
    s_sessao.interativo = true;   // the app's screen and the /terminal/ page
  }
  if (!s_saida) {
    s_saida = fwopen(nullptr, ao_escrever);
    if (!s_saida) { return false; }
    setvbuf(s_saida, nullptr, _IONBF, 0);   // unbuffered: each printf arrives whole
  }
  if (!s_lin) {
    s_lin = (linha_t*)heap_caps_calloc(TERM_LINHAS, sizeof(linha_t), MALLOC_CAP_SPIRAM);
    if (!s_lin) { ESP_LOGE(TAG, "no PSRAM for the lines"); return false; }
    char t[128];
    snprintf(t, sizeof(t), "M5tabOS %s - Terminal\n%s\n\n", esp_app_get_description()->version,
             tr("'help' lista os comandos; Ctrl+C para o que roda."));
    aviso(t);
  }
  return true;
}

bool tab5_terminal_enviar(const char* linha)
{
  if (!s_lin || !linha) { return false; }
  // A command is reading: the line is its answer (the password does not appear
  // nor go into the history).
  travar();
  const bool lendo = s_lendo && !s_tem_resp;
  bool oculto = s_oculto;
  if (lendo) { snprintf(s_resp, sizeof(s_resp), "%s", linha); s_tem_resp = true; }
  destravar();
  if (lendo) {
    char e[TERM_ENTRADA + 4];
    snprintf(e, sizeof(e), "%s\n", oculto ? "****" : linha);
    escrever(e, (int)strlen(e), TERM_COMANDO);
    xSemaphoreGive(s_resp_sem);
    return true;
  }
  char eco[TERM_ENTRADA + 170];
  tab5_terminal_prompt(eco, sizeof(eco));
  strlcat(eco, linha, sizeof(eco));
  strlcat(eco, "\n", sizeof(eco));
  escrever(eco, (int)strlen(eco), TERM_COMANDO);

  travar();
  // Does the history keep empty lines too? No: it is the previous one you want again.
  const int ult = (s_hist_ini + s_hist_n - 1 + HIST_N) % HIST_N;
  if (linha[0] && !(s_hist_n && !strcmp(s_hist[ult], linha))) {
    const int i = (s_hist_ini + s_hist_n) % HIST_N;
    snprintf(s_hist[i], TERM_ENTRADA, "%s", linha);
    if (s_hist_n < HIST_N) { ++s_hist_n; } else { s_hist_ini = (s_hist_ini + 1) % HIST_N; }
  }
  bool ok = true;
  if (!linha[0]) {
    destravar();
    return true;
  }
  if (s_fila_n >= FILA_N) {
    ok = false;
  } else {
    snprintf(s_fila[(s_fila_ini + s_fila_n) % FILA_N], TERM_ENTRADA, "%s", linha);
    ++s_fila_n;
  }
  const bool ligar = ok && !s_rodando;
  if (ligar) { s_rodando = true; }
  destravar();
  if (!ok) { aviso(tr("(a fila esta cheia - espere o comando que roda, ou Ctrl+C)\n")); return false; }
  // The kernel starts it (in the interface, on the next round if it came from outside); with the
  // previous one still exiting, it waits for the slot and starts straight after.
  if (ligar && !sistema_fundo_pedir(&FUNDO_TERMINAL)) { travar(); s_rodando = false; destravar(); }
  return true;
}

void tab5_terminal_interromper(void)
{
  if (!s_mx) { return; }
  travar();
  const bool algo = s_rodando || s_fila_n;
  s_fila_n = 0;   // whatever was waiting goes too
  destravar();
  if (algo) { s_parar = true; }
  else      { aviso("^C\n"); }
}

bool tab5_terminal_ocupado(void)
{
  if (!s_mx) { return false; }
  travar();
  const bool o = s_rodando || s_fila_n > 0;
  destravar();
  return o;
}

void tab5_terminal_limpar(void)
{
  if (!s_lin) { return; }
  travar();
  limpar_trancado();
  destravar();
  ++s_versao;
}

uint32_t tab5_terminal_versao(void) { return s_versao.load(); }

uint32_t tab5_terminal_primeira(void)
{
  if (!s_lin) { return 0; }
  travar();
  const uint32_t p = s_total - (uint32_t)s_qtd;
  destravar();
  return p;
}

int tab5_terminal_linhas(void)
{
  if (!s_lin) { return 0; }
  travar();
  const int n = s_qtd;
  destravar();
  return n;
}

int tab5_terminal_tamanho(int i)
{
  if (!s_lin) { return 0; }
  travar();
  const int n = (i >= 0 && i < s_qtd) ? s_lin[(s_ini + i) % TERM_LINHAS].n : 0;
  destravar();
  return n;
}

int tab5_terminal_linha(int i, char* out, size_t n, term_tipo_t* tipo)
{
  if (!s_lin || !out || !n) { return -1; }
  travar();
  if (i < 0 || i >= s_qtd) { destravar(); out[0] = 0; return -1; }
  const linha_t& l = s_lin[(s_ini + i) % TERM_LINHAS];
  const size_t k = l.n < n - 1 ? l.n : n - 1;
  memcpy(out, l.t, k);
  out[k] = 0;
  if (tipo) { *tipo = (term_tipo_t)l.tipo; }
  destravar();
  return (int)k;
}

int tab5_terminal_linha_abs(uint32_t abs, char* out, size_t n, term_tipo_t* tipo)
{
  if (!s_lin || !out || !n) { return -1; }
  travar();
  const uint32_t primeira = s_total - (uint32_t)s_qtd;
  if (abs < primeira || abs >= s_total) { destravar(); out[0] = 0; return -1; }
  const linha_t& l = s_lin[(s_ini + (int)(abs - primeira)) % TERM_LINHAS];
  const size_t k = l.n < n - 1 ? l.n : n - 1;
  memcpy(out, l.t, k);
  out[k] = 0;
  if (tipo) { *tipo = (term_tipo_t)l.tipo; }
  destravar();
  return (int)k;
}

bool tab5_terminal_lendo(bool* oculto)
{
  if (!s_mx) { return false; }
  travar();
  const bool l = s_lendo;
  if (oculto) { *oculto = s_oculto; }
  destravar();
  return l;
}

void tab5_terminal_prompt(char* out, size_t n)
{
  if (s_mx) {
    travar();
    const bool l = s_lendo;
    if (l) { snprintf(out, n, "%s", s_pergunta); }
    destravar();
    if (l) { return; }
  }
  // No lock: 'cd' changes the folder inside the command, and a read in the middle
  // of the change shows a garbled name for just one frame - the next one is right.
  snprintf(out, n, "%s $ ", s_sessao.cwd[0] ? s_sessao.cwd : "/sdcard");
}

bool tab5_terminal_historico(int k, char* out, size_t n)
{
  if (!s_mx || k < 0) { return false; }
  travar();
  const bool tem = k < s_hist_n;
  if (tem) { snprintf(out, n, "%s", s_hist[(s_hist_ini + s_hist_n - 1 - k + HIST_N) % HIST_N]); }
  destravar();
  return tem;
}
