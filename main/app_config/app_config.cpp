// APP: Ajustes - as preferencias do sistema.
//
// NOVA FORMA, a de qualquer celular:
//
//   - a tela inicial e UMA lista rolavel em GRUPOS (CONEXOES, TELA E SOM, SISTEMA), cada
//     grupo num cartao arredondado, como na maquete aprovada (a tabela LINHAS);
//   - as linhas de pagina mostram icone, nome e uma linha de estado
//     ("Conectado a ...", "Desligado") para saber sem entrar;
//   - BRILHO e VOLUME sao linhas da mesma lista, com o controle deslizante dentro
//     (tocar no trilho ou arrastar; o dedo que desce nele nao rola a lista);
//   - cada pagina tem VOLTAR e as opcoes dela. A lista rola (kernel/lista):
//     opcao nova e UMA linha na tabela PAGINAS, nao um remendo de posicao.
//
// A versao anterior empilhava todas as secoes numa coluna de 1800 px que o
// sistema rolava; cada secao nova empurrava as outras, e as alturas eram
// numeros somados a mao (e erraram mais de uma vez).
//
// DEITADO (teclado fisico encaixado, kernel/orientacao) e o mesmo esquema com
// mais uma coluna, mestre-detalhe como nos tablets: a lista a esquerda (LADO_L)
// e, a direita, a pagina escolhida, que rola
// sozinha (s_rol_pag) - rolar a pagina nao leva a lista embora. As paginas sao
// as mesmas de em pe: desenham na area que recebem e tocam relativo a ela.
//
// O que mora aqui e do USUARIO, nao do aplicativo: cada app continua
// declarando o hardware que precisa no perfil dele; este app controla o que
// atravessa todos (ajustes.h).

//
// PARTIDO POR ASSUNTO (so mudou de arquivo, nada de logica). Este arquivo ficou
// com as medidas, as paginas (a tabela PAGINAS), o estado da tela, os textos e
// botoes, a linha de estado de cada pagina, o inicio (brilho, volume e a
// lista), o despacho e o registro do app. O resto foi para app_config_*.cpp; o
// que as partes compartilham esta em app_config_interno.hpp:
//   app_config_icones.cpp   icones da lista e o icone do app na tela inicial
//   app_config_rede.cpp     paginas Wi-Fi, Bluetooth, ESP-NOW, Meshtastic e MQTT
//   app_config_paginas.cpp  paginas Data e hora, Acesso pelo PC, Idioma, Tela, Teclado, Grove, Sobre e USB

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
// MEDIDAS
// ---------------------------------------------------------------------------
static const int PM_L     = 88;       // largura dos botoes - e +
static const int TRILHO_A  = 12;       // espessura do trilho
static const int BOTAO_R   = 16;       // raio da bolinha do deslizante (32 px)
static const int TOQUE_EXTRA = 16;     // folga de toque alem das pontas do trilho (na altura vale a linha toda)
// Alturas das linhas da tela inicial (o toque e a lista usam as mesmas).
static const int GRUPO_A   = 56;       // rotulo do grupo (CONEXOES...), alinhado embaixo
static const int ITEM_A    = 92;       // linha de pagina
static const int DESL_A    = 100;      // linha com deslizante: nome e % em cima, trilho embaixo
static const int ICONE_L   = 64;

// Deitado (mestre-detalhe, ver o topo do arquivo)
static const int LADO_L      = 400;   // a lista, a esquerda
static const int PAG_X       = LADO_L + 1;   // onde a pagina comeca (deitado)

// Paginas
static const int VOLTAR_L = 200, VOLTAR_A = 64;

// ---------------------------------------------------------------------------
// AS PAGINAS - a lista de baixo sai daqui
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

// O desenho e o toque de cada pagina (definidos mais abaixo).
typedef void (*desenhar_fn)(lgfx::LovyanGFX& d, const area_t& a, bool tudo);
typedef void (*tocar_fn)(int tx, int ty);

