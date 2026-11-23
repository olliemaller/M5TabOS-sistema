// APP Mesh - the "NO" (NODE) page: configure the Meshtastic node from here (groups, internet,
// journal, the node's Wi-Fi network, web). Split from app_mesh.cpp (the app cycle); what
// the parts share is in app_mesh_interno.hpp.

#include "idioma.h"
#include "tab5_hash.hpp"
#include "tab5_mesh_admin.hpp"
#include "tab5_mesh_diario.hpp"
#include "tab5_mesh_wifi_no.hpp"
#include "tab5_tela.hpp"
#include "tab5_hora.hpp"
#include <ctime>
#include "tab5_ui.hpp"
#include "ajustes.h"
#include "tab5_gps.hpp"
#include "tab5_web.hpp"
#include "tab5_mesh_web.hpp"
#include "painel_web.hpp"
#include <stdio.h>
#include <string.h>
#include "app_mesh_interno.hpp"
#include "tab5_tema.hpp"

#define APP_MESH_NOME "Mesh"   // owner of the web server (the same as in tab5_mesh_web_ligar)

// ---------------------------------------------------------------------------
// "NO" (NODE) PAGE - configure the Meshtastic node from here (services/tab5_mesh_admin)
// ---------------------------------------------------------------------------
// Each row carries an icon of what it does (icone_cfg). Tapping does NOT save: it changes
// the PENDING value (yellow, with a dot). "GRAVAR NO NO" (write to node) sends everything
// in a single transaction - the node writes and restarts ONCE, however many options change.
// Writing on every tap would restart the node at every step of a cycle (3, 4, 5...).
//
// After writing, the screen shows what the NODE confirmed in the next dump, never
// what we asked for: if it refuses something, the old value keeps showing.
//
// Immediate actions (no restart): notification sound and Tab5 GPS to the node
// (both on the Tab5), fixed position from the Tab5 GPS, setting the node's clock.
// Restart asks for confirmation.
//
// IN GROUPS. The page opens on the list of GROUPS (Name and role, Location,
// Radio, Connections, Notices, Maintenance), each with a summary of what is
// inside; tapping opens the group with its rows together, and BACK goes up. The
// pending state applies to the whole page: changing something in two groups and writing
// once is still ONE transaction (the footer appears at both levels).
enum {
  RC_SOM = 0, RC_LONGO, RC_CURTO, RC_PAPEL, RC_SALTOS, RC_TX,
  RC_LOC_PRECISAO, RC_LOC_TAB5, RC_POS_S, RC_POS_INTELIGENTE, RC_GPS_NO, RC_POS_FIXA,
  RC_NODEINFO_S, RC_TELE_S, RC_MQTT, RC_PROXY, RC_BT, RC_WIFI, RC_HORA, RC_REINICIAR,
  RC_TELA_S, RC_UPLINK, RC_OK_MQTT,
  RC_I_FIRMWARE, RC_I_REGIAO, RC_I_NOS, RC_I_REINICIOS, RC_I_AVISO,
  RC_TOTAL
};
static const int RC_EDITAVEIS = RC_I_FIRMWARE;   // from here down, read only
// Rows that belong to the TAB5, not the node: they change at once, with or without the node.
static bool rc_do_tab5(int i) { return i == RC_SOM || i == RC_LOC_TAB5; }

// G_INTERNET belongs to the TAB5, not the node (MQTT server and "internet when the node
// is missing"): it opens its own sub-page (s_net_pg) and works with no node at all.
// G_PC too: Mesh in the PC's browser (services/tab5_mesh_web), in the
// s_web_pg sub-page with the system panel (kernel/painel_web).
enum { G_INTERNET = 0, G_PC, G_NOME, G_LOCAL, G_RADIO, G_CONEXOES, G_AVISOS, G_MANUT, G_TOTAL };
typedef struct { const char* nome; int8_t icone; int8_t n; int8_t rc[6]; } grupo_cfg_t;
static const grupo_cfg_t GRUPOS[G_TOTAL] = {
  { N_("Internet do Tab5 (MQTT)"), RC_MQTT,  0, { } },
  { N_("No navegador do PC"),  RC_WIFI,      0, { } },
  { N_("Nome e papel"),        RC_LONGO,     4, { RC_LONGO, RC_CURTO, RC_PAPEL, RC_NODEINFO_S } },
  { N_("Localização"),         RC_POS_S,     6, { RC_LOC_PRECISAO, RC_LOC_TAB5, RC_POS_S, RC_POS_INTELIGENTE,
                                                RC_GPS_NO, RC_POS_FIXA } },
  { N_("Rádio LoRa"),          RC_TX,        3, { RC_I_REGIAO, RC_SALTOS, RC_TX } },
  { N_("Conexões e internet"), RC_WIFI,      6, { RC_BT, RC_WIFI, RC_MQTT, RC_UPLINK, RC_OK_MQTT, RC_PROXY } },
  { N_("Avisos e tela"),       RC_SOM,       4, { RC_SOM, RC_TELA_S, RC_TELE_S, RC_I_AVISO } },
  { N_("Manutenção"),          RC_REINICIAR, 5, { RC_HORA, RC_REINICIAR, RC_I_FIRMWARE, RC_I_NOS, RC_I_REINICIOS } },
};
int s_grupo = -1;   // -1 = the list of groups
static int cfg_n_itens(void) { return s_grupo < 0 ? G_TOTAL : GRUPOS[s_grupo].n; }
// Wi-Fi text editing modes (s_editando): outside the row numbering.
enum { ED_WIFI_SSID = 100, ED_WIFI_SENHA = 101 };
// ...and the five fields of an MQTT server (internet sub-page).
enum { ED_MQ_NOME = 102, ED_MQ_SRV, ED_MQ_USR, ED_MQ_PWD, ED_MQ_RAIZ };
static bool ed_mq(int e) { return e >= ED_MQ_NOME && e <= ED_MQ_RAIZ; }

static const int CFG_LIN   = 68;
static const int CFG_TOPO  = 80;     // page header (the groups' BACK fits in it)
static const int CFG_RODAPE = 88;    // strip of the GRAVAR / DESCARTAR (write / discard) buttons
static const int CFG_BTN_L = 360, CFG_BTN_A = 64;
// Width of the GRAVAR/DESCARTAR buttons: 360 in landscape; in portrait, whatever fits side
// by side. Drawing and touch use this same function.
static int cfg_btn_l(const area_t& a)
{
  const int cabe = (a.largura - TRILHO_L - 3 * 24) / 2;
  return cabe < CFG_BTN_L ? cabe : CFG_BTN_L;
}

lista_t            s_l_cfg;
lista_t            s_l_redes;   // the node's Wi-Fi networks (scrolls: there may be many)
static mesh_cfg_t         s_cfg;            // what the node confirmed (copied per pass)
static mesh_cfg_mudanca_t s_mud;            // what is pending
static char               s_cfg_ant[RC_TOTAL][112];
// The row has been drawn at SOME point since the last full pass. Without
// this, a row outside the window had an empty signature, "changed" on every
// pass, and its mark (lista_item_mudou marks a RANGE, from the first
// to the last item) dragged along all the visible rows between them: each
// drag sample redrew the whole window over what the scrolling
// had just shifted - and what was left of that were trails at the edges.
static bool               s_cfg_pintada[RC_TOTAL];
int                s_editando = 0;   // 0 = nothing; RC_LONGO or RC_CURTO
static char               s_edit_txt[66];   // the largest field: Wi-Fi password (64)
tecvirt_t          s_kb_cfg;
// RESTART JOURNAL: full sub-page opened by tapping "reinicios do no" (node restarts).
bool               s_diario = false;
// IN THE PC BROWSER: sub-page of the G_PC group (the web server panel).
bool               s_web_pg = false;
// THE NODE'S WI-FI NETWORKS: sub-page with the networks already used, to choose from.
bool               s_redes_pg = false;
static char               s_redes_msg[72];
// THE TAB5'S INTERNET: sub-page of the G_INTERNET group - the list of MQTT
// servers and, with s_net_ed, the editor for one of them (s_net_p, the copy being
// edited; s_net_i = 1..n, 0 = new). It is the same setting as Settings > Mesh over the
// internet: the list belongs to the service (mesh_mqtt_*).
bool                   s_net_pg = false;
bool                   s_net_ed = false;
static int                    s_net_i = 0;
static int                    s_net_modelo = 0;     // the template applied (only on a new one)
static uint32_t               s_net_apagar_ms = 0;  // 1st tap on APAGAR (delete): confirm within 4 s
mesh_mqtt_servidor_t   s_net_p;
static mesh_reinicio_t    s_rei[MESH_DIARIO_MAX];
static int                s_rei_n = 0;
static uint32_t           s_limpar_ms = 0;      // 1st tap on LIMPAR (clear): confirm within 4 s
static uint32_t           s_reiniciar_ms = 0;   // 1st tap on REINICIAR (restart): confirm within 4 s
char               s_cfg_msg[64];        // short response from the last action

typedef struct { int32_t v; const char* nome; } opcao_t;
#define N_OPC(x) ((int)(sizeof(x) / sizeof((x)[0])))

// Roles offered. REPEATER and ROUTER_CLIENT are left out: obsolete in the
// current firmware. The others make no sense for a node attached to a screen.
static const opcao_t OPC_PAPEL[] = {
  { 0, N_("cliente") }, { 1, N_("cliente mudo (não repete)") }, { 8, N_("cliente oculto") },
  { 5, N_("rastreador") }, { 6, N_("sensor") }, { 2, N_("roteador") }, { 11, N_("roteador tardio") },
};
static const opcao_t OPC_SALTOS[] = { {1,"1"},{2,"2"},{3,"3"},{4,"4"},{5,"5"},{6,"6"},{7,"7"} };
static const opcao_t OPC_TX[] = {
  { 0, N_("máximo da região") }, { 10, N_("10 dBm") }, { 14, N_("14 dBm") }, { 17, N_("17 dBm") },
  { 20, "20 dBm" }, { 22, "22 dBm" }, { 27, "27 dBm" }, { 30, "30 dBm" },
};
// LOCATION ON THE MESH = precision of the primary channel (see mesh_cfg_t.precisao).
static const opcao_t OPC_PRECISAO[] = {
  { 0, N_("não compartilhar") }, { 32, N_("exata") }, { 18, N_("aproximada (~90 m)") },
  { 16, N_("aproximada (~360 m)") }, { 14, N_("aproximada (~1,5 km)") }, { 12, N_("só a cidade (~6 km)") },
};
// Every how many minutes the Tab5 GPS goes to the node (ajustes.h).
static const opcao_t OPC_TAB5[] = {
  { 0, N_("não usar") }, { 5, N_("a cada 5 min") }, { 15, N_("a cada 15 min") }, { 30, N_("a cada 30 min") }, { 60, N_("a cada 1 h") },
};
static const opcao_t OPC_POS_S[] = {
  { 0, N_("padrão (15 min)") }, { 300, N_("5 min") }, { 900, N_("15 min") }, { 1800, N_("30 min") },
  { 3600, "1 h" }, { 21600, "6 h" }, { 43200, "12 h" },
};
static const opcao_t OPC_NODEINFO_S[] = {
  { 0, N_("padrão (3 h)") }, { 3600, "1 h" }, { 10800, "3 h" }, { 21600, "6 h" }, { 43200, "12 h" }, { 86400, "24 h" },
};
static const opcao_t OPC_TELE_S[] = {
  { 0, N_("padrão (30 min)") }, { 900, N_("15 min") }, { 1800, N_("30 min") }, { 3600, "1 h" }, { 7200, "2 h" }, { 21600, "6 h" },
};

// How long the node's OWN screen (the C6L display) stays on.
static const opcao_t OPC_TELA_S[] = {
  { 0, N_("padrão (10 min)") }, { 15, "15 s" }, { 30, "30 s" }, { 60, "1 min" }, { 300, "5 min" }, { 900, "15 min" },
};

static const char* opc_nome(const opcao_t* o, int n, int32_t v, char* buf, size_t tam, const char* fmt)
{
  for (int i = 0; i < n; ++i) { if (o[i].v == v) { return tr(o[i].nome); } }
  snprintf(buf, tam, fmt, (long)v);   // value not in the list (set by the web client)
  return buf;
}

static int32_t opc_proximo(const opcao_t* o, int n, int32_t v)
{
  for (int i = 0; i < n; ++i) { if (o[i].v == v) { return o[(i + 1) % n].v; } }
  return o[0].v;
}

// Precision outside the list (set by the phone app): its distance. Each
// bit fewer doubles the cell; 10 bits ~ 23 km.
static const char* precisao_nome(int32_t p, char* buf, size_t tam)
{
  for (int i = 0; i < N_OPC(OPC_PRECISAO); ++i) { if (OPC_PRECISAO[i].v == p) { return tr(OPC_PRECISAO[i].nome); } }
  if (p < 0)            { return "-"; }
  if (p >= 32 || p < 1) { return p ? tr("exata") : tr("não compartilhar"); }
  const uint32_t m = p >= 10 ? (23300u >> (p - 10)) : (23300u << (10 - p));
  if (m >= 1000) { snprintf(buf, tam, tr("aproximada (~%lu km)"), (unsigned long)((m + 500) / 1000)); }
  else           { snprintf(buf, tam, tr("aproximada (~%lu m)"), (unsigned long)m); }
  return buf;
}

