// System web server - see tab5_web.hpp.
#include "tab5_web.hpp"

#include <atomic>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <unistd.h>   // close (the socket httpd hands over in close_fn)
#include "mbedtls/base64.h"
#include "app.h"
#include "notificacao.h"
#include "pool_tarefas.h"
#include "tab5_donos.hpp"
#include "tab5_hora.hpp"
#include "tab5_rede.hpp"
#include "tab5_wifi.hpp"
#include "tab5_web_portal.hpp"

static const char* TAG = "web";

#define ROTAS_MAX 12   // per part

typedef enum { WEB_PARADO = 0, WEB_SUBINDO, WEB_LIGADO, WEB_PARANDO } estado_t;

// ONE PART = one app open on the server. The table is changed by the UI
// (open/close), by the timer (deadline) and by the stop; dispatch reads it on the
// httpd task. All under s_trava, and dispatch only COPIES what it needs
// (the routes are static arrays of whoever opened: the pointer is always valid).
typedef struct {
  const char*            dono;      // nullptr = free slot
  char                   prefixo[16];
  const tab5_web_rota_t* rotas;
  int                    n;
  uint32_t               ultimo_ms; // last request to THIS part (the deadline is per part)
  uint32_t               ordem;     // when it opened (the root lists in this order)
} parte_t;

static std::atomic<int>       s_estado{WEB_PARADO};
static httpd_handle_t         s_srv = nullptr;
static parte_t                s_partes[WEB_PARTES_MAX];
static uint32_t               s_ordem = 0;
static const char*            s_ultimo_dono = nullptr;   // the status-bar notice opens this one
static SemaphoreHandle_t      s_trava = nullptr;         // created on the first open (internal heap)
static char                   s_senha[8];
static int                    s_erros = 0;          // wrong passwords this session (httpd task only)
static std::atomic<uint32_t>  s_ultimo_ms{0};
static char                   s_ultimo_ip[TAB5_REDE_IP_TXT];
static esp_timer_handle_t     s_relogio = nullptr;
static int                    s_notif = -1;
static TaskHandle_t           s_tarefa = nullptr;   // httpd's (registered in tab5_donos)
static char                   s_donos_txt[64];      // "Editor, Mesh": the task owner in Tasks

// THE OPEN WEBSOCKET CONNECTIONS: which part each belongs to (frame dispatch
// and the deadline). Under s_trava.
#define WS_MAX 6
typedef struct {
  int         fd;                               // -1 = free
  const char* dono;
  esp_err_t (*fn)(httpd_req_t*);
  void      (*fechou)(int fd);
} ws_t;
static ws_t s_ws[WS_MAX] = { { -1, nullptr, nullptr, nullptr }, { -1, nullptr, nullptr, nullptr },
                             { -1, nullptr, nullptr, nullptr }, { -1, nullptr, nullptr, nullptr },
                             { -1, nullptr, nullptr, nullptr }, { -1, nullptr, nullptr, nullptr } };

static void travar(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static void destravar(void) { xSemaphoreGive(s_trava); }

// ---------------------------------------------------------------------------
// THE GATEKEEPER
// ---------------------------------------------------------------------------
// The requester, as text; true = from the local network (the subnet of the device's
// IPv4, or IPv6 link-local).
static bool da_rede_local(httpd_req_t* req, char* ip, size_t n)
{
  ip[0] = 0;
  struct sockaddr_storage a;
  socklen_t l = sizeof(a);
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr*)&a, &l) != 0) { return false; }
  uint32_t ip4 = 0;
  if (a.ss_family == AF_INET) {
    ip4 = ((struct sockaddr_in*)&a)->sin_addr.s_addr;
  } else if (a.ss_family == AF_INET6) {
    const uint8_t* b = ((struct sockaddr_in6*)&a)->sin6_addr.s6_addr;
    static const uint8_t MAPEADO[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (memcmp(b, MAPEADO, 12) != 0) {
      inet_ntop(AF_INET6, b, ip, n);
      return b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
    }
    memcpy(&ip4, b + 12, 4);   // IPv4 over an IPv6 connection (::ffff:a.b.c.d)
  } else {
    return false;
  }
  inet_ntop(AF_INET, &ip4, ip, n);
  tab5_rede_ip_t eu;
  if (!tab5_rede_ip(&eu) || !eu.conectado) { return false; }
  const uint32_t meu = inet_addr(eu.ip), mascara = inet_addr(eu.mascara);
  return mascara && (ip4 & mascara) == (meu & mascara);
}

