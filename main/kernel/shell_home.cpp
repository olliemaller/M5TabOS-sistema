// SHELL - the home screen: the pages, the icon order (saved in NVS), moving and
// removing icons, the page dots and drawing the icons. Split out of
// shell.cpp; what the parts share lives in shell_interno.hpp.

#include "esp_attr.h"
#include "bloqueio_anim.hpp"
#include "tela_virtual.hpp"
#include "ajustes.h"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <strings.h>   // strncasecmp
#include <stdio.h>
#include "shell_interno.hpp"
#include "idioma.h"
#include "tab5_hora.hpp"
#include "tab5_wifi.hpp"
#include "tab5_mesh.hpp"
#include "tab5_tema.hpp"
#include <time.h>

static const char *TAG = "shell";

// HOME SCREEN GEOMETRY (px). Top to bottom: the clock card, the icon grid
// (each with its label underneath) and the page-dot strip.
// Columns and rows per page come from the free space (home_por_pagina), not
// from fixed numbers.
static const int CARTAO_Y   = TEMA_ESPACO_4;    // from the top of the area to the card
static const int CARTAO_PAD = 28;               // breathing room inside the card
static const int CARTAO_A   = 2 * CARTAO_PAD + TEMA_LETRA_GRANDE + 4 + TEMA_LETRA_CORPO;   // time + date
static const int GRADE_TOPO = CARTAO_Y + CARTAO_A + TEMA_ESPACO_5;   // where the 1st row of icons starts

// Label (app name): 10 px below the icon, 24 px lettering. The reserve is what
// the row takes beyond the icon square; between one row and the next there is
// ESPACO_LINHAS left over.
static const int ROTULO_ESPACO  = 10;
static const int ROTULO_LETRA   = 24;
static const int ROTULO_LETRA_MIN = 20;
static const int ROTULO_RESERVA = ROTULO_ESPACO + ROTULO_LETRA;
static const int ESPACO_LINHAS  = 28;

// Strip under the icons for the page dots.
static const int HOME_BOLINHAS_A = 32;

// Horizontal gap between icons: the grid takes the width minus the margin on
// both sides, and what is left is shared among the columns.
static int vao_colunas(const area_t& a)
{
  const int cols = icone_cols(a);
  return (a.largura - 2 * TEMA_MARGEM - cols * ICONE_LADO) / (cols - 1);
}

static void celula_do_icone(int i, const area_t& a, int* px, int* py)
{
  const int cols = icone_cols(a);
  const int col = i % cols;
  const int lin = i / cols;
  *px = a.x + TEMA_MARGEM + col * (ICONE_LADO + vao_colunas(a));
  *py = a.y + GRADE_TOPO + lin * (ICONE_LADO + ROTULO_RESERVA + ESPACO_LINHAS);
}

// ---------------------------------------------------------------------------
// HOME SCREEN PAGES
// ---------------------------------------------------------------------------
// Page p shows apps [p*por_pagina, (p+1)*por_pagina). celula_do_icone()
// still receives the index WITHIN the page, so drawing and touch (which
// use the same function) always stay in agreement. Nothing in apps.cpp changes: the
// number of pages comes from SISTEMA_NUM_APPS.
static int s_pagina_home = 0;

static int home_por_pagina(const area_t& a)
{
  int linhas = 0;
  while (linhas < TAB5_HOME_LINHAS_MAX) {
    int px, py;
    celula_do_icone(linhas * icone_cols(a), a, &px, &py);
    if (py + ICONE_LADO + ROTULO_RESERVA > a.y + a.altura - HOME_BOLINHAS_A) { break; }
    ++linhas;
  }
  return (linhas < 1 ? 1 : linhas) * icone_cols(a);
}

static int home_paginas(const area_t& a)
{
  const int pp = home_por_pagina(a);
  return (SISTEMA_NUM_APPS + pp - 1) / pp;
}

bool home_mudar_pagina(int delta)
{
  const int n = home_paginas(area_t(sistema_area()));
  int nova = s_pagina_home + delta;
  if (nova < 0 || nova >= n) { return false; }
  s_pagina_home = nova;
  s_sujo_layout = true;   // the icons' POSITION changed: full repaint
  return true;
}

