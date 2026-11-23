// APP: Ajustes (Settings) - the remaining pages: Date and time (time zone), PC
// access, Language, Screen, Keyboard, Grove, About and USB (flash drive), each
// with its drawing and touch handling. Split from app_config.cpp (the app);
// what the parts share is in app_config_interno.hpp.

#include "tab5_hora.hpp"
#include "tab5_fuso.hpp"
#include "app.h"
#include "tab5_power.hpp"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_wifi.hpp"
#include "tab5_teclado.hpp"
#include "tab5_ui.hpp"
#include "pintura.hpp"
#include "tab5_pendrive.hpp"
#include "tab5_web.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include "app_config_interno.hpp"

static const int PASSO_TELA_S = 5;

// The stamp of the whole build (main/carimbo.cmake), and not this file's
// __DATE__ - which only changed when THIS file was recompiled.
extern "C" const char* const m5tabos_compilado;

// ---- Date and time (time zone) ---------------------------------------------------
// Layout shared by the drawing and the touch: region row, then manual offset row.
static const int HORA_Y_REGIAO = CONTEUDO_Y + 84 + 84;   // label of the region row
static const int HORA_Y_MANUAL = HORA_Y_REGIAO + 34 + BTN_A + 56;
static const int HORA_Y_12H    = HORA_Y_MANUAL + 34 + BTN_A + 24;   // 12/24-hour switch

void pag_hora(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char agora[48], fuso[16], efetivo[40], assin[160];
  snprintf(agora, sizeof(agora), "%s", tr("relógio sem acerto"));
  fuso_texto(ajustes_fuso_min(), fuso, sizeof(fuso));
  const uint32_t e = tab5_hora_epoch();
  tab5_fuso_texto(e ? e : (uint32_t)time(nullptr), efetivo, sizeof(efetivo));
  const int zona = ajustes_fuso_zona();
  if (e) {
    struct tm t;
    tab5_hora_local(e, &t);
    char hm[8];
    tab5_hora_hhmm(&t, hm, sizeof(hm));
    snprintf(agora, sizeof(agora), "%02d/%02d/%04d  %s", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, hm);
  }
  snprintf(assin, sizeof(assin), "hora|%s|%s|%s|%d|%s|%d", agora, fuso, tab5_hora_fonte(), zona, efetivo, ajustes_relogio_12h());
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  y = info(d, a, y, tr("Agora (hora local)"), agora);
  y = info(d, a, y, tr("Acertado por"), tab5_hora_fonte());
  // Region: offset AND daylight-saving rule (services/tab5_fuso).
  tab5_escrever(d, tr("Região (horário de verão automático)"), a.x + MARGEM, a.y + HORA_Y_REGIAO, 22, false, cor_sub(d), TEMA_FUNDO);
  menos_mais(d, a, HORA_Y_REGIAO + 34, zona == TAB5_FUSO_MANUAL ? fuso : efetivo);
  tab5_escrever(d, tab5_fuso_nome(zona), a.x + MARGEM, a.y + HORA_Y_REGIAO + 34 + BTN_A + 10, 24, false,
                zona == TAB5_FUSO_MANUAL ? cor_sub(d) : TEMA_DESTAQUE, TEMA_FUNDO);
  // Manual offset: any tap here goes back to the fixed offset.
  tab5_escrever(d, tr("Fuso manual (passos de 30 min)"), a.x + MARGEM, a.y + HORA_Y_MANUAL, 22, false, cor_sub(d), TEMA_FUNDO);
  menos_mais(d, a, HORA_Y_MANUAL + 34, fuso);
  tab5_botao(d, a.x + MARGEM, a.y + HORA_Y_12H, a.largura - 2 * MARGEM, BTN_A,
             ajustes_relogio_12h() ? tr("Relógio de 12 horas (am/pm) - toque para 24 h")
                                   : tr("Relógio de 24 horas - toque para 12 h (am/pm)"),
             TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  fim(a.y + HORA_Y_12H + BTN_A);
  y = HORA_Y_12H + BTN_A + 20;
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("Escolha a sua região e o horário de verão muda sozinho; o fuso manual é um "
            "deslocamento fixo. Só a hora mostrada muda: o relógio interno, os registros e a malha continuam em UTC."));
}

