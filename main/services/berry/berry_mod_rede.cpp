// Modulo Berry 'rede' - a internet para o script (services/tab5_rede.hpp).
// Ver berry_apps.cpp (o motor) e berry_interno.hpp.
//
// O SCRIPT NUNCA ESPERA A REDE. O script roda com prazo curto (PRAZO_EVENTO_MS,
// 2 s) e desenhar() trava a interface: um GET que demora segundos no meio
// deles derrubaria o script e congelaria a tela. Por isso e em DOIS tempos:
// rede.pedir(url) so ENTREGA o pedido a uma tarefa propria e volta na
// hora; o app olha rede.estado() no desenhar() e, com "pronto", le
// rede.resposta(). O trabalho redesenha o app quando termina.
//
// UM PEDIDO POR VEZ NO SISTEMA INTEIRO (a tarefa e uma so), e a resposta e do
// app que pediu: nenhum outro a ve.
//
// POR QUE UMA TAREFA PROPRIA ("rede_berry"), e nao o worker do sistema: o TLS
// precisa de ~12 KB de pilha (a capa da Musica usa o mesmo numero) e o worker
// tem 6 KB, compartilhados com todo mundo - e um download lento o prenderia. A
// pilha fica na PSRAM (nao gasta RAM interna) e a tarefa e criada no primeiro
// pedido e MANTIDA viva, dormindo, para nao recriar pilha a cada pedido. O servico tab5_rede ja
// declara a rede e o processamento como em uso durante a transferencia
// (kernel/recursos.h), entao o repouso nao entra no meio do download.
//
// O DONO SAIU: rede_soltar() (chamada pelo motor quando a maquina do app e
// apagada) marca "cancelar". Se o trabalho ja terminou, o buffer e liberado
// ali mesmo; se ainda roda, o proprio trabalho larga o que baixou ao ver que
// o dono saiu - o buffer nunca fica orfao.

#include <stdio.h>
#include "idioma.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps - a pilha na PSRAM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tab5_rede.hpp"
#include "berry_interno.hpp"

static const size_t CORPO_PADRAO = 64 * 1024;    // o max_bytes de rede.pedir quando o script nao diz
static const size_t CORPO_TETO   = 256 * 1024;   // e o maximo que ele pode pedir (a resposta vira texto do Berry)
#define URL_MAX 256
#define PILHA_REDE 12288   // TLS + HTTP

static const char* TAG = "berry.rede";

enum { R_PARADO, R_BAIXANDO, R_PRONTO, R_ERRO };

typedef struct {
  const berry_app_t* dono;   // quem pediu (nullptr = ninguem)
  bool  rodando;             // o trabalho solto esta vivo
  bool  cancelar;            // o dono desistiu ou saiu: o trabalho descarta o que vier
  bool  tem_resposta;        // terminou: buf e info esperam o dono
  char* buf;                 // o corpo, com '\0' (nullptr = falhou: info.erro diz por que)
  tab5_rede_http_t info;
  size_t max;                // o limite do corpo deste pedido
  char   url[URL_MAX];
} pedido_rede_t;

static pedido_rede_t s_p;
static portMUX_TYPE  s_trava = portMUX_INITIALIZER_UNLOCKED;   // so campos: nada pesado dentro dela
static TaskHandle_t  s_tarefa = nullptr;   // criada no primeiro rede.pedir e mantida viva

// O estado que 'app' enxerga. Chamar com a trava. Cancelado ja e "parado"
// (o trabalho ainda esta acabando, mas o script nao tem mais o que esperar).
static int estado_de(const berry_app_t* app)
{
  if (!app || s_p.dono != app || s_p.cancelar) { return R_PARADO; }
  if (s_p.rodando)      { return R_BAIXANDO; }
  if (s_p.tem_resposta) { return s_p.buf ? R_PRONTO : R_ERRO; }
  return R_PARADO;
}

// O 'parar' do tab5_rede_http: o laco de leitura pergunta a cada pedaco.
static bool cancelado(void)
{
  portENTER_CRITICAL(&s_trava);
  const bool c = s_p.cancelar;
  portEXIT_CRITICAL(&s_trava);
  return c;
}

