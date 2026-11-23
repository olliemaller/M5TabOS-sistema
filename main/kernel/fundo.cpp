#include "fundo.h"
#include "app.h"
#include "idioma.h"
#include "tab5_donos.hpp"
#include "pool_tarefas.h"
#include "parada.hpp"      // request/confirm the exit - the same as GPS, Mesh and Mesh BLE
#include "internal.hpp"    // sistema_na_interface

#include <string.h>
#include <stdio.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "fundo";

#define FUNDO_PRIORIDADE  1      // below the UI (4), radios (3) and worker (2)
#define FUNDO_PARAR_MS    3000
#define FUNDO_CEDER_US    20000  // fundo_ceder() only actually yields every 20 ms

// THE STOP IS THE KERNEL'S (kernel/parada.hpp), not a state machine of
// its own. There used to be one (LIVRE, RODANDO, SAIU, ABANDONADA, LIXO) and whoever
// stopped it WAITED BY POLLING the state every 10 ms, up to 3 s, on the
// UI task - the same pattern parada.hpp retired in the services. Now:
//   - the task GIVES the 'saiu' semaphore as its last act (parada_confirmar) and
//     DOES NOT TOUCH THE SLOT AGAIN after that;
//   - whoever stops it WAITS ON IT: wakes the instant it exits;
//   - past the deadline, the slot is left 'abandonada' with the request standing, and
//     fundo_manter() cleans up when the semaphore is finally given;
//   - a loop that ends on its own is seen the same way (semaphore given without
//     anyone having asked).
// The 'saiu' and 'acordar' semaphores are created once per slot and never
// deleted: the trampoline may give 'saiu' after the UI has given up.
struct fundo_s {
  const fundo_def_t*  def;
  volatile bool       em_uso;         // from start until cleanup (finalizar)
  volatile bool       parar_pedido;   // someone outside the UI asked (console)
  bool                parando;        // the UI is inside sistema_fundo_parar()
  bool                abandonada;     // did not exit within the deadline: clean up when it exits
  parada_t            parada;         // stop request and exit confirmation
  SemaphoreHandle_t   acordar;        // takes the loop out of fundo_esperar(); created once, never deleted
  int                 notif;
  bool                visivel;        // the pill has already been published (see aviso_ms)
  int64_t             desde_us;
  int64_t             cedeu_us;
  int64_t             pediu_us;       // stop requested WITHOUT WAITING (0 = no): the deadline counts from here
  char                resumo[NOTIF_RESUMO];
};

static fundo_s          s_f[FUNDO_MAX];
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> s_perfil_sujo{false};
// Requests from outside the UI (sistema_fundo_pedir), until the slot frees up.
static std::atomic<const fundo_def_t*> s_pedidos[FUNDO_MAX];

static void trampolim(void* arg)
{
  fundo_s* f = (fundo_s*)arg;
  f->def->corpo(f);
  // The LAST time this task touches the slot: from here on the UI
  // may clean it (the semaphore is not deleted, only the slot is zeroed).
  parada_confirmar(&f->parada);
  sistema_acordar_interface();   // fundo_manter() cleans up whoever exited on their own
  // Returns: the pool slot gives back the owner and goes back to sleep. Never vTaskDelete.
}

// Only on the UI (the only one that changes 'def').
static fundo_s* achar(const fundo_def_t* d)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    if (s_f[i].em_uso && s_f[i].def == d) { return &s_f[i]; }
  }
  return nullptr;
}

// Returns the slot to free while preserving the two semaphores, and drains 'acordar':
// a notice left over from the stop must not make the next loop skip its
// first wait. (The 'saiu' one is drained by the next one's parada_preparar.)
static void zerar_vaga(fundo_s* f)
{
  SemaphoreHandle_t acordar = f->acordar;
  SemaphoreHandle_t saiu    = f->parada.saiu;
  taskENTER_CRITICAL(&s_mux);
  memset(f, 0, sizeof(*f));   // em_uso = false
  f->acordar = acordar;
  f->parada.saiu = saiu;
  taskEXIT_CRITICAL(&s_mux);
  if (acordar) { xSemaphoreTake(acordar, 0); }
}