// The Host is the device's IP (with or without :80), or an IPv6 in brackets.
static bool host_certo(httpd_req_t* req)
{
  char h[64];
  if (httpd_req_get_hdr_value_str(req, "Host", h, sizeof(h)) != ESP_OK) { return false; }
  if (h[0] == '[') { return true; }
  char* dp = strchr(h, ':');
  if (dp) {
    if (strcmp(dp, ":80") != 0) { return false; }
    *dp = 0;
  }
  tab5_rede_ip_t eu;
  return tab5_rede_ip(&eu) && eu.conectado && !strcmp(h, eu.ip);
}

// Comparison without shortcut: the response time does not reveal how many letters matched.
static bool iguais(const char* a, const char* b)
{
  const size_t na = strlen(a), nb = strlen(b);
  uint8_t d = na != nb;
  for (size_t i = 0; i < na && i < nb; ++i) { d |= (uint8_t)(a[i] ^ b[i]); }
  return d == 0;
}

// 1 = correct password, 0 = no password sent, -1 = wrong one sent.
static int autenticado(httpd_req_t* req)
{
  char h[96];
  if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) != ESP_OK) { return 0; }
  if (strncasecmp(h, "Basic ", 6) != 0) { return -1; }
  unsigned char dec[72];
  size_t n = 0;
  if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char*)h + 6, strlen(h + 6)) != 0) { return -1; }
  dec[n] = 0;
  const char* dp = strchr((const char*)dec, ':');   // "user:password" - the user does not matter
  return dp && iguais(dp + 1, s_senha) ? 1 : -1;
}

static esp_err_t recusar(httpd_req_t* req, const char* status, const char* msg)
{
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(req, msg);
}

// The request path, without the query: "/mesh/api/msgs".
static void caminho(httpd_req_t* req, char* out, size_t n)
{
  const size_t k = strcspn(req->uri, "?");
  snprintf(out, n, "%.*s", (int)k, req->uri);
}

// THE ROUTE FOR A PATH ("/mesh/api/msgs"), under the lock: the open part with
// that prefix and the relative route with the method (and the type: WebSocket or not).
// 'so_pasta' = the path is just the folder, without the slash ("/mesh").
static const tab5_web_rota_t* achar_rota(const char* c, httpd_method_t metodo, bool ws,
                                         const char** dono, bool* so_pasta)
{
  *so_pasta = false;
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    parte_t* p = &s_partes[i];
    if (!p->dono) { continue; }
    const size_t k = strlen(p->prefixo);
    if (c[0] != '/' || strncmp(c + 1, p->prefixo, k) != 0) { continue; }
    const char* resto = c + 1 + k;   // "" , "/" or "/api/x"
    if (!resto[0]) { *so_pasta = true; return nullptr; }
    if (resto[0] != '/') { continue; }
    for (int r = 0; r < p->n; ++r) {
      if (p->rotas[r].metodo == metodo && p->rotas[r].ws == ws && !strcmp(p->rotas[r].uri, resto)) {
        p->ultimo_ms = tab5_agora_ms();
        *dono = p->dono;
        return &p->rotas[r];
      }
    }
  }
  return nullptr;
}

// A FRAME from an already accepted WebSocket connection: goes to its part's route.
static esp_err_t quadro_ws(httpd_req_t* req, int fd)
{
  esp_err_t (*fn)(httpd_req_t*) = nullptr;
  travar();
  for (int i = 0; i < WS_MAX; ++i) {
    if (s_ws[i].fd != fd) { continue; }
    fn = s_ws[i].fn;
    for (int k = 0; k < WEB_PARTES_MAX; ++k) {
      if (s_partes[k].dono == s_ws[i].dono) { s_partes[k].ultimo_ms = tab5_agora_ms(); }
    }
  }
  destravar();
  return fn ? fn(req) : ESP_FAIL;   // nobody's: the server closes it
}