// A tarefa: dorme ate rede.pedir acorda-la, baixa, guarda o resultado e volta
// a dormir. url e max foram gravados por r_pedir antes de acordar, e ninguem
// mexe neles enquanto 'rodando'.
static void tarefa_rede(void*)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    tab5_rede_http_t info = {};
    char* buf = tab5_rede_http_texto(s_p.url, s_p.max, &info, cancelado);
    if (!buf && !info.erro[0]) { snprintf(info.erro, sizeof(info.erro), tr("sem memoria")); }

    char* descartar = nullptr;
    portENTER_CRITICAL(&s_trava);
    s_p.rodando = false;
    if (s_p.cancelar) {   // ninguem espera: o que baixou nao tem dono
      descartar = buf;
      s_p.cancelar = false;
      s_p.dono = nullptr;
      s_p.tem_resposta = false;
    } else {
      s_p.buf = buf;
      s_p.info = info;
      s_p.tem_resposta = true;
    }
    portEXIT_CRITICAL(&s_trava);
    free(descartar);
    sistema_pedir_redesenho();   // o app ve o "pronto" no proximo desenhar()
  }
}

// O app fechou (ou cancelou): ver o comentario do topo.
void rede_soltar(const berry_app_t* app)
{
  char* liberar = nullptr;
  portENTER_CRITICAL(&s_trava);
  if (app && s_p.dono == app) {
    if (s_p.rodando) {
      s_p.cancelar = true;
    } else {
      liberar = s_p.buf;
      s_p.buf = nullptr;
      s_p.tem_resposta = false;
      s_p.dono = nullptr;
    }
  }
  portEXIT_CRITICAL(&s_trava);
  free(liberar);
}

// ip() - {conectado, ip, mascara, gateway, dns1, dns2, ipv6, mac, nome} do
// aparelho na rede, ou nil sem interface de Wi-Fi. Sem IP: conectado false.
static int r_ip(bvm* vm)
{
  tab5_rede_ip_t r;
  if (!tab5_rede_ip(&r)) { be_return_nil(vm); }
  char mac[18];
  mac_txt(r.mac, mac);
  mapa_novo(vm);
  m_bool(vm, "conectado", r.conectado);
  m_txt(vm, "ip", r.ip);
  m_txt(vm, "mascara", r.mascara);
  m_txt(vm, "gateway", r.gateway);
  m_txt(vm, "dns1", r.dns[0]);
  m_txt(vm, "dns2", r.dns[1]);
  m_txt(vm, "ipv6", r.ipv6);
  m_txt(vm, "mac", mac);
  m_txt(vm, "nome", r.nome);
  fim(vm);
  be_return(vm);
}

// pedir(url [, max_bytes]) - comeca um GET (http:// ou https://, ate 255
// letras; segue redirecionamentos). O corpo e guardado ate max_bytes (padrao
// 64 KB, no maximo 256 KB; o que passar e cortado). true = comecou; false =
// ja ha um pedido em curso no sistema (de qualquer app), URL invalida ou a
// tarefa nao subiu. Com uma resposta que ninguem leu: se for deste app ela e
// descartada e o pedido novo comeca; se for de OUTRO app, recusa (false).
static int r_pedir(bvm* vm)
{
  const berry_app_t* app = C->app;
  const char* url = arg_txt(vm, 1);
  int max = arg_int(vm, 2, (int)CORPO_PADRAO);
  if (max < 1) { max = (int)CORPO_PADRAO; }
  if ((size_t)max > CORPO_TETO) { max = (int)CORPO_TETO; }
  bool ok = app && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) && strlen(url) < URL_MAX;
  if (ok) {
    char* velho = nullptr;
    portENTER_CRITICAL(&s_trava);
    if (s_p.rodando || (s_p.tem_resposta && s_p.dono != app)) {
      ok = false;
    } else {
      velho = s_p.buf;
      s_p.buf = nullptr;
      s_p.dono = app;
      s_p.cancelar = false;
      s_p.tem_resposta = false;
      s_p.max = (size_t)max;
      snprintf(s_p.url, sizeof(s_p.url), "%s", url);
      s_p.rodando = true;
    }
    portEXIT_CRITICAL(&s_trava);
    free(velho);
    if (ok && !s_tarefa &&
        xTaskCreatePinnedToCoreWithCaps(tarefa_rede, "rede_berry", PILHA_REDE, nullptr, 1, &s_tarefa,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
      ESP_LOGW(TAG, "a tarefa da rede nao subiu");
      s_tarefa = nullptr;
      portENTER_CRITICAL(&s_trava);   // desfaz o que este pedido gravou
      s_p.rodando = false;
      s_p.dono = nullptr;
      portEXIT_CRITICAL(&s_trava);
      ok = false;
    }
    if (ok) { xTaskNotifyGive(s_tarefa); }
  }
  be_pushbool(vm, ok);
  be_return(vm);
}

