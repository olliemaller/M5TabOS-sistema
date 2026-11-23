// APP: Ajustes (Settings) - the network and radio pages: Wi-Fi, Bluetooth,
// ESP-NOW, Meshtastic (the PIN) and the mesh over the internet (MQTT servers),
// each with its drawing and touch handling. Split from app_config.cpp (the
// app); what the parts share is in app_config_interno.hpp.

#include "tab5_hora.hpp"
#include "app.h"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_wifi.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_ble.hpp"
#include "tab5_sinal.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_teclado.hpp"
#include "tab5_ui.hpp"
#include "pintura.hpp"
#include "lista.hpp"
#include "teclado_virtual.hpp"
#include "trabalho.hpp"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "app_config_interno.hpp"

static int       s_bt_n = 0;
static tab5_ble_visto_t s_bt[TAB5_BLE_VISTOS];
static char  s_pin_txt[8];

// ---- Wi-Fi -----------------------------------------------------------------
// Status, on/off, scan, and the LIST of networks around. Tapping a known
// network connects; a new one asks for the password (on-screen keyboard or
// the ExtPort1 one), stores it encrypted on the SD card and connects. Tapping
// the network in use twice forgets it.
static const int WF_ITEM_A = 92;
lista_t   s_l_wifi;
uint32_t  s_wf_ver = 0;
static int       s_wf_n = 0;
static tab5_wifi_visto_t s_wf[TAB5_WIFI_MAX_VISTAS];
bool      s_wf_senha = false;          // editing the password of s_wf_alvo
static char      s_wf_alvo[33];
char      s_wf_txt[65];
static char      s_wf_campo[70];          // what is painted in the password field
static char      s_wf_esquecer[33];           // 1st tap on the network in use
static uint32_t  s_wf_esquecer_ms = 0;

static int wf_y_lista(void) { return CONTEUDO_Y + 84 + (BTN_A + 20) * 4 + 44; }

static bool wf_em_uso(const char* ssid)
{
  return tab5_wifi_estado() == TAB5_WIFI_CONECTADO && strcmp(tab5_wifi_ssid(), ssid) == 0;
}

int wf_item_altura(int, void*) { return WF_ITEM_A; }
void wf_item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= s_wf_n) { return; }
  const tab5_wifi_visto_t& v = s_wf[i];
  d.fillRect(x0, y, largura, WF_ITEM_A, TEMA_FUNDO);
  const bool uso = wf_em_uso(v.ssid);
  // Signal as 4 bars, on the left.
  const int nb = v.rssi > -55 ? 4 : v.rssi > -65 ? 3 : v.rssi > -75 ? 2 : 1;
  for (int b = 0; b < 4; ++b) {
    const int h = 8 + b * 8;
    d.fillRect(x0 + MARGEM + b * 10, y + 56 - h, 7, h, b < nb ? (uso ? TEMA_SUCESSO : TEMA_TEXTO) : cor_linha(d));
  }
  const int tx = x0 + MARGEM + 56;
  tab5_escrever(d, v.ssid, tx, y + 12, 30, true, uso ? TEMA_SUCESSO : TEMA_TEXTO, TEMA_FUNDO);
  char linha[80], dist[12];
  tab5_sinal_distancia_txt(v.rssi, TAB5_SINAL_P1M_WIFI, dist, sizeof(dist));
  snprintf(linha, sizeof(linha), tr("%s%s%d dBm %s  ·  canal %u  ·  %s"), uso ? tr("conectada  ·  ") : "",
           (!uso && v.conhecida) ? tr("conhecida  ·  ") : "", (int)v.rssi, dist, (unsigned)v.canal,
           v.aberta ? tr("aberta") : tr("com senha"));
  tab5_escrever(d, linha, tx, y + 52, 22, false, cor_sub(d), TEMA_FUNDO);
  d.drawFastHLine(tx, y + WF_ITEM_A - 1, x0 + largura - tx, cor_linha(d));
}

static void wf_recarregar(void)
{
  const int n = tab5_wifi_n_vistas();
  int k = 0;
  for (int i = 0; i < n && k < TAB5_WIFI_MAX_VISTAS; ++i) { if (tab5_wifi_vista(i, &s_wf[k])) { ++k; } }
  s_wf_n = k;
  lista_itens_mudaram(&s_l_wifi, s_wf_n);
  if (deitado(sistema_area())) { sistema_pedir_redesenho_completo(); }   // the page changed height
}

