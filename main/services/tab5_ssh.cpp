// SSH (client) - see tab5_ssh.hpp.
#include "tab5_ssh.hpp"
#include "idioma.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include "libssh2.h"
#include "tab5_rede.hpp"
#include "tab5_sd.hpp"
#include <atomic>
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "app.h"
#include "pool_tarefas.h"
#include "tab5_hora.hpp"
extern "C" {
#include "vterm.h"
}

// ---------------------------------------------------------------------------
// MEMORY: everything from libssh2 in PSRAM ('abstract' is the session context).
// ---------------------------------------------------------------------------
static LIBSSH2_ALLOC_FUNC(alocar)     { (void)abstract; return heap_caps_malloc(count, MALLOC_CAP_SPIRAM); }
static LIBSSH2_FREE_FUNC(soltar)      { (void)abstract; heap_caps_free(ptr); }
static LIBSSH2_REALLOC_FUNC(realocar) { (void)abstract; return heap_caps_realloc(ptr, count, MALLOC_CAP_SPIRAM); }

typedef struct {
  const tab5_ssh_t* s;
  char              senha[128];
} ctx_t;

static void dizer(const tab5_ssh_t* s, const char* fmt, ...)
{
  char t[192];
  va_list a;
  va_start(a, fmt);
  const int n = vsnprintf(t, sizeof(t), fmt, a);
  va_end(a);
  if (s->saida && n > 0) { s->saida(t, n < (int)sizeof(t) ? n : (int)sizeof(t) - 1, s->ctx); }
}

static bool pare(const tab5_ssh_t* s) { return s->parar && s->parar(); }

// ---------------------------------------------------------------------------
// KNOWN SERVERS: one line "host:porta SHA256:..." per server, in the
// SSH folder on the card, through the tab5_sd gates.
// ---------------------------------------------------------------------------
#define KH_APP "SSH"
#define KH_ARQ "known_hosts"
#define KH_MAX 4096

// The whole content (PSRAM, ends in '\0'), or nullptr if there is no file.
static char* kh_ler(void)
{
  if (!tab5_sd_montar()) { return nullptr; }
  char* b = nullptr;
  FILE* f = tab5_sd_abrir(KH_APP, KH_ARQ, "r");
  if (f) {
    b = (char*)heap_caps_calloc(1, KH_MAX + 1, MALLOC_CAP_SPIRAM);
    if (b) { b[fread(b, 1, KH_MAX, f)] = 0; }
    fclose(f);
  }
  tab5_sd_desmontar();
  return b;
}

// 1 = known and matches; 0 = new server; -1 = CHANGED ('antes' receives the stored one).
static int kh_conferir(const char* chave, const char* fp, char* antes, size_t n)
{
  char* b = kh_ler();
  if (!b) { return 0; }
  int r = 0;
  const size_t lc = strlen(chave);
  for (char* l = b; *l && r == 0; ) {
    char* fim = strchr(l, '\n');
    if (fim) { *fim = 0; }
    if (!strncmp(l, chave, lc) && l[lc] == ' ') {
      const char* guardada = l + lc + 1;
      if (!strcmp(guardada, fp)) { r = 1; }
      else { r = -1; snprintf(antes, n, "%s", guardada); }
    }
    l = fim ? fim + 1 : l + strlen(l);
  }
  heap_caps_free(b);
  return r;
}

static void kh_guardar(const char* chave, const char* fp)
{
  char linha[160];
  const int k = snprintf(linha, sizeof(linha), "%s %s\n", chave, fp);
  if (tab5_sd_montar()) {
    tab5_sd_acrescentar(KH_APP, KH_ARQ, linha, (size_t)k);
    tab5_sd_desmontar();
  }
}

bool tab5_ssh_esquecer(const char* host, int porta)
{
  char chave[96];
  snprintf(chave, sizeof(chave), "%s:%d", host, porta > 0 ? porta : 22);
  char* b = kh_ler();
  if (!b) { return false; }
  char* novo = (char*)heap_caps_calloc(1, KH_MAX + 1, MALLOC_CAP_SPIRAM);
  bool achou = false;
  size_t k = 0;
  const size_t lc = strlen(chave);
  for (char* l = b; novo && *l; ) {
    char* fim = strchr(l, '\n');
    const size_t n = fim ? (size_t)(fim - l) : strlen(l);
    if (n > lc && !strncmp(l, chave, lc) && l[lc] == ' ') { achou = true; }
    else if (n) { memcpy(novo + k, l, n); k += n; novo[k++] = '\n'; }
    l += n + (fim ? 1 : 0);
  }
  if (achou && tab5_sd_montar()) {
    tab5_sd_substituir(KH_APP, KH_ARQ, novo, k);
    tab5_sd_desmontar();
  }
  heap_caps_free(novo);
  heap_caps_free(b);
  return achou;
}

