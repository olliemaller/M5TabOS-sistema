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

// ---- os limites, tirados dos registros ----------------------------------------
// Nas 4 descargas ate o fim (out/2026), o estimador chegou a 0% com o pacote
// a ~7,00 V sob 80-100 mA - tensao "aberta" (V - I*R) de ~7,02 V, 3,51 V por
// celula - e ainda sobravam 7 a 14 min ate o brownout (6,57 V).
static const float PISO_COM_PCT_V  = 7.10f;  // 0% so vale com a tensao aberta abaixo disto (3,55 V/celula)
static const float PISO_SOZINHO_V  = 6.90f;  // abaixo disto desliga mesmo que a % discorde (estimador perdido)
static const float PISO_DURO_V     = 6.75f;  // a tensao lida, sem corrigir: o brownout esta a minutos
static const int   CONFIRMAR_S     = 30;     // segundos seguidos no critico antes de desligar
static const float DESCARGA_MA     = -5.0f;  // abaixo disto esta na bateria (acima: tomada)
static const float CARGA_MA        = 20.0f;  // acima disto esta carregando
static const float CABO_MA         = -8.0f;  // acima disto ha cabo (carregando ou cheia na tomada)
static const int64_t GRAVAR_MAX_US = 8000000;  // espera do cartao antes de desligar de qualquer jeito

static int  s_notif   = -1;
static int  s_nivel   = 0;      // aviso publicado: 0 nenhum, 20, 10 ou 5
static int  s_critico = 0;      // contador de "critico", em segundos (o piso duro conta mais)
static bool s_desligando = false;
static int64_t s_desligar_us = 0;
static std::atomic<bool> s_gravou{false};
static bool s_semeou = false;   // a decisao do boot ja foi tomada

// A CARGA NA NVS. A RTC cobre o reinicio sem perder energia; desligado (pela
// guarda, pelo botao) so a NVS lembra. No boot COM O CABO a curva le muito
// alto (46% de verdade, 98% na curva, medido): ai o valor guardado vale mais.
// Desligado o aparelho quase nao gasta, e se carregou desligado o guardado so
// erra para menos - o fim de carga leva a 100% do mesmo jeito.
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
  if (!tab5_power_soc_da_curva_no_boot()) { return; }   // continuou da RTC: ja esta certo
  const int g = ajustes_bateria_soc();
  if (m.mA <= CARGA_MA || g == AJUSTES_SOC_NENHUM) { return; }   // em descanso a curva e boa
  float curva;
  tab5_power_soc_debug(nullptr, &curva, nullptr, nullptr);
  const float s = ((float)g < curva) ? (float)g : curva;
  if (tab5_power_soc_semear(s, "guardado (boot carregando)")) {
    ESP_LOGW(TAG, "boot carregando: curva diria %.0f%%, parte do guardado %d%%", curva, g);
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
  ESP_LOGW(TAG, "aviso: %s", r);
}

// No worker: gravar bloqueia, e a interface nao pode esperar o cartao.
static void gravar_tudo(void*)
{
  ajustes_gravar_agora();   // a ultima carga (guardar_soc) ainda esta no prazo da NVS
  tab5_log_descarregar_logo();
  tab5_sd_gravar_pendentes();
  s_gravou = true;
}

static void comecar_desligamento(const tab5_medida_t& m, float aberto)
{
  s_desligando  = true;
  s_desligar_us = esp_timer_get_time();
  guardar_soc(true);
  ESP_LOGW(TAG, "bateria vazia (%.3f V, aberta %.3f V, %.0f mA, %d%%): desligando",
           m.volts, aberto, m.mA, m.pct);
  char linha[96];
  snprintf(linha, sizeof(linha), "%lld,%.3f,%.1f,%d\n",
           (long long)(esp_timer_get_time() / 1000000), m.volts, m.mA, m.pct);
  tab5_sd_anexar("Sistema", "desligou_bateria.csv", linha, "uptime_s,volts,mA,pct\n", true);

  // Com a tela acesa, diz por que apagou. Em repouso o painel ja esta desligado.
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
  if (m.volts < 1.0f) { return; }   // sem leitura

  // CABO PRESENTE = NAO DORMIR. Com o chip em sono leve o USB nao responde a
  // enumeracao: ligar o cabo com a tela apagada deixava o Mac sem porta serial
  // (e sem gravacao) ate alguem tocar a tela (2026-10-17). O sinal nao pode ser
  // o do USB (o SOF que o pendrive usa so e visto acordado): e a corrente da
  // bateria. Na bateria o repouso puxa >= ~20 mA; com qualquer cabo ela fica
  // positiva (carregando) ou perto de zero (cheia). A leitura vem a cada 5 s
  // em repouso: o pior caso e o Mac tentar de novo uma vez.
  const bool cabo = m.mA > CABO_MA;
  sistema_recurso(RECURSO_ENERGIA_EXTERNA, "cabo", cabo);
  // O cabo CHEGOU enquanto o chip dormia: o Mac pode ter desistido da porta.
  // Um segundo acordado (SOF chegando, se for computador) e o console se
  // reanuncia se ninguem o enxergou.
  static bool    s_cabo_antes = true;      // boot com cabo: ja enumerou acordado
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
      // Voltou: algo segurou a energia (a tomada entrou no meio). Recomeca.
      s_desligando = false; s_gravou = false; s_critico = 0;
    }
    return;
  }

  // Na tomada: nenhum aviso, nenhum desligamento.
  if (m.mA > CARGA_MA) { publicar(0, m.pct); s_critico = 0; return; }
  if (m.mA > DESCARGA_MA) { s_critico = 0; return; }

  // Os avisos seguem a % mostrada; so descem (a % nao sobe fora da carga).
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
