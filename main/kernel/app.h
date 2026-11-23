// Contract for a system application.
//
// The idea: adding a new app means creating the folder main/app_<nome>/ with a
// file that defines 'extern const app_t APP_<NOME>' (see apps.cpp). Nothing else. The
// system takes care of opening, closing, drawing, routing touch and - most importantly -
// applying the power profile the app declared.
//
// On "C or C++": the code is written in C style (structs, function
// pointers, no classes), but in .cpp files because M5GFX is a C++
// library and there is no way to draw without going through it. The mental model is C's:
// tasks, queues and functions.

#pragma once

#include <M5Unified.h>
#include <stdint.h>
#include "tab5_power.hpp"
#include "tab5_sd.hpp"
#include "fundo.h"      // an app's background part (work that outlives the window)
#include "recursos.h"   // what each one uses: the system decides suspension

// Rectangle where the app may draw. It sits between the status bar (top) and the
// navigation bar (bottom) - the app never needs to know their heights.
typedef struct {
  int x;
  int y;
  int largura;
  int altura;
} area_t;

// SCREEN ORIENTATION of an app (the .orientacao field of app_t, below).
//
// ONLY TWO STATES EXIST: portrait (rotation 0, the home screen's) and landscape (rotation 3,
// the one that puts ExtPort1 - where the keyboard attaches - on the bottom edge). Rotations
// 1 and 2 are never used: 1 puts the screen upside down relative
// to the keyboard, and 2 is inverted portrait.
typedef enum {
  ORI_EM_PE = 0,    // the default: portrait, 720x1280
  ORI_DEITADO,      // fixed landscape, 1280x720
  // Follows the device (kernel/orientacao): portrait or landscape according to
  // gravity, between the same two states. With the physical keyboard attached,
  // it stays landscape - the only posture in which that makes sense.
  ORI_SEGUE_IMU,
  // Portrait ALWAYS, even with the physical keyboard attached (which lays all the
  // others down, kernel/orientacao). For whoever doesn't work rotated: the Camera (the
  // preview's DMA surface only exists with the screen at rotation 0).
  ORI_FIXO_EM_PE,
} orientacao_t;

// AN APP'S CONSOLE COMMAND (the .comandos field of app_t, below).
//
// The serial console belongs to the system, but an app may offer its own
// commands. They appear in 'help' in a group of their own (APPS) and are always
// called with the app name in front, so they never collide with a
// system command or another app's:
//
//     app <app> <comando> [argumentos]       e.g.: app exemplo contar 5
//
// <app> is the app's .nome, case-insensitive; spaces and hyphens may be
// omitted ('app espnow' finds "ESP-NOW").
//
// WHERE IT RUNS: on the CONSOLE task, not the UI one. So, as in
// any app task: don't draw (change the data and call
// sistema_pedir_redesenho()), don't talk internal I2C, and lock whatever
// the UI also reads. printf goes to the serial port.
//
// APP CLOSED: with so_aberto = true the system refuses the command while the app
// is not in the foreground - that is the rule for every command that touches
// state created in ao_abrir() (which doesn't exist with the app closed). A command that
// only reads static data or calls a service may leave it false.
//
// The list ends with an entry with nome = nullptr.
typedef struct {
  const char* nome;       // the word after the app name
  const char* uso;        // "argumentos\twhat it does", one form per line (without 'app <app> <comando>')
  void (*fn)(int argc, char** argv);   // argv[0] = the command name
  bool so_aberto;
} app_comando_t;

