// TERMINAL - ver tab5_terminal.hpp.
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
// ESTADO. Tudo sob s_mx (uma trava so, pequena: as copias sao de uma linha).
// ---------------------------------------------------------------------------
typedef struct { uint8_t n; uint8_t tipo; char t[TERM_LINHA_MAX]; } linha_t;   // 128 B

static SemaphoreHandle_t s_mx  = nullptr;
static linha_t*          s_lin = nullptr;     // PSRAM, TERM_LINHAS, um anel
static int               s_ini = 0, s_qtd = 0;
static bool              s_aberta = false;    // a ultima linha ainda recebe texto (sem '\n')
static uint32_t          s_total = 0;         // linhas ja criadas desde o boot (a numeracao absoluta)
static std::atomic<uint32_t> s_versao{0};

#define FILA_N 4
#define HIST_N 16
EXT_RAM_BSS_ATTR static char s_fila[FILA_N][TERM_ENTRADA];
EXT_RAM_BSS_ATTR static char s_hist[HIST_N][TERM_ENTRADA];
static int  s_fila_ini = 0, s_fila_n = 0;
static int  s_hist_ini = 0, s_hist_n = 0;
static bool s_rodando = false;                // o corpo do fundo esta no laco

static std::atomic<bool> s_parar{false};
static fundo_t*          s_f = nullptr;       // o fundo que roda agora (so o corpo escreve)
static FILE*             s_saida = nullptr;   // o stdout da tarefa dos comandos

static bool interrompido(void) { return s_parar.load() || (s_f && !fundo_rodando(s_f)); }
EXT_RAM_BSS_ATTR static console_sessao_t s_sessao;

// O COMANDO QUE LE (console_ler_linha): enquanto 's_lendo', a linha digitada
// e dele - vai para a caixa 's_resp' em vez de virar comando novo na fila.
static SemaphoreHandle_t s_resp_sem = nullptr;
static bool  s_lendo = false, s_oculto = false, s_tem_resp = false;
EXT_RAM_BSS_ATTR static char s_pergunta[80];
EXT_RAM_BSS_ATTR static char s_resp[TERM_ENTRADA];