// ---------------------------------------------------------------------------
// ICON ORDER (see sistema_home_app in app.h)
// ---------------------------------------------------------------------------
// s_ordem[pos] = index into SISTEMA_APPS. Drawing and touch read from here; only
// move/load write, and always inside s_ordem_mux - the console changes it from
// another task. Reading without the lock is safe: each entry is one byte, and the worst case
// is one frame with the old order, soon repainted (moving requests a redraw).
#define HOME_APPS_MAX 64
static uint8_t s_ordem[HOME_APPS_MAX];
static int     s_n_ordem = 0;
static portMUX_TYPE s_ordem_mux = portMUX_INITIALIZER_UNLOCKED;

static int home_n(void) { return s_n_ordem ? s_n_ordem : SISTEMA_NUM_APPS; }

const app_t* sistema_home_app(int pos)
{
  if (pos < 0 || pos >= home_n()) { return nullptr; }
  return SISTEMA_APPS[s_n_ordem ? s_ordem[pos] : pos];
}

int sistema_home_posicao(const app_t* app)
{
  for (int i = 0; i < home_n(); ++i) { if (sistema_home_app(i) == app) { return i; } }
  return -1;
}

// Builds the order from the names: the known ones in the given order (a repeat
// counts once), and then those missing in apps.cpp order.
static void ordem_de_nomes(const char* nomes)
{
  uint8_t nova[HOME_APPS_MAX];
  bool usado[HOME_APPS_MAX] = {};
  const int total = SISTEMA_NUM_APPS < HOME_APPS_MAX ? SISTEMA_NUM_APPS : HOME_APPS_MAX;
  if (SISTEMA_NUM_APPS > HOME_APPS_MAX) {
    ESP_LOGE(TAG, "%d apps, the home screen only orders %d (HOME_APPS_MAX)", SISTEMA_NUM_APPS, HOME_APPS_MAX);
  }
  int n = 0;
  const char* p = nomes ? nomes : "";
  while (*p) {
    const char* fim = strchr(p, ',');
    const size_t len = fim ? (size_t)(fim - p) : strlen(p);
    for (int i = 0; i < total; ++i) {
      const char* nome = SISTEMA_APPS[i]->nome;
      if (SISTEMA_APPS[i]->fora_da_home) { continue; }   // has no icon (app.h)
      if (!usado[i] && strlen(nome) == len && strncasecmp(nome, p, len) == 0) {
        usado[i] = true; nova[n++] = (uint8_t)i; break;
      }
    }
    if (!fim) { break; }
    p = fim + 1;
  }
  for (int i = 0; i < total; ++i) { if (!usado[i] && !SISTEMA_APPS[i]->fora_da_home) { nova[n++] = (uint8_t)i; } }

  taskENTER_CRITICAL(&s_ordem_mux);
  memcpy(s_ordem, nova, n);
  s_n_ordem = n;
  taskEXIT_CRITICAL(&s_ordem_mux);
}

// ONE buffer for saving and for loading the order (both only run in the
// UI, never at the same time): there used to be two of 1 KB in internal RAM.
EXT_RAM_BSS_ATTR static char s_ordem_nomes[1024];   // UI only (PSRAM: see main/linker.lf)

static void ordem_gravar(void)
{
  char* const nomes = s_ordem_nomes;
  const size_t tam = sizeof(s_ordem_nomes);
  size_t k = 0;
  nomes[0] = 0;
  for (int i = 0; i < home_n(); ++i) {
    const char* nome = sistema_home_app(i)->nome;
    const size_t len = strlen(nome);
    if (k + len + 2 > tam) { break; }
    if (k) { nomes[k++] = ','; }
    memcpy(nomes + k, nome, len);
    k += len;
    nomes[k] = 0;
  }
  ajustes_definir_ordem_apps(nomes);
}

void home_ordem_carregar(void)
{
  ajustes_ordem_apps(s_ordem_nomes, sizeof(s_ordem_nomes));
  ordem_de_nomes(s_ordem_nomes);
}

