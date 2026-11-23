// APP: Ajustes (Settings) - the system preferences.
//
// NEW LAYOUT, the one every phone uses:
//
//   - the start screen is ONE scrollable list in GROUPS (CONNECTIONS, SCREEN AND
//     SOUND, SYSTEM), each group in a rounded card, as in the approved mock-up
//     (the LINHAS table);
//   - page rows show an icon, a name and a status line
//     ("Connected to ...", "Off") so you know without going in;
//   - BRIGHTNESS and VOLUME are rows of the same list, with the slider inside
//     (tap the track or drag; a finger that lands on it does not scroll the list);
//   - each page has VOLTAR (back) and its options. The list scrolls (kernel/lista):
//     a new option is ONE row in the PAGINAS table, not a positional patch.
//
// The previous version stacked every section in an 1800 px column that the
// system scrolled; each new section pushed the others down, and the heights
// were numbers added up by hand (and got it wrong more than once).
//
// LANDSCAPE (physical keyboard docked, kernel/orientacao) is the same scheme
// with one more column, master-detail as on tablets: the list on the left
// (LADO_L) and, on the right, the chosen page, which scrolls
// on its own (s_rol_pag) - scrolling the page does not carry the list away. The
// pages are the same as in portrait: they draw in the area they are given and
// handle touch relative to it.
//
// What lives here belongs to the USER, not the app: each app still declares
// the hardware it needs in its own profile; this app controls what cuts
// across all of them (ajustes.h).

//
// SPLIT BY TOPIC (it only moved between files, no logic changed). This file
// kept the measurements, the pages (the PAGINAS table), the screen state, the
// texts and buttons, each page's status line, the start screen (brightness,
// volume and the list), the dispatch and the app registration. The rest went
// to app_config_*.cpp; what the parts share is in app_config_interno.hpp:
//   app_config_icones.cpp   list icons and the app's icon on the home screen
//   app_config_rede.cpp     Wi-Fi, Bluetooth, ESP-NOW, Meshtastic and MQTT pages
//   app_config_paginas.cpp  Date and time, PC access, Language, Screen, Keyboard, Grove, About and USB pages

#include "tab5_hora.hpp"
#include "tab5_fuso.hpp"
#include "app.h"
#include "tab5_power.hpp"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_wifi.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_ble.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_teclado.hpp"
#include "tab5_grove.hpp"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "pintura.hpp"
#include "lista.hpp"
#include "conversa.hpp"
#include "teclado_virtual.hpp"
#include "tab5_pendrive.hpp"
#include "tab5_web.hpp"
#include <stdlib.h>
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>
#include "app_config_interno.hpp"

// ---------------------------------------------------------------------------
// MEASUREMENTS
// ---------------------------------------------------------------------------
static const int PM_L     = 88;       // width of the - and + buttons
static const int TRILHO_A  = 12;       // track thickness
static const int BOTAO_R   = 16;       // radius of the slider knob (32 px)
static const int TOQUE_EXTRA = 16;     // touch slack beyond the ends of the track (vertically the whole row counts)
// Heights of the start screen rows (touch and the list use the same ones).
static const int GRUPO_A   = 56;       // group label (CONEXOES...), bottom-aligned
static const int ITEM_A    = 92;       // page row
static const int DESL_A    = 100;      // slider row: name and % on top, track below
static const int ICONE_L   = 64;

// Landscape (master-detail, see the top of the file)
static const int LADO_L      = 400;   // the list, on the left
static const int PAG_X       = LADO_L + 1;   // where the page starts (landscape)

// Pages
static const int VOLTAR_L = 200, VOLTAR_A = 64;

// ---------------------------------------------------------------------------
// THE PAGES - the list below comes from here
// ---------------------------------------------------------------------------
typedef enum {
  PAG_INICIO = -1,
  PAG_WIFI = 0, PAG_BLUETOOTH, PAG_ESPNOW, PAG_MESH, PAG_MQTT, PAG_TELA, PAG_ENERGIA, PAG_HORA, PAG_IDIOMA, PAG_TECLADO, PAG_GROVE, PAG_USB, PAG_PC, PAG_SOBRE,
  PAG_N
} pagina_t;

typedef void (*icone_fn)(lgfx::LovyanGFX& d, int x, int y, int l);
typedef void (*estado_fn)(char* out, size_t tam);
static void est_energia(char*, size_t);
static void est_mesh(char*, size_t);
static void est_tela(char*, size_t);
static void est_usb(char*, size_t);
static void est_sobre(char*, size_t);
static void est_mqtt(char*, size_t);
static void est_hora(char*, size_t);
static void est_idioma(char*, size_t);
static void est_pc(char*, size_t);

// Each page's drawing and touch handling (defined further down).
typedef void (*desenhar_fn)(lgfx::LovyanGFX& d, const area_t& a, bool tudo);
typedef void (*tocar_fn)(int tx, int ty);

// ONE ROW PER PAGE, with everything about it: the list item (name, icon,
// status) and the open page (drawing and touch). There used to be three
// parallel tables, and when Energia was added to only this one, the drawing
// and touch of the following ones shifted by one slot: tapping Date and time
// opened Language (04/11/2026). No drawing = the row opens something else
// (abrir_pagina).
typedef struct {
  const char* nome;
  icone_fn    icone;
  estado_fn   estado;
  desenhar_fn desenhar;
  tocar_fn    tocar;      // nullptr = read-only page
} pagina_def_t;
static const pagina_def_t PAGINAS[] = {
  { N_("Wi-Fi"),          ic_wifi,    est_wifi,    pag_wifi,    toque_wifi   },
  { N_("Bluetooth"),      ic_bt,      est_bt,      pag_bt,      toque_bt     },
  { N_("ESP-NOW"),        ic_espnow,  est_espnow,  pag_espnow,  toque_espnow },
  { N_("Meshtastic"),     ic_mesh,    est_mesh,    pag_mesh,    toque_mesh   },
  { N_("Malha pela internet"), ic_nuvem, est_mqtt, pag_mqtt,    toque_mqtt   },
  { N_("Tela"),           ic_tela,    est_tela,    pag_tela,    toque_tela   },
  { N_("Energia"),        ic_energia, est_energia, nullptr,     nullptr      },   // opens the Energia app (not on the home screen)
  { N_("Data e hora"),    ic_hora,    est_hora,    pag_hora,    toque_hora   },
  { N_("Idioma"),         ic_idioma,  est_idioma,  pag_idioma,  toque_idioma },
  { N_("Teclado"),        ic_teclado, est_teclado, pag_teclado, nullptr      },
  { N_("Grove 5V"),       ic_grove,   est_grove,   pag_grove,   toque_grove  },
  { N_("USB (pendrive)"), ic_usb,     est_usb,     pag_usb,     toque_usb    },
  { N_("Acesso pelo PC"), ic_pc,      est_pc,      pag_pc,      toque_pc     },
  { N_("Sobre"),          ic_sobre,   est_sobre,   pag_sobre,   nullptr      },
};
static_assert(sizeof(PAGINAS) / sizeof(PAGINAS[0]) == PAG_N, "uma linha em PAGINAS para cada pagina_t, na mesma ordem");