static void travar(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void destravar(void) { xSemaphoreGive(s_mx); }

// ---------------------------------------------------------------------------
// AS LINHAS
// ---------------------------------------------------------------------------
static linha_t* nova_linha(term_tipo_t tipo)
{
  int i;
  if (s_qtd < TERM_LINHAS) { i = (s_ini + s_qtd++) % TERM_LINHAS; }
  else                     { i = s_ini; s_ini = (s_ini + 1) % TERM_LINHAS; }   // a mais antiga sai
  s_lin[i].n = 0;
  s_lin[i].tipo = (uint8_t)tipo;
  s_aberta = true;
  ++s_total;
  return &s_lin[i];
}

static linha_t* ultima(void) { return &s_lin[(s_ini + s_qtd - 1) % TERM_LINHAS]; }

// Um byte do que o comando escreveu. Entende '\n', '\t', o ESC[2J do 'clear'
// e pula o resto das sequencias ANSI (cor, cursor) - nao ha onde usa-las aqui.
static uint8_t s_esc = 0;            // 0 = texto, 1 = viu ESC, 2 = dentro de ESC[...
static char    s_esc_par[8];
static uint8_t s_esc_n = 0;

static void limpar_trancado(void) { s_ini = s_qtd = 0; s_aberta = false; }

static void por_byte(uint8_t c, term_tipo_t tipo)
{
  if (s_esc == 1) { s_esc = (c == '[') ? 2 : 0; s_esc_n = 0; return; }
  if (s_esc == 2) {
    if (c >= 0x40 && c <= 0x7E) {   // a letra final da sequencia
      s_esc = 0;
      if (c == 'J' && s_esc_n == 1 && s_esc_par[0] == '2') { limpar_trancado(); }
    } else if (s_esc_n < sizeof(s_esc_par)) {
      s_esc_par[s_esc_n++] = (char)c;
    }
    return;
  }
  if (c == 0x1B) { s_esc = 1; return; }
  if (c == '\n') {
    if (!s_aberta) { nova_linha(tipo); }   // linha vazia tambem e linha
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
  // Cheia: continua na seguinte, sem cortar um caractere UTF-8 no meio (o
  // byte de continuacao, 10xxxxxx, ainda cabe na folga de 4).
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

// O stdout da tarefa dos comandos.
static int ao_escrever(void*, const char* b, int n) { escrever(b, n, TERM_SAIDA); return n; }

// ---------------------------------------------------------------------------
// A PARTE DE FUNDO: tira linhas da fila e roda, ate a fila secar
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
  // Comando rodando com a tela apagada: o aperto de mao do TLS (um HTTPS)
  // estoura o prazo a 40 MHz - a politica de PROCESSAMENTO segura 200.
  sistema_recurso_usar(RECURSO_PROCESSAMENTO, "Terminal");
  // Fila seca, sai na hora: a vaga do pool volta a dormir, e o proximo
  // comando liga outro fundo (barato - a tarefa ja existe no pool). O
  // 'sobrou' e conferido sob a trava, junto com o s_rodando = false: a linha
  // que chega nesse instante ou e vista aqui, ou liga um fundo novo.
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
    fundo_estado(f, 1, linha);   // o resumo da pastilha, se ela aparecer
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

// A sessao do Terminal pergunta pela barra da tela (console_ler_linha).
static int ler_linha(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms)
{
  travar();
  const bool mudou = !s_lendo || s_oculto != oculto || strcmp(s_pergunta, pergunta ? pergunta : "") != 0;
  s_lendo = true;
  s_oculto = oculto;
  snprintf(s_pergunta, sizeof(s_pergunta), "%s", pergunta ? pergunta : "");
  destravar();
  if (mudou) { ++s_versao; }   // a barra troca o prompt
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
  // A MESMA PILHA DO CONSOLE (8 KB, console.cpp): o Terminal roda os mesmos
  // comandos. Com 6 KB o monitor registrou so 256 B livres (07/11/2026) - a
  // um passo de estourar. Maior que as vagas do pool: vira tarefa comum
  // enquanto houver comando na fila e devolve a RAM ao terminar. Pilha na RAM
  // interna - comando que grava na NVS (wifi, ble) nao pode rodar com pilha
  // na PSRAM.
  .pilha = 8192,
  .perfil = { },
  .ao_iniciar = nullptr,
  .ao_parar = nullptr,
  .com_som = sem_som,
  // Comando curto (ls, cd, help) nao pisca na barra; o que passa de 3 s
  // (ping, copia, berry) aparece, com o X para parar.
  .aviso_ms = 3000,
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
bool tab5_terminal_abrir(void)
{
  if (!s_mx) {
    s_mx = xSemaphoreCreateMutex();
    s_resp_sem = xSemaphoreCreateBinary();
    if (!s_mx || !s_resp_sem) { return false; }
    snprintf(s_sessao.cwd, sizeof(s_sessao.cwd), "/sdcard");
    s_sessao.interrompido = interrompido;
    s_sessao.ler_linha = ler_linha;
    s_sessao.interativo = true;   // a tela do app e a pagina /terminal/
  }
  if (!s_saida) {
    s_saida = fwopen(nullptr, ao_escrever);
    if (!s_saida) { return false; }
    setvbuf(s_saida, nullptr, _IONBF, 0);   // sem buffer: cada printf ja chega inteiro
  }
  if (!s_lin) {
    s_lin = (linha_t*)heap_caps_calloc(TERM_LINHAS, sizeof(linha_t), MALLOC_CAP_SPIRAM);
    if (!s_lin) { ESP_LOGE(TAG, "sem PSRAM para as linhas"); return false; }
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
  // Um comando esta lendo: a linha e a resposta dele (a senha nao aparece
  // nem vai para o historico).
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
  // O historico guarda tambem linha vazia? Nao: e a de antes que se quer de novo.
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
  // O kernel liga (na interface, na volta seguinte se veio de fora dela); o
  // anterior ainda saindo, ele espera a vaga e liga em seguida.
  if (ligar && !sistema_fundo_pedir(&FUNDO_TERMINAL)) { travar(); s_rodando = false; destravar(); }
  return true;
}

void tab5_terminal_interromper(void)
{
  if (!s_mx) { return; }
  travar();
  const bool algo = s_rodando || s_fila_n;
  s_fila_n = 0;   // o que esperava tambem sai
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
  // Sem trava: o 'cd' troca a pasta dentro do comando, e uma leitura no meio
  // da troca mostra o nome torto por um quadro so - o seguinte ja sai certo.
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
