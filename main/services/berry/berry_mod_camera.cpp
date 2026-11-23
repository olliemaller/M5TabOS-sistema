// Modulo Berry 'camera' (o preview ao vivo e a foto). Ver berry_apps.cpp (o
// motor) e berry_interno.hpp.

#include <dirent.h>
#include "idioma.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "tab5_sd.hpp"
#include "tab5_tela.hpp"
#include "tab5_camera.hpp"
#include "tab5_bmp.hpp"
#include "trabalho.hpp"
#include "berry_interno.hpp"

// ---------------------------------------------------------------------------
// MODULO 'camera' - o preview ao vivo e a foto, o mesmo servico do app Camera
// ---------------------------------------------------------------------------
// O SCRIPT SO PEDE (camera.ligar/desligar/foto); quem mexe no sensor e a
// interface, depois que o trabalho volta (cam_aplicar, em pedir): o
// tab5_camera_iniciar fala I2C, coisa so da tarefa de interface (ver
// hal/tab5_camera.hpp) - o mesmo jeito do teclado virtual acima.
// O PREVIEW e escrito pelo PPA da camera DIRETO na tela virtual
// (sistema_superficie), sem passar pelo script: largura inteira, com o app em
// pe, topo e altura em multiplos de 8. O desenhar() do app nao apaga essa
// faixa (cam_limpar_fora) e pode desenhar POR CIMA dela (uma mira, um texto):
// o quadro da camera entra antes do desenhar(). Um app Berry aberto por vez,
// entao um estado so.
enum { CAM_NADA, CAM_LIGAR, CAM_DESLIGAR };
enum { FOTO_NADA, FOTO_CAPTURANDO, FOTO_GRAVANDO, FOTO_OK, FOTO_FALHOU };
typedef struct {
  int       pedido;          // CAM_*, do script para a interface
  int       ped_y, ped_a;    // a faixa pedida, em coordenadas da area
  bool      ligada;          // o preview esta no ar
  area_t    r;               // a faixa do preview, em coordenadas de TELA
  char      erro[80];        // por que nao ligou ("" = nada)
  bool      foto_pedida;
  char      foto_nome[40];   // "" = foto_NNNN
  uint16_t* foto;            // a captura em curso: do app ate virar trabalho
  volatile int foto_estado;  // FOTO_* (o fim da gravacao vem do worker)
  char      ultima[48];      // o ultimo .bmp gravado
} cam_t;
static cam_t s_cam;

// O preview esta no ar? (a interface nao apaga a faixa dele)
bool cam_no_ar(void) { return s_cam.ligada; }

static int cam_ligar(bvm* vm)
{
  // camera.ligar([y [, altura]]): o preview na largura toda, de y ate y+altura
  // (padrao: a area inteira que sobra abaixo de y).
  if (!so_janela(vm, "a camera")) { be_return_nil(vm); }
  const int y = arg_int(vm, 1, 0);
  s_cam.ped_y = y;
  s_cam.ped_a = arg_int(vm, 2, altura_livre() - y);
  s_cam.pedido = CAM_LIGAR;
  s_cam.erro[0] = 0;
  be_return_nil(vm);
}
static int cam_desligar(bvm* vm) { if (so_janela(vm, "a camera")) { s_cam.pedido = CAM_DESLIGAR; } be_return_nil(vm); }
static int cam_ligada(bvm* vm)   { be_pushbool(vm, s_cam.ligada); be_return(vm); }
static int cam_erro(bvm* vm)     { be_pushstring(vm, s_cam.erro); be_return(vm); }
static int cam_ultima(bvm* vm)   { be_pushstring(vm, s_cam.ultima); be_return(vm); }

// camera.faixa(): [y, altura] do preview no ar, em coordenadas da area (nil
// desligada) - ja arredondada para os multiplos de 8.
static int cam_faixa(bvm* vm)
{
  if (!s_cam.ligada) { be_return_nil(vm); }
  lista_nova(vm);
  l_int(vm, s_cam.r.y - C->tela_a.y);
  l_int(vm, s_cam.r.altura);
  fim(vm);
  be_return(vm);
}