void toque_hora(int tx, int ty)
{
  const int sr = menos_mais_toque(tx, ty, HORA_Y_REGIAO + 34);
  if (sr) {
    const int n = tab5_fuso_n();
    const int z = ((int)ajustes_fuso_zona() + sr + n) % n;
    ajustes_definir_fuso_zona((uint8_t)z);
    if (z != TAB5_FUSO_MANUAL) { ajustes_definir_fuso_min((int16_t)tab5_fuso_padrao_min(z)); }
    sistema_pedir_redesenho();
    return;
  }
  if (tab5_dentro(tx, ty, MARGEM, HORA_Y_12H, area_pagina().largura - 2 * MARGEM, BTN_A)) {
    ajustes_definir_relogio_12h(!ajustes_relogio_12h());
    sistema_pedir_redesenho();
    return;
  }
  const int s = menos_mais_toque(tx, ty, HORA_Y_MANUAL + 34);
  if (s) {
    ajustes_definir_fuso_zona(TAB5_FUSO_MANUAL);
    ajustes_definir_fuso_min((int16_t)(ajustes_fuso_min() + s * 30));
    sistema_pedir_redesenho();
  }
}

// ---- PC access -------------------------------------------------------------
// THE SYSTEM WEB SERVER in one place (services/tab5_web): the address and
// password, each app that has a page on the PC (the catalogue, app_t.no_pc)
// with the button to open or close its part, and TURN EVERYTHING OFF - without
// going app by app. Each app's "No PC" panels remain: this is the same server
// seen from above.
static const int PC_PASSO = BTN_A + 16;

// Where the app list starts (the drawing and the touch use the same maths).
static int pc_y_apps(bool ligado) { return CONTEUDO_Y + (ligado ? 2 * 84 : 0) + 40; }

void pag_pc(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  const char* cat[SISTEMA_APPS_MAX];
  const int nc = tab5_web_catalogo(cat, SISTEMA_APPS_MAX);
  const bool ligado = tab5_web_ligado(), wifi = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
  char url[64] = "", senha[8] = "", quem[48] = "", assin[160];
  tab5_web_endereco(nullptr, url, sizeof(url));
  tab5_web_senha(senha, sizeof(senha));
  tab5_web_ultimo(quem, sizeof(quem), nullptr);
  int k = snprintf(assin, sizeof(assin), "pc|%d|%d|%s|%s|%s", ligado, wifi, url, senha, quem);
  for (int i = 0; i < nc && k < (int)sizeof(assin) - 2; ++i) { k += snprintf(assin + k, sizeof(assin) - k, "|%d", tab5_web_aberto(cat[i])); }
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  if (ligado) {
    y = info(d, a, y, tr("No navegador do PC (mesma rede Wi-Fi)"), url, TEMA_DESTAQUE);
    y = info(d, a, y, tr("Senha (qualquer usuário)"), senha, TEMA_SUCESSO);
  }
  tab5_escrever(d, wifi ? tr("Apps no PC - toque para abrir ou fechar") : tr("Conecte o Wi-Fi para usar pelo PC"),
                a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y = pc_y_apps(ligado);
  for (int i = 0; i < nc; ++i, y += PC_PASSO) {
    const bool aberto = tab5_web_aberto(cat[i]);
    char t[64];
    snprintf(t, sizeof(t), "%s  -  %s", tr(cat[i]), aberto ? tr("aberto no PC") : tr("fechado"));
    tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, t,
               aberto ? TEMA_SUCESSO : TEMA_SUPERFICIE_ALTA, aberto ? TEMA_FUNDO : TEMA_TEXTO_2);
    fim(a.y + y + BTN_A);
  }
  if (ligado) { y = botao_largo(d, a, y + 8, tr("Desligar tudo"), TEMA_ALERTA); }
  char nota[160];
  if (ligado && quem[0]) { snprintf(nota, sizeof(nota), tr("Último acesso de %s. Cada app fecha sozinho após 10 min sem uso."), quem); }
  else { snprintf(nota, sizeof(nota), "%s", tr("Só liga quando você pede, só na rede local e com senha. Cada app fecha sozinho após 10 min sem uso.")); }
  paragrafo(d, a.x + MARGEM, a.y + y + 8, a.largura - 2 * MARGEM, nota);
}

