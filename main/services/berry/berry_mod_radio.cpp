// Berry modules 'espnow' (broadcast text) and 'mesh' (Meshtastic). See
// berry_apps.cpp (the engine) and berry_interno.hpp.

#include <stdint.h>
#include "idioma.h"
#include <stdio.h>
#include <string.h>
#include "tab5_hora.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "berry_interno.hpp"

// ---------------------------------------------------------------------------
// MODULE 'espnow' - text broadcast or direct, on the ESP-NOW channel
// ---------------------------------------------------------------------------
static int e_pronto(bvm* vm) { be_pushbool(vm, tab5_espnow_pronto()); be_return(vm); }
static int e_enviar(bvm* vm) { be_pushbool(vm, tab5_espnow_enviar(arg_txt(vm, 1))); be_return(vm); }

// enviar_para(mac, texto) - direct to a MAC ("AA:BB:CC:DD:EE:01"); false if the
// MAC is invalid or sending was refused.
static int e_enviar_para(bvm* vm)
{
  uint8_t mac[6];
  const bool ok = tab5_espnow_mac_de_texto(arg_txt(vm, 1), mac) && tab5_espnow_enviar_para(mac, arg_txt(vm, 2));
  be_pushbool(vm, ok);
  be_return(vm);
}

// enviar_todos(texto [, mac]) - on all channels (1 to 13); without mac, broadcast.
// Wi-Fi drops for a few seconds during the sweep and comes back by itself.
static int e_enviar_todos(bvm* vm)
{
  uint8_t mac[6];
  const bool direto = be_top(vm) >= 2;
  const bool ok = (!direto || tab5_espnow_mac_de_texto(arg_txt(vm, 2), mac)) &&
                  tab5_espnow_enviar_todos_canais(direto ? mac : nullptr, arg_txt(vm, 1));
  be_pushbool(vm, ok);
  be_return(vm);
}

static int e_n(bvm* vm)      { be_pushint(vm, tab5_espnow_n()); be_return(vm); }
static int e_total(bvm* vm)  { be_pushint(vm, (bint)tab5_espnow_total()); be_return(vm); }
static int e_canal(bvm* vm)  { be_pushint(vm, tab5_espnow_canal()); be_return(vm); }

// estado() - {codigo, texto}: 3 = on air; 2 = turning on; 1 = no link with the C6
// (Wi-Fi never turned on this boot); 4 = radio stopped; 0 = C6 without our
// firmware. 'texto' says what to do (empty when on air).
static int e_estado(bvm* vm)
{
  int est;
  if (!tab5_espnow_c6_compativel())  { est = tab5_wifi_c6_enlace() ? 0 : 1; }
  else if (!tab5_wifi_c6_enlace())   { est = 1; }
  else if (!tab5_wifi_radio_ativo()) { est = 4; }
  else                               { est = tab5_espnow_pronto() ? 3 : 2; }
  static const char* const TXT[] = {
    N_("radio C6 de fabrica: ESP-NOW pede atualizacao (opcional)"),
    N_("ligue o Wi-Fi em Ajustes (o C6 so tem energia com ele)"),
    N_("ligando o ESP-NOW..."), "",
    N_("radio parado - ligue o Wi-Fi ou fixe um canal em Ajustes"),
  };
  mapa_novo(vm);
  m_int(vm, "codigo", est);
  m_txt(vm, "texto", tr(TXT[est]));
  fim(vm);
  be_return(vm);
}

