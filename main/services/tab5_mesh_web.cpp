// Mesh in the PC's browser - see tab5_mesh_web.hpp.
#include "tab5_mesh_web.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "tab5_web.hpp"
#include "tab5_mesh.hpp"

extern const char PAGINA_INICIO[] asm("_binary_tab5_mesh_web_html_start");
extern const char PAGINA_FIM[]    asm("_binary_tab5_mesh_web_html_end");

#define MSGS_MAX 80   // the last ones of a conversation, like the app

static esp_err_t pagina(httpd_req_t* req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PAGINA_INICIO, PAGINA_FIM - PAGINA_INICIO - 1);   // TEXT adds a '\0' at the end
}

// CHUNKED JSON: lists that can be long (nodes, messages) go out item by
// item, without building everything in one buffer.
static void comecar_json(httpd_req_t* req)
{
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}
static void pedaco(httpd_req_t* req, const char* s) { httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN); }
static esp_err_t fim_json(httpd_req_t* req) { return httpd_resp_send_chunk(req, nullptr, 0); }

// The request's conversation: c=1&idx=N (channel) or no=N (direct). false = already answered 400.
static bool conversa_do_pedido(httpd_req_t* req, bool* canal, uint8_t* idx, uint32_t* no)
{
  char v[16];
  *canal = tab5_web_param(req, "c", v, sizeof(v)) && v[0] == '1';
  *idx = 0;
  *no = 0;
  if (*canal) {
    if (tab5_web_param(req, "idx", v, sizeof(v))) {
      const int i = atoi(v);
      if (i >= 0 && i < MESH_MAX_CANAIS) { *idx = (uint8_t)i; return true; }
    }
  } else if (tab5_web_param(req, "no", v, sizeof(v))) {
    *no = (uint32_t)strtoul(v, nullptr, 10);
    if (*no && *no != 0xFFFFFFFFu) { return true; }
  }
  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "conversa invalida");
  return false;
}

static esp_err_t estado(httpd_req_t* req)
{
  char nome[48], nj[64], js[256];
  const uint32_t eu = tab5_mesh_meu_numero();
  nome[0] = 0;
  if (eu) { tab5_mesh_nome_exibicao(eu, nome, sizeof(nome)); }
  tab5_web_json_texto(nj, sizeof(nj), nome);
  snprintf(js, sizeof(js),
           "{\"eu\":%lu,\"eu_nome\":%s,\"no_ar\":%s,\"conv\":%lu,\"msgs\":%lu,\"envios\":%lu,\"nao_lidas\":%d}",
           (unsigned long)eu, nj, tab5_mesh_no_ar() ? "true" : "false",
           (unsigned long)tab5_mesh_conversas_versao(), (unsigned long)tab5_mesh_total_msgs(),
           (unsigned long)tab5_mesh_envios_versao(), tab5_mesh_nao_lidas());
  return tab5_web_json(req, js);
}

static esp_err_t conversas(httpd_req_t* req)
{
  comecar_json(req);
  pedaco(req, "[");
  const int n = tab5_mesh_num_conversas();
  bool primeiro = true;
  for (int i = 0; i < n; ++i) {
    mesh_conversa_t c;
    if (!tab5_mesh_conversa(i, &c) || !c.usada) { continue; }
    char nome[MESH_NOME_CONVERSA * 2 + 4], sub[MESH_NOME_LONGO * 2 + 4], js[320];
    tab5_web_json_texto(nome, sizeof(nome), c.nome);
    tab5_web_json_texto(sub, sizeof(sub), c.sub);
    snprintf(js, sizeof(js), "%s{\"canal\":%s,\"idx\":%u,\"no\":%lu,\"nome\":%s,\"sub\":%s,\"nao_lidas\":%d,\"ultima\":%lu}",
             primeiro ? "" : ",", c.canal ? "true" : "false", (unsigned)c.idx, (unsigned long)c.no,
             nome, sub, c.nao_lidas, (unsigned long)c.ultima_em);
    pedaco(req, js);
    primeiro = false;
  }
  pedaco(req, "]");
  return fim_json(req);
}

static esp_err_t canais(httpd_req_t* req)
{
  comecar_json(req);
  pedaco(req, "[");
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n; ++i) {
    uint8_t idx = 0;
    char nome[32], nj[72], js[128];
    bool visivel = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &visivel)) { continue; }
    tab5_web_json_texto(nj, sizeof(nj), nome);
    snprintf(js, sizeof(js), "%s{\"idx\":%u,\"nome\":%s,\"visivel\":%s}", i ? "," : "", (unsigned)idx, nj,
             visivel ? "true" : "false");
    pedaco(req, js);
  }
  pedaco(req, "]");
  return fim_json(req);
}

