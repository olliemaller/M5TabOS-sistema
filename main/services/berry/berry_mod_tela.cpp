// Berry modules 'tela' (drawing, layers) and 'teclado' (the system's virtual one and
// knowing whether there is a physical one). See berry_apps.cpp (the engine) and berry_interno.hpp.

#include <string.h>
#include "idioma.h"
#include "esp_timer.h"
#include "tab5_ui.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "teclado_virtual.hpp"
#include "tela_virtual.hpp"
#include "tab5_glifos.hpp"
#include "berry_interno.hpp"

// THE SYSTEM'S ON-SCREEN KEYBOARD (just one: only one Berry app is open at a time).
// The script only requests (teclado.abrir/fechar); the UI opens it, after
// the job comes back - tecvirt_abrir locks navigation, which is the UI's business.
// The keys reach the script through the same tecla(texto, codigo) as the physical one.
tecvirt_t s_kb;
int       s_kb_pedido = KB_NADA;
uint8_t   s_kb_tipo = TECVIRT_TEXTO;

static bool kb_aberto(void)
{
  return s_kb_pedido == KB_ABRIR || (tecvirt_aberto(&s_kb) && s_kb_pedido != KB_FECHAR);
}

// The height left for the app: the open keyboard eats the bottom of the area.
int altura_livre(void)
{
  return C->a.altura - (kb_aberto() ? tecvirt_altura(C->a) : 0);
}

// An app's layers added up: at most 4 whole screens (~7 MB of PSRAM).
static const uint32_t CAMADAS_MAX_PX = 4u * 720u * 1280u;

// ---------------------------------------------------------------------------
// LAYERS - stored images, drawn whenever the app wants and pasted onto the screen
// ---------------------------------------------------------------------------
// tela.camada(l, a) creates one; tela.alvo(c) makes EVERY 'tela' function draw
// into it (in any event, not just in desenhar), tela.alvo() goes back to the
// screen; tela.colar(c, x, y [, rx, ry, rl, ra]) copies the layer (or a piece)
// to the screen. Memory in PSRAM, returned when the app closes.
void alvo_tela(void)
{
  if (C->alvo) { tab5_glifos_canvas_direto(nullptr, nullptr, 0, 0); }
  C->alvo = 0;
  C->d = C->tela_d;
  C->a = C->tela_a;
}

static lgfx::LGFX_Sprite* camada_de(bvm* vm, int i)
{
  if (!so_janela(vm, "tela.camada")) { return nullptr; }
  const int n = arg_int(vm, i, 0);
  lgfx::LGFX_Sprite* c = (C->app && n >= 1 && n <= 8) ? C->app->camadas[n - 1] : nullptr;
  if (!c) { be_raise(vm, "tela_error", "camada invalida (ja liberada?)"); }
  return c;
}

void camadas_liberar(berry_app_t* a)
{
  for (auto& c : a->camadas) { if (c) { c->deleteSprite(); delete c; c = nullptr; } }
}

static int t_camada(bvm* vm)
{
  if (!so_janela(vm, "tela.camada")) { be_return_nil(vm); }
  const int l = arg_int(vm, 1, 0), al = arg_int(vm, 2, 0);
  if (!C->app || l <= 0 || al <= 0 || l > 2048 || al > 2048) {
    be_raise(vm, "tela_error", "camada(largura, altura): tamanho invalido");
    be_return_nil(vm);
  }
  uint32_t usado = (uint32_t)l * al;
  int vaga = -1;
  for (int i = 0; i < 8; ++i) {
    if (C->app->camadas[i]) { usado += (uint32_t)C->app->camadas[i]->width() * C->app->camadas[i]->height(); }
    else if (vaga < 0)       { vaga = i; }
  }
  if (vaga < 0 || usado > CAMADAS_MAX_PX) {
    be_raise(vm, "tela_error", tr("camadas demais (no maximo 8, somando 4 telas)"));
    be_return_nil(vm);
  }
  auto* c = new lgfx::LGFX_Sprite();
  c->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);   // the screen format: pasting is a plain copy
  c->setPsram(true);
  if (!c->createSprite(l, al)) { delete c; be_raise(vm, "tela_error", tr("sem memoria para a camada")); be_return_nil(vm); }
  c->fillScreen(TFT_BLACK);
  C->app->camadas[vaga] = c;
  be_pushint(vm, vaga + 1);
  be_return(vm);
}