// ---------------------------------------------------------------------------
// THE START SCREEN - the order and the groups come from here
// ---------------------------------------------------------------------------
// Three kinds of row: a GROUP label; a PAGE (value = the pagina_t,
// name/icon/status come from PAGINAS); a SLIDER (value 0 = brightness,
// 1 = volume; the name is the label). Each run of rows between two groups is
// ONE card. PAGINAS follows the enum order (the rest of the app depends on
// it); the on-screen order is this table alone.
typedef enum { L_GRUPO, L_PAGINA, L_DESLIZANTE } linha_tipo_t;
typedef struct { linha_tipo_t tipo; int valor; const char* rotulo; } linha_t;
static constexpr linha_t LINHAS[] = {
  { L_GRUPO,      0,             N_("CONEXÕES")   },
  { L_PAGINA,     PAG_WIFI,      nullptr          },
  { L_PAGINA,     PAG_BLUETOOTH, nullptr          },
  { L_PAGINA,     PAG_ESPNOW,    nullptr          },
  { L_PAGINA,     PAG_MESH,      nullptr          },
  { L_PAGINA,     PAG_MQTT,      nullptr          },
  { L_GRUPO,      0,             N_("TELA E SOM") },
  { L_DESLIZANTE, 0,             N_("Brilho")     },
  { L_DESLIZANTE, 1,             N_("Volume")     },
  { L_PAGINA,     PAG_TELA,      nullptr          },
  { L_GRUPO,      0,             N_("SISTEMA")    },
  { L_PAGINA,     PAG_ENERGIA,   nullptr          },
  { L_PAGINA,     PAG_HORA,      nullptr          },
  { L_PAGINA,     PAG_IDIOMA,    nullptr          },
  { L_PAGINA,     PAG_TECLADO,   nullptr          },
  { L_PAGINA,     PAG_GROVE,     nullptr          },
  { L_PAGINA,     PAG_USB,       nullptr          },
  { L_PAGINA,     PAG_PC,        nullptr          },
  { L_PAGINA,     PAG_SOBRE,     nullptr          },
};
static constexpr int LINHAS_N = sizeof(LINHAS) / sizeof(LINHAS[0]);

// Every page appears once (forgetting one in the table above does not compile).
static constexpr int linhas_de_pagina(void)
{
  int n = 0;
  for (int i = 0; i < LINHAS_N; ++i) { if (LINHAS[i].tipo == L_PAGINA) { ++n; } }
  return n;
}
static_assert(linhas_de_pagina() == PAG_N, "toda pagina_t tem uma linha de pagina em LINHAS");

// The start screen row that shows page p (-1 = none).
static int linha_da_pagina(int p)
{
  for (int i = 0; i < LINHAS_N; ++i) { if (LINHAS[i].tipo == L_PAGINA && LINHAS[i].valor == p) { return i; } }
  return -1;
}
// The row of slider k (0 = brightness, 1 = volume).
static int linha_do_deslizante(int k)
{
  for (int i = 0; i < LINHAS_N; ++i) { if (LINHAS[i].tipo == L_DESLIZANTE && LINHAS[i].valor == k) { return i; } }
  return -1;
}
// Is the row the first / last of its card? (the card runs from one group to the next)
static bool abre_cartao(int i)  { return i == 0 || LINHAS[i - 1].tipo == L_GRUPO; }
static bool fecha_cartao(int i) { return i + 1 == LINHAS_N || LINHAS[i + 1].tipo == L_GRUPO; }

// ---------------------------------------------------------------------------
// SCREEN STATE
// ---------------------------------------------------------------------------
pintura_t s_cfg_p;
static int       s_pag = PAG_INICIO;
static lista_t   s_lista;
static char      s_sub[PAG_N][48];     // each PAGE's status line, as drawn
static int       s_ub = -1, s_uv = -1; // brightness/volume as drawn (to notice changes coming from elsewhere)
char      s_assin[512];         // "signature" of the open page: if it changed, redraw
char      s_aviso[96];          // short message on a page ("escolhido para o Mesh")

// Bluetooth: the list of devices seen
lista_t   s_l_bt;
uint32_t  s_bt_ver = 0;

// Meshtastic: the PIN editor
bool  s_pin_editando = false;
// The system's virtual keyboard - only one, for the Wi-Fi password and the
// PIN (never open at the same time: changing page closes it).
tecvirt_t s_cfg_kb;

static void abrir_pagina(int p);

// LANDSCAPE: where the page starts (relative to the app area; 0 in portrait),
// its scroll and the height of what it draws - measured by the helpers
// (s_fim, the bottom in screen coordinates of the last element) on the full pass.
int       s_dx_pag = 0;
int       s_dy_pag = 0;         // landscape: where the page starts vertically (currently the top of the area, 0)
rolagem_t s_rol_pag;
static int       s_pag_h = 0;
int       s_fim = 0;