void pag_wifi(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[160];
  est_wifi(sub, sizeof(sub));
  const bool con = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
  const uint32_t ver = tab5_wifi_redes_versao();
  if (ver != s_wf_ver) { s_wf_ver = ver; wf_recarregar(); }
  char det[48];
  const int r = tab5_wifi_rssi();
  if (con && r != TAB5_WIFI_RSSI_DESCONHECIDO) { snprintf(det, sizeof(det), tr("%s  ·  %d dBm"), tab5_wifi_ip(), r); }
  else if (con)                                { snprintf(det, sizeof(det), "%s", tab5_wifi_ip()); }
  else                                         { snprintf(det, sizeof(det), tr("%d rede(s) conhecida(s)"), tab5_wifi_n_conhecidas()); }
  // The typed text does NOT go into the signature: each letter used to repaint
  // the whole page and the keyboard with it. Now a letter repaints only the field.
  snprintf(assin, sizeof(assin), "%s|%s|%d|%d|%d|%s|%d|%d", sub, det, (int)ajustes_wifi(), (int)tab5_wifi_procurando(),
           s_wf_n, s_aviso, (int)s_wf_senha, (int)ajustes_wifi_cochilo() | ((int)ajustes_capa_internet() << 1));
  const bool mudou = pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) || tudo;
  if (s_wf_senha) {
    const int y_campo = CONTEUDO_Y + 36;
    tecvirt_posicionar(&s_cfg_kb, a);
    if (mudou) {
      lista_esconder(&s_l_wifi);
      conteudo_limpo(d, a);
      char rot[64];
      snprintf(rot, sizeof(rot), tr("Senha de \"%s\""), s_wf_alvo);
      tab5_escrever(d, rot, a.x + MARGEM, a.y + CONTEUDO_Y, 24, false, cor_sub(d), TEMA_FUNDO);
      paragrafo(d, a.x + MARGEM, a.y + y_campo + 92, a.largura - 2 * MARGEM,
                tr("Toque ENVIAR para guardar e conectar. A senha fica cifrada no cartão. X cancela."));
    }
    char campo[70];
    snprintf(campo, sizeof(campo), "%s_", s_wf_txt);
    if (pintura_mudou_s(&s_cfg_p, s_wf_campo, sizeof(s_wf_campo), campo) || mudou) {
      d.fillRoundRect(a.x + MARGEM, a.y + y_campo, a.largura - 2 * MARGEM, 72, 12, TEMA_SUPERFICIE_ALTA);
      d.setFont(tab5_fonte_suave(false, 34));
      d.setTextSize(1);
      d.setTextDatum(middle_left);
      d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE_ALTA);
      d.drawString(campo, a.x + MARGEM + 16, a.y + y_campo + 36);
      d.setTextDatum(top_left);
      tab5_fonte_corpo(d);
    }
    tecvirt_pintar(&s_cfg_kb, d, mudou);
    return;
  }
  if (mudou) {
    conteudo_limpo(d, a);
    int y = CONTEUDO_Y;
    tab5_escrever(d, tr("Estado"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
    tab5_escrever(d, sub, a.x + MARGEM, a.y + y + 28, 32, false, con ? TEMA_SUCESSO : TEMA_TEXTO, TEMA_FUNDO);
    tab5_escrever(d, det, a.x + a.largura - MARGEM, a.y + y + 34, 22, false, cor_sub(d), TEMA_FUNDO, top_right);
    y += 84;
    y = botao_largo(d, a, y, ajustes_wifi() ? tr("DESLIGAR O WI-FI") : tr("LIGAR O WI-FI"),
                    ajustes_wifi() ? TEMA_ALERTA : TEMA_SUCESSO);
    y = botao_largo(d, a, y, tab5_wifi_procurando() ? tr("PROCURANDO...") : tr("PROCURAR REDES"),
                    tab5_wifi_procurando() ? TEMA_TEXTO_3 : TEMA_DESTAQUE);
    // Idle power saving (services/tab5_wifi_cochilo): with the screen off, Wi-Fi
    // goes down and comes back when the screen wakes or for 1 min every 15.
    y = botao_largo(d, a, y, ajustes_wifi_cochilo() ? tr("ECONOMIA MÁXIMA: DESLIGA EM REPOUSO") : tr("EM REPOUSO: CONECTADO ECONÔMICO"),
                    ajustes_wifi_cochilo() ? TEMA_ALERTA : TEMA_SUCESSO);
    // What goes to the internet without the user asking: the music cover art (services/tab5_musica).
    y = botao_largo(d, a, y, ajustes_capa_internet() ? tr("CAPAS DE MÚSICA: BUSCA NA INTERNET") : tr("CAPAS DE MÚSICA: SÓ DO CARTÃO"),
                    ajustes_capa_internet() ? TEMA_DESTAQUE : TEMA_TEXTO_3);
    tab5_escrever(d, s_aviso[0] ? s_aviso : (s_wf_n ? tr("Toque numa rede para conectar") : tr("Toque PROCURAR REDES")), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  }
  lista_desenhar(&s_l_wifi, d, janela_da_lista(a, wf_y_lista(), s_wf_n, WF_ITEM_A), TEMA_FUNDO, mudou);
}

// Writing to the SD card and connecting: on the worker (the card is slow), with
// the password in a private copy that the job wipes when it finishes.
// PICKING A NETWORK LEAVES THE FIXED CHANNEL. With ESP-NOW on a fixed channel,
// Wi-Fi is detached from the router; whoever taps a network wants the router
// back. Without releasing the fixed channel here, Wi-Fi connected but ESP-NOW
// kept assuming the channel was the fixed one - and the saved setting dropped
// the router on the next boot.
static void conectar_rede(const char* ssid)
{
  if (tab5_espnow_canal_fixo()) { tab5_espnow_definir_canal(0); }
  tab5_wifi_conectar(ssid);
}

typedef struct { char ssid[33]; char senha[65]; bool esquecer; } wf_pedido_t;

static void wf_trabalho(void* arg)
{
  wf_pedido_t* p = (wf_pedido_t*)arg;
  // The write mounts the SD card by itself (and, with the card lent to the
  // computer, it goes into the log - the new network is not lost).
  if (p->esquecer) { tab5_wifi_esquecer(p->ssid); }
  else             { tab5_wifi_gravar_credenciais(p->ssid, p->senha[0] ? p->senha : "-"); }
  if (!p->esquecer) { conectar_rede(p->ssid); }
  memset(p, 0, sizeof(*p));
  free(p);
}

static void wf_pedir(const char* ssid, const char* senha, bool esquecer)
{
  wf_pedido_t* p = (wf_pedido_t*)calloc(1, sizeof(wf_pedido_t));
  if (!p) { return; }
  snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
  snprintf(p->senha, sizeof(p->senha), "%s", senha ? senha : "");
  p->esquecer = esquecer;
  if (!sistema_trabalho_solto(wf_trabalho, p)) { memset(p, 0, sizeof(*p)); free(p); }
}

// Connecting requires Wi-Fi on: turns it on (and saves the preference) if off.
static void wf_ligar_se_preciso(void)
{
  sistema_wifi(true);
}

static void wf_fechar_senha(void)
{
  s_wf_senha = false;
  memset(s_wf_txt, 0, sizeof(s_wf_txt));
  tecvirt_fechar(&s_cfg_kb);
  sistema_pedir_redesenho_completo();
}

static void wf_enviar_senha(void)
{
  if (!s_wf_txt[0]) { return; }
  wf_ligar_se_preciso();
  wf_pedir(s_wf_alvo, s_wf_txt, false);
  snprintf(s_aviso, sizeof(s_aviso), tr("conectando a %s..."), s_wf_alvo);
  wf_fechar_senha();
}

void wf_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR) { wf_enviar_senha(); return; }
  if (ac == TECVIRT_FECHAR) { wf_fechar_senha(); return; }
  // Passwords are ASCII (as on the physical keyboard): accents from the symbols page are not accepted.
  tecvirt_editar(s_wf_txt, sizeof(s_wf_txt), ac);
  sistema_pedir_redesenho();
}