typedef struct {
  // ---- identity -----------------------------------------------------------
  const char* nome;     // shown on the status bar and under the icon
  uint16_t    cor;      // icon colour on the home screen

  // ---- power --------------------------------------------------------------
  // WHAT THIS APP NEEDS SWITCHED ON. Everything not listed here stays off
  // while it is in the foreground, and goes back to the system state
  // when it closes. It is the mechanism that stops the device heating up
  // powering things the app doesn't even use.
  tab5_perfil_t perfil;

  // This app writes or reads files. The system MOUNTS the card before calling
  // ao_abrir() and UNMOUNTS it after ao_fechar() - with reference counting,
  // so two apps using SD at the same time don't get in each other's way.
  //
  // The app doesn't call mount or unmount. It just asks for a file inside its
  // own folder:
  //     FILE* f = tab5_sd_abrir(APP_MEU.nome, "dados.csv", "a");
  // and writes with plain fprintf/fwrite. The folder /sdcard/apps/<Nome>/ is created
  // automatically the first time.
  bool usa_sd;

  // WITHOUT THE CARD THIS APP HAS NO PURPOSE (Arquivos: browses it; Camera: the
  // photo goes to it). With the card lent to the computer (USB flash drive
  // mode), the SYSTEM doesn't open the app - it shows a screen in its place saying
  // why, and opens it by itself when the card comes back - and flash drive mode doesn't start
  // with it open. It is not the app that protects itself: no app needs to handle
  // "the card vanished midway".
  //
  // Whoever only WRITES data (history, log) doesn't need this: the write
  // ports (hal/tab5_sd.hpp) store it in the journal and the app keeps working.
  bool exige_sd;

  // Which orientation this app wants the screen in (see orientacao_t above).
  //
  // ORI_DEITADO was born for Meshtastic: with the ExtPort1 keyboard
  // attached, landscape is the natural posture, and a message list fits
  // much more. ORI_SEGUE_IMU lets the device itself choose.
  //
  // It works because rotation in M5GFX transforms COORDINATES, not the
  // framebuffer: the double buffer (tab5_tela) builds the line pointers from
  // the panel's physical dimensions, which don't change. And area_util() already computes
  // everything from Display.width()/height(), so the status bar and navigation
  // rearrange themselves.
  //
  // WHOEVER FOLLOWS THE IMU MUST DRAW FROM THE AREA, always: the width
  // can change with the app open. The system only rotates between two frames, with no
  // finger on the screen, and sends a full pass right after (the area arrives
  // clear, sistema_area_foi_limpa() = true). Geometry kept from one frame
  // to the next - text measurements, button positions - has to be recomputed.
  orientacao_t orientacao;

  // (The clock ceiling with the screen off no longer belongs to the app: it comes from the policy of the
  // RESOURCES in use - kernel/recursos.h. The app that plays, records or serves the
  // network uses the service, and the service declares it.)

  // ---- lifecycle ----------------------------------------------------------
  // Called on opening. It is HERE that the app creates its tasks and queues.
  // Returning false aborts the opening (the system goes back to the home screen).
  bool (*ao_abrir)(void);

  // Called on closing. Must destroy EVERYTHING ao_abrir() created - tasks,
  // queues, buffers. No orphan tasks: a closed app must cost zero.
  void (*ao_fechar)(void);

  // ---- idle ---------------------------------------------------------------
  // Called around inactivity idle (kernel/core.cpp) - NOT
  // around ao_abrir/ao_fechar. They exist so an app can release hardware that
  // ao_abrir() switched on and that must NOT stay active during idle, without
  // having to close the whole app for it.
  //
  // BORN from a live finding: the Camera app froze and restarted the
  // device when coming back from idle. Cause - the camera's V4L2 capture and the
  // DSI panel use the SAME generic DMA controller on the chip (DW_GDMA,
  // confirmed in the ESP-IDF driver: esp_cam_ctlr_csi.c and
  // esp_lcd_panel_dpi.c call the same functions). Idle DESTROYS and
  // RECREATES the panel's DMA channel (see desmontar_tela()/montar_tela() in
  // hal/tab5_power.cpp) - if the camera keeps ITS channel alive
  // during that window, the new panel may be born stepping on a channel still
  // in use, and the driver crashes inside dw_gdma_channel_use_link_list()
  // with a "Store access fault".
  //
  // THE DMA DEFECT WAS FIXED IN THE SYSTEM (05/11/2026): it was
  // esp_lcd_panel_del deleting the channel while still running, and now hal/tab5_dma.cpp
  // stops the channel first - this applies to any DMA consumer, including those
  // that aren't apps (the browser camera went down precisely for lacking this
  // hook). Don't use these hooks to work around hardware defects or to
  // hold off suspension: that is the system's job (docs/SUSPENSAO_2026-11-05.md).
  // They remain for what belongs to the app: saving what is open (Notas).
  // THE CAMERA LEFT HERE TOO (06/11/2026): the kernel pauses the open app's
  // camera when the screen goes off and turns it back on at wake (tab5_camera_pausar) - the
  // Camera app and the Berry apps no longer have any hook.
  //
  // ao_repouso(): called BEFORE the system switches the screen off (with the
  // panel still up).
  // ao_retomar(): called AFTER the panel has been rebuilt - switch back on
  // here whatever was released.
  //
  // Both are OPTIONAL (nullptr = nothing to do, the common case). They run on the
  // UI task, the same as ao_abrir/ao_fechar - safe to call
  // anything those already call, including internal I2C.
  void (*ao_repouso)(void);
  void (*ao_retomar)(void);

  // ---- interface ----------------------------------------------------------
  // Called by the UI task, never by another. Don't call drawing
  // functions from inside your own tasks: LovyanGFX is not safe
  // for concurrent access. Pass the data through a queue and draw here.
  void (*desenhar)(lgfx::LovyanGFX& tela, const area_t& area);

  // Touch inside the app's area, already in coordinates RELATIVE to the area.
  // Fires on finger RELEASE (the system click: went down and up without
  // dragging more than ~2 mm, of any duration - kernel/core.cpp,
  // TOQUE_TOLERANCIA_PX), which is right for a button: you can
  // back out by dragging your finger off before releasing.
  void (*ao_tocar)(int x, int y);

  // LEVEL touch, delivered on EVERY UI cycle (20 ms), with the finger
  // still on the screen. 'pressionado' says whether there is a finger on the app area now; x,y only
  // count when it is true (otherwise they come as -1, -1).
  //
  // It exists because ao_tocar() only signals when the finger LEAVES, and there are two things
  // a button never needs but a game does: reacting on finger DOWN, and
  // knowing it is STILL there. Whoever wants to detect the moment of touch keeps
  // the previous value and compares - the rising edge belongs to the app, not the system.
  //
  // The two coexist: an app can use only ao_tocar (the normal case), only this one,
  // or both. Leave nullptr if not needed.
  void (*ao_tocar_nivel)(int x, int y, bool pressionado);

  // PHYSICAL KEYBOARD KEY (the ExtPort1 one), already decoded.
  //
  // 'texto' comes NUL-terminated, in UTF-8 (an accented letter composed
  // by the dead key is 2 bytes); for special keys it comes EMPTY and
  // what counts is 'codigo' (hal/tab5_teclado.hpp):
  //
  //   TAB5_TECLA_ENTER, _APAGAR (Backspace), _DEL, _ESC, _TAB,
  //   _ESQUERDA, _DIREITA, _CIMA, _BAIXO
  //
  // A code the app doesn't know: ignore it (it's a new key, not text). With
  // Ctrl/Alt held the text comes anyway - it's a shortcut, not typing.
  //
  // 'modificador' is the combination of TAB5_TECLA_CTRL/SHIFT/ALT at the moment.
  //
  // WHY THIS EXISTS, and it is a fix rather than a feature: the system had always read
  // the keyboard and PRINTED THE KEY TO THE LOG, without delivering it to anyone. And
  // the Mesh app, on seeing a physical keyboard attached, stopped opening the
  // virtual keyboard - so as not to steal screen space. Result: with the keyboard plugged in
  // there was no way to type at all, neither virtual nor physical. That was
  // the reason for "I could never send".
  //
  // Leave nullptr if the app doesn't read the keyboard.
  void (*ao_teclar)(const char* texto, uint8_t codigo, uint8_t modificador);

  // How often to redraw by itself, in ms.
  // 0 = only redraws when someone calls sistema_pedir_redesenho().
  // Prefer 0 or large values: every redraw costs CPU and power.
  uint32_t periodo_ms;

  // PERIOD THAT CHANGES WITH THE SCREEN (optional). When present, it beats
  // periodo_ms: returns the period for NOW. For the app that only needs a
  // clock on one page (Mesh: node ages) and on no other - the rest
  // of the time it returns 0 and only wakes on a notice.
  uint32_t (*periodo_agora)(void);

  // NOTICES this app is interested in (mask of AVISO_*). When a service
  // publishes one of them, the system redraws the app IMMEDIATELY - without waiting for any
  // period. It trades "asking every second whether it changed" for "being notified
  // when it changes". ao_aviso is optional: without it, the notice becomes a
  // sistema_pedir_redesenho(); with it, the app decides what to do.
  uint32_t avisos;
  void (*ao_aviso)(uint32_t avisos);

  // ---------------------------------------------------------------------
  // TOUCH SCROLLING
  // ---------------------------------------------------------------------
  // TOTAL height of the app's content, in pixels. 0 (the default) = doesn't scroll, and the
  // app sees exactly the visible area, as it always did.
  //
  // When larger than the area, the SYSTEM takes care of everything: dragging a finger
  // scrolls, drawing is clipped to the area, and ao_tocar coordinates arrive
  // already in CONTENT space. In other words: the app draws and tests touch in a
  // coordinate system that starts at 0 and goes up to altura_conteudo, without
  // knowing that scrolling exists.
  //
  // That is on purpose. The alternative - each app adding an offset to
  // every y - is a recipe for the app that forgets to add it in ONE place and ends up
  // with a button that responds in the wrong place after scrolling.
  //
  // A drag does NOT become a touch: the system measures the distance travelled and swallows
  // the click if it exceeds the threshold. Without that, every end of scroll would press
  // whatever was under the finger.
  int altura_conteudo;
  // The same height, decided NOW (optional; beats altura_conteudo). For
  // the app whose content changes with orientation: landscape (physical keyboard
  // attached, kernel/orientacao) the area is ~584 px tall instead of
  // ~1144, and what fitted in portrait now needs to scroll. Receives the visible area
  // (current width and height); 0 = doesn't scroll.
  int (*altura_agora)(const area_t& a);

  // Draws the icon on the home screen, inside the square (x, y, lado).
  //
  // nullptr - the default - draws the square in the app's colour with the initial of the
  // name, which is what there always was and is still fine for most. An app that
  // speaks a third-party protocol gains from showing THEIR brand: whoever looks for
  // Meshtastic looks for the Meshtastic symbol, not the letter M of "Mesh".
  //
  // The name is still drawn underneath by the system, in both cases.
  void (*icone)(lgfx::LovyanGFX& d, int x, int y, int lado);

  // THE SCREEN ROTATED with the app open (ORI_SEGUE_IMU, or the physical keyboard
  // attached/removed in any app that isn't ORI_FIXO_EM_PE). Called
  // on the UI task, AFTER the new rotation takes effect (sistema_area()
  // already returns the new dimensions) and BEFORE the full pass that
  // redraws it. It lets the app switch layout - no need to request a
  // redraw, the system already has. Optional: nullptr = nothing to do beyond
  // drawing for the new area.
  void (*ao_girar)(void);

  // This app's console commands (see app_comando_t above). nullptr = none.
  const app_comando_t* comandos;

  // OFF THE HOME SCREEN: the app exists and opens (sistema_abrir_app), but has
  // no icon on the home screen - it lives inside another (Energia, opened from Ajustes).
  bool fora_da_home;

  // IN THE PC BROWSER: opens this app's part on the system web server
  // (services/tab5_web - its service's start function, e.g.
  // berry_web_ligar). The portal (http://<ip>/) lists the apps that have this and
  // opens them through here; the app's "No PC" panel does too. nullptr = device only.
  // From any task (the portal calls it from inside a request).
  bool (*no_pc)(void);

  // ON THE NEW THEME (2026-11 redesign): true = the system clears this
  // app's area with TEMA_FUNDO instead of black, and the common components (tab5_celula,
  // tab5_escrever) draw text on that colour. Each app turns this on when
  // migrated to the theme - while false, everything stays on the old black.
  bool fundo_tema;
} app_t;