// Is the request ORIGIN the Tab5's own ("http://<ip>")? That is what stops a
// page from another site opening a WebSocket here with the password the browser
// stored (WebSocket does not carry its own header like X-Tab5).
static bool origem_certa(httpd_req_t* req)
{
  char o[80], h[64];
  if (httpd_req_get_hdr_value_str(req, "Origin", o, sizeof(o)) != ESP_OK) { return false; }
  if (httpd_req_get_hdr_value_str(req, "Host", h, sizeof(h)) != ESP_OK) { return false; }
  char esperado[80];
  snprintf(esperado, sizeof(esperado), "http://%s", h);
  return !strcmp(o, esperado);
}

// The WebSocket HANDSHAKE: the same gatekeeper (without the password dialog - the
// browser already authenticated on the page) plus the origin; once accepted, the
// connection goes into the part's table. ESP_FAIL = the server refuses and closes.
static esp_err_t pre_aperto(httpd_req_t* req)
{
  if (s_estado.load() != WEB_LIGADO) { return ESP_FAIL; }
  char ip[TAB5_REDE_IP_TXT];
  if (!da_rede_local(req, ip, sizeof(ip)) || !host_certo(req) || autenticado(req) <= 0 || !origem_certa(req)) {
    ESP_LOGW(TAG, "WebSocket refused (%s)", ip);
    return ESP_FAIL;
  }
  char c[160];
  caminho(req, c, sizeof(c));
  const int fd = httpd_req_to_sockfd(req);
  bool aceito = false, so_pasta = false;
  const char* dono = nullptr;
  travar();
  const tab5_web_rota_t* r = achar_rota(c, HTTP_GET, true, &dono, &so_pasta);
  for (int i = 0; r && i < WS_MAX && !aceito; ++i) {
    if (s_ws[i].fd >= 0) { continue; }
    s_ws[i] = { fd, dono, r->fn, r->ws_fechou };
    aceito = true;
  }
  destravar();
  if (!aceito) { ESP_LOGW(TAG, "WebSocket %s: %s", c, r ? "no free slot" : "no open part has it"); return ESP_FAIL; }
  snprintf(s_ultimo_ip, sizeof(s_ultimo_ip), "%s", ip);
  s_ultimo_ms = tab5_agora_ms();
  sistema_avisar(AVISO_WEB);
  ESP_LOGI(TAG, "WebSocket open: %s (%s, fd %d)", c, dono, fd);
  return ESP_OK;
}

// Every connection that closes comes through here (httpd calls it): a WebSocket leaves
// the table and the part is told.
static void ao_fechar_socket(httpd_handle_t, int fd)
{
  void (*fechou)(int) = nullptr;
  if (s_trava) {
    travar();
    for (int i = 0; i < WS_MAX; ++i) {
      if (s_ws[i].fd == fd) { fechou = s_ws[i].fechou; s_ws[i] = { -1, nullptr, nullptr, nullptr }; }
    }
    destravar();
  }
  if (fechou) { fechou(fd); }
  close(fd);
}