void toque_wifi(int tx, int ty)
{
  const area_t a = area_pagina();
  if (s_wf_senha) {
    if (tecvirt_contem(&s_cfg_kb, a.x + tx, a.y + ty)) {
      const int ac = tecvirt_clique(&s_cfg_kb, a.x + tx, a.y + ty);
      if (ac) { wf_tecla(ac); }
    }
    return;
  }
  const int l = a.largura - 2 * MARGEM;
  int y = CONTEUDO_Y + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    sistema_wifi(!ajustes_wifi());
    s_aviso[0] = 0;
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    if (!tab5_wifi_procurando()) { tab5_wifi_procurar(); }
    s_aviso[0] = 0;
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    ajustes_definir_wifi_cochilo(!ajustes_wifi_cochilo());
    snprintf(s_aviso, sizeof(s_aviso), ajustes_wifi_cochilo()
             ? tr("Tela apagada: Wi-Fi desliga; volta ao acender (sem Mesh pela internet)")
             : tr("Tela apagada: segue conectado, em modo econômico"));
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    ajustes_definir_capa_internet(!ajustes_capa_internet());
    snprintf(s_aviso, sizeof(s_aviso), ajustes_capa_internet()
             ? tr("Sem capa no cartão: título e artista vão à busca do iTunes")
             : tr("Capas só do arquivo e da pasta: nada sai do aparelho"));
    sistema_pedir_redesenho();
    return;
  }
  int dy = 0;
  const int i = lista_item_tocado(&s_l_wifi, tx + s_dx_pag, ty + s_dy_pag - s_rol_pag.atual, &dy);
  if (i < 0 || i >= s_wf_n) { return; }
  const tab5_wifi_visto_t& v = s_wf[i];
  const uint32_t agora = tab5_agora_ms();
  if (wf_em_uso(v.ssid)) {
    // In use: the first tap warns, the second (within 4 s) forgets the network.
    if (strcmp(s_wf_esquecer, v.ssid) == 0 && agora - s_wf_esquecer_ms < 4000) {
      wf_pedir(v.ssid, nullptr, true);
      snprintf(s_aviso, sizeof(s_aviso), tr("%s esquecida"), v.ssid);
      s_wf_esquecer[0] = 0;
    } else {
      snprintf(s_wf_esquecer, sizeof(s_wf_esquecer), "%s", v.ssid);
      s_wf_esquecer_ms = agora;
      snprintf(s_aviso, sizeof(s_aviso), tr("Conectada. Toque de novo para esquecer"));
    }
  } else if (v.conhecida || v.aberta) {
    wf_ligar_se_preciso();
    if (v.conhecida) { conectar_rede(v.ssid); }
    else             { wf_pedir(v.ssid, nullptr, false); }   // open: save without a password and connect
    snprintf(s_aviso, sizeof(s_aviso), tr("conectando a %s..."), v.ssid);
  } else {
    snprintf(s_wf_alvo, sizeof(s_wf_alvo), "%s", v.ssid);
    memset(s_wf_txt, 0, sizeof(s_wf_txt));
    s_wf_senha = true;
    tecvirt_abrir(&s_cfg_kb, TECVIRT_SENHA);
    sistema_pedir_redesenho_completo();
    return;
  }
  sistema_pedir_redesenho();
}

void teclar_wifi(const char* txt, uint8_t codigo)
{
  if (!s_wf_senha) { return; }
  if (codigo == TAB5_TECLA_ENTER)  { wf_enviar_senha(); return; }
  if (codigo == TAB5_TECLA_APAGAR) { wf_tecla(TECVIRT_APAGAR); return; }
  if (codigo == TAB5_TECLA_ESC)    { wf_fechar_senha(); return; }
  // Passwords are ASCII: the letter composed by the dead key goes back to being the two keys.
  char cru[8];
  tab5_teclado_ascii(txt, cru, sizeof(cru));
  for (const char* c = cru; *c; ++c) { if ((unsigned char)*c >= 0x20) { wf_tecla((unsigned char)*c); } }
}

// ---- Bluetooth -------------------------------------------------------------
static const int BT_ITEM_A = 92;
static int bt_lista_y0(void) { return CONTEUDO_Y + 84 + 84 + BTN_A + 20 + BTN_A + 20 + 44; }

int bt_item_altura(int, void*) { return BT_ITEM_A; }
void bt_item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= s_bt_n) { return; }
  const tab5_ble_visto_t& v = s_bt[i];
  d.fillRect(x0, y, largura, BT_ITEM_A, TEMA_FUNDO);
  char end[18], linha[80], dist[12];
  tab5_ble_endereco_txt(v.addr, end);
  // Trackers in orange: that is what you look for when you want to know "is
  // there a tag near me?".
  const uint16_t cor = v.meshtastic ? TEMA_SUCESSO
                     : v.classe == TAB5_BLE_RASTREADOR ? TEMA_ALERTA : TEMA_TEXTO;
  char titulo[48];
  if (v.nome[0])       { snprintf(titulo, sizeof(titulo), "%s", v.nome); }
  else if (v.marca[0]) { snprintf(titulo, sizeof(titulo), "%s %s", v.classe == TAB5_BLE_RASTREADOR ? tr("Rastreador") : tr("Beacon"), v.marca); }
  else                 { snprintf(titulo, sizeof(titulo), tr("(sem nome)")); }
  tab5_escrever(d, titulo, x0 + MARGEM, y + 12, 30, true, cor, TEMA_FUNDO);
  tab5_sinal_distancia_txt(v.rssi, v.p1m ? v.p1m : TAB5_SINAL_P1M_BLE, dist, sizeof(dist));
  const char* tipo = v.meshtastic ? "  ·  Meshtastic" : "";
  char marca[24] = "";
  if (!v.meshtastic && v.marca[0] && v.nome[0]) { snprintf(marca, sizeof(marca), "  ·  %s", v.marca); }
  snprintf(linha, sizeof(linha), "%s  ·  %d dBm %s%s%s", end, (int)v.rssi, dist, tipo, marca);
  tab5_escrever(d, linha, x0 + MARGEM, y + 52, 22, false, cor_sub(d), TEMA_FUNDO);
  if (v.meshtastic) { chevron(d, x0 + largura - MARGEM - 12, y + BT_ITEM_A / 2, cor_sub(d)); }
  d.drawFastHLine(x0 + MARGEM, y + BT_ITEM_A - 1, largura - MARGEM, cor_linha(d));
}

