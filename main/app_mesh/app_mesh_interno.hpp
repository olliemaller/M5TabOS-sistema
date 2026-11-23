// What the Mesh app files (app_mesh.cpp and app_mesh_*.cpp) share: the
// constants and types used by more than one of them, the state (declared here,
// defined in app_mesh.cpp or in the file indicated) and the functions that cross
// files. For them only: no other app includes this header.
//
// The app was split by subject (it only moved files, no logic changed):
//   app_mesh.cpp           state, the lists, the app cycle and the app_t registration
//   app_mesh_desenho.cpp   drawing, glyphs, logo, column grid, nodes, search, mesh
//   app_mesh_conversa.cpp  location inside a message and the conversation
//   app_mesh_no.cpp        the "No" (Node) page
#pragma once

#include "lista.hpp"
#include "app.h"
#include "tab5_mesh.hpp"
#include "tab5_mesh_mqtt.hpp"   // internet mode servers
#include "teclado_virtual.hpp"
#include "conversa.hpp"
#include "pintura.hpp"

// ---------------------------------------------------------------------------
// Constants and types used by more than one file
// ---------------------------------------------------------------------------
static const int TRILHO_L = 90;
static const int CONV_L   = 200;
static const int ITEM_A   = 72;      // height of an item in the columns
// DELETE CONVERSATION BUTTON. Geometry in one place only, because drawing and touch
// must agree - and it was by "almost" agreeing that the node columns
// drifted out of place once.
//
// It sits BELOW the name, not beside it: beside would force shortening the name from 8
// to 6 characters, and the item is 72px high with the name taking only the
// first line. Height was spare; width was not.
static const int APAGAR_L  = 30;
static const int APAGAR_A  = 28;
static const int APAGAR_DY = 40;            // from the item's y
static const int FRENTE_TOPO_A = 84;        // the BACK and name strip
static const int FRENTE_VOLTAR_L = 200, FRENTE_VOLTAR_A = 56, FRENTE_VOLTAR_Y = 14;
// Rail order: CONVERSATIONS first. It is the most used page, and the one that
// prompts opening the app - the others are for reference.
// THE ORDER MATTERS: it is the same as the first four glyphs (GL_CONVERSAS,
// GL_NOS, GL_MALHA, GL_CONFIG), and the rail converts the page into a glyph by
// direct cast. Changing the order of one without the other puts the wrong icon.
typedef enum { PAG_MSGS = 0, PAG_NOS, PAG_MALHA, PAG_NO, PAG_TOTAL } pagina_t;
// THE OPEN CONVERSATION BY KEY, and not only by index. The index is the position in the
// column, and the column reorders itself (a direct one with a new message moves up, a new
// conversation, a channel disabled on the node). Storing only the index, the screen ended up
// showing and SENDING to the neighbouring conversation. When the order version changes
// (tab5_mesh_conversas_versao), the index is found again by this key.
typedef struct { bool ok; bool canal; uint8_t idx; uint32_t no; } chave_t;
// Local shortcut: inside desenhar() every block asks "is this pass
// full?", and writing pintura_completa(&s_p) in each would be unreadable.
#define TUDO  pintura_completa(&s_p)
typedef enum { GL_CONVERSAS = 0, GL_NOS, GL_MALHA, GL_CONFIG, GL_DIFUSAO, GL_DIRETO } glifo_t;
typedef struct {
  int         col;      // character where the column starts
  int         larg;
  bool        direita;  // alignment of the value AND the title
  const char* titulo;
} coluna_t;
typedef struct { int x, y, l, a; uint8_t idx; } chip_t;   // LOCAL area coordinates
static const int BUSCA_LIMPAR_L = 56;      // the "x", at the right end of the box
static const int DET_BTN_A = 64;
static const int DET_BTN_Y_DA_BASE = 84;    // from the bottom up, see ao_tocar
// STAR button, in the top right corner of the card (coordinates from
// the right of the area, so they hold at any width).
static const int DET_FAV_L = 260, DET_FAV_A = 56, DET_FAV_Y = 14, DET_FAV_DIR = 24;

