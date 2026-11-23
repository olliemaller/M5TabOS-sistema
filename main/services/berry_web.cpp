// The Berry editor in the PC's browser - see berry_web.hpp.
#include "berry_web.hpp"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app.h"
#include "tab5_web.hpp"
#include "berry_vm.hpp"
#include "berry_apps.hpp"
#include "berry_scripts.hpp"

static const char* TAG = "berry_web";

extern const char PAGINA_INICIO[] asm("_binary_berry_web_html_start");
extern const char PAGINA_FIM[]    asm("_binary_berry_web_html_end");

#define LISTA_MAX 64

static esp_err_t pagina(httpd_req_t* req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PAGINA_INICIO, PAGINA_FIM - PAGINA_INICIO - 1);   // TEXT adds a '\0' at the end
}

static esp_err_t sem_memoria(httpd_req_t* req)
{
  return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
}

static esp_err_t lista(httpd_req_t* req)
{
  tab5_arq_item_t* it = (tab5_arq_item_t*)heap_caps_malloc(LISTA_MAX * sizeof(tab5_arq_item_t), MALLOC_CAP_SPIRAM);
  char* js = (char*)heap_caps_malloc(LISTA_MAX * 128 + 16, MALLOC_CAP_SPIRAM);
  if (!it || !js) { heap_caps_free(it); heap_caps_free(js); return sem_memoria(req); }
  const int n = berry_scripts_listar(it, LISTA_MAX);
  esp_err_t r;
  if (n < 0) {
    r = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
  } else {
    size_t k = snprintf(js, 16, "[");
    for (int i = 0; i < n; ++i) {
      char nome[TAB5_ARQ_NOME_MAX + 8];
      tab5_web_json_texto(nome, sizeof(nome), it[i].nome);
      k += snprintf(js + k, LISTA_MAX * 128 + 16 - k, "%s{\"nome\":%s,\"bytes\":%lu}",
                    i ? "," : "", nome, (unsigned long)it[i].bytes);
    }
    snprintf(js + k, LISTA_MAX * 128 + 16 - k, "]");
    r = tab5_web_json(req, js);
  }
  heap_caps_free(it);
  heap_caps_free(js);
  return r;
}

// The requester's ?arq=, validated. false = already replied 400.
static bool nome_do_pedido(httpd_req_t* req, char* arq, size_t n)
{
  if (tab5_web_param(req, "arq", arq, n) && berry_script_nome_valido(arq)) { return true; }
  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid script name (letters, digits, _ - and .be at the end)");
  return false;
}

static esp_err_t ler(httpd_req_t* req)
{
  char arq[64];
  if (!nome_do_pedido(req, arq, sizeof(arq))) { return ESP_OK; }
  char* txt = (char*)heap_caps_malloc(BERRY_SCRIPT_MAX, MALLOC_CAP_SPIRAM);
  if (!txt) { return sem_memoria(req); }
  esp_err_t r;
  const berry_ler_t l = berry_script_ler(arq, txt, BERRY_SCRIPT_MAX);
  if (l == BERRY_LER_OK) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = httpd_resp_sendstr(req, txt);
  } else if (l == BERRY_LER_GRANDE) {
    r = httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "script too big to edit (over 32 KB)");
  } else {
    r = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "could not open");
  }
  heap_caps_free(txt);
  return r;
}

// The reply for check/run: ok, the line and the error message.
static esp_err_t resultado(httpd_req_t* req, bool ok, int linha, const char* erro, const char* extra)
{
  char e[200], js[320];
  tab5_web_json_texto(e, sizeof(e), erro);
  snprintf(js, sizeof(js), "{\"ok\":%s,\"linha\":%d,\"erro\":%s%s}", ok ? "true" : "false", linha, e, extra);
  return tab5_web_json(req, js);
}

// Writes (and, with 'rodar', checks and opens) the request body.
static esp_err_t gravar_ou_rodar(httpd_req_t* req, bool rodar)
{
  char arq[64];
  if (!nome_do_pedido(req, arq, sizeof(arq))) { return ESP_OK; }
  char* txt = (char*)heap_caps_malloc(BERRY_SCRIPT_MAX, MALLOC_CAP_SPIRAM);
  if (!txt) { return sem_memoria(req); }
  esp_err_t r = ESP_OK;
  const int n = tab5_web_corpo(req, txt, BERRY_SCRIPT_MAX);
  if (n < 0) {
    // tab5_web_corpo has already replied (or the connection dropped)
  } else if (!berry_script_gravar(arq, txt, n)) {
    r = resultado(req, false, 0, "not saved (SD card?)", "");
  } else if (!rodar) {
    char aut[4];
    if (!tab5_web_param(req, "auto", aut, sizeof(aut))) { berry_apps_reler(); }   // new script: the icon appears
    r = resultado(req, true, 0, "", "");
  } else {
    char erro[160] = "";
    int linha = 0;
    const bool ok = berry_verificar(txt, n, erro, sizeof(erro), &linha);
    bool aberto = false;
    if (ok) {
      berry_apps_reler();
      // Each opening reads the .be again - what runs is what has just arrived.
      // A new script only gets an app after the reread, which the home screen applies.
      char nome[48];
      berry_script_nome_app(txt, arq, nome, sizeof(nome));
      aberto = sistema_abrir_app(nome);
      ESP_LOGI(TAG, "run %s: %s", arq, aberto ? "opened" : "no app yet");
    }
    r = resultado(req, ok, linha, erro, aberto ? ",\"aberto\":true" : ",\"aberto\":false");
  }
  heap_caps_free(txt);
  return r;
}

static esp_err_t gravar(httpd_req_t* req) { return gravar_ou_rodar(req, false); }
static esp_err_t rodar(httpd_req_t* req)  { return gravar_ou_rodar(req, true); }

static esp_err_t verificar(httpd_req_t* req)
{
  char* txt = (char*)heap_caps_malloc(BERRY_SCRIPT_MAX, MALLOC_CAP_SPIRAM);
  if (!txt) { return sem_memoria(req); }
  esp_err_t r = ESP_OK;
  const int n = tab5_web_corpo(req, txt, BERRY_SCRIPT_MAX);
  if (n >= 0) {
    char erro[160] = "";
    int linha = 0;
    const bool ok = berry_verificar(txt, n, erro, sizeof(erro), &linha);
    r = resultado(req, ok, linha, erro, "");
  }
  heap_caps_free(txt);
  return r;
}

static const tab5_web_rota_t ROTAS[] = {
  { "/",              HTTP_GET,  pagina },
  { "/api/scripts",   HTTP_GET,  lista },
  { "/api/script",    HTTP_GET,  ler },
  { "/api/script",    HTTP_POST, gravar },
  { "/api/verificar", HTTP_POST, verificar },
  { "/api/rodar",     HTTP_POST, rodar },
};

bool berry_web_ligar(void)
{
  return tab5_web_abrir("Editor", "editor", ROTAS, sizeof(ROTAS) / sizeof(ROTAS[0]));
}