// UMA LINHA POR PAGINA, com tudo dela: o item da lista (nome, icone, estado)
// e a pagina aberta (desenho e toque). Eram tres tabelas paralelas, e quando
// a Energia entrou so nesta, o desenho e o toque das seguintes andaram uma
// casa: tocar em Data e hora abria o Idioma (04/11/2026). Sem desenho = a
// linha abre outra coisa (abrir_pagina).
typedef struct {
  const char* nome;
  icone_fn    icone;
  estado_fn   estado;
  desenhar_fn desenhar;
  tocar_fn    tocar;      // nullptr = pagina so de leitura
} pagina_def_t;
static const pagina_def_t PAGINAS[] = {
  { N_("Wi-Fi"),          ic_wifi,    est_wifi,    pag_wifi,    toque_wifi   },
  { N_("Bluetooth"),      ic_bt,      est_bt,      pag_bt,      toque_bt     },
  { N_("ESP-NOW"),        ic_espnow,  est_espnow,  pag_espnow,  toque_espnow },
  { N_("Meshtastic"),     ic_mesh,    est_mesh,    pag_mesh,    toque_mesh   },
  { N_("Malha pela internet"), ic_nuvem, est_mqtt, pag_mqtt,    toque_mqtt   },
  { N_("Tela"),           ic_tela,    est_tela,    pag_tela,    toque_tela   },
  { N_("Energia"),        ic_energia, est_energia, nullptr,     nullptr      },   // abre o app Energia (fora da home)
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
// A TELA INICIAL - a ordem e os grupos saem daqui
// ---------------------------------------------------------------------------
// Tres tipos de linha: o rotulo de um GRUPO; uma PAGINA (valor = o pagina_t,
// nome/icone/estado vem de PAGINAS); um DESLIZANTE (valor 0 = brilho, 1 = volume;
// o nome e o rotulo). Cada sequencia de linhas entre dois grupos e UM cartao.
// PAGINAS segue na ordem do enum (o resto do app depende dela); a ordem na
// tela e so esta tabela.
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

// Toda pagina aparece uma vez (esquecer uma na tabela de cima nao compila).
static constexpr int linhas_de_pagina(void)
{
  int n = 0;
  for (int i = 0; i < LINHAS_N; ++i) { if (LINHAS[i].tipo == L_PAGINA) { ++n; } }
  return n;
}
static_assert(linhas_de_pagina() == PAG_N, "toda pagina_t tem uma linha de pagina em LINHAS");

// A linha da tela inicial que mostra a pagina p (-1 = nenhuma).
static int linha_da_pagina(int p)
{
  for (int i = 0; i < LINHAS_N; ++i) { if (LINHAS[i].tipo == L_PAGINA && LINHAS[i].valor == p) { return i; } }
  return -1;
}
// A linha do deslizante k (0 = brilho, 1 = volume).
static int linha_do_deslizante(int k)
{
  for (int i = 0; i < LINHAS_N; ++i) { if (LINHAS[i].tipo == L_DESLIZANTE && LINHAS[i].valor == k) { return i; } }
  return -1;
}
// A linha e a primeira / a ultima do cartao dela? (o cartao vai de um grupo ao proximo)
static bool abre_cartao(int i)  { return i == 0 || LINHAS[i - 1].tipo == L_GRUPO; }
static bool fecha_cartao(int i) { return i + 1 == LINHAS_N || LINHAS[i + 1].tipo == L_GRUPO; }

// ---------------------------------------------------------------------------
// ESTADO DA TELA
// ---------------------------------------------------------------------------
pintura_t s_cfg_p;
static int       s_pag = PAG_INICIO;
static lista_t   s_lista;
static char      s_sub[PAG_N][48];     // a linha de estado de cada PAGINA, como desenhada
static int       s_ub = -1, s_uv = -1; // brilho/volume desenhados (para perceber mudanca vinda de fora)
char      s_assin[512];         // "assinatura" da pagina aberta: mudou, redesenha
char      s_aviso[96];          // recado curto numa pagina ("escolhido para o Mesh")

// Bluetooth: a lista de aparelhos vistos
lista_t   s_l_bt;
uint32_t  s_bt_ver = 0;

// Meshtastic: o editor de PIN
bool  s_pin_editando = false;
// O teclado virtual do sistema - um so, para a senha do Wi-Fi e o PIN (nunca
// abertos ao mesmo tempo: trocar de pagina fecha).
tecvirt_t s_cfg_kb;

static void abrir_pagina(int p);

// DEITADO: onde a pagina comeca (relativo a area do app; 0 em pe), a rolagem
// dela e a altura do que ela desenha - medida pelos helpers (s_fim, o fundo
// em coordenada de tela do ultimo elemento) na passada completa.
int       s_dx_pag = 0;
int       s_dy_pag = 0;         // deitado: onde a pagina comeca na vertical (hoje o topo da area, 0)
rolagem_t s_rol_pag;
static int       s_pag_h = 0;
int       s_fim = 0;

// A LISTA DENTRO DE UMA PAGINA (redes, aparelhos), comecando em 'y0'. Em pe a
// pagina e fixa e a lista rola por dentro; deitado a PAGINA rola inteira (a
// coluna e baixa demais para botoes e lista juntos), entao a lista entra com
// a altura de todos os itens e conta no fim da pagina.
area_t janela_da_lista(const area_t& a, int y0, int n, int item_a)
{
  if (!deitado(sistema_area())) { return { a.x, a.y + y0, a.largura, a.altura - y0 }; }
  const int h = n > 0 ? n * item_a : item_a;
  fim(a.y + y0 + h);
  return { a.x, a.y + y0, a.largura, h };
}

// A area da pagina, em tela: a do app em pe; deitado, a coluna da direita.
area_t area_pagina(void)
{
  area_t a = sistema_area();
  if (deitado(a)) { a.x += PAG_X; a.largura -= PAG_X; }
  return a;
}

// ---------------------------------------------------------------------------
// TEXTO E BOTOES
// ---------------------------------------------------------------------------

// Paragrafo quebrado na largura - o mesmo quebrador dos baloes de conversa.
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


// Chevron ">" desenhado (a fonte teria outro peso a cada tamanho).
void chevron(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  for (int k = 0; k < 3; ++k) {
    d.drawLine(cx - 6 + k, cy - 12, cx + 6 + k, cy, cor);
    d.drawLine(cx + 6 + k, cy, cx - 6 + k, cy + 12, cor);
  }
}

// Cabecalho de pagina: VOLTAR e o titulo.
static void cabecalho(lgfx::LovyanGFX& d, const area_t& a, const char* titulo)
{
  // Deitado a lista esta sempre a esquerda: sem VOLTAR, so o titulo.
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

// Linha de informacao: rotulo pequeno em cima, valor embaixo. 76 px.
int info(lgfx::LovyanGFX& d, const area_t& a, int y, const char* rotulo,
         const char* valor, uint16_t cor)
{
  tab5_escrever(d, rotulo, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), TEMA_FUNDO);
  tab5_escrever(d, valor, a.x + MARGEM, a.y + y + 28, 32, false, cor, TEMA_FUNDO);
  fim(a.y + y + 84);
  return y + 84;
}

// Botao da largura da pagina.
int botao_largo(lgfx::LovyanGFX& d, const area_t& a, int y, const char* txt, uint16_t cor)
{
  tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, txt, cor);
  fim(a.y + y + BTN_A);
  return y + BTN_A + 20;
}

