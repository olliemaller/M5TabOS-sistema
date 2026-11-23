// Console commands - TESTES (TESTS): concorrencia and simulacao (artificial load to prove the services).
// (Split from console.cpp; the COMANDOS table and the help remain there.)

#include "tab5_hora.hpp"   // tab5_agora_ms
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "tab5_gps.hpp"
#include "tab5_imu.hpp"
#include "tab5_tela.hpp"
#include "console/console_interno.hpp"

// ---------------------------------------------------------------------------
// FAKE SECOND CONSUMER (with a single app it cannot be tested).
// ---------------------------------------------------------------------------
// The system only opens ONE app at a time, so you cannot prove that
// tab5_gps_posicao()/tab5_gps_tempo()/tab5_imu_ultima() withstand TWO
// readers at the same time just by opening two apps - "two apps
// open" does not exist yet. This task is a real second reader, independent
// of any app: it hammers the three functions much faster than any
// screen draws, while you open Azimutal or IMU as normal. It proves
// two things: that the lock withstands real concurrent use without locking up
// or bringing the system down, and (comparing the numbers here with those on screen) that
// both readers always see the SAME value, never a "half-updated" one.
static bool s_conc_ativa = false;
static bool s_conc_parar = false;
static uint32_t s_conc_leituras = 0;

static void vTaskConcorrencia(void*)
{
  uint32_t ultimo_log = 0;
  while (!s_conc_parar) {
    nmea_posicao_t pos  = tab5_gps_posicao();
    nmea_tempo_t   hora = tab5_gps_tempo();
    tab5_imu_dados_t imu;
    tab5_imu_ultima(&imu);
    ++s_conc_leituras;
    (void)hora;

    const uint32_t agora = tab5_agora_ms();
    if (agora - ultimo_log >= 1000) {
      ultimo_log = agora;
      printf(tr("[concorrencia] %lu leituras | gps valida=%d lat=%.5f lon=%.5f "
             "| imu valida=%d ax=%.2f ay=%.2f az=%.2f\n"),
             (unsigned long)s_conc_leituras, (int)pos.valida, pos.lat, pos.lon,
             (int)imu.valida, imu.ax, imu.ay, imu.az);
    }
    // NO vTaskDelay(0)/pure yield - 1 tick (the real minimum) still hammers much
    // faster than any app draws (20ms cycle), without becoming a
    // busy-loop that steals the core from whoever is really drawing.
    vTaskDelay(1);
  }
  s_conc_ativa = false;
  s_conc_parar = false;
  vTaskDelete(nullptr);
}

void cmd_concorrencia(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "iniciar") == 0) {
    if (s_conc_ativa) { printf(tr("ja esta rodando - %lu leituras ate agora\n"), (unsigned long)s_conc_leituras); return; }
    s_conc_ativa = true;
    s_conc_parar = false;
    s_conc_leituras = 0;
    xTaskCreatePinnedToCore(vTaskConcorrencia, "conc_teste", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    printf("%s", tr("segundo leitor no ar - martelando GPS+IMU independente de qualquer app.\n"
           "abra o IMU (ou outro app que leia GPS/IMU) e compare os numeros com os daqui.\n"
           "'concorrencia parar' quando terminar.\n"));
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "parar") == 0) {
    if (!s_conc_ativa) { printf("%s", tr("nao estava rodando\n")); return; }
    s_conc_parar = true;
    printf("%s", tr("pedido para parar - confirma na proxima volta (ate 1 tick)\n"));
    return;
  }
  printf(tr("rodando: %s | leituras ate agora: %lu\n"
         "uso: concorrencia iniciar | concorrencia parar\n"),
         s_conc_ativa ? tr("sim") : tr("nao"), (unsigned long)s_conc_leituras);
}

