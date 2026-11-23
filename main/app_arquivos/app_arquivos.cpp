// APP: Arquivos (Files) - the device's disks (SD card and USB flash drive),
// and what is on them.
//
// UI ONLY. Listing, copying, moving, deleting and ejecting are the system's job
// (services/tab5_arquivos and services/tab5_volumes); this app shows and asks.
// The rules for "what appears / what can be touched" are also there:
// /sdcard/apps is read-only (except apps/Berry), names starting with '.' and
// the system passwords do not appear - the app only asks
// tab5_arq_gravavel/mexivel to enable a button.
//
// LEVELS:
//   DISCOS    the root: SD card and, when plugged in, the flash drive - space,
//             recycle bin, eject.
//   LIXEIRA   what was deleted on that disk (the root's "Lixeira" button): a
//             tap restores or deletes for good; a LONG tap selects several.
//   PASTA     the contents of a folder on any disk. A tap opens; a LONG tap
//             selects (and from then on a tap marks/unmarks), with the
//             Copy / Cut / Delete / Rename bar. With something copied,
//             "Colar aqui" (paste here) in any writable folder. "+ Pasta" creates.
//   CONTEUDO  the text reader (log, CSV) and the image viewer.
//
// OPEN WITH, by extension: music plays through the system player and opens
// Musica; .be opens as text, with the "Instalar" (install) button (Berry
// variant); images and text open here.
// COPY TO MUSIC: in a folder with music outside the music folder (typically
// the flash drive), a button copies the whole folder (or, at the root of the
// disk, the loose music files) to the SD card's music folder.
//
// COPYING IS A SYSTEM JOB: it carries on with the app closed; the progress
// shows here (bar in the footer) and in the status bar.

#include "app.h"
#include "idioma.h"
#include "toque_longo.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_teclado.hpp"
#include "pintura.hpp"
#include "lista.hpp"
#include "conversa.hpp"   // conversa_quebrar_linhas: the same wrapper as the chat bubbles
#include "teclado_virtual.hpp"
#include "trabalho.hpp"
#include "tab5_sd.hpp"
#include "tab5_volumes.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_imagem.hpp"
#include "tab5_texto.hpp"
#include "tab5_gif.hpp"
#include "tab5_musica.hpp"
#include "tab5_video.hpp"
#include "esp_heap_caps.h"
#include "tab5_hora.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app.arquivos";

// How many items a folder shows. The table lives in PSRAM only while the app is open.
#define MAX_ITENS      512
// READING AS FLOWING TEXT. Each line of the file is WRAPPED at the real
// width (by word; CSV gets a space after each separator so it breaks at the
// field), and the "page" is whatever fits on the screen.
#define LER_LINHAS     48    // file lines fetched per read - more than fit
#define LARG_LINHA     600   // bytes kept from ONE line of the file; the rest becomes "..."

#define TUDO  pintura_completa(&s_p)

typedef enum { NIVEL_DISCOS, NIVEL_PASTA, NIVEL_CONTEUDO, NIVEL_LIXEIRA } nivel_t;
typedef enum { VER_TEXTO, VER_IMG } ver_t;
typedef enum { DLG_NADA, DLG_APAGAR, DLG_ESVAZIAR, DLG_NOME, DLG_LIXO_ITEM, DLG_LIXO_EXCLUIR } dialogo_t;

// ---- state -----------------------------------------------------------------
static nivel_t          s_nivel = NIVEL_DISCOS;
static tab5_arq_item_t* s_itens = nullptr;   // MAX_ITENS, PSRAM, allocated on open
static int              s_n_itens = 0;
static int              s_n_achados = 0;     // how many the folder HAS (may exceed MAX_ITENS)
static char             s_pasta[TAB5_ARQ_CAMINHO_MAX] = "";   // absolute ("/usb/Rock")
static char             s_arquivo[TAB5_ARQ_NOME_MAX] = "";
static bool             s_gravavel = false;  // the open folder accepts create/paste
static bool             s_tem_musica = false;   // there are loose music files in the open folder
static char             s_musica_raiz[TAB5_ARQ_CAMINHO_MAX] = "";   // "/sdcard/Música" ("" = no SD card)

// The disks (DISCOS level), with what the worker measured.
typedef struct {
  tab5_vol_t v;
  uint64_t   total, livre;
  int        lixeira;
  uint64_t   lixeira_bytes;   // the size of everything in the disk's recycle bin
} disco_t;
static disco_t s_discos[TAB5_VOL_N];
static int     s_n_discos = 0;

// The open RECYCLE BIN (LIXEIRA level): the disk root is s_pasta. Both tables
// (the screen's and the worker's) are created on entering the bin, in PSRAM,
// and freed on closing.
static tab5_arq_lixo_t* s_lixo = nullptr;
static tab5_arq_lixo_t* s_lixo_carga = nullptr;
static uint64_t         s_lixo_bytes = 0;
// What the current action (or the dialogue) will touch: a copy of the chosen items.
EXT_RAM_BSS_ATTR static tab5_arq_lixo_t s_lixo_alvo[TAB5_ARQ_ITENS_MAX];
static int              s_lixo_alvo_n = 0;

// SELECTION (long tap). Indices into s_itens (or s_lixo, in the recycle bin).
EXT_RAM_BSS_ATTR static uint8_t s_sel[MAX_ITENS];
static bool s_selecionando = false;
static int  s_n_sel = 0;

// The message from the last action (on the description line), until navigating.
static char s_msg[96] = "";
static bool s_msg_erro = false;

// Dialogues.
static dialogo_t s_dlg = DLG_NADA;
static tecvirt_t s_kb;
static char      s_nome_txt[TAB5_ARQ_NOME_MAX] = "";
static bool      s_nome_criar = false;          // true = new folder; false = rename
static char      s_nome_alvo[TAB5_ARQ_CAMINHO_MAX] = "";   // the item to rename
static char      s_esvaziar_raiz[16] = "";
static int       s_esvaziar_itens = 0;
static uint64_t  s_esvaziar_bytes = 0;
static uint32_t  s_dlg_desde = 0;     // when the confirmation opened (see ao_tocar)

// The system copy job, as seen last time.
static tab5_arq_fase_t s_job_fase = TAB5_ARQ_PARADO;
static bool            s_instalando_be = false;   // the copy in progress installs a Berry app

static lista_t s_lista;
static int  item_altura(int i, void* ctx);
static void item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void* ctx);

// ---- text reader -------------------------------------------------------------
EXT_RAM_BSS_ATTR static char s_linhas[LER_LINHAS][LARG_LINHA + 1];
static int      s_n_linhas = 0;
static int      s_linha = 0;          // the first line of the file on screen (0 = the first)
static int      s_cabem = 1;          // how many file lines fitted on the last pass (the step for DEPOIS/next)
static int      s_total_linhas = 0;
#define VOLTAS_MAX 64
static int      s_voltas[VOLTAS_MAX];
static int      s_n_voltas = 0;
static const int TAM_LEITURA[] = { 24, 29, 36 };
static int      s_tam_leitura = 1;
static bool     s_abriu_ok = true;

// ---- image viewer ------------------------------------------------------------
static ver_t    s_ver = VER_TEXTO;
static uint32_t s_img_largura = 0, s_img_altura = 0;   // real, read from the header (0 = not known yet)

static pintura_t s_p;   // differential painting for this screen

// ---- geometry --------------------------------------------------------------
static const int MARGEM_X = 16;
static const int TOPO_Y = 16, TOPO_A = 64;           // VOLTAR (back) + title
static const int VOLTAR_L = 200;
static const int AA_L = 96;                            // "Aa" (only in the content view)
static const int DESC_Y = 92;                          // the description line (24 px)
static const int DIVISA_Y = 128;
static const int LISTA_Y = 140;                        // where the list starts
static const int NAV_Y = 140, NAV_A = 64, NAV_G = 10;
static const int BARRA_Y = 220, BARRA_A = 14;
static const int BARRA_TOQUE_Y = 204, BARRA_TOQUE_A = 48;
static const int TEXTO_Y = 252;
static const int ARRASTE_PAGINA = 90;
static const char* NAV_NOME[4] = { N_("INÍCIO"), N_("ANTES"), N_("DEPOIS"), N_("FIM") };
static int nav_l(int largura) { return (largura - 2 * MARGEM_X - 3 * NAV_G) / 4; }
static int nav_x(int largura, int i) { return MARGEM_X + i * (nav_l(largura) + NAV_G); }
static const int LIN_LISTA = 88;   // more breathing room between items (user request)
static const uint16_t COR_DIVISA = TEMA_LINHA;   // separates without weighing down
static const int LIN_DISCO = 150;
static const int RODAPE_A = 76;                        // a strip of buttons in the footer
static const int PROG_A = 70;                          // the copy progress strip
static const float TAM_BASE   = 2.6f;
static const float TAM_LISTA  = TAM_BASE * 1.6f;

static const uint16_t CINZA_BT = TEMA_SUPERFICIE_ALTA;   // greyed-out button (no action here)

// ---- buttons: what was drawn and what a touch hits ----------------------
// Each pass records the buttons it drew (AREA coordinates, the touch ones);
// ao_tocar asks which one was tapped. That way drawing and touch use the
// same maths, never two.
typedef enum {
  BT_NADA = 0, BT_VOLTAR, BT_AA, BT_NOVA_PASTA, BT_PARA_MUSICA, BT_TODOS, BT_INSTALAR,
  BT_COPIAR, BT_RECORTAR, BT_APAGAR, BT_RENOMEAR, BT_SAIR_SEL,
  BT_COLAR, BT_LIMPAR_AREA, BT_CANCELAR_JOB, BT_OK_JOB,
  BT_DLG_SIM, BT_DLG_NAO,
  BT_ESVAZIAR, BT_LIXO_RESTAURAR, BT_LIXO_EXCLUIR, BT_DLG_RESTAURAR, BT_DLG_EXCLUIR,
} bt_id_t;
typedef struct { int16_t x, y, l, a; uint8_t id; } bt_t;
static bt_t s_bts[16];
static int  s_n_bts = 0;

static void botao(lgfx::LovyanGFX& d, const area_t& a, int x, int y, int l, int h, const char* txt,
                  uint16_t fundo, uint16_t tinta, bt_id_t id)
{
  tab5_botao(d, a.x + x, a.y + y, l, h, txt, fundo, tinta);
  if (id != BT_NADA && s_n_bts < (int)(sizeof(s_bts) / sizeof(s_bts[0]))) {
    s_bts[s_n_bts++] = { (int16_t)x, (int16_t)y, (int16_t)l, (int16_t)h, (uint8_t)id };
  }
}

static bt_id_t botao_tocado(int x, int y)
{
  for (int i = s_n_bts - 1; i >= 0; --i) {
    const bt_t& b = s_bts[i];
    if (tab5_dentro(x, y, b.x, b.y, b.l, b.a)) { return (bt_id_t)b.id; }
  }
  return BT_NADA;
}

// ---------------------------------------------------------------------------
// PATHS
// ---------------------------------------------------------------------------
static bool juntar(char* out, size_t tam, const char* pasta, const char* nome)
{
  return (size_t)snprintf(out, tam, "%s/%s", pasta, nome) < tam;
}

static void caminho_arquivo(char* out, size_t n) { juntar(out, n, s_pasta, s_arquivo); }

static bool eh_raiz_de_disco(const char* c)
{
  return !strcmp(c, TAB5_VOL_CARTAO_RAIZ) || !strcmp(c, TAB5_VOL_PENDRIVE_RAIZ);
}

// "Pendrive / Rock / Anos 80" - where you are, for the description.
static void trilha_de(const char* c, char* out, size_t tam)
{
  snprintf(out, tam, "%s", tr(tab5_volume_tipo(c)));
  const char* p = c + (tab5_volume_de(c) == TAB5_VOL_CARTAO ? strlen(TAB5_VOL_CARTAO_RAIZ)
                                                             : strlen(TAB5_VOL_PENDRIVE_RAIZ));
  while (*p == '/') {
    ++p;
    const char* fim = strchr(p, '/');
    const int n = fim ? (int)(fim - p) : (int)strlen(p);
    const size_t k = strlen(out);
    if (k + 6 >= tam) { break; }
    snprintf(out + k, tam - k, " / %.*s", n, p);
    p += n;
  }
}

static void mensagem(const char* txt, bool erro)
{
  snprintf(s_msg, sizeof(s_msg), "%s", txt ? txt : "");
  s_msg_erro = erro;
  sistema_pedir_redesenho_completo();
}

// ---------------------------------------------------------------------------
// LISTING ON THE WORKER
// ---------------------------------------------------------------------------
// The worker lists into s_carga (the buffer the screen does NOT draw) and
// ao_aviso swaps the two.
static tab5_arq_item_t* s_carga = nullptr;
static volatile bool    s_listando = false;
static volatile bool    s_l_pronto = false;
static bool             s_l_manter = false, s_l_de_novo = false, s_l_de_novo_manter = false;
static bool             s_l_discos = false, s_l_lixo = false;
static uint64_t         s_l_bytes = 0;
static char             s_l_pasta[TAB5_ARQ_CAMINHO_MAX];
static int              s_l_n = 0, s_l_achados = 0;
static disco_t          s_l_discos_v[TAB5_VOL_N];
static int              s_l_n_discos = 0;
static char             s_l_musica[TAB5_ARQ_CAMINHO_MAX];

