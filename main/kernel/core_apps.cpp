// CORE - the apps: open and open by name, close, mounting the SD card in its
// own task, the refused app (card lent to the computer) and completing the
// opening. Split from core.cpp (the engine); what the parts share is in
// core_interno.hpp.

#include "tab5_sd.hpp"
#include "rolagem.hpp"
#include "tela_virtual.hpp"
#include "orientacao.hpp"
#include "trabalho.hpp"
#include "pool_tarefas.h"
#include "registro.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "core_interno.hpp"

static const char *TAG = "core";

bool sistema_abrir_app(const char* nome)
{
  if (!nome) { return false; }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (!strcmp(SISTEMA_APPS[i]->nome, nome)) { s_sim_abrir = SISTEMA_APPS[i]; return true; }
  }
  return false;
}


bool sistema_app_exige_sd(void)
{
  const app_t* a = s_app;
  return a && a != s_app_recusado && a->exige_sd;
}

// Whether the card was REALLY mounted for this app. Without this, an app that asked for
// SD but whose mount failed still called desmontar() on close -
// decrementing a count that was never incremented. If the power
// log had the card mounted at that instant, the counter dropped to
// zero and the card was UNMOUNTED FROM UNDER IT.
static bool s_app_montou_sd = false;

// App whose SD card mount is running in a background task (see
// vTaskMontarSDApp further down). NULL when there is no pending opening. The
// real opening - calling ao_abrir() and releasing the screen to the app - only
// happens when this flag goes back to NULL.
const app_t* s_app_carregando = nullptr;   // extern in internal.hpp - shell.cpp reads it in tratar_toque
const app_t* s_app_recusado   = nullptr;   // likewise - see recusar_app()
volatile bool s_sd_montando  = false;  // true from the trigger until the task finishes
static bool          s_sd_montou_ok = false;  // result of the last background mount

// The foreground app holds a card reference (app_t.usa_sd), or
// is opening and mounting. The kernel owns the reference: the flash drive mode
// asks here to refuse immediately, with the right reason. The read comes from
// another task and is not atomic - it only serves to pick the message; what
// really decides is the tab5_sd_emprestar count.
bool sistema_app_segura_sd(void)
{
  if (s_app_carregando || s_sd_montando) { return true; }
  const app_t* a = s_app;
  return a && a != s_app_recusado && s_app_montou_sd;
}
// ---------------------------------------------------------------------------
// App life cycle
// ---------------------------------------------------------------------------
// Declared here and defined further down (see the block "MONTAGEM (E DESMONTAGEM)
// DE SD EM TAREFA PROPRIA" - SD mounting in its own task): fechar_app() needs it before abrir_app()
// is read, and the definition lives next to its sibling vTaskMontarSDApp for clarity.
static void vTaskDesmontarSDApp(void*);

// NOT 'static' - declared in kernel/internal.hpp, called by
// kernel/shell.cpp (tratar_toque, when closing the app via the INICIO (home) button).
void fechar_app(void)
{
  if (!s_app) { return; }
  ESP_LOGI(TAG, "closing '%s'", s_app->nome);

  // REFUSED: the app never opened (not ao_abrir, not profile, not card) - only
  // the system's reason screen goes away.
  if (s_app == s_app_recusado) {
    s_app_recusado = nullptr;
    s_app = nullptr;
    rolagem_zerar(&s_rol);
    s_sujo_layout = true;
    return;
  }

  const bool devolver_sd = s_app_montou_sd;

  // THE APP'S WORK FINISHES BEFORE ao_fechar(): the queue is discarded and
  // whatever is executing is waited for - only then may the app free the
  // buffers the worker writes to (kernel/trabalho.hpp).
  // CLOSING IS A STALLED SCREEN: everything from here to the end runs on the UI task.
  // Past 150 ms, the log says who held it - the work or ao_fechar().
  const int64_t t0 = esp_timer_get_time();
  trabalho_encerrar_do_app();
  const int64_t t1 = esp_timer_get_time();
  if (s_app->ao_fechar) { s_app->ao_fechar(); }
  tela_virtual_soltar_superficie();   // the camera already stopped in ao_fechar()
  const int64_t t2 = esp_timer_get_time();
  if (t2 - t0 > 150000) {
    ESP_LOGW(TAG, "closing '%s' held the UI %lld ms (work %lld, ao_fechar %lld)",
             s_app->nome, (t2 - t0) / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000);
  }
  s_nav_travada     = false;  // safety net: a forgetful app does not trap anyone
  sistema_manter_acordado(SISTEMA_ACORDADO_APP, false);   // likewise: the request was the app's
  rolagem_zerar(&s_rol);
  s_app = nullptr;

  // Home is always portrait. Restore here, and not on the next open, so the
  // home screen never appears landscape for a frame. Also applies to the app
  // that followed the IMU and opened portrait but closed landscape.
  orientacao_ao_fechar();

  // Unmount AFTER ao_fechar(): the app may want to close files there.
  // Only give it back if THIS app actually took it.
  //
  // IN A BACKGROUND TASK, fire-and-forget: nothing from here on waits for the
  // unmount to finish, and the mutex + reference count inside
  // tab5_sd.cpp (see trava()/destrava()/s_refs) already protect against the obvious
  // race - opening another app with SD before this task runs only increments the
  // reference on the card that is still mounted, instead of mounting again.
  if (devolver_sd) {
    pool_criar(nullptr, vTaskDesmontarSDApp, nullptr, "sd_desmonta", 4096, 1);
    s_app_montou_sd = false;
  }
  registro_app_atual(nullptr);

  tab5_power_aplicar(perfil_da_home());
  s_sujo_layout = true;
}

