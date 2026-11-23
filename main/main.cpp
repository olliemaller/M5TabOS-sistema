// M5tabOS - a system skeleton with applications for the M5Stack Tab5.
//
// app_main = SETUP. Initialises, applies the boot power profile, creates the
// UI task and returns. When it returns, FreeRTOS destroys the
// "main" task and gives its stack back to the heap - the UI keeps running.

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <M5Unified.h>

#include "app.h"
#include "tab5_power.hpp"
#include "tab5_toque_gt911.hpp"
#include "ajustes.h"
#include "tab5_emoji.hpp"
#include "tab5_ui.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_marco - the internal RAM statement
#include "tab5_pendrive.hpp"
#include "tab5_usb.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_c6_sono.hpp"
#include "tab5_heap_psram.hpp"

static const char *TAG = "main";

// extern "C" is MANDATORY: the IDF startup is C and looks for the raw "app_main"
// symbol. Without it C++ mangles it to _Z8app_mainv and the link fails.
extern "C" void app_main(void)
{
    // The boot profile is the UNION of what the apps need to EXIST (IMU, mic,
    // speaker, RTC) - decisions that can only be taken before begin.
    // Everything switchable later is left off here and turned on per app.
    // The settings must come BEFORE begin: the initial screen brightness already comes
    // from what the user chose, without flashing the default value and correcting it later.
    // Internal RAM statement (tab5_monitor.hpp): each milestone records how much the
    // previous stage consumed. The first is the heap baseline.
    tab5_mem_marco("boot: app_main");
    ajustes_iniciar();
    tab5_mem_marco("settings (NVS)");

    tab5_perfil_t boot = sistema_perfil_de_boot();

    // The user's preferences apply FROM BOOT, not from the first app
    // opened. Before, only the brightness was imposed here, and the Grove 5V came up
    // off even with the preference saved as on.
    sistema_impor_preferencias(boot);

    auto cfg = M5.config();
    tab5_power_config(cfg, boot);     // phase 1: before begin
    tab5_heap_psram_conferir();       // PSRAM heap intact? if not, restart clean (tab5_heap_psram.hpp)
    M5.begin(cfg);                    // rails, MIPI-DSI panel, touch
    tab5_power_aplicar(boot);         // phase 2: after begin
    tab5_mem_marco("M5.begin (panel, touch)");
    tab5_painel();                    // which of the three screens (tab5_power.hpp) - before touching the clock
    tab5_toque_gt911_instalar();      // GT911 touch that does not invent a finger if I2C fails

    // Saved VOLUME, applied STRAIGHT AWAY - before any app plays its first sound
    // (M5.Speaker.tone()/playWav() brings up the ES8388 by itself, there and then, see the
    // big warning in ajustes.h). Without this, a session's first sound
    // would come out at the Speaker object's factory volume, only corrected later if
    // the user went through Settings.
    M5.Speaker.setVolume(ajustes_volume());

    // EMOJI, ONCE ONLY, FOR THE WHOLE SYSTEM. It must come after begin
    // (the callback is a panel method, which only exists from here on) and
    // before any app can draw text - hence the place, right at boot.
    // No app needs to know this exists: it is the same design as the rest of the
    // kernel, "whoever draws text should not have to worry about this".
    tab5_emoji_iniciar();
    tab5_mem_marco("emoji");

    // DEFAULT FONT FOR THE WHOLE SYSTEM. setFont() is GLOBAL STATE of the display
    // (not part of TextStyle - see the identical warning in tab5_ui.hpp), so
    // setting it once here, before any app draws, applies to the kernel and
    // apps without any of them having to call setFont() themselves.
    //
    // TAB5_FONTE_CORPO lives in tab5_ui.hpp (a change in progress: the
    // built-in Font0 became Font4, which was not comfortable to read and became
    // DejaVu24 - it only changes there, not here). Whoever draws a TITLE uses
    // Orbitron_Light on top and restores the body font afterwards (see tab5_fonte_titulo
    // / tab5_fonte_corpo in tab5_ui.hpp); the rest of the system stays on it.
    tab5_fontes_iniciar();   // the smooth fonts (tab5_ui.hpp) - before the first text
    M5.Display.setFont(TAB5_FONTE_CORPO);
    tab5_mem_marco("fonts");

    ESP_LOGI(TAG, "display %dx%d", M5.Display.width(), M5.Display.height());
    tab5_power_log("boot");

    // Right AFTER M5.begin(), which is when the MIPI-DSI panel is initialised:
    // if it is the panel that pins the CPU at maximum, its lock shows up here and not
    // before. This is the measurement that replaces our deduction about the locked clock.
    tab5_power_travas("apos M5.begin");

    // The SPLASH ("M5Tab OS" on screen) is NOT drawn here - it moved into
    // vTaskInterface (kernel/core.cpp), after tab5_power_relogio_dpi(). See the
    // warning there: drawing before that point means drawing before the
    // panel takes on its final clock, and the clock change rebuilds the DSI and
    // clears whatever is on screen - that was exactly the flicker observed
    // .
    sistema_iniciar();

    // USB FLASH DRIVE MODE: the notice and the cable watcher (turns the flash drive on by itself on
    // connecting to a computer, only with the setting on). After the system: it
    // checks the open app and the lock screen.
    tab5_pendrive_servico_iniciar();

    // USB-A: a flash drive someone plugs in appears at /usb (and in Music).
    // The connector's 5V already came from the boot profile (ajustes_usb_a).
    tab5_usb_iniciar();
    // The C6 sleeping while associated during idle, if the setting is on.
    tab5_c6_sono_iniciar();
    tab5_mem_marco("USB-A");

    tab5_mem_marco("sistema_iniciar (end)");
    ESP_LOGI(TAG, "M5tabOS %s started - boot complete", esp_app_get_description()->version);
}