// THE LIST INSIDE A PAGE (networks, devices), starting at 'y0'. In portrait
// the page is fixed and the list scrolls inside it; in landscape the WHOLE
// PAGE scrolls (the column is too short for buttons and list together), so
// the list goes in with the height of all its items and counts towards the
// end of the page.
area_t janela_da_lista(const area_t& a, int y0, int n, int item_a)
{
  if (!deitado(sistema_area())) { return { a.x, a.y + y0, a.largura, a.altura - y0 }; }
  const int h = n > 0 ? n * item_a : item_a;
  fim(a.y + y0 + h);
  return { a.x, a.y + y0, a.largura, h };
}

// The page area, on screen: the app's in portrait; in landscape, the right-hand column.
area_t area_pagina(void)
{
  area_t a = sistema_area();
  if (deitado(a)) { a.x += PAG_X; a.largura -= PAG_X; }
  return a;
}

// ---------------------------------------------------------------------------
// TEXT AND BUTTONS
// ---------------------------------------------------------------------------

// Paragraph wrapped to the width - the same wrapper as the chat bubbles.
int paragrafo(lgfx::LovyanGFX& d, int x, int y, int largura, const char* s)
{
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextSize(1);
  char linhas[6][CONVERSA_LINHA_BYTES];
  const int n = conversa_quebrar_linhas(d, s, largura, linhas, 6);
  d.setTextDatum(top_left);
  d.setTextColor(cor_sub(d), TEMA_FUNDO);
  for (int k = 0; k < n; ++k) { d.drawString(linhas[k], x, y + k * 32); }
  tab5_fonte_corpo(d);
  fim(y + n * 32);
  return y + n * 32;
}


// Drawn ">" chevron (the font would have a different weight at each size).
void chevron(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  for (int k = 0; k < 3; ++k) {
    d.drawLine(cx - 6 + k, cy - 12, cx + 6 + k, cy, cor);
    d.drawLine(cx + 6 + k, cy, cx - 6 + k, cy + 12, cor);
  }
}

// Page header: VOLTAR (back) and the title.
static void cabecalho(lgfx::LovyanGFX& d, const area_t& a, const char* titulo)
{
  // In landscape the list is always on the left: no VOLTAR, just the title.
  const bool voltar = !deitado(sistema_area());
  if (voltar) { tab5_botao(d, a.x + 16, a.y + 16, VOLTAR_L, VOLTAR_A, tr("< VOLTAR"), TEMA_DESTAQUE); }
  tab5_titulo(d, TEMA_LETRA_TITULO);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(titulo, a.x + (voltar ? 16 + VOLTAR_L + 28 : MARGEM), a.y + 16 + VOLTAR_A / 2);
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
  d.drawFastHLine(a.x, a.y + CONTEUDO_Y - 16, a.largura, cor_linha(d));
}

// Info row: small label on top, value below. 76 px.
int info(lgfx::LovyanGFX& d, const area_t& a, int y, const char* rotulo,
         const char* valor, uint16_t cor)
{
  tab5_escrever(d, rotulo, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  tab5_escrever(d, valor, a.x + MARGEM, a.y + y + 28, 32, false, cor, TEMA_FUNDO);
  fim(a.y + y + 84);
  return y + 84;
}

// Page-width button.
int botao_largo(lgfx::LovyanGFX& d, const area_t& a, int y, const char* txt, uint16_t cor)
{
  tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, txt, cor);
  fim(a.y + y + BTN_A);
  return y + BTN_A + 20;
}

// - / value / + pair (screen timeout, clock, channel).
static const int PM_VALOR_L = 260;
void menos_mais(lgfx::LovyanGFX& d, const area_t& a, int y, const char* valor)
{
  const int x = a.x + MARGEM;
  tab5_botao(d, x, a.y + y, PM_L + 40, BTN_A, "-", TEMA_ALERTA);
  tab5_escrever(d, valor, x + PM_L + 40 + PM_VALOR_L / 2 + 20, a.y + y + BTN_A / 2, 34, true, TEMA_TEXTO, TEMA_FUNDO, middle_center);
  tab5_botao(d, x + PM_L + 40 + PM_VALOR_L + 40, a.y + y, PM_L + 40, BTN_A, "+", TEMA_SUCESSO);
  fim(a.y + y + BTN_A);
}
int menos_mais_toque(int tx, int ty, int y)
{
  if (tab5_dentro(tx, ty, MARGEM, y, PM_L + 40, BTN_A)) { return -1; }
  if (tab5_dentro(tx, ty, MARGEM + PM_L + 40 + PM_VALOR_L + 40, y, PM_L + 40, BTN_A)) { return +1; }
  return 0;
}

static void est_mqtt(char* o, size_t t)
{
  snprintf(o, t, "%s", mesh_mqtt_servidor_nome(ajustes_mqtt_servidor()));
}