void toque_pc(int tx, int ty)
{
  const char* cat[SISTEMA_APPS_MAX];
  const int nc = tab5_web_catalogo(cat, SISTEMA_APPS_MAX);
  const bool ligado = tab5_web_ligado();
  const int l = area_pagina().largura - 2 * MARGEM;
  int y = pc_y_apps(ligado);
  for (int i = 0; i < nc; ++i, y += PC_PASSO) {
    if (!tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) { continue; }
    if (tab5_web_aberto(cat[i])) { tab5_web_fechar(cat[i]); }
    else if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { sistema_recado(tr("Conecte o Wi-Fi primeiro")); }
    else if (!tab5_web_abrir_app(cat[i])) { sistema_recado(tr("O servidor não ligou")); }
    sistema_pedir_redesenho();
    return;
  }
  if (ligado && tab5_dentro(tx, ty, MARGEM, y + 8, l, BTN_A)) {
    tab5_web_desligar();
    sistema_recado(tr("Acesso pelo PC desligado"));
    sistema_pedir_redesenho();
  }
}

// ---- Language --------------------------------------------------------------
// One row per language, each with its name IN THAT LANGUAGE. Takes effect at
// once: the page (and the status bar) repaint fully in the new language.
static const int IDIOMA_PASSO = BTN_A + 16;

void pag_idioma(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char assin[16];
  snprintf(assin, sizeof(assin), "idioma|%d", (int)idioma_atual());
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  for (int i = 0; i < idioma_n(); ++i, y += IDIOMA_PASSO) {
    const bool sel = (idioma_atual() == i);
    tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, idioma_nome(i),
               sel ? TEMA_SUCESSO : TEMA_SUPERFICIE_ALTA, sel ? TEMA_FUNDO : TEMA_TEXTO_2);
  }
  paragrafo(d, a.x + MARGEM, a.y + y + 8, a.largura - 2 * MARGEM,
            tr("A tradução chega aos apps aos poucos: o que ainda não foi traduzido "
               "continua em português."));
}

void toque_idioma(int tx, int ty)
{
  for (int i = 0; i < idioma_n(); ++i) {
    if (!tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + i * IDIOMA_PASSO, area_pagina().largura - 2 * MARGEM, BTN_A)) {
      continue;
    }
    idioma_definir(i);
    sistema_pedir_redesenho_completo();   // the title and VOLTAR (back) change too
    return;
  }
}

