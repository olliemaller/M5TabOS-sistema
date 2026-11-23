// Servidor web do sistema - ver tab5_web.hpp.
#include "tab5_web.hpp"

#include <atomic>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <unistd.h>   // close (o socket que o httpd entrega no close_fn)
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

#define ROTAS_MAX 12   // por parte

typedef enum { WEB_PARADO = 0, WEB_SUBINDO, WEB_LIGADO, WEB_PARANDO } estado_t;

// UMA PARTE = um app aberto no servidor. A tabela e trocada pela interface
// (abrir/fechar), pelo relogio (prazo) e pela parada; o despacho a le na
// tarefa do httpd. Tudo sob s_trava, e o despacho so COPIA o que precisa
// (as rotas sao vetores estaticos de quem abriu: o ponteiro vale sempre).
typedef struct {
  const char*            dono;      // nullptr = vaga livre
  char                   prefixo[16];
  const tab5_web_rota_t* rotas;
  int                    n;
  uint32_t               ultimo_ms; // ultimo pedido a ESTA parte (o prazo e por parte)
  uint32_t               ordem;     // quando abriu (a raiz lista nesta ordem)
} parte_t;

static std::atomic<int>       s_estado{WEB_PARADO};
static httpd_handle_t         s_srv = nullptr;
static parte_t                s_partes[WEB_PARTES_MAX];
static uint32_t               s_ordem = 0;
static const char*            s_ultimo_dono = nullptr;   // o aviso da barra abre este
static SemaphoreHandle_t      s_trava = nullptr;         // criada no primeiro abrir (heap interno)
static char                   s_senha[8];
static int                    s_erros = 0;          // senhas erradas neste ligar (so a tarefa do httpd)
static std::atomic<uint32_t>  s_ultimo_ms{0};
static char                   s_ultimo_ip[TAB5_REDE_IP_TXT];
static esp_timer_handle_t     s_relogio = nullptr;
static int                    s_notif = -1;
static TaskHandle_t           s_tarefa = nullptr;   // a do httpd (registrada no tab5_donos)
static char                   s_donos_txt[64];      // "Editor, Mesh": o dono da tarefa no Tarefas

// AS CONEXOES WEBSOCKET abertas: de que parte cada uma e (o despacho dos
// quadros e o prazo). Sob s_trava.
#define WS_MAX 6
typedef struct {
  int         fd;                               // -1 = livre
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
// O PORTEIRO
// ---------------------------------------------------------------------------
// Quem pede, em texto; true = da rede local (a sub-rede do IPv4 do aparelho,
// ou IPv6 link-local).
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
    memcpy(&ip4, b + 12, 4);   // IPv4 numa conexao IPv6 (::ffff:a.b.c.d)
  } else {
    return false;
  }
  inet_ntop(AF_INET, &ip4, ip, n);
  tab5_rede_ip_t eu;
  if (!tab5_rede_ip(&eu) || !eu.conectado) { return false; }
  const uint32_t meu = inet_addr(eu.ip), mascara = inet_addr(eu.mascara);
  return mascara && (ip4 & mascara) == (meu & mascara);
}

// O Host e o IP do aparelho (com ou sem :80), ou um IPv6 entre colchetes.
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

// Comparacao sem atalho: o tempo da resposta nao diz quantas letras acertou.
static bool iguais(const char* a, const char* b)
{
  const size_t na = strlen(a), nb = strlen(b);
  uint8_t d = na != nb;
  for (size_t i = 0; i < na && i < nb; ++i) { d |= (uint8_t)(a[i] ^ b[i]); }
  return d == 0;
}

// 1 = senha certa, 0 = nao mandou senha, -1 = mandou errada.
static int autenticado(httpd_req_t* req)
{
  char h[96];
  if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) != ESP_OK) { return 0; }
  if (strncasecmp(h, "Basic ", 6) != 0) { return -1; }
  unsigned char dec[72];
  size_t n = 0;
  if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char*)h + 6, strlen(h + 6)) != 0) { return -1; }
  dec[n] = 0;
  const char* dp = strchr((const char*)dec, ':');   // "usuario:senha" - o usuario nao importa
  return dp && iguais(dp + 1, s_senha) ? 1 : -1;
}