static const char* nome_regiao(int r)
{
  static const char* const N[] = {
    N_("não definida"), "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN",
    "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923", "PH_433",
    "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902",
  };
  return (r >= 0 && r < N_OPC(N)) ? tr(N[r]) : tr("outra");
}

static const char* nome_preset(int p)
{
  static const char* const N[] = {
    "LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast", "ShortSlow",
    "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo",
  };
  return (p >= 0 && p < N_OPC(N)) ? N[p] : tr("outro");
}

// Which service field each row edits (-1 = none).
static int campo_da_linha(int i)
{
  switch (i) {
    case RC_PAPEL:      return MESH_CFG_PAPEL;
    case RC_SALTOS:     return MESH_CFG_SALTOS;
    case RC_TX:         return MESH_CFG_TX_DBM;
    case RC_POS_S:      return MESH_CFG_POS_S;
    case RC_NODEINFO_S: return MESH_CFG_NODEINFO_S;
    case RC_TELE_S:     return MESH_CFG_TELE_S;
    case RC_MQTT:       return MESH_CFG_MQTT;
    case RC_PROXY:      return MESH_CFG_MQTT_PROXY;
    case RC_BT:         return MESH_CFG_BT;
    case RC_POS_INTELIGENTE: return MESH_CFG_POS_INTELIGENTE;
    case RC_GPS_NO:     return MESH_CFG_GPS_MODO;
    case RC_TELA_S:     return MESH_CFG_TELA_S;
    case RC_OK_MQTT:    return MESH_CFG_OK_MQTT;
    default:            return -1;
  }
}

static int32_t valor_atual(int campo)
{
  return s_mud.muda[campo] ? s_mud.v[campo] : s_cfg.v[campo];
}

static bool ha_pendente(void)
{
  for (int k = 0; k < MESH_CFG_N; ++k) { if (s_mud.muda[k]) { return true; } }
  return s_mud.longo[0] || s_mud.curto[0] || s_mud.wifi_muda || s_mud.wifi_ssid[0] || s_mud.wifi_psk[0]
      || s_mud.precisao_muda || s_mud.uplink_muda;
}