// ---------------------------------------------------------------------------
// System API, available to apps
// ---------------------------------------------------------------------------

// Marks the screen dirty: the app will be redrawn on the next cycle.
// Use when some data has changed, instead of requesting fast periodic redraws.
void sistema_pedir_redesenho(void);

// SYSTEM BEEP - the only way to play a short tone. ALWAYS call from the
// UI task (it talks I2C to the codec).
//
// It exists because the speaker is switched off after a few idle seconds
// (see SPEAKER_OCIOSO_MS in core_som.cpp) and the ES8388 takes a while to come up: a
// direct tone(), with the codec cold, ends BEFORE the DAC becomes audible - the
// first beep after a pause came out silent. Here, with the codec cold, the tone
// is preceded by silence until the codec warms up.
void sistema_beep(float hz, uint32_t ms);

// NOTE: a short phrase on the status bar, in place of the app name, for
// ~2 s - confirmation of something that doesn't change the screen ("Copiado", "Colado").
// From the UI task.
void sistema_recado(const char* texto);

// THE SPEAKER PINS FOR USB - only flash drive mode uses this (services/
// tab5_pendrive). The speaker's I2S (GPIO 26/27) is the USB PHY that ESP-IDF
// thinks belongs to OTG: configuring those pins switches USB-C off during
// flash drive mode (see the warning in core.cpp). Yield: the speaker is switched off by the
// UI and stays silent; true when it has already stopped (waits up to espera_ms).
// Give back: sound returns on the next beep. From any task except the UI.
bool sistema_som_ceder_pinos(uint32_t espera_ms);
void sistema_som_devolver_pinos(void);

