// WELCOME - the initial setup, only on first use.
//
// Boot opens this app when ajustes_config_inicial() is false: a device that
// has never run the system, or one flashed with M5Burner (which wipes the settings).
// Anyone already using the system never sees it - flashing a new version from VS Code doesn't
// bring the wizard back (see ajustes.h).
//
// Four quick steps, each adjustable later in Settings:
//   1. language         (Settings > Language; takes effect immediately, already for the following steps)
//   2. time zone        (Settings > Date and time)
//   3. mesh over the internet: the region's MQTT server (Settings > Mesh over the internet)
//   4. Wi-Fi            (sends you to Settings > Wi-Fi, or finishes)
//
// IT IS NOT ON THE HOME SCREEN: it doesn't live in an app_* folder (the app list is
// generated from them). Closing by any route - CONCLUIR, or the bar's HOME -
// counts as done: whoever skipped it doesn't want to see it again.

#include <cstdlib>
#include "app.h"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_fuso.hpp"
#include "tab5_ui.hpp"
#include "tab5_tela.hpp"
#include "tab5_hora.hpp"
#include "tab5_mesh_mqtt.hpp"

extern const app_t APP_CONFIG;

static int s_passo = 0;
static const int N_PASSOS = 4;

static const int M = 32;          // margin
static const int BTN_A = 80;

static void titulo(lgfx::LovyanGFX& d, const area_t& a, const char* t, const char* sub)
{
  tab5_titulo(d, 44);
  d.setTextDatum(top_left);
  d.setTextColor(TFT_WHITE);
  d.drawString(tr(t), a.x + M, a.y + 48);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.6f);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(tr(sub), a.x + M, a.y + 112);
}

static void passo_e_botoes(lgfx::LovyanGFX& d, const area_t& a)
{
  // The progress dots and the VOLTAR / PROXIMO (or CONCLUIR) buttons.
  for (int i = 0; i < N_PASSOS; ++i) {
    d.fillCircle(a.x + a.largura / 2 + (i - 1) * 32 - 16, a.y + a.altura - BTN_A - 64, 8,
                 i == s_passo ? TFT_CYAN : TFT_DARKGREY);
  }
  const int y = a.y + a.altura - BTN_A - 24;
  const int l = (a.largura - 3 * M) / 2;
  if (s_passo > 0) { tab5_botao(d, a.x + M, y, l, BTN_A, tr("VOLTAR"), d.color565(50, 55, 65), TFT_LIGHTGREY); }
  tab5_botao(d, a.x + 2 * M + l, y, l, BTN_A, s_passo == N_PASSOS - 1 ? tr("CONCLUIR") : tr("PRÓXIMO"), TFT_GREENYELLOW);
}

// ---- step 2: time zone ---------------------------------------------------------
static const int FUSO_Y = 260;
static const int PM_L = 150;
static const int REGIAO_Y = FUSO_Y + BTN_A + 200;   // the region row (daylight saving)

