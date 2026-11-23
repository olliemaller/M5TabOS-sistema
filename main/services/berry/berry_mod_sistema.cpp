// Modulos Berry 'sistema' (tempo, energia, sensores, avisos, bip, fundo),
// 'idioma' e 'transferencia'. Ver berry_apps.cpp (o motor) e berry_interno.hpp.

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "berry_apps.hpp"   // berry.h: o be_gc.h precisa dele antes
extern "C" {
#include "be_gc.h"   // be_gc_memcount (fora da API publica)
}
#include "berry_mem.h"
#include "idioma.h"
#include "transferencia.h"
#include "notificacao.h"
#include "tab5_hora.hpp"
#include "tab5_imu.hpp"
#include "tab5_wifi.hpp"
#include "tab5_gps.hpp"
#include "nmea.h"
#include "berry_interno.hpp"

static const char* TAG = "berry.apps";

// O bip pedido pelo script: o codec fala I2C, que e da interface - toca
// depois que o trabalho volta, ja na tarefa de interface.
float    s_bip_hz = 0;
uint32_t s_bip_ms = 0;

// Manter a tela acesa: tambem aplicado pela interface (-1 = nada pedido).
int s_acordado_pedido = -1;

// ---------------------------------------------------------------------------
// MODULO 'sistema'
// ---------------------------------------------------------------------------
static int s_redesenhar(bvm* vm) { sistema_pedir_redesenho(); be_return_nil(vm); }
static int s_ms(bvm* vm)         { be_pushint(vm, (bint)tab5_agora_ms()); be_return(vm); }
// Microssegundos desde o boot, para MEDIR (so a diferenca vale: o numero da a
// volta a cada ~35 min, e a subtracao continua certa).
static int s_us(bvm* vm)         { be_pushint(vm, (bint)(int32_t)(uint32_t)esp_timer_get_time()); be_return(vm); }
static int s_agora(bvm* vm)      { be_pushint(vm, (bint)tab5_hora_epoch()); be_return(vm); }

static int s_hora(bvm* vm)
{
  const uint32_t e = tab5_hora_epoch();
  if (!e) { be_pushstring(vm, ""); be_return(vm); }
  struct tm t;
  tab5_hora_local(e, &t);
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  be_pushstring(vm, buf);
  be_return(vm);
}

static int s_bateria(bvm* vm) { be_pushint(vm, tab5_power_ultima().pct); be_return(vm); }
// A tela esta apagada (repouso)? Para a parte de fundo escolher o proprio
// ritmo: cada chamada de fundo() acorda o P4 do sono leve.
static int s_repouso(bvm* vm) { be_pushbool(vm, tab5_power_em_repouso()); be_return(vm); }