// STREAMED SOUND - music, played in blocks by an app's background part
// (app_musica). The codec only switches on via the UI (I2C), so opening REQUESTS and
// waits; from then on the background part feeds it alone, one block at a time,
// without going through the UI. From any task except the UI.
//   abrir:  switches the speaker on (and waits for the codec to warm up); false = no sound
//           (pins yielded to flash drive mode, or the UI didn't respond).
//   livre:  one more block fits now (the speaker holds two: the one playing and
//           the next). The block owner uses THREE buffers in rotation.
//   tocar:  queues 'quadros' (stereo: L/R pairs). false = the stream was
//           closed from underneath (flash drive) - stop playing.
//   calar:  discards what is in the queue (pause, track change).
//   fechar: releases the speaker (it switches itself off after being idle).
bool sistema_som_fluxo_abrir(uint32_t espera_ms);
bool sistema_som_fluxo_livre(void);
int  sistema_som_fluxo_na_fila(void);   // blocks in the speaker now (0 = the queue ran dry: sound failed)
bool sistema_som_fluxo_tocar(const int16_t* pcm, size_t quadros, uint32_t hz, bool estereo);
void sistema_som_fluxo_calar(void);
void sistema_som_fluxo_fechar(void);

// WAKES THE UI NOW - Stage 2 of docs/ARQUITETURA.md. When
// idle, the UI may be sleeping up to ~1s waiting for the next periodic
// deadline (see kernel/core.cpp); this skips the wait the moment something
// genuinely new has happened, instead of letting the notice wait. Intended for
// SERVICES to call (notificacao.cpp and the camera bridge already do), but
// nothing stops an app from using it if it ever needs the same effect - it just puts an
// event on the queue, never blocks, safe from any task.
void sistema_acordar_interface(void);