// ---------------------------------------------------------------------------
// THREE FAKE "APPS", EACH ON ITS OWN TASK
// (stricter test: the 3 apps appear in the console requesting the service).
// ---------------------------------------------------------------------------
// The difference from "concorrencia" above: that one is ONE generic reader,
// hammering as fast as possible. This is three DIFFERENT
// CONSUMERS, each with the access pattern of a real app -
// its own cadence, logging under its own name - running at the same time, not on the
// SAME UI task any real app would use (no - each one on
// ITS OWN task, independent of the other two and of any app open on
// screen). It is the closest to "3 apps open at the same time" that can be
// simulated without the system really supporting that yet.
//
// App-Mapa   : reads only GPS, every 200ms - like a position/map screen.
// App-Nivel  : reads only IMU, as fast as possible - like a bubble level.
// App-Log    : reads GPS AND IMU together, every 1s - like a periodic logger.
//              This is the most interesting one: it proves that a consumer reading
//              TWO services in sequence does not deadlock (the GPS and IMU locks
//              are independent, never one inside the other - without that
//              there would be a risk of deadlock if one day someone reversed the
//              order in another consumer).
static bool     s_sim_ativa = false;
static bool     s_sim_parar = false;
static uint32_t s_sim_leituras[3] = { 0, 0, 0 };

static void vTaskSimMapa(void*)
{
  uint32_t ultimo_log = 0;
  while (!s_sim_parar) {
    nmea_posicao_t pos = tab5_gps_posicao();
    ++s_sim_leituras[0];

    const uint32_t agora = tab5_agora_ms();
    if (agora - ultimo_log >= 1000) {
      ultimo_log = agora;
      printf(tr("[app-mapa]  pedido #%-6lu -> gps valida=%d lat=%.5f lon=%.5f\n"),
             (unsigned long)s_sim_leituras[0], (int)pos.valida, pos.lat, pos.lon);
    }
    vTaskDelay(pdMS_TO_TICKS(200));   // cadence of a position screen
  }
  vTaskDelete(nullptr);
}

static void vTaskSimNivel(void*)
{
  uint32_t ultimo_log = 0;
  while (!s_sim_parar) {
    tab5_imu_dados_t d;
    tab5_imu_ultima(&d);
    ++s_sim_leituras[1];

    const uint32_t agora = tab5_agora_ms();
    if (agora - ultimo_log >= 1000) {
      ultimo_log = agora;
      printf(tr("[app-nivel] pedido #%-6lu -> imu valida=%d ax=%.2f ay=%.2f az=%.2f\n"),
             (unsigned long)s_sim_leituras[1], (int)d.valida, d.ax, d.ay, d.az);
    }
    vTaskDelay(1);   // as fast as possible, as a bubble level needs
  }
  vTaskDelete(nullptr);
}

static void vTaskSimLog(void*)
{
  while (!s_sim_parar) {
    nmea_posicao_t pos = tab5_gps_posicao();
    nmea_tempo_t   hora = tab5_gps_tempo();
    tab5_imu_dados_t imu;
    tab5_imu_ultima(&imu);
    ++s_sim_leituras[2];

    printf(tr("[app-log]   pedido #%-6lu -> gps(valida=%d) + hora(valida=%d) + imu(valida=%d, az=%.2f)\n"),
           (unsigned long)s_sim_leituras[2], (int)pos.valida, (int)hora.valida, (int)imu.valida, imu.az);
    vTaskDelay(pdMS_TO_TICKS(1000));   // cadence of a periodic logger
  }
  vTaskDelete(nullptr);
}

void cmd_simulacao(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "iniciar") == 0) {
    if (s_sim_ativa) { printf("%s", tr("ja esta rodando\n")); return; }
    s_sim_ativa = true;
    s_sim_parar = false;
    s_sim_leituras[0] = s_sim_leituras[1] = s_sim_leituras[2] = 0;
    xTaskCreatePinnedToCore(vTaskSimMapa,  "sim_mapa",  4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTaskSimNivel, "sim_nivel", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTaskSimLog,   "sim_log",   4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    printf("%s", tr("3 apps de mentira no ar: app-mapa (GPS/200ms), app-nivel (IMU/o mais rapido\n"
           "possivel), app-log (GPS+IMU/1s). Abra apps de verdade agora e compare.\n"
           "'simulacao parar' quando terminar.\n"));
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "parar") == 0) {
    if (!s_sim_ativa) { printf("%s", tr("nao estava rodando\n")); return; }
    s_sim_parar = true;
    s_sim_ativa = false;
    printf("%s", tr("pedido para parar os 3 - confirma nas proximas voltas de cada um\n"));
    return;
  }
  printf(tr("rodando: %s | leituras: mapa=%lu nivel=%lu log=%lu\n"
         "uso: simulacao iniciar | simulacao parar\n"),
         s_sim_ativa ? tr("sim") : tr("nao"),
         (unsigned long)s_sim_leituras[0], (unsigned long)s_sim_leituras[1], (unsigned long)s_sim_leituras[2]);
}