static void desenhar_fuso(lgfx::LovyanGFX& d, const area_t& a)
{
  titulo(d, a, tr("Fuso horário"), tr("A hora que a barra e os apps mostram."));
  char f[40];
  tab5_fuso_texto((uint32_t)time(nullptr), f, sizeof(f));
  const int y = a.y + FUSO_Y;
  tab5_botao(d, a.x + M, y, PM_L, BTN_A, tr("-30 min"), TFT_ORANGE);
  tab5_botao(d, a.x + a.largura - M - PM_L, y, PM_L, BTN_A, tr("+30 min"), TFT_GREENYELLOW);
  d.setFont(tab5_fonte_suave(true, 44));
  d.setTextSize(1);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_WHITE);
  d.drawString(f, a.x + a.largura / 2, y + BTN_A / 2);
  const uint32_t e = tab5_hora_epoch();
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.6f);
  d.setTextColor(TFT_LIGHTGREY);
  if (e) {
    struct tm t;
    tab5_hora_local(e, &t);
    char h[48];
    char hm[8];
    tab5_hora_hhmm(&t, hm, sizeof(hm));
    snprintf(h, sizeof(h), tr("Agora: %s"), hm);
    d.drawString(h, a.x + a.largura / 2, y + BTN_A + 60);
  } else {
    d.drawString(tr("A hora certa chega pelo Wi-Fi, GPS ou malha."), a.x + a.largura / 2, y + BTN_A + 60);
  }
  // Region: offset plus the daylight-saving rule (services/tab5_fuso).
  const int yr = a.y + REGIAO_Y;
  d.drawString(tr("Ou escolha a sua região (horário de verão automático):"), a.x + a.largura / 2, yr - 40);
  tab5_botao(d, a.x + M, yr, PM_L, BTN_A, "<", d.color565(50, 55, 65), TFT_LIGHTGREY);
  tab5_botao(d, a.x + a.largura - M - PM_L, yr, PM_L, BTN_A, ">", d.color565(50, 55, 65), TFT_LIGHTGREY);
  const int z = ajustes_fuso_zona();
  d.setTextColor(z == TAB5_FUSO_MANUAL ? TFT_LIGHTGREY : TFT_GREENYELLOW);
  d.drawString(tab5_fuso_nome(z), a.x + a.largura / 2, yr + BTN_A / 2);
  d.setTextDatum(top_left);
}

// ---- step 3: mesh over the internet ----------------------------------------------
static const int LISTA_Y = 200, PASSO = BTN_A + 14;
static const int IDIOMA_Y = 220;
// Those from the list that fit above the footer buttons (the factory one has 3; the
// others are added later, in Settings > Meshtastic).
static const int MALHA_MAX = 6;
static int malha_itens(void)
{
  const int n = mesh_mqtt_n_servidores();
  return n < MALHA_MAX ? n : MALHA_MAX;
}

static void desenhar_malha(lgfx::LovyanGFX& d, const area_t& a)
{
  titulo(d, a, tr("Meshtastic pela internet"), tr("Sem um rádio, o Tab5 fala com a malha da sua região."));
  const int n = malha_itens();
  const uint8_t esc = ajustes_mqtt_servidor();
  const bool ligada = ajustes_mesh_internet();
  int y = a.y + LISTA_Y;
  for (int i = 0; i <= n; ++i, y += PASSO) {
    const bool desligar = (i == n);
    const bool sel = desligar ? !ligada : (ligada && esc == i);
    mesh_mqtt_servidor_t s;
    if (!desligar && !mesh_mqtt_servidor(i, &s)) { continue; }
    const char* nome = desligar ? tr("Não usar a internet") : s.nome;
    tab5_botao(d, a.x + M, y, a.largura - 2 * M, BTN_A, nome,
               sel ? TFT_GREENYELLOW : d.color565(50, 55, 65), sel ? TFT_BLACK : TFT_LIGHTGREY);
  }
}

// ---- drawing and touch -----------------------------------------------------------
static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  // Each change requests a full pass: the whole frame, without flicker.
  if (!sistema_area_foi_limpa()) { return; }
  const lgfx::TextStyle estilo = d.getTextStyle();
  switch (s_passo) {
    case 0:
      titulo(d, a, tr("Bem-vindo ao M5tabOS"), tr("Três ajustes rápidos e o Tab5 está pronto."));
      // Each language with its name IN ITSELF: that's how people find theirs.
      for (int i = 0; i < idioma_n(); ++i) {
        const bool sel = (idioma_atual() == i);
        tab5_botao(d, a.x + M, a.y + IDIOMA_Y + i * PASSO, a.largura - 2 * M, BTN_A, idioma_nome(i),
                   sel ? TFT_GREENYELLOW : d.color565(50, 55, 65), sel ? TFT_BLACK : TFT_LIGHTGREY);
      }
      break;
    case 1: desenhar_fuso(d, a); break;
    case 2: desenhar_malha(d, a); break;
    default:
      titulo(d, a, tr("Wi-Fi"), tr("Para a internet, a hora certa e a malha pela internet."));
      tab5_botao(d, a.x + M, a.y + 220, a.largura - 2 * M, BTN_A, tr("CONECTAR O WI-FI AGORA"), TFT_SKYBLUE);
      tab5_fonte_corpo(d);
      tab5_texto(d, 2.6f);
      d.setTextDatum(top_left);
      d.setTextColor(TFT_LIGHTGREY);
      d.drawString(tr("Abre Ajustes. Também dá depois:"), a.x + M, a.y + 330);
      d.drawString(tr("Ajustes > Wi-Fi > PROCURAR REDES."), a.x + M, a.y + 370);
      break;
  }
  passo_e_botoes(d, a);
  d.setTextStyle(estilo);
}

