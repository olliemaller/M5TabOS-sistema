// Apps em Berry - ver berry_apps.hpp.
//
// ESTE ARQUIVO E O MOTOR: os trabalhos que rodam na tarefa do Berry, o app_t de
// cada vaga, a leitura da pasta no boot, o app de exemplo e o registro dos
// modulos (berry_modulos_registrar). Os MODULOS que o script chama moram em
// services/berry/, um arquivo por assunto, com o que compartilham em
// berry_interno.hpp:
//   berry_mod_tela.cpp     tela, camadas e teclado
//   berry_mod_sistema.cpp  sistema (tempo, energia, avisos, bip, fundo), idioma, transferencia
//   berry_mod_cartao.cpp   cartao (a pasta do app) e arquivos (os discos)
//   berry_mod_radio.cpp    espnow e mesh
//   berry_mod_gnss.cpp     gnss
//   berry_mod_camera.cpp   camera
//   berry_mod_musica.cpp   musica
//   berry_mod_ajustes.cpp  ajustes
//   berry_mod_rede.cpp     rede
//   berry_mod_tema.cpp     tema (as cores e medidas do tema central)

#include "tab5_arquivos.hpp"
#include "idioma.h"
#include "berry_apps.hpp"
#include "berry_vm.hpp"
#include "berry_mem.h"
extern "C" {
#include "be_gc.h"   // be_gc_collect / be_gc_memcount (fora da API publica)
}

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app.h"
#include "tab5_ui.hpp"
#include "tab5_tela.hpp"
#include "tab5_sd.hpp"
#include "tab5_imu.hpp"
#include "notificacao.h"
#include "teclado_virtual.hpp"
#include "tab5_teclado.hpp"
#include "fundo.h"
#include "pool_tarefas.h"
#include "berry/berry_interno.hpp"

static const char* TAG = "berry.apps";

// Prazos de cada chamada ao script (berry_rodar). Desenhar e tocar acontecem
// com a interface parada esperando: mais que isso e tela congelada.
static const uint32_t PRAZO_ABRIR_MS  = 5000;
static const uint32_t PRAZO_EVENTO_MS = 2000;

static berry_app_t s_apps[BERRY_APPS_MAX];
static app_t       s_desc[BERRY_APPS_MAX];   // o app_t de cada um, montado no boot
static int         s_n = 0;

// As funcoes do script que o sistema chama. Sem a funcao, o evento nem vai a
// tarefa do Berry: uma ida e volta a menos (o toque de um app sem tocar(), a
// tecla de um app sem tecla()...).
enum { F_DESENHAR = 1, F_TOCAR = 2, F_DEDO = 4, F_TECLA = 8, F_AVISO = 16, F_FUNDO = 32 };

// OS AVISOS que um app Berry pode pedir no cabecalho (# avisos: espnow, mesh):
// quando o servico publica, o app e redesenhado NA HORA - sem periodo, sem
// perguntar de tempos em tempos - e o script recebe aviso(nome), se tiver.
static const struct { const char* nome; uint32_t bits; } AVISOS[] = {
  { "espnow", AVISO_ESPNOW },
  { "mesh",   AVISO_MESH_MSG | AVISO_MESH_ENVIO | AVISO_MESH_NOS },
  { "gps",    AVISO_GPS | AVISO_TRILHA },
  { "volumes", AVISO_VOLUMES },   // pendrive entrou ou saiu
  { "arquivos", AVISO_ARQUIVOS }, // a copia (arquivos.copiar) andou ou terminou
};


// O contexto do trabalho em curso (ver contexto_t em berry/berry_interno.hpp).
static contexto_t  s_ctx_vazio = {};
contexto_t* s_ctx = &s_ctx_vazio;

// ---------------------------------------------------------------------------
// REGISTRO DOS MODULOS
// ---------------------------------------------------------------------------
static void modulo(bvm* vm, const char* nome, const membro_t* m)
{
  be_newmodule(vm);
  for (; m->nome; ++m) {
    if (m->fn) { be_pushntvfunction(vm, m->fn); }
    else       { be_pushint(vm, m->valor); }
    be_setmember(vm, -2, m->nome);
    be_pop(vm, 1);
  }
  be_setglobal(vm, nome);
  be_pop(vm, 1);
}

void berry_modulos_registrar(bvm* vm)
{
  modulo(vm, "tela", BERRY_MOD_TELA);
  modulo(vm, "sistema", BERRY_MOD_SISTEMA);
  modulo(vm, "teclado", BERRY_MOD_TECLADO);
  modulo(vm, "cartao", BERRY_MOD_CARTAO);
  modulo(vm, "arquivos", BERRY_MOD_ARQUIVOS);
  modulo(vm, "espnow", BERRY_MOD_ESPNOW);
  modulo(vm, "mesh", BERRY_MOD_MESH);
  modulo(vm, "camera", BERRY_MOD_CAMERA);
  modulo(vm, "gnss", BERRY_MOD_GNSS);
  modulo(vm, "musica", BERRY_MOD_MUSICA);
  modulo(vm, "idioma", BERRY_MOD_IDIOMA);
  modulo(vm, "transferencia", BERRY_MOD_TRANSFERENCIA);
  modulo(vm, "rede", BERRY_MOD_REDE);
  modulo(vm, "ajustes", BERRY_MOD_AJUSTES);
  modulo(vm, "tema", BERRY_MOD_TEMA);
}