// ---- Screen ----------------------------------------------------------------
void pag_tela(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char assin[64], v1[16], v2[16];
  snprintf(v1, sizeof(v1), "%u s", (unsigned)ajustes_tela_s());
  // The clock IN USE, not the saved one: on the TDDI screen it stays at the
  // factory value (tab5_painel in hal/tab5_power.hpp) and the saved one does not apply.
  snprintf(v2, sizeof(v2), "%d MHz", tab5_power_relogio_dpi());
  snprintf(assin, sizeof(assin), "%s|%s|%d|%d", v1, v2, (int)ajustes_bloqueio(), (int)ajustes_parede());
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  tab5_escrever(d, tr("Apagar a tela depois de, sem toque"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34; menos_mais(d, a, y, v1); y += BTN_A + 12;
  char ajuda[64];
  snprintf(ajuda, sizeof(ajuda), tr("De %d a %d segundos."), AJUSTES_TELA_MIN_S, AJUSTES_TELA_MAX_S);
  y = paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, ajuda) - a.y + 24;
  // Lock: fixed heights, the touch handler (toque_tela) repeats this maths.
  const bool bq = ajustes_bloqueio();
  tab5_escrever(d, tr("Tela de bloqueio ao ligar e ao acordar"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34;
  tab5_escrever(d, bq ? tr("Ligada") : tr("Desligada"), a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, bq ? TEMA_SUCESSO : TEMA_TEXTO, TEMA_FUNDO, middle_left);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A, bq ? tr("DESLIGAR") : tr("LIGAR"),
             bq ? TEMA_ALERTA : TEMA_DESTAQUE);
  y += BTN_A + 24;
  // Home wallpaper: TROCAR (change) cycles through the options.
  static const char* const PAREDE[] = { N_("Sorteio"), N_("Fios"), N_("Ondas"), N_("Teia"), N_("Raios"), N_("Círculos"), N_("Nenhum") };
  const uint8_t pp = ajustes_parede();
  tab5_escrever(d, tr("Papel de parede do início"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34;
  tab5_escrever(d, tr(PAREDE[pp]), a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, pp == AJUSTES_PAREDE_NENHUM ? TEMA_TEXTO : TEMA_SUCESSO, TEMA_FUNDO, middle_left);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A, tr("TROCAR"), TEMA_DESTAQUE);
  y += BTN_A + 24;
  tab5_escrever(d, tr("Relógio do painel"), a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  y += 34; menos_mais(d, a, y, v2); y += BTN_A + 12;
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("Menor economiza bateria (menos leitura da memória pelo painel), mas reduz "
            "os quadros por segundo. 60 MHz é o padrão."));
}
void toque_tela(int tx, int ty)
{
  int y = CONTEUDO_Y + 34;
  int s = menos_mais_toque(tx, ty, y);
  if (s) { ajustes_definir_tela_s((uint8_t)(ajustes_tela_s() + s * PASSO_TELA_S)); sistema_pedir_redesenho(); return; }
  y += BTN_A + 12 + 32 + 24 + 34;   // lock button row
  if (tab5_dentro(tx, ty, area_pagina().largura - MARGEM - 260, y, 260, BTN_A)) {
    ajustes_definir_bloqueio(!ajustes_bloqueio());
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 24 + 34;   // wallpaper row
  if (tab5_dentro(tx, ty, area_pagina().largura - MARGEM - 260, y, 260, BTN_A)) {
    ajustes_definir_parede((uint8_t)((ajustes_parede() + 1) % (AJUSTES_PAREDE_NENHUM + 1)));
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 24 + 34;
  s = menos_mais_toque(tx, ty, y);
  if (s) {
    // Steps through the panel's table of real clocks (80, 60, 48, 40, 30, 24).
    const int v = tab5_power_relogio_vizinho(tab5_power_relogio_dpi(), s);
    // Apply first, save only if it worked: a value the panel rejected must
    // not become the one for the next boot.
    if (tab5_power_relogio_dpi(v)) {
      ajustes_definir_relogio_painel((uint8_t)v);
      sistema_pedir_redesenho_completo();
    }
  }
}

// ---- Keyboard --------------------------------------------------------------
void pag_teclado(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[64];
  est_teclado(sub, sizeof(sub));
  if (tab5_teclado_presente()) {
    snprintf(sub, sizeof(sub), tr("Encaixado (firmware %u)"), (unsigned)tab5_teclado_versao());
  }
  snprintf(assin, sizeof(assin), "%s", sub);
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  const int y = info(d, a, CONTEUDO_Y, tr("Estado"), sub, tab5_teclado_presente() ? TEMA_SUCESSO : TEMA_TEXTO);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("O teclado encaixa no ExtPort1 e é reconhecido sozinho: encaixado, "
            "ele vale nas conversas; sem ele, o teclado da tela."));
}

// ---- Grove -----------------------------------------------------------------
void pag_grove(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[64];
  est_grove(sub, sizeof(sub));
  snprintf(assin, sizeof(assin), "%s|%d", sub, (int)ajustes_grove());
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = info(d, a, CONTEUDO_Y, tr("Estado"), sub);
  y = botao_largo(d, a, y, ajustes_grove() ? tr("DESLIGAR O 5V") : tr("LIGAR O 5V"),
                  ajustes_grove() ? TEMA_ALERTA : TEMA_SUCESSO);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("Ligado, o módulo do cabo (C6L, GPS) fica sempre alimentado e não reinicia "
            "a cada uso. Desligado economiza bateria sem nada plugado."));
}
void toque_grove(int tx, int ty)
{
  if (tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + 84, area_pagina().largura - 2 * MARGEM, BTN_A)) {
    sistema_grove(!ajustes_grove());
    sistema_pedir_redesenho();
  }
}

// ---- About -----------------------------------------------------------------
void pag_sobre(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char assin[96], mem[48], ps[48], ativo[32];
  const size_t li = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t lp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  snprintf(mem, sizeof(mem), tr("%u KB livres"), (unsigned)(li / 1024));
  snprintf(ps, sizeof(ps), tr("%u KB livres"), (unsigned)(lp / 1024));
  const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
  tab5_duracao(s, ativo, sizeof(ativo));
  snprintf(assin, sizeof(assin), "%s|%s|%s", mem, ps, ativo);
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  char sistema[48];
  snprintf(sistema, sizeof(sistema), "M5tabOS %s", esp_app_get_description()->version);
  y = info(d, a, y, tr("Sistema"), sistema);
  y = info(d, a, y, tr("Compilado em"), m5tabos_compilado);
  y = info(d, a, y, "ESP-IDF", esp_get_idf_version());
  y = info(d, a, y, tr("Memória interna"), mem);
  y = info(d, a, y, "PSRAM", ps);
  y = info(d, a, y, tr("Ligado há"), ativo);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            tr("M5Stack Tab5: ESP32-P4 (sistema) e ESP32-C6 (Wi-Fi, Bluetooth e ESP-NOW)."));
}

// ---- USB (flash drive) -----------------------------------------------------
// The whole flash-drive screen: status, transfer, the log of saved writes,
// the result of the last round and the auto-enable setting. The work lives
// in the service (services/tab5_pendrive) - this page only requests and
// displays, and the user may leave it freely: the flash drive carries on in
// the background. The CONFIRMATION to hand back without the computer having
// ejected lives here.
typedef struct { int x, y, l, a; } usb_ret_t;
static usb_ret_t s_usb_bt[4];          // the buttons as drawn (area coordinates)
static bool      s_usb_confirmar = false;

static int usb_botao(lgfx::LovyanGFX& d, const area_t& a, int i, int y, const char* txt, uint16_t cor)
{
  s_usb_bt[i] = { MARGEM, y, a.largura - 2 * MARGEM, BTN_A };
  tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, txt, cor);
  fim(a.y + y + BTN_A);
  return y + BTN_A + 20;
}