static void bt_recarregar(void)
{
  // Meshtastic first, then by signal: what matters sits at the top.
  const int n = tab5_ble_n_vistos();
  int k = 0;
  for (int i = 0; i < n && k < TAB5_BLE_VISTOS; ++i) { if (tab5_ble_visto(i, &s_bt[k])) { ++k; } }
  for (int i = 1; i < k; ++i) {
    tab5_ble_visto_t t = s_bt[i];
    int j = i - 1;
    // Meshtastic, then trackers, then the rest - each group by signal.
    auto grupo = [](const tab5_ble_visto_t& x) {
      return x.meshtastic ? 0 : x.classe == TAB5_BLE_RASTREADOR ? 1 : 2;
    };
    auto antes = [grupo](const tab5_ble_visto_t& x, const tab5_ble_visto_t& y) {
      if (grupo(x) != grupo(y)) { return grupo(x) < grupo(y); }
      return x.rssi > y.rssi;
    };
    while (j >= 0 && antes(t, s_bt[j])) { s_bt[j + 1] = s_bt[j]; --j; }
    s_bt[j + 1] = t;
  }
  s_bt_n = k;
  lista_itens_mudaram(&s_l_bt, s_bt_n);
  if (deitado(sistema_area())) { sistema_pedir_redesenho_completo(); }
}

void pag_bt(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[160];
  est_bt(sub, sizeof(sub));
  const bool pronto = tab5_ble_pronto();
  const bool varrendo = tab5_ble_varrendo();
  const uint32_t ver = tab5_ble_versao();
  if (ver != s_bt_ver) { s_bt_ver = ver; bt_recarregar(); }
  const bool via_bt = ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH;
  const char* enlace = via_bt ? mesh_ble_descricao()
                     : ajustes_mesh_via() == AJUSTES_MESH_INTERNET ? tr("Mesh pela internet") : tr("Mesh pelo cabo Grove");
  snprintf(assin, sizeof(assin), "%s|%d|%d|%d|%s|%s", sub, (int)ajustes_bluetooth(), (int)varrendo, s_bt_n, s_aviso, enlace);
  const bool mudou = pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) || tudo;
  if (mudou) {
    conteudo_limpo(d, a);
    int y = CONTEUDO_Y;
    y = info(d, a, y, tr("Estado"), sub, pronto ? TEMA_SUCESSO : TEMA_TEXTO);
    y = info(d, a, y, tr("Conexão do Mesh"), enlace, mesh_ble_conectado() ? TEMA_SUCESSO : TEMA_TEXTO);
    const bool ligado = tab5_ble_estado() != TAB5_BLE_DESLIGADO;
    const bool descendo = tab5_ble_estado() == TAB5_BLE_DESLIGANDO;
    y = botao_largo(d, a, y, descendo ? tr("DESLIGANDO...") : ligado ? tr("DESLIGAR O BLUETOOTH") : tr("LIGAR O BLUETOOTH"),
                    descendo ? TEMA_TEXTO_3 : ligado ? TEMA_ALERTA : TEMA_SUCESSO);
    y = botao_largo(d, a, y, varrendo ? tr("PROCURANDO...") : tr("PROCURAR APARELHOS"),
                    pronto && !varrendo ? TEMA_DESTAQUE : TEMA_TEXTO_3);
    tab5_escrever(d, s_aviso[0] ? s_aviso : (s_bt_n ? tr("Toque num Meshtastic (verde) para conectar o Mesh") : tr("Nenhum aparelho visto ainda")), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  }
  lista_desenhar(&s_l_bt, d, janela_da_lista(a, bt_lista_y0(), s_bt_n, BT_ITEM_A), TEMA_FUNDO, mudou);
}

void toque_bt(int tx, int ty)
{
  const int l = area_pagina().largura - 2 * MARGEM;
  int y = CONTEUDO_Y + 84 + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    // Going down: the button waits for the stack to finish (repeated taps from
    // someone who thought it did not register do not pile up requests).
    if (tab5_ble_estado() == TAB5_BLE_DESLIGANDO) { return; }
    sistema_bluetooth(tab5_ble_estado() == TAB5_BLE_DESLIGADO);
    s_aviso[0] = 0;
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    if (tab5_ble_varrer(6000)) { s_aviso[0] = 0; }
    sistema_pedir_redesenho();
    return;
  }
  int dy = 0;
  const int i = lista_item_tocado(&s_l_bt, tx + s_dx_pag, ty + s_dy_pag - s_rol_pag.atual, &dy);
  if (i < 0 || i >= s_bt_n) { return; }
  char end[18];
  tab5_ble_endereco_txt(s_bt[i].addr, end);
  const char* nome = s_bt[i].nome[0] ? s_bt[i].nome : end;
  // Every tap responds: a device the system does not know how to use says so,
  // instead of the tap seeming lost.
  if (!s_bt[i].meshtastic) {
    char dist[12];
    tab5_sinal_distancia_txt(s_bt[i].rssi, s_bt[i].p1m ? s_bt[i].p1m : TAB5_SINAL_P1M_BLE, dist, sizeof(dist));
    if (s_bt[i].marca[0]) {
      snprintf(s_aviso, sizeof(s_aviso), tr("%s (%s) a %s - só leitura"), nome, s_bt[i].marca, dist);
    } else {
      snprintf(s_aviso, sizeof(s_aviso), tr("%s a %s: sem uso ainda (só nós Meshtastic)"), nome, dist);
    }
    ESP_LOGI("ajustes", "bluetooth: %s (%s) is not Meshtastic - nothing to do", nome, end);
    sistema_pedir_redesenho();
    return;
  }
  // Meshtastic: it becomes the Mesh node, and Mesh switches to Bluetooth -
  // tapping is the request to connect, and the "Conexão" row follows what happens.
  ajustes_definir_mesh_ble_endereco(end);
  // Connecting turns Bluetooth on (and it stays saved) and restarts Mesh over it.
  sistema_mesh_via(AJUSTES_MESH_BLUETOOTH);
  snprintf(s_aviso, sizeof(s_aviso), tr("conectando a %s..."), nome);
  ESP_LOGI("ajustes", "bluetooth: connect Mesh to %s (%s)", nome, end);
  sistema_pedir_redesenho();
}