// ---------------------------------------------------------------------------
// POWER-SAVING CONTRACT - software follows the hardware when idle
// ---------------------------------------------------------------------------
// With the screen off the P4 enters LIGHT SLEEP between one event and the next (tickless
// + light sleep, see tab5_power_sono_leve). Every wake-up has a cost: whoever has a
// periodic loop CHOOSES the timeout by level, instead of always spinning at the
// screen-on pace:
//     xQueueReceive(fila, &c, pdMS_TO_TICKS(sistema_ritmo_ms(1000, 10000)));
// No loss of responsiveness: a real event (queue, interrupt, notice) wakes it
// immediately - only the "look again just in case" gets spaced out more.
//
// Whoever NEEDS a fast response with the screen off (ESP-NOW in conversation:
// the C6's SDIO doesn't wake the P4) HOLDS off sleep with its own bit, and releases it when
// no longer needed - the same design as tab5_wifi_manter_acordado. Safe from
// any task.
typedef enum { ECONOMIA_ACORDADO = 0, ECONOMIA_REPOUSO = 1 } economia_t;
economia_t sistema_economia(void);
static inline uint32_t sistema_ritmo_ms(uint32_t acordado_ms, uint32_t repouso_ms)
{
  return sistema_economia() == ECONOMIA_REPOUSO ? repouso_ms : acordado_ms;
}
//
// SERVICES DON'T USE THIS DIRECTLY: they declare the RESOURCE they use (kernel/recursos.h)
// and its policy decides light sleep. What remains are the system's own bits:
#define ECONOMIA_SEGURA_WIFI     (1u << 0)   // Wi-Fi with receiver requested - services/tab5_wifi_cochilo
#define ECONOMIA_SEGURA_TESTE    (1u << 1)   // console: 'energia sono segurar'
#define ECONOMIA_SEGURA_RECURSOS (1u << 2)   // some resource in use forbids light sleep (kernel/recursos)
void     sistema_economia_segurar(uint32_t quem, bool sim);
uint32_t sistema_economia_seguros(void);   // the bits of whoever is holding now (0 = nobody)

// ---------------------------------------------------------------------------
// NOTICES - the service says what changed, the interested app is redrawn immediately
// ---------------------------------------------------------------------------
// Safe from ANY task (and cheap: an atomic OR and an event on the queue, without
// blocking). Several identical notices before the UI wakes become one.
#define AVISO_MESH_MSG    (1u << 0)   // new message (received or sent)
#define AVISO_MESH_ENVIO  (1u << 1)   // send state changed (ACK, error)
#define AVISO_MESH_NOS    (1u << 2)   // node table changed
#define AVISO_GPS         (1u << 3)   // new position
#define AVISO_TRILHA      (1u << 4)   // new track sample
#define AVISO_TRABALHO    (1u << 5)   // a sistema_trabalho() of this app finished (kernel/trabalho.hpp)
#define AVISO_ESPNOW      (1u << 6)   // ESP-NOW: message, send state or channel changed (services/tab5_espnow)
#define AVISO_VOLUMES     (1u << 7)   // a disk came or went (flash drive on USB-A) - services/tab5_volumes
#define AVISO_ARQUIVOS    (1u << 8)   // copy/move/empty progressed or finished - services/tab5_arquivos
#define AVISO_WEB         (1u << 9)   // the web server started, stopped or served a request - services/tab5_web
#define AVISO_SSH         (1u << 10)  // the shared SSH session changed (state or screen) - services/tab5_ssh
#define AVISO_VIDEO       (1u << 11)  // new frame, pause, end or error of the video - services/tab5_video
void sistema_avisar(uint32_t avisos);

// For when the whole screen changes content (turning a page, changing level):
// clears the area and repaints through the double buffer, without flicker. See kernel/core.cpp.
void sistema_pedir_redesenho_completo(void);

// True when the system has just CLEARED the area before calling the app's
// desenhar() - on opening, on returning from idle, on changing page.
// An app that paints by difference MUST check this and repaint everything; if it doesn't
// check, it comes back from idle with a blank screen.
bool sistema_area_foi_limpa(void);

// HOW MUCH TIME THIS FRAME REPRESENTS, in seconds - for whoever animates by
// time. It is the distance between the panel scans in which this frame and the
// previous one go on air (kernel/ritmo.hpp), and not between the instants they
// were drawn: what appears has moved exactly as much as stayed on screen.
// Call from inside desenhar(). Always > 0; clamp it as suits you (after
// a long pause it is large).
float sistema_quadro_dt(void);