void pag_usb(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  tab5_sd_resumo_t r;
  tab5_sd_resumo(&r);
  if (e.fase != PENDRIVE_ATIVO) { s_usb_confirmar = false; }
  // Opened with a result waiting: the user has seen it - the status bar notice goes.
  if (e.fase == PENDRIVE_PARADO) { tab5_pendrive_resultado_visto(); }

  // WHAT TO SHOW
  char estado[64], l2[96] = "", l3[80] = "", fim[96] = "";
  uint16_t cor = TEMA_TEXTO;
  switch (e.fase) {
    case PENDRIVE_PREPARANDO: snprintf(estado, sizeof(estado), tr("Preparando o cartão...")); break;
    case PENDRIVE_DEVOLVENDO: snprintf(estado, sizeof(estado), tr("Devolvendo o cartão...")); break;
    case PENDRIVE_ATIVO:
      if (!e.usb.montado)       { snprintf(estado, sizeof(estado), tr("Esperando o computador")); cor = TEMA_ALERTA; }
      else if (e.usb.suspenso)  { snprintf(estado, sizeof(estado), tr("Computador parado ou cabo solto")); cor = TEMA_ALERTA; }
      else if (e.usb.ativo)     { snprintf(estado, sizeof(estado), tr("Copiando...")); cor = TEMA_DESTAQUE; }
      else                      { snprintf(estado, sizeof(estado), tr("Cartão no computador")); cor = TEMA_SUCESSO; }
      break;
    default:
      snprintf(estado, sizeof(estado), "%s", e.oferta ? tr("Computador conectado") : tr("Desligado"));
      cor = e.oferta ? TEMA_ALERTA : TEMA_TEXTO;
      break;
  }
  if (e.fase == PENDRIVE_ATIVO) {
    char li[24], gr[24];
    tab5_bytes_txt(li, sizeof(li), e.usb.lidos);
    tab5_bytes_txt(gr, sizeof(gr), e.usb.gravados);
    snprintf(l2, sizeof(l2), tr("Lidos %s  ·  gravados %s%s"), li, gr, e.usb.erros ? tr("  ·  ERROS") : "");
  }
  if (e.fase != PENDRIVE_PARADO || r.no_diario) {
    snprintf(l3, sizeof(l3), tr("%u gravação(ões) guardadas (%u de %u KB)"),
             (unsigned)r.no_diario, (unsigned)(r.diario_usado / 1024), (unsigned)(r.diario_total / 1024));
  }
  // DIAGNOSTICS: how far the computer got, in the order it does things. On a
  // disk that does not appear, this is what tells whether the problem is in
  // USB (not even recognised), in the disk request or in reading (its OS gave up).
  char diag[96] = "";
  if (e.fase == PENDRIVE_ATIVO) {
    const tab5_pendrive_usb_t& u = e.usb;
    if (!u.n_config)            { snprintf(diag, sizeof(diag), tr("USB: o computador não reconheceu o aparelho")); }
    else if (!u.n_inquiry)      { snprintf(diag, sizeof(diag), tr("USB: reconheceu, mas não pediu o disco")); }
    else if (!u.n_respostas)    { snprintf(diag, sizeof(diag), tr("USB: pediu o disco %ux, a resposta não chegou"),
                                           (unsigned)u.n_inquiry); }
    else if (!u.n_capacidade)   { snprintf(diag, sizeof(diag), tr("USB: pediu o disco, não o tamanho (resp. %u, últ. 0x%02X)"),
                                           (unsigned)u.n_respostas, (unsigned)u.op_respondido); }
    else if (!u.n_leituras)     { snprintf(diag, sizeof(diag), tr("USB: viu o disco, ainda não leu")); }
    else                        { snprintf(diag, sizeof(diag), tr("USB: %u leitura(s), %u entregue(s), a mais lenta %u ms"),
                                           (unsigned)u.n_cmd_leitura, (unsigned)u.n_leituras_ok,
                                           (unsigned)u.leitura_max_ms); }
    // Stopped for more than 3 s: says WHERE - that is what separates a USB (or
    // SD card) hang from a computer that simply stopped asking.
    if (u.n_config && u.parado_ms >= 3000) {
      switch (u.onde) {
        case PENDRIVE_ESPERA_CARTAO:
          snprintf(diag, sizeof(diag), tr("USB: PARADO no cartão (setor %u)"), (unsigned)u.ultimo_setor); break;
        case PENDRIVE_ESPERA_USB:
          snprintf(diag, sizeof(diag), tr("USB: PARADO no USB, entregando o setor %u (%u/%u)"),
                   (unsigned)u.ultimo_setor, (unsigned)u.n_leituras_ok, (unsigned)u.n_cmd_leitura); break;
        default:
          if (u.n_cmd_leitura) {
            snprintf(diag, sizeof(diag), tr("USB: o computador parou de pedir (%u/%u leituras)"),
                     (unsigned)u.n_leituras_ok, (unsigned)u.n_cmd_leitura);
          }
          break;
      }
    }
    if (u.op_recusado) {
      const size_t k = strlen(diag);
      snprintf(diag + k, sizeof(diag) - k, tr(" · recusou 0x%02X"), (unsigned)u.op_recusado);
    }
  }
  char perdas[64] = "";
  if (r.perdidas) {
    snprintf(perdas, sizeof(perdas), tr("%u gravação(ões) perdidas: memória cheia"), (unsigned)r.perdidas);
  }
  if (e.fase == PENDRIVE_PARADO) {
    switch (e.motivo) {
      case PENDRIVE_RECUSA_APP:        snprintf(fim, sizeof(fim), tr("Feche o app aberto: ele está usando o cartão.")); break;
      case PENDRIVE_RECUSA_EM_USO:     snprintf(fim, sizeof(fim), tr("O cartão estava em uso. Tente de novo.")); break;
      case PENDRIVE_RECUSA_SEM_CARTAO: snprintf(fim, sizeof(fim), tr("Sem cartão SD, ou ele não respondeu.")); break;
      case PENDRIVE_RECUSA_SEM_DIARIO: snprintf(fim, sizeof(fim), tr("Falta a memória interna do pendrive: grave o firmware de novo.")); break;
      case PENDRIVE_RECUSA_USB:        snprintf(fim, sizeof(fim), tr("O USB não iniciou. O cartão continua no Tab5.")); break;
      default:
        if (e.sem_ejetar) {
          // See tab5_pendrive.cpp (s_saiu_sem_ejetar): if the disk does not appear
          // straight away next time, the computer is checking the card.
          snprintf(fim, sizeof(fim), tr("Saiu sem ejetar: o computador verifica o cartão antes de abrir."));
        } else if (r.completo) {
          snprintf(fim, sizeof(fim), tr("Última vez: %u gravação(ões) aplicada(s)%s%s"),
                   (unsigned)r.aplicadas, r.conflitos ? tr(", conflito em ") : "",
                   r.conflitos ? tr("arquivo(s) .tab5") : "");
        }
        break;
    }
  }

  char assin[sizeof(s_assin)];
  snprintf(assin, sizeof(assin), "%d|%d|%d|%s|%s|%s|%s|%d|%u|%u|%s", (int)e.fase, (int)e.motivo, (int)s_usb_confirmar,
           estado, l2, l3, fim, (int)ajustes_pendrive(), (unsigned)r.conflitos, (unsigned)r.perdidas, diag);
  if (!pintura_mudou_s(&s_cfg_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  memset(s_usb_bt, 0, sizeof(s_usb_bt));

  int y = info(d, a, CONTEUDO_Y, tr("Cartão SD como pendrive (USB-C)"), estado, cor);
  if (l2[0])  { tab5_escrever(d, l2, a.x + MARGEM, a.y + y, 26, false, TEMA_TEXTO, TEMA_FUNDO); y += 44; }
  if (l3[0])  { tab5_escrever(d, l3, a.x + MARGEM, a.y + y, 24, false, cor_sub(d), TEMA_FUNDO); y += 44; }
  if (diag[0])   { tab5_escrever(d, diag, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO); y += 40; }
  if (perdas[0]) { tab5_escrever(d, perdas, a.x + MARGEM, a.y + y, 24, false, TEMA_PERIGO, TEMA_FUNDO); y += 44; }
  if (fim[0]) { tab5_escrever(d, fim, a.x + MARGEM, a.y + y, 24, false, TEMA_ALERTA, TEMA_FUNDO); y += 44; }
  y += 8;

  switch (e.fase) {
    case PENDRIVE_PARADO:
      y = usb_botao(d, a, 0, y, tr("USAR COMO PENDRIVE"), TEMA_DESTAQUE);
      break;
    case PENDRIVE_ATIVO:
      if (s_usb_confirmar) {
        y = paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
                      tr("O computador não avisou que ejetou. Se ele tiver algo em cache, arquivos "
                      "podem ficar incompletos. Ejete no computador e o cartão volta sozinho. Já "
                      "usou \"Remover com segurança\" no Windows? Pode devolver.")) - a.y + 16;
        y = usb_botao(d, a, 0, y, tr("VOLTAR"), TEMA_DESTAQUE);
        y = usb_botao(d, a, 1, y, tr("DEVOLVER MESMO ASSIM"), TEMA_PERIGO);
      } else {
        y = usb_botao(d, a, 0, y, tr("DEVOLVER O CARTÃO AO TAB5"), TEMA_ALERTA);
      }
      break;
    default:
      y = usb_botao(d, a, 0, y, tr("AGUARDE..."), TEMA_TEXTO_3);
      s_usb_bt[0] = { };   // not responding
      break;
  }
  y = usb_botao(d, a, 2, y, ajustes_pendrive() ? tr("LIGAR SOZINHO NO COMPUTADOR: SIM") : tr("LIGAR SOZINHO NO COMPUTADOR: NÃO"),
                ajustes_pendrive() ? TEMA_SUCESSO : TEMA_TEXTO_2);
  y = paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
                tr("O Tab5 segue usável enquanto isso. O que ele gravar fica na memória interna e "
                "vai para o cartão na volta. Arquivos e Câmera esperam o cartão. Para terminar, "
                "ejete o disco no computador.")) - a.y + 20;
  // Disk RECYCLE BIN (Arquivos/Files app): automatically delete what is older than 30 days.
  tab5_escrever(d, tr("Lixeira: apagar itens com mais de 30 dias"), a.x + MARGEM, a.y + y, 24, false, TEMA_TEXTO, TEMA_FUNDO);
  y += 40;
  usb_botao(d, a, 3, y, ajustes_lixeira_dias() ? tr("30 dias") : tr("Desligado"),
            ajustes_lixeira_dias() ? TEMA_SUCESSO : TEMA_TEXTO_2);
}

