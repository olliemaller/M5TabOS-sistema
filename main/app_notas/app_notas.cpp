// APP: Notas (Notes) - short texts saved on the SD card (services/notas).
//
// Two screens:
//   LIST    the notes, newest first; "+ Nova" (new) at the top. Tap opens;
//           hold selects the note and shows Copiar / Apagar (copy / delete).
//   EDITOR  the text (kernel/editor; holding a finger opens Copiar tudo / Colar - copy all / paste).
//           Back saves; closing the app and idle also save. The note's title is the first line.
//
// The app is only the interface: the SD card belongs to the service (services/notas, through the
// tab5_sd ports), and is only touched on the system worker.

#include "app.h"
#include "lista.hpp"
#include "editor.hpp"
#include "toque_longo.hpp"
#include "notas.h"
#include "transferencia.h"
#include "tab5_unicode.hpp"
#include "idioma.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_tela.hpp"
#include "tab5_hora.hpp"
#include "tab5_teclado.hpp"
#include "trabalho.hpp"
#include "esp_heap_caps.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const int TOPO_A = 96;    // strip of buttons at the top
static const int ITEM_A = 96;
static const int BT_A   = 72;
static const int NOTAS_MAX = 200;

typedef enum { TELA_LISTA, TELA_EDITOR } tela_t;
typedef enum { DLG_NADA, DLG_APAGAR } dlg_t;
typedef enum { FEZ_NADA, FEZ_LISTA, FEZ_ABRIR, FEZ_COPIAR } fez_t;

static tela_t        s_tela = TELA_LISTA;
static dlg_t         s_dlg  = DLG_NADA;
static nota_t*       s_itens = nullptr;      // PSRAM, NOTAS_MAX, only with the app open
static int           s_n = 0;                // -1 = no SD card
static int           s_sel = -1;             // the one chosen by the long press
static char          s_prox[NOTA_ARQ_MAX];   // the file for the next new note
static lista_t       s_lista;
static toque_longo_t s_lp;
static bool          s_forcar = false;

static char*    s_txt = nullptr;             // PSRAM, NOTA_MAX, the open text
static char     s_arq[NOTA_ARQ_MAX];         // the file of the open note
static editor_t s_ed;

// ---------------------------------------------------------------------------
// THE SD CARD, ONLY ON THE WORKER (kernel/trabalho.hpp). One job at a time: while
// 's_ocupado', the UI reads neither s_itens nor s_txt. The result comes back via
// AVISO_TRABALHO, with 's_fez' saying what finished.
// ---------------------------------------------------------------------------
static bool             s_ocupado = false;   // only the UI touches it
static std::atomic<int> s_fez{FEZ_NADA};
static int              s_n_res = 0;         // written by the job, read on the notice
static int              s_alvo = -1;         // the job's note (index in s_itens)
static bool             s_res_ok = false;

static void trab_listar(void*)  { s_n_res = notas_listar(s_itens, NOTAS_MAX, s_prox); s_fez = FEZ_LISTA; }
static void trab_abrir(void*)   { s_res_ok = notas_ler(s_itens[s_alvo].arquivo, s_txt, NOTA_MAX); s_fez = FEZ_ABRIR; }
static void trab_copiar(void*)
{
  s_res_ok = notas_ler(s_itens[s_alvo].arquivo, s_txt, NOTA_MAX) && transf_copiar(s_txt);
  s_fez = FEZ_COPIAR;
}
static void trab_apagar(void*)  { s_res_ok = notas_apagar(s_itens[s_alvo].arquivo); trab_listar(nullptr); }
static void trab_gravar_e_listar(void*)
{
  s_res_ok = notas_gravar(s_arq, s_txt);
  trab_listar(nullptr);
}

static void pedir(trabalho_t fn, int alvo)
{
  if (s_ocupado) { return; }
  s_alvo = alvo;
  if (sistema_trabalho(fn, nullptr)) { s_ocupado = true; }
}

