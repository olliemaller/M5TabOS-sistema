// The Terminal in the PC's browser - see tab5_terminal_web.hpp.
#include "tab5_terminal_web.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tab5_web.hpp"
#include "tab5_ssh.hpp"
#include "tab5_terminal.hpp"

static const char* TAG = "terminal_web";

extern const char PAGINA_INICIO[]   asm("_binary_tab5_terminal_web_html_start");
extern const char PAGINA_FIM[]      asm("_binary_tab5_terminal_web_html_end");
extern const uint8_t XTERM_JS_I[]   asm("_binary_xterm_js_gz_start");
extern const uint8_t XTERM_JS_F[]   asm("_binary_xterm_js_gz_end");
extern const uint8_t XTERM_CSS_I[]  asm("_binary_xterm_css_gz_start");
extern const uint8_t XTERM_CSS_F[]  asm("_binary_xterm_css_gz_end");
extern const uint8_t FIT_JS_I[]     asm("_binary_addon_fit_js_gz_start");
extern const uint8_t FIT_JS_F[]     asm("_binary_addon_fit_js_gz_end");

#define CLIENTES_MAX 3
#define RETRATO_MAX  (48 * 1024)
#define CONSOLE_MAX  (48 * 1024)   // one api/console response (PSRAM)
#define CONSOLE_LIN  200           // lines per response; the rest comes in the next one

// The open tabs (WebSocket connections) and the session subscription, which
// exists while there is at least one. Under s_trava (the server task and the
// session task both touch this).
static SemaphoreHandle_t s_trava = nullptr;
static int s_fds[CLIENTES_MAX] = { -1, -1, -1 };
static int s_assinatura = -1;

static void travar(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static void destravar(void) { xSemaphoreGive(s_trava); }

static esp_err_t pagina(httpd_req_t* req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PAGINA_INICIO, PAGINA_FIM - PAGINA_INICIO - 1);   // TEXT adds a '\0' at the end
}

// The xterm.js files, compressed in flash: the browser decompresses them.
static esp_err_t gz(httpd_req_t* req, const char* tipo, const uint8_t* i, const uint8_t* f)
{
  httpd_resp_set_type(req, tipo);
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");   // does not change between firmware versions
  return httpd_resp_send(req, (const char*)i, f - i);
}
static esp_err_t xterm_js(httpd_req_t* req)  { return gz(req, "application/javascript", XTERM_JS_I, XTERM_JS_F); }
static esp_err_t xterm_css(httpd_req_t* req) { return gz(req, "text/css", XTERM_CSS_I, XTERM_CSS_F); }
static esp_err_t fit_js(httpd_req_t* req)    { return gz(req, "application/javascript", FIT_JS_I, FIT_JS_F); }

// The session state as JSON (short text, on the stack).
static int estado_json(char* out, size_t n)
{
  tab5_ssh_sessao_info_t i;
  tab5_ssh_sessao_info(&i);
  char msg[200], host[140], usu[72];
  tab5_web_json_texto(msg, sizeof(msg), i.msg);
  tab5_web_json_texto(host, sizeof(host), i.host);
  tab5_web_json_texto(usu, sizeof(usu), i.usuario);
  return snprintf(out, n,
                  "{\"t\":\"estado\",\"estado\":%d,\"u\":%s,\"h\":%s,\"p\":%d,\"impressao\":\"%s\",\"msg\":%s,\"c\":%d,\"l\":%d}",
                  (int)i.estado, usu, host, i.porta, i.impressao, msg, i.cols, i.linhas);
}

static void mandar_a_todos(const void* d, size_t n, bool texto)
{
  int fds[CLIENTES_MAX];
  travar();
  memcpy(fds, s_fds, sizeof(fds));
  destravar();
  for (int fd : fds) { if (fd >= 0) { tab5_web_ws_enviar(fd, d, n, texto); } }
}

// The session spoke (on its own task): the raw data goes to every tab; "state
// changed" (d = nullptr) becomes the JSON.
static void da_sessao(const char* d, size_t n, void*)
{
  if (d) { mandar_a_todos(d, n, false); return; }
  char js[480];
  const int k = estado_json(js, sizeof(js));
  mandar_a_todos(js, (size_t)k, true);
}