// ---- ESP-NOW ---------------------------------------------------------------
void pag_espnow(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[160], val[24];
  est_espnow(sub, sizeof(sub));
  const uint8_t f = tab5_espnow_canal_fixo();
  snprintf(assin, sizeof(assin), "%s|%u|%u", sub, (unsigned)f, (unsigned)tab5_espnow_canal());
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  y = info(d, a, y, tr("Canal agora"), sub);
  y = botao_largo(d, a, y, tr("USAR O CANAL DO ROTEADOR"), f ? TEMA_DESTAQUE : TEMA_TEXTO_3);
  tab5_escrever(d, tr("Canal fixo"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34;
  if (f) { snprintf(val, sizeof(val), "%u", (unsigned)f); } else { snprintf(val, sizeof(val), "-"); }
  menos_mais(d, a, y, val);
  y += BTN_A + 24;
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("O rádio fica em um canal por vez. No canal do roteador o Wi-Fi e o ESP-NOW "
            "funcionam juntos, mas os outros aparelhos precisam estar nesse canal. "
            "Canal fixo solta o roteador. A escolha fica salva."));
}
void toque_espnow(int tx, int ty)
{
  int y = CONTEUDO_Y + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, area_pagina().largura - 2 * MARGEM, BTN_A)) {
    tab5_espnow_definir_canal(0);
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20 + 34;
  const int s = menos_mais_toque(tx, ty, y);
  if (s) {
    int c = tab5_espnow_canal_fixo();
    c = (c == 0) ? (s > 0 ? 1 : 13) : c + s;
    if (c < 1) { c = 13; } if (c > 13) { c = 1; }
    tab5_espnow_definir_canal((uint8_t)c);
    sistema_pedir_redesenho();
  }
}

// ---- Meshtastic ------------------------------------------------------------
static int mesh_y_via(void)    { return CONTEUDO_Y + 30; }
static int mesh_y_pin(void)    { return mesh_y_via() + BTN_A + 24 + 84 + 84; }
// The GPS position for the mesh moved out of here: it belongs to the Location
// group of the Mesh app (Configure node), next to what the NODE does with it -
// two places for the same thing let one contradict the other.
static int mesh_y_net(void)    { return mesh_y_pin() + 34 + BTN_A + 24; }   // internet block (MQTT)

static bool s_pin_para_no = false;   // the editor opened because the node is asking for the PIN right now
static char s_pin_pintado[16];       // the number that is on the screen

// The PIN number, on its own: clears only its strip (up to the button) and writes.
static void pin_valor(lgfx::LovyanGFX& d, const area_t& a, bool forcar)
{
  char pin[16];
  if (s_pin_editando) { snprintf(pin, sizeof(pin), "%s_", s_pin_txt); }
  else                { snprintf(pin, sizeof(pin), "%06lu", (unsigned long)ajustes_mesh_pin()); }
  if (!pintura_mudou_s(&s_cfg_p, s_pin_pintado, sizeof(s_pin_pintado), pin) && !forcar) { return; }
  const int y = mesh_y_pin() + 34;
  if (!forcar) { d.fillRect(a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM - 260 - 8, BTN_A, TEMA_FUNDO); }
  tab5_escrever(d, pin, a.x + MARGEM, a.y + y + BTN_A / 2, 40, true, s_pin_editando ? TEMA_ALERTA : TEMA_TEXTO, TEMA_FUNDO, middle_left);
}

void pag_mesh(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  // The node asked for the PIN (random mode): the keyboard opens by itself.
  if (mesh_ble_esperando_pin() && !s_pin_editando) {
    s_pin_editando = true;
    s_pin_para_no = true;
    s_pin_txt[0] = 0;
    tecvirt_abrir(&s_cfg_kb, TECVIRT_NUMERO);
    tudo = true;
  }
  char assin[320], salvo[18];
  ajustes_mesh_ble_endereco(salvo);
  snprintf(assin, sizeof(assin), "%d|%s|%s|%lu|%u|%d|%s|%d|%s", (int)ajustes_mesh_via(), tab5_mesh_enlace_descricao(), salvo,
           (unsigned long)ajustes_mesh_pin(), 0u, (int)s_pin_editando, "",
           (int)ajustes_mesh_internet(), tab5_mesh_internet_estado());
  // The signature memory (s_assin) holds 159 characters: whatever changes
  // after that (the states, which come at the end) would not repaint. So the
  // HASH of the whole signature goes in (FNV-1a), which fits and changes with
  // any character.
  uint32_t h = 2166136261u;
  for (const char* c = assin; *c; ++c) { h = (h ^ (uint8_t)*c) * 16777619u; }
  snprintf(assin, sizeof(assin), "mesh|%08lx", (unsigned long)h);
  // The typed PIN stays OUT of the signature: a letter repaints only the number.
  if (s_pin_editando) { tecvirt_posicionar(&s_cfg_kb, a); }
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) {
    if (s_pin_editando) { pin_valor(d, a, false); tecvirt_pintar(&s_cfg_kb, d, false); }
    return;
  }
  conteudo_limpo(d, a);
  const int l3 = (a.largura - 2 * MARGEM - 32) / 3;

  // THREE PATHS. Grove wins by itself when the C6L is found on the cable; after
  // that the choice here holds until the next detection (see ajustes.h).
  tab5_escrever(d, tr("Falar com a malha por (o Grove tem prioridade)"), a.x + MARGEM, a.y + CONTEUDO_Y, 22, false, cor_sub(d), TEMA_FUNDO);
  int y = mesh_y_via();
  static const char* const ROTULO[3] = { N_("CABO GROVE"), N_("BLUETOOTH"), N_("INTERNET") };
  for (int k = 0; k < 3; ++k) {
    const bool sel = ajustes_mesh_via() == k;
    tab5_botao(d, a.x + MARGEM + k * (l3 + 16), a.y + y, l3, BTN_A, tr(ROTULO[k]),
               sel ? TEMA_SUCESSO : TEMA_SUPERFICIE_ALTA, sel ? TEMA_FUNDO : TEMA_TEXTO_2);
  }
  y += BTN_A + 24;
  y = info(d, a, y, tr("Enlace"), tr(tab5_mesh_enlace_descricao()),
           tab5_mesh_no_ar() ? TEMA_SUCESSO : TEMA_TEXTO);
  y = info(d, a, y, tr("Nó Bluetooth"), salvo[0] ? salvo : tr("o primeiro que aparecer"));

  // PIN
  tab5_escrever(d, tr("PIN do pareamento Bluetooth"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34;
  pin_valor(d, a, true);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A,
             s_pin_editando ? tr("CANCELAR") : tr("ALTERAR"), s_pin_editando ? TEMA_ALERTA : TEMA_DESTAQUE);
  y += BTN_A + 24;

  if (s_pin_editando) {
    paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
              s_pin_para_no ? tr("O nó está pedindo o PIN agora: digite o que ele mostra e toque ENVIAR (30 s).")
                            : tr("Digite o PIN fixo do nó (123456 nos sem tela) e toque ENVIAR."));
    tecvirt_pintar(&s_cfg_kb, d, true);
    return;
  }

  // Internet (MQTT over Wi-Fi): only runs without the node; Grove has priority.
  const bool net = ajustes_mesh_internet();
  tab5_escrever(d, tr("Internet também quando faltar o nó (MQTT pelo Wi-Fi)"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34;
  tab5_escrever(d, net ? tr("Ligada") : tr("Desligada"), a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, net ? TEMA_SUCESSO : TEMA_TEXTO, TEMA_FUNDO, middle_left);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A, net ? tr("DESLIGAR") : tr("LIGAR"),
             net ? TEMA_ALERTA : TEMA_DESTAQUE);
  y += BTN_A + 12;
  {
    char est[160];
    snprintf(est, sizeof(est), tr("%s. Servidor: %s (Ajustes > Malha pela internet)."),
             tr(tab5_mesh_internet_estado()), mesh_mqtt_servidor_nome(ajustes_mqtt_servidor()));
    paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, est);
  }
  y += 100;
  if (salvo[0]) { y = botao_largo(d, a, y, tr("ESQUECER O NÓ BLUETOOTH"), TEMA_SUPERFICIE_ALTA); }
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("Com o nó no Bluetooth o cabo Grove fica livre para o GPS. O que o nó faz "
            "(localização, internet do Tab5 para ele, rádio): app Mesh, página Nó."));
}