static esp_err_t porteiro(httpd_req_t* req)
{
  // Already accepted WebSocket connection: this is a frame (the gatekeeper ran at the handshake).
  const int fd = httpd_req_to_sockfd(req);
  if (httpd_ws_get_fd_info(req->handle, fd) == HTTPD_WS_CLIENT_WEBSOCKET) { return quadro_ws(req, fd); }
  if (s_estado.load() != WEB_LIGADO) { return recusar(req, "503 Service Unavailable", "desligado"); }
  char ip[TAB5_REDE_IP_TXT];
  if (!da_rede_local(req, ip, sizeof(ip))) {
    ESP_LOGW(TAG, "refused: %s is not on the local network", ip);
    return recusar(req, "403 Forbidden", "local network only");
  }
  if (!host_certo(req)) { return recusar(req, "403 Forbidden", "open it by the Tab5's IP address"); }
  const int a = autenticado(req);
  if (a <= 0) {
    if (a < 0) {
      // Wrong: wait a little (there is only one task - whoever tries guessing
      // waits in the queue) and, past the limit, close the door.
      ESP_LOGW(TAG, "wrong password from %s (%d)", ip, s_erros + 1);
      vTaskDelay(pdMS_TO_TICKS(400));
      if (++s_erros >= WEB_ERROS_MAX) {
        ESP_LOGW(TAG, "too many wrong passwords: turning off");
        tab5_web_desligar();
        return recusar(req, "403 Forbidden", "desligado");
      }
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tab5\", charset=\"UTF-8\"");
    return recusar(req, "401 Unauthorized", "password: the one shown on the Tab5 screen");
  }
  if (req->method != HTTP_GET && httpd_req_get_hdr_value_len(req, "X-Tab5") == 0) {
    return recusar(req, "403 Forbidden", "falta o cabecalho X-Tab5");
  }
  snprintf(s_ultimo_ip, sizeof(s_ultimo_ip), "%s", ip);
  s_ultimo_ms = tab5_agora_ms();

  // DISPATCH: the root routes (the portal), or /<prefixo>/<relative route>
  // of the open part.
  char c[160];
  caminho(req, c, sizeof(c));
  int nr = 0;
  const tab5_web_rota_t* raiz = tab5_web_portal_rotas(&nr);
  for (int i = 0; i < nr; ++i) {
    if (raiz[i].metodo == (httpd_method_t)req->method && !strcmp(raiz[i].uri, c)) {
      sistema_avisar(AVISO_WEB);
      return raiz[i].fn(req);
    }
  }
  bool so_pasta = false;
  const char* dono = nullptr;
  travar();
  const tab5_web_rota_t* rota = achar_rota(c, (httpd_method_t)req->method, false, &dono, &so_pasta);
  esp_err_t (*fn)(httpd_req_t*) = rota ? rota->fn : nullptr;
  destravar();
  if (so_pasta) {   // "/mesh" without the slash: the page routes are relative to the folder
    char l[40];
    snprintf(l, sizeof(l), "%s/", c);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", l);
    return httpd_resp_send(req, nullptr, 0);
  }
  if (!fn) {
    // The PAGE of a closed app (or of none): the portal, which shows what there is
    // and opens it with one click. An API call to a closed part: a real 404.
    if (req->method == HTTP_GET && !strstr(c, "/api/") && !strstr(c, "/ws")) {
      httpd_resp_set_status(req, "302 Found");
      httpd_resp_set_hdr(req, "Location", "/");
      return httpd_resp_send(req, nullptr, 0);
    }
    return recusar(req, "404 Not Found", "that app is not open on the Tab5");
  }
  sistema_avisar(AVISO_WEB);
  return fn(req);
}

// ---------------------------------------------------------------------------
// OPEN, CLOSE, START AND STOP
// ---------------------------------------------------------------------------
// The status-bar notice and the task owner in Tasks: the open ones, together. Under the lock.
static void publicar(void)
{
  if (s_notif < 0) {
    s_notif = notif_registrar("Servidor web");
  }
  int n = 0;
  size_t k = 0;
  s_donos_txt[0] = 0;
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    if (!s_partes[i].dono) { continue; }
    k += snprintf(s_donos_txt + k, sizeof(s_donos_txt) - k, "%s%s", n ? ", " : "", s_partes[i].dono);
    if (k >= sizeof(s_donos_txt)) { k = sizeof(s_donos_txt) - 1; }
    ++n;
  }
  if (s_tarefa && n) { tab5_dono_registrar(s_tarefa, DONO_FUNDO, s_donos_txt); }
  if (s_notif < 0) { return; }
  if (!n) { notif_limpar(s_notif); return; }
  char r[NOTIF_RESUMO];
  snprintf(r, sizeof(r), "%s", s_donos_txt);
  notif_associar_app(s_notif, s_ultimo_dono);
  notif_publicar(s_notif, n, NOTIF_INFO, r);
}

// Every 15 s: a part nobody has asked anything of for WEB_OCIOSO_MS closes; if
// Wi-Fi dropped, turn everything off.
static void ao_relogio(void*)
{
  if (s_estado.load() != WEB_LIGADO) { return; }
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) {
    ESP_LOGI(TAG, "no Wi-Fi: turning off");
    tab5_web_desligar();
    return;
  }
  const uint32_t agora = tab5_agora_ms();
  const char* fechar[WEB_PARTES_MAX];
  int nf = 0;
  travar();
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    if (!s_partes[i].dono || agora - s_partes[i].ultimo_ms <= WEB_OCIOSO_MS) { continue; }
    bool conectada = false;   // page open with a WebSocket: that is use, even without keystrokes
    for (int w = 0; w < WS_MAX; ++w) { conectada |= s_ws[w].fd >= 0 && s_ws[w].dono == s_partes[i].dono; }
    if (!conectada) { fechar[nf++] = s_partes[i].dono; }
  }
  destravar();
  for (int i = 0; i < nf; ++i) {
    ESP_LOGI(TAG, "%s unused: closing its part", fechar[i]);
    tab5_web_fechar(fechar[i]);
  }
}

