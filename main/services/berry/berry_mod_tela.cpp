// Modulos Berry 'tela' (desenho, camadas) e 'teclado' (o virtual do sistema e
// saber se ha o fisico). Ver berry_apps.cpp (o motor) e berry_interno.hpp.

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

// O TECLADO VIRTUAL DO SISTEMA (um so: so um app Berry fica aberto por vez).
// O script so pede (teclado.abrir/fechar); quem abre e a interface, depois
// que o trabalho volta - tecvirt_abrir trava a navegacao, coisa da interface.
// As teclas chegam ao script pelo mesmo tecla(texto, codigo) do fisico.
tecvirt_t s_kb;
int       s_kb_pedido = KB_NADA;
uint8_t   s_kb_tipo = TECVIRT_TEXTO;

static bool kb_aberto(void)
{
  return s_kb_pedido == KB_ABRIR || (tecvirt_aberto(&s_kb) && s_kb_pedido != KB_FECHAR);
}

// A altura que sobra para o app: o teclado aberto come a base da area.
int altura_livre(void)
{
  return C->a.altura - (kb_aberto() ? tecvirt_altura(C->a) : 0);
}

// As camadas de um app somadas: no maximo 4 telas inteiras (~7 MB de PSRAM).
static const uint32_t CAMADAS_MAX_PX = 4u * 720u * 1280u;

// ---------------------------------------------------------------------------
// CAMADAS - imagens guardadas, desenhadas quando o app quiser e coladas na tela
// ---------------------------------------------------------------------------
// tela.camada(l, a) cria; tela.alvo(c) faz TODA funcao de 'tela' desenhar
// nela (em qualquer evento, nao so no desenhar), tela.alvo() volta para a
// tela; tela.colar(c, x, y [, rx, ry, rl, ra]) copia a camada (ou um pedaco)
// para a tela. Memoria na PSRAM, devolvida ao fechar o app.
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
  c->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);   // o formato da tela: colar e copia simples
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
  // O texto transparente pode escrever direto na memoria dela (sem giro).
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

static int t_colar(bvm* vm);   // depois do medir_prim (e medida como desenho)

// QUANTO CADA DESENHO CUSTA ('berry tempos'): o tempo dentro de cada funcao
// de 'tela', para separar "o desenho e caro" de "o Berry e lento".
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

// colar(c, x, y [, rx, ry, rl, ra]): a camada c (ou o pedaco rx, ry, rl, ra
// dela) em x, y da TELA. So no desenhar(), com o alvo na tela. A copia e da
// memoria da camada direto para a pagina de tras (o compositor e avisado de
// que o retangulo foi coberto inteiro - nada vem da frente antes).
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
  // Recorta: o pedaco dentro da camada, e o destino dentro da area do app.
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
  // Direto da memoria da camada para a pagina de tras (memcpy por linha).
  if (tela_virtual_colar(C->d, x, y, rl, ra, img + (size_t)ry * passo + rx, passo)) { be_return_nil(vm); }
  // Outro alvo (nao deveria acontecer): pelo M5GFX, linha a linha.
  tela_virtual_cobrir(x, y, rl, ra);
  C->d->startWrite();
  if (rx == 0 && rl == passo) {   // largura inteira: as linhas sao contiguas, uma copia so
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
// MODULO 'tela' - coordenadas relativas a area do app
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
  if (C->alvo) { C->d->fillScreen(cor); }   // a camada inteira
  else        { tab5_tela_limpar(C->a.x, C->a.y, C->a.largura, C->a.altura, cor); }
  be_return_nil(vm);
}

// Retangulo grande vai em faixas (tab5_tela_limpar), como pede o ESCREVENDO_APPS.
static void preencher(int x, int y, int l, int a, uint16_t cor)
{
  // tab5_tela_limpar e da TELA do sistema: numa camada, o fillRect dela.
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

// texto(txt, x, y [, tamanho = 3 [, cor = branco [, alinhamento]]]) - tamanho
// N = N*8 px de altura; alinhamento tela.ESQUERDA (padrao), CENTRO ou DIREITA:
// x e onde o texto comeca, o meio dele ou onde termina.
static int t_texto(bvm* vm)
{
  if (!exige_tela(vm)) { be_return_nil(vm); }
  medir_prim _m(P_TEXTO);
  const lgfx::TextStyle estilo = C->d->getTextStyle();
  tab5_fonte_corpo(*C->d);
  tab5_texto(*C->d, arg_tam(vm, 4));
  const int al = arg_int(vm, 6, 0);
  C->d->setTextDatum(al == 1 ? top_center : al == 2 ? top_right : top_left);
  C->d->setTextColor((uint16_t)arg_int(vm, 5, TFT_WHITE));   // sem fundo: transparente
  C->d->drawString(arg_txt(vm, 1), C->a.x + arg_int(vm, 2, 0), C->a.y + arg_int(vm, 3, 0));
  C->d->setTextStyle(estilo);
  be_return_nil(vm);
}

// medir(txt [, tamanho]) - a largura em px que texto() ocuparia.
// cortar(txt, largura [, tam]) - o texto cortado para caber em 'largura' px,
// com "..." no fim quando cortou (UTF-8 inteiro: acento nunca vira lixo). O
// mesmo corte que os apps do sistema usam - em C, de uma vez.
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

// MODULO 'teclado' - o virtual do sistema, e saber se ha o fisico
// ---------------------------------------------------------------------------
// teclado.abrir([tipo]): o TIPO do campo, como nos apps do sistema
// (kernel/teclado_virtual.hpp) - "texto" (o padrao), "ascii", "senha",
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
