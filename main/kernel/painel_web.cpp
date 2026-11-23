// The "No PC" (PC access) panel - see painel_web.hpp.
#include "painel_web.hpp"
#include "app.h"
#include "idioma.h"
#include "tab5_ui.hpp"
#include "tab5_tela.hpp"
#include "tab5_web.hpp"
#include "tab5_wifi.hpp"
#include <stdio.h>
#include <string.h>

static const int BT_L = 360, BT_A = 72;

static void botao_rect(int x, int y, int l, int a, int* bx, int* by)
{
  *bx = x + (l - BT_L) / 2;
  *by = y + a - BT_A - 24;
}

static void linha(lgfx::LovyanGFX& d, const char* t, int cx, int y, int larg)
{
  char b[160];
  snprintf(b, sizeof(b), "%s", t);
  tab5_cortar_com_reticencias(d, b, sizeof(b), larg);
  d.drawString(b, cx, y);
}

void painel_web_desenhar(lgfx::LovyanGFX& d, int x, int y, int l, int a, const char* dono)
{
  tab5_tela_limpar(x, y, l, a, TFT_BLACK);
  const bool meu = tab5_web_aberto(dono);
  // The other apps open right now (the server is a single one, with one folder per app).
  const char* abertos[WEB_PARTES_MAX];
  const int na = tab5_web_abertos(abertos, WEB_PARTES_MAX);
  char outros[96] = "";
  for (int i = 0, k = 0; i < na; ++i) {
    if (dono && !strcmp(abertos[i], dono)) { continue; }
    k += snprintf(outros + k, sizeof(outros) - k, "%s%s", k ? ", " : "", tr(abertos[i]));
    if (k >= (int)sizeof(outros)) { break; }
  }
  const int cx = x + l / 2, larg = l - 48;
  int bx, by;
  botao_rect(x, y, l, a, &bx, &by);
  d.setTextDatum(middle_center);
  tab5_fonte_corpo(d);

  if (meu) {
    char url[64], senha[8], quem[48];
    // THE ROOT, not the app's folder: it is shorter to type, and the portal opens
    // straight on the app that has just started (and shows the others).
    tab5_web_endereco(nullptr, url, sizeof(url));
    tab5_web_senha(senha, sizeof(senha));
    tab5_web_ultimo(quem, sizeof(quem), nullptr);
    int ly = y + 36;
    tab5_texto(d, 2.4f);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    linha(d, tr("No navegador do PC (na mesma rede Wi-Fi), abra:"), cx, ly, larg);
    ly += 70;
    tab5_fonte_corpo_negrito(d);
    tab5_texto(d, 4.2f);
    if ((int)d.textWidth(url) > larg) { tab5_texto(d, 3.2f); }
    d.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    d.drawString(url, cx, ly);
    ly += 80;
    tab5_fonte_corpo(d);
    tab5_texto(d, 2.4f);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    linha(d, tr("A senha que o navegador pede (qualquer usuário):"), cx, ly, larg);
    ly += 72;
    tab5_fonte_corpo_negrito(d);
    tab5_texto(d, 5.2f);
    d.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    d.drawString(senha, cx, ly);
    ly += 82;
    tab5_fonte_corpo(d);
    tab5_texto(d, 2.4f);
    char t[96];
    if (quem[0]) { snprintf(t, sizeof(t), tr("Conectado: %s"), quem); d.setTextColor(TFT_GREENYELLOW, TFT_BLACK); }
    else         { snprintf(t, sizeof(t), "%s", tr("Esperando o navegador...")); d.setTextColor(TFT_DARKGREY, TFT_BLACK); }
    d.drawString(t, cx, ly);
    tab5_texto(d, 2.0f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    if (outros[0]) {
      char t2[140];
      snprintf(t2, sizeof(t2), tr("Também abertos no PC: %s"), outros);
      linha(d, t2, cx, by - 66, larg);
    }
    linha(d, tr("Roda de fundo; desliga sozinho após 10 min sem uso."), cx, by - 30, larg);
    tab5_botao(d, bx, by, BT_L, BT_A, tr("Desligar"), TFT_ORANGE);
  } else {
    tab5_texto(d, 3.0f);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    linha(d, tr("Usar pelo navegador do PC"), cx, y + 50, larg);
    tab5_texto(d, 2.4f);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    linha(d, tr("O Tab5 abre uma página na sua rede Wi-Fi, com senha."), cx, y + 120, larg);
    linha(d, tr("Só liga quando você pede; desliga após 10 min sem uso."), cx, y + 164, larg);
    if (outros[0]) {
      char t[140];
      snprintf(t, sizeof(t), tr("Já abertos no PC: %s (a mesma senha)"), outros);
      d.setTextColor(TFT_SKYBLUE, TFT_BLACK);
      linha(d, t, cx, y + 220, larg);
    }
    tab5_botao(d, bx, by, BT_L, BT_A, tr("Ligar"), TFT_GREENYELLOW);
  }
  d.setTextDatum(top_left);
}

bool painel_web_tocar(int x, int y, int l, int a, int tx, int ty, const char* dono, bool (*ligar)(void))
{
  int bx, by;
  botao_rect(x, y, l, a, &bx, &by);
  if (!tab5_dentro(tx, ty, bx, by, BT_L, BT_A)) { return false; }
  if (tab5_web_aberto(dono)) { tab5_web_fechar(dono); return true; }   // the screen changes via AVISO_WEB
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { sistema_recado(tr("Conecte o Wi-Fi primeiro")); return true; }
  if (!ligar || !ligar()) { sistema_recado(tr("O servidor não ligou")); }
  return true;
}

uint32_t painel_web_assinatura(void)
{
  char quem[48];
  tab5_web_ultimo(quem, sizeof(quem), nullptr);
  uint32_t h = tab5_web_ligado() ? 1u : 0u;
  const char* abertos[WEB_PARTES_MAX];
  const int na = tab5_web_abertos(abertos, WEB_PARTES_MAX);
  for (int i = 0; i < na; ++i) { for (const char* p = abertos[i]; *p; ++p) { h = h * 31u + (uint8_t)*p; } h = h * 31u + 1u; }
  for (const char* p = quem; *p; ++p) { h = h * 31u + (uint8_t)*p; }
  return h;
}