// Label and value of a row. 'pend' = pending value (not yet written).
static void linha_cfg(int i, char* rot, size_t tamr, char* val, size_t tv, bool* pend)
{
  char tmp[40];
  *pend = false;
  if (tv) { val[0] = 0; }
  const uint32_t meu = tab5_mesh_meu_numero();
  const int c = campo_da_linha(i);
  if (c >= 0) {
    *pend = s_mud.muda[c];
    if (!s_cfg.conhecido[c] && !*pend) { snprintf(val, tv, "-"); }
  }
  const int32_t v = (c >= 0) ? valor_atual(c) : 0;
  const bool conhecido = (c < 0) || s_cfg.conhecido[c] || *pend;

  switch (i) {
    case RC_SOM:
      snprintf(rot, tamr, tr("som das notificações"));
      snprintf(val, tv, "%s", tab5_mesh_beep_ligado() ? tr("ligado") : tr("mudo"));
      break;
    case RC_LONGO: case RC_CURTO: {
      const bool longo = (i == RC_LONGO);
      snprintf(rot, tamr, longo ? tr("nome longo") : tr("nome curto"));
      const char* pendente = longo ? s_mud.longo : s_mud.curto;
      if (pendente[0]) { *pend = true; snprintf(val, tv, "%s", pendente); break; }
      char n[MESH_NOME_LONGO] = "";
      if (longo) { tab5_mesh_nome_longo_cache(meu, n, sizeof(n)); } else { tab5_mesh_nome_cache(meu, n, sizeof(n)); }
      snprintf(val, tv, "%s", n[0] ? n : "-");
      break;
    }
    case RC_PAPEL:
      snprintf(rot, tamr, tr("papel na malha"));
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_PAPEL, N_OPC(OPC_PAPEL), v, tmp, sizeof(tmp), tr("papel %ld"))); }
      break;
    case RC_SALTOS:
      snprintf(rot, tamr, tr("saltos (hop limit)"));
      if (conhecido) { if (v == 0) { snprintf(val, tv, tr("3 (padrão)")); } else { snprintf(val, tv, tr("%ld"), (long)v); } }
      break;
    case RC_TX:
      snprintf(rot, tamr, tr("potência de transmissão"));
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_TX, N_OPC(OPC_TX), v, tmp, sizeof(tmp), tr("%ld dBm"))); }
      break;
    case RC_LOC_PRECISAO: {
      // TURNS location ON/OFF: it is the primary channel's precision. With 0 the
      // node sends no position at all - the rows below have no effect.
      snprintf(rot, tamr, tr("compartilhar a localização"));
      *pend = s_mud.precisao_muda;
      snprintf(val, tv, "%s", precisao_nome(*pend ? s_mud.precisao : s_cfg.precisao, tmp, sizeof(tmp)));
      break;
    }
    case RC_LOC_TAB5: {
      // ON THE TAB5 (ajustes.h), changes at once. It only goes to the node; it is the node that
      // broadcasts, at the "send every" rate (see enviar_posicao).
      snprintf(rot, tamr, tr("GPS do Tab5 para o nó"));
      const uint8_t m = ajustes_mesh_posicao_min();
      const char* n = opc_nome(OPC_TAB5, N_OPC(OPC_TAB5), m, tmp, sizeof(tmp), tr("a cada %ld min"));
      const bool fixa = s_cfg.pos_fixa;
      const bool gps_no = s_cfg.conhecido[MESH_CFG_GPS_MODO] && s_cfg.v[MESH_CFG_GPS_MODO] == 1;
      if (m && (fixa || gps_no)) { snprintf(val, tv, tr("parado: o nó usa %s"), fixa ? tr("a posição fixa") : tr("o GPS dele")); }
      else                       { snprintf(val, tv, "%s", n); }
      break;
    }
    case RC_POS_S:
      snprintf(rot, tamr, tr("enviar a posição a cada"));
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_POS_S, N_OPC(OPC_POS_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_POS_INTELIGENTE:
      snprintf(rot, tamr, tr("enviar também ao se mover"));
      if (conhecido) { snprintf(val, tv, "%s", v ? tr("ligado") : tr("desligado")); }
      break;
    case RC_GPS_NO:
      snprintf(rot, tamr, tr("GPS do próprio nó"));
      if (conhecido) {
        if (v == 2)      { snprintf(val, tv, tr("a placa não tem GPS")); }
        else             { snprintf(val, tv, "%s", v == 1 ? tr("ligado") : tr("desligado")); }
      }
      break;
    case RC_POS_FIXA: {
      snprintf(rot, tamr, tr("posição fixa do nó"));
      const nmea_posicao_t g = tab5_gps_posicao();
      const bool fix = g.valida && g.qualidade > 0;
      if (s_cfg.pos_fixa) { snprintf(val, tv, tr("fixa - tocar para soltar")); }
      else if (fix)       { snprintf(val, tv, tr("tocar para fixar no GPS do Tab5")); }
      else                { snprintf(val, tv, tr("GPS do Tab5 sem sinal")); }
      break;
    }
    case RC_NODEINFO_S:
      snprintf(rot, tamr, tr("apresentar o nó a cada"));
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_NODEINFO_S, N_OPC(OPC_NODEINFO_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_TELA_S:
      snprintf(rot, tamr, tr("tela do nó apaga em"));
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_TELA_S, N_OPC(OPC_TELA_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_UPLINK: {
      // What the node RECEIVES over the radio only goes up to MQTT with the channel's uplink
      // on (Channel.settings.uplink_enabled). And, on a public server, only
      // the message of whoever authorised it (the SENDER's "OK to MQTT", the row
      // below on their node): without it the gateway discards. What it sends
      // itself goes up regardless.
      snprintf(rot, tamr, tr("mandar ao MQTT o que recebe"));
      *pend = s_mud.uplink_muda;
      const int u = *pend ? (s_mud.uplink ? 1 : 0) : s_cfg.uplink;
      snprintf(val, tv, "%s", u < 0 ? "-" : u ? tr("ligado") : tr("desligado"));
      break;
    }
    case RC_OK_MQTT:
      // The AUTHORISATION for THIS node's messages (see MESH_CFG_OK_MQTT): without it,
      // public server gateways do not carry what it sends to MQTT.
      snprintf(rot, tamr, tr("minhas mensagens no MQTT"));
      if (conhecido) { snprintf(val, tv, "%s", v ? tr("autorizadas") : tr("não autorizadas")); }
      break;
    case RC_TELE_S:
      snprintf(rot, tamr, tr("telemetria a cada"));
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_TELE_S, N_OPC(OPC_TELE_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_MQTT:
      snprintf(rot, tamr, tr("MQTT do nó"));
      if (conhecido) { snprintf(val, tv, "%s", v ? tr("ligado") : tr("desligado")); }
      break;
    case RC_PROXY:
      // INTERNET FROM THE TAB5 TO THE NODE (MQTT Client Proxy). The field belongs to the node
      // (proxy_to_client_enabled); with it on and confirmed, the Tab5's
      // service carries what the node publishes to the server. Alongside, how the bridge is doing.
      snprintf(rot, tamr, tr("internet do Tab5 (proxy)"));
      if (conhecido) {
        if (v && !*pend) { snprintf(val, tv, tr("ligado  ·  %s"), tr(tab5_mesh_proxy_estado())); }
        else             { snprintf(val, tv, "%s", v ? tr("ligado") : tr("desligado")); }
      }
      break;
    case RC_BT:
      snprintf(rot, tamr, tr("Bluetooth do nó"));
      if (conhecido) {
        snprintf(val, tv, "%s%s", v ? tr("ligado") : tr("desligado"),
                 tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH ? tr(" (em uso pelo Tab5)") : "");
      }
      break;
    case RC_WIFI: {
      snprintf(rot, tamr, tr("Wi-Fi do nó"));
      if (s_mud.wifi_ssid[0]) { *pend = true; snprintf(val, tv, tr("ligar em %s"), s_mud.wifi_ssid); }
      else if (s_mud.wifi_muda) { *pend = true; snprintf(val, tv, "%s", s_mud.wifi_liga ? tr("ligar") : tr("desligar")); }
      else if (!s_cfg.wifi_conhecido) { snprintf(val, tv, "-"); }
      else if (!s_cfg.wifi_ligado) { snprintf(val, tv, tr("desligado")); }
      else { snprintf(val, tv, tr("ligado%s%s"), s_cfg.wifi_ssid[0] ? "  ·  " : "", s_cfg.wifi_ssid); }
      break;
    }
    case RC_HORA:
      snprintf(rot, tamr, tr("relógio do nó"));
      snprintf(val, tv, tr("tocar para acertar com o Tab5"));
      break;
    case RC_REINICIAR:
      snprintf(rot, tamr, tr("reiniciar o nó"));
      snprintf(val, tv, (s_reiniciar_ms && (tab5_agora_ms() - s_reiniciar_ms) < 4000) ? tr("toque de novo para confirmar") : tr("tocar para reiniciar"));
      break;
    case RC_I_FIRMWARE: {
      snprintf(rot, tamr, tr("firmware / placa"));
      const char* hw = mesh_hw_nome(s_cfg.hw);
      snprintf(val, tv, "%s / %s", s_cfg.firmware[0] ? s_cfg.firmware : "-", hw ? hw : "-");
      break;
    }
    case RC_I_REGIAO:
      snprintf(rot, tamr, tr("região / modem"));
      if (s_cfg.regiao >= 0) {
        snprintf(val, tv, "%s / %s", nome_regiao(s_cfg.regiao), s_cfg.usa_preset ? nome_preset(s_cfg.preset) : tr("personalizado"));
      } else { snprintf(val, tv, "-"); }
      break;
    case RC_I_NOS:
      snprintf(rot, tamr, tr("nós guardados"));
      if (s_cfg.nos_no_radio >= 0) { snprintf(val, tv, tr("rádio %ld  ·  Tab5 %d"), (long)s_cfg.nos_no_radio, tab5_mesh_num_nos()); }
      else                         { snprintf(val, tv, tr("Tab5 %d  (o firmware do rádio não informa)"), tab5_mesh_num_nos()); }
      break;
    case RC_I_REINICIOS:
      snprintf(rot, tamr, tr("reinícios do nó"));
      snprintf(val, tv, tr("%lu  ·  tocar para ver o diário"), (unsigned long)s_cfg.reinicios);
      break;
    case RC_I_AVISO:
      snprintf(rot, tamr, tr("último aviso do nó"));
      snprintf(val, tv, "%s", s_cfg.aviso[0] ? s_cfg.aviso : "-");
      break;
  }
}

// ---- configuration row icons ------------------------------------------------
// Each row carries a drawing tied to what it does, in place of the single
// gear. They fit in ~28 px around (cx, cy); all with primitives, no
// stored image. 'cor' is the ink; the hole/background is always the theme background.
static void icone_cfg(lgfx::LovyanGFX& d, int i, int cx, int cy, uint16_t cor)
{
  const uint16_t F = TEMA_FUNDO;
  switch (i) {
    case RC_SOM:        // speaker with waves
      d.fillRect(cx - 11, cy - 4, 6, 8, cor);
      d.fillTriangle(cx - 5, cy - 4, cx + 2, cy - 11, cx + 2, cy + 11, cor);
      d.fillTriangle(cx - 5, cy + 4, cx + 2, cy - 11, cx + 2, cy + 11, cor);
      d.fillArc(cx + 3, cy, 7, 5, -45, 45, cor);
      d.fillArc(cx + 3, cy, 12, 10, -45, 45, cor);
      break;
    case RC_LONGO: case RC_CURTO: {   // name badge: long = two lines, short = one
      d.drawRoundRect(cx - 13, cy - 9, 26, 18, 3, cor);
      d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, cor);
      d.fillCircle(cx - 7, cy - 2, 3, cor);
      d.fillRect(cx - 10, cy + 2, 7, 3, cor);
      d.fillRect(cx - 1, cy - 4, 10, 2, cor);
      if (i == RC_LONGO) { d.fillRect(cx - 1, cy + 1, 10, 2, cor); }
      break;
    }
    case RC_PAPEL:      // person
      d.fillCircle(cx, cy - 5, 5, cor);
      d.fillArc(cx, cy + 12, 12, 0, 180, 360, cor);
      break;
    case RC_SALTOS:     // three nodes in a chain
      d.drawLine(cx - 10, cy + 6, cx, cy - 6, cor);
      d.drawLine(cx, cy - 6, cx + 10, cy + 6, cor);
      d.fillCircle(cx - 10, cy + 6, 4, cor);
      d.fillCircle(cx, cy - 6, 4, cor);
      d.fillCircle(cx + 10, cy + 6, 4, cor);
      break;
    case RC_TX:         // antenna with waves
      d.fillTriangle(cx - 5, cy + 12, cx + 5, cy + 12, cx, cy - 2, cor);
      d.fillCircle(cx, cy - 5, 3, cor);
      d.fillArc(cx, cy - 5, 9, 7, -40, 40, cor);
      d.fillArc(cx, cy - 5, 9, 7, 140, 220, cor);
      d.fillArc(cx, cy - 5, 14, 12, -40, 40, cor);
      d.fillArc(cx, cy - 5, 14, 12, 140, 220, cor);
      break;
    case RC_POS_S: case RC_POS_FIXA: case RC_LOC_PRECISAO: case RC_POS_INTELIGENTE:
      // map pin; the fixed one gets the base, the "on movement" one a trail
      d.fillCircle(cx, cy - 4, 8, cor);
      d.fillTriangle(cx - 7, cy, cx + 7, cy, cx, cy + 10, cor);
      d.fillCircle(cx, cy - 4, 3, F);
      if (i == RC_POS_FIXA) { d.fillRect(cx - 9, cy + 11, 18, 3, cor); }
      if (i == RC_POS_INTELIGENTE) { d.fillRect(cx - 14, cy + 11, 4, 3, cor); d.fillRect(cx - 7, cy + 11, 4, 3, cor); }
      if (i == RC_LOC_PRECISAO) { d.drawCircle(cx, cy - 4, 12, cor); }
      break;
    case RC_LOC_TAB5:   // the tablet with the pin on screen
      d.drawRoundRect(cx - 12, cy - 11, 24, 22, 3, cor);
      d.drawRoundRect(cx - 11, cy - 10, 22, 20, 3, cor);
      d.fillCircle(cx, cy - 2, 4, cor);
      d.fillTriangle(cx - 4, cy, cx + 4, cy, cx, cy + 6, cor);
      break;
    case RC_GPS_NO:     // GPS crosshair
      d.drawCircle(cx, cy, 9, cor);
      d.drawCircle(cx, cy, 8, cor);
      d.fillCircle(cx, cy, 3, cor);
      d.fillRect(cx - 1, cy - 14, 3, 6, cor);
      d.fillRect(cx - 1, cy + 9, 3, 6, cor);
      d.fillRect(cx - 14, cy - 1, 6, 3, cor);
      d.fillRect(cx + 9, cy - 1, 6, 3, cor);
      break;
    case RC_NODEINFO_S: // ID card
      d.drawRoundRect(cx - 13, cy - 9, 26, 18, 3, cor);
      d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, cor);
      d.fillCircle(cx - 6, cy - 2, 3, cor);
      d.fillArc(cx - 6, cy + 8, 6, 0, 180, 360, cor);
      d.fillRect(cx + 1, cy - 4, 9, 2, cor);
      d.fillRect(cx + 1, cy + 1, 9, 2, cor);
      break;
    case RC_TELE_S:     // chart bars
      d.fillRect(cx - 11, cy + 2, 6, 9, cor);
      d.fillRect(cx - 3, cy - 4, 6, 15, cor);
      d.fillRect(cx + 5, cy - 11, 6, 22, cor);
      break;
    case RC_TELA_S:     // a small display (the node's screen)
      d.fillRoundRect(cx - 12, cy - 9, 24, 18, 3, cor);
      d.fillRect(cx - 8, cy - 5, 16, 10, F);
      d.fillRect(cx - 5, cy - 1, 10, 2, cor);
      break;
    case RC_UPLINK:     // up arrow (goes up to MQTT)
      d.fillTriangle(cx, cy - 12, cx - 10, cy - 1, cx + 10, cy - 1, cor);
      d.fillRect(cx - 4, cy - 2, 8, 13, cor);
      break;
    case RC_OK_MQTT:    // a tick (authorised)
      for (int k = -2; k <= 2; ++k) {
        d.drawLine(cx - 11, cy + k, cx - 4, cy + 7 + k, cor);
        d.drawLine(cx - 4, cy + 7 + k, cx + 11, cy - 8 + k, cor);
      }
      break;
    case RC_MQTT:       // cloud
      d.fillCircle(cx - 7, cy + 2, 6, cor);
      d.fillCircle(cx + 1, cy - 3, 8, cor);
      d.fillCircle(cx + 8, cy + 3, 5, cor);
      d.fillRect(cx - 7, cy + 2, 16, 7, cor);
      break;
    case RC_PROXY:      // small cloud with an arrow both ways: the bridge
      d.fillCircle(cx - 6, cy - 4, 5, cor);
      d.fillCircle(cx + 1, cy - 8, 6, cor);
      d.fillCircle(cx + 7, cy - 3, 4, cor);
      d.fillRect(cx - 6, cy - 4, 13, 5, cor);
      d.fillRect(cx - 1, cy + 3, 3, 9, cor);
      d.fillTriangle(cx - 5, cy + 6, cx + 5, cy + 6, cx, cy + 1, cor);
      d.fillTriangle(cx - 5, cy + 9, cx + 5, cy + 9, cx, cy + 14, cor);
      break;
    case RC_BT: {       // Bluetooth rune
      const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
      for (int k = -1; k <= 1; ++k) {
        d.drawLine(cx + k, t, cx + k, b, cor);
        d.drawLine(cx + k, t, cx + dx + k, t + q, cor);
        d.drawLine(cx + dx + k, t + q, cx - dx + k, cy + q, cor);
        d.drawLine(cx + k, b, cx + dx + k, b - q, cor);
        d.drawLine(cx + dx + k, b - q, cx - dx + k, cy - q, cor);
      }
      break;
    }
    case RC_WIFI:       // Wi-Fi arcs
      for (int k = 0; k < 3; ++k) { const int r = 7 + 6 * k; d.fillArc(cx, cy + 9, r, r - 3, 225, 315, cor); }
      d.fillCircle(cx, cy + 8, 2, cor);
      break;
    case RC_HORA:       // clock
      d.drawCircle(cx, cy, 12, cor);
      d.drawCircle(cx, cy, 11, cor);
      d.drawLine(cx, cy, cx, cy - 8, cor);
      d.drawLine(cx, cy, cx + 6, cy + 3, cor);
      d.fillCircle(cx, cy, 2, cor);
      break;
    case RC_REINICIAR: case RC_I_REINICIOS:   // circular arrow
      d.fillArc(cx, cy, 11, 8, 40, 330, cor);
      d.fillTriangle(cx + 14, cy - 7, cx + 3, cy - 9, cx + 10, cy + 2, cor);
      break;
    case RC_I_FIRMWARE: // chip
      d.fillRoundRect(cx - 8, cy - 8, 16, 16, 3, cor);
      d.fillRect(cx - 4, cy - 4, 8, 8, F);
      for (int k = -1; k <= 1; ++k) {
        d.fillRect(cx - 12, cy + k * 5 - 1, 4, 2, cor);
        d.fillRect(cx + 8,  cy + k * 5 - 1, 4, 2, cor);
        d.fillRect(cx + k * 5 - 1, cy - 12, 2, 4, cor);
        d.fillRect(cx + k * 5 - 1, cy + 8,  2, 4, cor);
      }
      break;
    case RC_I_REGIAO:   // globe
      d.drawCircle(cx, cy, 12, cor);
      d.drawCircle(cx, cy, 11, cor);
      d.drawEllipse(cx, cy, 5, 12, cor);
      d.drawFastHLine(cx - 12, cy, 25, cor);
      d.drawFastHLine(cx - 10, cy - 6, 21, cor);
      d.drawFastHLine(cx - 10, cy + 6, 21, cor);
      break;
    case RC_I_NOS:      // network of nodes
      d.drawLine(cx, cy, cx - 10, cy - 8, cor);
      d.drawLine(cx, cy, cx + 10, cy - 8, cor);
      d.drawLine(cx, cy, cx, cy + 11, cor);
      d.fillCircle(cx, cy, 4, cor);
      d.fillCircle(cx - 10, cy - 8, 3, cor);
      d.fillCircle(cx + 10, cy - 8, 3, cor);
      d.fillCircle(cx, cy + 11, 3, cor);
      break;
    case RC_I_AVISO:    // warning triangle
      d.fillTriangle(cx, cy - 12, cx - 13, cy + 10, cx + 13, cy + 10, cor);
      d.fillRect(cx - 1, cy - 4, 3, 8, F);
      d.fillRect(cx - 1, cy + 6, 3, 2, F);
      break;
    default:
      d.fillCircle(cx, cy, 4, cor);
      break;
  }
}

static const uint16_t* tinta_cfg(lgfx::LovyanGFX& d, int i)
{
  static uint16_t t[RC_TOTAL];
  static bool pronto = false;
  if (!pronto) {
    pronto = true;
    for (int k = 0; k < RC_TOTAL; ++k) { t[k] = d.color565(110, 120, 135); }   // read only: bluish grey
    t[RC_SOM] = d.color565(255, 214, 90);
    t[RC_LONGO] = t[RC_CURTO] = d.color565(120, 200, 255);
    t[RC_PAPEL] = d.color565(255, 160, 90);
    t[RC_SALTOS] = d.color565(190, 150, 255);
    t[RC_TX] = d.color565(255, 110, 110);
    t[RC_POS_S] = t[RC_POS_FIXA] = t[RC_LOC_PRECISAO] = t[RC_LOC_TAB5] = d.color565(120, 220, 130);
    t[RC_POS_INTELIGENTE] = t[RC_GPS_NO] = d.color565(120, 220, 130);
    t[RC_NODEINFO_S] = d.color565(120, 200, 255);
    t[RC_TELE_S] = d.color565(90, 220, 210);
    t[RC_MQTT] = d.color565(150, 190, 255);
    t[RC_UPLINK] = t[RC_OK_MQTT] = d.color565(150, 190, 255);
    t[RC_TELA_S] = d.color565(255, 214, 90);
    t[RC_PROXY] = d.color565(120, 230, 200);
    t[RC_BT] = d.color565(80, 140, 255);
    t[RC_WIFI] = d.color565(60, 170, 255);
    t[RC_HORA] = d.color565(200, 200, 210);
    t[RC_REINICIAR] = d.color565(255, 140, 80);
  }
  return &t[i];
}

int cfg_altura(int, void*) { return CFG_LIN; }