void fuso_texto(int16_t m, char* o, size_t t)
{
  snprintf(o, t, "UTC%c%d:%02d", m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
}

static void est_hora(char* o, size_t t)
{
  char f[40];
  const uint32_t e = tab5_hora_epoch();
  tab5_fuso_texto(e ? e : (uint32_t)time(nullptr), f, sizeof(f));
  if (!e) { snprintf(o, t, tr("Fuso %s  ·  relógio sem acerto"), f); return; }
  struct tm tm_;
  tab5_hora_local(e, &tm_);
  char hm[8];
  tab5_hora_hhmm(&tm_, hm, sizeof(hm));
  snprintf(o, t, tr("%s  ·  fuso %s"), hm, f);
}

static void est_idioma(char* o, size_t t)
{
  snprintf(o, t, "%s", idioma_nome(idioma_atual()));
}

void est_wifi(char* o, size_t t)
{
  const tab5_wifi_estado_t e = tab5_wifi_estado();
  if (e == TAB5_WIFI_CONECTADO)           { snprintf(o, t, tr("Conectado a %s"), tab5_wifi_ssid()); }
  else if (e == TAB5_WIFI_SEM_CREDENCIAL) { snprintf(o, t, tr("Nenhuma rede guardada")); }
  else if (e == TAB5_WIFI_C6_MUDO)        { snprintf(o, t, tr("O C6 não responde")); }
  else if (e == TAB5_WIFI_FALHOU)         { snprintf(o, t, tr("Não conectou")); }
  else if (e == TAB5_WIFI_DESLIGADO)      { snprintf(o, t, ajustes_wifi() ? tr("Solto do roteador") : tr("Desligado")); }
  else                                    { snprintf(o, t, tr("Conectando...")); }
}

void est_bt(char* o, size_t t)
{
  const tab5_ble_estado_t e = tab5_ble_estado();
  if (e == TAB5_BLE_PRONTO)       { snprintf(o, t, tr("Ligado")); }
  else if (e == TAB5_BLE_SUBINDO) { snprintf(o, t, tr("Ligando...")); }
  // C6 with the factory firmware (Wi-Fi works, Bluetooth does not): it is not
  // a fault, it is an optional update - the text has to say so.
  else if (e == TAB5_BLE_FALHOU && tab5_wifi_c6_enlace() && !tab5_wifi_c6_nosso())
                                  { snprintf(o, t, tr("Rádio C6 de fábrica: atualização opcional")); }
  else if (e == TAB5_BLE_FALHOU)  { snprintf(o, t, tr("Falhou - o C6 não deu o Bluetooth")); }
  else if (e == TAB5_BLE_DESLIGANDO) { snprintf(o, t, tr("Desligando...")); }
  else                            { snprintf(o, t, tr("Desligado")); }
}

void est_espnow(char* o, size_t t)
{
  if (!tab5_espnow_c6_compativel()) {
    // Without a link yet there is no way to know which firmware the C6 has.
    snprintf(o, t, tab5_wifi_c6_enlace() ? tr("Rádio C6 de fábrica: atualização opcional")
                                         : tr("Ligue o Wi-Fi"));
    return;
  }
  const uint8_t f = tab5_espnow_canal_fixo();
  if (f) { snprintf(o, t, tr("Canal fixo %u (sem roteador)"), (unsigned)f); }
  else   { snprintf(o, t, tr("Canal do roteador (%u)"), (unsigned)tab5_espnow_canal()); }
}

static const char* nome_via_mesh(uint8_t via)
{
  return via == AJUSTES_MESH_BLUETOOTH ? tr("Bluetooth") : via == AJUSTES_MESH_INTERNET ? tr("Internet") : tr("Cabo Grove");
}

static void est_mesh(char* o, size_t t)
{
  snprintf(o, t, "%s - %s", nome_via_mesh(ajustes_mesh_via()), tr(tab5_mesh_enlace_descricao()));
}

static void est_tela(char* o, size_t t)
{
  snprintf(o, t, tr("Apaga em %u s  ·  painel a %d MHz"), (unsigned)ajustes_tela_s(),
           tab5_power_relogio_dpi());
}

void est_teclado(char* o, size_t t)
{
  snprintf(o, t, "%s", tab5_teclado_presente() ? tr("Encaixado") : tr("Nenhum no ExtPort1"));
}

static void est_usb(char* o, size_t t)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  if (e.fase == PENDRIVE_ATIVO)           { snprintf(o, t, tr("Cartão no computador")); }
  else if (e.fase != PENDRIVE_PARADO)     { snprintf(o, t, tr("Pendrive: aguarde...")); }
  else if (e.oferta)                      { snprintf(o, t, tr("Computador conectado")); }
  else if (ajustes_pendrive())            { snprintf(o, t, tr("Liga sozinho no computador")); }
  else                                    { snprintf(o, t, tr("Pendrive desligado")); }
}

static void est_pc(char* o, size_t t)
{
  const char* abertos[WEB_PARTES_MAX];
  const int n = tab5_web_abertos(abertos, WEB_PARTES_MAX);
  if (!n) { snprintf(o, t, "%s", tr("Desligado")); return; }
  snprintf(o, t, n == 1 ? tr("%d app aberto no PC") : tr("%d apps abertos no PC"), n);
}

void est_grove(char* o, size_t t)
{
  const int dono = (int)tab5_grove_dono();
  if (sistema_grove_sondando())                 { snprintf(o, t, tr("Sondando o cabo...")); }
  else if (dono != (int)GROVE_DONO_NINGUEM)     { snprintf(o, t, tr("Em uso: %s"), tr(grove_dono_nome((grove_dono_t)dono))); }
  else if (ajustes_grove())                     { snprintf(o, t, tr("Ligado - %s"), tr(grove_nome(tab5_grove_tipo()))); }
  else                                          { snprintf(o, t, tr("Desligado")); }
}

// ENERGIA LIVES HERE: the row shows the battery right now and opens the
// Energia app (the charts need the whole screen), which no longer has an
// icon on the home screen.
const app_t* app_energia(void)
{
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) { if (!strcmp(SISTEMA_APPS[i]->nome, "Energia")) { return SISTEMA_APPS[i]; } }
  return nullptr;
}

static void est_energia(char* o, size_t t)
{
  const tab5_medida_t m = tab5_power_ultima();
  if (m.pct < 0) { snprintf(o, t, tr("Bateria e consumo")); return; }
  snprintf(o, t, m.mA >= 0 ? tr("%d%% - carregando") : tr("%d%% - gastando %.0f mA"), m.pct, (double)-m.mA);
}

static void est_sobre(char* o, size_t t)
{
  snprintf(o, t, "M5tabOS %s", esp_app_get_description()->version);
}

// ---------------------------------------------------------------------------
// START SCREEN: the list of groups (with BRIGHTNESS and VOLUME)
// ---------------------------------------------------------------------------
// BRIGHTNESS ON THE EYE'S SCALE. The PWM value is linear in light, but the eye
// is not: 4% duty already looks like "a quarter of the brightness". With a
// linear bar almost all of the travel was in the bright range and the first
// step (10 -> 25) jumped from 4% to 10%. The bar moves over a 0..1000 position
// and the PWM comes from it through a 2.2 gamma curve - the bottom half of
// the bar covers 0.9% to ~22%, in fine steps.
#define BRILHO_POS_MAX AJUSTES_BRILHO_POS_MAX   // the curve lives in services/ajustes.cpp
static int brilho_para_pos(int b) { return ajustes_brilho_para_pos(b); }
static int pos_para_brilho(int pos) { return ajustes_pos_para_brilho(pos); }

