// PRIVATE BRIDGE between core.cpp and shell.cpp - NEVER included by an app.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS (Stage 1 of docs/ARQUITETURA.md)
// -------------------------------------------------------------------------
// sistema.cpp had grown from 1129 to 2457 lines doing eight different
// things, sharing statics with no boundary at all - "the file
// where the next bug will hide", in the document's own words.
// It was split in two, WITHOUT CHANGING A LINE OF LOGIC:
//
//   core.cpp   the ENGINE - drawing schedule, app life cycle
//              (open/close), power policy. Decides WHAT runs and
//              WHEN.
//   shell.cpp  the APPEARANCE - home screen, status bar, routing
//              touch to the right app, touch calibration. "Shell" in the
//              OS sense (Windows Shell, GNOME Shell): the visual layer
//              around the apps. NOT the same "shell" as console.cpp (the
//              command-line interface over serial) - two different
//              concepts, two different files, unrelated.
//
// The ONLY exception to "zero logic change": tratar_toque() (now in
// shell.cpp) called the PRIVATE function area_util() to get the geometry of the
// usable area; it became the call to sistema_area() (the same thing, public -
// sistema_area() is literally "return area_util();"). Switching to that
// avoids exposing a whole function just for one call, without changing any
// result.
//
// After Stage 1, core.cpp and shell.cpp were split by topic (only
// moved between files): core_*.cpp and shell_*.cpp. Each group has its own internal header
// (core_interno.hpp, shell_interno.hpp), which includes this one.
//
// app.h is still the ONLY contract an app sees. This header is
// deliberately rawer than it - extern variables instead of getters -
// because the goal here is not a pretty API, it is not duplicating state between the
// two files.
#pragma once

#include "app.h"
#include "rolagem.hpp"

// ---------------------------------------------------------------------------
// SHARED GEOMETRY - used by core.cpp (area_util(), touch
// calculations) and shell.cpp (drawing the bar, home, navigation).
// ---------------------------------------------------------------------------
static const int ALTURA_BARRA = 64;
static const int ALTURA_NAV   = 72;   // was 96 - too tall with the smooth font

// VERTICAL TOUCH CORRECTION (the finger lands below where the eye aims) - the
// why is in shell.cpp, next to tratar_toque(). It lives here because BOTH
// touch paths need it: the click (shell.cpp) and level touch
// (core.cpp). Before, only the click corrected, and the virtual keyboard - which picks the
// key by level - hit 10 px below where the same finger hit on a button.
static const int TOQUE_DY = 10;

// ---------------------------------------------------------------------------
// BOOT ANIMATION (kernel/boot_anim.hpp) - the web with the name decoded.
// Comment out the line below to remove the animation while developing: whoever
// restarts the device all the time does not need to watch it every time.
// ---------------------------------------------------------------------------
#define TAB5_MOSTRAR_ANIM_BOOT

// ---------------------------------------------------------------------------
// SHARED STATE - defined in core.cpp, read (never written) by
// shell.cpp.
// ---------------------------------------------------------------------------
extern const app_t* s_app;             // NULL = home screen
extern const app_t* s_app_carregando;  // app with an SD mount pending
extern const app_t* s_app_recusado;    // app the system did not open (reason screen in its place)
extern rolagem_t    s_rol;             // scroll of the open app - mechanism in kernel/rolagem.hpp
extern bool         s_sujo_layout;     // the SCREEN changed - requests a full repaint
extern bool         s_sujo_dados;      // only a VALUE changed - requests an incremental repaint
extern bool         s_nav_travada;     // navigation locked (keyboard/capture on screen)
extern bool         s_bloqueado;       // lock screen in front of everything (home or app stay underneath)
extern bool         s_painel_avisos;   // notification centre open over the app/home

// THE APP IS HIDDEN behind something of the system's (lock screen or notification
// centre). While true it receives no touch, key or notice - it is not
// visible, and an app that "reads" what it shows (conversation) would mark as read
// what nobody saw. ONE rule only, so as not to forget one of the two on any
// of the input paths.
static inline bool sistema_app_encoberto(void) { return s_bloqueado || s_painel_avisos; }

// THERE IS AN APP REALLY OPEN: ao_abrir() has already run. Neither "abrindo..." (SD
// mounting) nor refused (system reason screen) - in those two the app has
// no state at all, and none of its functions may be called. ONE rule only,
// for all the paths that call the app.
static inline bool app_vivo(void) { return s_app && s_app != s_app_carregando && s_app != s_app_recusado; }