static void aplicar_pin(void)
{
  if (s_pin_txt[0]) {
    const uint32_t pin = (uint32_t)atol(s_pin_txt);
    if (s_pin_para_no) {
      // PIN REQUESTED NOW BY THE NODE (the one shown on its screen): used only for
      // this pairing. It is not saved - the node generates another next time, and an
      // old saved number would only make the next pairing fail.
      if (mesh_ble_esperando_pin()) { mesh_ble_digitar_pin(pin); }
    } else {
      // FIXED PIN, edited by the user (ALTERAR/change): it is saved and tried
      // first on subsequent connections.
      ajustes_definir_mesh_pin(pin);
      ajustes_definir_mesh_pin_na_tela(false);
      mesh_ble_tentar_de_novo();
    }
  }
  s_pin_para_no = false;
  s_pin_editando = false;
  tecvirt_fechar(&s_cfg_kb);
  sistema_pedir_redesenho_completo();
}

void pin_tecla(int ac)
{
  const size_t n = strlen(s_pin_txt);
  if (ac >= '0' && ac <= '9' && n < 6) { s_pin_txt[n] = (char)ac; s_pin_txt[n + 1] = 0; }
  else if (ac == TECVIRT_APAGAR && n) { s_pin_txt[n - 1] = 0; }
  else if (ac == TECVIRT_ENVIAR) { aplicar_pin(); return; }
  else if (ac == TECVIRT_FECHAR) { s_pin_editando = false; tecvirt_fechar(&s_cfg_kb); sistema_pedir_redesenho_completo(); return; }
  sistema_pedir_redesenho();
}

