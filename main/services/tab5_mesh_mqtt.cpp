#include "tab5_mesh_mqtt.hpp"
#include "tab5_mutex.hpp"
#include "mesh_pb.hpp"   // o codec do servico Mesh (leitura e escrita)
#include "tab5_hora.hpp"  // tab5_hora_epoch

#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/aes.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include <cstring>
#include <cstdio>
#include <ctime>

#include "ajustes.h"
#include "idioma.h"
#include "app.h"   // sistema_mesh_reiniciar

static const char* TAG = "mesh_mqtt";

// ---------------------------------------------------------------------------
// CONFIGURACAO (aprendida do no, guardada na NVS)
// ---------------------------------------------------------------------------
#define MAX_CANAIS 8

typedef struct {
  uint8_t papel;         // 0 desligado, 1 primario, 2 secundario (Channel.Role)
  char    nome[12];      // o nome do canal e no maximo 11 bytes no Meshtastic
  uint8_t n_psk;
  uint8_t psk[32];
} canal_t;

typedef struct {
  canal_t canais[MAX_CANAIS];
  int32_t regiao;        // Config.LoRaConfig.RegionCode; 0 = nao sabe
  char    srv[64], usr[32], pwd[32], raiz[48];
} cfg_t;

static cfg_t             s_cfg;
static bool              s_cfg_lido = false;
static Trava s_trava_cfg;

#define NVS_ESPACO "mesh"
#define NVS_CHAVE  "mqtt_cfg"

// Servidor da comunidade brasileira (Mesh Brasil), com as credenciais que ela
// publica. A raiz dele e "meshdev", sem regiao: o Brasil usa a faixa ANZ, e a
// regiao do no nao entra no topico aqui.
#define SRV_PADRAO "platform.meshbrasil.com"
#define SRV_MESHTASTIC "mqtt.meshtastic.org"
static void padrao_regional(const char** srv, const char** raiz);   // below, by RAIZ_PADRAO
#define USR_PADRAO "meshdev"
#define PWD_PADRAO "large4cats"

// OS MODELOS (ver mesh_mqtt_modelo). O servidor publico do Meshtastic separa a
// malha por regiao na raiz do topico ("msh/EU_868"); as credenciais sao as
// publicas que cada projeto divulga.
static const mesh_mqtt_servidor_t MODELOS[] = {
  { "Mesh Brasil",             "platform.meshbrasil.com", "meshdev", "large4cats", "meshdev" },
  { "Meshtastic Brasil",       "mqtt.meshtastic.org",     "meshdev", "large4cats", "msh/BR" },
  { "Meshtastic Europa",       "mqtt.meshtastic.org",     "meshdev", "large4cats", "msh/EU_868" },
  { "Meshtastic Australia", "mqtt.meshtastic.org",     "meshdev", "large4cats", "msh/ANZ" },
  { "Em branco",               "",                        "",        "",           "msh" },
};
#define N_MODELOS ((int)(sizeof(MODELOS) / sizeof(MODELOS[0])))
int mesh_mqtt_n_modelos(void) { return N_MODELOS; }
const mesh_mqtt_servidor_t* mesh_mqtt_modelo(int i) { return (i >= 0 && i < N_MODELOS) ? &MODELOS[i] : nullptr; }

int mesh_mqtt_n_servidores(void) { return 1 + ajustes_mqtt_n(); }

bool mesh_mqtt_servidor(int i, mesh_mqtt_servidor_t* out)
{
  if (!out) { return false; }
  if (i == AJUSTES_MQTT_AUTOMATICO) {
    memset(out, 0, sizeof(*out));
    const char* sv; const char* rz;
    padrao_regional(&sv, &rz);
    snprintf(out->nome, sizeof(out->nome), "%s (%s)", tr("Automático"), strcmp(sv, SRV_PADRAO) == 0 ? "Mesh Brasil" : rz);
    return true;
  }
  return ajustes_mqtt_ler(i - 1, out);
}

const char* mesh_mqtt_servidor_nome(uint8_t escolha)
{
  static char nome[sizeof(((mesh_mqtt_servidor_t*)nullptr)->nome)];
  mesh_mqtt_servidor_t s;
  if (!mesh_mqtt_servidor(escolha, &s)) { mesh_mqtt_servidor(AJUSTES_MQTT_AUTOMATICO, &s); }
  snprintf(nome, sizeof(nome), "%s", s.nome);
  memset(&s, 0, sizeof(s));   // levava a senha
  return nome;
}

bool mesh_mqtt_pode_adicionar(void) { return ajustes_mqtt_n() < AJUSTES_MQTT_MAX; }

int mesh_mqtt_salvar_servidor(int i, const mesh_mqtt_servidor_t* s)
{
  if (!s || !s->srv[0]) { return 0; }
  mesh_mqtt_servidor_t c = *s;
  if (!c.nome[0]) { snprintf(c.nome, sizeof(c.nome), "%s", c.srv); }
  if (!c.raiz[0]) { snprintf(c.raiz, sizeof(c.raiz), "msh"); }
  const int k = ajustes_mqtt_gravar(i >= 1 ? i - 1 : -1, &c);
  memset(&c, 0, sizeof(c));
  return k < 0 ? 0 : k + 1;
}

void mesh_mqtt_apagar_servidor(int i) { if (i >= 1) { ajustes_mqtt_apagar(i - 1); } }

void mesh_mqtt_usar_servidor(uint8_t escolha)
{
  ajustes_definir_mqtt_servidor(escolha);
  sistema_mesh_reiniciar();   // o cliente MQTT sobe de novo, ja no servidor escolhido
}

int mesh_mqtt_salvar_e_usar(int i, const mesh_mqtt_servidor_t* s)
{
  const int k = mesh_mqtt_salvar_servidor(i, s);
  if (k) { mesh_mqtt_usar_servidor((uint8_t)k); }
  return k;
}

bool mesh_mqtt_apagar_e_reconectar(int i)
{
  const bool era_o_usado = ajustes_mqtt_servidor() == i;
  mesh_mqtt_apagar_servidor(i);
  if (era_o_usado) { mesh_mqtt_usar_servidor(AJUSTES_MQTT_AUTOMATICO); }
  return era_o_usado;
}

bool mesh_mqtt_para_editar(int i, mesh_mqtt_servidor_t* p)
{
  memset(p, 0, sizeof(*p));
  if (i == 0) { *p = *mesh_mqtt_modelo(0); return true; }   // novo: comeca pelo primeiro modelo
  return mesh_mqtt_servidor(i, p);
}

int mesh_mqtt_proximo_modelo(int atual, mesh_mqtt_servidor_t* p)
{
  const int novo = (atual + 1) % mesh_mqtt_n_modelos();
  *p = *mesh_mqtt_modelo(novo);
  return novo;
}

char* mesh_mqtt_campo(mesh_mqtt_servidor_t* p, int k, size_t* tam)
{
  switch (k) {
    case 0:  *tam = sizeof(p->nome); return p->nome;
    case 1:  *tam = sizeof(p->srv);  return p->srv;
    case 2:  *tam = sizeof(p->usr);  return p->usr;
    case 3:  *tam = sizeof(p->pwd);  return p->pwd;
    default: *tam = sizeof(p->raiz); return p->raiz;
  }
}