// Par - / valor / + (tempo de tela, relogio, canal).
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
  // C6 com o firmware de fabrica (Wi-Fi funciona, Bluetooth nao): nao e
  // defeito, e uma atualizacao opcional - o texto tem que dizer isso.
  else if (e == TAB5_BLE_FALHOU && tab5_wifi_c6_enlace() && !tab5_wifi_c6_nosso())
                                  { snprintf(o, t, tr("Rádio C6 de fábrica: atualização opcional")); }
  else if (e == TAB5_BLE_FALHOU)  { snprintf(o, t, tr("Falhou - o C6 não deu o Bluetooth")); }
  else if (e == TAB5_BLE_DESLIGANDO) { snprintf(o, t, tr("Desligando...")); }
  else                            { snprintf(o, t, tr("Desligado")); }
}

void est_espnow(char* o, size_t t)
{
  if (!tab5_espnow_c6_compativel()) {
    // Sem enlace ainda nao da para saber qual firmware o C6 tem.
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

// ENERGIA MORA AQUI: a linha mostra a bateria agora e abre o app Energia
// (os graficos pedem a tela inteira), que nao tem mais icone na home.
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
// INICIO: a lista de grupos (com BRILHO e VOLUME)
// ---------------------------------------------------------------------------
// BRILHO NA ESCALA DO OLHO. O valor do PWM e linear na luz, mas o olho nao:
// 4% de duty ja parece "um quarto do brilho". Com a barra linear quase todo o
// curso ficava no claro e o primeiro degrau (10 -> 25) pulava de 4% para 10%.
// A barra anda numa posicao 0..1000 e o PWM sai dela pela curva gama 2,2 - a
// metade de baixo da barra cobre de 0,9% a ~22%, em degraus finos.
#define BRILHO_POS_MAX AJUSTES_BRILHO_POS_MAX   // a curva mora em services/ajustes.cpp
static int brilho_para_pos(int b) { return ajustes_brilho_para_pos(b); }
static int pos_para_brilho(int pos) { return ajustes_pos_para_brilho(pos); }

// A altura de cada linha depende do tipo (o toque usa a mesma conta, via a lista).
static int item_altura(int i, void*)
{
  if (i < 0 || i >= LINHAS_N) { return ITEM_A; }
  return LINHAS[i].tipo == L_GRUPO ? GRUPO_A : LINHAS[i].tipo == L_DESLIZANTE ? DESL_A : ITEM_A;
}

// A largura que a lista deu ao ultimo item desenhado (ela desconta a barra de
// rolagem): o toque no trilho usa a mesma, para cair exatamente onde foi pintado.
static int s_larg_item = 0;

// O CARTAO da maquete: cada sequencia de linhas entre dois grupos e UM cartao
// TEMA_SUPERFICIE recuado das bordas, com os cantos redondos so no topo da
// primeira linha e na base da ultima. Fora do cartao e o fundo.
static void cartao_fundo(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, int h, uint16_t cor)
{
  d.fillRect(x0, y, largura, h, TEMA_FUNDO);
  const int cx0 = x0 + TEMA_ESPACO_3, cl = largura - 2 * TEMA_ESPACO_3;
  const bool topo = abre_cartao(i), base = fecha_cartao(i);
  if (!topo && !base) { d.fillRect(cx0, y, cl, h, cor); return; }
  d.fillRoundRect(cx0, y, cl, h, TEMA_RAIO_G, cor);
  // O que nao deve ser redondo e reto: a metade de baixo se so o topo arredonda,
  // a de cima se so a base arredonda.
  if (topo && !base) { d.fillRect(cx0, y + h / 2, cl, h - h / 2, cor); }
  if (!topo && base) { d.fillRect(cx0, y, cl, h / 2, cor); }
}

// O trilho do deslizante (x e largura) dentro de uma linha de largura 'largura'
// que comeca em x0: a bolinha nas pontas ainda cabe dentro do cartao.
static void trilho_geo(int x0, int largura, int* tx, int* tl)
{
  *tx = x0 + MARGEM + BOTAO_R;
  *tl = largura - 2 * (MARGEM + BOTAO_R);
}

// Posicao, minimo e maximo do deslizante k (0 = brilho, na escala perceptual; 1 = volume).
static void deslizante_valores(int k, int* valor, int* minimo, int* maximo)
{
  if (k == 0) { *valor = brilho_para_pos(ajustes_brilho()); *minimo = 0;                  *maximo = BRILHO_POS_MAX; }
  else        { *valor = ajustes_volume();                  *minimo = AJUSTES_VOLUME_MIN; *maximo = AJUSTES_VOLUME_MAX; }
}

static void linha_grupo(lgfx::LovyanGFX& d, int i, int x0, int y, int largura)
{
  d.fillRect(x0, y, largura, GRUPO_A, TEMA_FUNDO);
  // Recuo do cartao + 8, colado embaixo (perto do cartao a que pertence).
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
  // Estado cortado MEDINDO, para nao passar por baixo do chevron.
  char sub[48];
  snprintf(sub, sizeof(sub), "%s", s_sub[p]);
  d.setFont(tab5_fonte_suave(false, TEMA_LETRA_CORPO));
  const int max_l = x0 + largura - MARGEM - 40 - tx;
  tab5_cortar_ao_caber(d, sub, max_l);
  tab5_fonte_corpo(d);
  tab5_escrever(d, sub, tx, y + 54, TEMA_LETRA_CORPO, false, TEMA_TEXTO_2, cor_cartao);
  if (deitado(sistema_area())) {
    // Deitado nao ha para onde ir (a pagina ja esta ao lado): no lugar do
    // chevron, a marca do item aberto.
    d.fillRect(cx0, y + 12, 6, ITEM_A - 24, p == s_pag ? TEMA_DESTAQUE : cor_cartao);
  } else {
    chevron(d, x0 + largura - MARGEM - 12, y + ITEM_A / 2, TEMA_TEXTO_3);
  }
  // Divisoria entre linhas do MESMO cartao (nao depois da ultima).
  if (!fecha_cartao(i)) { d.drawFastHLine(tx, y + ITEM_A - 1, cx0 + cl - tx, TEMA_LINHA); }
}

// Deslizante dentro do cartao: em cima o nome (esquerda) e o "NN%" (direita);
// embaixo o trilho com a bolinha.
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

// Aplica o valor novo do brilho (na posicao perceptual) ou do volume.
static void aplicar_rapido(bool brilho, int novo, int atual, bool beep)
{
  if (novo == atual) { return; }
  if (brilho) {
    int b = pos_para_brilho(novo);
    // No pe da curva dois toques de "-"/"+" podem cair no mesmo valor de PWM:
    // o botao sempre anda pelo menos um.
    const int b_atual = ajustes_brilho();
    if (b == b_atual) { b += (novo > atual) ? 1 : -1; }
    if (b < AJUSTES_BRILHO_MIN) { b = AJUSTES_BRILHO_MIN; }
    if (b > AJUSTES_BRILHO_MAX) { b = AJUSTES_BRILHO_MAX; }
    if (b == b_atual) { return; }
    ajustes_definir_brilho((uint8_t)b);
  }
  else {
    ajustes_definir_volume((uint8_t)novo);
    if (beep) { sistema_beep(880.0f, 80); }   // ouvir o volume novo
  }
  sistema_pedir_redesenho();
}

// ---- OS DESLIZANTES (0 = brilho, 1 = volume), linhas da lista ----
// O TOQUE CHEGA RELATIVO A AREA do app (0,0 = canto dela): o trilho e medido
// com x0 = 0 e a mesma largura com que a lista desenhou a linha.

// O dedo DESCEU numa linha de deslizante, na faixa do trilho (com folga nas
// pontas; na vertical vale a linha toda)? Qual (0/1), ou -1.
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

// Toque ou arraste: o valor sai da posicao do dedo ao longo do trilho.
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

// Qual deslizante o dedo agarrou (-1 = nenhum) e se o dedo ja estava embaixo
// na amostra anterior: so a DESCIDA sobre o trilho agarra o controle, e dai ate
// soltar a lista nao rola.
static int  s_desl_preso = -1;
static bool s_dedo_antes = false;

// Estado de cada linha: so a que mudou de texto ou de valor e redesenhada.
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
  // Brilho e volume tambem mudam por fora (barra de cima, teclas): so a linha deles repinta.
  if (pintura_mudou_i(&s_cfg_p, &s_ub, ajustes_brilho())) { lista_item_mudou(&s_lista, linha_do_deslizante(0)); }
  if (pintura_mudou_i(&s_cfg_p, &s_uv, ajustes_volume())) { lista_item_mudou(&s_lista, linha_do_deslizante(1)); }
}

// Em pe: a lista ocupa a area toda.
static void desenhar_inicio(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  atualizar_estados();
  lista_desenhar(&s_lista, d, a, TEMA_FUNDO, tudo);
}

// ---------------------------------------------------------------------------
// PAGINAS
// ---------------------------------------------------------------------------
// Cada pagina: desenhar(tudo) e tocar(x,y). A pagina redesenha o conteudo
// inteiro quando a ASSINATURA (os valores que ela mostra) muda - e so entao.
void conteudo_limpo(lgfx::LovyanGFX& d, const area_t& a)
{
  tab5_tela_limpar(a.x, a.y + CONTEUDO_Y, a.largura, a.altura - CONTEUDO_Y, TEMA_FUNDO);
}

// ---------------------------------------------------------------------------
// DESPACHO
// ---------------------------------------------------------------------------
static void abrir_pagina(int p)
{
  if (p == PAG_ENERGIA) { sistema_abrir_app("Energia"); return; }   // um app, nao uma pagina
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
    tab5_wifi_procurar();   // entrar na pagina ja procura
  } else {
    lista_esconder(&s_l_wifi);
  }
  if (p == PAG_BLUETOOTH) {
    lista_definir(&s_l_bt, ROLAGEM_INICIO, 0, bt_item_altura, bt_item_desenhar, nullptr);
    s_bt_ver = 0;
    // Entrar na pagina ja procura: e o que se quer ver ao abrir.
    if (tab5_ble_pronto()) { tab5_ble_varrer(6000); }
  } else {
    lista_esconder(&s_l_bt);
  }
  if (p == PAG_INICIO) { s_ub = s_uv = -1; }
  else if (!deitado(sistema_area())) { lista_esconder(&s_lista); }   // deitado a lista fica ao lado
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
  // O aviso do pendrive (barra de status) abre Ajustes: cai direto na pagina.
  if (tab5_pendrive_pede_atencao()) { abrir_pagina(PAG_USB); }
  return true;
}