static void trabalho_listar(void*)
{
  // The SD card's music folder: so "Copiar para Musica" knows the destination
  // (and does not appear inside it).
  if (!tab5_musica_pasta(s_l_musica, sizeof(s_l_musica))) { s_l_musica[0] = '\0'; }
  if (s_l_discos) {
    tab5_vol_t v[TAB5_VOL_N];
    s_l_n_discos = tab5_volumes_listar(v, TAB5_VOL_N);
    for (int i = 0; i < s_l_n_discos; ++i) {
      disco_t& dk = s_l_discos_v[i];
      dk.v = v[i];
      dk.total = dk.livre = 0;
      dk.lixeira = 0;
      dk.lixeira_bytes = 0;
      if (v[i].disponivel) {
        tab5_volume_espaco(v[i].raiz, &dk.total, &dk.livre);
        // Automatic cleanup (if enabled) and the large-recycle-bin notice: the service's job.
        tab5_arq_lixeira_manter(v[i].raiz);
        dk.lixeira = tab5_arq_lixeira_itens(v[i].raiz);
        if (dk.lixeira > 0) {
          tab5_arq_lixo_t um;   // only the total size matters
          tab5_arq_lixeira_listar(v[i].raiz, &um, 1, &dk.lixeira_bytes);
        }
      }
    }
    s_l_n = 0;
    s_l_achados = 0;
  } else if (s_l_lixo) {
    s_l_n = tab5_arq_lixeira_listar(s_l_pasta, s_lixo_carga, MAX_ITENS, &s_l_bytes);
    s_l_achados = s_l_n;
  } else {
    int total = 0;
    const int n = tab5_arq_listar(s_l_pasta, s_carga, MAX_ITENS, &total, TAB5_ARQ_ORDEM_NOME, nullptr);
    s_l_n = n < 0 ? -1 : n;
    s_l_achados = total;
  }
  s_l_pronto = true;
}

static void pedir_listagem(bool manter_posicao)
{
  if (s_listando) {   // one at a time: the latest request wins
    s_l_de_novo = true; s_l_de_novo_manter = manter_posicao;
    return;
  }
  if (!manter_posicao) { s_n_itens = s_n_achados = 0; lista_itens_mudaram(&s_lista, 0); }
  s_l_manter = manter_posicao;
  s_l_discos = (s_nivel == NIVEL_DISCOS);
  s_l_lixo = (s_nivel == NIVEL_LIXEIRA);
  snprintf(s_l_pasta, sizeof(s_l_pasta), "%s", s_pasta);
  s_l_pronto = false;
  s_listando = sistema_trabalho(trabalho_listar, nullptr);
}

static void sair_da_selecao(void)
{
  s_selecionando = false;
  s_n_sel = 0;
  memset(s_sel, 0, sizeof(s_sel));
}

// Goes to folder 'c' ("" = the disks).
static void navegar(const char* c)
{
  sair_da_selecao();
  s_msg[0] = '\0';
  if (!c || !c[0]) {
    s_nivel = NIVEL_DISCOS;
    s_pasta[0] = '\0';
  } else {
    s_nivel = NIVEL_PASTA;
    snprintf(s_pasta, sizeof(s_pasta), "%s", c);
  }
  s_gravavel = s_nivel == NIVEL_PASTA && tab5_arq_gravavel(s_pasta);
  pedir_listagem(false);
  sistema_pedir_redesenho_completo();
}

// Opens the recycle bin of the disk at 'raiz'.
static void navegar_lixeira(const char* raiz)
{
  if (!s_lixo) { s_lixo = (tab5_arq_lixo_t*)heap_caps_calloc(MAX_ITENS, sizeof(tab5_arq_lixo_t), MALLOC_CAP_SPIRAM); }
  if (!s_lixo_carga) { s_lixo_carga = (tab5_arq_lixo_t*)heap_caps_calloc(MAX_ITENS, sizeof(tab5_arq_lixo_t), MALLOC_CAP_SPIRAM); }
  if (!s_lixo || !s_lixo_carga) { mensagem(tr("Sem memória para abrir a lixeira."), true); return; }
  sair_da_selecao();
  s_msg[0] = '\0';
  s_nivel = NIVEL_LIXEIRA;
  snprintf(s_pasta, sizeof(s_pasta), "%s", raiz);   // the disk root (AVISO_VOLUMES checks against it)
  s_gravavel = false;
  s_lixo_bytes = 0;
  pedir_listagem(false);
  sistema_pedir_redesenho_completo();
}

static void listagem_chegou(void)
{
  s_l_pronto = false;
  s_listando = false;
  snprintf(s_musica_raiz, sizeof(s_musica_raiz), "%s", s_l_musica);
  if (s_l_discos) {
    memcpy(s_discos, s_l_discos_v, sizeof(s_discos));
    s_n_discos = s_l_n_discos;
    s_n_itens = s_n_discos;
    s_n_achados = s_n_discos;
  } else if (s_l_lixo) {
    // Only valid if we are still in THAT disk's recycle bin (the user may have gone back).
    if (s_nivel == NIVEL_LIXEIRA && !strcmp(s_l_pasta, s_pasta)) {
      tab5_arq_lixo_t* t = s_lixo; s_lixo = s_lixo_carga; s_lixo_carga = t;
      s_n_itens = s_n_achados = s_l_n;
      s_lixo_bytes = s_l_bytes;
      if (s_selecionando) { sair_da_selecao(); }   // the indices changed
    }
  } else if (s_l_n < 0) {
    // The folder vanished (flash drive removed, SD card lent to the computer): back to the disks.
    s_n_itens = s_n_achados = 0;
    if (s_nivel == NIVEL_PASTA && !strcmp(s_l_pasta, s_pasta)) {
      navegar("");
      mensagem(tr("A pasta não está mais disponível."), true);
      return;
    }
  } else {
    tab5_arq_item_t* t = s_itens; s_itens = s_carga; s_carga = t;
    s_n_itens = s_l_n;
    s_n_achados = s_l_achados;
    s_tem_musica = false;
    for (int i = 0; i < s_n_itens; ++i) { if (s_itens[i].tipo == TAB5_ARQ_MUSICA) { s_tem_musica = true; break; } }
    if (s_selecionando) { sair_da_selecao(); }   // the indices changed
  }
  lista_itens_mudaram(&s_lista, s_n_itens);
  if (!s_l_manter) { lista_ao_inicio(&s_lista); }
  if (s_l_de_novo) {
    s_l_de_novo = false;
    pedir_listagem(s_l_de_novo_manter);
  }
  sistema_pedir_redesenho_completo();
}

// ---------------------------------------------------------------------------
// ACTIONS ON THE WORKER (they touch the disk: never on the UI)
// ---------------------------------------------------------------------------
typedef enum {
  OP_APAGAR, OP_RENOMEAR, OP_CRIAR, OP_COLAR, OP_EJETAR, OP_PARA_MUSICA,
  OP_TOCAR, OP_INSTALAR, OP_ESVAZIAR, OP_RELER_BERRY, OP_RELER,
  OP_LIXO_RESTAURAR, OP_LIXO_EXCLUIR,
} op_t;
EXT_RAM_BSS_ATTR static char s_op_itens[TAB5_ARQ_ITENS_MAX][TAB5_ARQ_CAMINHO_MAX];
static struct {
  op_t  op;
  int   n;                                   // items in s_op_itens
  char  pasta[TAB5_ARQ_CAMINHO_MAX];
  char  nome[TAB5_ARQ_NOME_MAX];
  // result
  char  msg[96];
  bool  erro;
  bool  reler;          // list the folder again
  bool  abrir_musica;   // open the Musica app (it started playing)
  bool  instalou;       // started copying a Berry app
} s_op;
static volatile bool s_op_rodando = false;
static volatile bool s_op_pronto = false;

static void resultado(tab5_arq_erro_t r, const char* ok_txt)
{
  s_op.erro = r != TAB5_ARQ_OK;
  snprintf(s_op.msg, sizeof(s_op.msg), "%s", r == TAB5_ARQ_OK ? ok_txt : tr(tab5_arq_erro_texto(r)));
}

// Where a recycle-bin item goes back to, as TEXT for display: the original
// folder without the disk root ("/Fotos/2026"), or the disk name when the
// origin is the root, unknown or outside it. 'conferir' = the same rule as
// the service (folder that does not accept writes -> disk root); text only,
// no disk access.
static void lixo_destino(const char* raiz, const char* origem, bool conferir, char* out, size_t tam)
{
  snprintf(out, tam, "%s", tr(tab5_volume_tipo(raiz)));
  const size_t lr = strlen(raiz);
  if (strncmp(origem, raiz, lr) != 0 || origem[lr] != '/') { return; }
  char pai[TAB5_ARQ_CAMINHO_MAX];
  snprintf(pai, sizeof(pai), "%s", origem);
  if (char* b = strrchr(pai, '/')) { *b = '\0'; }
  if (strlen(pai) <= lr) { return; }
  if (conferir && !tab5_arq_gravavel(pai)) { return; }
  snprintf(out, tam, "%s", pai + lr);
}

static void trabalho_op(void*)
{
  s_op.msg[0] = '\0';
  s_op.erro = false;
  s_op.reler = false;
  s_op.abrir_musica = false;
  s_op.instalou = false;
  switch (s_op.op) {
    case OP_APAGAR: {
      int foram = 0;
      tab5_arq_erro_t r = TAB5_ARQ_OK;
      for (int i = 0; i < s_op.n; ++i) {
        const tab5_arq_erro_t e = tab5_arq_apagar(s_op_itens[i]);
        if (e == TAB5_ARQ_OK) { ++foram; } else { r = e; }
      }
      if (r == TAB5_ARQ_OK) { snprintf(s_op.msg, sizeof(s_op.msg), tr("%d item(s) na lixeira."), foram); }
      else { snprintf(s_op.msg, sizeof(s_op.msg), tr("%d na lixeira; os outros: %s"), foram, tr(tab5_arq_erro_texto(r))); s_op.erro = true; }
      s_op.reler = true;
      break;
    }
    case OP_RENOMEAR:
      resultado(tab5_arq_renomear(s_op_itens[0], s_op.nome), tr("Renomeado."));
      s_op.reler = true;
      break;
    case OP_CRIAR:
      resultado(tab5_arq_criar_pasta(s_op.pasta, s_op.nome), tr("Pasta criada."));
      s_op.reler = true;
      break;
    case OP_COLAR:
      resultado(tab5_arq_colar(s_op.pasta), tr("Colando..."));
      break;
    case OP_EJETAR:
      switch (tab5_volume_ejetar(TAB5_VOL_PENDRIVE_RAIZ, 3000)) {
        case TAB5_VOL_EJETADO:       snprintf(s_op.msg, sizeof(s_op.msg), tr("Pode tirar o pendrive.")); break;
        case TAB5_VOL_EJETAR_EM_USO: snprintf(s_op.msg, sizeof(s_op.msg), tr("O pendrive está em uso (cópia ou música).")); s_op.erro = true; break;
        default:                     snprintf(s_op.msg, sizeof(s_op.msg), tr("Não há pendrive para ejetar.")); s_op.erro = true; break;
      }
      s_op.reler = true;
      break;
    case OP_PARA_MUSICA: {
      char destino[TAB5_ARQ_CAMINHO_MAX];
      if (!tab5_musica_pasta(destino, sizeof(destino))) { resultado(TAB5_ARQ_ERRO_DISCO, ""); break; }
      const char* ptrs[TAB5_ARQ_ITENS_MAX];
      for (int i = 0; i < s_op.n; ++i) { ptrs[i] = s_op_itens[i]; }
      resultado(tab5_arq_copiar(ptrs, s_op.n, destino, false), tr("Copiando para a Música..."));
      break;
    }
    case OP_TOCAR: {
      // The player plays the FOLDER starting from an item in its list: find the index.
      tab5_musica_item_t* lista = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * MAX_ITENS, MALLOC_CAP_SPIRAM);
      const int n = lista ? tab5_musica_listar(s_op.pasta, lista, MAX_ITENS, nullptr) : -1;
      int idx = -1;
      for (int i = 0; i < n; ++i) { if (!lista[i].pasta && !strcmp(lista[i].nome, s_op.nome)) { idx = i; break; } }
      heap_caps_free(lista);
      if (idx >= 0 && tab5_musica_tocar(s_op.pasta, idx, "Música")) { s_op.abrir_musica = true; }
      else { snprintf(s_op.msg, sizeof(s_op.msg), tr("Não consegui tocar esta música.")); s_op.erro = true; }
      break;
    }
    case OP_INSTALAR: {
      const char* o = s_op_itens[0];
      resultado(tab5_arq_copiar(&o, 1, TAB5_SD_APPS "/Berry", false), "Instalando o app...");
      s_op.instalou = !s_op.erro;
      break;
    }
    case OP_ESVAZIAR:
      resultado(tab5_arq_esvaziar_lixeira(s_op.pasta), tr("Esvaziando a lixeira..."));
      break;
    case OP_LIXO_RESTAURAR:
    case OP_LIXO_EXCLUIR: {
      const bool restaurar = s_op.op == OP_LIXO_RESTAURAR;
      int foram = 0;
      tab5_arq_erro_t r = TAB5_ARQ_OK;
      char destino[96] = "";
      for (int i = 0; i < s_op.n; ++i) {
        const tab5_arq_lixo_t& it = s_lixo_alvo[i];
        const tab5_arq_erro_t e = restaurar ? tab5_arq_lixeira_restaurar(s_op.pasta, it.nome)
                                            : tab5_arq_lixeira_excluir(s_op.pasta, it.nome);
        if (e != TAB5_ARQ_OK) { r = e; continue; }
        if (++foram == 1) { lixo_destino(s_op.pasta, it.origem, true, destino, sizeof(destino)); }
      }
      if (r == TAB5_ARQ_OK) {
        if (restaurar && foram == 1) { snprintf(s_op.msg, sizeof(s_op.msg), tr("Voltou para %s"), destino); }
        else if (restaurar)          { snprintf(s_op.msg, sizeof(s_op.msg), tr("%d item(s) restaurados."), foram); }
        else                         { snprintf(s_op.msg, sizeof(s_op.msg), tr("%d item(s) excluídos."), foram); }
      } else if (foram == 0) {
        snprintf(s_op.msg, sizeof(s_op.msg), "%s", tr(tab5_arq_erro_texto(r)));
        s_op.erro = true;
      } else {
        snprintf(s_op.msg, sizeof(s_op.msg), restaurar ? tr("%d restaurados; os outros: %s") : tr("%d excluídos; os outros: %s"),
                 foram, tr(tab5_arq_erro_texto(r)));
        s_op.erro = true;
      }
      s_op.reler = true;
      break;
    }
    case OP_RELER_BERRY:
      sistema_apps_de_fora_reler();
      snprintf(s_op.msg, sizeof(s_op.msg), tr("App instalado: o ícone está na tela inicial."));
      break;
    case OP_RELER:
      sistema_apps_de_fora_reler();
      break;
  }
  // Touched a file: the external apps (the .be files in apps/Berry) may have
  // changed - deleted, renamed, pasted. The re-read only swaps the icons if the
  // list actually changed (and in the variant without Berry it does nothing).
  if (s_op.op == OP_APAGAR || s_op.op == OP_RENOMEAR) { sistema_apps_de_fora_reler(); }
  s_op_pronto = true;
}