// SAVE ON CLOSE / ON IDLE: the service writes in the background with a copy of the
// text - the app may close (and free s_txt) before the SD card finishes.
static void gravar_solto(void)
{
  if (s_tela != TELA_EDITOR || !s_ed.mudou) { return; }
  if (notas_gravar_em_fundo(s_arq, s_txt)) { s_ed.mudou = false; }
  else { sistema_recado(tr("Não gravou")); }
}

static void trocar_tela(tela_t t)
{
  s_tela = t;
  s_dlg = DLG_NADA;
  s_sel = -1;
  s_forcar = true;
  sistema_pedir_redesenho_completo();
}

// ---------------------------------------------------------------------------
// LIST
// ---------------------------------------------------------------------------
static int item_altura(int, void*) { return ITEM_A; }

static void item_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int larg, void*)
{
  if (i >= s_n || s_ocupado) { return; }
  const uint16_t fundo = (i == s_sel) ? TEMA_DESTAQUE_SUAVE : TEMA_FUNDO;
  d.fillRect(x, y, larg, ITEM_A, fundo);
  // As in phone notes apps: the title in bold; below it, the date
  // and the start of the text.
  char titulo[NOTA_TITULO_MAX];
  snprintf(titulo, sizeof(titulo), "%s", s_itens[i].titulo[0] ? s_itens[i].titulo : tr("(vazia)"));
  tab5_fonte_corpo_negrito(d);
  tab5_texto(d, 3.2f);
  d.setTextDatum(top_left);
  d.setTextColor(TEMA_TEXTO, fundo);
  tab5_cortar_com_reticencias(d, titulo, sizeof(titulo), larg - 48);
  d.drawString(titulo, x + 24, y + 12);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.2f);
  int px = x + 24;
  if (s_itens[i].data > 0) {
    struct tm t;
    tab5_hora_local((uint32_t)s_itens[i].data, &t);
    char quando[32];
    strftime(quando, sizeof(quando), "%d/%m/%Y %H:%M", &t);
    d.setTextColor(TEMA_TEXTO_2, fundo);
    d.drawString(quando, px, y + 58);
    px += d.textWidth(quando) + 18;
  }
  if (s_itens[i].previa[0]) {
    char previa[NOTA_TITULO_MAX];
    snprintf(previa, sizeof(previa), "%s", s_itens[i].previa);
    d.setTextColor(TEMA_TEXTO_3, fundo);
    tab5_cortar_com_reticencias(d, previa, sizeof(previa), x + larg - 24 - px);
    d.drawString(previa, px, y + 58);
  }
  d.drawFastHLine(x + 16, y + ITEM_A - 1, larg - 32, TEMA_LINHA);
}

static void abrir_editor(void)
{
  if (!editor_abrir(&s_ed, s_txt, NOTA_MAX)) { return; }
  trocar_tela(TELA_EDITOR);
}

static void nota_nova(void)
{
  snprintf(s_arq, sizeof(s_arq), "%s", s_prox);
  s_txt[0] = 0;
  abrir_editor();
}

static void voltar_a_lista(void)
{
  editor_fechar(&s_ed);
  trocar_tela(TELA_LISTA);
  pedir(s_ed.mudou ? trab_gravar_e_listar : trab_listar, -1);
}

// The top buttons and those of the confirmation box, in one place: drawing and
// touch use the same sum.
typedef struct { int x, y, l, a; } ret_t;
static bool dentro(const ret_t& r, int x, int y) { return x >= r.x && x < r.x + r.l && y >= r.y && y < r.y + r.a; }
static ret_t bt_topo(const area_t& a, int de_direita, int l)   // 0 = the rightmost
{
  int x = a.largura - 16;
  for (int k = 0; k <= de_direita; ++k) { x -= l + (k ? 12 : 0); }
  return { x, (TOPO_A - BT_A) / 2, l, BT_A };
}
static ret_t bt_dlg(const area_t& a, int k) { return { a.largura / 2 - 260 + k * 280, a.altura / 2 + 30, 240, BT_A }; }