// A whole node, for the page's Nodes tab. The texts in PSRAM (see msg_txt_t).
typedef struct {
  mesh_no_t no;
  char nome[48], nj[100], lj[MESH_NOME_LONGO * 6 + 4], hj[72];
  char js[640];
} no_txt_t;

static esp_err_t nos(httpd_req_t* req)
{
  no_txt_t* t = (no_txt_t*)heap_caps_malloc(sizeof(no_txt_t), MALLOC_CAP_SPIRAM);
  if (!t) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory"); }
  comecar_json(req);
  pedaco(req, "[");
  const int n = tab5_mesh_num_nos();
  const uint32_t eu = tab5_mesh_meu_numero();
  bool primeiro = true;
  for (int i = 0; i < n; ++i) {
    mesh_no_t* no = &t->no;
    if (!tab5_mesh_no(i, no) || !no->usado || no->num == eu) { continue; }
    tab5_mesh_nome_exibicao(no->num, t->nome, sizeof(t->nome));
    tab5_web_json_texto(t->nj, sizeof(t->nj), t->nome);
    tab5_web_json_texto(t->lj, sizeof(t->lj), no->longo);
    const char* hw = mesh_hw_nome(no->hw);
    char hw_num[8];
    snprintf(hw_num, sizeof(hw_num), "%u", (unsigned)no->hw);
    tab5_web_json_texto(t->hj, sizeof(t->hj), no->hw ? (hw ? hw : hw_num) : "");
    // No %f (stack): SNR in tenths of a dB, voltage in mV, position in degrees x 1e7.
    snprintf(t->js, sizeof(t->js),
             "%s{\"num\":%lu,\"id\":\"%s\",\"nome\":%s,\"longo\":%s,\"hw\":%s,\"visto\":%lu,\"idade\":%ld,"
             "\"saltos\":%u,\"snr10\":%d,\"bat\":%u,\"mv\":%d,\"mqtt\":%s,\"fav\":%s,\"lat\":%ld,\"lon\":%ld}",
             primeiro ? "" : ",", (unsigned long)no->num, no->id, t->nj, t->lj, t->hj, (unsigned long)no->visto_em,
             (long)no->idade_s, (unsigned)no->saltos, (int)(no->snr * 10.0f), (unsigned)no->bateria,
             (int)(no->tensao * 1000.0f), no->via_mqtt ? "true" : "false", no->favorito ? "true" : "false",
             (long)(no->tem_pos ? no->lat_i : 0), (long)(no->tem_pos ? no->lon_i : 0));
    pedaco(req, t->js);
    primeiro = false;
  }
  heap_caps_free(t);
  pedaco(req, "]");
  return fim_json(req);
}

// Mesh health (what our node reports about itself). Integers: usage in tenths of a %.
static esp_err_t saude(httpd_req_t* req)
{
  mesh_saude_t m;
  tab5_mesh_saude(&m);
  char js[320];
  snprintf(js, sizeof(js),
           "{\"tem_local\":%s,\"tem_metricas\":%s,\"uso_canal10\":%d,\"airtime10\":%d,\"uptime\":%lu,"
           "\"ruido\":%ld,\"online\":%u,\"total\":%u,\"tx\":%lu,\"rx\":%lu,\"ruins\":%lu,\"repetidos\":%lu,"
           "\"via\":%u,\"enlace\":\"%s\"}",
           m.tem_local ? "true" : "false", m.tem_metricas ? "true" : "false", (int)(m.uso_canal * 10.0f),
           (int)(m.airtime_tx * 10.0f), (unsigned long)m.uptime_s, (long)m.piso_ruido, (unsigned)m.nos_online,
           (unsigned)m.nos_total, (unsigned long)m.pkt_tx, (unsigned long)m.pkt_rx, (unsigned long)m.pkt_ruins,
           (unsigned long)m.pkt_repetidos, (unsigned)tab5_mesh_via(), tab5_mesh_enlace_descricao());
  return tab5_web_json(req, js);
}