static void sortear_senha(void)
{
  static const char LETRAS[] = "abcdefghjkmnpqrstuvwxyz23456789";   // no i, l, o, 0, 1
  for (int i = 0; i < 6; ++i) { s_senha[i] = LETRAS[esp_random() % (sizeof(LETRAS) - 1)]; }
  s_senha[6] = 0;
}

// Starts httpd: ONE handler per method, catching everything ("/*") - dispatch
// is done by the gatekeeper, via the parts table. That way opening and closing a
// part does not touch the running httpd.
static bool subir(void)
{
  s_erros = 0;
  s_ultimo_ip[0] = 0;
  s_ultimo_ms = tab5_agora_ms();
  sortear_senha();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size       = WEB_PILHA;
  cfg.task_priority    = 3;            // below the UI (4)
  cfg.max_uri_handlers = 2;
  cfg.max_open_sockets = 6;            // CONFIG_LWIP_MAX_SOCKETS (16) minus httpd's 3, with headroom
  cfg.close_fn         = ao_fechar_socket;   // the WebSocket table
  cfg.lru_purge_enable = true;         // the browser opens several and leaves them open
  cfg.uri_match_fn     = httpd_uri_match_wildcard;
  if (httpd_start(&s_srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd did not start");
    s_srv = nullptr;
    return false;
  }
  static const httpd_method_t METODOS[2] = { HTTP_GET, HTTP_POST };
  for (httpd_method_t m : METODOS) {
    httpd_uri_t u = {};
    u.uri     = "/*";
    u.method  = m;
    u.handler = porteiro;
    // The catch-all GET is also the WebSocket door: a normal request goes to the
    // gatekeeper; an upgrade request goes through pre_aperto first.
    u.is_websocket = (m == HTTP_GET);
    u.ws_pre_handshake_cb = (m == HTTP_GET) ? pre_aperto : nullptr;
    httpd_register_uri_handler(s_srv, &u);
  }
  // IN TASKS, like the owners running in the background ("APPS EM FUNDO: Editor,
  // Mesh"), and not as an anonymous system task. httpd does not return the
  // handle; its task is called "httpd".
  s_tarefa = xTaskGetHandle("httpd");
  if (!s_relogio) {
    const esp_timer_create_args_t a = { .callback = ao_relogio, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "web",
                                        .skip_unhandled_events = true };
    esp_timer_create(&a, &s_relogio);
  }
  if (s_relogio) { esp_timer_start_periodic(s_relogio, 15 * 1000 * 1000); }
  sistema_recurso(RECURSO_REDE, "servidor web", true);
  return true;
}

bool tab5_web_abrir(const char* dono, const char* prefixo, const tab5_web_rota_t* rotas, int n)
{
  if (!dono || !prefixo || n > ROTAS_MAX || tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return false; }
  if (!s_trava) { s_trava = xSemaphoreCreateMutex(); }
  if (!s_trava) { return false; }
  // The server: up, or starts now. Stopping (the last part has just
  // closed): cannot hitch a ride - try again in a moment.
  int esperado = WEB_PARADO;
  if (s_estado.compare_exchange_strong(esperado, WEB_SUBINDO)) {
    if (!subir()) { s_estado = WEB_PARADO; return false; }
    s_estado = WEB_LIGADO;
  } else if (esperado != WEB_LIGADO) {
    return false;
  }
  travar();
  parte_t* p = nullptr;
  for (int i = 0; i < WEB_PARTES_MAX && !p; ++i) { if (s_partes[i].dono && !strcmp(s_partes[i].dono, dono)) { p = &s_partes[i]; } }
  for (int i = 0; i < WEB_PARTES_MAX && !p; ++i) { if (!s_partes[i].dono) { p = &s_partes[i]; } }
  if (p) {
    snprintf(p->prefixo, sizeof(p->prefixo), "%s", prefixo);
    p->rotas = rotas;
    p->n = n;
    p->ultimo_ms = tab5_agora_ms();
    if (!p->dono) { p->ordem = ++s_ordem; }
    p->dono = dono;
    s_ultimo_dono = dono;
    publicar();
  }
  destravar();
  if (!p) { ESP_LOGW(TAG, "no free slot for %s (%d parts)", dono, WEB_PARTES_MAX); return false; }
  sistema_avisar(AVISO_WEB);
  ESP_LOGI(TAG, "opened: %s at /%s/ (%d routes)", dono, prefixo, n);
  return true;
}

static void parar(void*)
{
  if (s_relogio) { esp_timer_stop(s_relogio); }
  if (s_tarefa) { tab5_dono_esquecer(s_tarefa); s_tarefa = nullptr; }   // before it dies
  if (s_srv) { httpd_stop(s_srv); s_srv = nullptr; }   // waits for the request in progress
  s_senha[0] = 0;
  travar();
  memset(s_partes, 0, sizeof(s_partes));
  s_ultimo_dono = nullptr;
  publicar();
  destravar();
  sistema_recurso(RECURSO_REDE, "servidor web", false);
  s_estado = WEB_PARADO;
  sistema_avisar(AVISO_WEB);
  ESP_LOGI(TAG, "turned off");
}

void tab5_web_desligar(void)
{
  int esperado = WEB_LIGADO;
  if (!s_estado.compare_exchange_strong(esperado, WEB_PARANDO)) { return; }
  // From inside a request (too many wrong passwords) stopping there would mean waiting
  // for itself; from any other task, do not hold up the caller. On a pool slot.
  if (!pool_criar(nullptr, parar, nullptr, "web_parar", 3072, 3)) { parar(nullptr); }
}

// Drops the WebSockets of 'dono' (nullptr = all). httpd closes them on its task.
static void fechar_ws_de(const char* dono)
{
  int fds[WS_MAX], n = 0;
  travar();
  for (int i = 0; i < WS_MAX; ++i) {
    if (s_ws[i].fd >= 0 && (!dono || (s_ws[i].dono && !strcmp(s_ws[i].dono, dono)))) { fds[n++] = s_ws[i].fd; }
  }
  destravar();
  for (int i = 0; i < n && s_srv; ++i) { httpd_sess_trigger_close(s_srv, fds[i]); }
}

void tab5_web_fechar(const char* dono)
{
  if (!dono || !s_trava || s_estado.load() != WEB_LIGADO) { return; }
  int restam = 0;
  bool achou = false;
  travar();
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    if (s_partes[i].dono && !strcmp(s_partes[i].dono, dono)) { s_partes[i].dono = nullptr; achou = true; }
    else if (s_partes[i].dono) { ++restam; if (s_partes[i].ordem > 0) { s_ultimo_dono = s_partes[i].dono; } }
  }
  if (achou && restam) { publicar(); }
  destravar();
  if (!achou) { return; }
  fechar_ws_de(dono);
  ESP_LOGI(TAG, "closed: %s", dono);
  if (!restam) { tab5_web_desligar(); return; }   // the last one: the server goes down
  sistema_avisar(AVISO_WEB);
}