static void ao_fechar(void)
{
  if (s_mq_campo >= 0) { s_mq_campo = -1; memset(s_mq_txt, 0, sizeof(s_mq_txt)); tecvirt_fechar(&s_cfg_kb); }
  memset(&s_mq_p, 0, sizeof(s_mq_p));   // a senha digitada nao fica na RAM
  if (s_pin_editando) { s_pin_editando = false; tecvirt_fechar(&s_cfg_kb); }
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); tecvirt_fechar(&s_cfg_kb); }
}

// DEITADO: a lista a esquerda e a pagina ao lado, rolando.
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

  // ---- a pagina ----
  if (s_pag == PAG_INICIO) {
    if (tudo) {
      tab5_escrever(d, tr("Escolha uma opção"), dir.x + dir.largura / 2, dir.y + dir.altura / 2, 28, false,
                    cor_sub(d), TEMA_FUNDO, middle_center);
    }
    return;
  }
  // Rolou desde a ultima passada: desloca os pixels que ja estao certos e
  // pinta so a faixa exposta (o mesmo do kernel); sem como deslocar, tudo.
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
  // Devolve o recorte que achou (o do kernel: a area do app), em vez de
  // apaga-lo - o resto deste desenho continua sem poder pintar as barras.
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  d.setClipRect(recorte->x, recorte->y, recorte->largura, recorte->altura);
  s_fim = 0;
  if (completa) { cabecalho(d, pa, tr(PAGINAS[s_pag].nome)); }
  if (PAGINAS[s_pag].desenhar) { PAGINAS[s_pag].desenhar(d, pa, completa); }
  d.setClipRect(cx, cy, cl, ca);
  // A altura sai do que a pagina desenhou numa passada inteira (as de lista
  // contam a lista inteira - ver janela_da_lista).
  if (tudo && s_fim > 0) {
    const int h = s_fim - pa.y + 32;
    if (h != s_pag_h) {
      s_pag_h = h;
      if (rolagem_limite(&s_rol_pag, h, dir.altura)) { sistema_pedir_redesenho_completo(); }
    }
  }
  // A BARRINHA, como a das listas (o deslocamento a arrastou junto: refaz).
  if (completa) { rolagem_barra(d, &s_rol_pag, dir, TEMA_FUNDO); }
  rolagem_sincronizar(&s_rol_pag);
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  // O NO PEDIU O PIN: em qualquer pagina de Ajustes, vai para a do
  // Meshtastic, que abre o teclado (o pareamento espera ~30 s).
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
    if (tx < PAG_X) {                                     // a lista
      int dy = 0;
      const int i = lista_item_tocado(&s_lista, tx, ty, &dy);
      // Grupo e deslizante nao abrem nada (o deslizante foi tratado no nivel).
      if (i >= 0 && i < LINHAS_N && LINHAS[i].tipo == L_PAGINA && LINHAS[i].valor != s_pag) { abrir_pagina(LINHAS[i].valor); }
      return;
    }
    if (rolagem_foi_arraste(&s_rol_pag)) { return; }   // fim de arraste nao e clique
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
  // Teclado aberto (senha do Wi-Fi ou PIN): os dedos sao dele.
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
  // ARBITRAGEM DEDO x ROLAGEM: so a DESCIDA do dedo sobre o trilho de um
  // deslizante agarra o controle - dai ate soltar o dedo e dele (o valor segue o
  // x) e a lista nao rola. Dedo que desceu fora do trilho rola a lista, passe
  // por onde passar; toque curto abre a pagina (ao_tocar).
  const bool lista_visivel = deitado(sistema_area()) || s_pag == PAG_INICIO;
  if (!pressionado) { s_desl_preso = -1; s_dedo_antes = false; }
  else if (!s_dedo_antes) {
    s_dedo_antes = true;
    s_desl_preso = lista_visivel ? deslizante_sob(x, y) : -1;
  }
  if (s_desl_preso >= 0) {
    mexer_deslizante(s_desl_preso, x);
    return;   // a lista nem ve este dedo: sem ancora de rolagem, sem arraste a ser lido como clique
  }
  if (deitado(sistema_area())) {
    // CADA ZONA SABE, A CADA AMOSTRA, SE O DEDO ESTA NELA - e solta a ancora
    // quando nao esta (soltou, ou passou para outra coluna). Antes a soltura
    // ia so para a coluna onde a coordenada caia: a pagina guardava o ultimo
    // ponto do arraste, o toque seguinte media dali e ela pulava para cima.
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


// 'extern' e OBRIGATORIO: em C++ um const no escopo de arquivo tem ligacao
// interna, e o apps.cpp nao enxergaria este simbolo.
// O RITMO DE CADA PAGINA. Era 500 ms em todas: a interface acordava 2x/s com
// Ajustes aberto, mesmo parada na pagina da Tela (que so muda pelo toque, e o
// toque ja pede redesenho). Agora: 0 onde nada muda sozinho, 500 ms so onde
// algo anda depressa (procura de redes, varredura BLE, pendrive em uso,
// sondagem do Grove), 1 s no resto (estados de Wi-Fi/BLE/Mesh, relogio do
// Sobre). Cada pagina continua comparando a assinatura antes de pintar.
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
  .periodo_ms = 1000,                // o padrao; periodo_agora decide por pagina
  .periodo_agora = periodo_agora,
  .altura_conteudo = 0,   // nao rola inteiro: a lista e as paginas rolam por dentro
  .icone = icone_config,
  .fundo_tema = true,   // ja migrado para o tema central
};