static int t_alvo(bvm* vm)
{
  if (!so_janela(vm, "tela.alvo")) { be_return_nil(vm); }
  if (be_top(vm) < 1 || be_isnil(vm, 1)) { alvo_tela(); be_return_nil(vm); }
  lgfx::LGFX_Sprite* c = camada_de(vm, 1);
  if (!c) { be_return_nil(vm); }
  C->alvo = arg_int(vm, 1, 0);
  C->d = c;
  // Transparent text can write straight into its memory (no rotation).
  tab5_glifos_canvas_direto(c, (uint16_t*)c->getBuffer(), c->width(), c->height());
  C->a = area_t{ 0, 0, (int)c->width(), (int)c->height() };
  be_return_nil(vm);
}

static int t_liberar(bvm* vm)
{
  lgfx::LGFX_Sprite* c = camada_de(vm, 1);
  if (!c) { be_return_nil(vm); }
  const int n = arg_int(vm, 1, 0);
  if (C->alvo == n) { alvo_tela(); }
  tab5_glifos_canvas_direto(nullptr, nullptr, 0, 0);
  c->deleteSprite();
  delete c;
  C->app->camadas[n - 1] = nullptr;
  be_return_nil(vm);
}

static int t_colar(bvm* vm);   // after medir_prim (it is measured as drawing)

// WHAT EACH DRAWING COSTS ('berry tempos'): the time inside each 'tela'
// function, to separate "drawing is expensive" from "Berry is slow".
const char* const P_NOME[P_N] = { "limpar", "retangulo", "circulo", "linha", "texto", "medir", "arredondado", "triangulo", "pixel", "botao", "colar" };
tempo_t s_prim[P_N];

struct medir_prim {
  int i; int64_t t0;
  explicit medir_prim(int i) : i(i), t0(esp_timer_get_time()) {}
  ~medir_prim() {
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    tempo_t& t = s_prim[i];
    t.n++; t.soma_us += us;
    if (us > t.max_us) { t.max_us = us; }
  }
};

// colar(c, x, y [, rx, ry, rl, ra]): layer c (or the piece rx, ry, rl, ra
// of it) at x, y on the SCREEN. Only in desenhar(), with the target on the screen. The copy goes from
// the layer's memory straight to the back page (the compositor is told
// the rectangle was fully covered - nothing comes from the front first).
static int t_colar(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  if (C->alvo || !C->tela_d) { be_raise(vm, "tela_error", tr("colar vai para a tela: chame tela.alvo() antes")); be_return_nil(vm); }
  lgfx::LGFX_Sprite* c = camada_de(vm, 1);
  if (!c) { be_return_nil(vm); }
  medir_prim _m(P_COLAR);
  int rx = arg_int(vm, 4, 0), ry = arg_int(vm, 5, 0);
  int rl = arg_int(vm, 6, c->width()), ra = arg_int(vm, 7, c->height());
  int x = C->a.x + arg_int(vm, 2, 0), y = C->a.y + arg_int(vm, 3, 0);
  // Clip: the piece within the layer, and the destination within the app's area.
  if (rx < 0) { rl += rx; x -= rx; rx = 0; }
  if (ry < 0) { ra += ry; y -= ry; ry = 0; }
  if (rx + rl > c->width())  { rl = c->width() - rx; }
  if (ry + ra > c->height()) { ra = c->height() - ry; }
  if (x < C->a.x) { const int d = C->a.x - x; rx += d; rl -= d; x = C->a.x; }
  if (y < C->a.y) { const int d = C->a.y - y; ry += d; ra -= d; y = C->a.y; }
  if (x + rl > C->a.x + C->a.largura) { rl = C->a.x + C->a.largura - x; }
  if (y + ra > C->a.y + C->a.altura)  { ra = C->a.y + C->a.altura - y; }
  if (rl <= 0 || ra <= 0) { be_return_nil(vm); }
  const uint16_t* img = (const uint16_t*)c->getBuffer();
  const int passo = c->width();
  // Straight from the layer's memory to the back page (memcpy per row).
  if (tela_virtual_colar(C->d, x, y, rl, ra, img + (size_t)ry * passo + rx, passo)) { be_return_nil(vm); }
  // Another target (should not happen): via M5GFX, row by row.
  tela_virtual_cobrir(x, y, rl, ra);
  C->d->startWrite();
  if (rx == 0 && rl == passo) {   // full width: the rows are contiguous, a single copy
    C->d->pushImage(x, y, rl, ra, (const lgfx::rgb565_t*)(img + (size_t)ry * passo));
  } else {
    for (int j = 0; j < ra; ++j) {
      C->d->pushImage(x, y + j, rl, 1, (const lgfx::rgb565_t*)(img + (size_t)(ry + j) * passo + rx));
    }
  }
  C->d->endWrite();
  be_return_nil(vm);
}