bool tab5_web_ligado(void) { return s_estado.load() == WEB_LIGADO; }

bool tab5_web_aberto(const char* dono)
{
  if (!dono || !s_trava || !tab5_web_ligado()) { return false; }
  bool sim = false;
  travar();
  for (int i = 0; i < WEB_PARTES_MAX && !sim; ++i) { sim = s_partes[i].dono && !strcmp(s_partes[i].dono, dono); }
  destravar();
  return sim;
}

int tab5_web_abertos(const char** out, int max)
{
  if (!s_trava || !tab5_web_ligado()) { return 0; }
  int n = 0;
  travar();
  // In the order they opened.
  for (uint32_t o = 1; o <= s_ordem && n < max; ++o) {
    for (int i = 0; i < WEB_PARTES_MAX; ++i) { if (s_partes[i].dono && s_partes[i].ordem == o) { out[n++] = s_partes[i].dono; } }
  }
  destravar();
  return n;
}

bool tab5_web_endereco(const char* dono, char* out, size_t n)
{
  tab5_rede_ip_t eu;
  if (!tab5_rede_ip(&eu) || !eu.conectado) { out[0] = 0; return false; }
  char pre[16] = "";
  if (dono && s_trava) {
    travar();
    for (int i = 0; i < WEB_PARTES_MAX; ++i) {
      if (s_partes[i].dono && !strcmp(s_partes[i].dono, dono)) { snprintf(pre, sizeof(pre), "%s/", s_partes[i].prefixo); }
    }
    destravar();
  }
  snprintf(out, n, "http://%s/%s", eu.ip, pre);
  return true;
}