// The app's drawing area - the SAME one passed to desenhar().
//
// It exists because ao_tocar() only receives x and y (already relative to the area) and not the area
// itself. Whoever needed it - on-screen keyboard, bar anchored to the bottom -
// rebuilt it by copying the heights of the status bar and navigation from inside
// what is now kernel/internal.hpp (ALTURA_BARRA/ALTURA_NAV):
//
//     M5.Display.height() - 64 - 96      <-- that's how the Mesh app did it
//
// The danger isn't the magic number: it is that DRAWING uses the real area and
// TOUCH used the copy. Changing a bar's height would leave the screen right and the
// finger wrong - a silent failure, which only shows up as "it isn't clicking".
//
// Use this anywhere that needs the geometry outside desenhar().
area_t sistema_area(void);

// SURFACE FOR DMA. An app that produces images in hardware
// (camera, and one day video/JPEG) doesn't draw pixel by pixel: the PPA writes
// into a screen-sized image, the app's own, allocated on the first request
// and returned when the app closes. sistema_superficie() gives the buffer (RGB565,
// the panel format, DMA-aligned) and where rectangle 'r'
// (SCREEN coordinates, like area_t's) sits inside it. Portrait only
// (rotation 0) - false in landscape. The rectangle must have the FULL WIDTH
// of the screen and y/height multiples of 8: the PPA invalidates the cache in whole
// lines (see tela_virtual.cpp). Whoever wrote signals with
// sistema_superficie_mudou(r), ON THE UI TASK (in desenhar): that is when
// the area goes, via DMA, to the panel's back page.
typedef struct {
  void*  buf;
  size_t bytes;
  int    pic_l, pic_a;   // the whole image (the screen)
  int    x, y;           // the corner of 'r' within it
} superficie_t;
bool sistema_superficie(const area_t& r, superficie_t* s);
void sistema_superficie_mudou(const area_t& r);

// Imposes on the profile everything the user chose and the system guarantees: brightness,
// radio, Grove 5V. Called by boot, by the home screen and when opening each app - if a
// new path applies a profile, it MUST go through here, otherwise the saved
// preference doesn't apply on that path (see the comment in kernel/core.cpp).
void sistema_impor_preferencias(tab5_perfil_t& p);

// Reapplies the profile of the CURRENT app (or of the home screen, if none is open) with the
// CURRENT preferences. It is the way for a setting to take effect immediately, without waiting for
// the next app switch.
//
// Born from a bug: switching off the Grove 5V in Settings turned off
// nothing - the rail only dropped on returning to the home screen, when the system rebuilt the
// profile. The Ajustes app switched it on by hand with tab5_power_grove(true) and the opposite
// direction had no counterpart, so "OFF" on screen and 5V on the wire coexisted.
//
// Touching hardware by hand from an app is the trap: the one who knows how to build
// the final state is the system, because only it knows the rule that the app ADDS and
// never subtracts. If one day the Ajustes app called tab5_power_grove(false) with an app
// that needs the Grove open, it would cut power out from under whoever was
// using it. Going through here that is impossible.
//
// UI task only: it touches the I2C expander.
// SYSTEM PALETTE. Here and not in kernel/core.cpp or kernel/shell.cpp because
// apps need it:
// a screen that invents its own grey stops looking like part of the same
// device, and that is how the old status bar ended up too light.
//
// LovyanGFX's TFT_DARKGREY is NOT suitable as a frame despite its name - it is
// MEDIUM grey, rgb(123,123,123), and competes with the content instead of receding.
// PREFIXED. Without "SIS_" the COR_BORDA here collided with a constant of the
// same name inside the Cobra app - a macro in a shared header doesn't respect
// scope, and the error shows up in the file of someone who didn't even know it existed.
//
// REDESIGN (2026-11): the three names remain, but now they are ALIASES of the
// central theme (tab5_tema.hpp). New code should use TEMA_* directly.
#include "tab5_tema.hpp"
#define SIS_FUNDO   TEMA_SUPERFICIE    /* bar, column and card background   */
#define SIS_LINHA   TEMA_LINHA         /* subtle separator                  */
#define SIS_BORDA   TEMA_TEXTO_3       /* column frame                      */

// THE AREA'S CURRENT BACKGROUND COLOUR: TEMA_FUNDO on the home screen without wallpaper and in
// apps with fundo_tema; black elsewhere. It is what the system clears the area with,
// and the default text background for the common components (tab5_ui).
uint16_t sistema_cor_fundo(void);

void sistema_reaplicar_perfil(void);

// THE POWER PROFILE CHANGED FROM OUTSIDE AN APP (a service started needing
// a rail, or stopped needing it): the UI reapplies it on the next
// cycle - with the screen on; when idle, when it wakes. Safe from
// ANY task (it only flags). It is the path for those who can't talk I2C.
void sistema_perfil_mudou(void);

// Requests a new Grove probe - for when someone SWAPS the module on the cable
// with the device on. Doesn't block: flags the request and the UI loop
// serves it on the next cycle, so the screen doesn't freeze without explanation.
// false = refused because an app is using the port right now.
bool sistema_grove_redetectar(void);