// A escolha da pessoa, se houver (diferente de automatico): preenche e true.
static bool servidor_escolhido(char* srv, size_t ts, char* usr, size_t tu,
                               char* pwd, size_t tp, char* raiz, size_t tr)
{
  const uint8_t e = ajustes_mqtt_servidor();
  mesh_mqtt_servidor_t v;
  if (e == AJUSTES_MQTT_AUTOMATICO || !mesh_mqtt_servidor(e, &v) || !v.srv[0]) { return false; }
  snprintf(srv, ts, "%s", v.srv);  snprintf(usr, tu, "%s", v.usr);
  snprintf(pwd, tp, "%s", v.pwd);  snprintf(raiz, tr, "%s", v.raiz[0] ? v.raiz : "msh");
  memset(&v, 0, sizeof(v));
  return true;
}

// Config.LoRaConfig.RegionCode, na ordem do protobuf (meshtastic/config.proto).
static const char* const REGIOES[] = {
  "UNSET", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN",
  "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923",
  "PH_433", "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902",
};

static void travar_cfg(void)
{
  s_trava_cfg.travar();
}
static void destravar_cfg(void) { s_trava_cfg.destravar(); }

// Com a trava tomada.
static void garantir_cfg(void)
{
  if (s_cfg_lido) { return; }
  s_cfg_lido = true;
  memset(&s_cfg, 0, sizeof(s_cfg));
  nvs_handle_t h;
  if (nvs_open(NVS_ESPACO, NVS_READONLY, &h) == ESP_OK) {
    size_t tam = sizeof(s_cfg);
    if (nvs_get_blob(h, NVS_CHAVE, &s_cfg, &tam) != ESP_OK || tam != sizeof(s_cfg)) {
      memset(&s_cfg, 0, sizeof(s_cfg));
    }
    nvs_close(h);
  }
  // Nenhum canal aprendido: o padrao de fabrica de todo no, LongFast com a
  // chave padrao (psk de 1 byte = 1).
  bool algum = false;
  for (int i = 0; i < MAX_CANAIS; ++i) { if (s_cfg.canais[i].papel) { algum = true; } }
  if (!algum) {
    s_cfg.canais[0].papel = 1;
    snprintf(s_cfg.canais[0].nome, sizeof(s_cfg.canais[0].nome), "LongFast");
    s_cfg.canais[0].n_psk = 1;
    s_cfg.canais[0].psk[0] = 1;
  }
}