// SUMMARY OF A GROUP, in the top list: the essentials of what is inside, so
// you do not need to open it just to check. 'pend' = something pending inside.
static void linha_grupo(int g, char* rot, size_t tamr, char* val, size_t tv, bool* pend)
{
  char r2[48], v2[96], tmp[40];
  bool p2 = false;
  *pend = false;
  for (int k = 0; k < GRUPOS[g].n; ++k) {
    linha_cfg(GRUPOS[g].rc[k], r2, sizeof(r2), v2, sizeof(v2), &p2);
    *pend |= p2;
  }
  snprintf(rot, tamr, "%s", tr(GRUPOS[g].nome));
  const bool pronto = s_cfg.conhecido[MESH_CFG_PAPEL];
  switch (g) {
    case G_INTERNET:
      snprintf(val, tv, "%s  ·  %s", mesh_mqtt_servidor_nome(ajustes_mqtt_servidor()),
               ajustes_mesh_internet() ? tr("quando faltar o nó") : tr("desligada"));
      break;
    case G_PC: {
      char url[64];
      if (tab5_web_aberto(APP_MESH_NOME) && tab5_web_endereco(APP_MESH_NOME, url, sizeof(url))) { snprintf(val, tv, tr("ligado: %s"), url); }
      else { snprintf(val, tv, "%s", tr("conversar pelo PC")); }
      break;
    }
    case G_NOME: {
      char n[MESH_NOME_LONGO] = "";
      tab5_mesh_nome_longo_cache(tab5_mesh_meu_numero(), n, sizeof(n));
      if (s_mud.longo[0]) { snprintf(n, sizeof(n), "%s", s_mud.longo); }
      linha_cfg(RC_PAPEL, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "%s  ·  %s", n[0] ? n : "-", pronto ? v2 : "-");
      break;
    }
    case G_LOCAL: {
      const int32_t p = s_mud.precisao_muda ? s_mud.precisao : s_cfg.precisao;
      if (p == 0) { snprintf(val, tv, tr("não compartilhada")); break; }
      if (p < 0)  { snprintf(val, tv, "-"); break; }
      linha_cfg(RC_POS_S, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "%s  ·  %s%s", precisao_nome(p, tmp, sizeof(tmp)), v2,
               valor_atual(MESH_CFG_POS_INTELIGENTE) ? tr(" e ao se mover") : "");
      break;
    }
    case G_RADIO:
      linha_cfg(RC_SALTOS, r2, sizeof(r2), v2, sizeof(v2), &p2);
      if (s_cfg.regiao >= 0) {
        snprintf(val, tv, tr("%s / %s  ·  %s saltos"), nome_regiao(s_cfg.regiao),
                 s_cfg.usa_preset ? nome_preset(s_cfg.preset) : tr("personalizado"), v2);
      } else { snprintf(val, tv, "-"); }
      break;
    case G_CONEXOES: {
      if (!pronto) { snprintf(val, tv, "-"); break; }
      char w[48];
      if (s_cfg.wifi_conhecido && s_cfg.wifi_ligado) { snprintf(w, sizeof(w), tr("Wi-Fi %s"), s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : tr("ligado")); }
      else                                           { snprintf(w, sizeof(w), tr("sem Wi-Fi")); }
      snprintf(val, tv, tr("Bluetooth %s  ·  %s  ·  MQTT %s"), valor_atual(MESH_CFG_BT) ? tr("sim") : tr("não"), w,
               !valor_atual(MESH_CFG_MQTT) ? tr("não") : valor_atual(MESH_CFG_MQTT_PROXY) ? tr("pelo Tab5") : tr("sim"));
      break;
    }
    case G_AVISOS:
      linha_cfg(RC_TELE_S, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, tr("som %s  ·  telemetria %s"), tab5_mesh_beep_ligado() ? tr("ligado") : tr("mudo"), pronto ? v2 : "-");
      break;
    case G_MANUT:
      snprintf(val, tv, tr("%s  ·  %lu reinícios"), s_cfg.firmware[0] ? s_cfg.firmware : "-", (unsigned long)s_cfg.reinicios);
      break;
  }
}

// The text of item 'i' of the current list (group or row).
static void linha_item(int i, char* rot, size_t tamr, char* val, size_t tv, bool* pend)
{
  if (s_grupo < 0) { linha_grupo(i, rot, tamr, val, tv, pend); }
  else             { linha_cfg(GRUPOS[s_grupo].rc[i], rot, tamr, val, tv, pend); }
}

// A group: icon, name, summary and the "opens" arrow.
static void grupo_desenhar(lgfx::LovyanGFX& d, int g, int x, int y, int largura)
{
  char rot[48], val[96] = "";
  bool pend = false;
  linha_grupo(g, rot, sizeof(rot), val, sizeof(val), &pend);
  snprintf(s_cfg_ant[g], sizeof(s_cfg_ant[g]), "%d|%s", (int)pend, val);
  s_cfg_pintada[g] = true;

  d.fillRect(x, y, largura, CFG_LIN, TEMA_FUNDO);
  icone_cfg(d, GRUPOS[g].icone, x + 26, y + CFG_LIN / 2, *tinta_cfg(d, GRUPOS[g].icone));
  d.setTextDatum(middle_left);
  tab5_texto(d, 3.6f);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(rot, x + 52, y + CFG_LIN / 2);
  const int rot_fim = x + 52 + (int)d.textWidth(rot) + 24;

  // The arrow: this item opens another list.
  const int sx = x + largura - 30, sy = y + CFG_LIN / 2;
  d.drawLine(sx, sy - 9, sx + 9, sy, TEMA_TEXTO_3); d.drawLine(sx + 9, sy, sx, sy + 9, TEMA_TEXTO_3);
  d.drawLine(sx + 1, sy - 9, sx + 10, sy, TEMA_TEXTO_3); d.drawLine(sx + 10, sy, sx + 1, sy + 9, TEMA_TEXTO_3);

  tab5_texto(d, 2.8f);
  d.setTextColor(pend ? TEMA_ALERTA : TEMA_TEXTO_2, TEMA_FUNDO);
  char v2[100];
  snprintf(v2, sizeof(v2), pend ? "• %s" : "%s", val);
  const int vx = em_pe(sistema_area()) ? rot_fim : (rot_fim > x + 440 ? rot_fim : x + 440);
  cortar_ao_caber(d, v2, sx - 16 - vx);
  d.drawString(v2, vx, y + CFG_LIN / 2);
  d.drawFastHLine(x + 16, y + CFG_LIN - 1, largura - 32, TEMA_LINHA);
  d.setTextDatum(top_left);
}

void cfg_desenhar(lgfx::LovyanGFX& d, int item, int x, int y, int largura, void*)
{
  if (s_grupo < 0) { grupo_desenhar(d, item, x, y, largura); return; }
  const int i = GRUPOS[s_grupo].rc[item];
  char rot[48], val[96] = "";
  bool pend = false;
  linha_cfg(i, rot, sizeof(rot), val, sizeof(val), &pend);
  snprintf(s_cfg_ant[item], sizeof(s_cfg_ant[item]), "%d|%s", (int)pend, val);
  s_cfg_pintada[item] = true;

  // THE WHOLE ROW, always: a redraw covers everything that was in it.
  d.fillRect(x, y, largura, CFG_LIN, TEMA_FUNDO);

  const bool editavel = (i < RC_EDITAVEIS);
  // Radio row with no node on the other end: the icon is dimmed - viewing,
  // yes; changing, no (only the Tab5's own rows).
  const bool ativa = editavel && (rc_do_tab5(i) || s_cfg.estado == MESH_CFG_PRONTO);
  // Row icon: coloured when it can be changed, dimmed without the node on the other end.
  icone_cfg(d, i, x + 26, y + CFG_LIN / 2,
            editavel ? (ativa ? *tinta_cfg(d, i) : TEMA_TEXTO_3) : TEMA_TEXTO_3);

  d.setTextDatum(middle_left);
  tab5_texto(d, 3.2f);
  d.setTextColor(editavel ? TEMA_TEXTO_2 : TEMA_TEXTO_3, TEMA_FUNDO);
  d.drawString(rot, x + 52, y + CFG_LIN / 2);
  const int rot_fim = x + 52 + (int)d.textWidth(rot) + 24;

  tab5_texto(d, 3.6f);
  d.setTextColor(pend ? TEMA_ALERTA : (ativa || !editavel ? TEMA_TEXTO : TEMA_TEXTO_3), TEMA_FUNDO);
  char v2[100];
  snprintf(v2, sizeof(v2), pend ? "• %s" : "%s", val);
  if (em_pe(sistema_area())) {
    // IN PORTRAIT there is no width for the fixed column at 440: the value sits against the
    // right and takes whatever the label leaves.
    cortar_ao_caber(d, v2, x + largura - 16 - rot_fim);
    d.setTextDatum(middle_right);
    d.drawString(v2, x + largura - 16, y + CFG_LIN / 2);
    d.setTextDatum(middle_left);
  } else {
    // Cut per character, so as not to invade the edge.
    cortar_ao_caber(d, v2, largura - 470);
    d.drawString(v2, x + 440, y + CFG_LIN / 2);
  }
  d.drawFastHLine(x + 16, y + CFG_LIN - 1, largura - 32, TEMA_LINHA);
  d.setTextDatum(top_left);
}

// The footer only exists with something pending: without it, the list goes to the end of the
// area. When it appears or disappears, the window changes size and the list
// redraws itself in full (lista_desenhar compares with the previous window).
static area_t janela_cfg(const area_t& a, int x0)
{
  const int rodape = ha_pendente() ? CFG_RODAPE : 0;
  return area_t{ x0, a.y + CFG_TOPO, a.x + a.largura - x0, a.altura - CFG_TOPO - rodape };
}

// ---- restart journal -------------------------------------------------------
static const int REI_LIN = 126, REI_TOPO = 80, REI_RODAPE = 84;