// A tab joined: the state and, with the session open, the screen snapshot.
static void receber_aba(int fd)
{
  bool nova = true;
  travar();
  for (int f : s_fds) { nova &= f != fd; }
  if (nova) { for (int& f : s_fds) { if (f < 0) { f = fd; break; } } }
  if (s_assinatura < 0) { s_assinatura = tab5_ssh_sessao_assinar(da_sessao, nullptr); }
  destravar();
  char js[480];
  const int k = estado_json(js, sizeof(js));
  tab5_web_ws_enviar(fd, js, (size_t)k, true);
  char* r = (char*)heap_caps_malloc(RETRATO_MAX, MALLOC_CAP_SPIRAM);
  if (r) {
    const size_t n = tab5_ssh_sessao_retrato(r, RETRATO_MAX);
    if (n) { tab5_web_ws_enviar(fd, r, n, false); }
    heap_caps_free(r);
  }
}

static void aba_fechou(int fd)
{
  int assin = -1;
  travar();
  bool resta = false;
  for (int& f : s_fds) { if (f == fd) { f = -1; } resta |= f >= 0; }
  if (!resta) { assin = s_assinatura; s_assinatura = -1; }
  destravar();
  if (assin >= 0) { tab5_ssh_sessao_desassinar(assin); }   // the session carries on: just nobody watching from the PC
}

static int num(cJSON* j, const char* k, int padrao)
{
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsNumber(v) ? v->valueint : padrao;
}
static const char* txt(cJSON* j, const char* k)
{
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}

// A control message from the page.
static void controle(int fd, const char* s)
{
  cJSON* j = cJSON_Parse(s);
  if (!j) { return; }
  const char* t = txt(j, "t");
  if (!strcmp(t, "ola")) {
    receber_aba(fd);
  } else if (!strcmp(t, "tamanho")) {
    tab5_ssh_sessao_tamanho(num(j, "c", 80), num(j, "l", 24));
  } else if (!strcmp(t, "senha")) {
    tab5_ssh_sessao_senha(txt(j, "s"));
  } else if (!strcmp(t, "confirmar")) {
    tab5_ssh_sessao_confirmar(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "sim")));
  } else if (!strcmp(t, "fechar")) {
    tab5_ssh_sessao_fechar();
  }
  // The password does not stay on the heap: zero the text before releasing it.
  if (!strcmp(t, "senha")) {
    cJSON* sv = cJSON_GetObjectItemCaseSensitive(j, "s");
    if (cJSON_IsString(sv)) { memset(sv->valuestring, 0, strlen(sv->valuestring)); }
  }
  cJSON_Delete(j);
}

// ONE WebSocket FRAME: keys (binary) or control (text).
static esp_err_t ws(httpd_req_t* req)
{
  httpd_ws_frame_t q = {};
  if (httpd_ws_recv_frame(req, &q, 0) != ESP_OK) { return ESP_FAIL; }
  if (q.len == 0 || q.len > 8192) { return q.len ? ESP_FAIL : ESP_OK; }
  uint8_t* b = (uint8_t*)heap_caps_malloc(q.len + 1, MALLOC_CAP_SPIRAM);
  if (!b) { return ESP_FAIL; }
  q.payload = b;
  esp_err_t r = httpd_ws_recv_frame(req, &q, q.len);
  if (r == ESP_OK) {
    b[q.len] = 0;
    if (q.type == HTTPD_WS_TYPE_BINARY)    { tab5_ssh_sessao_escrever(b, q.len); }
    else if (q.type == HTTPD_WS_TYPE_TEXT) { controle(httpd_req_to_sockfd(req), (const char*)b); }
  }
  memset(b, 0, q.len);   // it could be the password
  heap_caps_free(b);
  return r;
}