// A line from the user, waiting up to 'ms' (in steps of 200, listening for Ctrl+C).
static bool pedir(const tab5_ssh_t* s, const char* pergunta, char* out, size_t n, bool oculto, uint32_t ms)
{
  if (!s->ler_linha) { return false; }
  for (uint32_t t = 0; t < ms && !pare(s); t += 200) {
    const int r = s->ler_linha(pergunta, out, n, oculto, 200);
    if (r == 1) { return true; }
    if (r < 0)  { return false; }
  }
  return false;
}

// "keyboard-interactive": the server sends the questions; the only one we know
// how to answer is the password (which is what nearly every server asks this way).
static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(kbd)
{
  (void)name; (void)name_len; (void)instruction; (void)instruction_len; (void)prompts;
  const ctx_t* c = (const ctx_t*)*abstract;
  for (int i = 0; i < num_prompts; ++i) {
    const size_t n = strlen(c->senha);
    responses[i].text = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (responses[i].text) { memcpy(responses[i].text, c->senha, n + 1); }
    responses[i].length = responses[i].text ? (unsigned int)n : 0;
  }
}

// Waits for the socket to have something to read (or be writable), up to ms.
static void esperar_socket(LIBSSH2_SESSION* ses, int sock, int ms)
{
  fd_set fd;
  FD_ZERO(&fd);
  FD_SET(sock, &fd);
  struct timeval tv = { 0, ms * 1000 };
  const int dir = libssh2_session_block_directions(ses);
  select(sock + 1, (dir & LIBSSH2_SESSION_BLOCK_INBOUND) ? &fd : nullptr,
         (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? &fd : nullptr, nullptr, &tv);
}

static bool escrever_tudo(LIBSSH2_SESSION* ses, LIBSSH2_CHANNEL* ch, int sock, const char* d, size_t n)
{
  while (n) {
    const ssize_t w = libssh2_channel_write(ch, d, n);
    if (w == LIBSSH2_ERROR_EAGAIN) { esperar_socket(ses, sock, 100); continue; }
    if (w < 0) { return false; }
    d += w;
    n -= (size_t)w;
  }
  return true;
}

// libssh2 initialises once (the command and the session use the same one).
static bool garantir_libssh2(void)
{
  static bool iniciado = false;
  if (!iniciado && libssh2_init(0) == 0) { iniciado = true; }
  return iniciado;
}

// The server key's fingerprint, in OpenSSH format
// ("SHA256:..."). false = libssh2 did not give the key.
static bool impressao_de(LIBSSH2_SESSION* ses, char* fp, size_t n)
{
  const unsigned char* h = (const unsigned char*)libssh2_hostkey_hash(ses, LIBSSH2_HOSTKEY_HASH_SHA256);
  if (!h) { return false; }
  unsigned char b64[64];
  size_t k = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &k, h, 32);
  while (k && b64[k - 1] == '=') { --k; }
  b64[k] = 0;
  snprintf(fp, n, "SHA256:%s", (const char*)b64);
  return true;
}