static void desenhar_diario(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  lista_esconder(&s_l_cfg);
  const bool conf = s_limpar_ms && (tab5_agora_ms() - s_limpar_ms) < 4000;
  const uint32_t assin = mesh_diario_versao() * 2u + (conf ? 1u : 0u);
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;
  s_rei_n = mesh_diario_ler(s_rei, MESH_DIARIO_MAX);

  tab5_tela_limpar(x0, a.y, larg, a.altura, TEMA_FUNDO);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  tab5_texto(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(tr("Reinícios do nó"), x0 + 250, a.y + 40);

  const int cabe = (a.altura - REI_TOPO - REI_RODAPE) / REI_LIN;
  const int mostra = s_rei_n < cabe ? s_rei_n : cabe;
  const int max_l = larg - 64;
  if (s_rei_n == 0) {
    tab5_texto(d, 2.8f);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr("nenhum reinício anotado ainda"), x0 + 32, a.y + REI_TOPO + 40);
  }
  for (int i = 0; i < mostra; ++i) {
    const mesh_reinicio_t& r = s_rei[i];
    const int y = a.y + REI_TOPO + i * REI_LIN;
    char hora[40], dur[24], l1[120], l2[120], l3[140];
    if (r.epoch) {
      const time_t t = (time_t)r.epoch;
      struct tm tmv; localtime_r(&t, &tmv);
      strftime(hora, sizeof(hora), "%d/%m %H:%M", &tmv);
    } else {
      tab5_duracao(r.up_tab5_s, dur, sizeof(dur));
      snprintf(hora, sizeof(hora), tr("Tab5 ligado há %s"), dur);
    }
    static const char* const ORI[] = { N_("primeira leitura"), N_("sozinho"), N_("pedido pelo Tab5"), N_("com o Tab5 desconectado") };
    snprintf(l1, sizeof(l1), "%s  ·  contador %lu  ·  %s", hora, (unsigned long)r.contador, tr(ORI[r.origem < 4 ? r.origem : 1]));
    l2[0] = 0;
    if (r.origem != MESH_REI_BASE) {
      if (r.up_no_s) { tab5_duracao(r.up_no_s, dur, sizeof(dur)); snprintf(l2, sizeof(l2), tr("estava ligado ~%s"), dur); }
      if (r.centivolts) {
        const size_t k = strlen(l2);
        snprintf(l2 + k, sizeof(l2) - k, tr("%sbateria %.2f V %ld mA"), k ? "  ·  " : "", r.centivolts / 100.0, (long)r.ma);
      }
    }
    snprintf(l3, sizeof(l3), "%s", tr(mesh_diario_veredito(&r)));
    const uint16_t c1 = r.origem == MESH_REI_SOZINHO ? TEMA_ALERTA : r.origem == MESH_REI_BASE ? TEMA_TEXTO_3
                      : r.origem == MESH_REI_AUSENTE ? TEMA_ALERTA : TEMA_TEXTO_2;
    const bool sem_indicio = strncmp(mesh_diario_veredito(&r), "sem indício", 11) == 0;   // the original text, not the translated one
    const uint16_t c3 = (r.origem == MESH_REI_SOZINHO && !sem_indicio) ? TEMA_PERIGO : TEMA_INFO;
    tab5_texto(d, 2.8f);
    d.setTextDatum(top_left);
    cortar_ao_caber(d, l1, max_l); cortar_ao_caber(d, l2, max_l); cortar_ao_caber(d, l3, max_l);
    d.setTextColor(c1, TEMA_FUNDO);        d.drawString(l1, x0 + 32, y + 6);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO); d.drawString(l2, x0 + 32, y + 42);
    d.setTextColor(c3, TEMA_FUNDO);        d.drawString(l3, x0 + 32, y + 78);
    d.drawFastHLine(x0 + 16, y + REI_LIN - 4, larg - 32, TEMA_LINHA);
  }
  const int by = a.y + a.altura - REI_RODAPE + 14;
  tab5_botao(d, x0 + 24, by, 320, 56, conf ? tr("TOQUE DE NOVO") : tr("LIMPAR DIÁRIO"),
             conf ? TEMA_PERIGO : TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  tab5_texto(d, 2.4f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
  char rod[80];
  if (s_rei_n > mostra) { snprintf(rod, sizeof(rod), tr("+%d mais antigos (no cartão: Meshtastic/reinicios.txt)"), s_rei_n - mostra); }
  else                  { snprintf(rod, sizeof(rod), tr("o motivo exato só aparece no log USB do nó; aqui é dedução")); }
  cortar_ao_caber(d, rod, larg - (24 + 320 + 24) - 16);
  d.drawString(rod, x0 + 24 + 320 + 24, by + 28);
  d.setTextDatum(top_left);
}

// ---- the node's Wi-Fi: all on one screen -----------------------------------
// On/off switch at the top; with the node's Wi-Fi on, the list of networks
// already used (tap chooses, the x forgets) and the NOVA REDE / TRAZER DO TAB5 (new network /
// bring from Tab5) actions. Flipping the switch or choosing a network is PENDING: GRAVAR NO NO
// sends it and the node restarts, like any other change.
static const int RD_LIN = 78, RD_BTN_A = 60;
static const int WF_TOG_Y = 84, WF_TOG_H = 100, WF_LISTA_Y = 84 + 100 + 56;

static void interruptor(lgfx::LovyanGFX& d, int x, int y, int w, int h, bool ligado, bool ativo)
{
  const uint16_t trilho = !ativo ? TEMA_LINHA : ligado ? TEMA_SUCESSO : TEMA_TEXTO_3;
  d.fillRoundRect(x, y, w, h, h / 2, trilho);
  const int r = h / 2 - 6;
  d.fillCircle(ligado ? x + w - h / 2 : x + h / 2, y + h / 2, r, ativo ? TEMA_TEXTO : TEMA_TEXTO_2);
}

// The screen's "changed?" signature: the system's single FNV-1a (tab5_hash.hpp).
static uint32_t fnv_str(const char* t) { return tab5_fnv1a_txt(t); }

static bool wf_liga_visivel(void) { return s_mud.wifi_muda ? s_mud.wifi_liga : s_cfg.wifi_ligado; }
// The list only exists with the node's Wi-Fi actually ON (after the restart).
static bool wf_lista_visivel(void) { return s_cfg.wifi_conhecido && s_cfg.wifi_ligado && wf_liga_visivel(); }
// 0 = no buttons, 1 = GRAVAR/DESCARTAR, 2 = NOVA REDE/TRAZER DO TAB5
static int wf_botoes(void) { return ha_pendente() ? 1 : (wf_lista_visivel() ? 2 : 0); }

int rede_altura(int, void*) { return RD_LIN; }

// A network saved on the node: the name, the state and the forget "x".
void rede_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  mesh_wifi_rede_t r;
  if (!mesh_wifi_no_rede(i, &r)) { return; }
  const int larg = largura + 12;   // the list reserves the scroll bar column
  const bool no_no = s_cfg.wifi_ssid[0] && strcmp(r.ssid, s_cfg.wifi_ssid) == 0;
  const bool escolhida = s_mud.wifi_ssid[0] && strcmp(r.ssid, s_mud.wifi_ssid) == 0;
  char t[60];
  snprintf(t, sizeof(t), "%s", r.ssid);
  d.setTextDatum(middle_left);
  tab5_texto(d, 3.6f);
  d.setTextColor(escolhida ? TEMA_ALERTA : TEMA_TEXTO, TEMA_FUNDO);
  tab5_cortar_ao_caber(d, t, larg - 440);
  d.drawString(t, x0 + 32, y + RD_LIN / 2);
  tab5_texto(d, 2.8f);
  d.setTextColor(escolhida ? TEMA_ALERTA : no_no ? TEMA_SUCESSO : TEMA_TEXTO_3, TEMA_FUNDO);
  d.drawString(escolhida ? tr("• escolhida") : no_no ? tr("● no nó agora") : (r.aberta ? tr("aberta") : tr("com senha")), x0 + larg - 380, y + RD_LIN / 2);
  const int cx = x0 + larg - 70, cy = y + RD_LIN / 2;   // "x" to forget
  d.drawLine(cx - 12, cy - 12, cx + 12, cy + 12, TEMA_PERIGO);
  d.drawLine(cx - 12, cy + 12, cx + 12, cy - 12, TEMA_PERIGO);
  d.drawFastHLine(x0 + 16, y + RD_LIN - 2, larg - 32, TEMA_LINHA);
  memset(&r, 0, sizeof(r));
}

static void desenhar_redes(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  lista_esconder(&s_l_cfg);
  const int reserva_l = (wf_botoes() ? RD_BTN_A + 14 + 14 : 0) + 40;   // buttons + message
  const area_t janela = { x0, a.y + WF_LISTA_Y, larg, a.altura - WF_LISTA_Y - reserva_l };
  const uint32_t assin = mesh_wifi_no_versao() * 31u + fnv_str(s_redes_msg) + fnv_str(s_cfg.wifi_ssid) * 7u
                       + fnv_str(s_mud.wifi_ssid) * 13u + (uint32_t)s_cfg.estado * 131u + (s_cfg.wifi_ligado ? 977u : 0)
                       + (s_mud.wifi_muda ? 1999u : 0) + (s_mud.wifi_liga ? 4001u : 0) + (ha_pendente() ? 8009u : 0);
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) {
    // Nothing changed on the page: only the list, which may be scrolling.
    if (wf_lista_visivel() && mesh_wifi_no_n() > 0) { lista_desenhar(&s_l_redes, d, janela, TEMA_FUNDO, false); }
    return;
  }
  s_assin_ant = assin;

  tab5_tela_limpar(x0, a.y, larg, a.altura, TEMA_FUNDO);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  tab5_texto(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(tr("Wi-Fi do nó"), x0 + 250, a.y + 40);

  // Switch row.
  const bool liga = wf_liga_visivel();
  const bool pode = s_cfg.estado == MESH_CFG_PRONTO && s_cfg.wifi_conhecido;
  icone_cfg(d, RC_WIFI, x0 + 50, a.y + WF_TOG_Y + WF_TOG_H / 2, pode ? TEMA_DESTAQUE : TEMA_TEXTO_3);
  tab5_texto(d, 3.6f);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(tr("Wi-Fi do nó"), x0 + 96, a.y + WF_TOG_Y + 30);
  tab5_texto(d, 2.8f);
  char sub[96];
  uint16_t csub = TEMA_TEXTO_3;
  if (!s_cfg.wifi_conhecido)               { snprintf(sub, sizeof(sub), tr("o nó ainda não enviou a configuração de rede")); }
  else if (s_mud.wifi_muda && s_mud.wifi_liga != s_cfg.wifi_ligado) {
    snprintf(sub, sizeof(sub), tr("vai %s ao gravar - o nó reinicia"), s_mud.wifi_liga ? tr("ligar") : tr("desligar")); csub = TEMA_ALERTA;
  }
  else if (s_cfg.wifi_ligado)              { snprintf(sub, sizeof(sub), tr("ligado%s%s"), s_cfg.wifi_ssid[0] ? tr(" em ") : "", s_cfg.wifi_ssid); csub = TEMA_SUCESSO; }
  else                                     { snprintf(sub, sizeof(sub), tr("desligado")); }
  d.setTextColor(csub, TEMA_FUNDO);
  cortar_ao_caber(d, sub, larg - 96 - 24 - 132 - 24);   // stops before the switch
  d.drawString(sub, x0 + 96, a.y + WF_TOG_Y + 70);
  interruptor(d, x0 + larg - 24 - 132, a.y + WF_TOG_Y + (WF_TOG_H - 68) / 2, 132, 68, liga, pode);
  d.drawFastHLine(x0 + 16, a.y + WF_TOG_Y + WF_TOG_H, larg - 32, TEMA_LINHA);

  // List of networks.
  const int botoes = wf_botoes();
  if (wf_lista_visivel()) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr("REDES  (toque para escolher)"), x0 + 32, a.y + WF_LISTA_Y - 24);
    const int n = mesh_wifi_no_n();
    if (n == 0) {
      tab5_texto(d, 2.8f);
      d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
      char t0[64];
      snprintf(t0, sizeof(t0), tr("nenhuma rede guardada - use NOVA REDE ou TRAZER DO TAB5"));
      cortar_ao_caber(d, t0, larg - 64);
      d.drawString(t0, x0 + 32, a.y + WF_LISTA_Y + 36);
    }
    if (n > 0) {
      lista_itens_mudaram(&s_l_redes, n);
      lista_desenhar(&s_l_redes, d, janela, TEMA_FUNDO, true);
    } else {
      lista_esconder(&s_l_redes);
    }
  } else if (s_cfg.wifi_conhecido && !s_cfg.wifi_ligado) {
    lista_esconder(&s_l_redes);
    tab5_texto(d, 2.8f);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    char t0[80];
    snprintf(t0, sizeof(t0), tr("ligue o Wi-Fi e grave: depois de reiniciar, as redes aparecem aqui"));
    cortar_ao_caber(d, t0, larg - 64);
    d.drawString(t0, x0 + 32, a.y + WF_LISTA_Y + 10);
  }

  // Message and buttons.
  d.setTextDatum(middle_left);
  tab5_texto(d, 2.4f);
  const int ym = a.y + a.altura - (botoes ? RD_BTN_A + 14 + 14 : 0) - 24;
  if (s_cfg.estado == MESH_CFG_GRAVANDO) { d.setTextColor(TEMA_ALERTA, TEMA_FUNDO); d.drawString(tr("gravando - o nó está reiniciando..."), x0 + 24, ym); }
  else if (s_redes_msg[0]) {
    char m[sizeof(s_redes_msg)];
    snprintf(m, sizeof(m), "%s", s_redes_msg);
    cortar_ao_caber(d, m, larg - 48);
    d.setTextColor(TEMA_ALERTA, TEMA_FUNDO);
    d.drawString(m, x0 + 24, ym);
  }
  const int by = a.y + a.altura - RD_BTN_A - 14;
  const int bl = (larg - 24 * 3) / 2;
  const bool pronto = s_cfg.estado == MESH_CFG_PRONTO;
  if (botoes == 1) {
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, tr("GRAVAR NO NÓ"), pronto ? TEMA_DESTAQUE : TEMA_SUPERFICIE_ALTA, pronto ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO_3);
    tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, tr("DESCARTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  } else if (botoes == 2) {
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, tr("NOVA REDE"), TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE);
    tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, tr("TRAZER DO TAB5"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  }
  d.setTextDatum(top_left);
}

// ---- the Tab5's internet (MQTT server) -------------------------------------
// What the Tab5 uses to talk to the mesh WITHOUT the node: the on/off for "internet
// when the node is missing" and the MQTT server. Nothing here goes through the node - it changes
// at once, with or without it. Changing the server restarts the Mesh service (the
// MQTT client reconnects to the new one), as in Settings > Mesh over the internet.
static const int NET_TOG_Y = 84, NET_TOG_H = 96, NET_LISTA_Y = 84 + 96 + 44, NET_LIN = 58;
static const int NET_CAMPO_Y = 96, NET_CAMPO_LIN = 76;
static const char* const NET_ROTULO[5] = {
  N_("nome (como aparece na lista)"), N_("endereço do servidor (host ou host:porta)"), N_("usuário"), N_("senha"),
  N_("raiz dos tópicos (ex.: msh/EU_868)"),
};
static const int NET_EDITAR_L = 120;   // the "edit" at the end of each server in the list

static char* net_campo(int k, size_t* tam) { return mesh_mqtt_campo(&s_net_p, k, tam); }