// ---------------------------------------------------------------------------
// FUNCTIONS IN core.cpp, called by shell.cpp (app life cycle).
// ---------------------------------------------------------------------------
// Is the caller running on the UI task?
bool sistema_na_interface(void);

// Changes the panel clock (destroys and recreates the pages) ON THE UI
// TASK - from another task, leaves the request and waits up to 5 s.
bool sistema_trocar_relogio_painel(int mhz);
// The same, for the DSI link speed (test only: does not save).
bool sistema_trocar_link_painel(int mbps);

// FULL PASS PER APP (open app, back to home), accumulated since
// boot, for the console's 'tela'. The three phases add up to the approximate total:
// status bar, clearing the area (+ navigation bar) and the content
// (the app's desenhar(), or home).
typedef struct {
  const char* nome;
  uint32_t n;
  uint32_t max_us;
  uint64_t us, us_barra, us_limpar, us_conteudo;
} sistema_passada_t;
int sistema_passadas_completas(sistema_passada_t* out, int max);

bool abrir_app(const app_t* app);
void fechar_app(void);

// ---------------------------------------------------------------------------
// FUNCTIONS IN shell.cpp, called by core.cpp (drawing and the main loop).
// ---------------------------------------------------------------------------
void tratar_toque(int tx, int ty);
void desenhar_barra_status(lgfx::LovyanGFX& d, bool completo);
bool recado_pendente(void);   // the bar message changed (sistema_recado): repaint now
bool barra_mudou(void);
void desenhar_barra_nav(lgfx::LovyanGFX& d);
void desenhar_home(lgfx::LovyanGFX& d, const area_t& a);
// HOME PAGES. How many rows of icons fit per page; with more apps
// than that, home gets dots at the bottom and a sideways drag changes page.
// 4 = all the area holds today (12 apps). Lower it to 3 to test the
// second page with the apps that already exist.
#define TAB5_HOME_LINHAS_MAX 4

// GESTURES: called on finger release with the total displacement (dx, dy) since
// it went down, with the finger that woke the screen and the ordinary click already discarded. On home
// changes page; with the lock screen on, dragging up unlocks.
void tratar_gesto(int dx, int dy, int y0);   // y0: where the finger went down (raw)

// MOVING AN ICON ON HOME. home_segurar() is called on every turn with the finger on the
// screen (home open, not locked): after HOME_SEGURAR_MS still over an
// icon it "lifts" and starts following the finger; returns true while there is an icon
// lifted, and then the touch becomes neither a click nor a gesture. home_soltar() on
// release saves the new order. home_ordem_carregar() once, before the loop.
bool home_segurar(int tx, int ty, uint32_t agora, uint32_t segurando_ms, int desvio);
bool home_soltar(void);
void home_ordem_carregar(void);

// LOCK SCREEN. desenhar_bloqueio() paints the whole usable area (full) or only
// what changed since last time (clock, notice); the status bar on top
// stays as usual. bloquear_tela()/desbloquear_tela() only change the state
// and request a full repaint.
void desenhar_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool completo);
void bloquear_tela(void);
// THE SCREEN HAS JUST TURNED ON (boot or wake): the ONLY rule for when the
// lock screen appears. Setting on and not yet locked: lock;
// already locked: pick another animation. Whatever turns the screen on calls this, and only this.
void bloqueio_ao_ligar_tela(void);
void desbloquear_tela(void);

// NOTIFICATION CENTRE (shell.cpp). Tapping the status bar opens a list of
// ALL active sources (notif_listar), one line per app, over the usable
// area; tapping a line opens its app, tapping outside (or the bar again)
// closes it. painel_avisos_mudou() tells the loop the open list has aged.
void painel_avisos_abrir(bool abrir);
bool painel_avisos_mudou(void);
bool painel_avisos_dados_mudou(void);   // only the mini player (data pass)
void desenhar_painel_avisos(lgfx::LovyanGFX& d, const area_t& a, bool completo);

void atualizar_linha_servicos(void);   // checks and redraws home's GROVE/CARTAO strip, if it changed

// Touch diagnostics turned on by the console ('toque') - kernel/shell.cpp.
bool diag_toque_ligado(void);