static int e_mac(bvm* vm)
{
  uint8_t mac[6];
  char t[18];
  tab5_espnow_meu_mac(mac);
  mac_txt(mac, t);
  be_pushstring(vm, t);
  be_return(vm);
}
static int e_vistos(bvm* vm)      { be_pushint(vm, tab5_espnow_vistos()); be_return(vm); }
// ultimo() - ms since the last one received, or -1 if nothing has arrived yet.
static int e_ultimo(bvm* vm)
{
  const uint32_t ms = tab5_espnow_ms_desde_rx();
  be_pushint(vm, ms == UINT32_MAX ? -1 : (bint)ms);
  be_return(vm);
}
// canal_fixo() - 0 = follows the router; fixar_canal(c) - 0, or 1 to 13.
static int e_canal_fixo(bvm* vm)  { be_pushint(vm, tab5_espnow_canal_fixo()); be_return(vm); }
static int e_fixar_canal(bvm* vm) { be_pushbool(vm, tab5_espnow_definir_canal((uint8_t)arg_int(vm, 1, 0))); be_return(vm); }
// janela(true) on opening the conversation: the service keeps the receiver on and
// what arrives does not become a notice on the bar. janela(false) on closing.
static int e_janela(bvm* vm)       { tab5_espnow_janela(be_top(vm) < 1 || be_tobool(vm, 1)); be_return_nil(vm); }
static int e_nao_lidas(bvm* vm)    { be_pushint(vm, tab5_espnow_nao_lidas()); be_return(vm); }
static int e_marcar_lidas(bvm* vm) { tab5_espnow_marcar_lidas(); be_return_nil(vm); }

// msg(i) - {texto, minha, quando, de, envio, espressif}: 0 = the oldest
// kept, n()-1 the latest. envio (mine): 1 sending, 2 went out over the antenna
// (broadcast has no confirmation of who received it), 3 failed.
static int e_msg(bvm* vm)
{
  espnow_msg_t m;
  if (!tab5_espnow_msg(arg_int(vm, 1, -1), &m)) { be_return_nil(vm); }
  char mac[18];
  mac_txt(m.mac, mac);
  mapa_novo(vm);
  m_txt(vm, "texto", m.texto);
  m_bool(vm, "minha", m.minha);
  m_int(vm, "quando", (int)m.quando);
  m_txt(vm, "de", mac);
  m_int(vm, "envio", m.envio);
  m_bool(vm, "espressif", m.espressif);
  fim(vm);
  be_return(vm);
}

// ---------------------------------------------------------------------------
// MODULE 'mesh' - reading, and only one write, with limits
// ---------------------------------------------------------------------------
// A script does not go chatting on a public channel: mesh.enviar only sends a
// DIRECT message to ONE node (never broadcast, never to itself), with Meshtastic's
// text limit, and at most one every 10 s across the whole system (from
// any app) - a script bug must not become a flood on the mesh.
static int h_estado(bvm* vm)
{
  mapa_novo(vm);
  m_bool(vm, "ativo", tab5_mesh_servico_ativo());
  m_bool(vm, "no_ar", tab5_mesh_no_ar());
  m_int(vm, "nos", tab5_mesh_num_nos());
  m_int(vm, "nao_lidas", tab5_mesh_nao_lidas());
  fim(vm);
  be_return(vm);
}

static int h_n_nos(bvm* vm)   { be_pushint(vm, tab5_mesh_num_nos()); be_return(vm); }
static int h_n_msgs(bvm* vm)  { be_pushint(vm, tab5_mesh_num_msgs()); be_return(vm); }
static int h_total(bvm* vm)   { be_pushint(vm, (bint)tab5_mesh_total_msgs()); be_return(vm); }
static int h_meu(bvm* vm)     { be_pushint(vm, (bint)tab5_mesh_meu_numero()); be_return(vm); }

// mesh.no(i): a node from the table (0 = ours, then those heard over the air), or nil.
static int h_no(bvm* vm)
{
  mesh_no_t n;
  if (!tab5_mesh_no(arg_int(vm, 1, -1), &n)) { be_return_nil(vm); }
  mapa_novo(vm);
  m_int(vm, "num", (int)n.num);
  m_txt(vm, "id", n.id);
  m_txt(vm, "nome", n.longo);
  m_txt(vm, "curto", n.curto);
  m_real(vm, "snr", n.snr);
  m_int(vm, "idade", n.idade_s);              // seconds since the last packet (-1 = never)
  m_int(vm, "bateria", n.bateria);            // 0-100; 101 = on mains
  m_int(vm, "saltos", n.saltos);
  m_bool(vm, "mqtt", n.via_mqtt);
  m_bool(vm, "favorito", n.favorito);
  if (n.tem_pos) { m_real(vm, "lat", n.lat_i / 1e7); m_real(vm, "lon", n.lon_i / 1e7); }
  fim(vm);
  be_return(vm);
}