void toque_mesh(int tx, int ty)
{
  const area_t a = area_pagina();
  if (s_pin_editando && tecvirt_contem(&s_cfg_kb, a.x + tx, a.y + ty)) {
    const int ac = tecvirt_clique(&s_cfg_kb, a.x + tx, a.y + ty);
    if (ac) { pin_tecla(ac); }
    return;
  }
  const int l3 = (a.largura - 2 * MARGEM - 32) / 3;
  int y = mesh_y_via();
  int via = -1;
  for (int k = 0; k < 3; ++k) { if (tab5_dentro(tx, ty, MARGEM + k * (l3 + 16), y, l3, BTN_A)) { via = k; } }
  if (via >= 0 && via != ajustes_mesh_via()) {
    sistema_mesh_via((uint8_t)via);
    sistema_pedir_redesenho();
    return;
  }
  y = mesh_y_pin() + 34;
  if (tab5_dentro(tx, ty, a.largura - MARGEM - 260, y, 260, BTN_A)) {
    s_pin_editando = !s_pin_editando;
    s_pin_txt[0] = 0;
    if (s_pin_editando) { tecvirt_abrir(&s_cfg_kb, TECVIRT_NUMERO); } else { tecvirt_fechar(&s_cfg_kb); }
    sistema_pedir_redesenho_completo();
    return;
  }
  if (s_pin_editando) { return; }
  y = mesh_y_net() + 34;
  if (tab5_dentro(tx, ty, a.largura - MARGEM - 260, y, 260, BTN_A)) {
    ajustes_definir_mesh_internet(!ajustes_mesh_internet());
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 12 + 100;
  char salvo[18];
  ajustes_mesh_ble_endereco(salvo);
  if (salvo[0] && tab5_dentro(tx, ty, MARGEM, y, a.largura - 2 * MARGEM, BTN_A)) {
    ajustes_definir_mesh_ble_endereco("");
    if (ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH) { sistema_mesh_reiniciar(); }
    sistema_pedir_redesenho();
  }
}

void teclar_mesh(const char* txt, uint8_t codigo)
{
  if (!s_pin_editando) { return; }
  const size_t n = strlen(s_pin_txt);
  if (codigo == TAB5_TECLA_ENTER) { aplicar_pin(); return; }
  if (codigo == TAB5_TECLA_APAGAR) { if (n) { s_pin_txt[n - 1] = 0; } sistema_pedir_redesenho(); return; }
  if (codigo == TAB5_TECLA_ESC) { s_pin_editando = false; tecvirt_fechar(&s_cfg_kb); sistema_pedir_redesenho_completo(); return; }
  if (txt && txt[0] >= '0' && txt[0] <= '9' && n < 6) { s_pin_txt[n] = txt[0]; s_pin_txt[n + 1] = 0; sistema_pedir_redesenho(); }
}

// ---- Mesh over the internet (MQTT servers) -----------------------------------
// Two screens: the LIST (automatic + the user's servers, each with EDITAR
// (edit), and + ADICIONAR (add)) and the EDITOR for a server (template, five
// fields - each edited on the keyboard - SALVAR E USAR (save and use), APAGAR
// (delete)). The list belongs to the service (mesh_mqtt_*); here we only draw
// and handle touch. Choosing restarts the Mesh service: the MQTT client
// reconnects to the new one.
bool  s_mq_ed      = false;     // in the editor
static int   s_mq_i       = 0;         // the server in the editor: 1..n, 0 = new
static int   s_mq_modelo  = 0;         // the template applied (only on a new one)
int   s_mq_campo   = -1;        // field being edited (0..4), -1 = none
char  s_mq_txt[64];
mesh_mqtt_servidor_t s_mq_p;    // the copy being edited
static uint32_t s_mq_apagar_ms = 0;    // 1st tap on APAGAR: confirm within 4 s
static const char* const MQ_ROTULO[5] = {
  N_("Nome (como aparece na lista)"), N_("Endereço (host ou host:porta)"), N_("Usuário"), N_("Senha"),
  N_("Raiz dos tópicos (ex.: msh/EU_868)"),
};
static const int MQ_EDITAR_L = 170;    // the EDITAR button at the end of each server

static char* mq_campo(int k, size_t* tam) { return mesh_mqtt_campo(&s_mq_p, k, tam); }

static int mq_y_lista(void) { return CONTEUDO_Y + 110; }
static const int MQ_PASSO = BTN_A + 12;

// Editor geometry, in one place: drawing and touch read the same maths.
static int mq_y_campos(void) { return CONTEUDO_Y + 40 + (s_mq_i == 0 ? BTN_A + 20 : 0); }
static int mq_y_botoes(void) { return mq_y_campos() + 5 * 84 + 12; }

static void mq_aplicar(uint8_t escolha)
{
  mesh_mqtt_usar_servidor(escolha);
  sistema_pedir_redesenho_completo();
}

static void mq_tela(void) { s_assin[0] = 0; sistema_pedir_redesenho_completo(); }

static void mq_abrir_editor(int i)
{
  s_mq_i = i;
  s_mq_modelo = 0;
  if (!mesh_mqtt_para_editar(i, &s_mq_p)) { return; }
  s_mq_ed = true;
  s_mq_apagar_ms = 0;
  mq_tela();
}

static void mq_fechar_editor(void)
{
  memset(&s_mq_p, 0, sizeof(s_mq_p));   // the typed password does not stay in RAM
  s_mq_ed = false;
  mq_tela();
}

static void mq_fechar_campo(bool guardar)
{
  if (guardar && s_mq_campo >= 0) {
    size_t tam;
    char* c = mq_campo(s_mq_campo, &tam);
    snprintf(c, tam, "%s", s_mq_txt);
  }
  memset(s_mq_txt, 0, sizeof(s_mq_txt));
  s_mq_campo = -1;
  tecvirt_fechar(&s_cfg_kb);
  mq_tela();
}

void mq_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR) { mq_fechar_campo(true); return; }
  if (ac == TECVIRT_FECHAR) { mq_fechar_campo(false); return; }
  size_t tam;
  mq_campo(s_mq_campo, &tam);
  // The name accepts accents (it is display-only); the rest goes to the server.
  tecvirt_editar(s_mq_txt, tam < sizeof(s_mq_txt) ? tam : sizeof(s_mq_txt), ac);
  sistema_pedir_redesenho();
}

void pag_mqtt(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (s_mq_apagar_ms && tab5_agora_ms() - s_mq_apagar_ms >= 4000) { s_mq_apagar_ms = 0; }   // gave up
  char assin[128];
  snprintf(assin, sizeof(assin), "mq|%d|%d|%d|%d|%d|%d|%d|%s", (int)ajustes_mqtt_servidor(), (int)s_mq_ed, s_mq_i,
           s_mq_campo, s_mq_modelo, (int)(s_mq_apagar_ms != 0), mesh_mqtt_n_servidores(), tab5_mesh_internet_estado());
  const bool mudou = pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) || tudo;

  // EDITING A FIELD: only it and the keyboard (the same way as the Wi-Fi password).
  if (s_mq_campo >= 0) {
    const int y_campo = CONTEUDO_Y + 36;
    tecvirt_posicionar(&s_cfg_kb, a);
    if (mudou) {
      conteudo_limpo(d, a);
      tab5_escrever(d, tr(MQ_ROTULO[s_mq_campo]), a.x + MARGEM, a.y + CONTEUDO_Y, 24, false, cor_sub(d), TEMA_FUNDO);
      paragrafo(d, a.x + MARGEM, a.y + y_campo + 92, a.largura - 2 * MARGEM, tr("ENVIAR guarda o campo. X cancela."));
    }
    char campo[72];
    snprintf(campo, sizeof(campo), "%s_", s_mq_txt);
    if (pintura_mudou_s(&s_cfg_p, s_wf_campo, sizeof(s_wf_campo), campo) || mudou) {
      d.fillRoundRect(a.x + MARGEM, a.y + y_campo, a.largura - 2 * MARGEM, 72, 12, TEMA_SUPERFICIE_ALTA);
      d.setFont(tab5_fonte_suave(false, 30));
      d.setTextSize(1);
      d.setTextDatum(middle_left);
      d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE_ALTA);
      d.drawString(campo, a.x + MARGEM + 16, a.y + y_campo + 36);
      d.setTextDatum(top_left);
      tab5_fonte_corpo(d);
    }
    tecvirt_pintar(&s_cfg_kb, d, mudou);
    return;
  }
  if (!mudou) { return; }
  conteudo_limpo(d, a);
  const int l = a.largura - 2 * MARGEM;

  if (s_mq_ed) {
    tab5_escrever(d, s_mq_i == 0 ? tr("Novo servidor - toque num campo para editar") : tr("Editar servidor - toque num campo"), a.x + MARGEM, a.y + CONTEUDO_Y, 22, false, cor_sub(d), TEMA_FUNDO);
    if (s_mq_i == 0) {
      char m[64];
      snprintf(m, sizeof(m), tr("MODELO: %s  >"), mesh_mqtt_modelo(s_mq_modelo)->nome);
      botao_largo(d, a, CONTEUDO_Y + 40, m, TEMA_SUPERFICIE_ALTA);
    }
    int y = mq_y_campos();
    for (int k = 0; k < 5; ++k) {
      size_t tam;
      const char* v = mq_campo(k, &tam);
      char mostra[64];
      if (k == 3 && v[0]) { snprintf(mostra, sizeof(mostra), "%.*s", (int)strlen(v), "********************************"); }
      else                { snprintf(mostra, sizeof(mostra), "%s", v[0] ? v : tr("(vazio)")); }
      y = info(d, a, y, tr(MQ_ROTULO[k]), mostra, v[0] ? TEMA_TEXTO : cor_sub(d));
    }
    y = mq_y_botoes();
    y = botao_largo(d, a, y, tr("SALVAR E USAR"), s_mq_p.srv[0] ? TEMA_SUCESSO : TEMA_TEXTO_3);
    if (s_mq_i > 0) {
      y = botao_largo(d, a, y, s_mq_apagar_ms ? tr("TOQUE DE NOVO PARA APAGAR") : tr("APAGAR ESTE SERVIDOR"), TEMA_PERIGO);
    }
    botao_largo(d, a, y, tr("VOLTAR À LISTA"), TEMA_SUPERFICIE_ALTA);
    return;
  }

  paragrafo(d, a.x + MARGEM, a.y + CONTEUDO_Y, l,
            tr("Sem um nó Meshtastic, o Tab5 conversa com a malha pela internet, por um servidor MQTT. "
            "Toque para usar; EDITAR muda ou apaga. Automático: o que o nó ensinou, ou o brasileiro."));
  int y = mq_y_lista();
  const int n = mesh_mqtt_n_servidores();
  const uint8_t esc = ajustes_mqtt_servidor();
  for (int i = 0; i < n; ++i, y += MQ_PASSO) {
    mesh_mqtt_servidor_t s;
    if (!mesh_mqtt_servidor(i, &s)) { continue; }
    const bool sel = (esc == i);
    const int bl = i ? l - MQ_EDITAR_L - 12 : l;
    tab5_botao(d, a.x + MARGEM, a.y + y, bl, BTN_A, s.nome,
               sel ? TEMA_SUCESSO : TEMA_SUPERFICIE_ALTA, sel ? TEMA_FUNDO : TEMA_TEXTO_2);
    if (i) {
      tab5_botao(d, a.x + MARGEM + bl + 12, a.y + y, MQ_EDITAR_L, BTN_A, tr("EDITAR"),
                 TEMA_SUPERFICIE_ALTA, TEMA_TEXTO_2);
    }
    memset(&s, 0, sizeof(s));
  }
  if (mesh_mqtt_pode_adicionar()) {
    tab5_botao(d, a.x + MARGEM, a.y + y, l, BTN_A, tr("+ ADICIONAR SERVIDOR"), TEMA_DESTAQUE, TEMA_FUNDO);
    y += MQ_PASSO;
  }
  paragrafo(d, a.x + MARGEM, a.y + y + 8, l, tr(tab5_mesh_internet_estado()));
}