// ---------------------------------------------------------------------------
// MODULE 'tela' - coordinates relative to the app's area
// ---------------------------------------------------------------------------
static int t_largura(bvm* vm) { be_pushint(vm, C->a.largura); be_return(vm); }
static int t_altura(bvm* vm)  { be_pushint(vm, C->alvo ? C->a.altura : altura_livre()); be_return(vm); }

static int t_cor(bvm* vm)
{
  be_pushint(vm, lgfx::color565(arg_int(vm, 1, 0), arg_int(vm, 2, 0), arg_int(vm, 3, 0)));
  be_return(vm);
}

static int t_dentro(bvm* vm)
{
  be_pushbool(vm, tab5_dentro(arg_int(vm, 1, -1), arg_int(vm, 2, -1), arg_int(vm, 3, 0),
                              arg_int(vm, 4, 0), arg_int(vm, 5, 0), arg_int(vm, 6, 0)));
  be_return(vm);
}

static int t_limpar(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_LIMPAR);
  const uint16_t cor = (uint16_t)arg_int(vm, 1, TFT_BLACK);
  if (C->alvo) { C->d->fillScreen(cor); }   // the whole layer
  else        { tab5_tela_limpar(C->a.x, C->a.y, C->a.largura, C->a.altura, cor); }
  be_return_nil(vm);
}

// A large rectangle goes in bands (tab5_tela_limpar), as ESCREVENDO_APPS asks.
static void preencher(int x, int y, int l, int a, uint16_t cor)
{
  // tab5_tela_limpar belongs to the system SCREEN: on a layer, its own fillRect.
  if (!C->alvo && (int64_t)l * a > 40000) { tab5_tela_limpar(x, y, l, a, cor); }
  else                        { C->d->fillRect(x, y, l, a, cor); }
}

static int t_retangulo(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_RETANGULO);
  const int x = C->a.x + arg_int(vm, 1, 0), y = C->a.y + arg_int(vm, 2, 0);
  const int l = arg_int(vm, 3, 0), a = arg_int(vm, 4, 0);
  const uint16_t cor = (uint16_t)arg_int(vm, 5, TFT_WHITE);
  const bool cheio = be_top(vm) < 6 || be_tobool(vm, 6);
  if (cheio) { preencher(x, y, l, a, cor); }
  else       { C->d->drawRect(x, y, l, a, cor); }
  be_return_nil(vm);
}

static int t_circulo(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_CIRCULO);
  const int x = C->a.x + arg_int(vm, 1, 0), y = C->a.y + arg_int(vm, 2, 0), r = arg_int(vm, 3, 10);
  const uint16_t cor = (uint16_t)arg_int(vm, 4, TFT_WHITE);
  if (be_top(vm) < 5 || be_tobool(vm, 5)) { C->d->fillCircle(x, y, r, cor); }
  else                                     { C->d->drawCircle(x, y, r, cor); }
  be_return_nil(vm);
}

static int t_linha(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_LINHA);
  C->d->drawLine(C->a.x + arg_int(vm, 1, 0), C->a.y + arg_int(vm, 2, 0),
                C->a.x + arg_int(vm, 3, 0), C->a.y + arg_int(vm, 4, 0),
                (uint16_t)arg_int(vm, 5, TFT_WHITE));
  be_return_nil(vm);
}