// ---------------------------------------------------------------------------
// SD MOUNTING (AND UNMOUNTING) IN ITS OWN TASK
// ---------------------------------------------------------------------------
// Tab5_sd_montar()/desmontar() used to run DIRECTLY on the UI
// task, inside abrir_app()/fechar_app() - the SAME task that calls
// M5.update() and processes touch. montar_de_verdade() (tab5_sd.cpp) turns on the
// card IO LDO, negotiates the SDMMC host and mounts FAT: nothing catastrophic like
// Grove's ~10s, but the same class of error - real hardware taking
// tens to hundreds of ms inside the task that needs to keep reading the
// finger. A card slow to respond was enough for the same symptom as Grove
// to appear on a smaller scale.
//
// UNMOUNTING is simple: the app closes, leaves the screen, nobody waits for
// it any more. It goes to a loose task (fire-and-forget) with no coordination at all - the
// mutex and reference count inside tab5_sd.cpp (see trava()/
// destrava()/s_refs) already guarantee that concurrent montar() and desmontar() from
// different tasks do not trample each other, so there is no race to resolve here.
//
// MOUNTING is more delicate: ao_abrir() of apps such as Files and Mesh assumes
// the card is already ready (they list a folder, read history) on the first
// call. We cannot just fire it off and carry on calling ao_abrir()
// as if nothing had changed - the app opening has to wait for the result
// of the mount before calling ao_abrir(). That is why abrir_app() locks the screen on the
// app (s_app = app, screen shows "abrindo..." (opening...)) but ONLY releases the app for real -
// calling ao_abrir() - when the background task finishes. See
// concluir_abertura_do_sd() and the check in the UI loop.
static void vTaskMontarSDApp(void*)
{
  s_sd_montou_ok = tab5_sd_montar();
  s_sd_montando  = false;   // LAST - only now may the loop complete the opening
}

static void vTaskDesmontarSDApp(void*)
{
  tab5_sd_desmontar();
}

// ---------------------------------------------------------------------------
// APP REFUSED BY THE SYSTEM
// ---------------------------------------------------------------------------
// An app that REQUIRES the card (app_t.exige_sd: Files, Camera) does not open with
// it lent to the computer (flash drive mode) - the rule is the system's, not
// each app's. In place of the app a system screen appears saying why;
// INICIO (home) goes back normally. If the card comes back with it on screen, the app opens
// by itself (UI loop).
//
// s_app points to the app (the bar shows the name, INICIO works), but
// none of its functions are called - app_vivo() is false.
static void recusar_app(const app_t* app)
{
  ESP_LOGW(TAG, "'%s' does not open: the SD card is lent to the computer", app->nome);
  s_app = app;
  s_app_recusado = app;
  rolagem_zerar(&s_rol);
  orientacao_ao_fechar();                 // the reason screen is portrait
  tab5_power_aplicar(perfil_da_home());   // nothing from the app's profile stays on
  s_sujo_layout = true;
}

// Second half of abrir_app() when the app uses SD: called by the UI
// loop as soon as the background mount finishes (ok or not). Mirrors
// exactly what abrir_app() did inline, only after the wait.
void concluir_abertura_do_sd(void)
{
  const app_t* app = s_app_carregando;
  s_app_carregando = nullptr;

  s_app_montou_sd = s_sd_montou_ok;
  // RACE WITH THE FLASH DRIVE MODE: the card was lent between abrir_app()'s check
  // and the mount. Same rule: whoever requires the card does not open.
  if (!s_app_montou_sd && app->exige_sd && tab5_sd_emprestado()) {
    recusar_app(app);
    return;
  }
  if (!s_app_montou_sd) {
    ESP_LOGW(TAG, "'%s' asked for the SD card and it did NOT mount - opening anyway, "
                  "but the app will not be able to save", app->nome);
  }

  if (app->ao_abrir && !app->ao_abrir()) {
    ESP_LOGE(TAG, "'%s' refused to open", app->nome);
    if (s_app_montou_sd) {
      pool_criar(nullptr, vTaskDesmontarSDApp, nullptr, "sd_desmonta", 4096, 1);
      s_app_montou_sd = false;
    }
    s_app = nullptr;      // back to home: the opening really failed
    tab5_power_aplicar(perfil_da_home());
    s_sujo_layout = true;
    return;
  }

  registro_app_atual(app->nome);
  s_sujo_layout = true;   // swaps the "abrindo..." screen for the real content
}