int tab5_ssh_rodar(const tab5_ssh_t* s, char* erro, size_t erro_n)
{
  if (!garantir_libssh2()) { snprintf(erro, erro_n, tr("o libssh2 nao iniciou")); return -1; }
  const int porta = s->porta > 0 ? s->porta : 22;
  const bool shell = !s->comando || !s->comando[0];
  int codigo = -1;
  erro[0] = 0;

  uint32_t ms = 0;
  const int sock = tab5_rede_conectar(s->host, porta, 8000, &ms);
  if (sock < 0) {
    snprintf(erro, erro_n, sock == -1 ? tr("%s nao resolveu") : tr("%s:%d nao conectou (%s)"), s->host, porta,
             sock == -1 ? "" : strerror(-sock));
    return -1;
  }
  // Socket read timeout: a server that vanishes does not hold up the command.
  struct timeval tv = { 15, 0 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ctx_t* c = (ctx_t*)heap_caps_calloc(1, sizeof(ctx_t), MALLOC_CAP_SPIRAM);
  LIBSSH2_SESSION* ses = c ? libssh2_session_init_ex(alocar, soltar, realocar, c) : nullptr;
  LIBSSH2_CHANNEL* ch = nullptr;
  if (!ses) { snprintf(erro, erro_n, tr("sem memoria")); goto fim; }
  c->s = s;
  libssh2_session_set_blocking(ses, 1);
  libssh2_session_set_timeout(ses, 15000);

  dizer(s, tr("conectado a %s:%d (%lu ms)\n"), s->host, porta, (unsigned long)ms);
  if (libssh2_session_handshake(ses, sock) != 0) {
    char* m = nullptr;
    libssh2_session_last_error(ses, &m, nullptr, 0);
    snprintf(erro, erro_n, tr("o aperto de mao falhou: %s"), m ? m : "?");
    goto fim;
  }
  {
    // THE FINGERPRINT of the server key, in OpenSSH format: whoever
    // connects for the first time checks it against the server's own
    // (ssh-keygen -lf /etc/ssh/ssh_host_*_key.pub).
    char fp[64];
    if (impressao_de(ses, fp, sizeof(fp))) {
      char chave[96], antes[64] = "";
      snprintf(chave, sizeof(chave), "%s:%d", s->host, porta);
      const int kh = kh_conferir(chave, fp, antes, sizeof(antes));
      if (kh < 0) {
        dizer(s, tr("ATENCAO: a chave deste servidor MUDOU.\n  guardada: %s\n  agora:    %s\n"
                 "Pode ser outro aparelho no lugar do servidor. Se ele foi reinstalado,\n"
                 "esqueca a antiga com: ssh esquecer %s:%d\n"), antes, fp, s->host, porta);
        snprintf(erro, erro_n, tr("chave do servidor diferente da guardada - nao conectei"));
        goto fim;
      }
      if (kh == 0) {
        kh_guardar(chave, fp);
        dizer(s, tr("servidor novo - chave guardada: %s\n"), fp);
      } else {
        dizer(s, tr("chave do servidor confere (%s)\n"), fp);
      }
    }
  }

  // AUTHENTICATION: password only (via "password" or "keyboard-interactive").
  {
    const char* metodos = libssh2_userauth_list(ses, s->usuario, (unsigned int)strlen(s->usuario));
    if (!metodos && !libssh2_userauth_authenticated(ses)) {
      snprintf(erro, erro_n, tr("o servidor nao disse como autenticar"));
      goto fim;
    }
    const bool por_senha = metodos && strstr(metodos, "password");
    const bool por_kbd   = metodos && strstr(metodos, "keyboard-interactive");
    if (metodos && !por_senha && !por_kbd) {
      snprintf(erro, erro_n, tr("o servidor so aceita: %s (senha nao)"), metodos);
      goto fim;
    }
    char pergunta[96];
    snprintf(pergunta, sizeof(pergunta), tr("senha de %s@%s: "), s->usuario, s->host);
    for (int tent = 0; metodos && tent < 3 && !libssh2_userauth_authenticated(ses); ++tent) {
      if (!pedir(s, pergunta, c->senha, sizeof(c->senha), true, 120000)) {
        snprintf(erro, erro_n, tr("sem senha"));
        goto fim;
      }
      const int r = por_senha ? libssh2_userauth_password(ses, s->usuario, c->senha)
                              : libssh2_userauth_keyboard_interactive(ses, s->usuario, kbd);
      memset(c->senha, 0, sizeof(c->senha));
      if (r != 0) { dizer(s, tr("senha recusada\n")); }
    }
    if (!libssh2_userauth_authenticated(ses)) {
      if (!erro[0]) { snprintf(erro, erro_n, tr("nao autenticou")); }
      goto fim;
    }
  }

  ch = libssh2_channel_open_session(ses);
  if (!ch) { snprintf(erro, erro_n, "o servidor recusou o canal"); goto fim; }
  if (shell) {
    // "dumb": the other side sends no colour and draws no screen - line by line.
    if (libssh2_channel_request_pty(ch, "dumb") != 0 || libssh2_channel_shell(ch) != 0) {
      snprintf(erro, erro_n, tr("o servidor nao abriu o shell"));
      goto fim;
    }
    dizer(s, tr("shell linha a linha ('exit' sai; nano/top/vim nao funcionam aqui)\n"));
  } else if (libssh2_channel_exec(ch, s->comando) != 0) {
    snprintf(erro, erro_n, "o servidor recusou o comando");
    goto fim;
  }

  // THE LOOP: what arrives (stdout and stderr from the other side) and, in the shell, what is typed.
  libssh2_session_set_blocking(ses, 0);
  {
    char* b = (char*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    char linha[194];
    while (b) {
      bool veio = false;
      for (int k = 0; k < 2; ++k) {
        const ssize_t n = libssh2_channel_read_ex(ch, k, b, 1024);
        if (n > 0) { if (s->saida) { s->saida(b, (int)n, s->ctx); } veio = true; }
        else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) { snprintf(erro, erro_n, "a conexao caiu"); break; }
      }
      if (erro[0] || libssh2_channel_eof(ch)) { break; }
      if (pare(s)) { snprintf(erro, erro_n, "interrompido"); break; }
      if (shell) {
        const int r = s->ler_linha ? s->ler_linha("", linha, sizeof(linha) - 2, false, veio ? 0 : 100) : -1;
        if (r < 0) { snprintf(erro, erro_n, "interrompido"); break; }
        if (r == 1) {
          strlcat(linha, "\n", sizeof(linha));
          if (!escrever_tudo(ses, ch, sock, linha, strlen(linha))) { snprintf(erro, erro_n, "a conexao caiu"); break; }
        }
      } else if (!veio) {
        esperar_socket(ses, sock, 200);
      }
    }
    heap_caps_free(b);
  }
  libssh2_session_set_blocking(ses, 1);
  libssh2_channel_close(ch);
  libssh2_channel_wait_closed(ch);
  if (!erro[0]) { codigo = libssh2_channel_get_exit_status(ch); }

fim:
  if (ch)  { libssh2_channel_free(ch); }
  if (ses) { libssh2_session_disconnect(ses, "tchau"); libssh2_session_free(ses); }
  if (c)   { memset(c->senha, 0, sizeof(c->senha)); heap_caps_free(c); }
  close(sock);
  return codigo;
}

// ===========================================================================
// THE INTERACTIVE SESSION - see tab5_ssh.hpp
// ===========================================================================
static const char* TAG_S = "ssh_sessao";

#define ENTRADA_TAM  2048   // keys waiting for the session task
#define ASSIN_MAX    4
#define PERGUNTA_MS  120000 // how long to wait for the password or "accept"

// State, screen (the vterm) and subscribers: under s_trava. The session task is the
// only one that talks to the server; the others only ask.
static SemaphoreHandle_t      s_trava_s = nullptr;
static tab5_ssh_sessao_info_t s_info;
static StreamBufferHandle_t   s_entrada = nullptr;
static std::atomic<bool>      s_sair{false};
static std::atomic<int>       s_aceitou{-1};      // CONFIRMAR: -1 waiting, 0 no, 1 yes
static char                   s_senha_dada[128];
static std::atomic<bool>      s_tem_senha{false};
static bool                   s_quer_tamanho = false;
static int                    s_quer_cols = 80, s_quer_lin = 24;
static VTerm*                 s_vt = nullptr;
static VTermScreen*           s_vs = nullptr;
static bool                   s_cursor_visivel = true;
static struct { tab5_ssh_saida_t fn; void* ctx; } s_assin[ASSIN_MAX];

static void trava_s(void)   { xSemaphoreTake(s_trava_s, portMAX_DELAY); }
static void solta_s(void)   { xSemaphoreGive(s_trava_s); }

// Notifies subscribers OUTSIDE the lock (the browser sends via the web server, which
// may be busy: holding the lock here would block whoever is writing).
// d = nullptr: "the state (or size) changed - read tab5_ssh_sessao_info".
static void avisar_assinantes(const char* d, size_t n)
{
  decltype(s_assin) copia;
  trava_s();
  memcpy(copia, s_assin, sizeof(copia));
  solta_s();
  for (auto& a : copia) { if (a.fn) { a.fn(d, n, a.ctx); } }
}

static void mudar(tab5_ssh_estado_t e, const char* msg)
{
  trava_s();
  s_info.estado = e;
  if (msg) { snprintf(s_info.msg, sizeof(s_info.msg), "%s", msg); }
  ++s_info.versao;
  solta_s();
  ESP_LOGI(TAG_S, "state %d%s%s", (int)e, msg && msg[0] ? ": " : "", msg ? msg : "");
  sistema_avisar(AVISO_SSH);
  avisar_assinantes(nullptr, 0);
}

// vterm allocates in PSRAM (a 200x60 screen is about 400 KB of cells).
static void* vt_alocar(size_t n, void*) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM); }
static void  vt_soltar(void* p, void*)  { heap_caps_free(p); }
static VTermAllocatorFunctions VT_ALOCADOR = { vt_alocar, vt_soltar };

