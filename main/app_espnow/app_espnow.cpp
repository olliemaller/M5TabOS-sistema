// APP: ESP-NOW - the window for the services/tab5_espnow service.
//
// A thin information bar at the top (state, channel, MAC, who has been
// heard) and the conversation below - bubbles, tap to type, keyboard. The
// conversation is the system component (kernel/conversa): this file only says
// WHICH messages exist and HOW to send. The radio, the history on the SD card
// and the notification while the app is closed belong to the service.
//
// BROADCAST sending: every device on the same channel hears it (the Cardputer
// with the official ESP-NOW firmware, for example). Broadcast has no ACK - the
// green tick means "left through the antenna", not "someone received it".
#include "app.h"
#include "idioma.h"
#include "conversa.hpp"
#include "pintura.hpp"
#include "tab5_espnow.hpp"
#include "tab5_wifi.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_tela.hpp"
#include "ajustes.h"
#include <cstdio>
#include <cstring>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// In PSRAM: ~1 KB (draft, its copy for "changed", keyboard) that only the
// UI task touches, and need not live in internal RAM while the app is closed.
EXT_RAM_BSS_ATTR static conversa_t s_c;
static pintura_t  s_p;
static char       s_barra_txt[128];
static int        s_barra_estado = -1;

// 88 px, "ESP-NOW" at 3.6 and the pill at 3.2 (was 56/2.2, then 72/3.0 -
// too small to read at normal working distance).
static const int BARRA_A = 88;

// The CHANNEL SELECTOR, on the bar: each tap moves to the next one. 0 = the
// router's; 1, 6 and 11 are the non-overlapping channels at 2.4 GHz - and the
// defaults of most devices (a lone ESP32 sits on 1).
static const uint8_t CANAIS[] = { 0, 1, 6, 11 };
static int s_pilula_x0 = -1, s_pilula_x1 = -1;   // where it was drawn (area x)

// The CONVERSATION FONT SIZE: the "Aa" button on the bar steps to the next one
// (saved in settings). 1.0 is the size calibrated in Mesh; the rest are for
// reading from a distance with the device on the desk.
static const float LETRA[AJUSTES_ESPNOW_LETRA_MAX + 1] = { 1.0f, 1.2f, 1.4f, 1.65f };
static int s_aa_x0 = -1, s_aa_x1 = -1;

// Stable colour per device: the same MAC always gets the same label colour,
// and on a desk with three Cardputers you can tell who is who at a glance.
// These are DATA colours (they identify the device): they stay outside the theme.
static uint16_t cor_do_mac(const uint8_t mac[6])
{
  static const uint16_t CORES[] = { TFT_SKYBLUE, TFT_ORANGE, TFT_GREENYELLOW, TFT_PINK,
                                    TFT_GOLD, TFT_CYAN, TFT_VIOLET, TFT_SILVER };
  const unsigned h = (unsigned)mac[3] * 31u + (unsigned)mac[4] * 7u + (unsigned)mac[5];
  return CORES[h % (sizeof(CORES) / sizeof(CORES[0]))];
}

// ---- what the conversation component asks ----------------------------------
static int contar(void*) { return tab5_espnow_n(); }

static bool ler(int i, conversa_msg_t* out, void*)
{
  static espnow_msg_t m, ant;   // off the UI task's stack
  if (!tab5_espnow_msg(i, &m)) { return false; }
  snprintf(out->texto, sizeof(out->texto), "%s", m.texto);
  out->minha  = m.minha;
  out->quando = m.quando;
  out->envio  = m.envio;   // espnow_envio_t and conversa_envio_t have the same order
  out->nome[0] = 0;
  // Label only when the sender changes - a run from the same device does not
  // repeat the name on every bubble.
  if (!m.minha) {
    const bool mesmo = (i > 0) && tab5_espnow_msg(i - 1, &ant) && !ant.minha
                    && memcmp(ant.mac, m.mac, 6) == 0;
    if (!mesmo) {
      snprintf(out->nome, sizeof(out->nome), "%02X%02X", m.mac[4], m.mac[5]);
      out->cor_nome = cor_do_mac(m.mac);
    }
  }
  // The DIALECT of each received message, small underneath: with Cardputer, StickC and
  // Basic on the desk, it says who speaks the esp-now component format and who sends plain text.
  out->meta[0] = 0;
  if (!m.minha) {
    snprintf(out->meta, sizeof(out->meta), "%s", m.espressif ? tr("esp-now Espressif") : tr("texto puro"));
    out->cor_meta = m.espressif ? TEMA_DESTAQUE : TEMA_TEXTO_3;
  }
  return true;
}