static bool usb_em(int i, int tx, int ty)
{
  const usb_ret_t& b = s_usb_bt[i];
  return b.l > 0 && tab5_dentro(tx, ty, b.x, b.y, b.l, b.a);
}

void toque_usb(int tx, int ty)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  if (usb_em(2, tx, ty)) {
    ajustes_definir_pendrive(!ajustes_pendrive());
    sistema_pedir_redesenho();
    return;
  }
  if (usb_em(3, tx, ty)) {
    ajustes_definir_lixeira_dias(ajustes_lixeira_dias() ? 0 : 30);   // Off <-> 30 days
    sistema_pedir_redesenho();
    return;
  }
  if (e.fase == PENDRIVE_PARADO && usb_em(0, tx, ty)) {
    tab5_pendrive_iniciar();
    sistema_pedir_redesenho();
    return;
  }
  if (e.fase == PENDRIVE_ATIVO) {
    const bool montado = e.usb.montado && !e.usb.ejetado;
    if (!s_usb_confirmar && usb_em(0, tx, ty)) {
      if (montado) { s_usb_confirmar = true; } else { tab5_pendrive_devolver(); }
    } else if (s_usb_confirmar && usb_em(0, tx, ty)) {
      s_usb_confirmar = false;
    } else if (s_usb_confirmar && usb_em(1, tx, ty)) {
      ESP_LOGW("ajustes", "SD card handed back without the computer ejecting it");
      s_usb_confirmar = false;
      tab5_pendrive_devolver();
    }
    sistema_pedir_redesenho();
  }
}
