// Resources - see recursos.h.
#include "recursos.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "app.h"
#include "tab5_hora.hpp"
#include "tab5_wifi.hpp"
#include "idioma.h"   // N_(): names are translated on screen (tr), not in the log

static const char* TAG = "recursos";

// THE POLICY: what suspension preserves for each resource. A single place - whoever
// declares does not choose, it only says what it uses.
//   sem_sono  light sleep is forbidden (the peripheral does not wake the P4, or loses data)
//   radio     the Wi-Fi receiver stays on (modem sleep loses frames)
//   rede      Wi-Fi does not nap (the service answers whoever arrives)
//   tela      the screen stays on
typedef struct { const char* nome; bool sem_sono; bool radio, rede, tela; } politica_t;
static const politica_t POLITICA[RECURSO_N] = {
  /* CAMERA          */ { N_("camera"),          true,  false, false, false },   // the ISP and PPA do not wake the P4 from light sleep
  /* SOM             */ { N_("som"),             true,  false, false, false },   // I2S does not wake the P4 from light sleep
  /* PROCESSAMENTO   */ { N_("processamento"),   false, false, false, false },   // heavy maths only: light sleep still applies
  /* REDE            */ { N_("rede"),            true,  false, true,  false },   // the C6's SDIO does not wake the P4 from light sleep
  /* RADIO_ESCUTA    */ { N_("radio"),           false, true,  false, false },   // sem_sono=false: what holds off sleep is tab5_wifi_cochilo (ECONOMIA_SEGURA_WIFI), only with Wi-Fi connected and the receiver requested
  /* SERIAL_GROVE    */ { N_("serial do Grove"), true,  false, false, false },   // the UART loses bytes waking from light sleep
  /* USB_HOST        */ { N_("USB-A"),           true,  false, false, false },   // the USB bus does not sleep
  /* CARTAO          */ { N_("cartao"),          true,  false, false, false },   // long write: no pauses
  /* ENERGIA_EXTERNA */ { N_("tomada"),          true,  false, false, false },   // on mains power responsiveness is worth more than saving
  /* TELA_ACESA      */ { N_("tela acesa"),      false, false, false, true  },
};

static recurso_uso_t s_usos[RECURSOS_MAX];
static bool          s_vale[RECURSOS_MAX];
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

const char* sistema_recurso_nome(recurso_t r) { return r < RECURSO_N ? POLITICA[r].nome : "?"; }

// The union of the policies in use, applied to the mechanisms below. Outside the lock:
// they have their own and are idempotent.
static void aplicar(void)
{
  bool sem_sono = false, radio = false, rede = false, tela = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < RECURSOS_MAX; ++i) {
    if (!s_vale[i]) { continue; }
    const politica_t& p = POLITICA[s_usos[i].r];
    sem_sono |= p.sem_sono; radio |= p.radio; rede |= p.rede; tela |= p.tela;
  }
  taskEXIT_CRITICAL(&s_mux);
  sistema_economia_segurar(ECONOMIA_SEGURA_RECURSOS, sem_sono);
  tab5_wifi_manter_acordado(TAB5_ACORDADO_RECURSOS, radio);
  tab5_wifi_precisar(TAB5_PRECISA_RECURSOS, rede);
  sistema_manter_acordado(SISTEMA_ACORDADO_RECURSOS, tela);
  sistema_acordar_interface();
}

bool sistema_recurso(recurso_t r, const char* quem, bool usando)
{
  if (r >= RECURSO_N || !quem) { return false; }
  bool mudou = false, ok = true;
  taskENTER_CRITICAL(&s_mux);
  int achou = -1, livre = -1;
  for (int i = 0; i < RECURSOS_MAX; ++i) {
    if (s_vale[i] && s_usos[i].r == r && !strncmp(s_usos[i].quem, quem, sizeof(s_usos[i].quem) - 1)) { achou = i; }
    if (!s_vale[i] && livre < 0) { livre = i; }
  }
  if (usando && achou < 0) {
    if (livre < 0) { ok = false; }
    else {
      s_usos[livre].r = r;
      strlcpy(s_usos[livre].quem, quem, sizeof(s_usos[livre].quem));
      s_usos[livre].desde_ms = tab5_agora_ms();
      s_vale[livre] = true;
      mudou = true;
    }
  } else if (!usando && achou >= 0) {
    s_vale[achou] = false;
    mudou = true;
  }
  taskEXIT_CRITICAL(&s_mux);
  if (!ok) { ESP_LOGE(TAG, "no slot for %s (%s): %d uses at the same time", POLITICA[r].nome, quem, RECURSOS_MAX); return false; }
  if (!mudou) { return true; }
  ESP_LOGI(TAG, "%s %s: %s", usando ? "using" : "released", POLITICA[r].nome, quem);
  aplicar();
  return true;
}

int sistema_recursos(recurso_uso_t* out, int max)
{
  int n = 0;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < RECURSOS_MAX && n < max; ++i) { if (s_vale[i]) { out[n++] = s_usos[i]; } }
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

bool sistema_recurso_em_uso(recurso_t r)
{
  bool sim = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < RECURSOS_MAX && !sim; ++i) { sim = s_vale[i] && s_usos[i].r == r; }
  taskEXIT_CRITICAL(&s_mux);
  return sim;
}

bool sistema_recurso_de(recurso_t r, const char* quem)
{
  if (!quem) { return false; }
  bool sim = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < RECURSOS_MAX && !sim; ++i) {
    sim = s_vale[i] && s_usos[i].r == r && !strncmp(s_usos[i].quem, quem, sizeof(s_usos[i].quem) - 1);
  }
  taskEXIT_CRITICAL(&s_mux);
  return sim;
}

void recursos_resumo(char* out, size_t n)
{
  recurso_uso_t u[RECURSOS_MAX];
  const int k = sistema_recursos(u, RECURSOS_MAX);
  size_t p = 0;
  out[0] = 0;
  for (int i = 0; i < k && p < n; ++i) {
    const int w = snprintf(out + p, n - p, "%s%s(%s)", i ? " " : "", POLITICA[u[i].r].nome, u[i].quem);
    if (w < 0) { break; }
    p += (size_t)w;
  }
  if (!k) { snprintf(out, n, "nenhum"); }
}