static bool enviar(const char* texto, void*) { return tab5_espnow_enviar(texto); }

// ---- information bar --------------------------------------------------------
// 0 = our firmware not on the C6, 1 = C6 without link (WiFi never turned on this
// boot), 2 = starting, 3 = on air, 4 = radio stopped (WiFi off in Settings,
// or gave up out of router range) - the link stays up, the radio does not.
static int estado_agora(void)
{
  if (!tab5_espnow_c6_compativel()) {
    return tab5_wifi_c6_enlace() ? 0 : 1;   // without a link we cannot tell the firmware yet
  }
  if (!tab5_wifi_c6_enlace()) { return 1; }
  if (!tab5_wifi_radio_ativo()) { return 4; }
  return tab5_espnow_pronto() ? 3 : 2;
}

static const char* motivo(int est)
{
  switch (est) {
    case 0:  return tr("rádio C6 de fábrica: ESP-NOW pede atualização (opcional)");
    case 1:  return tr("ligue o Wi-Fi em Ajustes (o C6 só tem energia com ele)");
    case 2:  return tr("ligando o ESP-NOW...");
    case 4:  return tr("rádio parado - ligue o Wi-Fi ou fixe um canal em Ajustes");
    default: return "";
  }
}

static void desenhar_barra(lgfx::LovyanGFX& d, const area_t& a, int est)
{
  const uint8_t fixo = tab5_espnow_canal_fixo();
  char canal[32];
  // Fixed: the channel the C6 SAID it is on (the service checks) - if it
  // refused the change, the pill shows the requested and the real one, rather than lying.
  const uint8_t real = tab5_espnow_canal();
  // In portrait the bar is 720 px: the pill goes short. "fixed" vs "router" is still
  // told by its COLOUR (orange = fixed), just without the word.
  const bool pe = a.largura < a.altura;
  if (fixo && est == 3 && real && real != fixo) {
    if (pe) { snprintf(canal, sizeof(canal), "ch %u? (%u)", (unsigned)real, (unsigned)fixo); }
    else    { snprintf(canal, sizeof(canal), tr("canal %u?  fixo %u"), (unsigned)real, (unsigned)fixo); }
  } else if (fixo) { snprintf(canal, sizeof(canal), pe ? tr("ch %u") : tr("canal %u  fixo"), (unsigned)fixo); }
  else if (est == 3) { snprintf(canal, sizeof(canal), pe ? tr("ch %u") : tr("canal %u  roteador"), (unsigned)tab5_espnow_canal()); }
  else { snprintf(canal, sizeof(canal), pe ? tr("ch auto") : tr("canal: roteador")); }

  char txt[80];
  if (est == 3) {
    uint8_t mac[6];
    tab5_espnow_meu_mac(mac);
    const uint32_t ms = tab5_espnow_ms_desde_rx();
    char ultimo[24];
    if (ms == UINT32_MAX)  { snprintf(ultimo, sizeof(ultimo), tr("nada ouvido")); }
    else if (ms < 60000)   { snprintf(ultimo, sizeof(ultimo), tr("último %lus"), (unsigned long)(ms / 1000)); }
    else                   { snprintf(ultimo, sizeof(ultimo), tr("último %lumin"), (unsigned long)(ms / 60000)); }
    // THE MAC DOES NOT FIT IN PORTRAIT. After the title and the pill ~150 px are
    // left, and the whole line shrunk to the minimum still ran past the edge. The MAC
    // is the datum that changes least (and it is the Tab5's own): it goes, keeping
    // what says whether anyone is nearby.
    if (pe) {
      snprintf(txt, sizeof(txt), tr("%d ouvido(s)  %s"), tab5_espnow_vistos(), ultimo);
    } else {
      snprintf(txt, sizeof(txt), tr("%02X:%02X:%02X:%02X:%02X:%02X   %d ouvido(s)   %s"),
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tab5_espnow_vistos(), ultimo);
    }
  } else {
    snprintf(txt, sizeof(txt), "%s", est == 2 ? tr("ligando...") : est == 4 ? tr("rádio parado") : tr("fora do ar"));
  }

  char tudo[128];
  snprintf(tudo, sizeof(tudo), "%s|%u|%s", canal, (unsigned)ajustes_espnow_letra(), txt);
  const bool m1 = pintura_mudou_i(&s_p, &s_barra_estado, est);
  const bool m2 = pintura_mudou_s(&s_p, s_barra_txt, sizeof(s_barra_txt), tudo);
  if (!m1 && !m2) { return; }

  const uint16_t FUNDO = TEMA_SUPERFICIE;   // the bar is a panel over the screen background
  tab5_tela_limpar(a.x, a.y, a.largura, BARRA_A, FUNDO);
  const int cy = a.y + BARRA_A / 2;
  const uint16_t cor = (est == 3) ? TEMA_SUCESSO : (est == 2) ? TEMA_ALERTA : TEMA_TEXTO_3;
  d.fillCircle(a.x + 34, cy, 15, cor);

  d.setTextDatum(middle_left);
  tab5_texto(d, 3.6f);
  d.setTextColor(TEMA_TEXTO, FUNDO);
  d.drawString("ESP-NOW", a.x + 64, cy);
  int x = a.x + 64 + (int)d.textWidth("ESP-NOW") + 28;

  // Channel pill: it is a button (tap changes it), so it looks like a button.
  tab5_texto(d, 3.2f);
  const int pl = (int)d.textWidth(canal) + 44;
  const int pa = BARRA_A - 22;
  const uint16_t cor_p = fixo ? (uint16_t)TEMA_DESTAQUE : (uint16_t)TEMA_SUPERFICIE_ALTA;   // fixed channel = accent
  d.fillRoundRect(x, cy - pa / 2, pl, pa, pa / 2, cor_p);
  d.setTextColor(fixo ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO, cor_p);
  d.drawString(canal, x + 22, cy);
  s_pilula_x0 = x - a.x;
  s_pilula_x1 = x - a.x + pl;
  x += pl + 16;

  // "Aa": the conversation font size. The button's letter grows with the level
  // - the button itself shows what is in effect.
  const uint8_t nivel = ajustes_espnow_letra();
  const int al = pa + 10;
  const uint16_t cor_aa = TEMA_SUPERFICIE_ALTA;
  d.fillRoundRect(x, cy - pa / 2, al, pa, pa / 2, cor_aa);
  tab5_texto(d, 2.6f + 0.4f * nivel);
  d.setTextColor(TEMA_TEXTO, cor_aa);
  d.setTextDatum(middle_center);
  d.drawString("Aa", x + al / 2, cy);
  d.setTextDatum(middle_left);
  s_aa_x0 = x - a.x;
  s_aa_x1 = x - a.x + al;
  x += al + 24;

  // The rest (MAC, heard, last) at the LARGEST size that fits in what is left -
  // measured, not guessed: in portrait or with a long MAC, it shrinks by itself
  // rather than running past the edge.
  const int sobra = a.x + a.largura - 16 - x;
  float tam = 3.2f;
  tab5_texto(d, tam);
  while (tam > 1.8f && (int)d.textWidth(txt) > sobra) {
    tam -= 0.2f;
    tab5_texto(d, tam);
  }
  // At the smallest size and still not fitting: cut at the end, per character (UTF-8),
  // rather than running past the edge of the screen.
  tab5_cortar_ao_caber(d, txt, sobra);
  d.setTextColor(TEMA_TEXTO_2, FUNDO);
  d.drawString(txt, x, cy);
  d.drawFastHLine(a.x, a.y + BARRA_A - 1, a.largura, TEMA_LINHA);
  d.setTextDatum(top_left);
}