// s_op and s_op_itens belong to the worker while an action runs: whoever is
// about to fill them asks first.
static bool op_livre(void)
{
  if (s_op_rodando) { mensagem(tr("Aguarde a ação anterior terminar."), true); return false; }
  return true;
}

static bool pedir_op(op_t op)
{
  if (s_op_rodando) { mensagem(tr("Aguarde a ação anterior terminar."), true); return false; }
  s_op.op = op;
  s_op_pronto = false;
  s_op_rodando = sistema_trabalho(trabalho_op, nullptr);
  if (!s_op_rodando) { mensagem(tr("O sistema está ocupado; tente de novo."), true); }
  return s_op_rodando;
}

static void op_chegou(void)
{
  s_op_pronto = false;
  s_op_rodando = false;
  if (s_op.msg[0]) { mensagem(s_op.msg, s_op.erro); }
  if (s_op.instalou) { s_instalando_be = true; }
  if (s_op.abrir_musica) { sistema_abrir_app("Música"); }
  if (s_op.reler) {
    if (s_op.op == OP_EJETAR && s_nivel != NIVEL_DISCOS) { navegar(""); mensagem(s_op.msg, s_op.erro); }
    else { pedir_listagem(true); }
  }
}

// ---------------------------------------------------------------------------
// TEXT READING ON THE WORKER
// ---------------------------------------------------------------------------
// Counting the lines and reading a page on the worker, ONLY with the state
// below (s_c_*) and the open text (s_texto); the UI copies the result in
// ao_aviso. Path and page are SNAPSHOTTED in the request.
static char          s_c_caminho[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
static int           s_c_linha = 0;
static bool          s_c_contar = false;
static int           s_c_total = 0;
EXT_RAM_BSS_ATTR static char s_c_linhas[LER_LINHAS][LARG_LINHA + 1];
static int           s_c_n = 0;
static bool          s_c_abriu = true;
static volatile bool s_c_pronto = false;
static bool          s_c_ocupado = false;
static bool          s_c_de_novo = false;
static bool          s_c_contar_de_novo = false;
static bool          s_precisa_ler = false;

// The open text (line index): building and reading it is the tab5_texto
// service's job. Created and replaced on the worker (when a file is opened);
// the UI only closes it with the worker idle.
static tab5_texto_t* s_texto = nullptr;

static void trabalho_ler(void*)
{
  // Opening (counting) builds the index; reading fetches the page's lines. Each
  // service call holds the disk (SD card or flash drive) only while reading.
  if (s_c_contar) {
    tab5_texto_fechar(s_texto);
    s_texto = tab5_texto_abrir(s_c_caminho, sistema_trabalho_cancelado);
    s_c_total = tab5_texto_linhas(s_texto);
  }
  const int n = s_texto ? tab5_texto_ler(s_texto, s_c_linha, LER_LINHAS, &s_c_linhas[0][0], LARG_LINHA + 1) : -1;
  s_c_abriu = (n >= 0);
  s_c_n = s_c_abriu ? n : 0;
  s_c_pronto = true;
}

static void pedir_leitura(bool contar)
{
  if (s_c_ocupado) {
    s_c_de_novo = true;
    s_c_contar_de_novo = s_c_contar_de_novo || contar;
    return;
  }
  caminho_arquivo(s_c_caminho, sizeof(s_c_caminho));
  s_c_linha   = s_linha;
  s_c_contar  = contar;
  s_c_pronto  = false;
  s_c_ocupado = sistema_trabalho(trabalho_ler, nullptr);
  if (!s_c_ocupado) { s_precisa_ler = true; }
}

// ---------------------------------------------------------------------------
// IMAGE ON THE WORKER
// ---------------------------------------------------------------------------
// Reading and decoding is the image service's job (services/tab5_imagem):
// JPEG, PNG, BMP, QOI and GIF, of any file size. The worker asks it to draw
// the whole image, already at the scale that fits, into a sprite the size of
// the box.
//
// ANIMATED GIF: the worker opens the GIF (s_gif) and decodes ONE frame per
// request, always into the same sprite. The app repaints when the frame
// arrives and, after its delay (periodo_agora), asks for the next one.
// Nothing is decoded on the UI.
static M5Canvas*     s_img_spr = nullptr;       // the whole box, drawn by the worker
static uint32_t      s_img_px_l = 0, s_img_px_a = 0;   // sprite size
static char          s_img_px_caminho[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX] = "";
static volatile bool s_img_lendo = false;
static volatile bool s_img_lida = false;
static volatile bool s_img_lida_ok = false;
static tab5_gif_t*   s_gif = nullptr;           // the open GIF: the worker opens it, the UI closes it with the worker idle
static volatile int  s_gif_atraso = 0;          // delay of the last frame (ms)
static volatile bool s_gif_anima = false;       // the open GIF has more than one frame
static int64_t       s_gif_pedido_us = 0;       // when the frame in progress was requested
static int64_t       s_gif_devido_us = 0;       // when the next frame should be requested

// One GIF frame (opens the file the first time). false = did not open as a GIF
// or the frame failed.
static bool ler_quadro_gif(void)
{
  if (!s_gif) {
    s_gif = tab5_gif_abrir(s_img_px_caminho);
    if (!s_gif) { return false; }
    s_img_largura = (uint32_t)tab5_gif_largura(s_gif);
    s_img_altura  = (uint32_t)tab5_gif_altura(s_gif);
    s_gif_anima   = tab5_gif_animado(s_gif);
  }
  s_gif_atraso = tab5_gif_proximo(s_gif, s_img_spr);
  return s_gif_atraso >= 0;
}

static void trabalho_ler_imagem(void*)
{
  s_img_lida_ok = false;
  const int64_t t0 = esp_timer_get_time();
  if (s_gif || tab5_arq_extensao_e(s_img_px_caminho, ".gif")) { s_img_lida_ok = ler_quadro_gif(); }
  if (!s_img_lida_ok && !s_gif) {
    // The other formats (and a ".gif" that is not a GIF: the service recognises it by content).
    int l = 0, a = 0;
    if (tab5_imagem_tamanho(s_img_px_caminho, &l, &a)) { s_img_largura = (uint32_t)l; s_img_altura = (uint32_t)a; }
    s_img_lida_ok = tab5_imagem_desenhar(s_img_px_caminho, s_img_spr, sistema_trabalho_cancelado);
  }
  ESP_LOGI(TAG, "image %s read in %d ms (ok=%d)", s_img_px_caminho,
           (int)((esp_timer_get_time() - t0) / 1000), (int)s_img_lida_ok);
  s_img_lida = true;
}

static void soltar_imagem(void)
{
  if (s_img_lendo) { return; }   // the worker is writing into it: free it when it reports back
  if (s_gif) { tab5_gif_fechar(s_gif); s_gif = nullptr; }   // only frees memory (the disk has already been released)
  s_gif_anima = false;
  tab5_sprite_soltar(&s_img_spr);
  s_img_px_l = s_img_px_a = 0;
  s_img_largura = s_img_altura = 0;
  s_img_px_caminho[0] = '\0';
}

// ---------------------------------------------------------------------------
// NOTIFICATIONS
// ---------------------------------------------------------------------------
static void job_mudou(void)
{
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  const bool rodava = s_job_fase == TAB5_ARQ_CONTANDO || s_job_fase == TAB5_ARQ_COPIANDO || s_job_fase == TAB5_ARQ_APAGANDO;
  const bool roda   = t.fase == TAB5_ARQ_CONTANDO || t.fase == TAB5_ARQ_COPIANDO || t.fase == TAB5_ARQ_APAGANDO;
  s_job_fase = t.fase;
  if (rodava && !roda) {
    // Finished: what is shown may have changed (destination and, on a move, the source).
    if (s_nivel != NIVEL_CONTEUDO) { pedir_listagem(true); }
    // Copying/moving to (or from) apps/Berry changes the apps: re-read. Silently,
    // except on install (which says "the icon is on the home screen").
    const bool instalou = s_instalando_be && t.fase == TAB5_ARQ_FEITO;
    s_instalando_be = false;
    if (!s_op_rodando) { pedir_op(instalou ? OP_RELER_BERRY : OP_RELER); }
  }
  if (s_nivel != NIVEL_CONTEUDO) { sistema_pedir_redesenho_completo(); }
}

static void ao_aviso(uint32_t av)
{
  if (av & AVISO_ARQUIVOS) { job_mudou(); }
  if (av & AVISO_VOLUMES) {
    // A disk came or went. At the root, the disk list changes; inside a disk that
    // went away, go back to the root.
    if (s_nivel == NIVEL_DISCOS) { pedir_listagem(true); }
    else if (!tab5_volume_disponivel(s_pasta)) {
      if (s_nivel == NIVEL_CONTEUDO) { soltar_imagem(); }
      navegar("");
      mensagem(tr("O disco saiu."), true);
    }
  }
  if (!(av & AVISO_TRABALHO)) { return; }
  if (s_op_pronto) { op_chegou(); }
  if (s_img_lendo && s_img_lida) {
    s_img_lendo = false;
    // Failed: the path stays (and the drawing shows the error instead of retrying).
    if (!s_img_lida_ok) { s_gif_anima = false; }
    if (s_nivel != NIVEL_CONTEUDO || s_ver == VER_TEXTO) {
      soltar_imagem();
    } else if (s_gif && s_gif_anima && s_img_lida_ok && tab5_gif_quadros_vistos(s_gif) > 1) {
      // New GIF frame: just repaint (it is not a new screen). The next one comes after the delay.
      const int64_t agora = esp_timer_get_time();
      s_gif_devido_us = s_gif_pedido_us + (int64_t)s_gif_atraso * 1000;
      if (s_gif_devido_us < agora) { s_gif_devido_us = agora; }
      sistema_pedir_redesenho();
    } else {
      s_gif_devido_us = esp_timer_get_time() + (int64_t)s_gif_atraso * 1000;
      sistema_pedir_redesenho_completo();
    }
  }
  if (s_l_pronto) { listagem_chegou(); }
  if (!s_c_pronto) { return; }
  s_c_pronto  = false;
  s_c_ocupado = false;
  char atual[sizeof(s_c_caminho)];
  caminho_arquivo(atual, sizeof(atual));
  const bool mesmo_arquivo = (strcmp(atual, s_c_caminho) == 0) && s_nivel == NIVEL_CONTEUDO;
  if (mesmo_arquivo) {
    if (s_c_contar) { s_total_linhas = s_c_total; }
    memcpy(s_linhas, s_c_linhas, sizeof(s_linhas));
    s_n_linhas = s_c_n;
    s_abriu_ok = s_c_abriu;
  }
  if (s_c_de_novo || (mesmo_arquivo && s_c_linha != s_linha)) {
    const bool contar = s_c_contar_de_novo;
    s_c_de_novo = false;
    s_c_contar_de_novo = false;
    pedir_leitura(contar);
  }
  if (mesmo_arquivo) { sistema_pedir_redesenho_completo(); }
}

// Animated GIF on screen: redraws at each frame delay (the drawing asks the
// worker for the next frame). Otherwise it only wakes on a notification or a tap.
static uint32_t periodo_agora(void)
{
  if (s_nivel != NIVEL_CONTEUDO || !s_gif_anima) { return 0; }
  return (uint32_t)s_gif_atraso;
}

// ---------------------------------------------------------------------------
// LIFE CYCLE
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  if (!s_itens) { s_itens = (tab5_arq_item_t*)heap_caps_calloc(MAX_ITENS, sizeof(tab5_arq_item_t), MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (tab5_arq_item_t*)heap_caps_calloc(MAX_ITENS, sizeof(tab5_arq_item_t), MALLOC_CAP_SPIRAM); }
  if (!s_itens || !s_carga) { return false; }
  s_listando = false; s_l_pronto = false; s_l_de_novo = false;
  s_op_rodando = false; s_op_pronto = false;
  s_c_ocupado = false; s_c_pronto = false;
  s_n_itens = s_n_achados = 0;
  s_dlg = DLG_NADA;
  s_linha = 0;
  s_n_voltas = 0;
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  s_job_fase = t.fase;
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, item_altura, item_desenhar, nullptr);
  navegar("");
  return true;
}

