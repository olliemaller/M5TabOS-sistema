// The browser portal - see tab5_web_portal.hpp.
#include "tab5_web_portal.hpp"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "app.h"
#include "tab5_power.hpp"
#include "tab5_rede.hpp"
#include "tab5_wifi.hpp"

extern const char PORTAL_INICIO[] asm("_binary_tab5_web_portal_html_start");
extern const char PORTAL_FIM[]    asm("_binary_tab5_web_portal_html_end");

#define SISTEMA_MAX 2048   // the api/sistema response (fits comfortably: 4 parts, few apps)

// ---------------------------------------------------------------------------
// THE CATALOGUE: the apps of this variant that have a PC page (app_t.no_pc)
// ---------------------------------------------------------------------------
static const app_t* app_do_pc(const char* nome)
{
  for (int i = 0; nome && i < SISTEMA_NUM_APPS; ++i) {
    const app_t* a = SISTEMA_APPS[i];
    if (a && a->no_pc && !strcasecmp(a->nome, nome)) { return a; }
  }
  return nullptr;
}

bool tab5_web_abrir_app(const char* nome)
{
  const app_t* a = app_do_pc(nome);
  return a && a->no_pc();
}

int tab5_web_catalogo(const char** out, int max)
{
  int n = 0;
  for (int i = 0; i < SISTEMA_NUM_APPS && n < max; ++i) {
    const app_t* a = SISTEMA_APPS[i];
    if (a && a->no_pc) { out[n++] = a->nome; }
  }
  return n;
}

// ---------------------------------------------------------------------------
// THE ROOT ROUTES
// ---------------------------------------------------------------------------
static esp_err_t pagina(httpd_req_t* req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PORTAL_INICIO, PORTAL_FIM - PORTAL_INICIO - 1);   // TEXT puts a '\0' at the end
}

// The overview: what the Tab5's status bar shows, plus the apps.
static esp_err_t sistema(httpd_req_t* req)
{
  char* out = (char*)heap_caps_malloc(SISTEMA_MAX, MALLOC_CAP_SPIRAM);
  if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory"); return ESP_FAIL; }
  tab5_rede_ip_t eu = {};
  tab5_rede_ip(&eu);
  const tab5_medida_t m = tab5_power_ultima();
  char nome[80], ssid[80], versao[80];
  tab5_web_json_texto(nome, sizeof(nome), eu.nome[0] ? eu.nome : "Tab5");
  tab5_web_json_texto(ssid, sizeof(ssid), tab5_wifi_ssid() ? tab5_wifi_ssid() : "");
  tab5_web_json_texto(versao, sizeof(versao), esp_app_get_description()->version);
  int k = snprintf(out, SISTEMA_MAX,
                   "{\"nome\":%s,\"versao\":%s,\"ip\":\"%s\",\"ssid\":%s,"
                   "\"bateria\":{\"pct\":%d,\"volts\":%.2f,\"mA\":%.0f},"
                   "\"ligado_s\":%llu,\"cpu_mhz\":%d,"
                   "\"mem\":{\"interna_livre\":%u,\"interna_total\":%u,\"psram_livre\":%u,\"psram_total\":%u},"
                   "\"ocioso_min\":%d,\"apps\":[",
                   nome, versao, eu.ip, ssid, m.pct, m.volts, m.mA,
                   (unsigned long long)(esp_timer_get_time() / 1000000), tab5_power_cpu_mhz(),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                   WEB_OCIOSO_MS / 60000);
  // Those in the catalogue, open or not; and the last one that opened (the page starts on it).
  const char* cat[SISTEMA_APPS_MAX];
  const int nc = tab5_web_catalogo(cat, SISTEMA_APPS_MAX);
  const char* abertos[WEB_PARTES_MAX];
  const int na = tab5_web_abertos(abertos, WEB_PARTES_MAX);
  for (int i = 0; i < nc && k < SISTEMA_MAX - 160; ++i) {
    char pasta[24] = "", nm[64];
    tab5_web_json_texto(nm, sizeof(nm), cat[i]);
    const bool aberto = tab5_web_pasta(cat[i], pasta, sizeof(pasta));
    k += snprintf(out + k, SISTEMA_MAX - k, "%s{\"nome\":%s,\"aberto\":%s,\"pasta\":\"%s\"}",
                  i ? "," : "", nm, aberto ? "true" : "false", pasta);
  }
  char ult[64] = "null";
  if (na) { tab5_web_json_texto(ult, sizeof(ult), abertos[na - 1]); }
  snprintf(out + k, SISTEMA_MAX - k, "],\"ultimo\":%s}", ult);
  const esp_err_t r = tab5_web_json(req, out);
  heap_caps_free(out);
  return r;
}