// ---------------------------------------------------------------------------
// State used by more than one file (each defined in the file indicated)
// ---------------------------------------------------------------------------
// app_mesh.cpp:
extern int s_col_l;
extern pagina_t s_pag;
extern int mesh_s_conv;
extern chave_t s_ab;
extern conversa_t s_c;
extern mesh_msg_t* s_hist;
extern chave_t s_hist_chave;
extern int s_n_hist;
extern lista_t s_l_conv;
extern lista_t s_l_nos;
extern bool s_hist_pronto;
extern pintura_t s_p;

// app_mesh_desenho.cpp:
extern chip_t s_chips[MESH_MAX_CANAIS + 1];
extern int s_n_chips;
extern char s_busca[MESH_BUSCA_MAX];
extern tecvirt_t s_kb_busca;
extern chip_t s_chip_busca;
extern uint32_t s_no_sel;

// app_mesh_no.cpp:
extern int s_grupo;
extern lista_t s_l_cfg;
extern lista_t s_l_redes;
extern int s_editando;
extern tecvirt_t s_kb_cfg;
extern bool s_diario;
extern bool s_web_pg;
extern bool s_redes_pg;
extern bool s_net_pg;
extern bool s_net_ed;
extern mesh_mqtt_servidor_t s_net_p;
extern char s_cfg_msg[64];

// ---------------------------------------------------------------------------
// Internal functions that cross files (each defined in the file indicated)
// ---------------------------------------------------------------------------
// app_mesh.cpp:
int apagar_dx(void);
bool em_pe(const area_t& a);
void cortar_ao_caber(lgfx::LovyanGFX& d, char* t, int max_l);
void texto_que_cabe(lgfx::LovyanGFX& d, char* t, int max_l, float tam, float tam_min);
bool chave_igual(const chave_t& a, const chave_t& b);
void carregar_hist(void);

// app_mesh_desenho.cpp:
void caixa_glifo(lgfx::LovyanGFX& d, int x, int y, int l, int a, glifo_t g, uint16_t fundo,
                 uint16_t tinta);
void marca_mesh(lgfx::LovyanGFX& d, int x, int y, int lado);
void desenhar_trilho(lgfx::LovyanGFX& d, const area_t& a);
int conv_altura(int, void*);
void conv_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void*);
void desenhar_conversas(lgfx::LovyanGFX& d, const area_t& a);
void campo(char* linha, const coluna_t& c, const char* txt);
int nos_altura(int, void*);
void nos_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int, void*);
void busca_abrir_teclado(void);
void busca_fechar_teclado(void);
void busca_limpar(void);
void busca_tecla(int ac);
int det_btn_l(const area_t& a);
int det_fav_l(const area_t& a);
void desenhar_nos(lgfx::LovyanGFX& d, const area_t& a, int x0);
void desenhar_malha(lgfx::LovyanGFX& d, const area_t& a, int x0);

// app_mesh_conversa.cpp:
void distancia_direcao(float lat1, float lon1, float lat2, float lon2, float* dist_km,
                       const char** direcao);
// Declared here, defined further down near desenhar_msgs() (where they
// were born) - the rail and the conversations column now also need the
// same palette, and they come EARLIER in the file.
uint16_t fundo_msg(lgfx::LovyanGFX& d);
void atualizar_eh_canal(void);
int conv_contar(void*);
bool conv_ler(int i, conversa_msg_t* o, void*);
bool conv_enviar(const char* texto, void*);
void desenhar_msgs(lgfx::LovyanGFX& d, const area_t& a, int x0);

// app_mesh_no.cpp:
int cfg_altura(int, void*);
void cfg_desenhar(lgfx::LovyanGFX& d, int item, int x, int y, int largura, void*);
int rede_altura(int, void*);
void rede_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*);
void desenhar_config(lgfx::LovyanGFX& d, const area_t& a, int x0);
void cfg_fechar_edicao(bool confirmar);
void cfg_tecla(int ac);
void tocar_config(int x, int y);
