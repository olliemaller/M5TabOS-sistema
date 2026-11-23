#include "tab5_bateria.hpp"
#include "idioma.h"
#include "tab5_hora.hpp"   // tab5_passou_s
#include "tab5_power.hpp"
#include "tab5_sd.hpp"
#include "tab5_log.hpp"
#include "notificacao.h"
#include "ajustes.h"
#include "trabalho.hpp"
#include "app.h"
#include "tab5_pendrive.hpp"

#include <M5Unified.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "bateria";

// ---- the limits, taken from the logs ----------------------------------------
// In the 4 discharges to the end (Oct 2026), the estimator reached 0% with the pack
// at ~7.00 V under 80-100 mA - an "open" voltage (V - I*R) of ~7.02 V, 3.51 V per
// cell - and there were still 7 to 14 min left until brownout (6.57 V).
static const float PISO_COM_PCT_V  = 7.10f;  // 0% only counts with the open voltage below this (3.55 V/cell)
static const float PISO_SOZINHO_V  = 6.90f;  // below this it shuts down even if the % disagrees (estimator lost)
static const float PISO_DURO_V     = 6.75f;  // the voltage as read, uncorrected: brownout is minutes away
static const int   CONFIRMAR_S     = 30;     // consecutive seconds in critical before shutting down
static const float DESCARGA_MA     = -5.0f;  // below this it is on battery (above: mains)
static const float CARGA_MA        = 20.0f;  // above this it is charging
static const float CABO_MA         = -8.0f;  // above this there is a cable (charging or full on mains)
static const int64_t GRAVAR_MAX_US = 8000000;  // wait for the SD card before shutting down regardless

static int  s_notif   = -1;
static int  s_nivel   = 0;      // notice published: 0 none, 20, 10 or 5
static int  s_critico = 0;      // "critical" counter, in seconds (the hard floor counts more)
static bool s_desligando = false;
static int64_t s_desligar_us = 0;
static std::atomic<bool> s_gravou{false};
static bool s_semeou = false;   // the boot decision has already been made

// THE CHARGE IN NVS. RTC covers a restart without losing power; powered off (by the
// guard, by the button) only NVS remembers. At boot WITH THE CABLE the curve reads far
// too high (46% in reality, 98% on the curve, measured): then the stored value is worth more.
// Switched off the device uses almost nothing, and if it charged while off the stored value only
// errs low - the end of charge takes it to 100% anyway.
static void guardar_soc(bool agora)
{
  float soc;
  tab5_power_soc_debug(&soc, nullptr, nullptr, nullptr);
  const int v = (int)lroundf(soc);
  const int g = ajustes_bateria_soc();
  if (v < 0 || v > 100) { return; }
  if (agora || g == AJUSTES_SOC_NENHUM || abs(v - g) >= 2) { ajustes_definir_bateria_soc((uint8_t)v); }
}

static void decidir_boot(const tab5_medida_t& m)
{
  s_semeou = true;
  if (!tab5_power_soc_da_curva_no_boot()) { return; }   // carried on from RTC: already correct
  const int g = ajustes_bateria_soc();
  if (m.mA <= CARGA_MA || g == AJUSTES_SOC_NENHUM) { return; }   // at rest the curve is good
  float curva;
  tab5_power_soc_debug(nullptr, &curva, nullptr, nullptr);
  const float s = ((float)g < curva) ? (float)g : curva;
  if (tab5_power_soc_semear(s, "guardado (boot carregando)")) {
    ESP_LOGW(TAG, "booted while charging: curve would say %.0f%%, starting from stored %d%%", curva, g);
  }
}

static void publicar(int nivel, int pct)
{
  if (nivel == s_nivel) { return; }
  s_nivel = nivel;
  if (s_notif < 0) {
    s_notif = notif_registrar("Bateria");
    notif_associar_app(s_notif, "Energia");
  }
  if (nivel == 0) { notif_limpar(s_notif); return; }
  char r[NOTIF_RESUMO];
  if (nivel == 5) { snprintf(r, sizeof(r), "%d%% - desliga em 0%%", pct); }
  else            { snprintf(r, sizeof(r), tr("%d%% - ligue na tomada"), pct); }
  notif_publicar(s_notif, 1, nivel == 20 ? NOTIF_ATENCAO : NOTIF_URGENTE, r);
  ESP_LOGW(TAG, "notice: %s", r);
}

// On the worker: writing blocks, and the UI can't wait for the SD card.
static void gravar_tudo(void*)
{
  ajustes_gravar_agora();   // the last charge (guardar_soc) is still within the NVS deadline
  tab5_log_descarregar_logo();
  tab5_sd_gravar_pendentes();
  s_gravou = true;
}