// Each row's height depends on its kind (touch uses the same maths, via the list).
static int item_altura(int i, void*)
{
  if (i < 0 || i >= LINHAS_N) { return ITEM_A; }
  return LINHAS[i].tipo == L_GRUPO ? GRUPO_A : LINHAS[i].tipo == L_DESLIZANTE ? DESL_A : ITEM_A;
}

// The width the list gave the last item drawn (it subtracts the scroll
// bar): touch on the track uses the same, to land exactly where it was painted.
static int s_larg_item = 0;

// The mock-up's CARD: each run of rows between two groups is ONE
// TEMA_SUPERFICIE card inset from the edges, with rounded corners only at the
// top of the first row and the bottom of the last. Outside the card is the background.
static void cartao_fundo(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, int h, uint16_t cor)
{
  d.fillRect(x0, y, largura, h, TEMA_FUNDO);
  const int cx0 = x0 + TEMA_ESPACO_3, cl = largura - 2 * TEMA_ESPACO_3;
  const bool topo = abre_cartao(i), base = fecha_cartao(i);
  if (!topo && !base) { d.fillRect(cx0, y, cl, h, cor); return; }
  d.fillRoundRect(cx0, y, cl, h, TEMA_RAIO_G, cor);
  // What should not be rounded is square: the bottom half if only the top is
  // rounded, the top half if only the bottom is.
  if (topo && !base) { d.fillRect(cx0, y + h / 2, cl, h - h / 2, cor); }
  if (!topo && base) { d.fillRect(cx0, y, cl, h / 2, cor); }
}

// The slider track (x and width) inside a row of width 'largura' starting at
// x0: the knob at the ends still fits inside the card.
static void trilho_geo(int x0, int largura, int* tx, int* tl)
{
  *tx = x0 + MARGEM + BOTAO_R;
  *tl = largura - 2 * (MARGEM + BOTAO_R);
}

// Position, minimum and maximum of slider k (0 = brightness, on the perceptual scale; 1 = volume).
static void deslizante_valores(int k, int* valor, int* minimo, int* maximo)
{
  if (k == 0) { *valor = brilho_para_pos(ajustes_brilho()); *minimo = 0;                  *maximo = BRILHO_POS_MAX; }
  else        { *valor = ajustes_volume();                  *minimo = AJUSTES_VOLUME_MIN; *maximo = AJUSTES_VOLUME_MAX; }
}

static void linha_grupo(lgfx::LovyanGFX& d, int i, int x0, int y, int largura)
{
  d.fillRect(x0, y, largura, GRUPO_A, TEMA_FUNDO);
  // Card inset + 8, sitting at the bottom (close to the card it belongs to).
  tab5_escrever(d, tr(LINHAS[i].rotulo), x0 + TEMA_ESPACO_3 + TEMA_ESPACO_2, y + GRUPO_A - TEMA_ESPACO_2,
                TEMA_LETRA_LEGENDA, true, TEMA_TEXTO_2, TEMA_FUNDO, bottom_left);
}

static void linha_pagina(lgfx::LovyanGFX& d, int i, int x0, int y, int largura)
{
  const int p = LINHAS[i].valor;
  const int cx0 = x0 + TEMA_ESPACO_3, cl = largura - 2 * TEMA_ESPACO_3;
  const bool escolhida = deitado(sistema_area()) && p == s_pag;
  const uint16_t cor_cartao = escolhida ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE;
  cartao_fundo(d, i, x0, y, largura, ITEM_A, cor_cartao);
  const int ix = x0 + MARGEM, iy = y + (ITEM_A - ICONE_L) / 2;
  PAGINAS[p].icone(d, ix, iy, ICONE_L);
  const int tx = ix + ICONE_L + 24;
  tab5_escrever(d, tr(PAGINAS[p].nome), tx, y + 14, TEMA_LETRA_SUBTITULO, true, TEMA_TEXTO, cor_cartao);
  // Status truncated BY MEASURING, so it does not run under the chevron.
  char sub[48];
  snprintf(sub, sizeof(sub), "%s", s_sub[p]);
  d.setFont(tab5_fonte_suave(false, TEMA_LETRA_CORPO));
  const int max_l = x0 + largura - MARGEM - 40 - tx;
  tab5_cortar_ao_caber(d, sub, max_l);
  tab5_fonte_corpo(d);
  tab5_escrever(d, sub, tx, y + 54, TEMA_LETRA_CORPO, false, TEMA_TEXTO_2, cor_cartao);
  if (deitado(sistema_area())) {
    // In landscape there is nowhere to go (the page is already beside it):
    // instead of the chevron, the open item's marker.
    d.fillRect(cx0, y + 12, 6, ITEM_A - 24, p == s_pag ? TEMA_DESTAQUE : cor_cartao);
  } else {
    chevron(d, x0 + largura - MARGEM - 12, y + ITEM_A / 2, TEMA_TEXTO_3);
  }
  // Divider between rows of the SAME card (not after the last).
  if (!fecha_cartao(i)) { d.drawFastHLine(tx, y + ITEM_A - 1, cx0 + cl - tx, TEMA_LINHA); }
}

// Slider inside the card: on top the name (left) and "NN%" (right);
// below, the track with the knob.
static void linha_deslizante(lgfx::LovyanGFX& d, int i, int x0, int y, int largura)
{
  const int k = LINHAS[i].valor;
  int valor, minimo, maximo;
  deslizante_valores(k, &valor, &minimo, &maximo);
  const int pct = ((valor - minimo) * 100) / (maximo - minimo);
  cartao_fundo(d, i, x0, y, largura, DESL_A, TEMA_SUPERFICIE);
  tab5_escrever(d, tr(LINHAS[i].rotulo), x0 + MARGEM, y + 14, TEMA_LETRA_CORPO, false, TEMA_TEXTO, TEMA_SUPERFICIE);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  tab5_escrever(d, buf, x0 + largura - MARGEM, y + 14, TEMA_LETRA_CORPO, false, TEMA_TEXTO_2, TEMA_SUPERFICIE, top_right);
  int tx, tl;
  trilho_geo(x0, largura, &tx, &tl);
  const int cy = y + 70;
  const int pos = ((valor - minimo) * tl) / (maximo - minimo);
  d.fillRoundRect(tx, cy - TRILHO_A / 2, tl, TRILHO_A, TRILHO_A / 2, TEMA_SUPERFICIE_ALTA);
  if (pos > 0) { d.fillRoundRect(tx, cy - TRILHO_A / 2, pos, TRILHO_A, TRILHO_A / 2, TEMA_DESTAQUE); }
  d.fillCircle(tx + pos, cy, BOTAO_R, TEMA_SOBRE_DESTAQUE);
  if (!fecha_cartao(i)) { d.drawFastHLine(x0 + MARGEM, y + DESL_A - 1, largura - 2 * MARGEM, TEMA_LINHA); }
}