static int vt_prop(VTermProp prop, VTermValue* v, void*)
{
  if (prop == VTERM_PROP_CURSORVISIBLE) { s_cursor_visivel = v->boolean; }
  return 1;
}
static const VTermScreenCallbacks VT_TELA = { nullptr, nullptr, nullptr, vt_prop, nullptr, nullptr,
                                              nullptr, nullptr, nullptr };

// What the emulator itself answers to the server (questions like "what terminal
// are you?"): goes through the same queue as the keys. Called with the lock held.
static void vt_saida(const char* d, size_t n, void*)
{
  if (s_entrada) { xStreamBufferSend(s_entrada, d, n, 0); }
}

// Waits for an answer from the user (password or accept), listening for "close".
static bool esperar_resposta(bool senha)
{
  for (uint32_t t = 0; t < PERGUNTA_MS && !s_sair.load(); t += 100) {
    if (senha ? s_tem_senha.load() : s_aceitou.load() >= 0) { return true; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

static void corpo_sessao(void*)
{
  char usuario[32], host[64], erro[96] = "";
  int porta, cols, lin;
  trava_s();
  snprintf(usuario, sizeof(usuario), "%s", s_info.usuario);
  snprintf(host, sizeof(host), "%s", s_info.host);
  porta = s_info.porta;
  cols = s_quer_cols;
  lin = s_quer_lin;
  s_quer_tamanho = false;
  solta_s();

  LIBSSH2_SESSION* ses = nullptr;
  LIBSSH2_CHANNEL* ch = nullptr;
  ctx_t* c = nullptr;
  char* buf = nullptr;
  int sock = -1;
  uint32_t ms = 0;
  if (!garantir_libssh2()) { snprintf(erro, sizeof(erro), tr("o libssh2 nao iniciou")); goto fim; }
  sock = tab5_rede_conectar(host, porta, 8000, &ms);
  if (sock < 0) { snprintf(erro, sizeof(erro), sock == -1 ? tr("%s nao resolveu") : tr("%s nao conectou"), host); goto fim; }
  {
    struct timeval tv = { 15, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  c = (ctx_t*)heap_caps_calloc(1, sizeof(ctx_t), MALLOC_CAP_SPIRAM);
  ses = c ? libssh2_session_init_ex(alocar, soltar, realocar, c) : nullptr;
  if (!ses) { snprintf(erro, sizeof(erro), tr("sem memoria")); goto fim; }
  libssh2_session_set_blocking(ses, 1);
  libssh2_session_set_timeout(ses, 15000);
  if (libssh2_session_handshake(ses, sock) != 0) { snprintf(erro, sizeof(erro), tr("o aperto de mao falhou")); goto fim; }

  // IS THE SERVER WHO IT CLAIMS TO BE? Same known_hosts as the ssh command.
  {
    char fp[64], chave[96], antes[64] = "";
    if (!impressao_de(ses, fp, sizeof(fp))) { snprintf(erro, sizeof(erro), tr("o servidor nao mostrou a chave")); goto fim; }
    snprintf(chave, sizeof(chave), "%s:%d", host, porta);
    trava_s();
    snprintf(s_info.impressao, sizeof(s_info.impressao), "%s", fp);
    solta_s();
    const int kh = kh_conferir(chave, fp, antes, sizeof(antes));
    if (kh < 0) {
      snprintf(erro, sizeof(erro), tr("a chave do servidor MUDOU (guardada %.20s...) - nao conectei"), antes);
      goto fim;
    }
    if (kh == 0) {
      s_aceitou = -1;
      mudar(TAB5_SSH_CONFIRMAR, "servidor novo: confira a impressao digital");
      if (!esperar_resposta(false) || s_aceitou.load() != 1) { snprintf(erro, sizeof(erro), tr("impressao digital nao aceita")); goto fim; }
      kh_guardar(chave, fp);
    }
  }

  // THE PASSWORD (via "password" or "keyboard-interactive"), up to 3 attempts.
  {
    const char* metodos = libssh2_userauth_list(ses, usuario, (unsigned int)strlen(usuario));
    const bool por_senha = metodos && strstr(metodos, "password");
    const bool por_kbd   = metodos && strstr(metodos, "keyboard-interactive");
    if (metodos && !por_senha && !por_kbd) { snprintf(erro, sizeof(erro), "o servidor so aceita: %.60s", metodos); goto fim; }
    for (int tent = 0; metodos && tent < 3 && !libssh2_userauth_authenticated(ses); ++tent) {
      s_tem_senha = false;
      mudar(TAB5_SSH_SENHA, tent ? tr("senha recusada - tente de novo") : "");
      if (!esperar_resposta(true)) { snprintf(erro, sizeof(erro), tr("sem senha")); goto fim; }
      trava_s();
      snprintf(c->senha, sizeof(c->senha), "%s", s_senha_dada);
      memset(s_senha_dada, 0, sizeof(s_senha_dada));
      solta_s();
      mudar(TAB5_SSH_CONECTANDO, tr("conferindo a senha..."));
      const int r = por_senha ? libssh2_userauth_password(ses, usuario, c->senha)
                              : libssh2_userauth_keyboard_interactive(ses, usuario, kbd);
      memset(c->senha, 0, sizeof(c->senha));
      (void)r;
    }
    if (!libssh2_userauth_authenticated(ses)) { snprintf(erro, sizeof(erro), tr("nao autenticou")); goto fim; }
  }

  // THE REAL TERMINAL: xterm-256color at the size of whoever opened it.
  ch = libssh2_channel_open_session(ses);
  if (!ch || libssh2_channel_request_pty_ex(ch, "xterm-256color", 14, nullptr, 0, cols, lin, 0, 0) != 0
      || libssh2_channel_shell(ch) != 0) {
    snprintf(erro, sizeof(erro), tr("o servidor nao abriu o terminal"));
    goto fim;
  }
  {
    struct VTermBuilder b = {};
    b.rows = lin; b.cols = cols; b.allocator = &VT_ALOCADOR;
    VTerm* vt = vterm_build(&b);
    if (!vt) { snprintf(erro, sizeof(erro), tr("sem memoria para a tela")); goto fim; }
    vterm_set_utf8(vt, 1);
    VTermScreen* vs = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(vs, &VT_TELA, nullptr);
    vterm_screen_enable_altscreen(vs, 1);
    vterm_screen_reset(vs, 1);
    vterm_output_set_callback(vt, vt_saida, nullptr);
    trava_s();
    s_vt = vt; s_vs = vs; s_cursor_visivel = true;
    s_info.cols = cols; s_info.linhas = lin;
    solta_s();
  }
  buf = (char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  if (!buf) { snprintf(erro, sizeof(erro), tr("sem memoria")); goto fim; }
  mudar(TAB5_SSH_ABERTA, "");

  // THE LOOP: non-blocking - what arrives goes to the screen and the subscribers; what
  // was typed (the queue) goes to the server; the requested size becomes the PTY's.
  libssh2_session_set_blocking(ses, 0);
  while (!s_sair.load()) {
    bool fez = false;
    trava_s();
    const bool quer = s_quer_tamanho;
    const int qc = s_quer_cols, ql = s_quer_lin;
    solta_s();
    if (quer) {
      const int r = libssh2_channel_request_pty_size(ch, qc, ql);
      if (r != LIBSSH2_ERROR_EAGAIN) {
        trava_s();
        s_quer_tamanho = false;
        vterm_set_size(s_vt, ql, qc);
        s_info.cols = qc; s_info.linhas = ql;
        ++s_info.versao; ++s_info.tela;
        solta_s();
        sistema_avisar(AVISO_SSH);
        avisar_assinantes(nullptr, 0);
        fez = true;
      }
    }
    const size_t nt = xStreamBufferReceive(s_entrada, buf, 512, 0);
    if (nt > 0) {
      if (!escrever_tudo(ses, ch, sock, buf, nt)) { snprintf(erro, sizeof(erro), "a conexao caiu"); break; }
      fez = true;
    }
    for (int k = 0; k < 2; ++k) {
      const ssize_t n = libssh2_channel_read_ex(ch, k, buf, 2048);
      if (n > 0) {
        trava_s();
        vterm_input_write(s_vt, buf, (size_t)n);
        ++s_info.tela;
        solta_s();
        sistema_avisar(AVISO_SSH);
        avisar_assinantes(buf, (size_t)n);
        fez = true;
      } else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
        snprintf(erro, sizeof(erro), "a conexao caiu");
      }
    }
    if (erro[0] || libssh2_channel_eof(ch)) { break; }
    if (!fez) { esperar_socket(ses, sock, 20); }
  }
  if (!erro[0]) { snprintf(erro, sizeof(erro), s_sair.load() ? "sessao encerrada" : "o servidor fechou a sessao"); }

fim:
  if (ch)  { libssh2_session_set_blocking(ses, 1); libssh2_channel_close(ch); libssh2_channel_free(ch); }
  if (ses) { libssh2_session_disconnect(ses, "tchau"); libssh2_session_free(ses); }
  if (c)   { memset(c->senha, 0, sizeof(c->senha)); heap_caps_free(c); }
  if (sock >= 0) { close(sock); }
  heap_caps_free(buf);
  trava_s();
  VTerm* vt = s_vt;
  s_vt = nullptr; s_vs = nullptr;
  memset(s_senha_dada, 0, sizeof(s_senha_dada));
  solta_s();
  if (vt) { vterm_free(vt); }
  mudar(TAB5_SSH_FIM, erro);
}

static bool preparar_sessao(void)
{
  if (!s_trava_s) { s_trava_s = xSemaphoreCreateMutex(); }
  if (!s_entrada) { s_entrada = xStreamBufferCreateWithCaps(ENTRADA_TAM, 1, MALLOC_CAP_SPIRAM); }
  return s_trava_s && s_entrada;
}

bool tab5_ssh_sessao_abrir(const char* usuario, const char* host, int porta, int cols, int linhas)
{
  if (!usuario || !usuario[0] || !host || !host[0] || !preparar_sessao()) { return false; }
  trava_s();
  const bool livre = s_info.estado == TAB5_SSH_LIVRE || s_info.estado == TAB5_SSH_FIM;
  if (livre) {
    const uint32_t v = s_info.versao, t = s_info.tela;
    memset(&s_info, 0, sizeof(s_info));
    s_info.versao = v + 1; s_info.tela = t + 1;
    s_info.estado = TAB5_SSH_CONECTANDO;
    snprintf(s_info.usuario, sizeof(s_info.usuario), "%s", usuario);
    snprintf(s_info.host, sizeof(s_info.host), "%s", host);
    s_info.porta = porta > 0 ? porta : 22;
    s_quer_cols = cols < 20 ? 80 : cols;
    s_quer_lin = linhas < 5 ? 24 : linhas;
    snprintf(s_info.msg, sizeof(s_info.msg), "conectando a %s...", host);
  }
  solta_s();
  if (!livre) { return false; }
  s_sair = false;
  s_aceitou = -1;
  s_tem_senha = false;
  xStreamBufferReset(s_entrada);
  if (!pool_criar(nullptr, corpo_sessao, nullptr, "ssh", 6144, 3, DONO_SERVICO, "SSH")) {
    mudar(TAB5_SSH_FIM, tr("sem tarefa para a sessao"));
    return false;
  }
  sistema_avisar(AVISO_SSH);
  avisar_assinantes(nullptr, 0);
  return true;
}

void tab5_ssh_sessao_info(tab5_ssh_sessao_info_t* out)
{
  if (!s_trava_s) { memset(out, 0, sizeof(*out)); return; }
  trava_s();
  *out = s_info;
  solta_s();
}

void tab5_ssh_sessao_confirmar(bool aceitar) { s_aceitou = aceitar ? 1 : 0; }

void tab5_ssh_sessao_senha(const char* senha)
{
  if (!s_trava_s || !senha) { return; }
  trava_s();
  snprintf(s_senha_dada, sizeof(s_senha_dada), "%s", senha);
  solta_s();
  s_tem_senha = true;
}

void tab5_ssh_sessao_escrever(const void* d, size_t n)
{
  if (!s_trava_s || !n) { return; }
  trava_s();   // the queue has a single writer: the app and the browser take turns here
  if (s_info.estado == TAB5_SSH_ABERTA) { xStreamBufferSend(s_entrada, d, n, 0); }
  solta_s();
}

void tab5_ssh_sessao_tamanho(int cols, int linhas)
{
  if (!s_trava_s) { return; }
  if (cols < 20) { cols = 20; }
  if (cols > 240) { cols = 240; }
  if (linhas < 5) { linhas = 5; }
  if (linhas > 80) { linhas = 80; }
  trava_s();
  if (cols != s_info.cols || linhas != s_info.linhas) { s_quer_cols = cols; s_quer_lin = linhas; s_quer_tamanho = true; }
  solta_s();
}

void tab5_ssh_sessao_fechar(void) { s_sair = true; s_aceitou = 0; }

void tab5_ssh_sessao_esquecer_fim(void)
{
  if (!s_trava_s) { return; }
  trava_s();
  if (s_info.estado == TAB5_SSH_FIM) { s_info.estado = TAB5_SSH_LIVRE; ++s_info.versao; }
  solta_s();
}

int tab5_ssh_sessao_assinar(tab5_ssh_saida_t fn, void* ctx)
{
  if (!fn || !preparar_sessao()) { return -1; }
  int id = -1;
  trava_s();
  for (int i = 0; i < ASSIN_MAX && id < 0; ++i) { if (!s_assin[i].fn) { s_assin[i].fn = fn; s_assin[i].ctx = ctx; id = i; } }
  solta_s();
  return id;
}

void tab5_ssh_sessao_desassinar(int id)
{
  if (!s_trava_s || id < 0 || id >= ASSIN_MAX) { return; }
  trava_s();
  s_assin[id].fn = nullptr;
  s_assin[id].ctx = nullptr;
  solta_s();
}

// SGR for a vterm colour: default, palette (16 and 256) or RGB.
static int sgr_cor(char* o, size_t n, const VTermColor* c, bool fundo)
{
  if (fundo ? VTERM_COLOR_IS_DEFAULT_BG(c) : VTERM_COLOR_IS_DEFAULT_FG(c)) { return snprintf(o, n, ";%d", fundo ? 49 : 39); }
  if (VTERM_COLOR_IS_INDEXED(c)) {
    const int i = c->indexed.idx;
    if (i < 8)  { return snprintf(o, n, ";%d", (fundo ? 40 : 30) + i); }
    if (i < 16) { return snprintf(o, n, ";%d", (fundo ? 100 : 90) + i - 8); }
    return snprintf(o, n, ";%d;5;%d", fundo ? 48 : 38, i);
  }
  return snprintf(o, n, ";%d;2;%d;%d;%d", fundo ? 48 : 38, c->rgb.red, c->rgb.green, c->rgb.blue);
}

size_t tab5_ssh_sessao_retrato(char* out, size_t max)
{
  if (!s_trava_s || max < 256) { return 0; }
  size_t k = 0;
  auto por = [&](const char* t, size_t n) { if (k + n < max) { memcpy(out + k, t, n); k += n; } };
  trava_s();
  if (!s_vs) { solta_s(); return 0; }
  const int L = s_info.linhas, C = s_info.cols;
  por("\x1b[0m\x1b[H\x1b[2J", 11);
  char sgr_ant[64] = "";
  for (int l = 0; l < L && k + 64 < max; ++l) {
    char cup[16];
    por(cup, (size_t)snprintf(cup, sizeof(cup), "\x1b[%d;1H", l + 1));
    for (int col = 0; col < C; ++col) {
      VTermScreenCell cel;
      if (!vterm_screen_get_cell(s_vs, (VTermPos){ l, col }, &cel)) { continue; }
      char sgr[64];
      int m = snprintf(sgr, sizeof(sgr), "\x1b[0");
      if (cel.attrs.bold)      { m += snprintf(sgr + m, sizeof(sgr) - m, ";1"); }
      if (cel.attrs.underline) { m += snprintf(sgr + m, sizeof(sgr) - m, ";4"); }
      if (cel.attrs.reverse)   { m += snprintf(sgr + m, sizeof(sgr) - m, ";7"); }
      m += sgr_cor(sgr + m, sizeof(sgr) - m, &cel.fg, false);
      m += sgr_cor(sgr + m, sizeof(sgr) - m, &cel.bg, true);
      m += snprintf(sgr + m, sizeof(sgr) - m, "m");
      if (strcmp(sgr, sgr_ant) != 0) { por(sgr, (size_t)m); memcpy(sgr_ant, sgr, (size_t)m + 1); }
      if (cel.chars[0] == 0 || cel.chars[0] == (uint32_t)-1) { if (cel.chars[0] == 0) { por(" ", 1); } continue; }
      for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cel.chars[i]; ++i) {
        char u[4];
        const uint32_t cp = cel.chars[i];
        int nu;
        if (cp < 0x80)         { u[0] = (char)cp; nu = 1; }
        else if (cp < 0x800)   { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); nu = 2; }
        else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); nu = 3; }
        else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); nu = 4; }
        por(u, (size_t)nu);
      }
      if (cel.width == 2) { ++col; }
    }
  }
  VTermPos cur;
  vterm_state_get_cursorpos(vterm_obtain_state(s_vt), &cur);
  const bool vis = s_cursor_visivel;
  solta_s();
  char fim[40];
  por(fim, (size_t)snprintf(fim, sizeof(fim), "\x1b[0m\x1b[%d;%dH\x1b[?25%c", cur.row + 1, cur.col + 1, vis ? 'h' : 'l'));
  return k;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