// estado() - "parado" (nada pedido, ou ja lido/cancelado), "baixando",
// "pronto" (rede.resposta() tem o resultado) ou "erro" (resposta() tem o motivo).
static int r_estado(bvm* vm)
{
  static const char* const NOME[] = { "parado", "baixando", "pronto", "erro" };
  portENTER_CRITICAL(&s_trava);
  const int e = estado_de(C->app);
  portEXIT_CRITICAL(&s_trava);
  be_pushstring(vm, NOME[e]);
  be_return(vm);
}

// resposta() - {status, texto, tipo, final, ms, erro} quando o estado e
// "pronto" ou "erro", e LIBERA o resultado (o estado volta a "parado"); nil
// se nao ha resposta deste app. status: o HTTP (200, 404...; 0 = nao
// conectou, e 'erro' diz por que); texto: o corpo; tipo: o Content-Type;
// final: a URL depois dos redirecionamentos; ms: quanto levou.
static int r_resposta(bvm* vm)
{
  portENTER_CRITICAL(&s_trava);
  const int e = estado_de(C->app);
  const tab5_rede_http_t info = s_p.info;
  const char* corpo = s_p.buf;
  portEXIT_CRITICAL(&s_trava);
  if (e != R_PRONTO && e != R_ERRO) { be_return_nil(vm); }

  // O resultado so e tirado do estado DEPOIS de o mapa pronto: se montar o
  // texto estourar a memoria do Berry (erro no script), o buffer segue com o
  // estado e e liberado ao fechar o app - nunca fica orfao.
  mapa_novo(vm);
  m_int(vm, "status", info.status);
  m_txt(vm, "texto", corpo);
  m_txt(vm, "tipo", info.tipo);
  m_txt(vm, "final", info.final);
  m_int(vm, "ms", (int)info.ms);
  m_txt(vm, "erro", info.erro);
  fim(vm);

  portENTER_CRITICAL(&s_trava);
  char* liberar = s_p.buf;
  s_p.buf = nullptr;
  s_p.tem_resposta = false;
  s_p.dono = nullptr;
  portEXIT_CRITICAL(&s_trava);
  free(liberar);
  be_return(vm);
}

// cancelar() - desiste do pedido em curso (ou joga fora a resposta que
// ninguem leu). So o do proprio app.
static int r_cancelar(bvm* vm)
{
  rede_soltar(C->app);
  be_return_nil(vm);
}

// codificar(texto) - o texto para usar dentro de uma URL ("a b&c" -> "a+b%26c").
// O buffer cabe o maximo de uma URL (URL_MAX letras): o que passar disso nao
// serviria para rede.pedir de qualquer jeito.
static int r_codificar(bvm* vm)
{
  char out[URL_MAX * 3];
  tab5_rede_url_codificar(arg_txt(vm, 1), out, sizeof(out));
  be_pushstring(vm, out);
  be_return(vm);
}

const membro_t BERRY_MOD_REDE[] = {
  { "ip", r_ip }, { "pedir", r_pedir }, { "estado", r_estado }, { "resposta", r_resposta },
  { "cancelar", r_cancelar }, { "codificar", r_codificar },
  { nullptr },
};