static void net_abrir_editor(int i)
{
  s_net_i = i;
  s_net_modelo = 0;
  if (!mesh_mqtt_para_editar(i, &s_net_p)) { return; }
  s_net_ed = true;
  s_net_apagar_ms = 0;
  s_redes_msg[0] = 0;
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

static void net_fechar_editor(void)
{
  memset(&s_net_p, 0, sizeof(s_net_p));   // the typed password does not stay in RAM
  s_net_ed = false;
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

static void net_usar(uint8_t escolha)
{
  mesh_mqtt_usar_servidor(escolha);
  snprintf(s_redes_msg, sizeof(s_redes_msg), tr("servidor: %s - reconectando"), mesh_mqtt_servidor_nome(escolha));
  sistema_pedir_redesenho_completo();
}

// THE LIST IN COLUMNS: in landscape the area is ~580 px high and the up to 10 items
// (automatic, the saved ones, "+ add") do not fit one under the other - they go
// in two columns. In portrait, just one. Drawing and touch use this same sum.
// Returns the item at (lx, ly) in the list, or -1; in *editar, whether the tap landed on
// the "edit" at the end.
static int net_colunas(const area_t& a) { return em_pe(a) ? 1 : 2; }
static int net_itens(void) { return mesh_mqtt_n_servidores() + (mesh_mqtt_pode_adicionar() ? 1 : 0); }
static int net_linhas(const area_t& a)
{
  const int c = net_colunas(a);
  return (net_itens() + c - 1) / c;
}
static int net_item_em(const area_t& a, int lx, int ly, bool* editar)
{
  const int col_l = (a.largura - TRILHO_L - 32) / net_colunas(a);
  if (lx < 16 || ly < 0) { return -1; }
  const int c = (lx - 16) / col_l, r = ly / NET_LIN;
  if (c >= net_colunas(a) || r >= net_linhas(a)) { return -1; }
  const int i = c * net_linhas(a) + r;
  if (editar) { *editar = (lx - 16) - c * col_l >= col_l - NET_EDITAR_L - 8; }
  return i < net_itens() ? i : -1;
}

// The footer buttons of the custom server screen (USAR / VOLTAR A LISTA - use / back to list).
static int net_btn_y(const area_t& a) { return a.altura - RD_BTN_A - 14; }

static void desenhar_net(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  lista_esconder(&s_l_cfg);
  if (s_net_apagar_ms && tab5_agora_ms() - s_net_apagar_ms >= 4000) { s_net_apagar_ms = 0; }   // gave up
  const char* est = tr(tab5_mesh_internet_estado());
  const uint32_t assin = fnv_str(est) + fnv_str(s_redes_msg) * 7u + (uint32_t)ajustes_mqtt_servidor() * 131u
                       + (ajustes_mesh_internet() ? 977u : 0) + (s_net_ed ? 1999u : 0)
                       + fnv_str(s_net_p.srv) * 13u + fnv_str(s_net_p.usr) * 17u + fnv_str(s_net_p.pwd) * 19u
                       + fnv_str(s_net_p.raiz) * 23u + fnv_str(s_net_p.nome) * 29u + (uint32_t)s_net_i * 31u
                       + (uint32_t)s_net_modelo * 37u + (s_net_apagar_ms ? 41u : 0) + (uint32_t)net_itens() * 43u;
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;

  tab5_tela_limpar(x0, a.y, larg, a.altura, TEMA_FUNDO);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  tab5_texto(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(!s_net_ed ? tr("Internet do Tab5") : s_net_i ? tr("Editar servidor") : tr("Novo servidor"), x0 + 250, a.y + 40);

  if (s_net_ed) {
    for (int k = 0; k < 5; ++k) {
      const int y = a.y + NET_CAMPO_Y + k * NET_CAMPO_LIN;
      size_t tam;
      const char* v = net_campo(k, &tam);
      char mostra[72];
      if (k == 3 && v[0]) { snprintf(mostra, sizeof(mostra), "%.*s", (int)strlen(v), "********************************"); }
      else                { snprintf(mostra, sizeof(mostra), "%s", v[0] ? v : tr("(vazio - tocar para editar)")); }
      tab5_texto(d, 2.4f);
      d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
      d.drawString(tr(NET_ROTULO[k]), x0 + 32, y + 16);
      tab5_texto(d, 3.6f);
      d.setTextColor(v[0] ? TEMA_TEXTO : TEMA_TEXTO_3, TEMA_FUNDO);
      tab5_cortar_ao_caber(d, mostra, larg - 64);
      d.drawString(mostra, x0 + 32, y + 48);
      d.drawFastHLine(x0 + 16, y + NET_CAMPO_LIN - 2, larg - 32, TEMA_LINHA);
    }
    // USAR (save and use) | MODELO (template, on a new one) or APAGAR (delete) | VOLTAR (back).
    const int by = a.y + net_btn_y(a);
    const int bl = (larg - 24 * 4) / 3;
    const bool pode = s_net_p.srv[0] != 0;
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, tr("USAR"),
               pode ? TEMA_DESTAQUE : TEMA_SUPERFICIE_ALTA, pode ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO_3);
    if (s_net_i == 0) {
      char m[48];
      snprintf(m, sizeof(m), tr("MODELO: %s"), mesh_mqtt_modelo(s_net_modelo)->nome);
      tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, m, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    } else {
      tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, s_net_apagar_ms ? tr("CONFIRMAR") : tr("APAGAR"),
                 TEMA_PERIGO, TEMA_SOBRE_DESTAQUE);
    }
    tab5_botao(d, x0 + 72 + 2 * bl, by, bl, RD_BTN_A, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    d.setTextDatum(top_left);
    return;
  }

  // Switch row: internet when the node is missing.
  const bool lig = ajustes_mesh_internet();
  icone_cfg(d, RC_MQTT, x0 + 50, a.y + NET_TOG_Y + NET_TOG_H / 2, *tinta_cfg(d, RC_MQTT));
  tab5_texto(d, 3.6f);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(tr("Internet quando faltar o nó"), x0 + 96, a.y + NET_TOG_Y + 28);
  tab5_texto(d, 2.4f);
  char sub[96];
  snprintf(sub, sizeof(sub), "%s", est);
  d.setTextColor(lig ? TEMA_TEXTO_2 : TEMA_TEXTO_3, TEMA_FUNDO);
  cortar_ao_caber(d, sub, larg - 96 - 24 - 132 - 24);   // stops before the switch
  d.drawString(sub, x0 + 96, a.y + NET_TOG_Y + 66);
  interruptor(d, x0 + larg - 24 - 132, a.y + NET_TOG_Y + (NET_TOG_H - 68) / 2, 132, 68, lig, true);
  d.drawFastHLine(x0 + 16, a.y + NET_TOG_Y + NET_TOG_H, larg - 32, TEMA_LINHA);

  // List of servers + the custom one.
  tab5_texto(d, 2.4f);
  d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
  d.drawString(tr("SERVIDOR MQTT  (toque para usar)"), x0 + 32, a.y + NET_LISTA_Y - 22);
  const int n = mesh_mqtt_n_servidores();
  const int itens = net_itens();
  const uint8_t esc = ajustes_mqtt_servidor();
  const int linhas = net_linhas(a);
  const int col_l = (larg - 32) / net_colunas(a);
  for (int i = 0; i < itens; ++i) {
    const bool novo = (i == n);
    const bool sel = !novo && esc == i;
    const int y = a.y + NET_LISTA_Y + (i % linhas) * NET_LIN;
    const int cx = x0 + 16 + (i / linhas) * col_l;   // start of the column
    char t[72];
    mesh_mqtt_servidor_t s;
    if (novo) { snprintf(t, sizeof(t), tr("+ adicionar servidor")); }
    else if (mesh_mqtt_servidor(i, &s)) { snprintf(t, sizeof(t), "%s%s", sel ? "● " : "", s.nome); memset(&s, 0, sizeof(s)); }
    else { continue; }
    tab5_texto(d, 3.2f);
    d.setTextColor(novo ? TEMA_DESTAQUE : sel ? TEMA_SUCESSO : TEMA_TEXTO, TEMA_FUNDO);
    tab5_cortar_ao_caber(d, t, col_l - 32 - NET_EDITAR_L);
    d.drawString(t, cx + 16, y + NET_LIN / 2);
    if (i > 0 && !novo) {   // the automatic one is not editable
      tab5_texto(d, 2.6f);
      d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
      d.drawString(tr("editar"), cx + col_l - NET_EDITAR_L, y + NET_LIN / 2);
    }
    d.drawFastHLine(cx + 8, y + NET_LIN - 2, col_l - 16, TEMA_LINHA);
  }
  if (s_redes_msg[0]) {
    char m[sizeof(s_redes_msg)];
    snprintf(m, sizeof(m), "%s", s_redes_msg);
    tab5_texto(d, 2.4f);
    cortar_ao_caber(d, m, larg - 48);
    d.setTextColor(TEMA_ALERTA, TEMA_FUNDO);
    d.drawString(m, x0 + 24, a.y + a.altura - 20);
  }
  d.setTextDatum(top_left);
}

// The "No navegador do PC" (in the PC browser) sub-page: BACK, the title and the system panel.
static void desenhar_web(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  lista_esconder(&s_l_cfg);
  static uint32_t s_assin_ant = 0;
  const uint32_t assin = painel_web_assinatura();
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;
  tab5_tela_limpar(x0, a.y, larg, CFG_TOPO, TEMA_FUNDO);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  tab5_texto(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(tr("No navegador do PC"), x0 + 250, a.y + CFG_TOPO / 2);
  d.setTextDatum(top_left);
  painel_web_desenhar(d, x0, a.y + CFG_TOPO, larg, a.altura - CFG_TOPO, APP_MESH_NOME);
}

void desenhar_config(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_cfg_ler(&s_cfg);
  const int larg = a.x + a.largura - x0;

  if (s_redes_pg) { desenhar_redes(d, a, x0, larg); return; }

  if (s_net_pg) { desenhar_net(d, a, x0, larg); return; }

  if (s_diario) { desenhar_diario(d, a, x0, larg); return; }

  if (s_web_pg) { desenhar_web(d, a, x0, larg); return; }

  // EDITING A NAME: field and keyboard, nothing else on the page.
  if (s_editando) {
    lista_esconder(&s_l_cfg);
    tecvirt_posicionar(&s_kb_cfg, a);
    static char s_ed_ant[sizeof(s_edit_txt) + 2];
    char ed[sizeof(s_edit_txt) + 2];
    snprintf(ed, sizeof(ed), "%s_", s_edit_txt);
    if (TUDO) {
      tab5_tela_limpar(x0, a.y, larg, a.altura - tecvirt_altura(a), TEMA_FUNDO);
      tab5_texto(d, 2.8f);
      d.setTextDatum(top_left);
      d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
      const char* tit = s_editando == RC_LONGO ? tr("nome longo do nó (até 39 letras)")
                      : s_editando == RC_CURTO ? tr("nome curto do nó (até 4 letras)")
                      : s_editando == ED_WIFI_SSID ? tr("rede Wi-Fi do nó (até 32 letras)")
                      : ed_mq(s_editando) ? tr(NET_ROTULO[s_editando - ED_MQ_NOME])
                      : tr("senha do Wi-Fi do nó (8 a 64 letras)");
      char tit2[96];
      if (s_editando == ED_WIFI_SENHA && s_mud.wifi_ssid[0]) { snprintf(tit2, sizeof(tit2), tr("senha da rede \"%s\" (8 a 64 letras)"), s_mud.wifi_ssid); }
      else { snprintf(tit2, sizeof(tit2), "%s", tit); }
      cortar_ao_caber(d, tit2, larg - 48);
      d.drawString(tit2, x0 + 24, a.y + 24);
    }
    if (pintura_mudou_s(&s_p, s_ed_ant, sizeof(s_ed_ant), ed)) {
      if (!TUDO) { tab5_tela_limpar(x0 + 24, a.y + 70, larg - 48, 60, TEMA_FUNDO); }
      tab5_texto(d, 4.2f);
      d.setTextDatum(top_left);
      d.setTextColor(TEMA_ALERTA, TEMA_FUNDO);
      d.drawString(ed, x0 + 24, a.y + 76);
    }
    tecvirt_pintar(&s_kb_cfg, d, TUDO);
    return;
  }

  // Header: the state of the conversation with the node.
  char est[96];
  uint16_t cor_est = TEMA_TEXTO_3;
  if (s_cfg.estado == MESH_CFG_GRAVANDO)    { snprintf(est, sizeof(est), tr("gravando - o nó está reiniciando...")); cor_est = TEMA_ALERTA; }
  else if (s_cfg.estado == MESH_CFG_PRONTO) { snprintf(est, sizeof(est), "%s", s_cfg_msg[0] ? s_cfg_msg : tr("conectado ao nó")); cor_est = TEMA_SUCESSO; }
  else { snprintf(est, sizeof(est), tr("sem o nó (cabo ou Bluetooth) - só o que é do Tab5")); }
  static char s_est_ant[96];
  // Inside a group: BACK and its name in place of the title.
  const char* titulo = s_grupo < 0 ? tr("Configurar o nó") : tr(GRUPOS[s_grupo].nome);
  const int tx = s_grupo < 0 ? x0 + 24 : x0 + 250;
  tab5_texto(d, 3.6f);
  const int ex_min = tx + (int)d.textWidth(titulo) + 32;
  const int ex = ex_min > x0 + 380 ? ex_min : x0 + 380;
  if (TUDO) {
    tab5_tela_limpar(x0, a.y, larg, CFG_TOPO, TEMA_FUNDO);
    if (s_grupo >= 0) { tab5_botao(d, x0 + 24, a.y + 12, 200, 56, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO); }
    tab5_texto(d, 3.6f);
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
    d.drawString(titulo, tx, a.y + CFG_TOPO / 2);
  }
  if (pintura_mudou_s(&s_p, s_est_ant, sizeof(s_est_ant), est)) {
    if (!TUDO) { tab5_tela_limpar(ex, a.y, a.x + a.largura - ex, CFG_TOPO, TEMA_FUNDO); }
    tab5_texto(d, 2.8f);
    cortar_ao_caber(d, est, a.x + a.largura - 16 - ex);
    d.setTextDatum(middle_left);
    d.setTextColor(cor_est, TEMA_FUNDO);
    d.drawString(est, ex, a.y + CFG_TOPO / 2);
  }
  d.setTextDatum(top_left);

  // Rows: only those whose text changed (the page runs at 1 Hz).
  const int n_itens = cfg_n_itens();
  if (TUDO || s_l_cfg.n != n_itens) {
    lista_itens_mudaram(&s_l_cfg, n_itens);
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
  } else {
    for (int i = 0; i < n_itens; ++i) {
      // Never drawn: scrolling draws it when it appears, with the
      // current value. There is nothing to compare - and marking it here would drag
      // the visible ones along (see s_cfg_pintada).
      if (!s_cfg_pintada[i]) { continue; }
      char rot[48], val[96] = "", sig[112];
      bool pend = false;
      linha_item(i, rot, sizeof(rot), val, sizeof(val), &pend);
      snprintf(sig, sizeof(sig), "%d|%s", (int)pend, val);
      if (strcmp(sig, s_cfg_ant[i]) != 0) { lista_item_mudou(&s_l_cfg, i); }
    }
  }
  // The gear lights up and dims with the state: everything changes at once.
  static int s_estado_ant = -1;
  if (s_estado_ant != s_cfg.estado) {
    s_estado_ant = s_cfg.estado;
    lista_itens_mudaram(&s_l_cfg, n_itens);
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
  }
  lista_desenhar(&s_l_cfg, d, janela_cfg(a, x0), TEMA_FUNDO, TUDO);

  // Footer: GRAVAR (with something pending and the node ready) and DESCARTAR.
  const bool pend = ha_pendente();
  const int estado_rod = (pend ? 1 : 0) | (s_cfg.estado == MESH_CFG_PRONTO ? 2 : 0);
  static int s_rod_ant = -1;
  if (TUDO || estado_rod != s_rod_ant) {
    s_rod_ant = estado_rod;
    // With nothing pending the strip belongs to the list (already drawn above): nothing to clear.
    if (pend) {
      const int by = a.y + a.altura - CFG_RODAPE + 12;
      tab5_tela_limpar(x0, a.y + a.altura - CFG_RODAPE, larg, CFG_RODAPE, TEMA_FUNDO);
      const bool pode = (s_cfg.estado == MESH_CFG_PRONTO);
      const int bl = cfg_btn_l(a);
      tab5_botao(d, x0 + 24, by, bl, CFG_BTN_A, tr("GRAVAR NO NÓ"),
                 pode ? TEMA_DESTAQUE : TEMA_SUPERFICIE_ALTA, pode ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO_3);
      tab5_botao(d, x0 + 24 + bl + 24, by, bl, CFG_BTN_A, tr("DESCARTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
      // The hint only where there is room beside the buttons (in portrait, there is not).
      tab5_texto(d, 2.4f);
      const int dx = x0 + 24 + 2 * (bl + 24);
      if (dx + (int)d.textWidth(tr("o nó grava e reinicia")) < a.x + a.largura - 16) {
        d.setTextDatum(middle_left);
        d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
        d.drawString(tr("o nó grava e reinicia"), dx, by + CFG_BTN_A / 2);
        d.setTextDatum(top_left);
      }
    }
  }
}

static void cfg_aviso(const char* msg)
{
  snprintf(s_cfg_msg, sizeof(s_cfg_msg), "%s", msg);
  sistema_pedir_redesenho();
}

void cfg_fechar_edicao(bool confirmar)
{
  // Custom server field: empty is valid (username and password may not exist).
  if (ed_mq(s_editando)) {
    if (confirmar) {
      size_t tam;
      char* c = net_campo(s_editando - ED_MQ_NOME, &tam);
      snprintf(c, tam, "%s", s_edit_txt);
    }
    memset(s_edit_txt, 0, sizeof(s_edit_txt));   // it may have been the password
    s_editando = 0;
    tecvirt_fechar(&s_kb_cfg);
    s_net_pg = true;
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  if (confirmar && s_editando == ED_WIFI_SENHA && strlen(s_edit_txt) < 8) {
    cfg_aviso(tr("a senha do Wi-Fi precisa de 8 letras ou mais"));   // the firmware would refuse it
    snprintf(s_redes_msg, sizeof(s_redes_msg), tr("a senha do Wi-Fi precisa de 8 letras ou mais"));
    confirmar = false;
  }
  if (confirmar && s_edit_txt[0]) {
    char* dst; size_t tam;
    switch (s_editando) {
      case RC_LONGO:     dst = s_mud.longo;     tam = sizeof(s_mud.longo);     break;
      case ED_WIFI_SSID: dst = s_mud.wifi_ssid; tam = sizeof(s_mud.wifi_ssid); break;
      case ED_WIFI_SENHA: dst = s_mud.wifi_psk; tam = sizeof(s_mud.wifi_psk);  break;
      default:           dst = s_mud.curto;     tam = sizeof(s_mud.curto);     break;
    }
    snprintf(dst, tam, "%s", s_edit_txt);
  }
  const bool era_wifi = s_editando == ED_WIFI_SSID || s_editando == ED_WIFI_SENHA;
  const bool encadear = confirmar && s_editando == ED_WIFI_SSID && s_edit_txt[0] && !s_mud.wifi_psk[0];
  if (confirmar && s_editando == ED_WIFI_SENHA && s_edit_txt[0]) {
    s_mud.wifi_aberta = false;
    s_mud.wifi_muda = true; s_mud.wifi_liga = true;   // new network typed: ready to write
    snprintf(s_redes_msg, sizeof(s_redes_msg), tr("rede nova pronta - toque GRAVAR NO NÓ (o nó reinicia)"));
  }
  memset(s_edit_txt, 0, sizeof(s_edit_txt));   // it may have been the Wi-Fi password
  // New network typed: next it asks for the password, without leaving the Wi-Fi screen.
  s_editando = encadear ? ED_WIFI_SENHA : 0;
  if (encadear) { tecvirt_tipo(&s_kb_cfg, TECVIRT_SENHA); }   // from SSID to password, the same keyboard
  if (!encadear) { tecvirt_fechar(&s_kb_cfg); }
  if (era_wifi && !encadear) { s_redes_pg = true; }   // back to the Wi-Fi screen (confirmed or cancelled)
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

void cfg_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR) { cfg_fechar_edicao(true); return; }
  if (ac == TECVIRT_FECHAR) { cfg_fechar_edicao(false); return; }
  const size_t max = s_editando == RC_LONGO ? MESH_NOME_LONGO - 1
                   : s_editando == ED_WIFI_SSID ? 32
                   : s_editando == ED_WIFI_SENHA ? 64
                   : s_editando == ED_MQ_NOME ? sizeof(s_net_p.nome) - 1
                   : s_editando == ED_MQ_SRV ? sizeof(s_net_p.srv) - 1
                   : s_editando == ED_MQ_USR ? sizeof(s_net_p.usr) - 1
                   : s_editando == ED_MQ_PWD ? sizeof(s_net_p.pwd) - 1
                   : s_editando == ED_MQ_RAIZ ? sizeof(s_net_p.raiz) - 1 : MESH_NOME_CURTO - 2;   // short: 4
  // Node name and password are ASCII (see ao_teclar). 'max' characters + the zero.
  tecvirt_editar(s_edit_txt, max + 1 < sizeof(s_edit_txt) ? max + 1 : sizeof(s_edit_txt), ac);
  sistema_pedir_redesenho();
}

// GRAVAR NO NO (write to node): validates the pending Wi-Fi, sends the transaction and saves the network.
static void cfg_gravar(void)
{
  if (s_cfg.estado != MESH_CFG_PRONTO) { return; }
  // Switch flipped without a new network: resends the current network WITH the password we
  // saved, so the node's password is not lost when the section is rewritten.
  if (s_mud.wifi_muda && !s_mud.wifi_ssid[0] && s_cfg.wifi_ssid[0]) {
    for (int k = 0; k < mesh_wifi_no_n(); ++k) {
      mesh_wifi_rede_t r;
      if (mesh_wifi_no_rede(k, &r) && strcmp(r.ssid, s_cfg.wifi_ssid) == 0) {
        snprintf(s_mud.wifi_ssid, sizeof(s_mud.wifi_ssid), "%s", r.ssid);
        snprintf(s_mud.wifi_psk, sizeof(s_mud.wifi_psk), "%s", r.senha);
        s_mud.wifi_aberta = r.aberta;
        memset(&r, 0, sizeof(r));
        break;
      }
      memset(&r, 0, sizeof(r));
    }
  }
  if (s_mud.wifi_psk[0] && !s_mud.wifi_ssid[0]) { cfg_aviso(tr("Wi-Fi: informe a rede E a senha")); return; }
  if (s_mud.wifi_ssid[0] && !s_mud.wifi_psk[0] && !s_mud.wifi_aberta) { cfg_aviso(tr("Wi-Fi: informe a rede E a senha")); return; }
  char nova_rede[33], nova_senha[65];
  snprintf(nova_rede, sizeof(nova_rede), "%s", s_mud.wifi_ssid);
  snprintf(nova_senha, sizeof(nova_senha), "%s", s_mud.wifi_psk);
  if (mesh_cfg_gravar(&s_mud)) {
    // Saved to choose next time (SD card, encrypted password).
    if (nova_rede[0]) { mesh_wifi_no_salvar(nova_rede, nova_senha); }
    memset(&s_mud, 0, sizeof(s_mud)); cfg_aviso(""); s_redes_msg[0] = 0;
  }
  else {
    const char* motivo = mesh_cfg_motivo()[0] ? mesh_cfg_motivo() : tr("não deu para gravar - veja o log");
    cfg_aviso(motivo);
    snprintf(s_redes_msg, sizeof(s_redes_msg), "%s", motivo);
  }
  memset(nova_senha, 0, sizeof(nova_senha));
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

void tocar_config(int x, int y)
{
  const area_t a = sistema_area();
  if (s_redes_pg) {
    const int bx = TRILHO_L + 24;
    const int larg = a.largura - TRILHO_L;
    const int by = a.altura - RD_BTN_A - 14;
    const int bl = (larg - 24 * 3) / 2;
    const int botoes = wf_botoes();
    const bool bt_y = y >= by && y < by + RD_BTN_A;
    const bool bt_1 = bt_y && x >= bx && x < bx + bl;
    const bool bt_2 = bt_y && x >= bx + bl + 24 && x < bx + 2 * bl + 24;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {                  // BACK
      s_redes_pg = false;
      lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (y >= WF_TOG_Y && y < WF_TOG_Y + WF_TOG_H) {               // switch
      if (s_cfg.estado != MESH_CFG_PRONTO || !s_cfg.wifi_conhecido) { return; }
      const bool novo = !wf_liga_visivel();
      s_mud.wifi_liga = novo;
      s_mud.wifi_muda = (novo != s_cfg.wifi_ligado);
      if (!novo || !s_mud.wifi_muda) { memset(s_mud.wifi_ssid, 0, sizeof(s_mud.wifi_ssid)); memset(s_mud.wifi_psk, 0, sizeof(s_mud.wifi_psk)); s_mud.wifi_aberta = false; }
      s_redes_msg[0] = 0;
      sistema_pedir_redesenho();
    } else if (botoes == 1 && bt_1) {                                    // WRITE TO NODE
      cfg_gravar();
    } else if (botoes == 1 && bt_2) {                                    // DISCARD
      memset(&s_mud, 0, sizeof(s_mud));
      s_redes_msg[0] = 0;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (botoes == 2 && bt_1) {                                    // NEW NETWORK
      s_redes_pg = false;
      memset(s_mud.wifi_psk, 0, sizeof(s_mud.wifi_psk));
      s_mud.wifi_aberta = false;
      s_editando = ED_WIFI_SSID;
      s_edit_txt[0] = 0;
      tecvirt_abrir(&s_kb_cfg, TECVIRT_ASCII);
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (botoes == 2 && bt_2) {                                    // BRING FROM TAB5
      const int n = mesh_wifi_no_trazer_do_tab5();
      if (n < 0)       { snprintf(s_redes_msg, sizeof(s_redes_msg), tr("o Tab5 não tem redes carregadas (ligue o Wi-Fi do Tab5 uma vez)")); }
      else if (n == 0) { snprintf(s_redes_msg, sizeof(s_redes_msg), tr("nada novo: todas as redes do Tab5 já estão aqui")); }
      else             { snprintf(s_redes_msg, sizeof(s_redes_msg), tr("%d rede(s) trazida(s) do Tab5"), n); }
      sistema_pedir_redesenho();
    } else if (wf_lista_visivel()) {
      const int i = lista_item_tocado(&s_l_redes, x, y, nullptr);   // -1 outside; LISTA_ARRASTE = scrolled
      mesh_wifi_rede_t r;
      if (i >= 0 && mesh_wifi_no_rede(i, &r)) {
        if (x >= TRILHO_L + larg - 130) {                  // the "x": forget
          mesh_wifi_no_esquecer(i);
          snprintf(s_redes_msg, sizeof(s_redes_msg), tr("\"%s\" esquecida"), r.ssid);
        } else if (s_cfg.estado == MESH_CFG_PRONTO) {      // choose: stays pending, GRAVAR appears
          snprintf(s_mud.wifi_ssid, sizeof(s_mud.wifi_ssid), "%s", r.ssid);
          snprintf(s_mud.wifi_psk, sizeof(s_mud.wifi_psk), "%s", r.senha);
          s_mud.wifi_aberta = r.aberta;
          s_mud.wifi_muda = true;
          s_mud.wifi_liga = true;
          snprintf(s_redes_msg, sizeof(s_redes_msg), tr("\"%s\" escolhida - toque GRAVAR NO NÓ (o nó reinicia)"), r.ssid);
        }
        memset(&r, 0, sizeof(r));
        sistema_pedir_redesenho_completo();
      }
    }
    return;
  }
  if (s_net_pg) {
    const int bx = TRILHO_L + 24;
    const int larg = a.largura - TRILHO_L;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {                  // BACK
      if (s_net_ed) { s_net_ed = false; memset(&s_net_p, 0, sizeof(s_net_p)); }
      else {
        s_net_pg = false;
        lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
        memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      }
      s_redes_msg[0] = 0;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    }
    if (s_net_ed) {
      const int by = net_btn_y(a);
      const int bl = (larg - 24 * 4) / 3;
      if (y >= by && y < by + RD_BTN_A) {
        if (x >= bx && x < bx + bl) {                                    // USE: save and use
          const int i = mesh_mqtt_salvar_e_usar(s_net_i, &s_net_p);   // writes, uses and reconnects
          if (!i) { return; }   // no address (or list full): the button is already dimmed
          net_fechar_editor();
          snprintf(s_redes_msg, sizeof(s_redes_msg), tr("servidor: %s - reconectando"), mesh_mqtt_servidor_nome((uint8_t)i));
          sistema_pedir_redesenho_completo();
        } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24) {
          if (s_net_i == 0) {                                            // TEMPLATE: the next one
            s_net_modelo = mesh_mqtt_proximo_modelo(s_net_modelo, &s_net_p);
            sistema_pedir_redesenho_completo();
          } else if (s_net_apagar_ms && tab5_agora_ms() - s_net_apagar_ms < 4000) {   // DELETE, confirmed
            const bool reconectou = mesh_mqtt_apagar_e_reconectar(s_net_i);   // it was the one in use: back to automatic
            net_fechar_editor();
            if (reconectou) {
              snprintf(s_redes_msg, sizeof(s_redes_msg), tr("servidor: %s - reconectando"), mesh_mqtt_servidor_nome(AJUSTES_MQTT_AUTOMATICO));
              sistema_pedir_redesenho_completo();
            }
          } else {
            s_net_apagar_ms = tab5_agora_ms();
            sistema_pedir_redesenho_completo();
          }
        } else if (x >= bx + 2 * bl + 48 && x < bx + 3 * bl + 48) {      // BACK TO THE LIST
          net_fechar_editor();
        }
        return;
      }
      const int k = (y - NET_CAMPO_Y) / NET_CAMPO_LIN;
      if (y >= NET_CAMPO_Y && k >= 0 && k < 5) {                         // a field: the keyboard
        size_t tam;
        snprintf(s_edit_txt, sizeof(s_edit_txt), "%s", net_campo(k, &tam));
        s_editando = ED_MQ_NOME + k;
        s_net_pg = false;
        tecvirt_abrir(&s_kb_cfg, s_editando == ED_MQ_PWD ? TECVIRT_SENHA : TECVIRT_ASCII);
        pintura_pedir(&s_p);
        sistema_pedir_redesenho_completo();
      }
      return;
    }
    if (y >= NET_TOG_Y && y < NET_TOG_Y + NET_TOG_H) {                    // switch
      ajustes_definir_mesh_internet(!ajustes_mesh_internet());
      sistema_pedir_redesenho();
      return;
    }
    const int n = mesh_mqtt_n_servidores();
    bool editar = false;
    const int i = net_item_em(a, x - TRILHO_L, y - NET_LISTA_Y, &editar);
    if (i < 0) { return; }
    s_redes_msg[0] = 0;
    if (i == n)               { net_abrir_editor(0); }   // + add
    else if (i > 0 && editar) { net_abrir_editor(i); }
    else if (i != ajustes_mqtt_servidor()) { net_usar((uint8_t)i); }
    return;
  }
  if (s_web_pg) {
    if (y >= 12 && y < 68 && x >= TRILHO_L + 24 && x < TRILHO_L + 224) {
      s_web_pg = false;
      lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    }
    // The page changes via AVISO_WEB (turned on, turned off).
    painel_web_tocar(TRILHO_L, CFG_TOPO, a.largura - TRILHO_L, a.altura - CFG_TOPO, x, y, APP_MESH_NOME, tab5_mesh_web_ligar);
    return;
  }
  if (s_diario) {
    const int bx = TRILHO_L + 24;
    const int ly = a.altura - REI_RODAPE + 14;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {
      s_diario = false;
      s_limpar_ms = 0;
      lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (y >= ly && y < ly + 56 && x >= bx && x < bx + 320) {
      if (s_limpar_ms && (tab5_agora_ms() - s_limpar_ms) < 4000) { s_limpar_ms = 0; mesh_diario_limpar(); }
      else { s_limpar_ms = tab5_agora_ms() | 1u; }
      sistema_pedir_redesenho();
    }
    return;
  }
  if (s_editando) {
    if (tecvirt_contem(&s_kb_cfg, a.x + x, a.y + y)) {
      const int ac = tecvirt_clique(&s_kb_cfg, a.x + x, a.y + y);
      if (ac) { cfg_tecla(ac); }
    }
    return;
  }

  // BACK from inside a group: goes up to the list of groups.
  if (s_grupo >= 0 && y >= 12 && y < 68 && x >= TRILHO_L + 24 && x < TRILHO_L + 224) {
    s_grupo = -1;
    s_reiniciar_ms = 0;
    lista_ao_inicio(&s_l_cfg);
    lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }

  // Footer.
  const int by = a.altura - CFG_RODAPE + 12;
  if (y >= by && y < by + CFG_BTN_A && ha_pendente()) {
    const int bx = TRILHO_L + 24;
    const int bl = cfg_btn_l(a);
    if (x >= bx && x < bx + bl) {
      cfg_gravar();
    } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24) {
      memset(&s_mud, 0, sizeof(s_mud));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    }
    return;
  }

  const int item = lista_item_tocado(&s_l_cfg, x, y, nullptr);
  if (item < 0) { return; }
  // In the list of groups: opens the tapped group.
  if (s_grupo < 0) {
    if (item >= G_TOTAL) { return; }
    if (item == G_PC) {
      s_web_pg = true;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    }
    if (item == G_INTERNET) {
      s_net_pg = true;
      s_net_ed = false;
      s_redes_msg[0] = 0;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    }
    s_grupo = item;
    s_cfg_msg[0] = 0;
    lista_ao_inicio(&s_l_cfg);
    lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  if (item >= GRUPOS[s_grupo].n) { return; }
  const int i = GRUPOS[s_grupo].rc[item];
  if (i == RC_I_REINICIOS) {
    s_diario = true;
    s_limpar_ms = 0;
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  if (i < 0 || i >= RC_EDITAVEIS) { return; }
  if (i == RC_SOM) { tab5_mesh_definir_beep(!tab5_mesh_beep_ligado()); sistema_pedir_redesenho(); return; }
  if (i == RC_LOC_TAB5) {
    ajustes_definir_mesh_posicao_min((uint8_t)opc_proximo(OPC_TAB5, N_OPC(OPC_TAB5), ajustes_mesh_posicao_min()));
    sistema_pedir_redesenho();
    return;
  }
  if (s_cfg.estado != MESH_CFG_PRONTO) { return; }
  if (i == RC_UPLINK) {
    if (s_cfg.uplink < 0) { cfg_aviso(tr("o canal principal do nó ainda não chegou")); return; }
    const bool atual = s_mud.uplink_muda ? s_mud.uplink : s_cfg.uplink == 1;
    s_mud.uplink = !atual;
    s_mud.uplink_muda = (s_mud.uplink != (s_cfg.uplink == 1));
    sistema_pedir_redesenho();
    return;
  }
  if (i == RC_LOC_PRECISAO) {
    if (s_cfg.precisao < 0) { cfg_aviso(tr("o canal principal do nó ainda não chegou")); return; }
    const int32_t atual = s_mud.precisao_muda ? s_mud.precisao : s_cfg.precisao;
    s_mud.precisao = opc_proximo(OPC_PRECISAO, N_OPC(OPC_PRECISAO), atual);
    s_mud.precisao_muda = (s_mud.precisao != s_cfg.precisao);
    sistema_pedir_redesenho();
    return;
  }

  const int c = campo_da_linha(i);
  if (c >= 0) {
    if (!s_cfg.conhecido[c]) { return; }
    int32_t novo = 0;
    const int32_t v = valor_atual(c);
    switch (i) {
      case RC_PAPEL:      novo = opc_proximo(OPC_PAPEL, N_OPC(OPC_PAPEL), v); break;
      case RC_SALTOS:     novo = opc_proximo(OPC_SALTOS, N_OPC(OPC_SALTOS), v == 0 ? 3 : v); break;
      case RC_TX:         novo = opc_proximo(OPC_TX, N_OPC(OPC_TX), v); break;
      case RC_POS_S:      novo = opc_proximo(OPC_POS_S, N_OPC(OPC_POS_S), v); break;
      case RC_NODEINFO_S: novo = opc_proximo(OPC_NODEINFO_S, N_OPC(OPC_NODEINFO_S), v); break;
      case RC_TELE_S:     novo = opc_proximo(OPC_TELE_S, N_OPC(OPC_TELE_S), v); break;
      case RC_TELA_S:     novo = opc_proximo(OPC_TELA_S, N_OPC(OPC_TELA_S), v); break;
      case RC_MQTT:
        novo = v ? 0 : 1;
        mesh_cfg_mqtt_ligar(&s_mud, &s_cfg, novo);   // the MQTT vs proxy rule belongs to the service
        break;
      case RC_POS_INTELIGENTE: novo = v ? 0 : 1; break;
      case RC_OK_MQTT:         novo = v ? 0 : 1; break;
      case RC_GPS_NO:
        // 2 = NOT_PRESENT: the firmware decides this by itself (the board has no
        // GPS); turning it on from here would not make one appear.
        if (v == 2) { cfg_aviso(tr("a placa do nó não tem GPS")); return; }
        novo = v == 1 ? 0 : 1;
        break;
      case RC_PROXY:
        novo = v ? 0 : 1;
        mesh_cfg_proxy_ligar(&s_mud, &s_cfg, novo);   // the MQTT vs proxy rule belongs to the service
        break;
      case RC_BT:
        // Turning off the node's Bluetooth with the Tab5 talking through it would cut
        // the conversation itself - and without it there is no way to turn it back on from here.
        if (v && tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH) { cfg_aviso(tr("o Tab5 está falando com o nó pelo Bluetooth")); return; }
        novo = v ? 0 : 1;
        break;
    }
    // Back to the value the node already has: no longer pending.
    s_mud.v[c] = novo;
    s_mud.muda[c] = (novo != s_cfg.v[c]);
    sistema_pedir_redesenho();
    return;
  }

  switch (i) {
    case RC_WIFI:
      s_redes_pg = true;
      s_redes_msg[0] = 0;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    case RC_LONGO: case RC_CURTO:
      s_editando = i;
      s_edit_txt[0] = 0;
      tecvirt_abrir(&s_kb_cfg, TECVIRT_ASCII);   // node names: ASCII only (what the node shows)
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    case RC_POS_FIXA:
      if (s_cfg.pos_fixa) { cfg_aviso(mesh_cfg_tirar_posicao_fixa() ? tr("posição fixa removida") : tr("não deu para remover")); }
      else                { cfg_aviso(mesh_cfg_posicao_fixa_do_gps() ? tr("posição do GPS enviada ao nó") : tr("GPS do Tab5 sem sinal")); }
      return;
    case RC_HORA:
      cfg_aviso(mesh_cfg_acertar_hora() ? tr("hora do nó acertada") : tr("o Tab5 ainda não sabe a hora"));
      return;
    case RC_REINICIAR:
      if (s_reiniciar_ms && (tab5_agora_ms() - s_reiniciar_ms) < 4000) {
        s_reiniciar_ms = 0;
        cfg_aviso(mesh_cfg_reiniciar() ? "" : tr("não deu para reiniciar"));
      } else {
        s_reiniciar_ms = tab5_agora_ms();
        sistema_pedir_redesenho();
      }
      return;
  }
}