static void item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= LINHAS_N) { return; }
  s_larg_item = largura;
  switch (LINHAS[i].tipo) {
    case L_GRUPO:      linha_grupo(d, i, x0, y, largura); break;
    case L_PAGINA:     linha_pagina(d, i, x0, y, largura); break;
    case L_DESLIZANTE: linha_deslizante(d, i, x0, y, largura); break;
  }
}

// Applies the new brightness value (at the perceptual position) or the volume.
static void aplicar_rapido(bool brilho, int novo, int atual, bool beep)
{
  if (novo == atual) { return; }
  if (brilho) {
    int b = pos_para_brilho(novo);
    // At the foot of the curve two "-"/"+" taps can land on the same PWM value:
    // the button always moves at least one.
    const int b_atual = ajustes_brilho();
    if (b == b_atual) { b += (novo > atual) ? 1 : -1; }
    if (b < AJUSTES_BRILHO_MIN) { b = AJUSTES_BRILHO_MIN; }
    if (b > AJUSTES_BRILHO_MAX) { b = AJUSTES_BRILHO_MAX; }
    if (b == b_atual) { return; }
    ajustes_definir_brilho((uint8_t)b);
  }
  else {
    ajustes_definir_volume((uint8_t)novo);
    if (beep) { sistema_beep(880.0f, 80); }   // to hear the new volume
  }
  sistema_pedir_redesenho();
}

// ---- THE SLIDERS (0 = brightness, 1 = volume), list rows ----
// TOUCH ARRIVES RELATIVE TO THE app AREA (0,0 = its corner): the track is
// measured with x0 = 0 and the same width the list drew the row with.

// Did the finger LAND on a slider row, within the track strip (with slack at
// the ends; vertically the whole row counts)? Which one (0/1), or -1.
static int deslizante_sob(int tx, int ty)
{
  int dy = 0;
  const int i = lista_item_em(&s_lista, tx, ty, &dy);
  if (i < 0 || LINHAS[i].tipo != L_DESLIZANTE) { return -1; }
  int t0, tl;
  trilho_geo(0, s_larg_item, &t0, &tl);
  if (tx >= t0 - BOTAO_R - TOQUE_EXTRA && tx <= t0 + tl + BOTAO_R + TOQUE_EXTRA) { return LINHAS[i].valor; }
  return -1;
}

// Tap or drag: the value comes from the finger's position along the track.
static void mexer_deslizante(int k, int tx)
{
  int t0, tl;
  trilho_geo(0, s_larg_item, &t0, &tl);
  if (tl <= 0) { return; }
  int atual, minimo, maximo;
  deslizante_valores(k, &atual, &minimo, &maximo);
  int rel = tx - t0;
  if (rel < 0) { rel = 0; }
  if (rel > tl) { rel = tl; }
  aplicar_rapido(k == 0, minimo + (rel * (maximo - minimo)) / tl, atual, false);
}

// Which slider the finger grabbed (-1 = none) and whether the finger was
// already down on the previous sample: only LANDING on the track grabs the
// control, and from then until release the list does not scroll.
static int  s_desl_preso = -1;
static bool s_dedo_antes = false;

// State of each row: only the one whose text or value changed is redrawn.
static void atualizar_estados(void)
{
  for (int i = 0; i < PAG_N; ++i) {
    char agora[48];
    PAGINAS[i].estado(agora, sizeof(agora));
    if (strcmp(agora, s_sub[i]) != 0) {
      snprintf(s_sub[i], sizeof(s_sub[i]), "%s", agora);
      lista_item_mudou(&s_lista, linha_da_pagina(i));
    }
  }
  // Brightness and volume also change from elsewhere (top bar, keys): only their row repaints.
  if (pintura_mudou_i(&s_cfg_p, &s_ub, ajustes_brilho())) { lista_item_mudou(&s_lista, linha_do_deslizante(0)); }
  if (pintura_mudou_i(&s_cfg_p, &s_uv, ajustes_volume())) { lista_item_mudou(&s_lista, linha_do_deslizante(1)); }
}

// Portrait: the list fills the whole area.
static void desenhar_inicio(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  atualizar_estados();
  lista_desenhar(&s_lista, d, a, TEMA_FUNDO, tudo);
}

// ---------------------------------------------------------------------------
// PAGES
// ---------------------------------------------------------------------------
// Each page: draw(everything) and touch(x,y). The page redraws its whole
// content when the SIGNATURE (the values it shows) changes - and only then.
void conteudo_limpo(lgfx::LovyanGFX& d, const area_t& a)
{
  tab5_tela_limpar(a.x, a.y + CONTEUDO_Y, a.largura, a.altura - CONTEUDO_Y, TEMA_FUNDO);
}