// The cleanup common to every ending: stop requested, loop that ended on its own, or
// abandoned one that finally exited. Always on the UI.
static void finalizar(fundo_s* f)
{
  if (f->def->ao_parar) { f->def->ao_parar(); }
  notif_limpar(f->notif);
  ESP_LOGI(TAG, "'%s' stopped (ran %lld s)", f->def->nome,
           (esp_timer_get_time() - f->desde_us) / 1000000);
  zerar_vaga(f);
  s_perfil_sujo = true;
}

// Has the slot's task already exited (given the semaphore)? Does not consume the semaphore.
static bool saiu(const fundo_s* f) { return !parada_rodando(&f->parada); }

bool sistema_fundo_iniciar(const fundo_def_t* d)
{
  if (!d || !d->corpo || !d->nome) { return false; }
  if (achar(d)) { return false; }   // already running (or still exiting)

  fundo_s* f = nullptr;
  for (int i = 0; i < FUNDO_MAX && !f; ++i) {
    if (!s_f[i].em_uso) { f = &s_f[i]; }
  }
  if (!f) {
    ESP_LOGW(TAG, "no slot for '%s' (limit %d)", d->nome, FUNDO_MAX);
    return false;
  }

  if (d->ao_iniciar && !d->ao_iniciar()) {
    ESP_LOGW(TAG, "'%s' refused to start", d->nome);
    return false;
  }

  if (!f->acordar) { f->acordar = xSemaphoreCreateBinary(); }   // once per slot, forever
  zerar_vaga(f);
  f->def      = d;
  f->desde_us = esp_timer_get_time();
  f->cedeu_us = f->desde_us;
  snprintf(f->resumo, sizeof(f->resumo), tr("rodando"));

  // VISIBLE FROM THE FIRST INSTANT - the grey pill is the promise that
  // "nothing runs hidden". The registration is idempotent by name.
  f->notif = notif_registrar(d->nome);
  notif_silenciar(f->notif, d->com_som ? !d->com_som() : false);
  f->visivel = d->aviso_ms == 0;
  if (f->visivel) { notif_publicar(f->notif, 1, NOTIF_ESTADO, f->resumo); }

  const bool pronta = f->acordar && parada_preparar(&f->parada);
  taskENTER_CRITICAL(&s_mux);
  f->em_uso = true;   // last and under the lock: whoever lists sees the slot already complete
  taskEXIT_CRITICAL(&s_mux);
  if (!pronta
   || !pool_criar(nullptr, trampolim, f, d->nome, d->pilha ? d->pilha : 4096,
                  d->prioridade ? d->prioridade : FUNDO_PRIORIDADE, DONO_FUNDO, d->nome)) {   // owner: see hal/tab5_donos.hpp
    ESP_LOGE(TAG, "no memory for the task of '%s'", d->nome);
    parada_desistir(&f->parada);
    finalizar(f);
    return false;
  }
  s_perfil_sujo = true;
  sistema_acordar_interface();
  ESP_LOGI(TAG, "'%s' running in the background", d->nome);
  return true;
}

bool sistema_fundo_parar(const fundo_def_t* d)
{
  fundo_s* f = achar(d);
  if (!f || f->parando || f->abandonada) { return false; }   // abandoned: we have already given up on it

  f->parando = true;
  // The request BEFORE waking: the woken loop must already see "stop", otherwise
  // it goes back to sleep for its whole wait (fundo_esperar may be minutes).
  f->parada.pedido = true;
  xSemaphoreGive(f->acordar);
  if (parada_pedir(&f->parada, FUNDO_PARAR_MS)) {   // waits on the semaphore, without polling
    finalizar(f);
    return true;
  }
  // We do not kill the task (see why in kernel/tarefa_app.h): the request stays
  // standing, it exits when it can, and fundo_manter() cleans up at that point. The
  // pill tells the truth in the meantime.
  ESP_LOGE(TAG, "'%s' did not exit in %d ms - left stopping in the background", d->nome, FUNDO_PARAR_MS);
  notif_publicar(f->notif, 1, NOTIF_ESTADO, tr("parando..."));
  f->abandonada = true;
  f->parando = false;
  return false;
}