void toque_mqtt(int tx, int ty)
{
  const area_t a = area_pagina();
  if (s_mq_campo >= 0) {
    if (tecvirt_contem(&s_cfg_kb, a.x + tx, a.y + ty)) {
      const int ac = tecvirt_clique(&s_cfg_kb, a.x + tx, a.y + ty);
      if (ac) { mq_tecla(ac); }
    }
    return;
  }
  const int l = a.largura - 2 * MARGEM;
  if (s_mq_ed) {
    // TEMPLATE (only on a new one): switches to the next and fills in the fields.
    if (s_mq_i == 0 && tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + 40, l, BTN_A)) {
      s_mq_modelo = mesh_mqtt_proximo_modelo(s_mq_modelo, &s_mq_p);
      mq_tela();
      return;
    }
    int y = mq_y_campos();
    for (int k = 0; k < 5; ++k, y += 84) {
      if (tab5_dentro(tx, ty, MARGEM, y, l, 76)) {
        size_t tam;
        snprintf(s_mq_txt, sizeof(s_mq_txt), "%s", mq_campo(k, &tam));
        s_mq_campo = k;
        // name: free text; password: password; server, user and root: ASCII.
        tecvirt_abrir(&s_cfg_kb, k == 0 ? TECVIRT_TEXTO : k == 3 ? TECVIRT_SENHA : TECVIRT_ASCII);
        mq_tela();
        return;
      }
    }
    y = mq_y_botoes();
    if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {   // SALVAR E USAR (save and use)
      // Saves, switches to it and reconnects (the service).
      if (!mesh_mqtt_salvar_e_usar(s_mq_i, &s_mq_p)) { return; }   // no address (or list full): the button is already greyed out
      mq_fechar_editor();
      return;
    }
    y += BTN_A + 20;
    if (s_mq_i > 0) {
      if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {   // APAGAR (delete), with confirmation
        if (s_mq_apagar_ms && tab5_agora_ms() - s_mq_apagar_ms < 4000) {
          mesh_mqtt_apagar_e_reconectar(s_mq_i);   // it was the one in use: back to automatic
          mq_fechar_editor();
        } else {
          s_mq_apagar_ms = tab5_agora_ms();
          mq_tela();
        }
        return;
      }
      y += BTN_A + 20;
    }
    if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) { mq_fechar_editor(); }   // VOLTAR (back)
    return;
  }
  const int n = mesh_mqtt_n_servidores();
  int y = mq_y_lista();
  for (int i = 0; i < n; ++i, y += MQ_PASSO) {
    if (!tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) { continue; }
    const int bl = i ? l - MQ_EDITAR_L - 12 : l;
    if (i && tx >= MARGEM + bl) { mq_abrir_editor(i); return; }   // EDITAR (edit) (and the gap before it)
    if (i != ajustes_mqtt_servidor()) { mq_aplicar((uint8_t)i); }
    return;
  }
  if (mesh_mqtt_pode_adicionar() && tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) { mq_abrir_editor(0); }
}

void teclar_mqtt(const char* txt, uint8_t codigo)
{
  if (s_mq_campo < 0) { return; }
  if (codigo == TAB5_TECLA_ENTER) { mq_fechar_campo(true); return; }
  if (codigo == TAB5_TECLA_ESC)   { mq_fechar_campo(false); return; }
  const size_t n = strlen(s_mq_txt);
  if (codigo == TAB5_TECLA_APAGAR) { if (n) { s_mq_txt[n - 1] = 0; } sistema_pedir_redesenho(); return; }
  if (txt && txt[0] && !txt[1] && n + 1 < sizeof(s_mq_txt)) { s_mq_txt[n] = txt[0]; s_mq_txt[n + 1] = 0; sistema_pedir_redesenho(); }
}