static void ao_fechar(void)
{
  if (s_dlg == DLG_NOME) { tecvirt_fechar(&s_kb); }
  s_dlg = DLG_NADA;
  s_n_itens = 0;
  if (s_itens) { heap_caps_free(s_itens); s_itens = nullptr; }
  if (s_carga) { heap_caps_free(s_carga); s_carga = nullptr; }   // closing has already waited for the worker
  if (s_lixo) { heap_caps_free(s_lixo); s_lixo = nullptr; }
  if (s_lixo_carga) { heap_caps_free(s_lixo_carga); s_lixo_carga = nullptr; }
  s_n_linhas = 0;
  tab5_texto_fechar(s_texto);   // closing has already waited for the worker
  s_texto = nullptr;
  s_ver = VER_TEXTO;
  s_img_lendo = false;   // closing the app has already waited for the worker
  soltar_imagem();
  sair_da_selecao();
}

// ---------------------------------------------------------------------------
// DRAWING: icons and items
// ---------------------------------------------------------------------------
static void icone_pasta_pequena(lgfx::LovyanGFX& d, int x, int y, int w, int h)
{
  const uint16_t pasta = TEMA_APP_AMARELO;
  const uint16_t aba   = TEMA_APP_LARANJA;
  const int aba_l = (int)(0.5f  * w);
  const int aba_a = (int)(0.28f * h);
  d.fillRoundRect(x, y,                 aba_l, aba_a,          2, aba);
  d.fillRoundRect(x, y + aba_a - 1,     w,     h - aba_a + 1,  3, pasta);
}

// Sheet with a folded corner; the bottom strip has the type's colour (music,
// image, text, app) - the type reads without reading the extension.
static void icone_arquivo_pequeno(lgfx::LovyanGFX& d, int x, int y, int w, int h, tab5_arq_tipo_t tipo)
{
  const uint16_t pagina = TEMA_TEXTO_2;
  const uint16_t dobra  = TEMA_TEXTO_3;
  const int pl = (int)(0.55f * w);
  const int pa = (int)(0.85f * h);
  const int px = x + (w - pl) / 2;
  const int py = y + (h - pa) / 2;
  const int canto = (pl < pa ? pl : pa) / 3;
  d.fillRoundRect(px, py, pl, pa, 2, pagina);
  d.fillTriangle(px + pl - canto, py, px + pl, py, px + pl, py + canto, TEMA_FUNDO);
  d.fillTriangle(px + pl - canto, py, px + pl, py + canto, px + pl - canto, py + canto, dobra);
  uint16_t cor = dobra;
  switch (tipo) {
    case TAB5_ARQ_MUSICA: cor = TEMA_APP_ROXO; break;
    case TAB5_ARQ_IMAGEM: cor = TEMA_APP_VERDE; break;
    case TAB5_ARQ_TEXTO:  cor = TEMA_APP_AZUL; break;
    case TAB5_ARQ_BERRY:  cor = TEMA_APP_LARANJA; break;
    default: break;
  }
  const int linha_x = px + (int)(0.18f * pl);
  const int linha_l = (int)(0.64f * pl);
  d.fillRect(linha_x, py + (int)(0.50f * pa), linha_l, 3, cor);
  d.fillRect(linha_x, py + (int)(0.70f * pa), linha_l, 3, cor);
}

static void icone_disco(lgfx::LovyanGFX& d, int x, int y, int lado, bool pendrive, bool ativo)
{
  const uint16_t corpo = ativo ? (pendrive ? TEMA_APP_AZUL : TEMA_APP_VERDE) : CINZA_BT;
  if (pendrive) {
    d.fillRoundRect(x + lado / 4, y, lado / 2, lado / 4, 3, TEMA_TEXTO_2);           // the connector
    d.fillRoundRect(x + lado / 8, y + lado / 4 - 2, lado * 3 / 4, lado * 3 / 4, 8, corpo);
  } else {
    // SD card: the cut corner and the contacts.
    d.fillRoundRect(x + lado / 8, y, lado * 3 / 4, lado, 8, corpo);
    d.fillTriangle(x + lado / 8, y, x + lado / 8 + lado / 4, y, x + lado / 8, y + lado / 4, TEMA_FUNDO);
    for (int k = 0; k < 4; ++k) { d.fillRect(x + lado / 3 + k * lado / 10, y + 6, lado / 16, lado / 5, TEMA_FUNDO); }
  }
}

static int item_altura(int, void*) { return s_nivel == NIVEL_DISCOS ? LIN_DISCO : LIN_LISTA; }

// A disk row: icon, name, space, recycle bin and the buttons on the right
// (Ejetar/eject, Esvaziar/empty). The buttons are not registered: the tap on
// the list decides by 'x' (see tocar_disco) - the list scrolls, the register
// does not.
static const int DISCO_BT_L = 190, DISCO_BT_A = 56;

static void disco_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura)
{
  const disco_t& dk = s_discos[i];
  const bool pendrive = dk.v.id == TAB5_VOL_PENDRIVE;
  const int x = x0 + MARGEM_X;
  icone_disco(d, x, y + 14, 64, pendrive, dk.v.disponivel);
  const int tx = x + 90;
  const int bt_x = x0 + largura - DISCO_BT_L - 8;
  d.setTextDatum(top_left);
  d.setFont(tab5_fonte_suave(true, 34));
  d.setTextSize(1);
  d.setTextColor(dk.v.disponivel ? TEMA_TEXTO : TEMA_TEXTO_3, TEMA_FUNDO);
  char t[96];
  snprintf(t, sizeof(t), "%s%s%s", dk.v.tipo, dk.v.nome[0] ? "  " : "", dk.v.nome);
  tab5_cortar_com_reticencias(d, t, sizeof(t), bt_x - tx - 12);
  d.drawString(t, tx, y + 10);
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  if (!dk.v.disponivel) {
    d.drawString(pendrive ? tr("saindo...") : tr("indisponível (no computador ou sem cartão)"), tx, y + 56);
  } else {
    char a1[16], a2[16];
    tab5_bytes_txt(a1, sizeof(a1), dk.livre);
    tab5_bytes_txt(a2, sizeof(a2), dk.total);
    snprintf(t, sizeof(t), tr("livres %s de %s"), a1, a2);
    d.drawString(t, tx, y + 56);
    // The used-space bar.
    const int bl = bt_x - tx - 20;
    if (bl > 40 && dk.total) {
      d.fillRoundRect(tx, y + 92, bl, 10, 5, TEMA_SUPERFICIE_ALTA);
      const int usado = (int)((double)(dk.total - dk.livre) / dk.total * bl);
      d.fillRoundRect(tx, y + 92, usado < 10 ? 10 : usado, 10, 5, pendrive ? TEMA_APP_AZUL : TEMA_APP_VERDE);
    }
    if (dk.lixeira > 0) {
      char peso[16];
      tab5_bytes_txt(peso, sizeof(peso), dk.lixeira_bytes);
      snprintf(t, sizeof(t), tr("lixeira: %d item(s) - %s"), dk.lixeira, peso);
      d.drawString(t, tx, y + 110);
    }
  }
  // Buttons on the right.
  if (dk.v.disponivel) {
    int by = y + 12;
    if (pendrive) {
      tab5_botao(d, bt_x, by, DISCO_BT_L, DISCO_BT_A, tr("Ejetar"), TEMA_ALERTA, TEMA_FUNDO);
      by += DISCO_BT_A + 10;
    }
    // Opens the recycle bin (where you restore, delete or empty). Understated on purpose.
    if (dk.lixeira > 0) { tab5_botao(d, bt_x, by, DISCO_BT_L, DISCO_BT_A, tr("Lixeira"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO_2); }
  }
  if (i + 1 < s_n_itens) { d.drawFastHLine(x0 + MARGEM_X, y + LIN_DISCO - 4, largura - MARGEM_X, COR_DIVISA); }
}

// "de /Fotos · 07/11 14:32 · 1.2 MB" - the second line of a recycle-bin item.
static void lixo_detalhe(const tab5_arq_lixo_t& it, char* out, size_t tam)
{
  char destino[96], peso[16];
  if (it.origem[0]) {
    lixo_destino(s_pasta, it.origem, false, destino, sizeof(destino));
    snprintf(out, tam, tr("de %s"), destino);
  } else {
    snprintf(out, tam, "%s", tr("origem desconhecida"));
  }
  if (it.quando) {
    struct tm t;
    tab5_hora_local(it.quando, &t);
    char quando[24];
    strftime(quando, sizeof(quando), "%d/%m %H:%M", &t);
    const size_t k = strlen(out);
    snprintf(out + k, tam - k, " · %s", quando);
  }
  tab5_bytes_txt(peso, sizeof(peso), it.bytes);
  const size_t k = strlen(out);
  snprintf(out + k, tam - k, " · %s", peso);
}

// A recycle-bin item: icon, the name highlighted and, below, where it came
// from, when it was deleted and the size. While selecting: the checkbox
// before the icon.
static void lixo_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura)
{
  const tab5_arq_lixo_t& it = s_lixo[i];
  int x = x0 + MARGEM_X;
  if (s_selecionando) {
    const bool marcado = s_sel[i] != 0;
    const int lado = 40, cy = y + (LIN_LISTA - lado) / 2;
    d.fillRoundRect(x, cy, lado, lado, 6, marcado ? TEMA_DESTAQUE : TEMA_FUNDO);
    d.drawRoundRect(x, cy, lado, lado, 6, marcado ? TEMA_DESTAQUE : TEMA_TEXTO_2);
    if (marcado) {
      tab5_linha_grossa(d, x + 8, cy + 20, x + 17, cy + 29, 2.5f, TEMA_SOBRE_DESTAQUE);
      tab5_linha_grossa(d, x + 17, cy + 29, x + 32, cy + 10, 2.5f, TEMA_SOBRE_DESTAQUE);
    }
    x += lado + 14;
  }
  if (it.pasta) { icone_pasta_pequena(d, x, y + (LIN_LISTA - 34) / 2, 40, 34); }
  else          { icone_arquivo_pequeno(d, x, y + (LIN_LISTA - 48) / 2, 40, 48, tab5_arq_tipo(it.nome)); }
  const int tx = x + 40 + 14;
  const int util = x0 + largura - 16 - tx;
  d.setTextDatum(top_left);
  d.setTextSize(1);
  char t[TAB5_ARQ_NOME_MAX + 4];
  snprintf(t, sizeof(t), "%s", it.nome);
  d.setFont(tab5_fonte_suave(true, 30));
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  tab5_cortar_com_reticencias(d, t, sizeof(t), util);
  d.drawString(t, tx, y + 10);
  char det[200];
  lixo_detalhe(it, det, sizeof(det));
  d.setFont(tab5_fonte_suave(false, 22));
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  tab5_cortar_com_reticencias(d, det, sizeof(det), util);
  d.drawString(det, tx, y + 50);
  if (i + 1 < s_n_itens) { d.drawFastHLine(x0 + MARGEM_X, y + LIN_LISTA - 1, largura - MARGEM_X, COR_DIVISA); }
}