static void comecar_desligamento(const tab5_medida_t& m, float aberto)
{
  s_desligando  = true;
  s_desligar_us = esp_timer_get_time();
  guardar_soc(true);
  ESP_LOGW(TAG, "battery empty (%.3f V, open %.3f V, %.0f mA, %d%%): shutting down",
           m.volts, aberto, m.mA, m.pct);
  char linha[96];
  snprintf(linha, sizeof(linha), "%lld,%.3f,%.1f,%d\n",
           (long long)(esp_timer_get_time() / 1000000), m.volts, m.mA, m.pct);
  tab5_sd_anexar("Sistema", "desligou_bateria.csv", linha, "uptime_s,volts,mA,pct\n", true);

  // With the screen on, say why it went off. When idle the panel is already off.
  if (!tab5_power_em_repouso()) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(tr("Bateria vazia"), M5.Display.width() / 2, M5.Display.height() / 2 - 30);
    M5.Display.drawString(tr("desligando - ligue na tomada"), M5.Display.width() / 2, M5.Display.height() / 2 + 30);
    M5.Display.setTextDatum(top_left);
  }
  if (!sistema_trabalho_solto(gravar_tudo, nullptr)) { s_gravou = true; }
}

void tab5_bateria_vigiar(void)
{
  const tab5_medida_t m = tab5_power_ultima();
  if (m.volts < 1.0f) { return; }   // no reading

  // CABLE PRESENT = DON'T SLEEP. With the chip in light sleep USB doesn't respond to
  // enumeration: plugging in the cable with the screen off left the Mac without a serial port
  // (and without flashing) until someone touched the screen (2026-10-17). The signal can't be
  // USB's (the SOF the USB drive uses is only seen while awake): it is the battery
  // current. On battery, idle draws >= ~20 mA; with any cable it goes
  // positive (charging) or near zero (full). The reading comes every 5 s
  // when idle: the worst case is the Mac retrying once.
  const bool cabo = m.mA > CABO_MA;
  sistema_recurso(RECURSO_ENERGIA_EXTERNA, "cabo", cabo);
  // The cable ARRIVED while the chip was asleep: the Mac may have given up on the port.
  // One second awake (SOF arriving, if it is a computer) and the console
  // re-announces itself if nobody saw it.
  static bool    s_cabo_antes = true;      // boot with cable: already enumerated while awake
  static bool    s_dormia = false;
  static int64_t s_cabo_us = 0;
  if (cabo && !s_cabo_antes) { s_cabo_us = esp_timer_get_time(); s_dormia = tab5_power_sono_leve_ligado() || tab5_power_em_repouso(); }
  if (cabo && s_dormia && s_cabo_us && esp_timer_get_time() - s_cabo_us > 1000000) {
    s_cabo_us = 0; s_dormia = false;
    tab5_pendrive_console_reanunciar();
  }
  s_cabo_antes = cabo;

  if (!s_semeou) { decidir_boot(m); }
  guardar_soc(false);

  if (s_desligando) {
    if (s_gravou || esp_timer_get_time() - s_desligar_us > GRAVAR_MAX_US) {
      ESP_LOGW(TAG, "powerOff");
      M5.Power.powerOff();
      // It came back: something held the power (mains was plugged in midway). Start over.
      s_desligando = false; s_gravou = false; s_critico = 0;
    }
    return;
  }

  // On mains: no notice, no shutdown.
  if (m.mA > CARGA_MA) { publicar(0, m.pct); s_critico = 0; return; }
  if (m.mA > DESCARGA_MA) { s_critico = 0; return; }

  // The notices follow the displayed %; they only go down (the % doesn't rise outside charging).
  const int nivel = (m.pct < 0) ? s_nivel : (m.pct <= 5) ? 5 : (m.pct <= 10) ? 10 : (m.pct <= 20) ? 20 : 0;
  if (nivel != 0 && (s_nivel == 0 || nivel < s_nivel)) { publicar(nivel, m.pct); }

  const float aberto = m.volts - (m.mA / 1000.0f) * TAB5_R_INTERNA;
  const bool critico = (m.pct == 0 && aberto < PISO_COM_PCT_V) || aberto < PISO_SOZINHO_V;
  static int64_t ult = 0;
  const int dt = tab5_passou_s(&ult);
  if (m.volts < PISO_DURO_V)  { s_critico += 6 * dt; }   // ~5 s
  else if (critico)           { s_critico += dt; }
  else                        { s_critico = 0; }

  if (s_critico >= CONFIRMAR_S) { comecar_desligamento(m, aberto); }
}

void tab5_bateria_resumo(char* out, size_t tam)
{
  if (s_desligando)      { snprintf(out, tam, tr("desligando (bateria vazia)")); }
  else if (s_critico > 0) { snprintf(out, tam, "critico ha %d s (desliga em %d s)", s_critico, CONFIRMAR_S); }
  else if (s_nivel > 0)  { snprintf(out, tam, tr("aviso de %d%% publicado; desliga em 0%%"), s_nivel); }
  else                   { snprintf(out, tam, "ok (avisa em 20/10/5%%, desliga em 0%%)"); }
}