// recursos() - o que o sistema esta usando agora e por quem (kernel/recursos.h):
// [{"recurso": "camera", "quem": "MeuApp", "s": 12}]. O script ve o proprio
// uso - a camera que ele ligou aparece com o nome dele - e o dos servicos.
static int s_recursos(bvm* vm)
{
  recurso_uso_t u[RECURSOS_MAX];
  const int n = sistema_recursos(u, RECURSOS_MAX);
  const uint32_t agora = tab5_agora_ms();
  lista_nova(vm);
  for (int i = 0; i < n; ++i) {
    mapa_novo(vm);
    m_txt(vm, "recurso", sistema_recurso_nome(u[i].r));
    m_txt(vm, "quem", u[i].quem);
    m_int(vm, "s", (int)((agora - u[i].desde_ms) / 1000));
    fim(vm);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  fim(vm);
  be_return(vm);
}
static int s_wifi(bvm* vm)    { be_pushstring(vm, tab5_wifi_descricao()); be_return(vm); }

// [ax, ay, az] em g, ou nil sem leitura. Chamar marca o QUADRO: a interface le
// o sensor antes do proximo desenhar() (o cache do sistema so anda 1 vez por
// segundo - lento para uma bolha de nivel). Quadro que nao chamou desmarca:
// a aba sem IMU nao paga o I2C a cada quadro.
static int imu_tres(bvm* vm, bool giro)
{
  if (C->app) { C->app->usa_imu = true; }   // marca o quadro (ver acima)
  // O giroscopio so fica ligado com quem le (hal/tab5_imu): o pedido do
  // Berry e por prazo - cada chamada renova, 5 s sem chamar ele cai.
  if (giro) { tab5_imu_giroscopio_berry(); }
  tab5_imu_dados_t d;
  if (!tab5_imu_ultima(&d) || !d.valida) { be_return_nil(vm); }
  lista_nova(vm);
  if (giro) { l_real(vm, d.gx); l_real(vm, d.gy); l_real(vm, d.gz); }
  else      { l_real(vm, d.ax); l_real(vm, d.ay); l_real(vm, d.az); }
  fim(vm);
  be_return(vm);
}
static int s_imu(bvm* vm)  { return imu_tres(vm, false); }
static int s_giro(bvm* vm) { return imu_tres(vm, true); }   // [gx, gy, gz] em graus/s

// hora_de(epoch) - "HH:MM" no fuso local (a hora de uma mensagem), "" se 0.
static int s_hora_de(bvm* vm)
{
  const uint32_t e = (uint32_t)arg_int(vm, 1, 0);
  if (!e) { be_pushstring(vm, ""); be_return(vm); }
  struct tm t;
  tab5_hora_local(e, &t);
  char buf[8];
  tab5_hora_hhmm(&t, buf, sizeof(buf));
  be_pushstring(vm, buf);
  be_return(vm);
}

// "dd/mm/aaaa" no fuso local, ou "" sem relogio acertado.
static int s_data(bvm* vm)
{
  const uint32_t e = tab5_hora_epoch();
  if (!e) { be_pushstring(vm, ""); be_return(vm); }
  struct tm t;
  tab5_hora_local(e, &t);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  be_pushstring(vm, buf);
  be_return(vm);
}

// {pct, volts, ma}: ma > 0 carregando. pct -1 = sem estimativa.
static int s_energia(bvm* vm)
{
  const tab5_medida_t m = tab5_power_ultima();
  mapa_novo(vm);
  m_int(vm, "pct", m.pct);
  m_real(vm, "volts", m.volts);
  m_real(vm, "ma", m.mA);
  fim(vm);
  be_return(vm);
}

// {ssid, ip, rssi, texto}: ip "" = sem conexao.
static int s_rede(bvm* vm)
{
  const char* ip = tab5_wifi_ip();
  const bool on = ip && ip[0];
  mapa_novo(vm);
  m_txt(vm, "ssid", on ? tab5_wifi_ssid() : "");
  m_txt(vm, "ip", on ? ip : "");
  m_int(vm, "rssi", on ? (int)tab5_wifi_rssi() : 0);
  m_txt(vm, "texto", tab5_wifi_descricao());
  fim(vm);
  be_return(vm);
}

// nil sem GPS no Grove; {fix, lat, lon, alt, sats}. Sem fix, lat/lon = 0.
static int s_gps(bvm* vm)
{
  if (!tab5_gps_no_ar()) { be_return_nil(vm); }
  const nmea_posicao_t p = tab5_gps_posicao();
  mapa_novo(vm);
  m_bool(vm, "fix", p.qualidade != 0);
  m_real(vm, "lat", p.lat);
  m_real(vm, "lon", p.lon);
  m_real(vm, "alt", p.altitude_m);
  m_int(vm, "sats", p.sats_usados);
  fim(vm);
  be_return(vm);
}

// acordado(true): a tela nao apaga enquanto o app estiver aberto.
static int s_acordado(bvm* vm)
{
  if (!so_janela(vm, "sistema.acordado")) { be_return_nil(vm); }
  s_acordado_pedido = (be_top(vm) < 1 || be_tobool(vm, 1)) ? 1 : 0;
  be_return_nil(vm);
}

// periodo(ms): troca o '# periodo:' com o app aberto (0 = so quando pedir).
// periodo() devolve o de agora.
static int s_periodo(bvm* vm)
{
  if (!C->app || !so_janela(vm, "sistema.periodo")) { be_return_nil(vm); }
  if (be_top(vm) >= 1 && be_isnumber(vm, 1)) {
    const int v = (int)be_toint(vm, 1);
    C->app->periodo = v < 0 ? 0 : (uint32_t)v;
    sistema_pedir_redesenho();   // o ritmo novo vale a partir do proximo quadro
  }
  be_pushint(vm, (bint)C->app->periodo);
  be_return(vm);
}

// acumular(true): o quadro deixa de chegar limpo. O desenhar(tudo) recebe
// tudo = true quando a area FOI limpa (abriu, girou, teclado, voltou do
// bloqueio) - desenhe tudo; e tudo = false no resto - desenhe SO o que mudou
// (o que ja estava na tela continua la). Custa so o que muda: e o jeito de
// animar uma coisa pequena numa tela cheia, ou de pintar com o dedo.
static int s_acumular(bvm* vm)
{
  if (!C->app || !so_janela(vm, "sistema.acumular")) { be_return_nil(vm); }
  if (be_top(vm) >= 1) {
    const bool v = be_tobool(vm, 1);
    if (v != C->app->acumular) {
      C->app->acumular = v;
      sistema_pedir_redesenho_completo();   // comeca de uma area limpa (tudo = true)
    }
  }
  be_pushbool(vm, C->app->acumular);
  be_return(vm);
}

// dt(): segundos entre este quadro e o anterior NA TELA - para animar por
// tempo (posicao += velocidade * dt) e nao por quadro.
static int s_dt(bvm* vm) { be_pushreal(vm, (breal)sistema_quadro_dt()); be_return(vm); }

// A PARTE DE FUNDO. fundo(true [, ms]): com a janela fechada o sistema segue
// chamando fundo() do script a cada 'ms' (padrao 1000) - a mesma maquina, as
// mesmas variaveis. fundo(false) para (de dentro do fundo() tambem). fundo()
// diz se esta rodando. Ligar e parar sao aplicados pela interface depois que o
// trabalho volta (o kernel liga o fundo na tarefa de quem pede).

static int s_fundo(bvm* vm)
{
  berry_app_t* a = C->app;
  if (!a) { be_return_nil(vm); }
  if (be_top(vm) >= 1) {
    const bool ligar = be_tobool(vm, 1);
    if (C->fundo) {
      if (!ligar) { a->fundo_sair = true; }   // o laco sai depois desta volta
    } else {
      a->fundo_pedido = ligar ? FP_LIGAR : FP_DESLIGAR;
      if (ligar) {
        const int ms = arg_int(vm, 2, 1000);
        a->fundo_ms = ms < 100 ? 100 : (uint32_t)ms;   // 100 ms: mais que isso e a tarefa do Berry ocupada sem parar
      }
    }
  }
  be_pushbool(vm, a->fundo_pedido == FP_LIGAR ||
                  (a->fundo_pedido != FP_DESLIGAR && sistema_fundo_ativo(&a->fundo_def) && !a->fundo_sair));
  be_return(vm);
}

// fundo_estado(texto [, n]): o que a pastilha da barra diz enquanto o fundo roda.
static int s_fundo_estado(bvm* vm)
{
  berry_app_t* a = C->app;
  if (!a) { be_return_nil(vm); }
  snprintf(a->fundo_txt, sizeof(a->fundo_txt), "%s", arg_txt(vm, 1));
  a->fundo_n = arg_int(vm, 2, 0);
  be_return_nil(vm);
}

// memoria(): {app, berry, orcamento} em bytes - o que ESTA maquina usa, o que
// todas as maquinas Berry somam, e o teto delas (ver berry_mem.h).
static int s_memoria(bvm* vm)
{
  mapa_novo(vm);
  m_int(vm, "app", (int)be_gc_memcount(vm));
  m_int(vm, "berry", (int)berry_mem_usada());
  m_int(vm, "orcamento", (int)BERRY_ORCAMENTO);
  fim(vm);
  be_return(vm);
}

static int s_bip(bvm* vm)
{
  if (!so_janela(vm, "sistema.bip")) { be_return_nil(vm); }
  s_bip_hz = (float)arg_int(vm, 1, 880);
  s_bip_ms = (uint32_t)arg_int(vm, 2, 100);
  be_return_nil(vm);
}

// O nivel do aviso (sistema.avisar, 2o argumento): "info", "atencao" ou
// "urgente". Texto desconhecido (ou sem o argumento) = atencao, o de sempre.
static notif_nivel_t aviso_nivel(bvm* vm)
{
  const char* n = arg_txt(vm, 2);
  if (!strcmp(n, "info"))    { return NOTIF_INFO; }
  if (!strcmp(n, "urgente")) { return NOTIF_URGENTE; }
  return NOTIF_ATENCAO;
}

// avisar("texto" [, nivel]) poe um aviso na barra; avisar() tira. nivel:
// "info" (nada exige a pessoa), "atencao" (o padrao) ou "urgente" (exige acao).
static int s_avisar(bvm* vm)
{
  if (!C->app) { be_return_nil(vm); }
  if (C->app->notif < 0) {
    C->app->notif = notif_registrar(C->app->nome);   // o nome sobrevive: e estatico
    notif_associar_app(C->app->notif, C->app->nome);
  }
  const char* t = arg_txt(vm, 1);
  if (t[0]) { notif_publicar(C->app->notif, 1, aviso_nivel(vm), t); }
  else      { notif_limpar(C->app->notif); }
  be_return_nil(vm);
}

static int s_log(bvm* vm)
{
  ESP_LOGI(TAG, "%s: %s", C->app ? C->app->nome : "console", arg_txt(vm, 1));
  be_return_nil(vm);
}

// ---------------------------------------------------------------------------
// IDIOMA - o da interface, escolhido em Ajustes (services/idioma.h). O app so
// le: a escolha e de quem segura o aparelho.
// ---------------------------------------------------------------------------
// atual() - "pt" ou "en".
static int i_atual(bvm* vm)
{
  be_pushstring(vm, idioma_codigo(idioma_atual()));
  be_return(vm);
}

// nome() - o idioma atual nele mesmo: "Português", "English".
static int i_nome(bvm* vm)
{
  be_pushstring(vm, idioma_nome(idioma_atual()));
  be_return(vm);
}

// tr(texto) - a frase do SISTEMA no idioma atual ("Cancelar" -> "Cancel");
// sem traducao, volta a mesma.
static int i_tr(bvm* vm)
{
  be_pushstring(vm, tr(arg_txt(vm, 1)));
  be_return(vm);
}

// escolher(pt, en) - as frases do proprio app: devolve a do idioma atual
// (sem a inglesa, a portuguesa).
static int i_escolher(bvm* vm)
{
  const bool en = idioma_atual() != 0 && be_top(vm) >= 2 && be_isstring(vm, 2);   // fora do portugues, a do app em ingles
  be_pushstring(vm, arg_txt(vm, en ? 2 : 1));
  be_return(vm);
}

// ---------------------------------------------------------------------------
// TRANSFERENCIA - a area de transferencia do sistema (services/transferencia.h):
// o mesmo texto que o toque longo copia no Mesh, no ESP-NOW e nas Notas.
// ---------------------------------------------------------------------------
// copiar(texto) - true se guardou (ate 4 KB; o resto e cortado).
static int tr_copiar(bvm* vm)
{
  be_pushbool(vm, transf_copiar(arg_txt(vm, 1)));
  be_return(vm);
}

// colar() - o texto copiado ("" = vazia).
static int tr_colar(bvm* vm)
{
  char* t = (char*)heap_caps_malloc(TRANSF_MAX, MALLOC_CAP_SPIRAM);
  if (t) { transf_colar(t, TRANSF_MAX); }
  be_pushstring(vm, t ? t : "");
  heap_caps_free(t);
  be_return(vm);
}

const membro_t BERRY_MOD_SISTEMA[] = {
  { "redesenhar", s_redesenhar }, { "ms", s_ms }, { "us", s_us }, { "agora", s_agora }, { "hora", s_hora },
  { "bateria", s_bateria }, { "wifi", s_wifi }, { "imu", s_imu }, { "bip", s_bip },
  { "avisar", s_avisar }, { "log", s_log }, { "giro", s_giro }, { "data", s_data },
  { "energia", s_energia }, { "rede", s_rede }, { "gps", s_gps }, { "acordado", s_acordado },
  { "periodo", s_periodo }, { "dt", s_dt }, { "acumular", s_acumular }, { "hora_de", s_hora_de },
  { "fundo", s_fundo }, { "fundo_estado", s_fundo_estado }, { "memoria", s_memoria },
  { "repouso", s_repouso },
  { "recursos", s_recursos },
  { nullptr },
};

const membro_t BERRY_MOD_IDIOMA[] = {
  { "atual", i_atual }, { "nome", i_nome }, { "tr", i_tr }, { "escolher", i_escolher },
  { nullptr },
};

const membro_t BERRY_MOD_TRANSFERENCIA[] = {
  { "copiar", tr_copiar }, { "colar", tr_colar }, { nullptr },
};