static void item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (s_nivel == NIVEL_DISCOS) { disco_desenhar(d, i, x0, y, largura); return; }
  if (s_nivel == NIVEL_LIXEIRA) { lixo_desenhar(d, i, x0, y, largura); return; }
  const tab5_arq_item_t& it = s_itens[i];
  int x = x0 + MARGEM_X;
  tab5_texto(d, TAM_LISTA);
  d.setTextDatum(top_left);
  const int icone_a = d.fontHeight();
  // The content centred in the item's height; the separating line at the bottom.
  const int y_item = y;
  y += (LIN_LISTA - icone_a) / 2 - 2;
  // While selecting: the checkbox before the icon.
  if (s_selecionando) {
    const bool marcado = s_sel[i] != 0;
    d.fillRoundRect(x, y + 2, icone_a - 4, icone_a - 4, 6, marcado ? TEMA_DESTAQUE : TEMA_FUNDO);
    d.drawRoundRect(x, y + 2, icone_a - 4, icone_a - 4, 6, marcado ? TEMA_DESTAQUE : TEMA_TEXTO_2);
    if (marcado) {
      tab5_linha_grossa(d, x + 8, y + icone_a / 2, x + icone_a / 2 - 4, y + icone_a - 12, 2.5f, TEMA_SOBRE_DESTAQUE);
      tab5_linha_grossa(d, x + icone_a / 2 - 4, y + icone_a - 12, x + icone_a - 12, y + 10, 2.5f, TEMA_SOBRE_DESTAQUE);
    } else {
      d.fillRoundRect(x + 3, y + 5, icone_a - 10, icone_a - 10, 4, TEMA_FUNDO);
    }
    x += icone_a + 6;
  }
  const int util = x0 + largura - x;
  const int tamanho_x = x + (int)(util * 0.74f);
  const int icone_l = 40;
  if (it.tipo == TAB5_ARQ_PASTA) { icone_pasta_pequena(d, x, y, icone_l, icone_a); }
  else                           { icone_arquivo_pequeno(d, x, y, icone_l, icone_a, it.tipo); }
  const int nome_x = x + icone_l + 14;
  char nome[TAB5_ARQ_NOME_MAX + 4];
  snprintf(nome, sizeof(nome), "%s", it.nome);
  tab5_cortar_com_reticencias(d, nome, sizeof(nome), (it.tipo == TAB5_ARQ_PASTA ? x0 + largura - 16 : tamanho_x - 12) - nome_x);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  d.drawString(nome, nome_x, y);
  if (it.tipo != TAB5_ARQ_PASTA) {
    char buf[24];
    tab5_bytes_txt(buf, sizeof(buf), it.bytes);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(buf, tamanho_x, y);
  }
  if (i + 1 < s_n_itens) {
    d.drawFastHLine(x0 + MARGEM_X, y_item + LIN_LISTA - 1, largura - MARGEM_X, COR_DIVISA);
  }
}

static void texto_cabendo(lgfx::LovyanGFX& d, const char* txt, int x, int y, int largura)
{
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", txt);
  tab5_cortar_com_reticencias(d, buf, sizeof(buf), largura);
  d.drawString(buf, x, y);
}

static void milhar(char* out, size_t n, int v)
{
  if (v >= 1000000) { snprintf(out, n, "%d.%03d.%03d", v / 1000000, (v / 1000) % 1000, v % 1000); }
  else if (v >= 1000) { snprintf(out, n, "%d.%03d", v / 1000, v % 1000); }
  else { snprintf(out, n, "%d", v); }
}

// ---------------------------------------------------------------------------
// DRAWING: the content (text and image)
// ---------------------------------------------------------------------------
static int desenhar_texto(lgfx::LovyanGFX& d, const area_t& a, int y)
{
  const int px = TAM_LEITURA[s_tam_leitura];
  d.setFont(tab5_fonte_suave(false, px));
  d.setTextSize(1);
  d.setTextDatum(top_left);
  const int alt_linha = px + px / 4;
  const int respiro = px / 3;
  const int recuo = px;
  const int x = a.x + MARGEM_X;
  const int largura = a.largura - 2 * MARGEM_X;
  const int fundo = a.y + a.altura - 10;
  static char vis[CONVERSA_MAX_LINHAS][CONVERSA_LINHA_BYTES];
  int cabem = 0;
  for (int i = 0; i < s_n_linhas; ++i) {
    const char* txt = s_linhas[i][0] ? s_linhas[i] : " ";
    const int n = conversa_quebrar_linhas(d, txt, largura - recuo, vis, CONVERSA_MAX_LINHAS);
    const int altura = n * alt_linha;
    if (y + altura > fundo && cabem > 0) { break; }
    for (int k = 0; k < n; ++k) {
      if (y + alt_linha > fundo) { break; }
      d.setTextColor(k == 0 ? TEMA_TEXTO : TEMA_TEXTO_2, TEMA_FUNDO);
      d.drawString(vis[k], k == 0 ? x : x + recuo, y);
      y += alt_linha;
    }
    y += respiro;
    ++cabem;
    if (y >= fundo) { break; }
  }
  if (s_n_linhas == 0) {
    d.setTextColor(s_abriu_ok ? TEMA_TEXTO_2 : TEMA_PERIGO, TEMA_FUNDO);
    d.drawString(s_abriu_ok ? tr("(arquivo vazio ou fim do conteúdo)") : tr("NÃO CONSEGUI ABRIR ESTE ARQUIVO"), x, y);
    y += alt_linha;
  }
  if (y < fundo) { tab5_tela_limpar(a.x, y, a.largura, a.y + a.altura - y, TEMA_FUNDO); }
  tab5_fonte_corpo(d);
  return cabem > 0 ? cabem : 1;
}

static void desenhar_barra(lgfx::LovyanGFX& d, const area_t& a)
{
  const int x = a.x + MARGEM_X, l = a.largura - 2 * MARGEM_X;
  const int y = a.y + BARRA_Y;
  d.fillRoundRect(x, y, l, BARRA_A, BARRA_A / 2, TEMA_SUPERFICIE_ALTA);
  if (s_total_linhas <= 0) { return; }
  int ini = (int)((int64_t)l * s_linha / s_total_linhas);
  int fim = (int)((int64_t)l * (s_linha + s_cabem) / s_total_linhas);
  if (fim > l) { fim = l; }
  if (fim - ini < BARRA_A) { ini = (fim - BARRA_A > 0) ? fim - BARRA_A : 0; fim = ini + BARRA_A; }
  d.fillRoundRect(x + ini, y, fim - ini, BARRA_A, BARRA_A / 2, TEMA_DESTAQUE);
}

static void desenhar_imagem(lgfx::LovyanGFX& d, const area_t& a, int x, int y)
{
  const int caixa_l = a.largura - 2 * (x - a.x);
  const int caixa_a = (a.y + a.altura - 10) - y;
  if (caixa_l <= 0 || caixa_a <= 0) { return; }
  char caminho[sizeof(s_img_px_caminho)];
  caminho_arquivo(caminho, sizeof(caminho));
  const bool mesma = s_img_px_caminho[0] && !strcmp(s_img_px_caminho, caminho);

  // The worker writes into the sprite: no painting while it works. For a GIF
  // already on screen, the image stays as it is until the next frame arrives.
  if (s_img_lendo && s_gif_anima && mesma) { return; }

  // The sprite is the whole box; the service draws the image inside it,
  // reduced to fit and centred, on a black background.
  const bool pronta = !s_img_lendo && mesma && s_img_spr && s_img_px_l == (uint32_t)caixa_l && s_img_px_a == (uint32_t)caixa_a;
  if (pronta) {
    s_img_spr->pushSprite(&d, a.x + (a.largura - caixa_l) / 2, y);
    // Animated GIF: after the frame's delay, the worker decodes the next one.
    if (s_gif_anima && esp_timer_get_time() + 16000 >= s_gif_devido_us) {
      s_gif_pedido_us = esp_timer_get_time();
      s_img_lida = false;
      s_img_lendo = sistema_trabalho(trabalho_ler_imagem, nullptr);
    }
    return;
  }
  if (!s_img_lendo && mesma && !s_img_lida_ok && s_img_lida) {
    d.setTextColor(TEMA_PERIGO, TEMA_FUNDO);
    d.drawString(tr("NÃO CONSEGUI LER ESTA IMAGEM"), x, y);
    return;
  }
  if (!s_img_lendo) {
    soltar_imagem();
    s_img_spr = tab5_sprite_novo(&d, caixa_l, caixa_a);
    if (!s_img_spr) {
      d.setTextColor(TEMA_PERIGO, TEMA_FUNDO);
      d.drawString(tr("SEM MEMÓRIA PARA A IMAGEM"), x, y);
      return;
    }
    s_img_px_l = (uint32_t)caixa_l;
    s_img_px_a = (uint32_t)caixa_a;
    snprintf(s_img_px_caminho, sizeof(s_img_px_caminho), "%s", caminho);
    s_img_lida = false;
    s_gif_pedido_us = esp_timer_get_time();
    s_img_lendo = sistema_trabalho(trabalho_ler_imagem, nullptr);
    if (!s_img_lendo) { soltar_imagem(); }
  }
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(tr("carregando imagem..."), x, y);
}

// ---------------------------------------------------------------------------
// DRAWING: dialogues, footer and the screen
// ---------------------------------------------------------------------------
static void desenhar_dialogo(lgfx::LovyanGFX& d, const area_t& a)
{
  if (s_dlg == DLG_NOME) {
    tecvirt_posicionar(&s_kb, a);
    if (TUDO) {
      d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
      d.setFont(tab5_fonte_suave(true, 40));
      d.setTextSize(1);
      d.setTextDatum(middle_left);
      d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
      d.drawString(s_nome_criar ? tr("Nova pasta") : tr("Renomear"), a.x + 24, a.y + 60);
      d.setFont(tab5_fonte_suave(false, 24));
      d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
      d.drawString(tr("ENVIAR confirma; X cancela."), a.x + 24, a.y + 230);
    }
    char campo[TAB5_ARQ_NOME_MAX + 2];
    snprintf(campo, sizeof(campo), "%s_", s_nome_txt);
    const uint16_t escuro = TEMA_SUPERFICIE_ALTA;
    d.fillRoundRect(a.x + 24, a.y + 110, a.largura - 48, 80, 14, escuro);
    d.setFont(tab5_fonte_suave(false, 34));
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, escuro);
    tab5_cortar_com_reticencias(d, campo, sizeof(campo), a.largura - 88);
    d.drawString(campo, a.x + 44, a.y + 150);
    d.setTextDatum(top_left);
    tecvirt_pintar(&s_kb, d, TUDO);
    return;
  }
  if (s_dlg == DLG_LIXO_ITEM) {
    // A recycle-bin item: what to do with it (three buttons, one above the other).
    const int bl = a.largura - 80, bh = 400;
    const int bx = 40, by = (a.altura - bh) / 2;
    const uint16_t caixa = TEMA_SUPERFICIE;
    d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
    d.fillRoundRect(a.x + bx, a.y + by, bl, bh, 20, caixa);
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setFont(tab5_fonte_suave(true, 34));
    d.setTextColor(TEMA_TEXTO, caixa);
    texto_cabendo(d, s_lixo_alvo[0].nome, a.x + bx + 30, a.y + by + 24, bl - 60);
    char det[200];
    lixo_detalhe(s_lixo_alvo[0], det, sizeof(det));
    d.setFont(tab5_fonte_suave(false, 24));
    d.setTextColor(TEMA_TEXTO_2, caixa);
    texto_cabendo(d, det, a.x + bx + 30, a.y + by + 76, bl - 60);
    botao(d, a, bx + 30, by + 124, bl - 60, 72, tr("Restaurar"), TEMA_SUCESSO, TEMA_FUNDO, BT_DLG_RESTAURAR);
    botao(d, a, bx + 30, by + 206, bl - 60, 72, tr("Excluir de vez"), TEMA_PERIGO, TEMA_SOBRE_DESTAQUE, BT_DLG_EXCLUIR);
    botao(d, a, bx + 30, by + 288, bl - 60, 72, tr("Cancelar"), CINZA_BT, TEMA_TEXTO, BT_DLG_NAO);
    return;
  }
  // Confirm: box in the middle.
  const int bl = a.largura - 80, bh = 330;
  const int bx = 40, by = (a.altura - bh) / 2;
  d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
  d.fillRoundRect(a.x + bx, a.y + by, bl, bh, 20, TEMA_SUPERFICIE);
  d.setFont(tab5_fonte_suave(true, 34));
  d.setTextSize(1);
  d.setTextDatum(top_left);
  d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
  char t[96], t2[96];
  if (s_dlg == DLG_APAGAR) {
    snprintf(t, sizeof(t), tr("Mandar %d item(s) para a lixeira?"), s_n_sel);
    snprintf(t2, sizeof(t2), "%s", tr("Dá para recuperar pela lixeira até esvaziar."));
  } else if (s_dlg == DLG_LIXO_EXCLUIR) {
    snprintf(t, sizeof(t), tr("Excluir de vez %d item(s)?"), s_lixo_alvo_n);
    snprintf(t2, sizeof(t2), "%s", tr("Não dá para recuperar depois."));
  } else {
    char peso[16];
    tab5_bytes_txt(peso, sizeof(peso), s_esvaziar_bytes);
    snprintf(t, sizeof(t), tr("Apagar de vez %d item(s) da lixeira (%s)?"), s_esvaziar_itens, tr(tab5_volume_tipo(s_esvaziar_raiz)));
    snprintf(t2, sizeof(t2), tr("Total: %s. Apaga de vez, sem recuperação."), peso);
  }
  texto_cabendo(d, t, a.x + bx + 30, a.y + by + 40, bl - 60);
  d.setFont(tab5_fonte_suave(false, 26));
  d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
  texto_cabendo(d, t2, a.x + bx + 30, a.y + by + 110, bl - 60);
  const int bw = (bl - 90) / 2;
  botao(d, a, bx + 30, by + bh - 110, bw, 80, tr("Cancelar"), CINZA_BT, TEMA_TEXTO, BT_DLG_NAO);
  botao(d, a, bx + 60 + bw, by + bh - 110, bw, 80, s_dlg == DLG_APAGAR ? tr("Apagar") : s_dlg == DLG_LIXO_EXCLUIR ? tr("Excluir") : tr("Esvaziar"),
        TEMA_PERIGO, TEMA_SOBRE_DESTAQUE, BT_DLG_SIM);
}