static esp_err_t recusar(httpd_req_t* req, const char* status, const char* msg)
{
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(req, msg);
}

// O caminho do pedido, sem a query: "/mesh/api/msgs".
static void caminho(httpd_req_t* req, char* out, size_t n)
{
  const size_t k = strcspn(req->uri, "?");
  snprintf(out, n, "%.*s", (int)k, req->uri);
}

// A ROTA DE UM CAMINHO ("/mesh/api/msgs"), sob a trava: a parte aberta com
// aquele prefixo e a rota relativa com o metodo (e o tipo: WebSocket ou nao).
// 'so_pasta' = o caminho e so a pasta, sem a barra ("/mesh").
static const tab5_web_rota_t* achar_rota(const char* c, httpd_method_t metodo, bool ws,
                                         const char** dono, bool* so_pasta)
{
  *so_pasta = false;
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    parte_t* p = &s_partes[i];
    if (!p->dono) { continue; }
    const size_t k = strlen(p->prefixo);
    if (c[0] != '/' || strncmp(c + 1, p->prefixo, k) != 0) { continue; }
    const char* resto = c + 1 + k;   // "" , "/" ou "/api/x"
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

// Um QUADRO de uma conexao WebSocket ja aceita: vai a rota da parte dela.
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
  return fn ? fn(req) : ESP_FAIL;   // de ninguem: o servidor fecha
}

// A ORIGEM do pedido e a do proprio Tab5 ("http://<ip>")? E o que segura uma
// pagina de outro site de abrir um WebSocket aqui com a senha que o navegador
// guardou (o WebSocket nao leva cabecalho proprio como o X-Tab5).
static bool origem_certa(httpd_req_t* req)
{
  char o[80], h[64];
  if (httpd_req_get_hdr_value_str(req, "Origin", o, sizeof(o)) != ESP_OK) { return false; }
  if (httpd_req_get_hdr_value_str(req, "Host", h, sizeof(h)) != ESP_OK) { return false; }
  char esperado[80];
  snprintf(esperado, sizeof(esperado), "http://%s", h);
  return !strcmp(o, esperado);
}