// ---------------------------------------------------------------------------
// OS TRABALHOS (rodam na tarefa do Berry)
// ---------------------------------------------------------------------------
typedef struct {
  berry_app_t* app;
  int          evento;      // EV_*
  contexto_t*  ctx;         // o contexto deste trabalho (ver contexto_t)
  bool         tudo;        // desenhar
  bool         manter;      // fechar: o fundo segue, a maquina fica
  int          x, y;        // tocar
  const char*  texto;       // tecla
  int          codigo;
} pedido_t;

enum { EV_ABRIR, EV_FECHAR, EV_DESENHAR, EV_TOCAR, EV_TECLA, EV_DEDO, EV_AVISO, EV_FUNDO, EV_APAGAR, EV_N };
static const char* const EV_NOME[EV_N] = { "abrir", "fechar", "desenhar", "tocar", "tecla", "dedo", "aviso", "fundo", "apagar" };

// O fundo() tem prazo CURTO: enquanto ele roda, a interface que quiser o
// Berry espera (o prazo e o pior caso de um toque atrasado).
static const uint32_t PRAZO_FUNDO_MS = 200;

// Quanto o script leva em cada evento (so o Berry rodando) - 'berry tempos'.
static tempo_t s_tempo[EV_N];

static void apagar_vm(berry_app_t* a)
{
  if (a->vm) { be_vm_delete(a->vm); a->vm = nullptr; }
  gnss_soltar(a);   // a assinatura do GPS era desta maquina
  rede_soltar(a);   // e o pedido de rede (e a resposta que ninguem leu) tambem
}

static void trabalho(void* arg)
{
  pedido_t* p = (pedido_t*)arg;
  berry_app_t* a = p->app;
  s_ctx = p->ctx;
  alvo_tela();   // todo evento comeca desenhando na tela (ou em lugar nenhum, fora do desenhar)
  const int64_t t0 = esp_timer_get_time();
  char* erro = a->erro;
  const size_t et = sizeof(a->erro);

  switch (p->evento) {
    case EV_ABRIR:
      a->usa_imu = false;
      a->dedo_antes = false;
      a->acumular = false;
      a->periodo = a->periodo_cab;
      camadas_liberar(a);
      if (a->vm) {
        // A MAQUINA SOBREVIVEU (o fundo estava rodando): o estado do script
        // continua - so a janela volta. O codigo de fora das funcoes NAO roda
        // de novo; abrir() sim.
        if (berry_funcao(a->vm, "abrir")) { berry_chamar(a->vm, 0, erro, et); }
        break;
      }
      a->erro[0] = 0;
      a->funcs = 0;
      a->pasta[0] = 0;
      a->vm = berry_vm_nova();
      if (!a->vm) {
        snprintf(erro, et, tr("sem memoria para o Berry (%u KB de %u KB em uso)"),
                 (unsigned)(berry_mem_usada() / 1024), (unsigned)(BERRY_ORCAMENTO / 1024));
        break;
      }
      // O codigo de fora das funcoes roda aqui (define as funcoes e o estado).
      if (berry_executar(a->vm, a->arquivo, true, false, erro, et)) {
        static const struct { const char* nome; uint8_t f; } FUNCS[] = {
          { "desenhar", F_DESENHAR }, { "tocar", F_TOCAR }, { "dedo", F_DEDO }, { "tecla", F_TECLA },
          { "aviso", F_AVISO }, { "fundo", F_FUNDO },
        };
        for (const auto& f : FUNCS) {
          if (berry_funcao(a->vm, f.nome)) { a->funcs |= f.f; be_pop(a->vm, 1); }   // so queria saber se existe
        }
        if (berry_funcao(a->vm, "abrir")) { berry_chamar(a->vm, 0, erro, et); }
      }
      break;
    case EV_FECHAR:
      if (a->vm) {
        if (berry_funcao(a->vm, "fechar")) { berry_chamar(a->vm, 0, nullptr, 0); }
        if (p->manter) {
          // Vai para o segundo plano: a coleta AGORA. Com o steprate 350 uma
          // maquina parada guardaria ate 4,5x o lixo do que esta vivo.
          be_gc_collect(a->vm);
        } else {
          apagar_vm(a);
        }
      }
      camadas_liberar(a);   // as camadas sao da janela: voltam ao fechar
      break;
    case EV_APAGAR:
      apagar_vm(a);
      break;
    case EV_FUNDO:
      if (!a->vm || a->erro[0] || !berry_funcao(a->vm, "fundo")) { a->fundo_sair = true; break; }
      if (!berry_chamar(a->vm, 0, erro, et)) { a->fundo_sair = true; }
      break;
    case EV_DESENHAR:
      if (a->vm && !a->erro[0] && berry_funcao(a->vm, "desenhar")) {
        be_pushbool(a->vm, p->tudo);
        berry_chamar(a->vm, 1, erro, et);
      }
      break;
    case EV_TOCAR:
      if (a->vm && !a->erro[0] && berry_funcao(a->vm, "tocar")) {
        be_pushint(a->vm, p->x);
        be_pushint(a->vm, p->y);
        berry_chamar(a->vm, 2, erro, et);
      }
      break;
    case EV_DEDO:
      if (a->vm && !a->erro[0] && berry_funcao(a->vm, "dedo")) {
        be_pushint(a->vm, p->x);
        be_pushint(a->vm, p->y);
        be_pushbool(a->vm, p->codigo != 0);
        berry_chamar(a->vm, 3, erro, et);
      }
      break;
    case EV_AVISO:
      if (a->vm && !a->erro[0] && berry_funcao(a->vm, "aviso")) {
        be_pushstring(a->vm, p->texto);
        berry_chamar(a->vm, 1, erro, et);
      }
      break;
    case EV_TECLA:
      if (a->vm && !a->erro[0] && berry_funcao(a->vm, "tecla")) {
        be_pushstring(a->vm, p->texto);
        be_pushint(a->vm, p->codigo);
        berry_chamar(a->vm, 2, erro, et);
      }
      break;
  }
  C->tela_d = nullptr;   // nenhum desenho na tela fora do desenhar()
  alvo_tela();
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  tempo_t& tm = s_tempo[p->evento];
  tm.n++;
  tm.soma_us += us;
  if (us > tm.max_us) { tm.max_us = us; }
  s_ctx = &s_ctx_vazio;
}