// mesh.msg(i): one of the recent messages (0 = the newest), or nil.
static int h_msg(bvm* vm)
{
  mesh_msg_t m;
  if (!tab5_mesh_msg(arg_int(vm, 1, -1), &m)) { be_return_nil(vm); }
  char nome[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(m.de, nome, sizeof(nome));
  mapa_novo(vm);
  m_int(vm, "de", (int)m.de);
  m_txt(vm, "nome", nome);
  m_txt(vm, "texto", m.texto);
  m_int(vm, "quando", (int)m.quando);
  m_int(vm, "canal", m.canal);
  m_bool(vm, "direta", m.direta);
  m_real(vm, "snr", m.snr);
  m_bool(vm, "mqtt", m.via_mqtt);
  m_bool(vm, "minha", m.de == tab5_mesh_meu_numero());
  fim(vm);
  be_return(vm);
}

// The minimum interval between two mesh.enviar calls, across the whole system.
static const uint32_t ENVIO_INTERVALO_MS = 10000;

// mesh.enviar(numero, texto) - DIRECT message to node 'numero' (the 'num' from
// mesh.no(i)). true = it entered the service's send queue (progress, like
// any of our messages, appears in mesh.msg with 'minha'). false = refused:
// number 0, broadcast (0xFFFFFFFF) or our own node; text empty or longer than
// MESH_PAYLOAD_MAX; last send less than 10 s ago; or the service did not accept it.
// Direct uses channel 0, like the Mesh app.
static int h_enviar(bvm* vm)
{
  static bool     enviou = false;   // something has already gone out this boot
  static uint32_t ultimo_ms = 0;    // when (tab5_agora_ms)
  const uint32_t no = arg_u32(vm, 1, 0);   // Berry's -1 = 0xFFFFFFFF (see arg_u32)
  const char* texto = arg_txt(vm, 2);
  const size_t n = strlen(texto);
  const uint32_t agora = tab5_agora_ms();
  bool ok = no != 0 && no != 0xFFFFFFFFu && no != tab5_mesh_meu_numero() &&
            n > 0 && n <= MESH_PAYLOAD_MAX &&
            !(enviou && agora - ultimo_ms < ENVIO_INTERVALO_MS);
  if (ok) { ok = tab5_mesh_enviar(no, 0, texto); }
  if (ok) { enviou = true; ultimo_ms = agora; }
  be_pushbool(vm, ok);
  be_return(vm);
}

const membro_t BERRY_MOD_ESPNOW[] = {
  { "pronto", e_pronto }, { "enviar", e_enviar }, { "enviar_para", e_enviar_para },
  { "enviar_todos", e_enviar_todos }, { "n", e_n }, { "msg", e_msg },
  { "total", e_total }, { "canal", e_canal }, { "estado", e_estado }, { "mac", e_mac },
  { "vistos", e_vistos }, { "ultimo", e_ultimo }, { "canal_fixo", e_canal_fixo },
  { "fixar_canal", e_fixar_canal }, { "janela", e_janela }, { "nao_lidas", e_nao_lidas },
  { "marcar_lidas", e_marcar_lidas },
  { nullptr },
};

const membro_t BERRY_MOD_MESH[] = {
  { "estado", h_estado }, { "n_nos", h_n_nos }, { "no", h_no }, { "n_msgs", h_n_msgs },
  { "msg", h_msg }, { "total", h_total }, { "meu", h_meu }, { "enviar", h_enviar },
  { nullptr },
};