// O APERTO DE MAO do WebSocket: o mesmo porteiro (sem a janela de senha - o
// navegador ja autenticou na pagina) mais a origem; aceita, a conexao entra
// na tabela da parte. ESP_FAIL = o servidor recusa e fecha.
static esp_err_t pre_aperto(httpd_req_t* req)
{
  if (s_estado.load() != WEB_LIGADO) { return ESP_FAIL; }
  char ip[TAB5_REDE_IP_TXT];
  if (!da_rede_local(req, ip, sizeof(ip)) || !host_certo(req) || autenticado(req) <= 0 || !origem_certa(req)) {
    ESP_LOGW(TAG, "WebSocket recusado (%s)", ip);
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
  if (!aceito) { ESP_LOGW(TAG, "WebSocket %s: %s", c, r ? "sem vaga" : "nenhuma parte aberta com ele"); return ESP_FAIL; }
  snprintf(s_ultimo_ip, sizeof(s_ultimo_ip), "%s", ip);
  s_ultimo_ms = tab5_agora_ms();
  sistema_avisar(AVISO_WEB);
  ESP_LOGI(TAG, "WebSocket aberto: %s (%s, fd %d)", c, dono, fd);
  return ESP_OK;
}

// Toda conexao que fecha passa aqui (o httpd chama): a WebSocket sai da
// tabela e a parte fica sabendo.
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
  // Conexao WebSocket ja aceita: e um quadro (o porteiro ja passou no aperto de mao).
  const int fd = httpd_req_to_sockfd(req);
  if (httpd_ws_get_fd_info(req->handle, fd) == HTTPD_WS_CLIENT_WEBSOCKET) { return quadro_ws(req, fd); }
  if (s_estado.load() != WEB_LIGADO) { return recusar(req, "503 Service Unavailable", "desligado"); }
  char ip[TAB5_REDE_IP_TXT];
  if (!da_rede_local(req, ip, sizeof(ip))) {
    ESP_LOGW(TAG, "recusado: %s nao e da rede local", ip);
    return recusar(req, "403 Forbidden", "local network only");
  }
  if (!host_certo(req)) { return recusar(req, "403 Forbidden", "open it by the Tab5's IP address"); }
  const int a = autenticado(req);
  if (a <= 0) {
    if (a < 0) {
      // Errou: espera um pouco (a tarefa e uma so - quem tenta adivinhar fica
      // na fila) e, passando do limite, fecha a porta.
      ESP_LOGW(TAG, "senha errada de %s (%d)", ip, s_erros + 1);
      vTaskDelay(pdMS_TO_TICKS(400));
      if (++s_erros >= WEB_ERROS_MAX) {
        ESP_LOGW(TAG, "senhas erradas demais: desligando");
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

  // O DESPACHO: as rotas da raiz (o portal), ou /<prefixo>/<rota relativa>
  // da parte aberta.
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
  if (so_pasta) {   // "/mesh" sem a barra: as rotas da pagina sao relativas a pasta
    char l[40];
    snprintf(l, sizeof(l), "%s/", c);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", l);
    return httpd_resp_send(req, nullptr, 0);
  }
  if (!fn) {
    // A PAGINA de um app fechado (ou de nenhum): o portal, que mostra o que ha
    // e abre com um clique. Chamada de API de parte fechada: 404 de verdade.
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
// ABRIR, FECHAR, LIGAR E DESLIGAR
// ---------------------------------------------------------------------------
// O aviso da barra e o dono da tarefa no Tarefas: os abertos, juntos. Sob a trava.
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

// A cada 15 s: a parte que ninguem pediu nada ha WEB_OCIOSO_MS fecha; o
// Wi-Fi caiu, desliga tudo.
static void ao_relogio(void*)
{
  if (s_estado.load() != WEB_LIGADO) { return; }
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) {
    ESP_LOGI(TAG, "sem Wi-Fi: desligando");
    tab5_web_desligar();
    return;
  }
  const uint32_t agora = tab5_agora_ms();
  const char* fechar[WEB_PARTES_MAX];
  int nf = 0;
  travar();
  for (int i = 0; i < WEB_PARTES_MAX; ++i) {
    if (!s_partes[i].dono || agora - s_partes[i].ultimo_ms <= WEB_OCIOSO_MS) { continue; }
    bool conectada = false;   // pagina aberta com WebSocket: e uso, mesmo sem tecla
    for (int w = 0; w < WS_MAX; ++w) { conectada |= s_ws[w].fd >= 0 && s_ws[w].dono == s_partes[i].dono; }
    if (!conectada) { fechar[nf++] = s_partes[i].dono; }
  }
  destravar();
  for (int i = 0; i < nf; ++i) {
    ESP_LOGI(TAG, "%s sem uso: fechando a parte dele", fechar[i]);
    tab5_web_fechar(fechar[i]);
  }
}

static void sortear_senha(void)
{
  static const char LETRAS[] = "abcdefghjkmnpqrstuvwxyz23456789";   // sem i, l, o, 0, 1
  for (int i = 0; i < 6; ++i) { s_senha[i] = LETRAS[esp_random() % (sizeof(LETRAS) - 1)]; }
  s_senha[6] = 0;
}

// Sobe o httpd: UM manipulador para cada metodo, que pega tudo ("/*") - o
// despacho e do porteiro, pela tabela de partes. Assim abrir e fechar uma
// parte nao mexe no httpd rodando.
static bool subir(void)
{
  s_erros = 0;
  s_ultimo_ip[0] = 0;
  s_ultimo_ms = tab5_agora_ms();
  sortear_senha();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size       = WEB_PILHA;
  cfg.task_priority    = 3;            // abaixo da interface (4)
  cfg.max_uri_handlers = 2;
  cfg.max_open_sockets = 6;            // CONFIG_LWIP_MAX_SOCKETS (16) menos as 3 do httpd, com folga
  cfg.close_fn         = ao_fechar_socket;   // a tabela das WebSocket
  cfg.lru_purge_enable = true;         // o navegador abre varias e larga abertas
  cfg.uri_match_fn     = httpd_uri_match_wildcard;
  if (httpd_start(&s_srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "o httpd nao subiu");
    s_srv = nullptr;
    return false;
  }
  static const httpd_method_t METODOS[2] = { HTTP_GET, HTTP_POST };
  for (httpd_method_t m : METODOS) {
    httpd_uri_t u = {};
    u.uri     = "/*";
    u.method  = m;
    u.handler = porteiro;
    // O GET "pega tudo" e tambem a porta do WebSocket: pedido normal vai ao
    // porteiro; pedido de upgrade passa antes pelo pre_aperto.
    u.is_websocket = (m == HTTP_GET);
    u.ws_pre_handshake_cb = (m == HTTP_GET) ? pre_aperto : nullptr;
    httpd_register_uri_handler(s_srv, &u);
  }
  // NO TAREFAS, como os donos rodando de fundo ("APPS EM FUNDO: Editor,
  // Mesh"), e nao como uma tarefa anonima do sistema. O httpd nao devolve o
  // handle; a tarefa dele se chama "httpd".
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
  // O servidor: de pe, ou sobe agora. Parando (a ultima parte acabou de
  // fechar): nao da para pegar carona - tente de novo em instantes.
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
  if (!p) { ESP_LOGW(TAG, "sem vaga para %s (%d partes)", dono, WEB_PARTES_MAX); return false; }
  sistema_avisar(AVISO_WEB);
  ESP_LOGI(TAG, "aberto: %s em /%s/ (%d rotas)", dono, prefixo, n);
  return true;
}

static void parar(void*)
{
  if (s_relogio) { esp_timer_stop(s_relogio); }
  if (s_tarefa) { tab5_dono_esquecer(s_tarefa); s_tarefa = nullptr; }   // antes de ela morrer
  if (s_srv) { httpd_stop(s_srv); s_srv = nullptr; }   // espera o pedido em curso
  s_senha[0] = 0;
  travar();
  memset(s_partes, 0, sizeof(s_partes));
  s_ultimo_dono = nullptr;
  publicar();
  destravar();
  sistema_recurso(RECURSO_REDE, "servidor web", false);
  s_estado = WEB_PARADO;
  sistema_avisar(AVISO_WEB);
  ESP_LOGI(TAG, "desligado");
}

void tab5_web_desligar(void)
{
  int esperado = WEB_LIGADO;
  if (!s_estado.compare_exchange_strong(esperado, WEB_PARANDO)) { return; }
  // De dentro de um pedido (senha errada demais) parar ali seria esperar a si
  // mesmo; de qualquer outra tarefa, nao segurar quem chamou. Numa vaga.
  if (!pool_criar(nullptr, parar, nullptr, "web_parar", 3072, 3)) { parar(nullptr); }
}

// Derruba as WebSocket de 'dono' (nullptr = todas). O httpd fecha na tarefa dele.
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
  ESP_LOGI(TAG, "fechado: %s", dono);
  if (!restam) { tab5_web_desligar(); return; }   // a ultima: o servidor cai
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
  // Na ordem em que abriram.
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
// PARA AS ROTAS
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
  // Da propria tarefa do servidor (respondendo a um quadro), o send_data
  // esperaria por ela mesma: ai manda direto no socket.
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