// O contexto dos trabalhos da JANELA (so a interface pede estes).
static contexto_t s_ctx_janela;

// O pedido de ligar/desligar o fundo, na interface (o kernel liga e para na
// tarefa de quem pede - e ao_parar roda sempre aqui).
static void fundo_aplicar(berry_app_t* a)
{
  const int ped = a->fundo_pedido;
  a->fundo_pedido = FP_NADA;
  if (ped == FP_LIGAR && !sistema_fundo_ativo(&a->fundo_def)) {
    if (!(a->funcs & F_FUNDO)) {
      snprintf(a->erro, sizeof(a->erro), tr("sistema.fundo(true) sem a funcao fundo() no script"));
      return;
    }
    a->fundo_sair = false;
    if (!sistema_fundo_iniciar(&a->fundo_def)) { ESP_LOGW(TAG, "%s: o fundo nao ligou (sem vaga?)", a->nome); }
  } else if (ped == FP_DESLIGAR && sistema_fundo_ativo(&a->fundo_def)) {
    a->fundo_sair = true;
    sistema_fundo_parar(&a->fundo_def);
  }
}

// Na tarefa de interface: entrega o pedido, espera, e faz o que so ela pode.
// 'd' e 'area': a tela do desenhar() (sem eles, so a area - tela.largura e
// tela.altura valem em todo evento; desenhar, so no desenhar()).
static void pedir(pedido_t& p, uint32_t prazo_ms, lgfx::LovyanGFX* d = nullptr, const area_t* area = nullptr)
{
  const bool tinha_erro = p.app->erro[0] != 0;
  s_ctx_janela = contexto_t{};
  s_ctx_janela.app = p.app;
  s_ctx_janela.tela_d = d;
  s_ctx_janela.tela_a = area ? *area : sistema_area();
  p.ctx = &s_ctx_janela;
  berry_rodar(trabalho, &p, prazo_ms);
  if (s_bip_hz > 0) { sistema_beep(s_bip_hz, s_bip_ms); s_bip_hz = 0; }
  cam_aplicar();   // o que o script pediu da camera (sem pedido, nao faz nada)
  if (s_acordado_pedido >= 0) {
    sistema_manter_acordado(SISTEMA_ACORDADO_APP, s_acordado_pedido == 1);   // solto sozinho ao fechar
    s_acordado_pedido = -1;
  }
  if (s_kb_pedido != KB_NADA) {
    const bool abrir = s_kb_pedido == KB_ABRIR;
    s_kb_pedido = KB_NADA;
    if (abrir)                                { tecvirt_abrir(&s_kb, s_kb_tipo); }   // aberto: so troca o tipo
    else if (!abrir && tecvirt_aberto(&s_kb)) { tecvirt_fechar(&s_kb); }
    sistema_pedir_redesenho_completo();
  }
  if (p.app->fundo_pedido != FP_NADA) { fundo_aplicar(p.app); }
  if (!tinha_erro && p.app->erro[0]) {
    ESP_LOGW(TAG, "%s: %s", p.app->nome, p.app->erro);
    sistema_pedir_redesenho_completo();   // a tela de erro substitui o app
  }
}

// O erro, no lugar do app: o script parou, o aparelho nao.
static void desenhar_erro(lgfx::LovyanGFX& d, const area_t& a, const berry_app_t* app)
{
  tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TFT_BLACK);
  const lgfx::TextStyle estilo = d.getTextStyle();
  tab5_fonte_corpo_negrito(d);
  tab5_texto(d, 3.0f);
  d.setTextDatum(top_left);
  d.setTextColor(TFT_RED);
  d.drawString(tr("O script parou com um erro:"), a.x + 24, a.y + 40);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.4f);
  d.setTextColor(TFT_WHITE);
  // Quebra a mensagem em linhas que cabem na largura.
  const char* p = app->erro;
  int y = a.y + 100;
  char linha[96];
  while (*p && y < a.y + a.altura - 40) {
    size_t n = strlen(p) < sizeof(linha) - 1 ? strlen(p) : sizeof(linha) - 1;
    memcpy(linha, p, n);
    linha[n] = 0;
    n = tab5_cortar_ao_caber(d, linha, a.largura - 48);
    d.drawString(linha, a.x + 24, y);
    p += n ? n : 1;
    y += 40;
  }
  d.setTextColor(TFT_DARKGREY);
  d.drawString(app->arquivo, a.x + 24, y + 20);
  d.setTextStyle(estilo);
}

