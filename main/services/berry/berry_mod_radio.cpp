// Modulos Berry 'espnow' (texto em difusao) e 'mesh' (Meshtastic). Ver
// berry_apps.cpp (o motor) e berry_interno.hpp.

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
// MODULO 'espnow' - texto em difusao ou direto, no canal do ESP-NOW
// ---------------------------------------------------------------------------
static int e_pronto(bvm* vm) { be_pushbool(vm, tab5_espnow_pronto()); be_return(vm); }
static int e_enviar(bvm* vm) { be_pushbool(vm, tab5_espnow_enviar(arg_txt(vm, 1))); be_return(vm); }

// enviar_para(mac, texto) - direto para um MAC ("AA:BB:CC:DD:EE:01"); false se o
// MAC e invalido ou o envio foi recusado.
static int e_enviar_para(bvm* vm)
{
  uint8_t mac[6];
  const bool ok = tab5_espnow_mac_de_texto(arg_txt(vm, 1), mac) && tab5_espnow_enviar_para(mac, arg_txt(vm, 2));
  be_pushbool(vm, ok);
  be_return(vm);
}

// enviar_todos(texto [, mac]) - em todos os canais (1 a 13); sem mac, difusao.
// O Wi-Fi cai por alguns segundos durante a varredura e volta sozinho.
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

// estado() - {codigo, texto}: 3 = no ar; 2 = ligando; 1 = sem enlace com o C6
// (Wi-Fi nunca ligado neste boot); 4 = radio parado; 0 = C6 sem o firmware
// nosso. 'texto' diz o que fazer (vazio quando esta no ar).
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
// ultimo() - ms desde a ultima recebida, ou -1 se nada chegou ainda.
static int e_ultimo(bvm* vm)
{
  const uint32_t ms = tab5_espnow_ms_desde_rx();
  be_pushint(vm, ms == UINT32_MAX ? -1 : (bint)ms);
  be_return(vm);
}
// canal_fixo() - 0 = segue o roteador; fixar_canal(c) - 0, ou 1 a 13.
static int e_canal_fixo(bvm* vm)  { be_pushint(vm, tab5_espnow_canal_fixo()); be_return(vm); }
static int e_fixar_canal(bvm* vm) { be_pushbool(vm, tab5_espnow_definir_canal((uint8_t)arg_int(vm, 1, 0))); be_return(vm); }
// janela(true) ao abrir a conversa: o servico mantem o receptor ligado e o
// que chega nao vira aviso na barra. janela(false) ao fechar.
static int e_janela(bvm* vm)       { tab5_espnow_janela(be_top(vm) < 1 || be_tobool(vm, 1)); be_return_nil(vm); }
static int e_nao_lidas(bvm* vm)    { be_pushint(vm, tab5_espnow_nao_lidas()); be_return(vm); }
static int e_marcar_lidas(bvm* vm) { tab5_espnow_marcar_lidas(); be_return_nil(vm); }

// msg(i) - {texto, minha, quando, de, envio, espressif}: 0 = a mais antiga
// guardada, n()-1 a ultima. envio (as minhas): 1 saindo, 2 saiu pela antena
// (difusao nao tem confirmacao de quem recebeu), 3 falhou.
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
// MODULO 'mesh' - leitura, e uma escrita so, com limites
// ---------------------------------------------------------------------------
// Um script nao sai falando num canal publico: mesh.enviar so manda mensagem
// DIRETA para UM no (nunca difusao, nunca para si mesmo), com o limite de
// texto do Meshtastic, e no maximo uma a cada 10 s no sistema inteiro (de
// qualquer app) - um erro de script nao pode virar enxurrada na malha.
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

// mesh.no(i): um no da tabela (0 = o nosso, depois os ouvidos pelo ar), ou nil.
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
  m_int(vm, "idade", n.idade_s);              // segundos desde o ultimo pacote (-1 = nunca)
  m_int(vm, "bateria", n.bateria);            // 0-100; 101 = na tomada
  m_int(vm, "saltos", n.saltos);
  m_bool(vm, "mqtt", n.via_mqtt);
  m_bool(vm, "favorito", n.favorito);
  if (n.tem_pos) { m_real(vm, "lat", n.lat_i / 1e7); m_real(vm, "lon", n.lon_i / 1e7); }
  fim(vm);
  be_return(vm);
}

// mesh.msg(i): uma das mensagens recentes (0 = a mais nova), ou nil.
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

// O intervalo minimo entre dois mesh.enviar, no sistema inteiro.
static const uint32_t ENVIO_INTERVALO_MS = 10000;

// mesh.enviar(numero, texto) - mensagem DIRETA para o no 'numero' (o 'num' de
// mesh.no(i)). true = entrou na fila de envio do servico (o andamento, como
// qualquer mensagem nossa, aparece em mesh.msg com 'minha'). false = recusou:
// numero 0, difusao (0xFFFFFFFF) ou o proprio no; texto vazio ou maior que
// MESH_PAYLOAD_MAX; ultimo envio ha menos de 10 s; ou o servico nao aceitou.
// Direta usa o canal 0, como o app Mesh.
static int h_enviar(bvm* vm)
{
  static bool     enviou = false;   // ja saiu algum neste boot
  static uint32_t ultimo_ms = 0;    // quando (tab5_agora_ms)
  const uint32_t no = arg_u32(vm, 1, 0);   // -1 do Berry = 0xFFFFFFFF (ver arg_u32)
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