bool tab5_web_pasta(const char* dono, char* out, size_t n)
{
  out[0] = 0;
  if (!dono || !s_trava || !tab5_web_ligado()) { return false; }
  travar();
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    if (s_partes[i].dono && !strcmp(s_partes[i].dono, dono)) { snprintf(out, n, "%s", s_partes[i].prefixo); }
  }
  destravar();
  return out[0] != 0;
}

void tab5_web_senha(char* out, size_t n) { snprintf(out, n, "%s", tab5_web_ligado() ? s_senha : ""); }

void tab5_web_ultimo(char* quem, size_t n, uint32_t* quando_ms)
{
  snprintf(quem, n, "%s", s_ultimo_ip);
  if (quando_ms) { *quando_ms = s_ultimo_ms.load(); }
}

// ---------------------------------------------------------------------------
// FOR THE ROUTES
// ---------------------------------------------------------------------------
bool tab5_web_ws_enviar(int fd, const void* d, size_t n, bool texto)
{
  httpd_handle_t srv = s_srv;
  if (!srv || s_estado.load() != WEB_LIGADO) { return false; }
  httpd_ws_frame_t q = {};
  q.final   = true;
  q.type    = texto ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY;
  q.payload = (uint8_t*)d;
  q.len     = n;
  // From the server's own task (answering a frame), send_data would
  // wait for itself: in that case send straight to the socket.
  if (s_tarefa && xTaskGetCurrentTaskHandle() == s_tarefa) { return httpd_ws_send_frame_async(srv, fd, &q) == ESP_OK; }
  return httpd_ws_send_data(srv, fd, &q) == ESP_OK;
}

int tab5_web_corpo(httpd_req_t* req, char* buf, size_t tam)
{
  if (req->content_len >= tam) {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "grande demais");
    return -1;
  }
  size_t lido = 0;
  while (lido < req->content_len) {
    const int r = httpd_req_recv(req, buf + lido, req->content_len - lido);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) { continue; }
    if (r <= 0) { return -1; }
    lido += r;
  }
  buf[lido] = 0;
  return (int)lido;
}

static int hex(char c)
{
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

bool tab5_web_param(httpd_req_t* req, const char* nome, char* out, size_t n)
{
  char q[256], v[192];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) { return false; }
  if (httpd_query_key_value(q, nome, v, sizeof(v)) != ESP_OK) { return false; }
  size_t k = 0;
  for (const char* p = v; *p && k + 1 < n; ++p) {
    if (*p == '+') { out[k++] = ' '; continue; }
    if (*p == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) { out[k++] = (char)(hex(p[1]) * 16 + hex(p[2])); p += 2; continue; }
    out[k++] = *p;
  }
  out[k] = 0;
  return true;
}

esp_err_t tab5_web_json(httpd_req_t* req, const char* json)
{
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, json);
}

void tab5_web_json_texto(char* out, size_t n, const char* s)
{
  size_t k = 0;
  if (n < 3) { if (n) { out[0] = 0; } return; }
  out[k++] = '"';
  for (; *s; ++s) {
    const unsigned char c = (unsigned char)*s;
    char e[8];
    if (c == '"' || c == '\\')  { snprintf(e, sizeof(e), "\\%c", c); }
    else if (c == '\n')         { snprintf(e, sizeof(e), "\\n"); }
    else if (c == '\t')         { snprintf(e, sizeof(e), "\\t"); }
    else if (c < 0x20)          { snprintf(e, sizeof(e), "\\u%04x", c); }
    else                        { e[0] = (char)c; e[1] = 0; }
    const size_t l = strlen(e);
    if (k + l + 2 > n) { break; }
    memcpy(out + k, e, l);
    k += l;
  }
  out[k++] = '"';
  out[k] = 0;
}