// ---------------------------------------------------------------------------
// OS app_t: um conjunto de funcoes por vaga (o app_t nao carrega contexto)
// ---------------------------------------------------------------------------
template <int I> static bool abrir_i(void)
{
  s_apps[I].janela = true;
  pedido_t p = { &s_apps[I], EV_ABRIR };
  pedir(p, PRAZO_ABRIR_MS);
  return true;   // com erro abre assim mesmo: a tela mostra o que houve
}

template <int I> static void fechar_i(void)
{
  berry_app_t* a = &s_apps[I];
  pedido_t p = { a, EV_FECHAR };
  // O fundo rodando segura a maquina: a janela fecha, o script continua.
  p.manter = sistema_fundo_ativo(&a->fundo_def) && !a->fundo_sair && !a->erro[0];
  pedir(p, PRAZO_ABRIR_MS);
  a->janela = false;
  s_kb_pedido = KB_NADA;
  tecvirt_fechar(&s_kb);
  cam_fechar();
}

template <int I> static void desenhar_i(lgfx::LovyanGFX& d, const area_t& a)
{
  berry_app_t* app = &s_apps[I];
  if (app->erro[0]) {
    if (sistema_area_foi_limpa()) { desenhar_erro(d, a, app); }
    return;
  }
  // SEMPRE A TELA INTEIRA: um script redesenha tudo a cada vez (e o jeito
  // simples de escrever). Na passada parcial (periodo, tocar, redesenhar) a
  // limpeza e AQUI, na hora - tudo vai para a pagina de tras do compositor e o
  // quadro sai inteiro, sem piscar. Antes ela virava pedido de passada
  // completa: o quadro so saia na volta SEGUINTE da interface (ate ~20 ms a
  // mais em cada toque e cada quadro de animacao) e ainda repintava as barras
  // de status e de navegacao sem precisar.
  // ACUMULANDO (sistema.acumular), a passada parcial NAO limpa: o script
  // desenha so o que mudou por cima do que ja esta la (desenhar(false)).
  // COM A CAMERA LIGADA a faixa do preview nao e apagada (o PPA escreve la)
  // e o quadro novo entra ANTES do script - o que ele desenhar por cima (uma
  // mira, um texto) sai junto, em cima do preview.
  bool tudo = sistema_area_foi_limpa();
  if (!tudo && !app->acumular) {
    if (cam_no_ar()) { cam_limpar_fora(a); } else { tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TFT_BLACK); }
    tudo = true;
  }
  cam_quadro(app);
  if (!(app->funcs & F_DESENHAR)) { return; }
  // A IMU fala I2C: a leitura e aqui, na interface, nunca na tarefa do Berry.
  // So se o quadro anterior usou; este remarca se usar de novo.
  if (app->usa_imu) { tab5_imu_medir(); }
  app->usa_imu = false;
  pedido_t p = { app, EV_DESENHAR };
  p.tudo = tudo;
  pedir(p, PRAZO_EVENTO_MS, &d, &a);
  if (app->erro[0]) { desenhar_erro(d, a, app); return; }
  // O teclado por cima, na base da area (o script desenha so ate tela.altura()).
  // Acumulando, so o que mudou nele; senao a area foi limpa e ele vai inteiro.
  if (tecvirt_aberto(&s_kb)) {
    tecvirt_posicionar(&s_kb, a);
    tecvirt_pintar(&s_kb, d, tudo);
  }
}

// Uma acao do teclado virtual vira o mesmo tecla(texto, codigo) do fisico.
static void entregar_acao(berry_app_t* app, int ac)
{
  char txt[5] = "";
  int codigo = TAB5_TECLA_NENHUMA;
  if (ac > 0)                    { txt[tecvirt_utf8(ac, txt)] = 0; }
  else if (ac == TECVIRT_APAGAR) { codigo = TAB5_TECLA_APAGAR; }
  else if (ac == TECVIRT_ENVIAR || ac == TECVIRT_LINHA) { codigo = TAB5_TECLA_ENTER; }   // a tecla verde, em qualquer tipo
  else if (ac == TECVIRT_FECHAR) { tecvirt_fechar(&s_kb); sistema_pedir_redesenho_completo(); codigo = TAB5_TECLA_ESC; }
  else { return; }
  if (!(app->funcs & F_TECLA)) { sistema_pedir_redesenho(); return; }
  pedido_t p = { app, EV_TECLA };
  p.texto = txt; p.codigo = codigo;
  pedir(p, PRAZO_EVENTO_MS);
  sistema_pedir_redesenho();
}

template <int I> static void tocar_i(int x, int y)
{
  const area_t a = sistema_area();
  if (tecvirt_aberto(&s_kb) && tecvirt_contem(&s_kb, a.x + x, a.y + y)) {
    const int ac = tecvirt_clique(&s_kb, a.x + x, a.y + y);
    if (ac) { entregar_acao(&s_apps[I], ac); }
    return;
  }
  if (!(s_apps[I].funcs & F_TOCAR)) { return; }
  pedido_t p = { &s_apps[I], EV_TOCAR };
  p.x = x; p.y = y;
  pedir(p, PRAZO_EVENTO_MS);
  sistema_pedir_redesenho();
}