// ---- home screen icon -------------------------------------------------------
// In the style of the Espressif brand (owner of ESP-NOW, which has no logo of
// its own - the official page uses the company brand): its red, and in
// white the point of origin with three waves opening up and to the right -
// the signal leaving a device, with no router in between. Everything proportional
// to the side: the same drawing works at any grid size.
static void marca_espnow(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t vermelho = d.color565(231, 53, 44);   // Espressif red
  d.fillRoundRect(x, y, lado, lado, lado / 6, vermelho);

  const int cx = x + (lado * 28) / 100;
  const int cy = y + (lado * 72) / 100;
  int grosso = (lado * 7) / 100;
  if (grosso < 3) { grosso = 3; }

  d.fillCircle(cx, cy, (lado * 8) / 100, TFT_WHITE);
  // 270..360 degrees = the top-right quadrant (y grows downwards).
  static const int RAIO[3] = { 24, 40, 56 };   // % of the side, to the outer edge of the wave
  for (int k = 0; k < 3; ++k) {
    const int r1 = (lado * RAIO[k]) / 100;
    d.fillArc(cx, cy, r1 - grosso, r1, 270, 360, TFT_WHITE);
  }
}

// ---- life cycle -------------------------------------------------------------
static bool ao_abrir(void)
{
  s_barra_estado = -1;
  s_barra_txt[0] = 0;
  conversa_abrir(&s_c, &s_p, contar, ler, enviar, nullptr);
  conversa_escala(&s_c, LETRA[ajustes_espnow_letra()]);
  tab5_espnow_marcar_lidas();
  tab5_espnow_janela(true);    // conversation in front: the service asks for the receiver on
  return true;
}