// NOT 'static' - declared in kernel/internal.hpp, called by
// kernel/shell.cpp (tratar_toque, when a home icon is touched).
bool abrir_app(const app_t* app)
{
  if (s_app) { fechar_app(); }
  ESP_LOGI(TAG, "opening '%s'", app->nome);
  // THE FINGER THAT OPENED THE APP IS NOT A TOUCH ON IT. The app opens on release, but the
  // GT911 still says "pressed" for two reads, where the icon was: the
  // Settings app took that as a drag in the left column and changed the volume.
  // The same rule as the finger that lights the screen - applies until no finger is on the screen.
  s_dedo_do_despertar = true;

  // THE CARD IS IN THE COMPUTER and this app does not work without it: the system
  // does not open it (see recusar_app). Before anything else - no profile,
  // no orientation, no mount.
  if (app->exige_sd && tab5_sd_emprestado()) {
    recusar_app(app);
    return true;
  }

  // Order matters: turn on what the app needs BEFORE it starts.
  //
  // And BRIGHTNESS DOES NOT COME FROM THE APP. The app's profile says what hardware it needs;
  // how bright the screen is, is the preference of whoever holds the device. We copy the
  // profile and overwrite - that way app_t still declares the field (the
  // tab5_power is self-contained and uses it), but the setting is in charge.
  // THE SAME GOES FOR THE RADIO, and this line was born from a costly bug.
  //
  // Every app declares radio=false (the default), so opening ANY app cut
  // WLAN_PWR_EN and killed the ESP32-C6 with WiFi running. The log leaves
  // no doubt:
  //
  //   sistema: abrindo 'Ajustes'
  //   tab5pwr: trilho do C6 (WLAN_PWR_EN) = 0
  //   sdmmc_send_cmd returned 0x108        <- 1ms later
  //   eh_sdio: Unrecoverable host sdio state  -> board restart
  //
  // The general rule: the app's profile says what IT needs on, never what the
  // others may turn off. A system SERVICE resource - brightness, radio - is
  // decided here, and the app can only ADD, never subtract.
  // BOTH paths need the rule, which is why it also lives in
  // perfil_da_home(): closing an app also reapplies the profile, and a WiFi that
  // survives opening the Settings app but dies on closing it would be worse than the old bug -
  // it would only fail sometimes.
  // Orientation BEFORE the profile and any drawing: rotating changes width and
  // height, and everything calculated from them needs to see the new value.
  // Which rotations exist and why: kernel/orientacao.cpp.
  orientacao_ao_abrir(app);

  tab5_perfil_t perfil = app->perfil;
  sistema_impor_preferencias(perfil);
  tab5_power_aplicar(perfil);

  // Card mounted before ao_abrir(), for the usual reason: the app
  // may want to read configuration or open a log file right at opening.
  //
  // THE MOUNT ITSELF NO LONGER RUNS HERE (see the big block above,
  // "MONTAGEM (E DESMONTAGEM) DE SD EM TAREFA PROPRIA"). We lock the screen
  // on this app now - which is why s_app is already assigned in this function, before
  // ao_abrir() has been called - and return true: home goes away, the navigation
  // bar shows "INICIO" (the open-app check only looks at s_app) and
  // desenhar_conteudo() shows "abrindo..." while s_app_carregando points
  // to this app. Touch is swallowed in the meantime (see tratar_toque,
  // kernel/shell.cpp), for a few frames in practice - the same slack the
  // card took to mount, only now outside the task that reads the finger.
  //
  // Failing to mount does NOT stop the app opening - see concluir_abertura_do_sd().
  // Without a card, Azimutal still draws the map; it just does not save.
  s_app_montou_sd = false;
  if (app->usa_sd) {
    s_app_carregando = app;
    s_sd_montando    = true;
    pool_criar(nullptr, vTaskMontarSDApp, nullptr, "sd_monta", 4096, 1);

    s_app = app;
    rolagem_zerar(&s_rol);
    s_sujo_layout = true;
    return true;   // the opening completes in concluir_abertura_do_sd(), in the loop
  }

  if (app->ao_abrir && !app->ao_abrir()) {
    ESP_LOGE(TAG, "'%s' refused to open", app->nome);
    tab5_power_aplicar(perfil_da_home());
    return false;
  }

  s_app = app;
  rolagem_zerar(&s_rol);   // every app starts at the top
  registro_app_atual(app->nome);
  s_sujo_layout = true;
  return true;
}