template <int I> static void teclar_i(const char* texto, uint8_t codigo, uint8_t)
{
  if (!(s_apps[I].funcs & F_TECLA)) { return; }
  pedido_t p = { &s_apps[I], EV_TECLA };
  p.texto = texto; p.codigo = codigo;
  pedir(p, PRAZO_EVENTO_MS);
  sistema_pedir_redesenho();
}

// O toque por nivel: alimenta o teclado aberto e, se o script tem dedo(x, y,
// pressionado), entrega o dedo enquanto ele esta na tela e uma vez ao sair.
template <int I> static void nivel_i(int x, int y, bool pressionado)
{
  berry_app_t* app = &s_apps[I];
  const area_t a = sistema_area();
  if (tecvirt_aberto(&s_kb)) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb, acoes, 8);
    for (int i = 0; i < n && tecvirt_aberto(&s_kb); ++i) { entregar_acao(app, acoes[i]); }
    if (pressionado && tecvirt_contem(&s_kb, a.x + x, a.y + y)) { pressionado = false; x = y = -1; }
  }
  if (!(app->funcs & F_DEDO) || app->erro[0] || (!pressionado && !app->dedo_antes)) { return; }
  app->dedo_antes = pressionado;
  pedido_t p = { app, EV_DEDO };
  p.x = x; p.y = y; p.codigo = pressionado;
  pedir(p, PRAZO_EVENTO_MS);
  sistema_pedir_redesenho();
}

template <int I> static uint32_t periodo_i(void) { return s_apps[I].periodo; }

