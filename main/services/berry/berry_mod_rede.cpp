// Berry module 'rede' - the internet for the script (services/tab5_rede.hpp).
// See berry_apps.cpp (the engine) and berry_interno.hpp.
//
// THE SCRIPT NEVER WAITS FOR THE NETWORK. The script runs with a short deadline (PRAZO_EVENTO_MS,
// 2 s) and desenhar() blocks the UI: a GET that takes seconds in the middle of
// them would bring down the script and freeze the screen. That is why it works in TWO steps:
// rede.pedir(url) only HANDS the request to a dedicated task and returns
// straight away; the app checks rede.estado() in desenhar() and, on "pronto", reads
// rede.resposta(). The job redraws the app when it finishes.
//
// ONE REQUEST AT A TIME ACROSS THE WHOLE SYSTEM (there is only one task), and the reply belongs to the
// app that asked: no other sees it.
//
// WHY A DEDICATED TASK ("rede_berry"), and not the system worker: TLS
// needs ~12 KB of stack (Music's cover art uses the same number) and the worker
// has 6 KB, shared with everyone - and a slow download would hold it up. The
// stack lives in PSRAM (it does not use internal RAM) and the task is created on the first
// request and KEPT alive, sleeping, so as not to recreate the stack on every request. The tab5_rede service already
// declares the network and processing as in use during the transfer
// (kernel/recursos.h), so idle does not kick in mid-download.
//
// THE OWNER LEFT: rede_soltar() (called by the engine when the app's machine is
// deleted) marks "cancel". If the job has already finished, the buffer is freed
// right there; if it is still running, the job itself drops what it downloaded when it sees that
// the owner has left - the buffer is never orphaned.

#include <stdio.h>
#include "idioma.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps - the stack in PSRAM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tab5_rede.hpp"
#include "berry_interno.hpp"

static const size_t CORPO_PADRAO = 64 * 1024;    // rede.pedir's max_bytes when the script does not say
static const size_t CORPO_TETO   = 256 * 1024;   // and the most it may ask for (the reply becomes Berry text)
#define URL_MAX 256
#define PILHA_REDE 12288   // TLS + HTTP

static const char* TAG = "berry.rede";

enum { R_PARADO, R_BAIXANDO, R_PRONTO, R_ERRO };

typedef struct {
  const berry_app_t* dono;   // who asked (nullptr = nobody)
  bool  rodando;             // the detached job is alive
  bool  cancelar;            // the owner gave up or left: the job discards whatever comes
  bool  tem_resposta;        // finished: buf and info wait for the owner
  char* buf;                 // the body, with '\0' (nullptr = failed: info.erro says why)
  tab5_rede_http_t info;
  size_t max;                // this request's body limit
  char   url[URL_MAX];
} pedido_rede_t;

static pedido_rede_t s_p;
static portMUX_TYPE  s_trava = portMUX_INITIALIZER_UNLOCKED;   // fields only: nothing heavy inside it
static TaskHandle_t  s_tarefa = nullptr;   // created on the first rede.pedir and kept alive

// The state 'app' sees. Call with the lock held. Cancelled is already "parado" (stopped)
// (the job is still finishing, but the script has nothing more to wait for).
static int estado_de(const berry_app_t* app)
{
  if (!app || s_p.dono != app || s_p.cancelar) { return R_PARADO; }
  if (s_p.rodando)      { return R_BAIXANDO; }
  if (s_p.tem_resposta) { return s_p.buf ? R_PRONTO : R_ERRO; }
  return R_PARADO;
}

// tab5_rede_http's 'parar': the read loop asks on every chunk.
static bool cancelado(void)
{
  portENTER_CRITICAL(&s_trava);
  const bool c = s_p.cancelar;
  portEXIT_CRITICAL(&s_trava);
  return c;
}

// The task: sleeps until rede.pedir wakes it, downloads, stores the result and goes back
// to sleep. url and max were written by r_pedir before waking it, and nobody
// touches them while 'rodando'.
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
    if (s_p.cancelar) {   // nobody is waiting: what was downloaded has no owner
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
    sistema_pedir_redesenho();   // the app sees "pronto" on the next desenhar()
  }
}

// The app closed (or cancelled): see the comment at the top.
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

// ip() - {conectado, ip, mascara, gateway, dns1, dns2, ipv6, mac, nome} of the
// device on the network, or nil with no Wi-Fi interface. Without an IP: conectado false.
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

// pedir(url [, max_bytes]) - starts a GET (http:// or https://, up to 255
// characters; follows redirects). The body is kept up to max_bytes (default
// 64 KB, at most 256 KB; anything beyond is cut). true = started; false =
// there is already a request in progress in the system (from any app), invalid URL or the
// task did not come up. With a reply nobody has read: if it is this app's it is
// discarded and the new request starts; if it belongs to ANOTHER app, it refuses (false).
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
      ESP_LOGW(TAG, "the network task did not come up");
      s_tarefa = nullptr;
      portENTER_CRITICAL(&s_trava);   // undo what this request wrote
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

// estado() - "parado" (nothing requested, or already read/cancelled), "baixando" (downloading),
// "pronto" (rede.resposta() has the result) or "erro" (resposta() has the reason).
static int r_estado(bvm* vm)
{
  static const char* const NOME[] = { "parado", "baixando", "pronto", "erro" };
  portENTER_CRITICAL(&s_trava);
  const int e = estado_de(C->app);
  portEXIT_CRITICAL(&s_trava);
  be_pushstring(vm, NOME[e]);
  be_return(vm);
}

// resposta() - {status, texto, tipo, final, ms, erro} when the state is
// "pronto" or "erro", and RELEASES the result (the state goes back to "parado"); nil
// if there is no reply for this app. status: the HTTP one (200, 404...; 0 = did not
// connect, and 'erro' says why); texto: the body; tipo: the Content-Type;
// final: the URL after redirects; ms: how long it took.
static int r_resposta(bvm* vm)
{
  portENTER_CRITICAL(&s_trava);
  const int e = estado_de(C->app);
  const tab5_rede_http_t info = s_p.info;
  const char* corpo = s_p.buf;
  portEXIT_CRITICAL(&s_trava);
  if (e != R_PRONTO && e != R_ERRO) { be_return_nil(vm); }

  // The result is only taken out of the state AFTER the map is ready: if building the
  // text runs Berry out of memory (an error in the script), the buffer stays with the
  // state and is freed when the app closes - it is never orphaned.
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

// cancelar() - gives up on the request in progress (or throws away the reply
// nobody read). Only the app's own.
static int r_cancelar(bvm* vm)
{
  rede_soltar(C->app);
  be_return_nil(vm);
}

// codificar(texto) - the text for use inside a URL ("a b&c" -> "a+b%26c").
// The buffer holds the maximum of a URL (URL_MAX characters): anything beyond that would not
// work for rede.pedir anyway.
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