// Com a trava tomada.
static void gravar_cfg(void)
{
  nvs_handle_t h;
  if (nvs_open(NVS_ESPACO, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_blob(h, NVS_CHAVE, &s_cfg, sizeof(s_cfg));
  nvs_commit(h);
  nvs_close(h);
}

void mesh_mqtt_aprender_canal(int idx, int papel, const char* nome, const uint8_t* psk, size_t n_psk)
{
  if (idx < 0 || idx >= MAX_CANAIS) { return; }
  canal_t novo = { };
  novo.papel = (uint8_t)papel;
  snprintf(novo.nome, sizeof(novo.nome), "%s", nome ? nome : "");
  novo.n_psk = (uint8_t)(n_psk > sizeof(novo.psk) ? sizeof(novo.psk) : n_psk);
  if (psk && novo.n_psk) { memcpy(novo.psk, psk, novo.n_psk); }
  travar_cfg();
  garantir_cfg();
  if (memcmp(&s_cfg.canais[idx], &novo, sizeof(novo)) != 0) {
    s_cfg.canais[idx] = novo;
    gravar_cfg();
    ESP_LOGI(TAG, "canal %d aprendido do no: \"%s\" (chave de %u bytes)", idx, novo.nome, (unsigned)novo.n_psk);
  }
  destravar_cfg();
}

void mesh_mqtt_aprender_regiao(int regiao)
{
  travar_cfg();
  garantir_cfg();
  if (s_cfg.regiao != regiao) { s_cfg.regiao = regiao; gravar_cfg(); }
  destravar_cfg();
}

void mesh_mqtt_aprender_servidor(const char* endereco, const char* usuario,
                                 const char* senha, const char* raiz)
{
  cfg_t c;
  travar_cfg();
  garantir_cfg();
  c = s_cfg;
  snprintf(c.srv,  sizeof(c.srv),  "%s", endereco ? endereco : "");
  snprintf(c.usr,  sizeof(c.usr),  "%s", usuario ? usuario : "");
  snprintf(c.pwd,  sizeof(c.pwd),  "%s", senha ? senha : "");
  snprintf(c.raiz, sizeof(c.raiz), "%s", raiz ? raiz : "");
  if (memcmp(&c, &s_cfg, sizeof(c)) != 0) { s_cfg = c; gravar_cfg(); ESP_LOGI(TAG, "servidor aprendido do no"); }
  destravar_cfg();
}

// Raiz do servidor padrao (ver SRV_PADRAO).
#define RAIZ_PADRAO "meshdev"

// AUTOMATIC BY REGION: with no server chosen and none learned from the node,
// the internet mode used to go to Mesh Brasil from anywhere. The time zone
// says roughly where the Tab5 is: Brazil keeps Mesh Brasil, and elsewhere the
// public Meshtastic server with the root of that region's LoRa band.
static void padrao_regional(const char** srv, const char** raiz)
{
  const int m = ajustes_fuso_min();
  *srv = SRV_MESHTASTIC;
  if (m == -180)                  { *srv = SRV_PADRAO; *raiz = RAIZ_PADRAO; }   // Brazil (the original default)
  else if (m >= 570 && m <= 780)  { *raiz = "msh/ANZ"; }                        // Australia, New Zealand
  else if (m == 480 && ajustes_fuso_zona() == 5) { *raiz = "msh/ANZ"; }         // Perth (tab5_fuso region)
  else if (m == 540)              { *raiz = "msh/JP"; }
  else if (m == 330)              { *raiz = "msh/IN"; }
  else if (m >= -60 && m <= 180)  { *raiz = "msh/EU_868"; }
  else if (m >= -600 && m <= -210) { *raiz = "msh/US"; }
  else                            { *raiz = "msh"; }
}

// "msh/BR_902": a raiz que o no usa. O firmware poe a regiao na raiz quando a
// regiao e escolhida; se o no mandou uma raiz, vale ela.
static void raiz_efetiva(char* out, size_t tam)
{
  // A escolha da pessoa vence tudo (ver servidor_escolhido).
  { char sv[64], us[32], pw[32]; if (servidor_escolhido(sv, sizeof sv, us, sizeof us, pw, sizeof pw, out, tam)) { return; } }
  // Sem servidor aprendido do no vale o padrao, com a raiz dele.
  if (!s_cfg.srv[0]) { const char* sv; const char* rz; padrao_regional(&sv, &rz); snprintf(out, tam, "%s", rz); return; }
  if (s_cfg.raiz[0]) { snprintf(out, tam, "%s", s_cfg.raiz); return; }
  const int r = s_cfg.regiao;
  if (r > 0 && r < (int)(sizeof(REGIOES) / sizeof(REGIOES[0]))) { snprintf(out, tam, "msh/%s", REGIOES[r]); }
  else { snprintf(out, tam, "%s", RAIZ_PADRAO); }
}

// ---------------------------------------------------------------------------
// CRIPTOGRAFIA DO CANAL - AES-CTR, a mesma do firmware (CryptoEngine.cpp)
// ---------------------------------------------------------------------------
// psk de 1 byte e um INDICE: 0 = sem cifra, 1 = a chave padrao, N = a padrao
// com o ultimo byte somado de N-1. 16 ou 32 bytes = AES-128/256 com ela.
static const uint8_t CHAVE_PADRAO[16] = {
  0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static size_t chave_do_canal(const canal_t& c, uint8_t chave[32])
{
  if (c.n_psk == 1) {
    if (c.psk[0] == 0) { return 0; }
    memcpy(chave, CHAVE_PADRAO, 16);
    chave[15] = (uint8_t)(chave[15] + c.psk[0] - 1);
    return 16;
  }
  if (c.n_psk == 16 || c.n_psk == 32) { memcpy(chave, c.psk, c.n_psk); return c.n_psk; }
  return 0;
}

// O "channel" de um pacote cifrado nao e o indice, e este hash: XOR dos bytes
// do nome com o XOR dos bytes da chave.
static uint8_t hash_do_canal(const canal_t& c)
{
  uint8_t h = 0, chave[32];
  for (const char* p = c.nome; *p; ++p) { h ^= (uint8_t)*p; }
  const size_t n = chave_do_canal(c, chave);
  for (size_t i = 0; i < n; ++i) { h ^= chave[i]; }
  return h;
}

// Nonce: id do pacote (64 bits, LE) + no de origem (32 bits, LE) + zeros. O
// contador do CTR anda nos ultimos bytes - igual para cifrar e decifrar.
static bool cifrar(const canal_t& c, uint32_t id, uint32_t de, const uint8_t* in, size_t n, uint8_t* out)
{
  uint8_t chave[32];
  const size_t nk = chave_do_canal(c, chave);
  if (nk == 0) { memcpy(out, in, n); return true; }
  uint8_t nonce[16] = { }, bloco[16] = { };
  const uint64_t id64 = id;
  memcpy(nonce, &id64, 8);
  memcpy(nonce + 8, &de, 4);
  size_t desl = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, chave, (unsigned)(nk * 8)) == 0
         && mbedtls_aes_crypt_ctr(&aes, n, &desl, nonce, bloco, in, out) == 0;
  mbedtls_aes_free(&aes);
  return ok;
}

// ---------------------------------------------------------------------------
// ESTADO DO ENLACE
// ---------------------------------------------------------------------------
#define QUADRO_MAX 600
typedef struct { uint16_t n; uint8_t d[QUADRO_MAX]; } quadro_t;

static esp_mqtt_client_handle_t s_cli = nullptr;
static QueueHandle_t   s_fila = nullptr;
static volatile bool   s_conectado = false, s_conectou_agora = false;
static uint32_t        s_num = 0;
static char            s_curto[8] = "", s_longo[40] = "";
static char            s_descr[96] = "internet: stopped";

// PROXY (ver o .hpp). O que o no disse da propria configuracao MQTT vem do
// dump e mora so em RAM; a raiz e aprendida do PRIMEIRO topico que o no
// publica - e a fonte mais certa, porque e literalmente o que ele usa.
static volatile bool s_proxy = false;   // o cliente de pe e o do proxy
static bool     s_no_mqtt = false, s_no_proxy = false, s_no_tls = false;
static bool     s_downlink[MAX_CANAIS], s_uplink[MAX_CANAIS];
static char     s_px_raiz[48] = "";       // a assinada agora ("" = nenhuma)
static char     s_px_raiz_no[48] = "";    // a aprendida do topico publicado pelo no
static uint32_t s_px_subiu = 0, s_px_desceu = 0, s_px_perdeu = 0;

// Envio publicado com QoS 1 -> o PUBACK do servidor vira o "confirmado" da tela.
#define PENDENTES 8
typedef struct { int msg; uint32_t id; } pend_t;
static pend_t s_pend[PENDENTES];
static int s_prox_pend = 0;

// Duas tarefas entregam (a do cliente MQTT e a do servico, respondendo ao
// want_config): o quadro de montagem e unico e estatico (600 bytes na stack
// do servico pesariam), entao passa por uma trava.
static Trava s_trava_ent;
static Trava s_trava_env;   // ver mesh_mqtt_enviar
// FILA CHEIA: um aviso a cada 10 s, com o total - um por pacote inundava o
// log numa rajada.
static uint32_t s_perdidos = 0;
static void perdeu(QueueHandle_t)
{
  ++s_perdidos;
  static int64_t ultimo_us = 0;
  const int64_t agora = esp_timer_get_time();
  if (agora - ultimo_us >= 10000000) {
    ultimo_us = agora;
    ESP_LOGW(TAG, "fila cheia - %lu pacote(s) descartado(s) desde o boot", (unsigned long)s_perdidos);
  }
}

// MONTAGEM SEM PILHA. A tarefa do cliente MQTT tem pilha justa: o quadro e
// montado DIRETO no quadro estatico que vai para a fila, sob s_trava_ent -
// nenhum buffer de 600 bytes na pilha de ninguem.
static quadro_t s_q;                     // o que vai para a fila (xQueueSend copia)
static uint8_t  s_pacote[QUADRO_MAX];    // MeshPacket em montagem (entregar_pacote)

// Com s_trava_ent tomada.
static bool enfileirar(QueueHandle_t fila, size_t n)
{
  if (!n) { return false; }
  s_q.n = (uint16_t)n;
  const bool ok = xQueueSend(fila, &s_q, 0) == pdTRUE;
  if (!ok) { perdeu(fila); }
  return ok;
}

static bool entregar_em(QueueHandle_t fila, const uint8_t* d, size_t n)
{
  if (!fila || n > QUADRO_MAX) { return false; }
  s_trava_ent.travar();
  memcpy(s_q.d, d, n);
  const bool ok = enfileirar(fila, n);
  s_trava_ent.destravar();
  return ok;
}
static bool entregar(const uint8_t* d, size_t n) { return entregar_em(s_fila, d, n); }

// FromRadio{ campo: corpo } para o servico, como o radio entregaria.
static bool entregar_from_radio(QueueHandle_t fila, int campo, const pb_escritor_t* corpo)
{
  if (!fila) { return false; }
  s_trava_ent.travar();
  pb_escritor_t f = pb_escritor(s_q.d, sizeof(s_q.d));
  pb_w_sub(&f, campo, corpo);
  const bool ok = enfileirar(fila, pb_tamanho(&f));
  s_trava_ent.destravar();
  return ok;
}

// UM PACOTE COMO O RADIO O ENTREGARIA, a partir de um Data ja montado:
// FromRadio{ 2: packet MeshPacket{ 1: from, 2: to, 4: decoded, 6: id,
// 7: rx_time, 14: via_mqtt } }. 'da_internet' carimba a hora de chegada e o
// via_mqtt (o que veio do servidor); a confirmacao do PUBACK vai sem os dois,
// como a do radio.
static bool entregar_pacote(QueueHandle_t fila, uint32_t de, uint32_t para,
                            const pb_escritor_t* data, bool da_internet)
{
  if (!fila) { return false; }
  s_trava_ent.travar();
  pb_escritor_t w = pb_escritor(s_pacote, sizeof(s_pacote));
  pb_w_fixo32(&w, 1, de);
  pb_w_fixo32(&w, 2, para);
  pb_w_sub(&w, 4, data);
  pb_w_fixo32(&w, 6, esp_random() | 1u);
  if (da_internet) {
    if (const uint32_t agora = tab5_hora_epoch()) { pb_w_fixo32(&w, 7, agora); }
    pb_w_num(&w, 14, 1);
  }
  pb_escritor_t f = pb_escritor(s_q.d, sizeof(s_q.d));
  pb_w_sub(&f, 2, &w);
  const bool ok = enfileirar(fila, pb_tamanho(&f));
  s_trava_ent.destravar();
  return ok;
}

static int canal_por_nome(const char* nome, size_t n)
{
  for (int i = 0; i < MAX_CANAIS; ++i) {
    const canal_t& c = s_cfg.canais[i];
    if (c.papel && strlen(c.nome) == n && memcmp(c.nome, nome, n) == 0) { return i; }
  }
  return -1;
}

// Relatorio de mapa (<raiz>/2/map/): vai sem cifra e traz nome, papel, modelo e
// posicao do no - o jeito de conhecer os nos que nao mandam NodeInfo pelo canal.
// Vira um NodeInfo e uma Position, como se o radio os tivesse ouvido.
static void processar_mapa(const uint8_t* d, size_t n)
{
  pb_leitor_t l = pb_leitor(d, n);
  pb_campo_t c;
  const uint8_t* mp = nullptr; size_t n_mp = 0;
  while (pb_campo(&l, &c)) { if (c.num == 1 && c.tipo == 2) { mp = c.val; n_mp = c.n_val; } }
  if (!mp) { return; }
  uint32_t de = 0;
  const uint8_t* dec = nullptr; size_t n_dec = 0;
  l = pb_leitor(mp, n_mp);
  while (pb_campo(&l, &c)) {
    if (c.num == 1 && c.tipo == 5) { de = (uint32_t)c.v; }
    else if (c.num == 4 && c.tipo == 2) { dec = c.val; n_dec = c.n_val; }
  }
  if (!dec || !de || de == s_num) { return; }
  const uint8_t* rel = nullptr; size_t n_rel = 0;
  uint64_t porta = 0;
  l = pb_leitor(dec, n_dec);
  while (pb_campo(&l, &c)) {
    if (c.num == 1 && c.tipo == 0) { porta = c.v; }
    else if (c.num == 2 && c.tipo == 2) { rel = c.val; n_rel = c.n_val; }
  }
  if (porta != 73 || !rel) { return; }   // MAP_REPORT_APP

  const uint8_t* longo = nullptr; size_t n_longo = 0;
  const uint8_t* curto = nullptr; size_t n_curto = 0;
  uint64_t papel = 0, modelo = 0;
  uint32_t lat = 0, lon = 0;
  l = pb_leitor(rel, n_rel);
  while (pb_campo(&l, &c)) {
    if      (c.num == 1  && c.tipo == 2) { longo = c.val; n_longo = c.n_val; }
    else if (c.num == 2  && c.tipo == 2) { curto = c.val; n_curto = c.n_val; }
    else if (c.num == 3  && c.tipo == 0) { papel = c.v; }
    else if (c.num == 4  && c.tipo == 0) { modelo = c.v; }
    else if (c.num == 9  && c.tipo == 5) { lat = (uint32_t)c.v; }
    else if (c.num == 10 && c.tipo == 5) { lon = (uint32_t)c.v; }
  }
  if (!n_longo && !n_curto) { return; }

  char id[12];
  snprintf(id, sizeof(id), "!%08lx", (unsigned long)de);

  // NodeInfo: Data{ 1: NODEINFO_APP, 2: User{ 1: id, 2: long_name,
  // 3: short_name, 5: hw_model, 7: role } }
  static uint8_t user[128], dados[160];   // so a tarefa do cliente le mapa: estatico, fora da pilha
  pb_escritor_t wu = pb_escritor(user, sizeof(user));
  pb_w_texto(&wu, 1, id, sizeof(id));
  if (n_longo) { pb_w_bytes(&wu, 2, longo, n_longo); }
  if (n_curto) { pb_w_bytes(&wu, 3, curto, n_curto); }
  if (modelo)  { pb_w_num(&wu, 5, modelo); }
  if (papel)   { pb_w_num(&wu, 7, papel); }
  pb_escritor_t wd = pb_escritor(dados, sizeof(dados));
  pb_w_num(&wd, 1, 4);
  pb_w_sub(&wd, 2, &wu);
  entregar_pacote(s_fila, de, MESH_PB_DIFUSAO, &wd, true);

  // Position: Data{ 1: POSITION_APP, 2: Position{ 1: lat, 2: lon } }
  if (lat || lon) {
    uint8_t pos[24];
    pb_escritor_t wp = pb_escritor(pos, sizeof(pos));
    pb_w_fixo32(&wp, 1, lat);
    pb_w_fixo32(&wp, 2, lon);
    wd = pb_escritor(dados, sizeof(dados));
    pb_w_num(&wd, 1, 3);
    pb_w_sub(&wd, 2, &wp);
    entregar_pacote(s_fila, de, MESH_PB_DIFUSAO, &wd, true);
  }
}

// ServiceEnvelope{1: packet, 2: channel_id, 3: gateway_id} -> FromRadio.
// Anel de (origem ^ id) ja vistos, mexido so pela tarefa do cliente.
#define VISTOS 128
static uint32_t s_vistos[VISTOS];
static int      s_prox_visto = 0;

static bool ja_visto(uint32_t de, uint32_t id)
{
  if (id == 0) { return false; }
  const uint32_t ch = (de * 2654435761u) ^ id;
  for (int i = 0; i < VISTOS; ++i) { if (s_vistos[i] == ch) { return true; } }
  s_vistos[s_prox_visto] = ch;
  s_prox_visto = (s_prox_visto + 1) % VISTOS;
  return false;
}

static void processar(const char* topico, int n_top, const uint8_t* d, size_t n, QueueHandle_t fila)
{
  for (int i = 0; i + 7 <= n_top; ++i) {
    if (memcmp(topico + i, "/2/map/", 7) == 0) { processar_mapa(d, n); return; }
  }
  // O canal pelo topico: <raiz>/2/e/<canal>/!<gateway>
  const char* e = nullptr;
  for (int i = 0; i + 5 <= n_top; ++i) { if (memcmp(topico + i, "/2/e/", 5) == 0) { e = topico + i + 5; } }
  if (!e) { return; }
  const char* fim_nome = e;
  while (fim_nome < topico + n_top && *fim_nome != '/') { ++fim_nome; }

  // Estatico, e nao pilha: so a tarefa do cliente chega aqui.
  static uint8_t claro[QUADRO_MAX], pkt[QUADRO_MAX];
  travar_cfg();
  const int idx = canal_por_nome(e, (size_t)(fim_nome - e));
  const canal_t canal = idx >= 0 ? s_cfg.canais[idx] : canal_t{ };
  destravar_cfg();
  if (idx < 0) { return; }

  pb_leitor_t l = pb_leitor(d, n);
  pb_campo_t c;
  const uint8_t* mp = nullptr; size_t n_mp = 0;
  while (pb_campo(&l, &c)) { if (c.num == 1 && c.tipo == 2) { mp = c.val; n_mp = c.n_val; } }
  if (!mp) { return; }

  uint32_t de = 0, id = 0;
  const uint8_t* cif = nullptr; size_t n_cif = 0;
  const uint8_t* dec = nullptr; size_t n_dec = 0;
  l = pb_leitor(mp, n_mp);
  while (pb_campo(&l, &c)) {
    if      (c.num == 1 && c.tipo == 5) { de = (uint32_t)c.v; }
    else if (c.num == 6 && c.tipo == 5) { id = (uint32_t)c.v; }
    else if (c.num == 5 && c.tipo == 2) { cif = c.val; n_cif = c.n_val; }
    else if (c.num == 4 && c.tipo == 2) { dec = c.val; n_dec = c.n_val; }
  }
  if (de == s_num && s_num) { return; }   // o nosso proprio envio voltando pelo servidor
  // O MESMO PACOTE DE VARIOS GATEWAYS: cada no com MQTT que o ouviu publica a
  // sua copia. Num servidor movimentado sao 3 de cada 4 mensagens (medido:
  // 75% repetidas) - e cada uma era decifrada, enfileirada e lida pelo
  // servico a toa. Corta aqui, antes do AES, pelo par (origem, id).
  if (ja_visto(de, id)) { return; }
  if (cif) {
    if (n_cif > QUADRO_MAX || !cifrar(canal, id, de, cif, n_cif, claro)) { return; }
    // Chave errada (ou mensagem direta com PKI) da lixo: um Data de verdade
    // comeca pelo campo 1 (portnum, varint).
    if (n_cif == 0 || claro[0] != 0x08) { return; }
    dec = claro; n_dec = n_cif;
  }
  if (!dec) { return; }

  // MeshPacket de novo, como o radio entregaria: o resto dos campos cru, o
  // canal como INDICE, o Data decifrado, a hora de chegada e via_mqtt.
  pb_escritor_t w = pb_escritor(pkt, QUADRO_MAX);
  l = pb_leitor(mp, n_mp);
  while (pb_campo(&l, &c)) {
    if (c.num == 3 || c.num == 4 || c.num == 5 || c.num == 7 || c.num == 14) { continue; }
    pb_w_cru(&w, c.ini, c.n_total);
  }
  pb_w_num(&w, 3, (uint64_t)idx);
  pb_w_bytes(&w, 4, dec, n_dec);
  if (const uint32_t agora = tab5_hora_epoch()) { pb_w_fixo32(&w, 7, agora); }
  pb_w_num(&w, 14, 1);                 // via_mqtt (o 13 e o obsoleto 'delayed')
  entregar_from_radio(fila, 2, &w);   // FromRadio.packet
}

// O PUBACK do nosso envio: um Routing com erro 0 para o id, como o radio
// manda quando um vizinho repete o pacote - aqui quer dizer "o servidor
// recebeu".
static void confirmar(uint32_t id, QueueHandle_t fila)
{
  uint8_t data[24];
  pb_escritor_t d = pb_escritor(data, sizeof(data));
  pb_w_num(&d, 1, 5);                                // portnum ROUTING_APP
  const uint8_t routing[] = { 0x18, 0x00 };          // Routing{ 3: error_reason = NONE }
  pb_w_bytes(&d, 2, routing, sizeof(routing));
  pb_w_fixo32(&d, 6, id);                            // request_id
  entregar_pacote(fila, s_num, s_num, &d, false);
}

static void assinar(void)
{
  char raiz[48], topico[96];
  travar_cfg();
  raiz_efetiva(raiz, sizeof(raiz));
  for (int i = 0; i < MAX_CANAIS; ++i) {
    if (!s_cfg.canais[i].papel || !s_cfg.canais[i].nome[0]) { continue; }
    snprintf(topico, sizeof(topico), "%s/2/e/%s/#", raiz, s_cfg.canais[i].nome);
    esp_mqtt_client_subscribe(s_cli, topico, 0);
    ESP_LOGI(TAG, "assinando %s", topico);
    // Sub-raizes (msh/BR/SP/2/e/...): um nivel abaixo, onde os estados e
    // cidades publicam. Nao tem cifra diferente: e o mesmo canal.
    snprintf(topico, sizeof(topico), "%s/+/2/e/%s/#", raiz, s_cfg.canais[i].nome);
    esp_mqtt_client_subscribe(s_cli, topico, 0);
    snprintf(topico, sizeof(topico), "%s/+/+/2/e/%s/#", raiz, s_cfg.canais[i].nome);   // meshdev/<pais>/<regiao>/2/e/...
    esp_mqtt_client_subscribe(s_cli, topico, 0);
  }
  // Relatorios de mapa: nomes e posicoes de nos que nunca mandaram NodeInfo.
  snprintf(topico, sizeof(topico), "%s/2/map/", raiz);
  esp_mqtt_client_subscribe(s_cli, topico, 0);
  snprintf(topico, sizeof(topico), "%s/+/2/map/", raiz);
  esp_mqtt_client_subscribe(s_cli, topico, 0);
  snprintf(topico, sizeof(topico), "%s/+/+/2/map/", raiz);
  esp_mqtt_client_subscribe(s_cli, topico, 0);
  destravar_cfg();
}

// AS ASSINATURAS DO PROXY: as mesmas do app do celular - cada canal com
// downlink ligado no no, mais o topico PKI das mensagens diretas (o no
// decifra; para o Tab5 sao bytes). Canal sem downlink nao e assinado: o no
// descartaria o que viesse dele, e o trafego entraria a toa.
static void assinar_proxy(const char* raiz, bool desfazer)
{
  char topico[96];
  travar_cfg();
  for (int i = 0; i < MAX_CANAIS; ++i) {
    if (!s_cfg.canais[i].papel || !s_cfg.canais[i].nome[0] || !s_downlink[i]) { continue; }
    snprintf(topico, sizeof(topico), "%s/2/e/%s/+", raiz, s_cfg.canais[i].nome);
    if (desfazer) { esp_mqtt_client_unsubscribe(s_cli, topico); }
    else          { esp_mqtt_client_subscribe(s_cli, topico, 0); ESP_LOGI(TAG, "proxy assinando %s", topico); }
  }
  destravar_cfg();
  snprintf(topico, sizeof(topico), "%s/2/e/PKI/+", raiz);
  if (desfazer) { esp_mqtt_client_unsubscribe(s_cli, topico); }
  else          { esp_mqtt_client_subscribe(s_cli, topico, 0); }
}

// A raiz do proxy: a que o no usa de fato (aprendida do topico), senao a do
// dump, senao "msh" - o padrao do firmware.
static void raiz_proxy(char* out, size_t tam)
{
  if (s_px_raiz_no[0]) { snprintf(out, tam, "%s", s_px_raiz_no); return; }
  travar_cfg();
  snprintf(out, tam, "%s", s_cfg.raiz[0] ? s_cfg.raiz : "msh");
  destravar_cfg();
}

static void assinar_proxy_agora(void)
{
  char raiz[48];
  raiz_proxy(raiz, sizeof(raiz));
  if (s_px_raiz[0] && strcmp(s_px_raiz, raiz) != 0) { assinar_proxy(s_px_raiz, true); }
  snprintf(s_px_raiz, sizeof(s_px_raiz), "%s", raiz);
  assinar_proxy(raiz, false);
}

// DESCIDA: o que chegou do servidor vira ToRadio{ 6: MqttClientProxyMessage
// { 1 topic, 2 data, 4 retained } } e vai para a fila; o servico entrega ao
// no. 512 e o teto de um ToRadio no firmware (MAX_TO_FROM_RADIO_SIZE), pelo
// cabo e pelo Bluetooth.
static void proxy_descer(const char* topico, int n_top, const uint8_t* d, size_t n, bool retido)
{
  static uint8_t msg[QUADRO_MAX], saida[QUADRO_MAX];   // so a tarefa do cliente MQTT desce
  pb_escritor_t m = pb_escritor(msg, sizeof(msg));
  pb_w_bytes(&m, 1, topico, (size_t)n_top);
  pb_w_bytes(&m, 2, d, n);
  if (retido) { pb_w_num(&m, 4, 1); }
  pb_escritor_t f = pb_escritor(saida, 512);
  pb_w_sub(&f, 6, &m);
  const size_t k = pb_tamanho(&f);
  if (k && entregar(saida, k)) { ++s_px_desceu; } else { ++s_px_perdeu; }
}

static void ao_evento(void*, esp_event_base_t, int32_t id_ev, void* dados)
{
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)dados;
  switch ((esp_mqtt_event_id_t)id_ev) {
    case MQTT_EVENT_CONNECTED:
      if (!s_conectado) { tab5_mem_marco("MQTT conectado"); }
      s_conectado = true;
      s_conectou_agora = true;
      if (s_proxy) { s_px_raiz[0] = 0; assinar_proxy_agora(); }   // sessao nova: nada assinado
      else         { assinar(); }
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_conectado = false;
      break;
    case MQTT_EVENT_DATA:
      // Pacote em pedacos (maior que o buffer) nao e Meshtastic de verdade.
      if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
        if (s_proxy) { proxy_descer(ev->topic, ev->topic_len, (const uint8_t*)ev->data, (size_t)ev->data_len, ev->retain); }
        else         { processar(ev->topic, ev->topic_len, (const uint8_t*)ev->data, (size_t)ev->data_len, s_fila); }
      } else if (s_proxy && ev->current_data_offset == 0) { ++s_px_perdeu; }
      break;
    case MQTT_EVENT_PUBLISHED:
      for (int i = 0; i < PENDENTES; ++i) {
        if (s_pend[i].msg == ev->msg_id && s_pend[i].id) { confirmar(s_pend[i].id, s_fila); s_pend[i].id = 0; }
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void mesh_mqtt_definir_no(uint32_t num, const char* curto, const char* longo)
{
  s_num = num;
  snprintf(s_curto, sizeof(s_curto), "%s", curto ? curto : "");
  snprintf(s_longo, sizeof(s_longo), "%s", longo ? longo : "");
}

// Os dois papeis sobem pelo mesmo caminho; muda o servidor, a identidade e o
// que se assina (no CONNECTED). Com o cliente de pe no OUTRO papel, recusa:
// quem troca de papel e o laco do servico, parando antes.
static bool iniciar_cliente(bool proxy)
{
  if (s_cli) { return s_proxy == proxy; }
  // 16 quadros: a descida do proxy chega em rajada (um servidor publico
  // entrega o canal inteiro), e o enlace com o no esvazia a fila a cada volta.
  if (!s_fila) { s_fila = xQueueCreateWithCaps(32, sizeof(quadro_t), MALLOC_CAP_SPIRAM); }
  if (!s_fila) { return false; }
  xQueueReset(s_fila);

  static char uri[96], usr[32], pwd[32], raiz[48], srv[64];
  const bool tls = proxy && s_no_tls;
  travar_cfg();
  garantir_cfg();
  // O SERVIDOR: no proxy e o do NO, exatamente - endereco vazio e o padrao do
  // firmware (mqtt.meshtastic.org, meshdev/large4cats). No modo internet,
  // vazio e o padrao deste sistema (Mesh Brasil).
  const char* padrao = SRV_MESHTASTIC;
  if (!proxy) { const char* rz; padrao_regional(&padrao, &rz); }
  snprintf(srv, sizeof(srv), "%s", s_cfg.srv[0] ? s_cfg.srv : padrao);
  // "host" ou "host:porta", como o app do Meshtastic grava.
  snprintf(uri, sizeof(uri), strchr(srv, ':') ? (tls ? "mqtts://%s" : "mqtt://%s") : (tls ? "mqtts://%s:8883" : "mqtt://%s:1883"), srv);
  snprintf(usr, sizeof(usr), "%s", s_cfg.srv[0] ? s_cfg.usr : USR_PADRAO);
  snprintf(pwd, sizeof(pwd), "%s", s_cfg.srv[0] ? s_cfg.pwd : PWD_PADRAO);
  if (proxy) { destravar_cfg(); raiz_proxy(raiz, sizeof(raiz)); travar_cfg(); }
  else       { raiz_efetiva(raiz, sizeof(raiz)); }
  destravar_cfg();
  // MODO INTERNET COM SERVIDOR ESCOLHIDO (Ajustes > Meshtastic): vale por cima
  // do que o no ensinou e do padrao. O proxy nao muda: e sempre o do no.
  if (!proxy && servidor_escolhido(srv, sizeof(srv), usr, sizeof(usr), pwd, sizeof(pwd), raiz, sizeof(raiz))) {
    snprintf(uri, sizeof(uri), strchr(srv, ':') ? "mqtt://%s" : "mqtt://%s:1883", srv);
    ESP_LOGI(TAG, "servidor escolhido: %s (%s, raiz %s)", mesh_mqtt_servidor_nome(ajustes_mqtt_servidor()), srv, raiz);
  }

  // Identidade propria do proxy: dois clientes com o mesmo id no servidor
  // derrubariam um ao outro, e o modo internet pode ter ficado registrado.
  static char cliente[24];
  snprintf(cliente, sizeof(cliente), proxy ? "tab5px-%08lx" : "tab5-%08lx", (unsigned long)s_num);
  esp_mqtt_client_config_t c = { };
  c.broker.address.uri = uri;
  c.credentials.username = usr;
  c.credentials.authentication.password = pwd;
  c.credentials.client_id = cliente;
  c.buffer.size = 1024;
  // Pico medido de ~2,6 KB com 6 KB (pilhas-*.csv, horas conectado recebendo
  // a malha). Os quadros e a decifragem usam buffers static, nao pilha.
  c.task.stack_size = 4096;
  c.network.reconnect_timeout_ms = 15000;
  if (tls) { c.broker.verification.crt_bundle_attach = esp_crt_bundle_attach; }
  s_proxy = proxy;
  s_cli = esp_mqtt_client_init(&c);
  if (!s_cli) { return false; }
  esp_mqtt_client_register_event(s_cli, MQTT_EVENT_ANY, ao_evento, nullptr);
  if (esp_mqtt_client_start(s_cli) != ESP_OK) {
    esp_mqtt_client_destroy(s_cli);
    s_cli = nullptr;
    return false;
  }
  tab5_mem_marco("MQTT (cliente + tarefa)");
  snprintf(s_descr, sizeof(s_descr), "%s: %s (%s)", proxy ? "proxy" : "internet", srv, raiz);
  ESP_LOGI(TAG, "conectando a %s, raiz %s, como !%08lx%s", uri, raiz, (unsigned long)s_num,
           proxy ? " - proxy do no" : "");
  return true;
}

bool mesh_mqtt_iniciar(void)       { return iniciar_cliente(false); }
bool mesh_mqtt_iniciar_proxy(void) { return iniciar_cliente(true); }
bool mesh_mqtt_em_proxy(void)      { return s_cli && s_proxy; }

void mesh_mqtt_parar(void)
{
  if (!s_cli) { return; }
  esp_mqtt_client_stop(s_cli);
  esp_mqtt_client_destroy(s_cli);
  s_cli = nullptr;
  s_conectado = false;
  s_proxy = false;
  s_px_raiz[0] = 0;
  if (s_fila) { xQueueReset(s_fila); }   // descida do proxy para um no que nao vai mais receber
  snprintf(s_descr, sizeof(s_descr), "internet: stopped");
  ESP_LOGI(TAG, "desconectado do servidor");
}

bool mesh_mqtt_ativo(void)     { return s_cli != nullptr; }
bool mesh_mqtt_conectado(void) { return s_cli && s_conectado; }
bool mesh_mqtt_conectou_agora(void)
{
  if (!s_conectou_agora) { return false; }
  s_conectou_agora = false;
  return true;
}

const char* mesh_mqtt_descricao(void)
{
  static char d[112];
  snprintf(d, sizeof(d), "%s%s", s_descr, !s_cli ? "" : s_conectado ? " - connected" : " - connecting");
  return d;
}

size_t mesh_mqtt_receber(uint8_t* buf, size_t max, uint32_t espera_ms)
{
  if (!s_fila || s_proxy) { if (espera_ms) { vTaskDelay(pdMS_TO_TICKS(espera_ms)); } return 0; }
  static quadro_t q;   // so a tarefa do servico recebe
  if (xQueueReceive(s_fila, &q, pdMS_TO_TICKS(espera_ms)) != pdTRUE) { return 0; }
  const size_t n = q.n < max ? q.n : max;
  memcpy(buf, q.d, n);
  return n;
}

// O want_config de um radio: o que o dump dele traria de essencial - o nosso
// numero, o nosso NodeInfo, os canais e o fim da configuracao.
static void responder_config(void)
{
  // FromRadio{ 3: my_info MyNodeInfo{ 1: my_node_num } }
  uint8_t a[160];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, 1, s_num);
  entregar_from_radio(s_fila, 3, &w);

  // FromRadio{ 4: node_info NodeInfo{ 1: num, 2: user User{ 1: id, 2: long, 3: short } } }
  char id[12];
  snprintf(id, sizeof(id), "!%08lx", (unsigned long)s_num);
  uint8_t u[96];
  pb_escritor_t wu = pb_escritor(u, sizeof(u));
  pb_w_texto(&wu, 1, id, sizeof(id));
  if (s_longo[0]) { pb_w_texto(&wu, 2, s_longo, sizeof(s_longo)); }
  if (s_curto[0]) { pb_w_texto(&wu, 3, s_curto, sizeof(s_curto)); }
  w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, 1, s_num);
  pb_w_sub(&w, 2, &wu);
  entregar_from_radio(s_fila, 4, &w);

  travar_cfg();
  garantir_cfg();
  const cfg_t cfg = s_cfg;
  destravar_cfg();
  // FromRadio{ 10: channel Channel{ 1: index, 2: settings{ 2: psk, 3: name }, 3: role } }
  for (int i = 0; i < MAX_CANAIS; ++i) {
    const canal_t& c = cfg.canais[i];
    if (!c.papel) { continue; }
    uint8_t cs[64];
    pb_escritor_t ws = pb_escritor(cs, sizeof(cs));
    if (c.n_psk) { pb_w_bytes(&ws, 2, c.psk, c.n_psk); }
    pb_w_texto(&ws, 3, c.nome, sizeof(c.nome));
    w = pb_escritor(a, sizeof(a));
    pb_w_num(&w, 1, (uint64_t)i);
    pb_w_sub(&w, 2, &ws);
    pb_w_num(&w, 3, c.papel);
    entregar_from_radio(s_fila, 10, &w);
  }

  // FromRadio{ 7: config_complete_id = 42 } - o mesmo id que o servico pediu.
  uint8_t b[8];
  pb_escritor_t f = pb_escritor(b, sizeof(b));
  pb_w_num(&f, 7, 42);
  entregar(b, pb_tamanho(&f));
}

// Chamada da tarefa do servico (want_config, batimento) E da de interface
// (o app enviando): os buffers de montagem sao estaticos, uma de cada vez.
static bool enviar_travado(const uint8_t* toradio, size_t n);
static bool publicar_data(esp_mqtt_client_handle_t cli, pend_t* pend, int* prox, int n_pend,
                          const canal_t& canal, const char* raiz, uint32_t para,
                          const uint8_t* dec, size_t n_dec, uint32_t id, uint64_t saltos);

// O QUE O SERVICO MANDA, lido de volta: ToRadio{ 1: packet MeshPacket{ 2: to,
// 3: channel, 4: decoded, 6: id, 9: hop_limit }, 3: want_config_id }.
typedef struct {
  bool           quer_config;   // want_config_id: o servico pedindo o "dump"
  uint32_t       para, id;
  uint64_t       canal, saltos;
  const uint8_t* data;          // Data ja serializado (nullptr = sem pacote)
  size_t         n_data;
} to_radio_t;

static void ler_to_radio(const uint8_t* b, size_t n, to_radio_t* t)
{
  *t = { false, MESH_PB_DIFUSAO, 0, 0, 3, nullptr, 0 };
  pb_leitor_t l = pb_leitor(b, n);
  pb_campo_t c;
  const uint8_t* mp = nullptr; size_t n_mp = 0;
  while (pb_campo(&l, &c)) {
    if      (c.num == 1 && c.tipo == PB_LEN)    { mp = c.val; n_mp = c.n_val; }
    else if (c.num == 3 && c.tipo == PB_VARINT) { t->quer_config = true; }
  }
  if (!mp) { return; }
  l = pb_leitor(mp, n_mp);
  while (pb_campo(&l, &c)) {
    if      (c.num == 2 && c.tipo == PB_FIXO32) { t->para = (uint32_t)c.v; }
    else if (c.num == 3 && c.tipo == PB_VARINT) { t->canal = c.v; }
    else if (c.num == 4 && c.tipo == PB_LEN)    { t->data = c.val; t->n_data = c.n_val; }
    else if (c.num == 6 && c.tipo == PB_FIXO32) { t->id = (uint32_t)c.v; }
    else if (c.num == 9 && c.tipo == PB_VARINT) { t->saltos = c.v; }
  }
  if (t->id == 0) { t->id = esp_random() | 1u; }
}

bool mesh_mqtt_enviar(const uint8_t* toradio, size_t n)
{
  // NO PROXY O CLIENTE E DO NO: nada do Tab5 sai por ele como se fosse um
  // radio (cifrar com a chave do canal e publicar em nome do no daria a
  // mesma mensagem duas vezes, quando o no publicar a dele).
  if (s_proxy) { return false; }
  s_trava_env.travar();
  const bool ok = enviar_travado(toradio, n);
  s_trava_env.destravar();
  return ok;
}

static bool enviar_travado(const uint8_t* toradio, size_t n)
{
  // SEM CLIENTE NAO HA PARA QUEM RESPONDER. O servico continua pedindo a
  // configuracao (a cada 2 min) mesmo com o Wi-Fi fora; a resposta ia para a
  // fila, que so e lida com o cliente de pe, e enchia ("queue cheia").
  if (!s_cli) { return false; }
  to_radio_t t;
  ler_to_radio(toradio, n, &t);
  if (t.quer_config) { responder_config(); return true; }
  if (!t.data) { return true; }                  // batimento e afins: nada a fazer aqui
  if (!mesh_mqtt_conectado() || s_num == 0) { return false; }
  if (t.canal >= MAX_CANAIS) { return false; }
  // MENSAGEM DIRETA NAO SAI POR AQUI. Sem a chave publica (PKI) so daria para
  // cifrar com a chave do CANAL, que todo assinante do topico tem: a conversa
  // particular apareceria para a malha inteira (e no bot do Telegram). Melhor
  // recusar - o envio falha na tela - do que vazar.
  if (t.para != MESH_PB_DIFUSAO) {
    ESP_LOGW(TAG, "mensagem direta recusada: pela internet so vai mensagem de canal (falta PKI)");
    return false;
  }

  travar_cfg();
  const canal_t canal = s_cfg.canais[t.canal];
  char raiz[48];
  raiz_efetiva(raiz, sizeof(raiz));
  destravar_cfg();
  if (!canal.papel) { return false; }
  return publicar_data(s_cli, s_pend, &s_prox_pend, PENDENTES, canal, raiz, t.para, t.data, t.n_data, t.id, t.saltos);
}

// Data -> cifra -> MeshPacket -> ServiceEnvelope -> publica em
// <raiz>/2/e/<canal>/!<no>. Os dois clientes saem por aqui (buffers estaticos:
// com s_trava_env tomada).
static bool publicar_data(esp_mqtt_client_handle_t cli, pend_t* pend, int* prox, int n_pend,
                          const canal_t& canal, const char* raiz, uint32_t para,
                          const uint8_t* dec, size_t n_dec, uint32_t id, uint64_t saltos)
{
  // Data + bitfield OK_TO_MQTT (campo 9, bit 0): sem ele os gateways que
  // respeitam a privacidade nao descem a mensagem para o radio.
  static uint8_t data[QUADRO_MAX], cif[QUADRO_MAX], pkt[QUADRO_MAX], env[QUADRO_MAX];
  pb_escritor_t wdata = pb_escritor(data, sizeof(data));
  pb_w_cru(&wdata, dec, n_dec);
  pb_w_num(&wdata, 9, 1);                        // bitfield: OK_TO_MQTT
  const size_t n_data = pb_tamanho(&wdata);
  if (!n_data) { return false; }
  uint8_t chave[32];
  const bool cifrado = chave_do_canal(canal, chave) > 0;
  if (!cifrar(canal, id, s_num, data, n_data, cif)) { return false; }

  // MeshPacket{ 1: from, 2: to, 3: channel (hash), 4: decoded | 5: encrypted,
  // 6: id, 9: hop_limit, 15: hop_start }
  pb_escritor_t w = pb_escritor(pkt, sizeof(pkt));
  pb_w_fixo32(&w, 1, s_num);
  pb_w_fixo32(&w, 2, para);
  pb_w_num(&w, 3, hash_do_canal(canal));
  if (cifrado) { pb_w_bytes(&w, 5, cif, n_data); } else { pb_w_bytes(&w, 4, data, n_data); }
  pb_w_fixo32(&w, 6, id);
  pb_w_num(&w, 9, saltos);
  pb_w_num(&w, 15, saltos);                      // hop_start: a malha conta os saltos por ele

  // ServiceEnvelope{ 1: packet, 2: channel_id, 3: gateway_id }
  char gw[12];
  snprintf(gw, sizeof(gw), "!%08lx", (unsigned long)s_num);
  pb_escritor_t e = pb_escritor(env, sizeof(env));
  pb_w_sub(&e, 1, &w);
  pb_w_texto(&e, 2, canal.nome, sizeof(canal.nome));
  pb_w_texto(&e, 3, gw, sizeof(gw));
  const size_t n_env = pb_tamanho(&e);
  if (!n_env) { return false; }

  char topico[112];
  snprintf(topico, sizeof(topico), "%s/2/e/%s/%s", raiz, canal.nome, gw);
  // enqueue, e nao publish: o publish escreve no socket NA HORA e pode
  // segurar quem chamou (a tarefa de interface) ate o prazo da rede. O
  // enqueue so poe na fila do cliente, que manda na tarefa dele.
  const int msg = esp_mqtt_client_enqueue(cli, topico, (const char*)env, (int)n_env, 1, 0, true);
  if (msg < 0) { return false; }
  pend[*prox].msg = msg;
  pend[*prox].id  = id;
  *prox = (*prox + 1) % n_pend;
  ESP_LOGI(TAG, "publicado em %s (id %08lx)", topico, (unsigned long)id);
  return true;
}

// ---------------------------------------------------------------------------
// PROXY - API
// ---------------------------------------------------------------------------
void mesh_mqtt_aprender_no_mqtt(bool ligado, bool proxy, bool tls)
{
  s_no_mqtt = ligado; s_no_proxy = proxy; s_no_tls = tls;
}

void mesh_mqtt_aprender_downlink(int idx, bool downlink)
{
  if (idx >= 0 && idx < MAX_CANAIS) { s_downlink[idx] = downlink; }
}

void mesh_mqtt_aprender_uplink(int idx, bool uplink)
{
  if (idx >= 0 && idx < MAX_CANAIS) { s_uplink[idx] = uplink; }
}

void mesh_mqtt_no_resumo(char* out, size_t tam)
{
  travar_cfg();
  garantir_cfg();
  size_t k = (size_t)snprintf(out, tam, "node MQTT: %s, proxy %s, tls %s, server \"%s\" root \"%s\"\n",
                              s_no_mqtt ? "LIGADO" : "desligado", s_no_proxy ? "ligado" : "desligado",
                              s_no_tls ? "sim" : "nao", s_cfg.srv[0] ? s_cfg.srv : "(firmware default)",
                              s_cfg.raiz[0] ? s_cfg.raiz : "(padrao)");
  for (int i = 0; i < MAX_CANAIS && k < tam; ++i) {
    if (!s_cfg.canais[i].papel) { continue; }
    // A CHAVE: so o tipo, nunca o valor. "padrao" = psk de 1 byte igual a 1, a
    // que todo LongFast de fabrica usa - e a unica que a malha publica le.
    const canal_t& c = s_cfg.canais[i];
    char chave[24];
    if (c.n_psk == 0)                        { snprintf(chave, sizeof(chave), "NO encryption"); }
    else if (c.n_psk == 1 && c.psk[0] == 1)  { snprintf(chave, sizeof(chave), "padrao"); }
    else if (c.n_psk == 1)                   { snprintf(chave, sizeof(chave), "padrao+%u (DIFERENTE)", (unsigned)(c.psk[0] - 1)); }
    else                                     { snprintf(chave, sizeof(chave), "custom, %u bytes", (unsigned)c.n_psk); }
    k += (size_t)snprintf(out + k, tam - k, "  canal %d %-11s sobe %s  desce %s  chave %s\n", i,
                          c.nome, s_uplink[i] ? "sim" : "NAO", s_downlink[i] ? "sim" : "NAO", chave);
  }
  destravar_cfg();
}

bool mesh_mqtt_no_tem_proxy(void) { return s_no_mqtt && s_no_proxy; }

// "msh/BR/2/e/LongFast/!a1b2c3d4" -> "msh/BR": tudo antes do primeiro "/2/".
static bool raiz_do_topico(const char* t, char* out, size_t tam)
{
  const char* p = strstr(t, "/2/");
  if (!p || p == t || (size_t)(p - t) >= tam) { return false; }
  memcpy(out, t, (size_t)(p - t));
  out[p - t] = 0;
  return true;
}

bool mesh_mqtt_proxy_publicar(const char* topico, const uint8_t* d, size_t n, bool retido)
{
  if (!s_proxy || !mesh_mqtt_conectado() || !topico || !topico[0]) { ++s_px_perdeu; return false; }
  // A raiz que o no usa de verdade. Mudou (ou era o palpite do dump): refaz
  // as assinaturas na raiz certa.
  char raiz[48];
  if (raiz_do_topico(topico, raiz, sizeof(raiz)) && strcmp(raiz, s_px_raiz_no) != 0) {
    snprintf(s_px_raiz_no, sizeof(s_px_raiz_no), "%s", raiz);
    if (strcmp(raiz, s_px_raiz) != 0) {
      ESP_LOGI(TAG, "proxy: o no publica na raiz %s - assinando nela", raiz);
      assinar_proxy_agora();
    }
  }
  // enqueue: nao segura a tarefa do servico esperando a rede (ver enviar_travado).
  const int msg = esp_mqtt_client_enqueue(s_cli, topico, (const char*)d, (int)n, 1, retido ? 1 : 0, true);
  if (msg < 0) { ++s_px_perdeu; return false; }
  ++s_px_subiu;
  return true;
}

size_t mesh_mqtt_proxy_receber(uint8_t* buf, size_t max)
{
  if (!s_fila || !s_proxy) { return 0; }
  static quadro_t q;   // so a tarefa do servico recebe
  if (xQueueReceive(s_fila, &q, 0) != pdTRUE) { return 0; }
  if (q.n > max) { ++s_px_perdeu; return 0; }
  memcpy(buf, q.d, q.n);
  return q.n;
}

void mesh_mqtt_proxy_contadores(uint32_t* subiu, uint32_t* desceu, uint32_t* perdeu)
{
  if (subiu)  { *subiu  = s_px_subiu; }
  if (desceu) { *desceu = s_px_desceu; }
  if (perdeu) { *perdeu = s_px_perdeu; }
}