// Um aviso do cabecalho chegou: aviso(nome) no script (um por grupo que
// mudou) e redesenho. Sem aviso() no script, so o redesenho.
template <int I> static void aviso_i(uint32_t bits)
{
  berry_app_t* app = &s_apps[I];
  if ((app->funcs & F_AVISO) && !app->erro[0]) {
    for (const auto& av : AVISOS) {
      if (!(bits & av.bits)) { continue; }
      pedido_t p = { app, EV_AVISO };
      p.texto = av.nome;
      pedir(p, PRAZO_EVENTO_MS);
    }
  }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// A PARTE DE FUNDO DE UM APP BERRY (kernel/fundo.h)
// ---------------------------------------------------------------------------
// O MODELO COOPERATIVO: o laco e uma tarefa do pool do kernel (nenhuma pilha
// nova), mas o fundo() do script roda na MESMA tarefa do Berry que a janela,
// um trabalho curto de cada vez (PRAZO_FUNDO_MS). A janela e o fundo nunca
// rodam juntos - por isso dividem a maquina (e as variaveis) sem trava. A
// interface pede com prioridade 4 e o fundo com 1: na fila do Berry, o toque
// passa na frente (berry_rodar).
template <int I> static void fundo_laco_i(fundo_t* f)
{
  berry_app_t* a = &s_apps[I];
  contexto_t ctx = {};
  ctx.app = a;
  ctx.fundo = true;
  for (;;) {
    if (a->fundo_sair) { break; }
    pedido_t p = { a, EV_FUNDO };
    p.ctx = &ctx;
    berry_rodar(trabalho, &p, PRAZO_FUNDO_MS);
    fundo_estado(f, a->fundo_n, a->fundo_txt[0] ? a->fundo_txt : "rodando");
    if (a->erro[0]) {
      ESP_LOGW(TAG, "%s (fundo): %s", a->nome, a->erro);
      if (a->janela) { sistema_pedir_redesenho_completo(); }   // a tela de erro
      break;
    }
    if (a->fundo_sair || !fundo_esperar(f, a->fundo_ms)) { break; }
  }
}

template <int I> static bool fundo_iniciar_i(void)
{
  berry_app_t* a = &s_apps[I];
  a->fundo_sair = false;
  a->fundo_txt[0] = 0;
  a->fundo_n = 0;
  return a->vm != nullptr;
}

// Na interface, depois que o laco saiu: com a janela fechada, ninguem mais
// precisa da maquina - toda a memoria volta.
template <int I> static void fundo_parar_i(void)
{
  berry_app_t* a = &s_apps[I];
  a->fundo_sair = true;
  if (!a->janela && a->vm) {
    pedido_t p = { a, EV_APAGAR };
    p.ctx = &s_ctx_vazio;
    berry_rodar(trabalho, &p, PRAZO_EVENTO_MS);
  }
  ESP_LOGI(TAG, "%s: fundo parado%s", a->nome, a->janela ? "" : " - maquina apagada");
}

static bool fundo_sem_som(void) { return false; }   // a pastilha so mostra; nao bipa

typedef struct {
  bool (*abrir)(void);
  void (*fechar)(void);
  void (*desenhar)(lgfx::LovyanGFX&, const area_t&);
  void (*tocar)(int, int);
  void (*teclar)(const char*, uint8_t, uint8_t);
  void (*nivel)(int, int, bool);
  uint32_t (*periodo)(void);
  void (*aviso)(uint32_t);
  fundo_corpo_t fundo_laco;
  bool (*fundo_iniciar)(void);
  void (*fundo_parar)(void);
} vaga_t;

template <int... I> struct vagas_s { static constexpr vaga_t v[] = { { abrir_i<I>, fechar_i<I>, desenhar_i<I>, tocar_i<I>, teclar_i<I>, nivel_i<I>, periodo_i<I>, aviso_i<I>, fundo_laco_i<I>, fundo_iniciar_i<I>, fundo_parar_i<I> }... }; };
static const vaga_t* const VAGAS = vagas_s<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15>::v;
static_assert(BERRY_APPS_MAX == 16, "a lista de vagas acima tem 16 entradas");

// ---------------------------------------------------------------------------
// LEITURA DA PASTA NO BOOT
// ---------------------------------------------------------------------------
// Le o cabecalho (# nome: / # cor: / # periodo:) das primeiras linhas.
static void ler_cabecalho(const char* caminho, char* nome, size_t nome_tam,
                          uint16_t* cor, uint32_t* periodo, uint32_t* avisos, uint8_t* orientacao)
{
  FILE* f = fopen(caminho, "r");
  if (!f) { return; }
  char l[96];
  for (int i = 0; i < 12 && fgets(l, sizeof(l), f); ++i) {
    char* p = l;
    while (*p == ' ' || *p == '\t') { ++p; }
    if (*p != '#') { continue; }
    ++p;
    while (*p == ' ') { ++p; }
    char* v = strchr(p, ':');
    if (!v) { continue; }
    *v++ = 0;
    while (*v == ' ') { ++v; }
    v[strcspn(v, "\r\n")] = 0;
    if (!strcasecmp(p, "nome") && *v)  { snprintf(nome, nome_tam, "%s", v); }
    else if (!strcasecmp(p, "cor"))     { *cor = (uint16_t)strtoul(v, nullptr, 0); }
    else if (!strcasecmp(p, "periodo")) { *periodo = (uint32_t)strtoul(v, nullptr, 0); }
    else if (!strcasecmp(p, "orientacao")) {
      if (!strncasecmp(v, "deitado", 7))                                   { *orientacao = ORI_DEITADO; }
      else if (!strncasecmp(v, "girar", 5) || !strncasecmp(v, "imu", 3))  { *orientacao = ORI_SEGUE_IMU; }
      else if (!strncasecmp(v, "fixo", 4))                                 { *orientacao = ORI_FIXO_EM_PE; }
      else                                                                 { *orientacao = ORI_EM_PE; }
    }
    else if (!strcasecmp(p, "avisos")) {
      // "espnow, mesh" - nomes separados por virgula ou espaco.
      char* resto = nullptr;
      for (char* t = strtok_r(v, ", ", &resto); t; t = strtok_r(nullptr, ", ", &resto)) {
        for (const auto& av : AVISOS) { if (!strcasecmp(t, av.nome)) { *avisos |= av.bits; } }
      }
    }
  }
  fclose(f);
}

// O QUE A PASTA TEM: lido do cartao por quem pode esperar (o boot, o fim do
// pendrive, o console) e registrado pela interface (berry_apps_atender).
typedef struct {
  char     arquivo[96];
  char     nome[24];
  uint16_t cor;
  uint32_t periodo;
  uint32_t avisos;   // AVISO_* pedidos no cabecalho
  uint8_t  orientacao;   // orientacao_t: '# orientacao: em_pe | deitado | girar | fixo'
} lido_t;

typedef struct {
  int    n;
  lido_t v[BERRY_APPS_MAX];
} lista_t;

// O APP DE EXEMPLO (berry_exemplos/Ola.be, embutido pelo main/CMakeLists.txt).
// Vai para o cartao SO quando a pasta apps/Berry ainda nao existe - o primeiro
// boot com aquele cartao. Apagado depois, nao volta: a pasta ja existe.
extern const char OLA_BE_INICIO[] asm("_binary_Ola_be_start");
extern const char OLA_BE_FIM[]    asm("_binary_Ola_be_end");

static void instalar_exemplo(void)
{
  FILE* f = tab5_sd_abrir(BERRY_PASTA, "Ola.be", "wb");
  if (!f) { return; }
  const size_t n = (size_t)(OLA_BE_FIM - OLA_BE_INICIO) - 1;   // TEXT: sem o '\0' do fim
  const bool ok = fwrite(OLA_BE_INICIO, 1, n, f) == n;
  fclose(f);
  ESP_LOGI(TAG, "app de exemplo %s em apps/%s/Ola.be", ok ? "instalado" : "NAO instalado", BERRY_PASTA);
}

static lista_t* ler_pasta(void)
{
  char pasta[64];
  if (!tab5_sd_montar()) { return nullptr; }
  struct stat st;
  const bool primeira_vez = stat(TAB5_SD_APPS "/" BERRY_PASTA, &st) != 0;
  if (!tab5_sd_pasta(BERRY_PASTA, pasta, sizeof(pasta))) { tab5_sd_desmontar(); return nullptr; }
  if (primeira_vez) { instalar_exemplo(); }
  lista_t* L = (lista_t*)heap_caps_calloc(1, sizeof(lista_t), MALLOC_CAP_SPIRAM);
  DIR* d = L ? opendir(pasta) : nullptr;
  struct dirent* e;
  while (d && L->n < BERRY_APPS_MAX && (e = readdir(d))) {
    const size_t n = strlen(e->d_name);
    if (n < 4 || strcasecmp(e->d_name + n - 3, ".be") != 0 || e->d_name[0] == '.') { continue; }
    lido_t* l = &L->v[L->n++];
    snprintf(l->arquivo, sizeof(l->arquivo), "%s/%s", pasta, e->d_name);
    snprintf(l->nome, sizeof(l->nome), "%.*s", (int)(n - 3), e->d_name);
    l->cor = TFT_CYAN;
    ler_cabecalho(l->arquivo, l->nome, sizeof(l->nome), &l->cor, &l->periodo, &l->avisos, &l->orientacao);
  }
  if (d) { closedir(d); }
  tab5_sd_desmontar();
  return L;
}

static int s_base = -1;   // onde os apps Berry comecam em SISTEMA_APPS

static void registrar(const lista_t* L)
{
  s_n = 0;
  for (int i = 0; L && i < L->n; ++i) {
    const lido_t* l = &L->v[i];
    berry_app_t* a = &s_apps[s_n];
    // O mesmo arquivo de antes guarda a fonte de aviso (nao registra outra).
    const int notif = strcmp(a->arquivo, l->arquivo) == 0 ? a->notif : -1;
    *a = berry_app_t{};
    snprintf(a->arquivo, sizeof(a->arquivo), "%s", l->arquivo);
    snprintf(a->nome, sizeof(a->nome), "%s", l->nome);
    a->notif = notif;
    a->periodo_cab = a->periodo = l->periodo;
    a->fundo_ms = 1000;
    fundo_def_t& fd = a->fundo_def;
    fd.nome = a->nome;   // estavel: a lista so e trocada sem nenhuma maquina viva
    fd.corpo = VAGAS[s_n].fundo_laco;
    fd.pilha = 4096;     // o laco so entrega trabalhos: cabe numa vaga de 4 KB do pool
    fd.ao_iniciar = VAGAS[s_n].fundo_iniciar;
    fd.ao_parar = VAGAS[s_n].fundo_parar;
    fd.com_som = fundo_sem_som;
    if (notif >= 0) { notif_associar_app(notif, a->nome); }

    app_t& ap = s_desc[s_n];
    ap = app_t{};
    ap.nome = a->nome;
    ap.cor = l->cor;
    ap.perfil.imu = true;            // sistema.imu() e sistema.bip() valem para todo script
    ap.perfil.alto_falante = true;
    ap.usa_sd = true;                // o .be e lido do cartao ao abrir
    ap.exige_sd = true;
    ap.ao_abrir  = VAGAS[s_n].abrir;
    ap.ao_fechar = VAGAS[s_n].fechar;
    ap.desenhar  = VAGAS[s_n].desenhar;
    ap.ao_tocar  = VAGAS[s_n].tocar;
    ap.ao_teclar = VAGAS[s_n].teclar;
    ap.ao_tocar_nivel = VAGAS[s_n].nivel;
    ap.periodo_ms = l->periodo;
    ap.periodo_agora = VAGAS[s_n].periodo;   // sistema.periodo() muda com o app aberto
    ap.avisos = l->avisos;
    ap.ao_aviso = l->avisos ? VAGAS[s_n].aviso : nullptr;
    ap.orientacao = (orientacao_t)l->orientacao;
    if (!sistema_apps_registrar(&ap)) { break; }
    ESP_LOGI(TAG, "app Berry: %s (%s)", a->nome, l->arquivo);
    ++s_n;
  }
  for (int i = s_n; i < BERRY_APPS_MAX; ++i) { s_apps[i].arquivo[0] = 0; }
  ESP_LOGI(TAG, "%d app(s) Berry em /sdcard/apps/%s", s_n, BERRY_PASTA);
}

void berry_apps_iniciar(void)
{
  s_base = SISTEMA_NUM_APPS;
  lista_t* L = ler_pasta();
  if (!L) { ESP_LOGI(TAG, "sem cartao: nenhum app Berry"); return; }
  registrar(L);
  heap_caps_free(L);
}

// A lista lida, esperando a interface (nullptr = nada novo).
static lista_t* volatile s_pendente = nullptr;

void berry_apps_reler(void)
{
  lista_t* L = ler_pasta();
  if (!L) { return; }   // sem cartao agora: fica a lista de antes
  lista_t* velha = __atomic_exchange_n(&s_pendente, L, __ATOMIC_ACQ_REL);
  if (velha) { heap_caps_free(velha); }
  sistema_pedir_redesenho();   // acorda a interface
}

static bool mesma_lista(const lista_t* L)
{
  if (L->n != s_n) { return false; }
  for (int i = 0; i < s_n; ++i) {
    if (strcmp(L->v[i].arquivo, s_apps[i].arquivo) || strcmp(L->v[i].nome, s_apps[i].nome) ||
        L->v[i].cor != s_desc[i].cor || L->v[i].periodo != s_desc[i].periodo_ms ||
        L->v[i].avisos != s_desc[i].avisos) { return false; }
  }
  return true;
}

bool berry_apps_atender(void)
{
  if (!s_pendente || s_base < 0) { return false; }
  // Nenhum script aberto (a home nao tem app aberto, mas um .be com erro na
  // tela de fechar ainda pode estar com a maquina): fica para a proxima volta.
  for (int i = 0; i < s_n; ++i) { if (s_apps[i].vm) { return false; } }
  lista_t* L = __atomic_exchange_n(&s_pendente, (lista_t*)nullptr, __ATOMIC_ACQ_REL);
  if (!L) { return false; }
  const bool mudou = !mesma_lista(L);
  if (mudou) {
    sistema_apps_truncar(s_base);
    registrar(L);
  }
  heap_caps_free(L);
  return mudou;
}

bool berry_apps_eh_meu(const app_t* app)
{
  return app && app >= &s_desc[0] && app < &s_desc[s_n];
}

typedef struct {
  const fundo_def_t* fundo;
  char script[96];
  char dados[TAB5_ARQ_CAMINHO_MAX];
} remover_t;

static void corpo_remover(void* arg)
{
  remover_t* r = (remover_t*)arg;
  // A parte de fundo para antes (e a maquina e apagada com ela): sem isto a
  // releitura nao seria atendida - a lista so troca sem maquina viva.
  if (sistema_fundo_ativo(r->fundo)) { sistema_fundo_parar(r->fundo); }
  const tab5_arq_erro_t e = tab5_arq_apagar(r->script);
  struct stat st;
  if (r->dados[0] && tab5_sd_montar()) {
    const bool tem = stat(r->dados, &st) == 0;
    tab5_sd_desmontar();
    if (tem) { tab5_arq_apagar(r->dados); }
  }
  ESP_LOGI(TAG, "removido %s: %s", r->script, tab5_arq_erro_texto(e));
  heap_caps_free(r);
  berry_apps_reler();
}

void berry_apps_remover(const app_t* app)
{
  if (!berry_apps_eh_meu(app)) { return; }
  const berry_app_t* a = &s_apps[app - s_desc];
  remover_t* r = (remover_t*)heap_caps_calloc(1, sizeof(remover_t), MALLOC_CAP_SPIRAM);
  if (!r) { return; }
  r->fundo = &a->fundo_def;
  snprintf(r->script, sizeof(r->script), "%s", a->arquivo);
  char rel[48];
  pasta_rel(a->nome, rel, sizeof(rel));
  if (!tab5_sd_pasta_caminho(rel, r->dados, sizeof(r->dados))) { r->dados[0] = '\0'; }
  // O cartao bloqueia: numa vaga do pool, nunca na interface.
  if (!pool_criar(nullptr, corpo_remover, r, "berry_rem", 4096, 2)) { heap_caps_free(r); }
}

void berry_apps_tempos(bool zerar)
{
  berry_medida_t m;
  berry_medida(&m, zerar);
  printf("Berry task hand-off: %lu times, average %lu us, worst %lu us\n",
         (unsigned long)m.entregas,
         (unsigned long)(m.entregas ? m.espera_soma_us / m.entregas : 0),
         (unsigned long)m.espera_max_us);
  printf("%-9s %8s %10s %10s\n", "evento", "vezes", "media us", "pior us");
  for (int i = 0; i < EV_N; ++i) {
    const tempo_t& t = s_tempo[i];
    if (!t.n) { continue; }
    printf("%-9s %8lu %10lu %10lu\n", EV_NOME[i], (unsigned long)t.n,
           (unsigned long)(t.soma_us / t.n), (unsigned long)t.max_us);
  }
  // O desenhar() por dentro: quanto foi desenho (C) e quanto foi o script.
  const tempo_t& d = s_tempo[EV_DESENHAR];
  if (d.n) {
    uint64_t prim_us = 0;
    for (int i = 0; i < P_N; ++i) { prim_us += s_prim[i].soma_us; }
    const uint64_t q = d.n;
    printf("\nper frame (average of %lu): draw %lu us = drawing %lu us + Berry script %lu us\n",
           (unsigned long)d.n, (unsigned long)(d.soma_us / q), (unsigned long)(prim_us / q),
           (unsigned long)((d.soma_us > prim_us ? d.soma_us - prim_us : 0) / q));
    printf("%-12s %10s %10s %10s %10s\n", "tela.", "chamadas/q", "us/quadro", "us/chamada", "pior us");
    for (int i = 0; i < P_N; ++i) {
      const tempo_t& t = s_prim[i];
      if (!t.n) { continue; }
      printf("%-12s %10lu %10lu %10lu %10lu\n", P_NOME[i], (unsigned long)(t.n / q),
             (unsigned long)(t.soma_us / q), (unsigned long)(t.soma_us / t.n), (unsigned long)t.max_us);
    }
  }
  if (zerar) { memset(s_tempo, 0, sizeof(s_tempo)); memset(s_prim, 0, sizeof(s_prim)); }
}

void berry_apps_mem(bvm* console)
{
  // Leitura solta do contador de cada maquina (um size_t): pode estar no meio
  // de um trabalho, e serve so para olhar.
  printf("%-16s %-14s %10s %10s\n", "maquina", "estado", "KB berry", "KB camadas");
  for (int i = 0; i < s_n; ++i) {
    const berry_app_t* a = &s_apps[i];
    if (!a->vm) { continue; }
    uint32_t px = 0;
    for (auto* c : a->camadas) { if (c) { px += (uint32_t)c->width() * c->height(); } }
    const bool f = sistema_fundo_ativo(&a->fundo_def);
    printf("%-16s %-14s %10.1f %10.1f\n", a->nome,
           a->janela ? (f ? "janela+fundo" : "janela") : "fundo",
           be_gc_memcount(a->vm) / 1024.0, px * 2 / 1024.0);
  }
  if (console) { printf("%-16s %-14s %10.1f\n", "(console)", "", be_gc_memcount(console) / 1024.0); }
  printf("\nBerry: %.1f KB em uso, pico %.1f KB, orcamento %u KB\n",
         berry_mem_usada() / 1024.0, berry_mem_pico() / 1024.0, (unsigned)(BERRY_ORCAMENTO / 1024));
}