// Takes the app out of 'de' and slots it into 'para'; those in between shift one place.
static void ordem_mover(int de, int para)
{
  taskENTER_CRITICAL(&s_ordem_mux);
  if (s_n_ordem && de >= 0 && de < s_n_ordem && para >= 0 && para < s_n_ordem && de != para) {
    const uint8_t v = s_ordem[de];
    if (de < para) { memmove(&s_ordem[de], &s_ordem[de + 1], para - de); }
    else           { memmove(&s_ordem[para + 1], &s_ordem[para], de - para); }
    s_ordem[para] = v;
  }
  taskEXIT_CRITICAL(&s_ordem_mux);
}

bool sistema_home_mover(const app_t* app, int pos)
{
  const int de = sistema_home_posicao(app);
  if (de < 0 || pos < 0 || pos >= home_n()) { return false; }
  ordem_mover(de, pos);
  ordem_gravar();
  sistema_pedir_redesenho_completo();
  return true;
}

void sistema_home_ordem_padrao(void)
{
  ordem_de_nomes("");
  ajustes_definir_ordem_apps("");
  sistema_pedir_redesenho_completo();
}

// Which position (in the order) is under the finger, on the current page; -1 = none.
// Without slack it is the icon square (the click). With slack, the whole cell,
// label and gap included, and an empty cell at the end of the last page
// counts as the last position - it is the drag target, which must not land in a hole.
int home_pos_em(const area_t& a, int tx, int ty, bool folga)
{
  const int por_pagina = home_por_pagina(a);
  const int primeiro = s_pagina_home * por_pagina;
  const int fx = folga ? vao_colunas(a) / 2 : 0;
  const int fy_cima  = folga ? ESPACO_LINHAS / 2 : 0;
  const int fy_baixo = folga ? ROTULO_RESERVA + ESPACO_LINHAS / 2 : 0;
  for (int k = 0; k < por_pagina; ++k) {
    int px, py;
    celula_do_icone(k, a, &px, &py);
    if (tx >= px - fx && tx < px + ICONE_LADO + fx && ty >= py - fy_cima && ty < py + ICONE_LADO + fy_baixo) {
      const int pos = primeiro + k;
      if (pos < home_n()) { return pos; }
      return folga ? home_n() - 1 : -1;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// MOVE ICON (hold and drag) - see home_segurar in internal.hpp
// ---------------------------------------------------------------------------
// Repaints the home screen only when the icon CHANGES place, never on every finger
// sample: each full repaint is ~70KB per icon (see desenhar_home).
// To another page: hold the finger at the side edge for HOME_BORDA_MS.
#define HOME_SEGURAR_MS 600
#define HOME_BORDA_PX   60
#define HOME_BORDA_MS   700

static int      s_mover = -1;          // position of the lifted icon; -1 = none
static bool     s_mover_mudou = false;
static uint32_t s_borda_desde = 0;

// REMOVING A THIRD-PARTY APP (script from the card): with its icon lifted, a
// strip appears at the top; dropping on it asks for confirmation. System apps
// do not show the strip (sistema_apps_de_fora_removivel). The one that actually
// removes is the owner of the third-party apps - the kernel only asks and requests.
#define HOME_REMOVER_A  110
static bool         s_sobre_remover = false;   // the finger is on the strip
const app_t* s_remover_app = nullptr;   // waiting for the confirmation's "Remover"
static area_t       s_rem_bt_sim = {}, s_rem_bt_nao = {};

bool home_segurar(int tx, int ty, uint32_t agora, uint32_t segurando_ms, int desvio)
{
  const area_t a = sistema_area();
  ty -= TOQUE_DY;   // the same correction as the click (tratar_toque)

  if (s_mover < 0) {
    if (segurando_ms < HOME_SEGURAR_MS || desvio > ROLAGEM_ARRASTE_MIN) { return false; }
    const int pos = home_pos_em(a, tx, ty, false);
    if (pos < 0) { return false; }
    s_mover = pos;
    s_mover_mudou = false;
    s_borda_desde = 0;
    s_sujo_layout = true;
    ESP_LOGI(TAG, "moving '%s' (position %d)", sistema_home_app(pos)->nome, pos + 1);
    return true;
  }

  // On the remove strip the icon does not change place: it only lights the strip.
  const bool sobre = ty < a.y + HOME_REMOVER_A && sistema_apps_de_fora_removivel(sistema_home_app(s_mover));
  if (sobre != s_sobre_remover) { s_sobre_remover = sobre; s_sujo_layout = true; }
  if (sobre) { s_borda_desde = 0; return true; }

  const int paginas = home_paginas(a);
  const int lado = (tx < a.x + HOME_BORDA_PX && s_pagina_home > 0) ? -1
                 : (tx >= a.x + a.largura - HOME_BORDA_PX && s_pagina_home < paginas - 1) ? +1 : 0;
  if (lado) {
    if (!s_borda_desde) { s_borda_desde = agora; return true; }
    if (agora - s_borda_desde < HOME_BORDA_MS) { return true; }
    s_borda_desde = agora;   // holding there, the next page comes after another timeout
    const int pp = home_por_pagina(a);
    s_pagina_home += lado;
    // Enters from the edge it came from: going right, the first slot.
    int para = (lado > 0) ? s_pagina_home * pp : (s_pagina_home + 1) * pp - 1;
    if (para >= home_n()) { para = home_n() - 1; }
    ordem_mover(s_mover, para);
    s_mover = para;
    s_mover_mudou = true;
    s_sujo_layout = true;
    return true;
  }
  s_borda_desde = 0;

  const int pos = home_pos_em(a, tx, ty, true);
  if (pos >= 0 && pos != s_mover) {
    ordem_mover(s_mover, pos);
    s_mover = pos;
    s_mover_mudou = true;
    s_sujo_layout = true;
  }
  return true;
}

bool home_soltar(void)
{
  if (s_mover < 0) { return false; }
  if (s_sobre_remover) {
    s_remover_app = sistema_home_app(s_mover);
    s_sobre_remover = false;
  }
  if (s_mover_mudou) {
    ESP_LOGI(TAG, "'%s' now at position %d", sistema_home_app(s_mover)->nome, s_mover + 1);
    ordem_gravar();
  }
  s_mover = -1;
  s_sujo_layout = true;
  return true;
}

// Page indicator: dots centred in the strip above the navigation bar.
// The current page is a 28x8 pill; the others, 8x8 dots.
static void desenhar_bolinhas(lgfx::LovyanGFX& d, const area_t& a)
{
  const int n = home_paginas(a);
  if (n < 2) { return; }
  const int atual_l = 28, ponto_l = 8, alt = 8, vao = 8;
  const int total = atual_l + (n - 1) * (ponto_l + vao);
  int x = a.x + a.largura / 2 - total / 2;
  const int y = a.y + a.altura - HOME_BOLINHAS_A / 2 - alt / 2;
  for (int i = 0; i < n; ++i) {
    const bool atual = (i == s_pagina_home);
    const int l = atual ? atual_l : ponto_l;
    d.fillRoundRect(x, y, l, alt, alt / 2, atual ? TEMA_TEXTO : TEMA_LINHA);
    x += l + vao;
  }
}

// ---------------------------------------------------------------------------
// CLOCK CARD (top of the home screen)
// ---------------------------------------------------------------------------
// On the left the time and the date written out; on the right two pills with the
// Wi-Fi and Mesh state. Everything comes from existing services (tab5_hora,
// tab5_wifi, tab5_mesh): here we only read and draw.
static const int PILULA_A = 40;   // height of each pill

static char s_cartao_chave[96] = "";   // what the card shows now (see home_cartao_atualizar)

// The texts of the two pills. 'mesh' is empty when the service is not active
// (the pill disappears). 'wifi_ok' says whether the dot is green.
static void cartao_textos(char* wifi, size_t nw, bool* wifi_ok, char* mesh, size_t nm)
{
  *wifi_ok = (tab5_wifi_estado() == TAB5_WIFI_CONECTADO) && tab5_wifi_ssid() && tab5_wifi_ssid()[0];
  if (*wifi_ok) { snprintf(wifi, nw, "Wi-Fi %s", tab5_wifi_ssid()); }
  else          { snprintf(wifi, nw, "%s", tr("Wi-Fi desligado")); }
  if (tab5_mesh_servico_ativo()) { snprintf(mesh, nm, tr("Mesh · %d nós"), tab5_mesh_num_nos()); }
  else                           { mesh[0] = 0; }
}

// A pill flush right (x_dir), with the coloured dot and the text
// cut to fit in 'larg_max'.
static void desenhar_pilula(lgfx::LovyanGFX& d, int x_dir, int y, int larg_max, uint16_t cor_ponto, const char* txt)
{
  const int pad = 16, ponto = 12, espaco = 10;
  char t[64];
  snprintf(t, sizeof(t), "%s", txt);
  d.setFont(tab5_fonte_suave(false, ROTULO_LETRA));
  d.setTextSize(1);
  tab5_cortar_ao_caber(d, t, larg_max - 2 * pad - ponto - espaco);
  const int l = 2 * pad + ponto + espaco + (int)d.textWidth(t);
  const int x = x_dir - l;
  d.fillRoundRect(x, y, l, PILULA_A, PILULA_A / 2, TEMA_SUPERFICIE_ALTA);
  d.fillCircle(x + pad + ponto / 2, y + PILULA_A / 2, ponto / 2, cor_ponto);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE_ALTA);
  d.drawString(t, x + pad + ponto + espaco, y + PILULA_A / 2);
}

// The key sums up what the card shows: if it has not changed, there is nothing to repaint.
static void cartao_chave(char* out, size_t n)
{
  char hora[8], wifi[48], mesh[48];
  bool ok;
  hora_agora(hora, sizeof(hora));
  cartao_textos(wifi, sizeof(wifi), &ok, mesh, sizeof(mesh));
  struct tm lt = {};
  const uint32_t t = tab5_hora_epoch();
  if (t) { tab5_hora_local(t, &lt); }
  snprintf(out, n, "%s|%d|%s|%s|%d", hora, lt.tm_mday, wifi, mesh, idioma_atual());
}

static void desenhar_cartao_relogio(lgfx::LovyanGFX& d, const area_t& a)
{
  const int cx = a.x + TEMA_MARGEM, cy = a.y + CARTAO_Y, cl = a.largura - 2 * TEMA_MARGEM;
  d.fillRoundRect(cx, cy, cl, CARTAO_A, TEMA_RAIO_G, TEMA_SUPERFICIE);

  // Time and date. Without the time set: "--:--" and no date.
  char hora[8], data[48] = "";
  hora_agora(hora, sizeof(hora));
  const uint32_t t = tab5_hora_epoch();
  if (t) {
    struct tm lt;
    tab5_hora_local(t, &lt);
    idioma_data_por_extenso(&lt, data, sizeof(data));
  }
  d.setTextSize(1);
  d.setTextDatum(top_left);
  d.setFont(tab5_fonte_suave(false, TEMA_LETRA_GRANDE));
  d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
  d.drawString(hora, cx + CARTAO_PAD, cy + CARTAO_PAD);
  d.setFont(tab5_fonte_suave(false, TEMA_LETRA_CORPO));
  d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
  d.drawString(data, cx + CARTAO_PAD, cy + CARTAO_PAD + TEMA_LETRA_GRANDE + 4);
  const int larg_esq = (int)d.textWidth(data);

  // The pills, in what is left to the right of the text, centred on the card's height.
  char wifi[48], mesh[48];
  bool wifi_ok;
  cartao_textos(wifi, sizeof(wifi), &wifi_ok, mesh, sizeof(mesh));
  const int n = mesh[0] ? 2 : 1;
  const int x_dir = cx + cl - CARTAO_PAD;
  const int livre = x_dir - (cx + CARTAO_PAD + larg_esq + TEMA_ESPACO_5);
  int y = cy + (CARTAO_A - (n * PILULA_A + (n - 1) * TEMA_ESPACO_2)) / 2;
  desenhar_pilula(d, x_dir, y, livre, wifi_ok ? TEMA_SUCESSO : TEMA_TEXTO_3, wifi);
  if (mesh[0]) { desenhar_pilula(d, x_dir, y + PILULA_A + TEMA_ESPACO_2, livre, TEMA_DESTAQUE, mesh); }

  tab5_fonte_corpo(d);
  cartao_chave(s_cartao_chave, sizeof(s_cartao_chave));
}

void desenhar_home(lgfx::LovyanGFX& d, const area_t& a)
{
  // WALLPAPER (Settings > Display). Only in this pass - the home screen is static and
  // is only drawn whole, with the area freshly cleared. The seed is taken once
  // per boot - and again when the user picks "Sorteio" (Random) again -,
  // so the drawing does not change every time you return to the home screen.
  static uint32_t s_semente_parede = 0;
  static uint8_t  s_parede_ant = 0xFF;
  const uint8_t parede = ajustes_parede();
  if (parede == AJUSTES_PAREDE_SORTEIO && s_parede_ant != AJUSTES_PAREDE_SORTEIO) { s_semente_parede = 0; }
  s_parede_ant = parede;
  while (!s_semente_parede) { s_semente_parede = esp_random(); }
  if (parede != AJUSTES_PAREDE_NENHUM) {
    const int estilo = (parede == AJUSTES_PAREDE_SORTEIO)
                     ? (int)(s_semente_parede % AJUSTES_PAREDE_ESTILOS) : parede - 1;
    bq_fundo_parede(d, a, estilo, s_semente_parede);
  } else {
    // No wallpaper: the theme background (in bands, see tab5_tela_limpar).
    tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
  }

  desenhar_cartao_relogio(d, a);

  const int por_pagina = home_por_pagina(a);
  if (s_pagina_home >= home_paginas(a)) { s_pagina_home = 0; }
  const int primeiro = s_pagina_home * por_pagina;
  // Width the label may use: the icon and the gap to the neighbour, with
  // 8 px of slack so two names side by side do not touch.
  const int largura_disponivel = ICONE_LADO + vao_colunas(a) - 8;
  for (int k = 0; k < por_pagina && primeiro + k < home_n(); ++k) {
    int px, py;
    celula_do_icone(k, a, &px, &py);

    // BREATHING ROOM BETWEEN ICONS: each icon's drawing belongs to the app (app_t.icone); the
    // pause between them, if needed, belongs to the system loop - whoever writes an app
    // draws ONE icon and does not need to know about this.
    const app_t* app = sistema_home_app(primeiro + k);
    // THE LIFTED ICON (hold and drag): a frame around it, in the gap between
    // icons - the icon drawing itself does not change.
    if (primeiro + k == s_mover) {
      for (int e = 0; e < 4; ++e) {
        d.drawRoundRect(px - 8 + e, py - 8 + e, ICONE_LADO + 16 - 2 * e, ICONE_LADO + 16 - 2 * e,
                        TEMA_RAIO_ICONE + 8 - e, TEMA_TEXTO);
      }
    }
    if (app->icone) {
      app->icone(d, px, py, ICONE_LADO);
    } else {
      // App without its own icon: a square in the app's colour with its initial.
      d.fillRoundRect(px, py, ICONE_LADO, ICONE_LADO, TEMA_RAIO_ICONE, app->cor);
      char inicial[2] = { app->nome[0], 0 };
      d.setFont(tab5_fonte_suave(true, TEMA_LETRA_GRANDE));
      d.setTextSize(1);
      d.setTextDatum(middle_center);
      d.setTextColor(TEMA_SOBRE_DESTAQUE, app->cor);
      d.drawString(inicial, px + ICONE_LADO / 2, py + ICONE_LADO / 2);
    }

    // APP NAME: regular Roboto, centred below the icon. NO BACKGROUND COLOUR:
    // with it the name came out on a band over the wallpaper.
    // WIDTH ACTUALLY MEASURED: if it does not fit, the lettering drops 2 px at a time (down
    // to ROTULO_LETRA_MIN) in native heights - better than cutting the name, which
    // would stop identifying the app.
    int alt_nome = ROTULO_LETRA;
    d.setFont(tab5_fonte_suave(false, alt_nome));
    d.setTextSize(1);
    d.setTextDatum(top_center);
    d.setTextColor(TEMA_TEXTO);
    while ((int)d.textWidth(tr(app->nome)) > largura_disponivel && alt_nome > ROTULO_LETRA_MIN) {
      alt_nome -= 2;
      d.setFont(tab5_fonte_suave(false, alt_nome));
    }
    d.drawString(tr(app->nome), px + ICONE_LADO / 2, py + ICONE_LADO + ROTULO_ESPACO);
    tab5_fonte_corpo(d);
  }
  desenhar_bolinhas(d, a);

  // THE REMOVE STRIP, with a third-party app lifted.
  if (s_mover >= 0 && sistema_apps_de_fora_removivel(sistema_home_app(s_mover))) {
    d.fillRoundRect(a.x + TEMA_MARGEM, a.y + 8, a.largura - 2 * TEMA_MARGEM, HOME_REMOVER_A - 16, TEMA_RAIO_G,
                    s_sobre_remover ? TEMA_PERIGO : TEMA_SUPERFICIE_ALTA);
    tab5_titulo(d, 30);
    d.setTextDatum(middle_center);
    d.setTextColor(s_sobre_remover ? TEMA_SOBRE_DESTAQUE : TEMA_PERIGO);
    d.drawString(s_sobre_remover ? tr("Solte para remover") : tr("Arraste aqui para remover"),
                 a.x + a.largura / 2, a.y + HOME_REMOVER_A / 2);
    tab5_fonte_corpo(d);
  }
  // THE CONFIRMATION, after dropping on the strip.
  if (s_remover_app) {
    const int bl = a.largura - 80, bh = 330, bx = a.x + 40, by = a.y + (a.altura - bh) / 2;
    d.fillRoundRect(bx, by, bl, bh, TEMA_RAIO_G, TEMA_SUPERFICIE);
    d.drawRoundRect(bx, by, bl, bh, TEMA_RAIO_G, TEMA_LINHA);
    tab5_titulo(d, 34);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
    char t[64];
    snprintf(t, sizeof(t), tr("Remover \"%s\"?"), tr(s_remover_app->nome));
    d.drawString(t, bx + 30, by + 34);
    d.setFont(tab5_fonte_suave(false, 26));
    d.setTextSize(1);
    d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
    d.drawString(tr("O app e os dados dele vão para a"), bx + 30, by + 100);
    d.drawString(tr("lixeira do cartão (Arquivos recupera)."), bx + 30, by + 136);
    const int bw = (bl - 90) / 2;
    s_rem_bt_nao = { bx + 30, by + bh - 110, bw, 80 };
    s_rem_bt_sim = { bx + 60 + bw, by + bh - 110, bw, 80 };
    tab5_botao(d, s_rem_bt_nao.x, s_rem_bt_nao.y, bw, 80, tr("Cancelar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    tab5_botao(d, s_rem_bt_sim.x, s_rem_bt_sim.y, bw, 80, tr("Remover"), TEMA_PERIGO, TEMA_SOBRE_DESTAQUE);
    tab5_fonte_corpo(d);
  }
}

// Called by the main loop on the status bar tick: if the time (minute),
// the Wi-Fi or the Mesh changed since the last paint, repaints ONLY the clock
// card. Does nothing outside a clean home screen (app open, lock screen, notification
// centre, icon lifted or confirmation on screen).
void home_cartao_atualizar(void)
{
  if (s_app || s_bloqueado || s_painel_avisos || s_mover >= 0 || s_remover_app) { return; }
  char chave[sizeof(s_cartao_chave)];
  cartao_chave(chave, sizeof(chave));
  if (strcmp(chave, s_cartao_chave) == 0) { return; }
  tela_virtual().startWrite();
  desenhar_cartao_relogio(tela_virtual(), sistema_area());
  tela_virtual().endWrite();
}

// Touch with the remove confirmation on screen: Remover removes; anywhere
// else cancels.
void home_remover_tocar(int tx, int ty)
{
  const app_t* app = s_remover_app;
  s_remover_app = nullptr;
  s_sujo_layout = true;
  if (tab5_dentro(tx, ty, s_rem_bt_sim.x, s_rem_bt_sim.y, s_rem_bt_sim.largura, s_rem_bt_sim.altura)) {
    ESP_LOGI(TAG, "removing '%s'", app->nome);
    sistema_apps_de_fora_remover(app);
  }
}