// camera.foto([nome]): tira a foto (1200x1600) e grava <nome>.bmp na pasta do
// app (sem nome: foto_0001.bmp, foto_0002.bmp...). false = camera desligada
// ou ja ha uma foto em curso. O andamento: camera.foto_estado().
static int cam_foto(bvm* vm)
{
  if (!so_janela(vm, "a camera")) { be_return_nil(vm); }
  const char* nome = be_top(vm) >= 1 ? arg_txt(vm, 1) : "";
  if (strchr(nome, '/') || strchr(nome, '\\') || strstr(nome, "..") || strlen(nome) > 32) {
    be_raise(vm, "camera_error", tr("camera.foto(nome): so o nome, sem pasta nem extensao"));
    be_return_nil(vm);
  }
  const int e = s_cam.foto_estado;
  if (!s_cam.ligada || s_cam.foto_pedida || e == FOTO_CAPTURANDO || e == FOTO_GRAVANDO) {
    be_pushbool(vm, false);
    be_return(vm);
  }
  snprintf(s_cam.foto_nome, sizeof(s_cam.foto_nome), "%s", nome);
  s_cam.foto_pedida = true;
  s_cam.foto_estado = FOTO_CAPTURANDO;
  be_pushbool(vm, true);
  be_return(vm);
}

static int cam_foto_estado(bvm* vm)
{
  static const char* const N[] = { "", "capturando", "gravando", "ok", "falhou" };
  const int e = s_cam.foto_estado;
  be_pushstring(vm, (e >= 0 && e <= FOTO_FALHOU) ? N[e] : "");
  be_return(vm);
}

// ---- a parte da interface ----------------------------------------------------
static void cam_soltar_foto(void)
{
  if (s_cam.foto) { heap_caps_free(s_cam.foto); s_cam.foto = nullptr; }
}

// Liga o sensor com o preview em s_cam.r. A superficie primeiro: com a tela
// deitada ela nao existe, e ai nem vale ligar o sensor.
static char s_cam_quem[24] = "Berry";   // o app que ligou: o mesmo nome ao soltar

static bool cam_subir(void)
{
  superficie_t sup;
  if (!sistema_superficie(s_cam.r, &sup)) {
    snprintf(s_cam.erro, sizeof(s_cam.erro), tr("o preview so funciona com o app em pe"));
    return false;
  }
  // Quem usa a camera e o app Berry aberto (aparece nos recursos com o nome dele).
  snprintf(s_cam_quem, sizeof(s_cam_quem), "%s", sistema_app_atual() ? sistema_app_atual() : "Berry");
  if (!tab5_camera_iniciar(s_cam_quem)) {
    snprintf(s_cam.erro, sizeof(s_cam.erro), tr("a camera nao ligou (ver o log)"));
    return false;
  }
  const tab5_cam_destino_t d = { sup.buf, sup.bytes, sup.pic_l, sup.pic_a, sup.x, sup.y,
                                 s_cam.r.largura, s_cam.r.altura, sistema_pedir_redesenho };
  tab5_camera_definir_destino(&d);
  return true;
}

// Desliga: espera a tarefa da camera (nenhum PPA escrevendo depois daqui) e
// so entao devolve o buffer de uma foto que nao chegou a virar trabalho.
static void cam_descer(void)
{
  tab5_camera_parar(s_cam_quem);
  if (s_cam.foto_estado == FOTO_CAPTURANDO) { s_cam.foto_estado = FOTO_FALHOU; }
  s_cam.foto_pedida = false;
  cam_soltar_foto();
}

// Chamada pela interface a cada pedido; sem nada pedido nao faz nada.
void cam_aplicar(void)
{
  if (s_cam.pedido == CAM_DESLIGAR) {
    s_cam.pedido = CAM_NADA;
    if (s_cam.ligada) { cam_descer(); s_cam.ligada = false; sistema_pedir_redesenho_completo(); }
  } else if (s_cam.pedido == CAM_LIGAR) {
    s_cam.pedido = CAM_NADA;
    const area_t a = sistema_area();
    const int y = s_cam.ped_y < 0 ? 0 : s_cam.ped_y;
    int al = s_cam.ped_a;
    if (y + al > a.altura) { al = a.altura - y; }
    const int topo = (a.y + y + 7) & ~7;               // o topo desce ate o multiplo de 8
    al = (al - (topo - (a.y + y))) & ~7;
    const area_t r = { a.x, topo, a.largura, al };
    if (al < 64) {
      snprintf(s_cam.erro, sizeof(s_cam.erro), "camera.ligar: faixa pequena demais");
    } else if (!s_cam.ligada || memcmp(&r, &s_cam.r, sizeof(r)) != 0) {
      if (s_cam.ligada) { cam_descer(); s_cam.ligada = false; }
      s_cam.r = r;
      s_cam.ligada = cam_subir();
      sistema_pedir_redesenho_completo();
    }
  }
  if (s_cam.foto_pedida) {
    s_cam.foto_pedida = false;
    if (!s_cam.foto) { s_cam.foto = tab5_camera_quadro_alocar(TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A); }
    if (!s_cam.ligada || !s_cam.foto || !tab5_camera_pedir_quadro(s_cam.foto, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A)) {
      s_cam.foto_estado = FOTO_FALHOU;
      cam_soltar_foto();
    }
  }
}