static void ao_fechar(void) { tab5_espnow_janela(false); conversa_fechar(&s_c); }

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);
  const int est = estado_agora();
  desenhar_barra(d, a, est);
  const area_t r = { a.x, a.y + BARRA_A, a.largura, a.altura - BARRA_A };
  conversa_desenhar(&s_c, d, r, pintura_completa(&s_p), est == 3, motivo(est));
}

static void ao_aviso(uint32_t)
{
  // With the window open, what arrives is born already read.
  tab5_espnow_marcar_lidas();
  conversa_mudou(&s_c);
}

static void ao_tocar(int x, int y)
{
  // The channel pill steps to the next: router -> 1 -> 6 -> 11 -> router.
  if (y < BARRA_A && s_pilula_x0 >= 0 && x >= s_pilula_x0 && x < s_pilula_x1) {
    const uint8_t atual = tab5_espnow_canal_fixo();
    int k = 0;
    while (k < (int)sizeof(CANAIS) && CANAIS[k] != atual) { ++k; }
    tab5_espnow_definir_canal(CANAIS[(k + 1) % (int)sizeof(CANAIS)]);
    sistema_pedir_redesenho();
    return;
  }
  if (y < BARRA_A && s_aa_x0 >= 0 && x >= s_aa_x0 && x < s_aa_x1) {
    const uint8_t n = (uint8_t)((ajustes_espnow_letra() + 1) % (AJUSTES_ESPNOW_LETRA_MAX + 1));
    ajustes_definir_espnow_letra(n);
    conversa_escala(&s_c, LETRA[n]);   // re-measures the bubbles and requests a full pass
    return;
  }
  if (y < BARRA_A) { return; }
  conversa_tocar(&s_c, x, y);
}
static void ao_tocar_nivel(int x, int y, bool p)       { conversa_tocar_nivel(&s_c, x, y, p); }
static void ao_teclar(const char* t, uint8_t c, uint8_t m) { conversa_teclar(&s_c, t, c, m); }

extern const app_t APP_ESPNOW = {
  .nome  = "ESP-NOW",
  .cor   = TFT_CYAN,
  // The radio belongs to the WiFi service (the system setting keeps it). The IMU is
  // so the screen follows the device.
  .perfil = { .imu = true },
  .usa_sd = false,          // the history belongs to the service
  // Portrait or landscape following the device; with the keyboard attached, landscape
  // (same as Mesh). The conversation is the system component and already measures
  // itself by the width it gets - only the top bar needs to know about portrait.
  .orientacao = ORI_SEGUE_IMU,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,   // key lights under the finger; scrolling
  .ao_teclar = ao_teclar,             // physical keyboard on ExtPort1
  .periodo_ms = 1000,                 // the bar's "last Ns ago"
  .avisos = AVISO_ESPNOW,
  .ao_aviso = ao_aviso,
  .icone = marca_espnow,
  .fundo_tema = true,   // the conversation takes the background from sistema_cor_fundo()
};