static void desenhar_lista(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (tudo) {
    tab5_tela_limpar(a.x, a.y, a.largura, TOPO_A, TEMA_FUNDO);
    tab5_fonte_corpo(d);
    tab5_texto(d, 2.6f);
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    char n[32];
    if (s_ocupado)  { snprintf(n, sizeof(n), "%s", tr("lendo...")); }
    else if (s_n < 0) { snprintf(n, sizeof(n), "%s", tr("sem cartão")); }
    else            { snprintf(n, sizeof(n), tr("%d notas"), s_n); }
    d.drawString(n, a.x + 24, a.y + TOPO_A / 2);
    if (s_sel >= 0) {
      const ret_t ap = bt_topo(a, 0, 180), cp = bt_topo(a, 1, 180), cn = bt_topo(a, 2, 180);
      tab5_botao(d, a.x + ap.x, a.y + ap.y, ap.l, ap.a, tr("Apagar"), TEMA_PERIGO, TEMA_SOBRE_DESTAQUE);
      tab5_botao(d, a.x + cp.x, a.y + cp.y, cp.l, cp.a, tr("Copiar"), TEMA_DESTAQUE);
      tab5_botao(d, a.x + cn.x, a.y + cn.y, cn.l, cn.a, tr("Cancelar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    } else if (s_n >= 0 && !s_ocupado) {
      const ret_t nv = bt_topo(a, 0, 200);
      tab5_botao(d, a.x + nv.x, a.y + nv.y, nv.l, nv.a, tr("+ Nova"), TEMA_SUCESSO);
    }
  }
  if (s_ocupado) { return; }   // the job is writing s_itens
  const area_t janela = { a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A };
  lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, tudo);
  if (tudo && s_n == 0) {
    tab5_texto(d, 2.8f);
    d.setTextDatum(middle_center);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr("nenhuma nota ainda - toque em + Nova"), a.x + a.largura / 2, janela.y + janela.altura / 3);
  }
  if (tudo && s_dlg == DLG_APAGAR && s_sel >= 0) {
    const int w = 600, h = 260, x = a.x + (a.largura - w) / 2, y = a.y + a.altura / 2 - 150;
    d.fillRoundRect(x, y, w, h, TEMA_RAIO_G, TEMA_SUPERFICIE);
    d.drawRoundRect(x, y, w, h, TEMA_RAIO_G, TEMA_LINHA);
    char msg[96];
    snprintf(msg, sizeof(msg), tr("Apagar \"%s\"?"), s_itens[s_sel].titulo);
    tab5_texto(d, 2.8f);
    d.setTextDatum(top_center);
    d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
    tab5_cortar_com_reticencias(d, msg, sizeof(msg), w - 40);
    d.drawString(msg, x + w / 2, y + 30);
    d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
    d.drawString(tr("Não dá para desfazer."), x + w / 2, y + 80);
    const ret_t c = bt_dlg(a, 0), ap = bt_dlg(a, 1);
    tab5_botao(d, a.x + c.x, a.y + c.y, c.l, c.a, tr("Cancelar"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    tab5_botao(d, a.x + ap.x, a.y + ap.y, ap.l, ap.a, tr("Apagar"), TEMA_PERIGO, TEMA_SOBRE_DESTAQUE);
  }
  d.setTextDatum(top_left);
}

static void tocar_lista(int x, int y)
{
  if (s_ocupado) { return; }
  const area_t a = sistema_area();
  if (s_dlg == DLG_APAGAR) {
    if (dentro(bt_dlg(a, 1), x, y))      { const int i = s_sel; trocar_tela(TELA_LISTA); pedir(trab_apagar, i); }
    else if (dentro(bt_dlg(a, 0), x, y)) { s_dlg = DLG_NADA; s_forcar = true; sistema_pedir_redesenho_completo(); }
    return;
  }
  if (y < TOPO_A) {
    if (s_sel >= 0) {
      if (dentro(bt_topo(a, 0, 180), x, y))      { s_dlg = DLG_APAGAR; s_forcar = true; sistema_pedir_redesenho_completo(); }
      else if (dentro(bt_topo(a, 1, 180), x, y)) { const int i = s_sel; trocar_tela(TELA_LISTA); pedir(trab_copiar, i); }
      else if (dentro(bt_topo(a, 2, 180), x, y)) { trocar_tela(TELA_LISTA); }
    } else if (s_n >= 0 && dentro(bt_topo(a, 0, 200), x, y)) {
      nota_nova();
    }
    return;
  }
  const int i = lista_item_tocado(&s_lista, x, y, nullptr);
  if (i == LISTA_ARRASTE || i < 0) { return; }
  if (s_sel >= 0) { s_sel = (i == s_sel) ? -1 : i; s_forcar = true; sistema_pedir_redesenho_completo(); return; }
  snprintf(s_arq, sizeof(s_arq), "%s", s_itens[i].arquivo);
  pedir(trab_abrir, i);
}

// ---------------------------------------------------------------------------
// EDITOR
// ---------------------------------------------------------------------------
// The open note's title at the top: the first line with text, while
// typing ("Nova nota" - new note - while it is empty).
static void titulo_do_texto(const char* t, char* out, size_t n)
{
  while (*t == '\n' || *t == ' ' || *t == '\t') { ++t; }
  size_t k = strcspn(t, "\n");
  if (k >= n) { k = n - 1; }
  k = (size_t)tab5_utf8_corte(t, (int)k);
  memcpy(out, t, k);
  out[k] = 0;
}

static void desenhar_editor(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  static char s_tit_pintado[NOTA_TITULO_MAX] = "";
  char tit[NOTA_TITULO_MAX];
  titulo_do_texto(s_txt, tit, sizeof(tit));
  if (!tit[0]) { snprintf(tit, sizeof(tit), "%s", tr("Nova nota")); }
  if (tudo) {
    tab5_tela_limpar(a.x, a.y, a.largura, TOPO_A, TEMA_FUNDO);
    const ret_t v = { 16, (TOPO_A - BT_A) / 2, 200, BT_A };
    tab5_botao(d, a.x + v.x, a.y + v.y, v.l, v.a, tr("< Notas"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
    d.drawFastHLine(a.x, a.y + TOPO_A - 1, a.largura, TEMA_LINHA);
  }
  if (tudo || strcmp(tit, s_tit_pintado) != 0) {
    snprintf(s_tit_pintado, sizeof(s_tit_pintado), "%s", tit);
    const int tx = a.x + 16 + 200 + 24;
    tab5_tela_limpar(tx, a.y + 4, a.x + a.largura - tx, TOPO_A - 8, TEMA_FUNDO);
    tab5_fonte_corpo_negrito(d);
    tab5_texto(d, 3.0f);
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
    tab5_cortar_com_reticencias(d, tit, sizeof(tit), a.x + a.largura - 16 - tx);
    d.drawString(tit, tx, a.y + TOPO_A / 2);
    d.setTextDatum(top_left);
  }
  const area_t r = { a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A };
  editor_desenhar(&s_ed, d, r, tudo);
}

static void tocar_editor(int x, int y)
{
  const area_t a = sistema_area();
  if (y < TOPO_A && !tecvirt_contem(&s_ed.kb, a.x + x, a.y + y)) {
    if (x < 216) { voltar_a_lista(); }
    return;
  }
  editor_tocar(&s_ed, x, y);
}

// ---------------------------------------------------------------------------
// LIFE CYCLE AND INPUTS
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  s_itens = (nota_t*)heap_caps_calloc(NOTAS_MAX, sizeof(nota_t), MALLOC_CAP_SPIRAM);
  s_txt = (char*)heap_caps_malloc(NOTA_MAX, MALLOC_CAP_SPIRAM);
  if (!s_itens || !s_txt) { heap_caps_free(s_itens); heap_caps_free(s_txt); s_itens = nullptr; s_txt = nullptr; return false; }
  s_tela = TELA_LISTA;
  s_dlg = DLG_NADA;
  s_sel = -1;
  s_n = 0;
  s_ocupado = false;
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, item_altura, item_desenhar, nullptr);
  pedir(trab_listar, -1);
  return true;
}

// The system has already waited for the job in progress: s_txt and s_itens are free.
static void ao_fechar(void)
{
  gravar_solto();
  if (s_tela == TELA_EDITOR) { editor_fechar(&s_ed); }
  s_tela = TELA_LISTA;
  heap_caps_free(s_itens); s_itens = nullptr;
  heap_caps_free(s_txt);   s_txt = nullptr;
}

static void ao_repouso(void) { gravar_solto(); }

static void ao_aviso(uint32_t avisos)
{
  if (!(avisos & AVISO_TRABALHO) || !s_ocupado) { return; }
  s_ocupado = false;
  const int fez = s_fez.exchange(FEZ_NADA);
  if (fez == FEZ_LISTA) {
    s_n = s_n_res;
    lista_itens_mudaram(&s_lista, s_n > 0 ? s_n : 0);
  } else if (fez == FEZ_ABRIR) {
    if (s_res_ok) { abrir_editor(); return; }
    sistema_recado(tr("Não abriu"));
  } else if (fez == FEZ_COPIAR) {
    sistema_recado(s_res_ok ? tr("Copiado") : tr("Não copiou"));
  }
  s_forcar = true;
  sistema_pedir_redesenho_completo();
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  const bool tudo = sistema_area_foi_limpa() || s_forcar;
  s_forcar = false;
  if (s_tela == TELA_EDITOR) { desenhar_editor(d, a, tudo); }
  else                       { desenhar_lista(d, a, tudo); }
  d.setTextStyle(estilo);
}

static void ao_tocar(int x, int y)
{
  if (s_tela == TELA_EDITOR) { tocar_editor(x, y); } else { tocar_lista(x, y); }
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (s_tela == TELA_EDITOR) { editor_tocar_nivel(&s_ed, x, y, pressionado); return; }
  if (s_dlg != DLG_NADA) { return; }
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
  if (toque_longo(&s_lp, x, y, pressionado)) {
    const int i = lista_item_em(&s_lista, x, y, nullptr);
    if (i >= 0) {
      sistema_consumir_toque();
      s_sel = i;
      s_forcar = true;
      sistema_pedir_redesenho_completo();
    }
  }
}

static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  if (s_tela != TELA_EDITOR) {
    if (codigo == TAB5_TECLA_ENTER && s_dlg == DLG_NADA && !s_ocupado && s_n >= 0) { nota_nova(); }
    return;
  }
  if (codigo == TAB5_TECLA_ESC) { voltar_a_lista(); return; }
  editor_teclar(&s_ed, texto, codigo, mod);
}

// The icon: a sheet with lines and a folded corner.
static void icone_notas(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(70, 60, 20));
  const int m = lado / 5, fx = x + m, fy = y + m, fl = lado - 2 * m, fa = lado - 2 * m, c = fl / 4;
  d.fillRect(fx, fy, fl - c, fa, TFT_WHITE);
  d.fillRect(fx, fy + c, fl, fa - c, TFT_WHITE);
  d.fillTriangle(fx + fl - c, fy, fx + fl, fy + c, fx + fl - c, fy + c, TFT_LIGHTGREY);
  for (int k = 1; k <= 3; ++k) {
    d.fillRect(fx + fl / 8, fy + c + k * (fa - c) / 4 - 1, fl * 3 / 4, lado / 40 + 2, d.color565(120, 120, 120));
  }
}

extern const app_t APP_NOTAS = {
  .nome  = "Notas",
  .cor   = TFT_YELLOW,
  .perfil = { },
  .exige_sd = true,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .ao_repouso = ao_repouso,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .avisos = AVISO_TRABALHO,
  .ao_aviso = ao_aviso,
  .icone = icone_notas,
  .fundo_tema = true,   // the system clears the area with TEMA_FUNDO
};