// ---------------------------------------------------------------------------
// DISPATCH
// ---------------------------------------------------------------------------
static void abrir_pagina(int p)
{
  if (p == PAG_ENERGIA) { sistema_abrir_app("Energia"); return; }   // an app, not a page
  if (s_pin_editando) { s_pin_editando = false; tecvirt_fechar(&s_cfg_kb); }
  s_pag = p;
  s_assin[0] = 0;
  s_aviso[0] = 0;
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); tecvirt_fechar(&s_cfg_kb); }
  if (s_mq_campo >= 0) { s_mq_campo = -1; memset(s_mq_txt, 0, sizeof(s_mq_txt)); tecvirt_fechar(&s_cfg_kb); }
  s_mq_ed = false;
  memset(&s_mq_p, 0, sizeof(s_mq_p));
  if (p == PAG_WIFI) {
    lista_definir(&s_l_wifi, ROLAGEM_INICIO, 0, wf_item_altura, wf_item_desenhar, nullptr);
    s_wf_ver = 0;
    tab5_wifi_procurar();   // entering the page starts a scan
  } else {
    lista_esconder(&s_l_wifi);
  }
  if (p == PAG_BLUETOOTH) {
    lista_definir(&s_l_bt, ROLAGEM_INICIO, 0, bt_item_altura, bt_item_desenhar, nullptr);
    s_bt_ver = 0;
    // Entering the page starts a scan: that is what you want to see on opening.
    if (tab5_ble_pronto()) { tab5_ble_varrer(6000); }
  } else {
    lista_esconder(&s_l_bt);
  }
  if (p == PAG_INICIO) { s_ub = s_uv = -1; }
  else if (!deitado(sistema_area())) { lista_esconder(&s_lista); }   // in landscape the list stays alongside
  rolagem_zerar(&s_rol_pag);
  s_pag_h = 0;
  pintura_pedir(&s_cfg_p);
  sistema_pedir_redesenho_completo();
}

static bool ao_abrir(void)
{
  s_pag = PAG_INICIO;
  s_ub = s_uv = -1;
  memset(s_sub, 0, sizeof(s_sub));
  for (int i = 0; i < PAG_N; ++i) { PAGINAS[i].estado(s_sub[i], sizeof(s_sub[i])); }
  lista_definir(&s_lista, ROLAGEM_INICIO, LINHAS_N, item_altura, item_desenhar, nullptr);
  pintura_pedir(&s_cfg_p);
  // The flash drive notice (status bar) opens Ajustes: lands straight on the page.
  if (tab5_pendrive_pede_atencao()) { abrir_pagina(PAG_USB); }
  return true;
}

static void ao_fechar(void)
{
  if (s_mq_campo >= 0) { s_mq_campo = -1; memset(s_mq_txt, 0, sizeof(s_mq_txt)); tecvirt_fechar(&s_cfg_kb); }
  memset(&s_mq_p, 0, sizeof(s_mq_p));   // the typed password does not stay in RAM
  if (s_pin_editando) { s_pin_editando = false; tecvirt_fechar(&s_cfg_kb); }
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); tecvirt_fechar(&s_cfg_kb); }
}

// LANDSCAPE: the list on the left and the page alongside, scrolling.
static void desenhar_deitado(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  const area_t dir = { a.x + PAG_X, a.y, a.largura - PAG_X, a.altura };
  s_dx_pag = PAG_X;
  s_dy_pag = 0;

  if (tudo) {
    d.drawFastVLine(dir.x - 1, dir.y, dir.altura, cor_linha(d));
  }
  atualizar_estados();
  const area_t janela = { a.x, dir.y, LADO_L, dir.altura };
  lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, tudo);

  // ---- the page ----
  if (s_pag == PAG_INICIO) {
    if (tudo) {
      tab5_escrever(d, tr("Escolha uma opção"), dir.x + dir.largura / 2, dir.y + dir.altura / 2, 28, false,
                    cor_sub(d), TEMA_FUNDO, middle_center);
    }
    return;
  }
  // Scrolled since the last pass: shifts the pixels that are already right and
  // paints only the exposed strip (the same as the kernel); if it cannot shift,
  // everything.
  const area_t* recorte = &dir;
  rolagem_passada_t rp = {};
  bool completa = tudo;
  if (!tudo && rolagem_pendente(&s_rol_pag)) {
    rp = rolagem_deslocar(&s_rol_pag, dir, TEMA_FUNDO);
    if (rp.deslocou) { recorte = &rp.exposta; }
    else             { tab5_tela_limpar(dir.x, dir.y, dir.largura, dir.altura, TEMA_FUNDO); }
    completa = true;
  }
  area_t pa = dir;
  pa.y -= s_rol_pag.atual;
  if (s_pag_h > pa.altura) { pa.altura = s_pag_h; }
  // Restores the clip it found (the kernel's: the app area), instead of
  // erasing it - the rest of this drawing still must not paint over the bars.
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  d.setClipRect(recorte->x, recorte->y, recorte->largura, recorte->altura);
  s_fim = 0;
  if (completa) { cabecalho(d, pa, tr(PAGINAS[s_pag].nome)); }
  if (PAGINAS[s_pag].desenhar) { PAGINAS[s_pag].desenhar(d, pa, completa); }
  d.setClipRect(cx, cy, cl, ca);
  // The height comes from what the page drew on a full pass (list pages
  // count the whole list - see janela_da_lista).
  if (tudo && s_fim > 0) {
    const int h = s_fim - pa.y + 32;
    if (h != s_pag_h) {
      s_pag_h = h;
      if (rolagem_limite(&s_rol_pag, h, dir.altura)) { sistema_pedir_redesenho_completo(); }
    }
  }
  // THE SCROLL BAR, like the lists' one (the shift dragged it along: redo it).
  if (completa) { rolagem_barra(d, &s_rol_pag, dir, TEMA_FUNDO); }
  rolagem_sincronizar(&s_rol_pag);
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  // THE NODE ASKED FOR THE PIN: from any Ajustes page, go to the Meshtastic
  // one, which opens the keyboard (pairing waits ~30 s).
  if (mesh_ble_esperando_pin() && s_pag != PAG_MESH) { abrir_pagina(PAG_MESH); }
  pintura_iniciar(&s_cfg_p);
  const bool tudo = pintura_completa(&s_cfg_p);
  if (deitado(a)) { desenhar_deitado(d, a, tudo); return; }
  s_dx_pag = 0;
  s_dy_pag = 0;
  if (s_pag == PAG_INICIO) { desenhar_inicio(d, a, tudo); return; }
  if (tudo) { cabecalho(d, a, tr(PAGINAS[s_pag].nome)); }
  if (PAGINAS[s_pag].desenhar) { PAGINAS[s_pag].desenhar(d, a, tudo); }
}