static esp_err_t abrir(httpd_req_t* req)
{
  char nome[32], pasta[24];
  if (!tab5_web_param(req, "app", nome, sizeof(nome)) || !app_do_pc(nome)) {
    httpd_resp_set_status(req, "404 Not Found");
    return tab5_web_json(req, "{\"ok\":false,\"msg\":\"that app does not open on the PC\"}");
  }
  if (!tab5_web_abrir_app(nome) || !tab5_web_pasta(nome, pasta, sizeof(pasta))) {
    return tab5_web_json(req, "{\"ok\":false,\"msg\":\"the Tab5 could not open it (no free slot?)\"}");
  }
  char js[64];
  snprintf(js, sizeof(js), "{\"ok\":true,\"pasta\":\"%s\"}", pasta);
  return tab5_web_json(req, js);
}

// SCREENSHOT: the Tab5 screen as it is now, as a .bmp the browser shows
// (Overview > Screenshot). Streamed in bands of rows, so no 1.8 MB buffer:
// the header says top-down (negative height) and each band goes out as read.
// readRect gives LovyanGFX byte order (swapped), undone per pixel.
static esp_err_t tela(httpd_req_t* req)
{
  static const int FAIXA = 16;
  const int w = M5.Display.width(), h = M5.Display.height();
  uint16_t* px = (uint16_t*)heap_caps_malloc((size_t)w * FAIXA * 2, MALLOC_CAP_SPIRAM);
  if (!px) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory"); }
  uint8_t cab[66] = { 0 };
  auto u32 = [&](int o, uint32_t v) { cab[o] = v; cab[o + 1] = v >> 8; cab[o + 2] = v >> 16; cab[o + 3] = v >> 24; };
  const uint32_t dados = (uint32_t)w * h * 2;   // 720 * 2 is a multiple of 4: no row padding
  cab[0] = 'B'; cab[1] = 'M';
  u32(2, sizeof(cab) + dados); u32(10, sizeof(cab)); u32(14, 40); u32(18, (uint32_t)w); u32(22, (uint32_t)(-h));
  cab[26] = 1; cab[28] = 16; u32(30, 3); u32(34, dados);
  u32(54, 0xF800); u32(58, 0x07E0); u32(62, 0x001F);
  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"tab5-screen.bmp\"");
  esp_err_t r = httpd_resp_send_chunk(req, (const char*)cab, sizeof(cab));
  for (int y = 0; y < h && r == ESP_OK; y += FAIXA) {
    const int a = (h - y) < FAIXA ? (h - y) : FAIXA;
    M5.Display.readRect(0, y, w, a, px);
    for (int i = 0; i < w * a; ++i) { px[i] = (uint16_t)((px[i] << 8) | (px[i] >> 8)); }
    r = httpd_resp_send_chunk(req, (const char*)px, (size_t)w * a * 2);
  }
  heap_caps_free(px);
  if (r == ESP_OK) { r = httpd_resp_send_chunk(req, nullptr, 0); }
  return r;
}

static esp_err_t desligar(httpd_req_t* req)
{
  const esp_err_t r = tab5_web_json(req, "{\"ok\":true}");
  tab5_web_desligar();   // the stop waits for this response to finish
  return r;
}

static const tab5_web_rota_t ROTAS[] = {
  { "/",             HTTP_GET,  pagina,   false, nullptr },
  { "/api/sistema",  HTTP_GET,  sistema,  false, nullptr },
  { "/api/abrir",    HTTP_POST, abrir,    false, nullptr },
  { "/api/desligar", HTTP_POST, desligar, false, nullptr },
  { "/api/tela.bmp", HTTP_GET,  tela,     false, nullptr },
};

const tab5_web_rota_t* tab5_web_portal_rotas(int* n)
{
  *n = sizeof(ROTAS) / sizeof(ROTAS[0]);
  return ROTAS;
}