bool sistema_fundo_pedir(const fundo_def_t* d)
{
  if (!d || !d->corpo || !d->nome) { return false; }
  bool ok = false;
  for (int i = 0; i < FUNDO_MAX && !ok; ++i) { ok = s_pedidos[i].load() == d; }
  for (int i = 0; i < FUNDO_MAX && !ok; ++i) {
    const fundo_def_t* livre = nullptr;
    ok = s_pedidos[i].compare_exchange_strong(livre, d);
  }
  if (!ok) { ESP_LOGW(TAG, "no slot for the request for '%s'", d->nome); return false; }
  if (sistema_na_interface()) { fundo_manter(); } else { sistema_acordar_interface(); }
  return true;
}

// On the UI: starts what was requested and already has a free slot. What is still
// exiting stays requested - the trampoline wakes the UI when it exits.
static void atender_pedidos(void)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const fundo_def_t* d = s_pedidos[i].load();
    if (!d || achar(d)) { continue; }
    s_pedidos[i] = nullptr;
    if (!sistema_fundo_iniciar(d)) { ESP_LOGW(TAG, "the request for '%s' did not start", d->nome); }
  }
}

bool sistema_fundo_ativo(const fundo_def_t* d)
{
  bool ativo = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const fundo_s* f = &s_f[i];
    if (f->em_uso && f->def == d && !f->abandonada && !parada_pedida(&f->parada) && !saiu(f)) { ativo = true; }
  }
  taskEXIT_CRITICAL(&s_mux);
  return ativo;
}

int sistema_fundo_listar(fundo_info_t* out, int max)
{
  int n = 0;
  const int64_t agora = esp_timer_get_time();
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX && n < max; ++i) {
    const fundo_s* f = &s_f[i];
    if (!f->em_uso || !f->visivel) { continue; }
    out[n].nome = f->def->nome;
    out[n].ha_s = (uint32_t)((agora - f->desde_us) / 1000000);
    memcpy(out[n].resumo, f->resumo, sizeof(out[n].resumo));
    ++n;
  }
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

// From ANY task (the console): only marks, and the UI really stops it in
// fundo_manter() - that way ao_parar() always runs on the UI, as the
// contract promises.
bool sistema_fundo_parar_nome(const char* nome)
{
  // Under the lock: runs on the console, and the UI may be zeroing the slot.
  bool achou = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    fundo_s* f = &s_f[i];
    if (f->em_uso && !f->abandonada && strcmp(f->def->nome, nome) == 0) {
      f->parar_pedido = true;
      achou = true;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  if (achou) { sistema_acordar_interface(); }
  return achou;
}

// ---- the loop --------------------------------------------------------------

bool fundo_esperar(fundo_t* f, uint32_t ms)
{
  if (parada_pedida(&f->parada)) { return false; }
  xSemaphoreTake(f->acordar, pdMS_TO_TICKS(ms));
  f->cedeu_us = esp_timer_get_time();
  return !parada_pedida(&f->parada);
}

bool fundo_ceder(fundo_t* f)
{
  const int64_t agora = esp_timer_get_time();
  if (agora - f->cedeu_us >= FUNDO_CEDER_US) {
    vTaskDelay(1);   // this core's IDLE runs: watchdog and power kept up to date
    f->cedeu_us = esp_timer_get_time();
  }
  return !parada_pedida(&f->parada);
}

bool fundo_rodando(const fundo_t* f) { return !parada_pedida(&f->parada); }

void fundo_estado(fundo_t* f, int quantidade, const char* resumo)
{
  char r[NOTIF_RESUMO];
  snprintf(r, sizeof(r), "%s", resumo ? resumo : "");
  taskENTER_CRITICAL(&s_mux);
  memcpy(f->resumo, r, sizeof(r));
  taskEXIT_CRITICAL(&s_mux);
  // Minimum quantity 1: the pill never disappears while the background runs.
  if (f->visivel) { notif_publicar(f->notif, quantidade > 0 ? quantidade : 1, NOTIF_ESTADO, r); }
}

// ---- the kernel ------------------------------------------------------------

// app.h. The same mark a background part starting or stopping leaves: what serves it is
// fundo_manter(), on the UI.
void sistema_perfil_mudou(void) { s_perfil_sujo = true; sistema_acordar_interface(); }

// Under the lock: sistema_impor_preferencias() may be called outside the
// UI. It only ADDS - and the clock ceiling (cpu_max) stays with the screen: a background part
// with the default profile (360) would erase the lower ceiling an app asked for.
void fundo_somar_perfil(tab5_perfil_t& p)
{
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const fundo_s* f = &s_f[i];
    if (!f->em_uso) { continue; }
    const tab5_perfil_t& q = f->def->perfil;
    p.imu          = p.imu          || q.imu;
    p.microfone    = p.microfone    || q.microfone;
    p.alto_falante = p.alto_falante || q.alto_falante;
    p.grove_5v     = p.grove_5v     || q.grove_5v;
    p.usb_a_5v     = p.usb_a_5v     || q.usb_a_5v;
    p.camera       = p.camera       || q.camera;
    p.radio        = p.radio        || q.radio;
    if (q.cpu_min_mhz > p.cpu_min_mhz) { p.cpu_min_mhz = q.cpu_min_mhz; }
  }
  taskEXIT_CRITICAL(&s_mux);
}