// A row of equal-width buttons in the footer.
typedef struct { const char* txt; bt_id_t id; uint16_t cor; uint16_t tinta; } bt_def_t;
static bt_def_t bt_def(const char* txt, bt_id_t id, uint16_t cor, uint16_t tinta) { return { txt, id, cor, tinta }; }
static void fileira(lgfx::LovyanGFX& d, const area_t& a, int y, const bt_def_t* b, int n)
{
  const int g = 10;
  const int l = (a.largura - 2 * MARGEM_X - (n - 1) * g) / n;
  for (int i = 0; i < n; ++i) {
    botao(d, a, MARGEM_X + i * (l + g), y, l, RODAPE_A - 12, b[i].txt, b[i].cor, b[i].tinta,
          b[i].id);
  }
}

// The footer: copy progress (if any) and the current row of actions.
// Returns the height it took (the list ends above it).
static int desenhar_rodape(lgfx::LovyanGFX& d, const area_t& a)
{
  int ocupado = 0;
  int y = a.altura - 8;
  // 1. The row of actions.
  bt_def_t b[5];
  int n = 0;
  bool recortar = false;
  const int na = tab5_arq_area(&recortar);
  if (s_nivel == NIVEL_PASTA && s_selecionando) {
    const bool mexe = s_gravavel;
    b[n++] = bt_def(tr("Copiar"), s_n_sel ? BT_COPIAR : BT_NADA, s_n_sel ? TEMA_DESTAQUE : CINZA_BT, s_n_sel ? TEMA_FUNDO : TEMA_TEXTO_3);
    b[n++] = bt_def(tr("Recortar"), (mexe && s_n_sel) ? BT_RECORTAR : BT_NADA, (mexe && s_n_sel) ? TEMA_DESTAQUE : CINZA_BT, (mexe && s_n_sel) ? TEMA_FUNDO : TEMA_TEXTO_3);
    b[n++] = bt_def(tr("Apagar"), (mexe && s_n_sel) ? BT_APAGAR : BT_NADA, (mexe && s_n_sel) ? TEMA_PERIGO : CINZA_BT, TEMA_SOBRE_DESTAQUE);
    b[n++] = bt_def(tr("Renomear"), (mexe && s_n_sel == 1) ? BT_RENOMEAR : BT_NADA, (mexe && s_n_sel == 1) ? TEMA_DESTAQUE : CINZA_BT, (mexe && s_n_sel == 1) ? TEMA_FUNDO : TEMA_TEXTO_3);
    b[n++] = bt_def(tr("Pronto"), BT_SAIR_SEL, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  } else if (s_nivel == NIVEL_LIXEIRA && s_selecionando) {
    static char restaurar[32], excluir[32];
    snprintf(restaurar, sizeof(restaurar), tr("Restaurar (%d)"), s_n_sel);
    snprintf(excluir, sizeof(excluir), tr("Excluir (%d)"), s_n_sel);
    b[n++] = bt_def(restaurar, s_n_sel ? BT_LIXO_RESTAURAR : BT_NADA, s_n_sel ? TEMA_SUCESSO : CINZA_BT, s_n_sel ? TEMA_FUNDO : TEMA_TEXTO_3);
    b[n++] = bt_def(excluir, s_n_sel ? BT_LIXO_EXCLUIR : BT_NADA, s_n_sel ? TEMA_PERIGO : CINZA_BT, TEMA_SOBRE_DESTAQUE);
    b[n++] = bt_def(tr("Cancelar"), BT_SAIR_SEL, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  } else if (s_nivel == NIVEL_PASTA && na > 0) {
    static char colar[40];
    snprintf(colar, sizeof(colar), tr("%s aqui (%d)"), recortar ? tr("Mover") : tr("Colar"), na);
    b[n++] = bt_def(colar, s_gravavel ? BT_COLAR : BT_NADA, s_gravavel ? TEMA_SUCESSO : CINZA_BT, s_gravavel ? TEMA_FUNDO : TEMA_TEXTO_3);
    b[n++] = bt_def(tr("Esquecer"), BT_LIMPAR_AREA, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  }
  if (n) {
    y -= RODAPE_A;
    tab5_tela_limpar(a.x, a.y + y, a.largura, RODAPE_A, TEMA_FUNDO);
    fileira(d, a, y + 6, b, n);
    ocupado += RODAPE_A;
  }
  // 2. The copy job: running, or finished and not yet seen.
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  if (t.fase != TAB5_ARQ_PARADO) {
    y -= PROG_A;
    const int x = a.x + MARGEM_X;
    const int bt_l = 170;
    const int l = a.largura - 2 * MARGEM_X - bt_l - 16;
    tab5_tela_limpar(a.x, a.y + y, a.largura, PROG_A, TEMA_FUNDO);
    const bool roda = tab5_arq_ocupado();
    char linha[160];
    const int pct = t.bytes ? (int)(t.bytes_feitos * 100 / t.bytes) : t.itens ? t.itens_feitos * 100 / t.itens : 0;
    switch (t.fase) {
      case TAB5_ARQ_CONTANDO: snprintf(linha, sizeof(linha), tr("Medindo o que vai copiar...")); break;
      case TAB5_ARQ_APAGANDO: snprintf(linha, sizeof(linha), "%s", t.mover ? tr("Movendo: limpando a origem") : tr("Esvaziando a lixeira...")); break;
      case TAB5_ARQ_COPIANDO:
        snprintf(linha, sizeof(linha), "%s %d%% · %d/%d · %s", t.mover ? tr("Movendo") : tr("Copiando"), pct,
                 t.itens_feitos, t.itens, t.atual);
        break;
      case TAB5_ARQ_FEITO:
        snprintf(linha, sizeof(linha), tr("Pronto: %d arquivo(s)%s"), t.itens_feitos, t.mover ? tr(" movidos") : "");
        break;
      case TAB5_ARQ_CANCELADO: snprintf(linha, sizeof(linha), tr("Cancelado (%d arquivo(s) ficaram)."), t.itens_feitos); break;
      default: snprintf(linha, sizeof(linha), tr("Falhou: %s"), t.erro[0] ? t.erro : tr("veja o log")); break;
    }
    d.setFont(tab5_fonte_suave(false, 24));
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(t.fase == TAB5_ARQ_FALHOU ? TEMA_PERIGO : TEMA_TEXTO, TEMA_FUNDO);
    texto_cabendo(d, linha, x, a.y + y + 6, l);
    d.fillRoundRect(x, a.y + y + 42, l, 12, 6, TEMA_SUPERFICIE_ALTA);
    const int feito = roda ? l * (pct > 100 ? 100 : pct) / 100 : (t.fase == TAB5_ARQ_FEITO ? l : 0);
    if (feito > 0) { d.fillRoundRect(x, a.y + y + 42, feito < 12 ? 12 : feito, 12, 6, TEMA_DESTAQUE); }
    botao(d, a, MARGEM_X + l + 16, y + 6, bt_l, PROG_A - 14, roda ? tr("Cancelar") : tr("OK"),
          roda ? TEMA_TEXTO_3 : TEMA_DESTAQUE, roda ? TEMA_TEXTO : TEMA_FUNDO, roda ? BT_CANCELAR_JOB : BT_OK_JOB);
    ocupado += PROG_A;
  }
  return ocupado;
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  char linha[TAB5_ARQ_CAMINHO_MAX + 64], t1[16], t2[16], t3[16];
  const int x = a.x + MARGEM_X;
  tab5_texto(d, TAM_BASE);
  d.setTextDatum(top_left);
  pintura_iniciar(&s_p);
  s_n_bts = 0;

  if (s_dlg != DLG_NADA) { lista_esconder(&s_lista); desenhar_dialogo(d, a); return; }

  if (s_precisa_ler) { s_precisa_ler = false; pedir_leitura(false); }

  const bool conteudo = s_nivel == NIVEL_CONTEUDO;
  const bool texto = conteudo && s_ver == VER_TEXTO;

  // ---- header: [< VOLTAR] title [buttons on the right] -----------------
  int titulo_x = x;
  int titulo_fim = a.x + a.largura - MARGEM_X;
  if (s_nivel != NIVEL_DISCOS) {
    botao(d, a, MARGEM_X, TOPO_Y, VOLTAR_L, TOPO_A, tr("< VOLTAR"), TEMA_DESTAQUE, TEMA_FUNDO, BT_VOLTAR);
    titulo_x = x + VOLTAR_L + 20;
  }
  auto direita = [&](const char* txt, int l, uint16_t cor, uint16_t tinta, bt_id_t id) {
    titulo_fim -= l;
    botao(d, a, titulo_fim - a.x, TOPO_Y, l, TOPO_A, txt, cor, tinta, id);
    titulo_fim -= 12;
  };
  if (texto) { direita("Aa", AA_L, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO, BT_AA); }
#if M5_COM_BERRY
  if (texto && tab5_arq_tipo(s_arquivo) == TAB5_ARQ_BERRY && strcmp(s_pasta, TAB5_SD_APPS "/Berry")) {
    direita(tr("Instalar"), 190, TEMA_APP_VERDE, TEMA_FUNDO, BT_INSTALAR);
  }
#endif
  if (s_nivel == NIVEL_PASTA && s_selecionando) {
    direita(tr("Todos"), 150, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO, BT_TODOS);
  } else if (s_nivel == NIVEL_PASTA) {
    if (s_gravavel) { direita(tr("+ Pasta"), 160, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO, BT_NOVA_PASTA); }
    // COPY TO MUSIC: there is music here, there is a music folder, and we are not in it.
    if (s_tem_musica && s_musica_raiz[0] && strncmp(s_pasta, s_musica_raiz, strlen(s_musica_raiz))) {
      direita(tr("Para Música"), 230, TEMA_APP_ROXO, TEMA_TEXTO, BT_PARA_MUSICA);
    }
  }
  if (s_nivel == NIVEL_LIXEIRA && !s_selecionando && s_n_itens > 0) {
    // Understated on purpose: emptying deletes for good (the red is only on the confirmation).
    direita(tr("Esvaziar"), 190, TEMA_SUPERFICIE_ALTA, TEMA_TEXTO_2, BT_ESVAZIAR);
  }
  tab5_titulo(d, 36);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  char titulo_lixo[64];
  if (s_nivel == NIVEL_LIXEIRA) { snprintf(titulo_lixo, sizeof(titulo_lixo), tr("Lixeira - %s"), tr(tab5_volume_tipo(s_pasta))); }
  const char* titulo = s_nivel == NIVEL_DISCOS ? tr("Arquivos")
                     : s_nivel == NIVEL_LIXEIRA ? titulo_lixo
                     : conteudo ? s_arquivo
                     : eh_raiz_de_disco(s_pasta) ? tr(tab5_volume_tipo(s_pasta))
                     : strrchr(s_pasta, '/') + 1;
  if (titulo_fim > titulo_x + 40) {
    texto_cabendo(d, titulo, titulo_x, a.y + TOPO_Y + TOPO_A / 2, titulo_fim - titulo_x);
  }
  d.setTextDatum(top_left);

  // ---- description -----------------------------------------------------------
  linha[0] = '\0';
  uint16_t cor_desc = TEMA_TEXTO_2;
  if (s_msg[0] && !conteudo) {
    snprintf(linha, sizeof(linha), "%s", s_msg);
    cor_desc = s_msg_erro ? TEMA_ALERTA : TEMA_SUCESSO;
  } else if (s_nivel == NIVEL_DISCOS) {
    snprintf(linha, sizeof(linha), tr("Toque num disco para abrir."));
  } else if (s_nivel == NIVEL_PASTA) {
    char trilha[160];
    trilha_de(s_pasta, trilha, sizeof(trilha));
    if (s_selecionando)       { snprintf(linha, sizeof(linha), tr("%d selecionado(s) - toque para marcar"), s_n_sel); }
    else if (!s_gravavel)     { snprintf(linha, sizeof(linha), tr("%s  (só leitura)"), trilha); }
    else if (s_n_achados > s_n_itens) { snprintf(linha, sizeof(linha), tr("%s  ·  %d de %d"), trilha, s_n_itens, s_n_achados); }
    else                      { snprintf(linha, sizeof(linha), tr("%s  ·  segure um item para selecionar"), trilha); }
  } else if (s_nivel == NIVEL_LIXEIRA) {
    char peso[16];
    tab5_bytes_txt(peso, sizeof(peso), s_lixo_bytes);
    if (s_selecionando) { snprintf(linha, sizeof(linha), tr("%d selecionado(s) - toque para marcar"), s_n_sel); }
    else                { snprintf(linha, sizeof(linha), tr("%d item(s)  ·  %s  ·  segure um item para selecionar"), s_n_itens, peso); }
  } else if (s_ver != VER_TEXTO) {
    if (s_img_largura) { snprintf(linha, sizeof(linha), tr("imagem %ux%u"), (unsigned)s_img_largura, (unsigned)s_img_altura); }
    else               { snprintf(linha, sizeof(linha), tr("imagem")); }
  }
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextSize(1);
  d.setTextColor(cor_desc, TEMA_FUNDO);
  tab5_tela_limpar(a.x, a.y + DESC_Y, a.largura, DIVISA_Y - DESC_Y - 2, TEMA_FUNDO);
  if (!texto) { texto_cabendo(d, linha, x, a.y + DESC_Y, a.largura - 2 * MARGEM_X); }
  d.drawFastHLine(a.x, a.y + DIVISA_Y, a.largura, TEMA_LINHA);
  tab5_texto(d, TAM_BASE);

  // ---- body ------------------------------------------------------------------
  if (conteudo) {
    lista_esconder(&s_lista);
    if (s_ver != VER_TEXTO) { desenhar_imagem(d, a, x, a.y + LISTA_Y); return; }
    s_cabem = desenhar_texto(d, a, a.y + TEXTO_Y);
    const int L = a.largura;
    for (int i = 0; i < 4; ++i) {
      const bool ativo = (i < 2) ? s_linha > 0 : s_linha + s_cabem < s_total_linhas;
      tab5_botao(d, a.x + nav_x(L, i), a.y + NAV_Y, nav_l(L), NAV_A, tr(NAV_NOME[i]),
                 ativo ? ((i == 0 || i == 3) ? TEMA_APP_CIANO : TEMA_DESTAQUE) : TEMA_SUPERFICIE_ALTA,
                 ativo ? TEMA_FUNDO : TEMA_TEXTO_3);
    }
    desenhar_barra(d, a);
    milhar(t1, sizeof(t1), s_linha + 1);
    int ult = s_linha + s_cabem;
    if (ult > s_total_linhas) { ult = s_total_linhas; }
    milhar(t2, sizeof(t2), ult);
    milhar(t3, sizeof(t3), s_total_linhas);
    const int pct = s_total_linhas > 0 ? (int)((int64_t)ult * 100 / s_total_linhas) : 100;
    if (s_total_linhas <= 0) { snprintf(linha, sizeof(linha), s_c_ocupado ? tr("lendo...") : tr("vazio")); }
    else { snprintf(linha, sizeof(linha), tr("linhas %s-%s de %s  (%d%%)"), t1, t2, t3, pct); }
    d.setFont(tab5_fonte_suave(false, 24));
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    texto_cabendo(d, linha, x, a.y + DESC_Y, a.largura - 2 * MARGEM_X);
    tab5_texto(d, TAM_BASE);
    return;
  }

  const int rodape = desenhar_rodape(d, a);
  tab5_texto(d, TAM_LISTA);
  const int y = a.y + LISTA_Y;
  const int fim = a.y + a.altura - 10 - rodape;
  if (s_n_itens == 0) {
    lista_esconder(&s_lista);
    tab5_tela_limpar(a.x, y, a.largura, fim - y, TEMA_FUNDO);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(s_listando ? tr("lendo...") : (s_nivel == NIVEL_DISCOS ? tr("nenhum disco")
                 : s_nivel == NIVEL_LIXEIRA ? tr("A lixeira está vazia") : tr("(vazio)")), x, y);
    return;
  }
  const area_t janela = { a.x, y, a.largura, fim - y };
  lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, TUDO);
}

// ---------------------------------------------------------------------------
// TOUCH
// ---------------------------------------------------------------------------
static void ir_para(int nova, bool lembrar)
{
  if (nova > s_total_linhas - 1) { nova = s_total_linhas - 1; }
  if (nova < 0) { nova = 0; }
  if (nova == s_linha) { return; }
  if (lembrar) {
    if (s_n_voltas == VOLTAS_MAX) { memmove(s_voltas, s_voltas + 1, sizeof(int) * (VOLTAS_MAX - 1)); --s_n_voltas; }
    s_voltas[s_n_voltas++] = s_linha;
  } else {
    s_n_voltas = 0;
  }
  s_linha = nova;
  s_precisa_ler = true;
  sistema_pedir_redesenho_completo();
}

static void pagina_seguinte(void)
{
  if (s_linha + s_cabem < s_total_linhas) { ir_para(s_linha + s_cabem, true); }
}

static void pagina_anterior(void)
{
  if (s_linha <= 0) { return; }
  int volta = s_n_voltas > 0 ? s_voltas[--s_n_voltas] : s_linha - s_cabem;
  if (volta < 0) { volta = 0; }
  s_linha = volta;
  s_precisa_ler = true;
  sistema_pedir_redesenho_completo();
}

// Gathers the selected paths (up to 'max') into 'out'. Returns how many.
static int selecionados(char (*out)[TAB5_ARQ_CAMINHO_MAX], int max)
{
  int n = 0;
  for (int i = 0; i < s_n_itens && n < max; ++i) {
    if (s_sel[i] && juntar(out[n], TAB5_ARQ_CAMINHO_MAX, s_pasta, s_itens[i].nome)) { ++n; }
  }
  return n;
}

static void marcar(int i)
{
  if (i < 0 || i >= s_n_itens) { return; }
  if (!s_sel[i] && s_n_sel >= TAB5_ARQ_ITENS_MAX) {
    mensagem(tr("Até 64 itens por vez."), true);
    return;
  }
  s_sel[i] = !s_sel[i];
  s_n_sel += s_sel[i] ? 1 : -1;
  lista_item_mudou(&s_lista, i);
  sistema_pedir_redesenho_completo();   // the description and the footer change with it
}

static void abrir_dialogo_nome(bool criar, const char* atual)
{
  s_nome_criar = criar;
  snprintf(s_nome_txt, sizeof(s_nome_txt), "%s", atual ? atual : "");
  s_dlg = DLG_NOME;
  tecvirt_abrir(&s_kb, TECVIRT_TEXTO);
  sistema_pedir_redesenho_completo();
}

static void fechar_dialogo(void)
{
  if (s_dlg == DLG_NOME) { tecvirt_fechar(&s_kb); }
  s_dlg = DLG_NADA;
  sistema_pedir_redesenho_completo();
}

static void nome_tecla(int ac)
{
  if (ac == TECVIRT_FECHAR) { fechar_dialogo(); return; }
  if (ac == TECVIRT_ENVIAR) {
    if (!op_livre()) { fechar_dialogo(); return; }
    if (!tab5_arq_nome_valido(s_nome_txt)) { mensagem(tr("Nome inválido (sem / \\ : * ? \" < > |, sem começar com ponto)."), true); fechar_dialogo(); return; }
    snprintf(s_op.nome, sizeof(s_op.nome), "%s", s_nome_txt);
    snprintf(s_op.pasta, sizeof(s_op.pasta), "%s", s_pasta);
    if (!s_nome_criar) { snprintf(s_op_itens[0], sizeof(s_op_itens[0]), "%s", s_nome_alvo); s_op.n = 1; }
    fechar_dialogo();
    sair_da_selecao();
    pedir_op(s_nome_criar ? OP_CRIAR : OP_RENOMEAR);
    return;
  }
  tecvirt_editar(s_nome_txt, sizeof(s_nome_txt), ac);
  sistema_pedir_redesenho();
}

// Opens file 'i' of the folder, by type ("open with").
static void abrir_arquivo(int i)
{
  const tab5_arq_item_t& it = s_itens[i];
  // .avi: the video service opens it (and plays the .mp3 with the same name alongside); the app only shows it.
  if (tab5_video_suportado(it.nome)) {
    char caminho[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
    juntar(caminho, sizeof(caminho), s_pasta, it.nome);
    if (tab5_video_abrir(caminho)) { sistema_abrir_app("Vídeo"); }
    else { mensagem(tr("Não consegui abrir este vídeo."), true); }
    return;
  }
  if (it.tipo == TAB5_ARQ_MUSICA && !op_livre()) { return; }
  if (it.tipo == TAB5_ARQ_MUSICA) {
    snprintf(s_op.pasta, sizeof(s_op.pasta), "%s", s_pasta);
    snprintf(s_op.nome, sizeof(s_op.nome), "%s", it.nome);
    pedir_op(OP_TOCAR);
    return;
  }
  // A .be OPENS as text (to read the script first); installing is the
  // "Instalar" button at the top of that screen. Before, a tap copied it straight to apps/Berry.
  snprintf(s_arquivo, sizeof(s_arquivo), "%s", it.nome);
  s_nivel = NIVEL_CONTEUDO;
  s_linha = 0;
  s_cabem = 1;
  s_n_voltas = 0;
  s_ver = VER_TEXTO;
  if (it.tipo == TAB5_ARQ_IMAGEM) {
    s_ver = VER_IMG;
    s_total_linhas = 0;
  } else {
    s_total_linhas = 0;
    s_n_linhas = 0;
    pedir_leitura(true);
  }
  ESP_LOGI(TAG, "opening %s/%s", s_pasta, s_arquivo);
  sistema_pedir_redesenho_completo();
}

// Tap on a disk row: the buttons on the right by x/dy, otherwise enter it.
static void tocar_disco(int i, int x, int dy)
{
  const disco_t& dk = s_discos[i];
  if (!dk.v.disponivel) { return; }
  const area_t a = sistema_area();
  const int bt_x = a.largura - DISCO_BT_L - 8;
  if (x >= bt_x) {
    const bool pendrive = dk.v.id == TAB5_VOL_PENDRIVE;
    int by = 12;
    if (pendrive) {
      if (dy >= by && dy < by + DISCO_BT_A) { pedir_op(OP_EJETAR); return; }
      by += DISCO_BT_A + 10;
    }
    if (dk.lixeira > 0 && dy >= by && dy < by + DISCO_BT_A) { navegar_lixeira(dk.v.raiz); return; }
  }
  navegar(dk.v.raiz);
}

static void voltar(void)
{
  if (s_nivel == NIVEL_CONTEUDO) {
    s_nivel = NIVEL_PASTA;
    soltar_imagem();
    pedir_listagem(true);
  } else if (s_selecionando) {
    sair_da_selecao();
  } else if (s_nivel == NIVEL_LIXEIRA) {
    navegar("");
    return;
  } else if (s_nivel == NIVEL_PASTA) {
    if (eh_raiz_de_disco(s_pasta)) { navegar(""); return; }
    char pai[TAB5_ARQ_CAMINHO_MAX];
    snprintf(pai, sizeof(pai), "%s", s_pasta);
    if (char* b = strrchr(pai, '/')) { *b = '\0'; }
    navegar(pai);
    return;
  }
  sistema_pedir_redesenho_completo();
}

// Copies the items marked in the recycle bin into s_lixo_alvo. Returns how many.
static int lixo_levar_marcados(void)
{
  s_lixo_alvo_n = 0;
  for (int i = 0; i < s_n_itens && s_lixo_alvo_n < TAB5_ARQ_ITENS_MAX; ++i) {
    if (s_sel[i]) { s_lixo_alvo[s_lixo_alvo_n++] = s_lixo[i]; }
  }
  return s_lixo_alvo_n;
}

// Asks the worker for what is in s_lixo_alvo.
static void lixo_pedir(op_t op)
{
  snprintf(s_op.pasta, sizeof(s_op.pasta), "%s", s_pasta);
  s_op.n = s_lixo_alvo_n;
  sair_da_selecao();
  pedir_op(op);
}

// Asks whether to delete for good what is in s_lixo_alvo.
static void lixo_perguntar_excluir(void)
{
  s_dlg = DLG_LIXO_EXCLUIR;
  s_dlg_desde = tab5_agora_ms();
  sistema_pedir_redesenho_completo();
}

static void tocar_botao(bt_id_t b)
{
  switch (b) {
    case BT_VOLTAR: voltar(); break;
    case BT_AA:
      s_tam_leitura = (s_tam_leitura + 1) % (int)(sizeof(TAM_LEITURA) / sizeof(TAM_LEITURA[0]));
      sistema_pedir_redesenho_completo();
      break;
    case BT_NOVA_PASTA: abrir_dialogo_nome(true, ""); break;
    case BT_INSTALAR:
      if (!op_livre()) { break; }
      caminho_arquivo(s_op_itens[0], sizeof(s_op_itens[0]));
      s_op.n = 1;
      pedir_op(OP_INSTALAR);
      break;
    case BT_PARA_MUSICA:
      if (!op_livre()) { break; }
      // In a folder: the whole folder (the album goes along with its name). At the
      // disk root (which cannot be copied): its loose music files.
      if (!eh_raiz_de_disco(s_pasta)) {
        snprintf(s_op_itens[0], sizeof(s_op_itens[0]), "%s", s_pasta);
        s_op.n = 1;
      } else {
        s_op.n = 0;
        for (int i = 0; i < s_n_itens && s_op.n < TAB5_ARQ_ITENS_MAX; ++i) {
          if (s_itens[i].tipo == TAB5_ARQ_MUSICA && juntar(s_op_itens[s_op.n], TAB5_ARQ_CAMINHO_MAX, s_pasta, s_itens[i].nome)) { ++s_op.n; }
        }
      }
      if (s_op.n) { pedir_op(OP_PARA_MUSICA); }
      break;
    case BT_TODOS: {
      const bool todos = s_n_sel < s_n_itens && s_n_itens <= TAB5_ARQ_ITENS_MAX;
      memset(s_sel, todos ? 1 : 0, sizeof(s_sel));
      s_n_sel = todos ? s_n_itens : 0;
      if (!todos && s_n_itens > TAB5_ARQ_ITENS_MAX) { mensagem(tr("Até 64 itens por vez."), true); }
      lista_itens_mudaram(&s_lista, s_n_itens);
      sistema_pedir_redesenho_completo();
      break;
    }
    case BT_COPIAR:
    case BT_RECORTAR: {
      if (!op_livre()) { break; }
      const int n = selecionados(s_op_itens, TAB5_ARQ_ITENS_MAX);
      const char* ptrs[TAB5_ARQ_ITENS_MAX];
      for (int i = 0; i < n; ++i) { ptrs[i] = s_op_itens[i]; }
      // Text only (the permission rules do not touch the disk): allowed on the UI.
      const bool ok = n && tab5_arq_area_por(ptrs, n, b == BT_RECORTAR);
      sair_da_selecao();
      char t[96];
      snprintf(t, sizeof(t), ok ? tr("%d item(s) %s: abra o destino e toque %s.") : tr("Não deu para %s estes itens."),
               n, b == BT_RECORTAR ? tr("recortados") : tr("copiados"), b == BT_RECORTAR ? tr("Mover aqui") : tr("Colar aqui"));
      if (!ok) { snprintf(t, sizeof(t), tr("Não deu para %s estes itens."), b == BT_RECORTAR ? tr("recortar") : tr("copiar")); }
      mensagem(t, !ok);
      lista_itens_mudaram(&s_lista, s_n_itens);
      break;
    }
    case BT_APAGAR: s_dlg = DLG_APAGAR; sistema_pedir_redesenho_completo(); break;
    case BT_RENOMEAR:
      for (int i = 0; i < s_n_itens; ++i) {
        if (s_sel[i] && juntar(s_nome_alvo, sizeof(s_nome_alvo), s_pasta, s_itens[i].nome)) {
          abrir_dialogo_nome(false, s_itens[i].nome);
          break;
        }
      }
      break;
    case BT_SAIR_SEL: sair_da_selecao(); lista_itens_mudaram(&s_lista, s_n_itens); sistema_pedir_redesenho_completo(); break;
    case BT_COLAR:
      if (!op_livre()) { break; }
      snprintf(s_op.pasta, sizeof(s_op.pasta), "%s", s_pasta);
      pedir_op(OP_COLAR);
      break;
    case BT_LIMPAR_AREA: tab5_arq_area_limpar(); sistema_pedir_redesenho_completo(); break;
    case BT_CANCELAR_JOB: tab5_arq_cancelar(); break;
    case BT_OK_JOB: tab5_arq_trabalho_visto(); s_job_fase = TAB5_ARQ_PARADO; sistema_pedir_redesenho_completo(); break;
    case BT_DLG_NAO: fechar_dialogo(); break;
    case BT_ESVAZIAR:
      if (!op_livre()) { break; }
      snprintf(s_esvaziar_raiz, sizeof(s_esvaziar_raiz), "%s", s_pasta);
      s_esvaziar_itens = s_n_itens;
      s_esvaziar_bytes = s_lixo_bytes;
      s_dlg = DLG_ESVAZIAR;
      s_dlg_desde = tab5_agora_ms();
      sistema_pedir_redesenho_completo();
      break;
    case BT_LIXO_RESTAURAR:
      if (!op_livre()) { break; }
      if (lixo_levar_marcados()) { lixo_pedir(OP_LIXO_RESTAURAR); }
      break;
    case BT_LIXO_EXCLUIR:
      if (!op_livre()) { break; }
      if (lixo_levar_marcados()) { lixo_perguntar_excluir(); }
      break;
    case BT_DLG_RESTAURAR:
      fechar_dialogo();
      if (op_livre()) { lixo_pedir(OP_LIXO_RESTAURAR); }
      break;
    case BT_DLG_EXCLUIR:
      lixo_perguntar_excluir();
      break;
    case BT_DLG_SIM:
      if (!op_livre()) { fechar_dialogo(); break; }
      if (s_dlg == DLG_LIXO_EXCLUIR) {
        fechar_dialogo();
        lixo_pedir(OP_LIXO_EXCLUIR);
      } else if (s_dlg == DLG_APAGAR) {
        s_op.n = selecionados(s_op_itens, TAB5_ARQ_ITENS_MAX);
        sair_da_selecao();
        fechar_dialogo();
        if (s_op.n) { pedir_op(OP_APAGAR); }
      } else {
        snprintf(s_op.pasta, sizeof(s_op.pasta), "%s", s_esvaziar_raiz);
        fechar_dialogo();
        pedir_op(OP_ESVAZIAR);
      }
      break;
    default: break;
  }
}

// LONG TAP (kernel/toque_longo.hpp): a finger held still on a folder item becomes "select".
static toque_longo_t s_lp;

// DRAG on the text: up turns to the next page, down goes back.
static int  s_arr_y0 = -1;
static bool s_arr_valeu = false;

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (s_dlg == DLG_NOME) {
    if (tecvirt_aberto(&s_kb)) {
      int acoes[8];
      const int n = tecvirt_nivel(&s_kb, acoes, 8);
      for (int i = 0; i < n && s_dlg == DLG_NOME; ++i) { nome_tecla(acoes[i]); }
    }
    return;
  }
  if (s_dlg != DLG_NADA) { return; }
  if (s_nivel == NIVEL_CONTEUDO) {
    if (s_ver != VER_TEXTO) { return; }
    if (pressionado) {
      if (s_arr_y0 < 0) { s_arr_y0 = (y >= TEXTO_Y) ? y : -2; s_arr_valeu = false; }
      else if (s_arr_y0 >= 0 && !s_arr_valeu) {
        const int dy = y - s_arr_y0;
        if (dy <= -ARRASTE_PAGINA) { s_arr_valeu = true; sistema_consumir_toque(); pagina_seguinte(); }
        else if (dy >= ARRASTE_PAGINA) { s_arr_valeu = true; sistema_consumir_toque(); pagina_anterior(); }
      }
    } else {
      s_arr_y0 = -1;
    }
    return;
  }
  // Long tap (folders only).
  if ((s_nivel == NIVEL_PASTA || s_nivel == NIVEL_LIXEIRA) && toque_longo(&s_lp, x, y, pressionado) && !s_listando) {
    const int i = lista_item_em(&s_lista, x, y, nullptr);
    if (i >= 0) {
      sistema_consumir_toque();
      if (!s_selecionando) { s_selecionando = true; s_n_sel = 0; memset(s_sel, 0, sizeof(s_sel)); lista_itens_mudaram(&s_lista, s_n_itens); }
      marcar(i);
      sistema_beep(1800, 25);
      return;
    }
  }
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();
  if (s_dlg == DLG_NOME) {
    if (tecvirt_contem(&s_kb, a.x + x, a.y + y)) {
      const int ac = tecvirt_clique(&s_kb, a.x + x, a.y + y);
      if (ac) { nome_tecla(ac); }
    }
    return;
  }
  const bt_id_t b = botao_tocado(x, y);
  if (s_dlg != DLG_NADA) {
    // A double tap must not confirm a permanent delete by accident: YES only
    // counts 0.7 s after the question appears.
    if (b == BT_DLG_SIM && (s_dlg == DLG_ESVAZIAR || s_dlg == DLG_LIXO_EXCLUIR) && tab5_agora_ms() - s_dlg_desde < 700) { return; }
    if (b == BT_DLG_SIM || b == BT_DLG_NAO || b == BT_DLG_RESTAURAR || b == BT_DLG_EXCLUIR) { tocar_botao(b); }
    return;
  }
  if (b != BT_NADA) { tocar_botao(b); return; }

  if (s_nivel == NIVEL_CONTEUDO) {
    if (s_ver != VER_TEXTO) { return; }
    if (y >= NAV_Y && y < NAV_Y + NAV_A) {
      for (int i = 0; i < 4; ++i) {
        if (x < nav_x(a.largura, i) || x >= nav_x(a.largura, i) + nav_l(a.largura)) { continue; }
        switch (i) {
          case 0: ir_para(0, false); break;
          case 1: pagina_anterior(); break;
          case 2: pagina_seguinte(); break;
          default: ir_para(s_total_linhas - (s_cabem > 1 ? s_cabem : 1), false); break;
        }
        return;
      }
      return;
    }
    if (y >= BARRA_TOQUE_Y && y < BARRA_TOQUE_Y + BARRA_TOQUE_A && s_total_linhas > 0) {
      const int l = a.largura - 2 * MARGEM_X;
      int px = x - MARGEM_X;
      if (px < 0) { px = 0; }
      if (px > l) { px = l; }
      ir_para((int)((int64_t)s_total_linhas * px / l), false);
    }
    return;
  }

  if (s_listando) { return; }   // the list on screen is about to be replaced
  int dy = 0;
  const int i = lista_item_tocado(&s_lista, x, y, &dy);
  if (i < 0 || i >= s_n_itens) { return; }
  if (s_nivel == NIVEL_DISCOS) { tocar_disco(i, x, dy); return; }
  if (s_selecionando) { marcar(i); return; }
  if (s_nivel == NIVEL_LIXEIRA) {
    // One item: the dialogue (restore / delete for good / cancel).
    if (!op_livre()) { return; }
    s_lixo_alvo[0] = s_lixo[i];
    s_lixo_alvo_n = 1;
    s_dlg = DLG_LIXO_ITEM;
    sistema_pedir_redesenho_completo();
    return;
  }
  if (s_itens[i].tipo == TAB5_ARQ_PASTA) {
    char c[TAB5_ARQ_CAMINHO_MAX];
    if (juntar(c, sizeof(c), s_pasta, s_itens[i].nome)) { navegar(c); }
    else { mensagem(tr("Caminho longo demais."), true); }
    return;
  }
  abrir_arquivo(i);
}

static void ao_teclar(const char* txt, uint8_t codigo, uint8_t)
{
  if (s_dlg != DLG_NOME) { return; }
  if (codigo == TAB5_TECLA_ENTER)       { nome_tecla(TECVIRT_ENVIAR); }
  else if (codigo == TAB5_TECLA_ESC)    { nome_tecla(TECVIRT_FECHAR); }
  else if (codigo == TAB5_TECLA_APAGAR) { nome_tecla(TECVIRT_APAGAR); }
  else if (!codigo && txt && txt[0] && strlen(s_nome_txt) + strlen(txt) < sizeof(s_nome_txt)) {
    strcat(s_nome_txt, txt);
    sistema_pedir_redesenho();
  }
}

// ---------------------------------------------------------------------------
// HOME SCREEN ICON
// ---------------------------------------------------------------------------
static void icone_arquivos(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(30, 48, 70);
  const uint16_t pasta = d.color565(240, 190, 90);
  const uint16_t aba   = d.color565(205, 155, 60);
  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);
  #define IX(f) (x + (int)((f) * lado))
  #define IY(f) (y + (int)((f) * lado))
  // ORDER MATTERS: tab, sheet, body - each layer covers the previous one's seam.
  d.fillRoundRect(IX(0.16f), IY(0.26f), (int)(0.34f * lado), (int)(0.14f * lado), lado / 22, aba);
  d.fillRoundRect(IX(0.26f), IY(0.30f), (int)(0.48f * lado), (int)(0.10f * lado), lado / 30, d.color565(245, 245, 245));
  d.fillRoundRect(IX(0.16f), IY(0.36f), (int)(0.68f * lado), (int)(0.40f * lado), lado / 16, pasta);
  #undef IX
  #undef IY
}

// 'extern' is MANDATORY: in C++ a const at file scope has internal
// linkage, and apps.cpp would not see this symbol.
extern const app_t APP_ARQUIVOS = {
  .nome  = "Arquivos",
  .cor   = TFT_SKYBLUE,
  .perfil = { },
  // The SD card stays mounted while the app is open (it is the main disk).
  // Without a card the app opens anyway: the flash drive may be there.
  .usa_sd = true,
  .exige_sd = false,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_ms = 0,
  .periodo_agora = periodo_agora,
  .avisos = AVISO_TRABALHO | AVISO_VOLUMES | AVISO_ARQUIVOS,
  .ao_aviso = ao_aviso,
  .icone = icone_arquivos,
  .fundo_tema = true,   // the system clears the area with TEMA_FUNDO
};