// A GRAVACAO leva tudo o que precisa: o trabalho e solto e pode terminar com
// o app ja fechado (o buffer passa a ser dele).
typedef struct {
  uint16_t* px;
  char      app[48];     // a pasta relativa (pasta_rel): Berry/<nome do app>
  char      nome[40];
} cam_gravacao_t;

static void cam_trabalho_gravar(void* arg)
{
  cam_gravacao_t* g = (cam_gravacao_t*)arg;
  if (!g->nome[0]) {   // sem nome: o maior foto_NNNN da pasta + 1
    unsigned maior = 0;
    char pasta[96];
    if (tab5_sd_montar()) {
      if (tab5_sd_pasta(g->app, pasta, sizeof(pasta))) {
        DIR* d = opendir(pasta);
        struct dirent* e;
        while (d && (e = readdir(d))) {
          unsigned n;
          if (sscanf(e->d_name, "foto_%u.bmp", &n) == 1 && n > maior) { maior = n; }
        }
        if (d) { closedir(d); }
      }
      tab5_sd_desmontar();
    }
    snprintf(g->nome, sizeof(g->nome), "foto_%04u", maior + 1);
  }
  const bool ok = tab5_bmp_salvar(g->app, g->nome, g->px, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A);
  if (ok) { snprintf(s_cam.ultima, sizeof(s_cam.ultima), "%s.bmp", g->nome); }
  s_cam.foto_estado = ok ? FOTO_OK : FOTO_FALHOU;
  heap_caps_free(g->px);
  heap_caps_free(g);
  sistema_pedir_redesenho();
}

// A cada desenhar, antes do script: o quadro novo vai para a tela e a foto
// que chegou vai para o cartao.
void cam_quadro(const berry_app_t* app)
{
  if (!s_cam.ligada) { return; }
  if (tab5_camera_quadro_escrito()) { sistema_superficie_mudou(s_cam.r); }
  bool ok = false;
  if (s_cam.foto_estado != FOTO_CAPTURANDO || !s_cam.foto || !tab5_camera_quadro_pronto(s_cam.foto, &ok)) { return; }
  cam_gravacao_t* g = ok ? (cam_gravacao_t*)heap_caps_malloc(sizeof(cam_gravacao_t), MALLOC_CAP_DEFAULT) : nullptr;
  if (g) {
    g->px = s_cam.foto;
    pasta_rel(app->nome, g->app, sizeof(g->app));
    snprintf(g->nome, sizeof(g->nome), "%s", s_cam.foto_nome);
    if (sistema_trabalho_solto(cam_trabalho_gravar, g)) {
      s_cam.foto = nullptr;
      s_cam.foto_estado = FOTO_GRAVANDO;
      return;
    }
    heap_caps_free(g);
  }
  s_cam.foto_estado = FOTO_FALHOU;
}

// A area sem a faixa do preview (o PPA ja escreveu la; apagar seria piscar).
void cam_limpar_fora(const area_t& a)
{
  const int r0 = s_cam.r.y, r1 = s_cam.r.y + s_cam.r.altura;
  if (r0 > a.y)              { tab5_tela_limpar(a.x, a.y, a.largura, r0 - a.y, TFT_BLACK); }
  if (r1 < a.y + a.altura)   { tab5_tela_limpar(a.x, r1, a.largura, a.y + a.altura - r1, TFT_BLACK); }
}

// Repouso: nada aqui. O sistema pausa a camera do app aberto com a tela
// apagada e a religa ao acordar (kernel/core.cpp, tab5_camera_pausar) - o
// script nem fica sabendo.

// O app fechou: tudo desce, e o estado volta ao zero para o proximo.
void cam_fechar(void)
{
  if (s_cam.ligada) { cam_descer(); }
  cam_soltar_foto();
  const int e = s_cam.foto_estado;
  s_cam = cam_t{};
  if (e == FOTO_GRAVANDO) { s_cam.foto_estado = FOTO_GRAVANDO; }   // o trabalho solto ainda escreve aqui
}

const membro_t BERRY_MOD_CAMERA[] = {
  { "ligar", cam_ligar }, { "desligar", cam_desligar }, { "ligada", cam_ligada }, { "erro", cam_erro },
  { "faixa", cam_faixa }, { "foto", cam_foto }, { "foto_estado", cam_foto_estado }, { "ultima", cam_ultima },
  { nullptr },
};