void fundo_manter(void)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    fundo_s* f = &s_f[i];
    if (!f->em_uso || f->parando) { continue; }
    if (!f->visivel && !saiu(f)
     && esp_timer_get_time() - f->desde_us >= (int64_t)f->def->aviso_ms * 1000) {
      // Past aviso_ms and still running: now it appears, with the latest summary.
      char r[NOTIF_RESUMO];
      taskENTER_CRITICAL(&s_mux);
      memcpy(r, f->resumo, sizeof(r));
      taskEXIT_CRITICAL(&s_mux);
      f->visivel = true;
      notif_publicar(f->notif, 1, NOTIF_ESTADO, r);
    }
    if (saiu(f)) {
      // The loop ended on its own, or the abandoned one finally exited: collect the
      // semaphore (it was given) and clean up.
      parada_pedir(&f->parada, 0);
      finalizar(f);
    } else if (f->parar_pedido && !f->abandonada) {
      // WITHOUT WAITING: whoever asked was the user (the centre's X) or the console.
      // Waiting here would freeze the UI until the loop exits - and the loop
      // may be waiting precisely for the UI (the player, turning on the
      // speaker). Request, wake the loop and carry on: when it exits, the
      // branch above (exited) calls ao_parar() and cleans up, as the contract promises.
      f->parar_pedido = false;
      f->parada.pedido = true;
      f->pediu_us = esp_timer_get_time();
      xSemaphoreGive(f->acordar);
      notif_publicar(f->notif, 1, NOTIF_ESTADO, tr("parando..."));
    } else if (f->pediu_us && !f->abandonada
            && esp_timer_get_time() - f->pediu_us > (int64_t)FUNDO_PARAR_MS * 1000) {
      ESP_LOGE(TAG, "'%s' did not exit in %d ms - left stopping in the background", f->def->nome, FUNDO_PARAR_MS);
      f->abandonada = true;
    }
  }
  atender_pedidos();   // after the cleanup: the slot of whoever just exited is already usable
  // The set changed: the effective profile (screen + background parts) is reapplied here, on the
  // UI, and only here. WHEN IDLE IT WAITS: applying a profile touches brightness and
  // the clock ceiling, which idle is holding - it applies on waking.
  if (!tab5_power_em_repouso() && s_perfil_sujo.exchange(false)) { sistema_reaplicar_perfil(); }
}