// true while the requested probe has not been served yet. Lets the screen
// say what is happening during the ~4s it takes.
bool sistema_grove_sondando(void);


// ALL THE FINGERS OF THE CURRENT SAMPLE, for whoever needs more than one - the
// virtual keyboard. ao_tocar_nivel() only counts the first finger: typing
// fast, the next finger goes down before the previous one lifts, becomes "the first"
// when it leaves, and the previous key never saw the release - the letter vanished.
// 'id' is the touch controller's: the same finger keeps the same id while
// it is on the screen. x/y in the SAME terms as ao_tocar_nivel (app area, with
// scrolling added); only fingers inside the area and pressed IN THIS sample
// (a finger that lifted disappears in the first sample without it). Valid during
// ao_tocar_nivel (the UI task samples and delivers in the same round).
#define SISTEMA_TOQUES_MAX 5
typedef struct { uint8_t id; int16_t x, y; } sistema_toque_t;
int sistema_toques(sistema_toque_t* out, int max);

// THE TOUCH IN PROGRESS HAS ALREADY BEEN HANDLED BY LEVEL: its release doesn't become a click.
// Call in ao_tocar_nivel, in the same round in which the finger is handled. Without this
// the system click (which arrives in the SAME round as the release, right after the
// level) landed on whatever was under the finger AFTER what the level did -
// the keyboard's X key closed the keyboard and the click landed on the send button
// of the bar that dropped into its place. Valid until the end of the touch.
void sistema_consumir_toque(void);

// BLUETOOTH ON/OFF BY THE USER: saves the choice (applies on the next
// boot), starts or stops the stack and takes Mesh along when it is
// configured for Bluetooth. From the UI task (switching on touches the
// C6 rail's I2C).
void sistema_bluetooth(bool ligar);

// THE OTHER SERVICES, UNDER THE SAME CONTRACT: saves the choice (applies on the next
// boot) and applies it now - the app doesn't repeat the steps or the dependencies. From
// the UI task (they touch rails/I2C).
void sistema_wifi(bool ligar);         // only acts if it changed
void sistema_grove(bool ligar);        // Grove 5V always on + new cable probe
// The Mesh path (AJUSTES_MESH_*). Bluetooth switches Bluetooth on along with it; always
// restarts the service via the new path.
void sistema_mesh_via(uint8_t via);
// The Mesh service again, via the saved path (MQTT server, node address or
// path changed). Doesn't block: runs on the worker.
void sistema_mesh_reiniciar(void);

// Switches on touch diagnostics: each click prints to the log where it landed, raw and
// converted. It exists to measure finger offset instead of guessing it.
void sistema_diag_toque(bool ligar);
// Requests a GT911 restart via the reset pin (the same as the watchdog does, when the
// chip goes "mute"). Done by the UI on the next round: I2C belongs to it.
void sistema_pedir_reinicio_toque(void);
// Requests the GT911 "X-ray" in the log (identification, firmware, configuration and its
// checksum). Read-only. Done by the UI on the next round.
void sistema_pedir_regs_toque(void);
// TEST BENCH (console): a click at SCREEN x,y (like a finger, status bar and navigation
// included), and opening an app (nullptr = back to the home screen). The UI
// executes it on the next round; wakes the screen if idle.
void sistema_simular_toque(int x, int y);
// Simulated drag (test bench): from (x0,y0) to (x1,y1), like a finger - unlocks,
// closes or scrolls the notification centre, changes the home page.
void sistema_simular_gesto(int x0, int y0, int x1, int y1);
void sistema_simular_abrir(const app_t* app);

// OPEN ANOTHER APP BY NAME ("Música") - Arquivos' "open with". The switch
// happens on the UI's next round (safe from inside an ao_tocar).
// false = there is no app with that name in this variant.
bool sistema_abrir_app(const char* nome);

// ICON ORDER ON THE HOME SCREEN. SISTEMA_APPS (apps.cpp) is the factory
// order; on top of it the user's applies, saved in NVS (ajustes.h) and
// changed by holding an icon on the home screen and dragging, or with 'ordem' on the console.
// Positions from 0 to SISTEMA_NUM_APPS-1. Move/default save and repaint the home screen.
const app_t* sistema_home_app(int pos);
int          sistema_home_posicao(const app_t* app);   // -1 = not registered
bool         sistema_home_mover(const app_t* app, int pos);
void         sistema_home_ordem_padrao(void);

// WHERE THE UI IS - for diagnosing "froze without a log".
// The monitor watches (loop stalled > 3 s becomes an event) and the console shows it ('ui').
typedef struct {
  const char* etapa;           // the loop step it is at (or stopped at)
  uint32_t    voltas;          // complete rounds since boot
  uint32_t    ms_desde_volta;  // how long since the last one finished
  uint32_t    ms_desde_dedo;   // how long since touch read a finger (UINT32_MAX = never)
  // Touch watchdog (core.cpp): GT911 pulses since boot, I2C failures,
  // stuck reports, "deaf" seconds (pulses with no finger read), and the last
  // status register read (0x814E). toque_reinicios: how many times the
  // "mute" GT911 was restarted via the reset pin (tab5_toque_reiniciar).
  uint32_t    toque_pulsos, toque_falhas, toque_presos, toque_surdos;
  uint32_t    toque_reinicios;
  uint8_t     toque_status;
  bool        repouso;
  const char* app;             // nullptr = home screen
} sistema_diag_t;
void sistema_diag_interface(sistema_diag_t* d);