// Star on a node (the same as the app's: goes to the node when it is there).
static esp_err_t favorito(httpd_req_t* req)
{
  char v[16];
  if (!tab5_web_param(req, "no", v, sizeof(v))) { return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing node"); }
  const uint32_t no = (uint32_t)strtoul(v, nullptr, 10);
  const bool sim = tab5_web_param(req, "sim", v, sizeof(v)) && v[0] == '1';
  tab5_mesh_definir_favorito(no, sim);
  return tab5_web_json(req, "{\"ok\":true}");
}

// A MESSAGE'S TEXTS IN PSRAM, not on the stack: the httpd one is internal and
// small (WEB_PILHA), and with them on it + the card history + printf it
// OVERFLOWED (end-of-stack Breakpoint inside snprintf).
typedef struct {
  char nome[48], nj[100], tj[MESH_TEXTO * 6 + 4], ej[48];
  char js[MESH_TEXTO * 6 + 400];
} msg_txt_t;

static esp_err_t msgs(httpd_req_t* req)
{
  bool canal;
  uint8_t idx;
  uint32_t no;
  if (!conversa_do_pedido(req, &canal, &idx, &no)) { return ESP_OK; }
  mesh_msg_t* m = (mesh_msg_t*)heap_caps_malloc(MSGS_MAX * sizeof(mesh_msg_t), MALLOC_CAP_SPIRAM);
  msg_txt_t* t = (msg_txt_t*)heap_caps_malloc(sizeof(msg_txt_t), MALLOC_CAP_SPIRAM);
  if (!m || !t) { heap_caps_free(m); heap_caps_free(t); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory"); }
  const int n = tab5_mesh_historico_de(canal, idx, no, m, MSGS_MAX);
  // Read on the PC = read (like opening the conversation in the app).
  const int vis = tab5_mesh_conversa_indice(canal, idx, no);
  if (vis >= 0) { tab5_mesh_marcar_conversa_lida(vis); }
  const uint32_t eu = tab5_mesh_meu_numero();
  comecar_json(req);
  pedaco(req, "[");
  for (int i = 0; i < n; ++i) {
    const bool minha = eu && m[i].de == eu;
    tab5_mesh_nome_exibicao(m[i].de, t->nome, sizeof(t->nome));
    tab5_web_json_texto(t->nj, sizeof(t->nj), t->nome);
    tab5_web_json_texto(t->tj, sizeof(t->tj), m[i].texto);
    uint8_t envio = m[i].envio, erro = m[i].erro;
    if (minha && m[i].id) { envio = tab5_mesh_estado_envio(m[i].id, &erro); }   // the current one
    tab5_web_json_texto(t->ej, sizeof(t->ej), envio == MESH_FALHOU ? mesh_erro_nome(erro) : "");
    // No %f: formatting floating point is what uses the most stack in printf.
    snprintf(t->js, sizeof(t->js),
             "%s{\"de\":%lu,\"nome\":%s,\"texto\":%s,\"quando\":%lu,\"minha\":%s,\"envio\":%u,\"erro\":%s,"
             "\"mqtt\":%s,\"saltos\":%u,\"snr10\":%d,\"rssi\":%ld}",
             i ? "," : "", (unsigned long)m[i].de, t->nj, t->tj, (unsigned long)m[i].quando, minha ? "true" : "false",
             (unsigned)envio, t->ej, m[i].via_mqtt ? "true" : "false", (unsigned)m[i].saltos,
             (int)(m[i].snr * 10.0f), (long)m[i].rssi);
    pedaco(req, t->js);
  }
  heap_caps_free(m);
  heap_caps_free(t);
  pedaco(req, "]");
  return fim_json(req);
}

static esp_err_t enviar(httpd_req_t* req)
{
  bool canal;
  uint8_t idx;
  uint32_t no;
  if (!conversa_do_pedido(req, &canal, &idx, &no)) { return ESP_OK; }
  char texto[MESH_PAYLOAD_MAX + 1];
  const int n = tab5_web_corpo(req, texto, sizeof(texto));
  if (n < 0) { return ESP_OK; }   // too big: already answered 413
  if (n == 0) { return tab5_web_json(req, "{\"ok\":false,\"erro\":\"empty message\"}"); }
  if (!tab5_mesh_servico_ativo()) { return tab5_web_json(req, "{\"ok\":false,\"erro\":\"Mesh is off on the Tab5\"}"); }
  // The conversation appears in the column (a direct one with someone not yet spoken to, the hidden channel).
  if (canal) { tab5_mesh_conversa_do_canal(idx); } else { tab5_mesh_conversa_com(no); }
  const bool ok = tab5_mesh_enviar(canal ? 0xFFFFFFFFu : no, canal ? idx : 0, texto);
  return tab5_web_json(req, ok ? "{\"ok\":true,\"erro\":\"\"}" : "{\"ok\":false,\"erro\":\"the node did not answer (link silent)\"}");
}

static const tab5_web_rota_t ROTAS[] = {
  { "/",               HTTP_GET,  pagina },
  { "/api/estado",     HTTP_GET,  estado },
  { "/api/conversas",  HTTP_GET,  conversas },
  { "/api/canais",     HTTP_GET,  canais },
  { "/api/nos",        HTTP_GET,  nos },
  { "/api/msgs",       HTTP_GET,  msgs },
  { "/api/enviar",     HTTP_POST, enviar },
  { "/api/saude",      HTTP_GET,  saude },
  { "/api/favorito",   HTTP_POST, favorito },
};

bool tab5_mesh_web_ligar(void)
{
  return tab5_web_abrir("Mesh", "mesh", ROTAS, sizeof(ROTAS) / sizeof(ROTAS[0]));
}