// ---------------------------------------------------------------------------
// THE COMMANDS (the services/tab5_terminal session)
// ---------------------------------------------------------------------------
// What changed since 'desde' (absolute line number). With no change since the
// browser's version 'v', only {"v":...}: the page polls every fraction of a
// second and there is almost never anything. The last line it has comes again
// (it could still grow).
static esp_err_t console(httpd_req_t* req)
{
  char p[16];
  const uint32_t v = tab5_terminal_versao();
  const uint32_t v_dele = tab5_web_param(req, "v", p, sizeof(p)) ? (uint32_t)strtoul(p, nullptr, 10) : v + 1;
  if (v_dele == v) {
    char js[32];
    snprintf(js, sizeof(js), "{\"v\":%lu}", (unsigned long)v);
    return tab5_web_json(req, js);
  }
  char* out = (char*)heap_caps_malloc(CONSOLE_MAX, MALLOC_CAP_SPIRAM);
  char* esc = (char*)heap_caps_malloc(TERM_LINHA_MAX * 6 + 4, MALLOC_CAP_SPIRAM);
  if (!out || !esc) {
    heap_caps_free(out);
    heap_caps_free(esc);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }
  const uint32_t primeira = tab5_terminal_primeira();
  uint32_t de = tab5_web_param(req, "desde", p, sizeof(p)) ? (uint32_t)strtoul(p, nullptr, 10) : primeira;
  if (de < primeira) { de = primeira; }
  char prompt[TERM_ENTRADA + 180], pr[sizeof(prompt) * 2];
  tab5_terminal_prompt(prompt, sizeof(prompt));
  tab5_web_json_texto(pr, sizeof(pr), prompt);
  bool oculto = false;
  const bool lendo = tab5_terminal_lendo(&oculto);
  size_t k = (size_t)snprintf(out, CONSOLE_MAX,
                              "{\"v\":%lu,\"primeira\":%lu,\"de\":%lu,\"prompt\":%s,\"lendo\":%s,\"oculto\":%s,"
                              "\"ocupado\":%s,\"linhas\":[",
                              (unsigned long)v, (unsigned long)primeira, (unsigned long)de, pr,
                              lendo ? "true" : "false", oculto ? "true" : "false",
                              tab5_terminal_ocupado() ? "true" : "false");
  char t[TERM_LINHA_MAX + 1];
  term_tipo_t tipo;
  bool mais = false;
  int n = 0;
  for (uint32_t a = de; tab5_terminal_linha_abs(a, t, sizeof(t), &tipo) >= 0; ++a, ++n) {
    tab5_web_json_texto(esc, TERM_LINHA_MAX * 6 + 4, t);
    if (n >= CONSOLE_LIN || k + strlen(esc) + 16 >= CONSOLE_MAX) { mais = true; break; }
    k += (size_t)snprintf(out + k, CONSOLE_MAX - k, "%s[%d,%s]", n ? "," : "", (int)tipo, esc);
  }
  snprintf(out + k, CONSOLE_MAX - k, "],\"mais\":%s}", mais ? "true" : "false");
  const esp_err_t r = tab5_web_json(req, out);
  heap_caps_free(esc);
  heap_caps_free(out);
  return r;
}

// A line typed in the browser: the same path as the screen (a new command, or
// the answer to a command that prompts - the password included, which is not echoed).
static esp_err_t enviar(httpd_req_t* req)
{
  char linha[TERM_ENTRADA];
  if (tab5_web_corpo(req, linha, sizeof(linha)) < 0) { return ESP_FAIL; }
  linha[strcspn(linha, "\r\n")] = 0;
  const bool ok = tab5_terminal_enviar(linha);
  memset(linha, 0, sizeof(linha));
  return tab5_web_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"msg\":\"the queue is full\"}");
}

static esp_err_t parar(httpd_req_t* req)  { tab5_terminal_interromper(); return tab5_web_json(req, "{\"ok\":true}"); }
static esp_err_t limpar(httpd_req_t* req) { tab5_terminal_limpar();      return tab5_web_json(req, "{\"ok\":true}"); }

static const tab5_web_rota_t ROTAS[] = {
  { "/",              HTTP_GET,  pagina,    false, nullptr },
  { "/xterm.js",      HTTP_GET,  xterm_js,  false, nullptr },
  { "/xterm.css",     HTTP_GET,  xterm_css, false, nullptr },
  { "/addon-fit.js",  HTTP_GET,  fit_js,    false, nullptr },
  { "/api/console",   HTTP_GET,  console,   false, nullptr },
  { "/api/enviar",    HTTP_POST, enviar,    false, nullptr },
  { "/api/parar",     HTTP_POST, parar,     false, nullptr },
  { "/api/limpar",    HTTP_POST, limpar,    false, nullptr },
  { "/ws",            HTTP_GET,  ws,        true,  aba_fechou },
};

bool tab5_terminal_web_ligar(void)
{
  if (!s_trava) { s_trava = xSemaphoreCreateMutex(); }
  if (!s_trava || !tab5_terminal_abrir()) { return false; }
  ESP_LOGI(TAG, "terminal in the browser: /terminal/");
  return tab5_web_abrir("Terminal", "terminal", ROTAS, sizeof(ROTAS) / sizeof(ROTAS[0]));
}