// Locks/unlocks the HOME button on the navigation bar.
//
// The caller is the virtual keyboard (tecvirt_abrir/tecvirt_fechar) - apps don't
// need to. HOME sits centred just below the space bar, and a
// touch that overshoots the target would close the app mid-typing. When locked, the
// navigation strip belongs to the app: a touch there (click and level) arrives at the
// LAST line of the area, and the keyboard can sit against the bar. When locked, the
// status bar also doesn't open the notification centre, and outside requests
// to switch app (sistema_simular_abrir) are refused. The system unlocks
// by itself when any app closes.
void sistema_travar_navegacao(bool travar);

// KEEP THE SCREEN ON while there is a request - inactivity idle doesn't
// happen. For work that can't stop and that the person follows on
// screen without touching it. One bit per reason, from ANY task: one requester's
// request doesn't release another's. A request arriving with the screen off TURNS IT ON (the
// idle in progress also holds the clock at the floor). The app's one (SISTEMA_ACORDADO_APP) the system releases
// by itself when any app closes.
#define SISTEMA_ACORDADO_APP       (1u << 0)
#define SISTEMA_ACORDADO_RECURSOS  (1u << 1)   // RECURSO_TELA_ACESA in use (kernel/recursos) - the service declares the resource
void sistema_manter_acordado(uint32_t motivo, bool sim);

// The foreground app REQUIRES the card (app_t.exige_sd). Safe from
// any task: it is what flash drive mode checks before lending the card.
bool sistema_app_exige_sd(void);

// The foreground app has the card mounted for it (app_t.usa_sd),
// or is opening and mounting. Safe from any task (read only): flash drive mode
// refuses at once instead of waiting for the card to become free.
bool sistema_app_segura_sd(void);

// The lock screen is in front (home screen or app underneath). Safe from
// any task (reads a flag).
bool sistema_bloqueado(void);

// Name of the foreground app, or NULL if on the home screen.
const char* sistema_app_atual(void);

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

// Profile to use in M5.begin(). It is the UNION of what all registered apps
// need to EXIST - because flags like internal_imu and internal_mic can only
// be decided BEFORE begin, and the IMU can't be initialised afterwards.
// What is switchable at runtime (5V rails, camera, radio, brightness) does NOT go
// here: that is applied per app, at opening time.
tab5_perfil_t sistema_perfil_de_boot(void);

// Creates the UI task. Call after M5.begin().
void sistema_iniciar(void);

// Application table. Defined in apps.cpp: first the fixed ones (from the folders
// main/app_*, generated at build time), then those registered at boot (Berry apps from the
// card). Read-only outside boot.
#define SISTEMA_APPS_MAX 48
extern const app_t* SISTEMA_APPS[];
extern int          SISTEMA_NUM_APPS;

// Adds an app to the table. At boot, before the UI comes up, or on the
// UI after sistema_apps_truncar. 'app' must live forever.
// false = table full.
bool sistema_apps_registrar(const app_t* app);

// Removes from the table those registered from 'n' onwards (the fixed ones stay). ONLY ON THE
// UI, with the home screen up, and followed by new registrations and the
// icon order rebuilt - that is how the Berry apps are reread.
void sistema_apps_truncar(int n);

// EXTERNAL APPS - those not compiled in (scripts on the card). The core
// only knows these three functions; they are implemented by services/apps_de_fora.cpp,
// which in the berry variant hooks up the Berry apps and in the full one does nothing (see
// docs/VARIANTES.md). No other core file knows that Berry exists.
//   iniciar: at boot, after the card and before the home screen builds the order;
//   atender: on the UI, with the home screen up - applies a reread;
//            true = the list changed (the home screen rebuilds the order and redraws);
//   reler:   from any task EXCEPT the UI (reads the card) - the card
//            changed (back from flash drive mode, console).
void sistema_apps_de_fora_iniciar(void);
bool sistema_apps_de_fora_atender(void);
void sistema_apps_de_fora_reler(void);
// REMOVE (home screen: drag the icon to "Remover").
//   removivel: is the app external (a script on the card)? The system ones can't be removed.
//   remover:   removes the app - in the background (stops its background part,
//              sends the script and the data to the card's bin and rereads);
//              the icon disappears when the reread is served. From the UI.
bool sistema_apps_de_fora_removivel(const app_t* app);
void sistema_apps_de_fora_remover(const app_t* app);