static float arg_tam(bvm* vm, int i)
{
  return be_top(vm) >= i && be_isnumber(vm, i) ? (float)be_toreal(vm, i) : 3.0f;
}

// texto(txt, x, y [, tamanho = 3 [, cor = white [, alinhamento]]]) - size
// N = N*8 px tall; alignment tela.ESQUERDA (default), CENTRO or DIREITA:
// x is where the text starts, its middle or where it ends.
static int t_texto(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_TEXTO);
  const lgfx::TextStyle estilo = C->d->getTextStyle();
  tab5_fonte_corpo(*C->d);
  tab5_texto(*C->d, arg_tam(vm, 4));
  const int al = arg_int(vm, 6, 0);
  C->d->setTextDatum(al == 1 ? top_center : al == 2 ? top_right : top_left);
  C->d->setTextColor((uint16_t)arg_int(vm, 5, TFT_WHITE));   // no background: transparent
  C->d->drawString(arg_txt(vm, 1), C->a.x + arg_int(vm, 2, 0), C->a.y + arg_int(vm, 3, 0));
  C->d->setTextStyle(estilo);
  be_return_nil(vm);
}

// medir(txt [, tamanho]) - the width in px that texto() would take up.
// cortar(txt, largura [, tam]) - the text cut to fit in 'largura' px,
// with "..." at the end when it was cut (whole UTF-8: an accent never turns into garbage). The
// same cut the system apps use - in C, in one go.
static int t_cortar(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  char t[256];
  snprintf(t, sizeof(t), "%s", arg_txt(vm, 1));
  const lgfx::TextStyle estilo = C->d->getTextStyle();
  tab5_fonte_corpo(*C->d);
  tab5_texto(*C->d, arg_tam(vm, 3));
  tab5_cortar_com_reticencias(*C->d, t, sizeof(t), arg_int(vm, 2, 0));
  C->d->setTextStyle(estilo);
  be_pushstring(vm, t);
  be_return(vm);
}

static int t_medir(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_MEDIR);
  const lgfx::TextStyle estilo = C->d->getTextStyle();
  tab5_fonte_corpo(*C->d);
  tab5_texto(*C->d, arg_tam(vm, 2));
  be_pushint(vm, C->d->textWidth(arg_txt(vm, 1)));
  C->d->setTextStyle(estilo);
  be_return(vm);
}

static int t_arredondado(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_ARREDONDADO);
  const int x = C->a.x + arg_int(vm, 1, 0), y = C->a.y + arg_int(vm, 2, 0);
  const int l = arg_int(vm, 3, 0), a = arg_int(vm, 4, 0), r = arg_int(vm, 5, 12);
  const uint16_t cor = (uint16_t)arg_int(vm, 6, TFT_WHITE);
  if (be_top(vm) < 7 || be_tobool(vm, 7)) { C->d->fillRoundRect(x, y, l, a, r, cor); }
  else                                     { C->d->drawRoundRect(x, y, l, a, r, cor); }
  be_return_nil(vm);
}

static int t_triangulo(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_TRIANGULO);
  const int x0 = C->a.x + arg_int(vm, 1, 0), y0 = C->a.y + arg_int(vm, 2, 0);
  const int x1 = C->a.x + arg_int(vm, 3, 0), y1 = C->a.y + arg_int(vm, 4, 0);
  const int x2 = C->a.x + arg_int(vm, 5, 0), y2 = C->a.y + arg_int(vm, 6, 0);
  const uint16_t cor = (uint16_t)arg_int(vm, 7, TFT_WHITE);
  if (be_top(vm) < 8 || be_tobool(vm, 8)) { C->d->fillTriangle(x0, y0, x1, y1, x2, y2, cor); }
  else                                     { C->d->drawTriangle(x0, y0, x1, y1, x2, y2, cor); }
  be_return_nil(vm);
}

static int t_pixel(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_PIXEL);
  C->d->drawPixel(C->a.x + arg_int(vm, 1, 0), C->a.y + arg_int(vm, 2, 0), (uint16_t)arg_int(vm, 3, TFT_WHITE));
  be_return_nil(vm);
}