bool tab5_ssh_sessao_celulas(int linha, tab5_ssh_celula_t* out, int max)
{
  if (!s_trava_s) { return false; }
  trava_s();
  if (!s_vs || linha < 0 || linha >= s_info.linhas) { solta_s(); return false; }
  const int C = s_info.cols < max ? s_info.cols : max;
  for (int col = 0; col < C; ++col) {
    VTermScreenCell cel;
    tab5_ssh_celula_t& o = out[col];
    memset(&o, 0, sizeof(o));
    if (!vterm_screen_get_cell(s_vs, (VTermPos){ linha, col }, &cel)) { continue; }
    o.cp = (cel.chars[0] == (uint32_t)-1) ? 0 : cel.chars[0];
    o.largura = cel.width == 2 ? 2 : 1;
    o.negrito = cel.attrs.bold;
    o.sublinhado = cel.attrs.underline != 0;
    VTermColor fg = cel.fg, bg = cel.bg;
    const bool fg_pad = VTERM_COLOR_IS_DEFAULT_FG(&fg), bg_pad = VTERM_COLOR_IS_DEFAULT_BG(&bg);
    vterm_screen_convert_color_to_rgb(s_vs, &fg);
    vterm_screen_convert_color_to_rgb(s_vs, &bg);
    o.fg = fg_pad ? (uint16_t)0xD69A : rgb565(fg.rgb.red, fg.rgb.green, fg.rgb.blue);   // default: light grey
    o.bg = bg_pad ? (uint16_t)0x0000 : rgb565(bg.rgb.red, bg.rgb.green, bg.rgb.blue);
    if (cel.attrs.reverse) { const uint16_t t = o.fg; o.fg = o.bg; o.bg = t; }
  }
  solta_s();
  return true;
}

void tab5_ssh_sessao_cursor(int* linha, int* coluna, bool* visivel)
{
  *linha = *coluna = 0;
  *visivel = false;
  if (!s_trava_s) { return; }
  trava_s();
  if (s_vt) {
    VTermPos p;
    vterm_state_get_cursorpos(vterm_obtain_state(s_vt), &p);
    *linha = p.row; *coluna = p.col; *visivel = s_cursor_visivel;
  }
  solta_s();
}
