// Berry modules 'sistema' (time, power, sensors, notices, beep, background),
// 'idioma' and 'transferencia'. See berry_apps.cpp (the engine) and berry_interno.hpp.

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "berry_apps.hpp"   // berry.h: be_gc.h needs it first
extern "C" {
#include "be_gc.h"   // be_gc_memcount (outside the public API)
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

// The beep requested by the script: the codec talks I2C, which belongs to the UI - it plays
// after the job comes back, on the UI task.
float    s_bip_hz = 0;
uint32_t s_bip_ms = 0;

// Keep the screen on: also applied by the UI (-1 = nothing requested).
int s_acordado_pedido = -1;

// ---------------------------------------------------------------------------
// MODULE 'sistema'
// ---------------------------------------------------------------------------
static int s_redesenhar(bvm* vm) { sistema_pedir_redesenho(); be_return_nil(vm); }
static int s_ms(bvm* vm)         { be_pushint(vm, (bint)tab5_agora_ms()); be_return(vm); }
// Microseconds since boot, for MEASURING (only the difference matters: the number wraps
// around every ~35 min, and the subtraction stays correct).
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
// Is the screen off (idle)? So the background part can choose its own
// pace: each call to fundo() wakes the P4 from light sleep.
static int s_repouso(bvm* vm) { be_pushbool(vm, tab5_power_em_repouso()); be_return(vm); }

// recursos() - what the system is using right now and by whom (kernel/recursos.h):
// [{"recurso": "camera", "quem": "MeuApp", "s": 12}]. The script sees its own
// usage - the camera it turned on appears with its name - and that of the services.
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

// [ax, ay, az] in g, or nil with no reading. Calling it marks the FRAME: the UI reads
// the sensor before the next desenhar() (the system cache only moves once per
// second - slow for a spirit-level bubble). A frame that did not call it clears the mark:
// a tab without the IMU does not pay for I2C on every frame.
static int imu_tres(bvm* vm, bool giro)
{
  if (C->app) { C->app->usa_imu = true; }   // marks the frame (see above)
  // The gyroscope only stays on while someone reads it (hal/tab5_imu): Berry's
  // request is time-limited - each call renews it, 5 s without calling and it drops.
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
static int s_giro(bvm* vm) { return imu_tres(vm, true); }   // [gx, gy, gz] in degrees/s

// hora_de(epoch) - "HH:MM" in the local time zone (the time of a message), "" if 0.
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

// "dd/mm/yyyy" in the local time zone, or "" with the clock not set.
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

// {pct, volts, ma}: ma > 0 charging. pct -1 = no estimate.
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

// {ssid, ip, rssi, texto}: ip "" = no connection.
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

// nil with no GPS on the Grove; {fix, lat, lon, alt, sats}. Without a fix, lat/lon = 0.
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

// acordado(true): the screen does not turn off while the app is open.
static int s_acordado(bvm* vm)
{
  if (!so_janela(vm, "sistema.acordado")) { be_return_nil(vm); }
  s_acordado_pedido = (be_top(vm) < 1 || be_tobool(vm, 1)) ? 1 : 0;
  be_return_nil(vm);
}

// periodo(ms): changes the '# periodo:' with the app open (0 = only on request).
// periodo() returns the current one.
static int s_periodo(bvm* vm)
{
  if (!C->app || !so_janela(vm, "sistema.periodo")) { be_return_nil(vm); }
  if (be_top(vm) >= 1 && be_isnumber(vm, 1)) {
    const int v = (int)be_toint(vm, 1);
    C->app->periodo = v < 0 ? 0 : (uint32_t)v;
    sistema_pedir_redesenho();   // the new pace applies from the next frame
  }
  be_pushint(vm, (bint)C->app->periodo);
  be_return(vm);
}

// acumular(true): the frame stops arriving clean. desenhar(tudo) receives
// tudo = true when the area WAS cleared (opened, rotated, keyboard, back from the
// lock screen) - draw everything; and tudo = false otherwise - draw ONLY what changed
// (what was already on screen stays there). It costs only what changes: it is the way to
// animate something small on a full screen, or to paint with a finger.
static int s_acumular(bvm* vm)
{
  if (!C->app || !so_janela(vm, "sistema.acumular")) { be_return_nil(vm); }
  if (be_top(vm) >= 1) {
    const bool v = be_tobool(vm, 1);
    if (v != C->app->acumular) {
      C->app->acumular = v;
      sistema_pedir_redesenho_completo();   // starts from a clean area (tudo = true)
    }
  }
  be_pushbool(vm, C->app->acumular);
  be_return(vm);
}

// dt(): seconds between this frame and the previous one ON SCREEN - to animate by
// time (position += speed * dt) and not by frame.
static int s_dt(bvm* vm) { be_pushreal(vm, (breal)sistema_quadro_dt()); be_return(vm); }

// THE BACKGROUND PART. fundo(true [, ms]): with the window closed the system keeps
// calling the script's fundo() every 'ms' (default 1000) - the same machine, the
// same variables. fundo(false) stops it (from inside fundo() too). fundo()
// says whether it is running. Starting and stopping are applied by the UI after the
// job comes back (the kernel starts the background on the requester's task).

static int s_fundo(bvm* vm)
{
  berry_app_t* a = C->app;
  if (!a) { be_return_nil(vm); }
  if (be_top(vm) >= 1) {
    const bool ligar = be_tobool(vm, 1);
    if (C->fundo) {
      if (!ligar) { a->fundo_sair = true; }   // the loop exits after this pass
    } else {
      a->fundo_pedido = ligar ? FP_LIGAR : FP_DESLIGAR;
      if (ligar) {
        const int ms = arg_int(vm, 2, 1000);
        a->fundo_ms = ms < 100 ? 100 : (uint32_t)ms;   // 100 ms: any faster and the Berry task is busy non-stop
      }
    }
  }
  be_pushbool(vm, a->fundo_pedido == FP_LIGAR ||
                  (a->fundo_pedido != FP_DESLIGAR && sistema_fundo_ativo(&a->fundo_def) && !a->fundo_sair));
  be_return(vm);
}

// fundo_estado(texto [, n]): what the bar chip says while the background runs.
static int s_fundo_estado(bvm* vm)
{
  berry_app_t* a = C->app;
  if (!a) { be_return_nil(vm); }
  snprintf(a->fundo_txt, sizeof(a->fundo_txt), "%s", arg_txt(vm, 1));
  a->fundo_n = arg_int(vm, 2, 0);
  be_return_nil(vm);
}

// memoria(): {app, berry, orcamento} in bytes - what THIS machine uses, what
// all the Berry machines add up to, and their ceiling (see berry_mem.h).
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

// The notice level (sistema.avisar, 2nd argument): "info", "atencao" or
// "urgente". Unknown text (or no argument) = atencao, the usual.
static notif_nivel_t aviso_nivel(bvm* vm)
{
  const char* n = arg_txt(vm, 2);
  if (!strcmp(n, "info"))    { return NOTIF_INFO; }
  if (!strcmp(n, "urgente")) { return NOTIF_URGENTE; }
  return NOTIF_ATENCAO;
}

// avisar("texto" [, nivel]) puts a notice on the bar; avisar() removes it. nivel:
// "info" (nothing required of the person), "atencao" (the default) or "urgente" (requires action).
static int s_avisar(bvm* vm)
{
  if (!C->app) { be_return_nil(vm); }
  if (C->app->notif < 0) {
    C->app->notif = notif_registrar(C->app->nome);   // the name survives: it is static
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
// IDIOMA - the interface language, chosen in Settings (services/idioma.h). The app only
// reads it: the choice belongs to whoever holds the device.
// ---------------------------------------------------------------------------
// atual() - "pt" or "en".
static int i_atual(bvm* vm)
{
  be_pushstring(vm, idioma_codigo(idioma_atual()));
  be_return(vm);
}

// nome() - the current language in itself: "Português", "English".
static int i_nome(bvm* vm)
{
  be_pushstring(vm, idioma_nome(idioma_atual()));
  be_return(vm);
}

// tr(texto) - the SYSTEM phrase in the current language ("Cancelar" -> "Cancel");
// with no translation, it comes back unchanged.
static int i_tr(bvm* vm)
{
  be_pushstring(vm, tr(arg_txt(vm, 1)));
  be_return(vm);
}

// escolher(pt, en) - the app's own phrases: returns the one in the current language
// (without the English one, the Portuguese).
static int i_escolher(bvm* vm)
{
  const bool en = idioma_atual() != 0 && be_top(vm) >= 2 && be_isstring(vm, 2);   // outside Portuguese, the app's English one
  be_pushstring(vm, arg_txt(vm, en ? 2 : 1));
  be_return(vm);
}

// ---------------------------------------------------------------------------
// TRANSFERENCIA - the system clipboard (services/transferencia.h):
// the same text that long press copies in Mesh, ESP-NOW and Notes.
// ---------------------------------------------------------------------------
// copiar(texto) - true if it stored it (up to 4 KB; the rest is cut).
static int tr_copiar(bvm* vm)
{
  be_pushbool(vm, transf_copiar(arg_txt(vm, 1)));
  be_return(vm);
}

// colar() - the copied text ("" = empty).
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