static void ao_tocar(int tx, int ty)
{
  if (deitado(sistema_area())) {
    if (tx < PAG_X) {                                     // the list
      int dy = 0;
      const int i = lista_item_tocado(&s_lista, tx, ty, &dy);
      // Groups and sliders open nothing (the slider was handled at the level above).
      if (i >= 0 && i < LINHAS_N && LINHAS[i].tipo == L_PAGINA && LINHAS[i].valor != s_pag) { abrir_pagina(LINHAS[i].valor); }
      return;
    }
    if (rolagem_foi_arraste(&s_rol_pag)) { return; }   // end of a drag is not a click
    const int px = tx - s_dx_pag, py = ty + s_rol_pag.atual - s_dy_pag;
    if (s_pag == PAG_INICIO) { return; }
    if (PAGINAS[s_pag].tocar) { PAGINAS[s_pag].tocar(px, py); }
    return;
  }
  if (s_pag == PAG_INICIO) {
    int dy = 0;
    const int i = lista_item_tocado(&s_lista, tx, ty, &dy);
    if (i >= 0 && i < LINHAS_N && LINHAS[i].tipo == L_PAGINA) { abrir_pagina(LINHAS[i].valor); }
    return;
  }
  if (!s_pin_editando && !s_wf_senha && s_mq_campo < 0 && tab5_dentro(tx, ty, 16, 16, VOLTAR_L, VOLTAR_A)) { abrir_pagina(PAG_INICIO); return; }
  if (PAGINAS[s_pag].tocar) { PAGINAS[s_pag].tocar(tx, ty); }
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  // Keyboard open (Wi-Fi password or PIN): the fingers belong to it.
  if (tecvirt_aberto(&s_cfg_kb)) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_cfg_kb, acoes, 8);
    for (int i = 0; i < n && tecvirt_aberto(&s_cfg_kb); ++i) {
      if (s_wf_senha)          { wf_tecla(acoes[i]); }
      else if (s_pin_editando) { pin_tecla(acoes[i]); }
      else if (s_mq_campo >= 0) { mq_tecla(acoes[i]); }
    }
    return;
  }
  // FINGER x SCROLL ARBITRATION: only the finger LANDING on a slider's track
  // grabs the control - from then until the finger lifts it owns it (the value
  // follows x) and the list does not scroll. A finger that landed off the track
  // scrolls the list, wherever it goes; a short tap opens the page (ao_tocar).
  const bool lista_visivel = deitado(sistema_area()) || s_pag == PAG_INICIO;
  if (!pressionado) { s_desl_preso = -1; s_dedo_antes = false; }
  else if (!s_dedo_antes) {
    s_dedo_antes = true;
    s_desl_preso = lista_visivel ? deslizante_sob(x, y) : -1;
  }
  if (s_desl_preso >= 0) {
    mexer_deslizante(s_desl_preso, x);
    return;   // the list does not even see this finger: no scroll anchor, no drag to be read as a click
  }
  if (deitado(sistema_area())) {
    // EACH ZONE KNOWS, ON EVERY SAMPLE, WHETHER THE FINGER IS IN IT - and releases
    // the anchor when it is not (lifted, or moved to the other column). Before,
    // the release only went to the column the coordinate fell in: the page kept
    // the last drag point, the next touch measured from there and it jumped up.
    const bool na_lista  = pressionado && x < PAG_X;
    const bool na_pagina = pressionado && x >= PAG_X && s_pag != PAG_INICIO;
    if (lista_arrastar(&s_lista, x, y, na_lista)) { sistema_pedir_redesenho(); }
    if (rolagem_arrastar(&s_rol_pag, y, na_pagina)) { sistema_pedir_redesenho(); }
    return;
  }
  if (s_pag == PAG_INICIO) {
    if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
  } else if (s_pag == PAG_BLUETOOTH) {
    if (lista_arrastar(&s_l_bt, x, y, pressionado)) { sistema_pedir_redesenho(); }
  } else if (s_pag == PAG_WIFI && !s_wf_senha) {
    if (lista_arrastar(&s_l_wifi, x, y, pressionado)) { sistema_pedir_redesenho(); }
  }
}

static void ao_teclar(const char* txt, uint8_t codigo, uint8_t)
{
  if (s_pag == PAG_MESH) { teclar_mesh(txt, codigo); }
  if (s_pag == PAG_MQTT) { teclar_mqtt(txt, codigo); }
  if (s_pag == PAG_WIFI) { teclar_wifi(txt, codigo); }
}


// 'extern' is MANDATORY: in C++ a const at file scope has internal
// linkage, and apps.cpp would not see this symbol.
// EACH PAGE'S PACE. It was 500 ms on all of them: the UI woke 2x/s with
// Ajustes open, even sitting on the Screen page (which only changes by touch,
// and touch already requests a redraw). Now: 0 where nothing changes by
// itself, 500 ms only where something moves fast (network scan, BLE scan,
// flash drive in use, Grove probing), 1 s for the rest (Wi-Fi/BLE/Mesh
// states, the About clock). Each page still compares the signature before painting.
static uint32_t periodo_agora(void)
{
  if (sistema_grove_sondando()) { return 500; }
  switch (s_pag) {
    case PAG_TELA:      return 0;
    case PAG_IDIOMA:    return 0;
    case PAG_WIFI:      return tab5_wifi_procurando() ? 500 : 1000;
    case PAG_BLUETOOTH: return tab5_ble_varrendo() ? 500 : 1000;
    case PAG_USB: {
      tab5_pendrive_estado_t e;
      tab5_pendrive_estado(&e);
      return e.fase != PENDRIVE_PARADO ? 500 : 1000;
    }
    default:            return 1000;
  }
}

extern const app_t APP_CONFIG = {
  .nome  = "Ajustes",
  .cor   = TEMA_APP_AMARELO,
  .perfil = { .alto_falante = true },
  .usa_sd = false,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_ms = 1000,                // the default; periodo_agora decides per page
  .periodo_agora = periodo_agora,
  .altura_conteudo = 0,   // does not scroll as a whole: the list and the pages scroll internally
  .icone = icone_config,
  .fundo_tema = true,   // already migrated to the central theme
};