static int t_botao(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_BOTAO);
  tab5_botao(*C->d, C->a.x + arg_int(vm, 1, 0), C->a.y + arg_int(vm, 2, 0),
             arg_int(vm, 3, 200), arg_int(vm, 4, 64), arg_txt(vm, 5),
             (uint16_t)arg_int(vm, 6, TFT_SKYBLUE));
  be_return_nil(vm);
}

// MODULE 'teclado' - the system's virtual one, and knowing whether there is a physical one
// ---------------------------------------------------------------------------
// teclado.abrir([tipo]): the field TYPE, as in the system apps
// (kernel/teclado_virtual.hpp) - "texto" (the default), "ascii", "senha",
// "numero", "busca", "paragrafo", "codigo" ou "comando".
static int k_abrir(bvm* vm)
{
  if (so_janela(vm, "o teclado")) {
    static const char* const NOMES[TECVIRT_TIPOS] = { "texto", "ascii", "senha", "numero", "busca",
                                                       "paragrafo", "codigo", "comando" };
    const char* nome = arg_txt(vm, 1);
    s_kb_tipo = TECVIRT_TEXTO;
    for (int i = 0; i < TECVIRT_TIPOS; ++i) { if (!strcmp(nome, NOMES[i])) { s_kb_tipo = (uint8_t)i; } }
    s_kb_pedido = KB_ABRIR;
  }
  be_return_nil(vm);
}
static int k_fechar(bvm* vm) { if (so_janela(vm, "o teclado")) { s_kb_pedido = KB_FECHAR; } be_return_nil(vm); }
static int k_aberto(bvm* vm) { be_pushbool(vm, kb_aberto()); be_return(vm); }
static int k_fisico(bvm* vm) { be_pushbool(vm, tab5_teclado_presente()); be_return(vm); }


const membro_t BERRY_MOD_TELA[] = {
  { "largura", t_largura }, { "altura", t_altura }, { "cor", t_cor }, { "dentro", t_dentro },
  { "limpar", t_limpar }, { "retangulo", t_retangulo }, { "circulo", t_circulo },
  { "linha", t_linha }, { "texto", t_texto }, { "botao", t_botao }, { "medir", t_medir },
  { "cortar", t_cortar },
  { "arredondado", t_arredondado }, { "triangulo", t_triangulo }, { "pixel", t_pixel },
  { "camada", t_camada }, { "alvo", t_alvo }, { "colar", t_colar }, { "liberar", t_liberar },
  { "ESQUERDA", nullptr, 0 }, { "CENTRO", nullptr, 1 }, { "DIREITA", nullptr, 2 },
  { "PRETO", nullptr, TFT_BLACK }, { "BRANCO", nullptr, TFT_WHITE },
  { "VERMELHO", nullptr, TFT_RED }, { "VERDE", nullptr, TFT_GREEN },
  { "AZUL", nullptr, TFT_BLUE }, { "AMARELO", nullptr, TFT_YELLOW },
  { "CIANO", nullptr, TFT_CYAN }, { "LARANJA", nullptr, TFT_ORANGE },
  { "CINZA", nullptr, TFT_DARKGREY }, { "ROXO", nullptr, TFT_MAGENTA },
  { "ROSA", nullptr, TFT_PINK }, { "CINZA_CLARO", nullptr, TFT_LIGHTGREY },
  { nullptr },
};

const membro_t BERRY_MOD_TECLADO[] = {
  { "abrir", k_abrir }, { "fechar", k_fechar }, { "aberto", k_aberto }, { "fisico", k_fisico },
  { "ENTER", nullptr, TAB5_TECLA_ENTER }, { "APAGAR", nullptr, TAB5_TECLA_APAGAR },
  { "ESC", nullptr, TAB5_TECLA_ESC }, { "TAB", nullptr, TAB5_TECLA_TAB },
  { "ESQUERDA", nullptr, TAB5_TECLA_ESQUERDA }, { "DIREITA", nullptr, TAB5_TECLA_DIREITA },
  { "CIMA", nullptr, TAB5_TECLA_CIMA }, { "BAIXO", nullptr, TAB5_TECLA_BAIXO },
  { "DEL", nullptr, TAB5_TECLA_DEL },
  { nullptr },
};