static void concluir(const app_t* abrir_depois)
{
  ajustes_definir_config_inicial(true);
  sistema_simular_abrir(abrir_depois);   // nullptr = back to the home screen
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();
  const int yb = a.altura - BTN_A - 24;
  const int l = (a.largura - 3 * M) / 2;
  if (tab5_dentro(x, y, 2 * M + l, yb, l, BTN_A)) {
    if (s_passo == N_PASSOS - 1) { concluir(nullptr); return; }
    ++s_passo;
  } else if (s_passo > 0 && tab5_dentro(x, y, M, yb, l, BTN_A)) {
    --s_passo;
  } else if (s_passo == 0) {
    for (int i = 0; i < idioma_n(); ++i) {
      if (tab5_dentro(x, y, M, IDIOMA_Y + i * PASSO, a.largura - 2 * M, BTN_A)) { idioma_definir(i); }
    }
  } else if (s_passo == 1) {
    const int yf = FUSO_Y;
    int s = 0;
    if (tab5_dentro(x, y, M, yf, PM_L, BTN_A)) { s = -1; }
    if (tab5_dentro(x, y, a.largura - M - PM_L, yf, PM_L, BTN_A)) { s = +1; }
    if (s) {
      ajustes_definir_fuso_zona(TAB5_FUSO_MANUAL);
      ajustes_definir_fuso_min((int16_t)(ajustes_fuso_min() + s * 30));
    }
    int r = 0;
    if (tab5_dentro(x, y, M, REGIAO_Y, PM_L, BTN_A)) { r = -1; }
    if (tab5_dentro(x, y, a.largura - M - PM_L, REGIAO_Y, PM_L, BTN_A)) { r = +1; }
    if (r) {
      const int n = tab5_fuso_n();
      const int z = ((int)ajustes_fuso_zona() + r + n) % n;
      ajustes_definir_fuso_zona((uint8_t)z);
      if (z != TAB5_FUSO_MANUAL) { ajustes_definir_fuso_min((int16_t)tab5_fuso_padrao_min(z)); }
    }
  } else if (s_passo == 2) {
    const int n = malha_itens();
    for (int i = 0; i <= n; ++i) {
      if (!tab5_dentro(x, y, M, LISTA_Y + i * PASSO, a.largura - 2 * M, BTN_A)) { continue; }
      if (i == n) { ajustes_definir_mesh_internet(false); }
      else        { ajustes_definir_mesh_internet(true); ajustes_definir_mqtt_servidor((uint8_t)i); }
    }
  } else if (s_passo == 3 && tab5_dentro(x, y, M, 220, a.largura - 2 * M, BTN_A)) {
    concluir(&APP_CONFIG);
    return;
  } else {
    return;
  }
  sistema_pedir_redesenho_completo();
}

static bool ao_abrir(void) { s_passo = 0; return true; }

// Closing by any route counts as done (see the top). The Mesh service
// picks up the chosen server when it comes up over the internet.
static void ao_fechar(void) { if (!ajustes_config_inicial()) { ajustes_definir_config_inicial(true); } }

extern const app_t APP_BOAS_VINDAS = {
  .nome  = N_("Boas-vindas"),
  .cor   = TFT_CYAN,
  .perfil = { },
  .orientacao = ORI_EM_PE,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
};
