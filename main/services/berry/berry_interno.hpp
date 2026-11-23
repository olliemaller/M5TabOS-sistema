// What the Berry modules (berry_mod_*.cpp) and the engine (berry_apps.cpp)
// share: the running script's context, the argument and reply
// helpers, and the functions the engine calls inside the modules. For them only:
// no app includes this header - apps talk to Berry through berry_apps.hpp.
//
// WARNING: include this header LAST in each .cpp. It defines the macro 'C'
// (the context), and such a short name would break any header included afterwards.
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "berry_apps.hpp"
#include "berry_vm.hpp"      // berry_prazo_conferir (so_janela)
#include "teclado_virtual.hpp"

// ---------------------------------------------------------------------------
// EACH APP'S STATE
// ---------------------------------------------------------------------------
typedef struct {
  char  nome[24];
  char  arquivo[96];       // full path of the .be
  bvm*  vm;                // nullptr = closed
  char  erro[160];         // the script's last error ("" = none): shown on screen
  int   notif;             // notice source (sistema.avisar), -1 = not yet
  bool  usa_imu;           // the previous frame called sistema.imu()/giro(): read the sensor before this one
  uint8_t funcs;           // F_*: the functions the script defined (seen on opening)
  char  pasta[96];         // the app's folder on the card, apps/Berry/<nome> ("" = not yet created in this session)
  bool  dedo_antes;        // there was a finger in the previous sample (to deliver the release)
  uint32_t periodo_cab;    // the header's '# periodo:'
  uint32_t periodo;        // the current one (sistema.periodo changes it)
  bool  acumular;          // sistema.acumular(true): the frame does NOT arrive clean (desenhar(tudo=false))
  lgfx::LGFX_Sprite* camadas[8];   // tela.camada: stored images (nullptr = free slot)
  // THE BACKGROUND PART (sistema.fundo): the script keeps running fundo() with the
  // window closed - the SAME machine, so window and background see the same
  // variables. The kernel runs the loop (kernel/fundo.h).
  bool     janela;         // the window is open (from abrir to fechar)
  uint32_t fundo_ms;       // the requested interval between two fundo() calls
  int      fundo_pedido;   // FP_*: the script asked to turn it on/off (the UI applies it)
  volatile bool fundo_sair;   // fundo() itself asked to stop, or it errored
  char     fundo_txt[40];  // sistema.fundo_estado(texto): the chip on the bar
  int      fundo_n;
  fundo_def_t fundo_def;   // the description for the kernel (name = the app's)
} berry_app_t;

// The request to turn the background on/off (sistema.fundo), applied by the UI.
enum { FP_NADA, FP_LIGAR, FP_DESLIGAR };

// Timing: count, sum and worst (see 'berry tempos').
typedef struct { uint32_t n, max_us; uint64_t soma_us; } tempo_t;

// THE CONTEXT OF A JOB - who runs and where it draws. Whoever requests builds
// their own (the UI in pedir(), the background in its loop) and points s_ctx at it only
// during the job; the natives read it through C. Outside an app job (the
// console) C is the empty context: no app, no screen.
//
// WHY NOT LOOSE VARIABLES: the running app and where it draws belong to the
// JOB, not the module. Today there is just one Berry task (one job at a
// time, s_ctx is enough); to run machines on different tasks, only s_ctx
// needs to become per-task - the rest no longer assumes a single app.
typedef struct {
  berry_app_t*      app;      // nullptr = the console
  bool              fundo;    // background-part job: no screen, keyboard, camera or beep
  lgfx::LovyanGFX*  d;        // where 'tela' draws now (the screen, or the target layer)
  area_t            a;        // the area of 'd' (on a layer: 0, 0, width, height)
  // The REAL SCREEN, kept apart from the target: tela.alvo(c) diverts d/a to the
  // layer, tela.alvo() goes back to these (nullptr outside desenhar()).
  lgfx::LovyanGFX*  tela_d;
  area_t            tela_a;
  int               alvo;     // 0 = the screen; N = layer N
} contexto_t;

extern contexto_t* s_ctx;   // defined in berry_apps.cpp
#define C s_ctx

// A module member: native function (fn) or integer constant (valor).
// Each table ends with { nullptr }.
typedef struct { const char* nome; bntvfunc fn; int valor; } membro_t;

// The module tables, registered by berry_modulos_registrar (berry_apps.cpp).
extern const membro_t BERRY_MOD_TELA[];
extern const membro_t BERRY_MOD_SISTEMA[];
extern const membro_t BERRY_MOD_TECLADO[];
extern const membro_t BERRY_MOD_CARTAO[];
extern const membro_t BERRY_MOD_ARQUIVOS[];
extern const membro_t BERRY_MOD_ESPNOW[];
extern const membro_t BERRY_MOD_MESH[];
extern const membro_t BERRY_MOD_CAMERA[];
extern const membro_t BERRY_MOD_GNSS[];
extern const membro_t BERRY_MOD_MUSICA[];
extern const membro_t BERRY_MOD_IDIOMA[];
extern const membro_t BERRY_MOD_TRANSFERENCIA[];
extern const membro_t BERRY_MOD_REDE[];
extern const membro_t BERRY_MOD_AJUSTES[];
extern const membro_t BERRY_MOD_TEMA[];

// ---------------------------------------------------------------------------
// MODULE HELPERS
// ---------------------------------------------------------------------------
static inline int arg_int(bvm* vm, int i, int padrao)
{
  return (be_top(vm) >= i && be_isnumber(vm, i)) ? (int)be_toint(vm, i) : padrao;
}

// An UNSIGNED 32-bit number (Meshtastic node number). Berry's integer
// here is SIGNED 32-bit: a node above 2^31 arrives negative (which is what
// mesh.no()/meu() return) or, typed out in full (4294967295), becomes a REAL -
// and casting an out-of-range real to int is undefined (out came some
// arbitrary number, which slipped past the checks). A real outside 0..2^32-1: 'padrao'.
static inline uint32_t arg_u32(bvm* vm, int i, uint32_t padrao)
{
  if (be_top(vm) < i) { return padrao; }
  if (be_isint(vm, i)) { return (uint32_t)be_toint(vm, i); }
  if (be_isreal(vm, i)) {
    const double v = (double)be_toreal(vm, i);
    return (v >= 0.0 && v <= 4294967295.0) ? (uint32_t)v : padrao;
  }
  return padrao;
}

static inline const char* arg_txt(bvm* vm, int i)
{
  return (be_top(vm) >= i) ? be_tostring(vm, i) : "";
}

// Building a map/list to return to the script. mapa_novo/lista_nova leave TWO
// values on the stack (the object and its data); fim() removes the data and the object remains.
static inline void mapa_novo(bvm* vm)  { be_newobject(vm, "map"); }
static inline void lista_nova(bvm* vm) { be_newobject(vm, "list"); }
static inline void fim(bvm* vm)        { be_pop(vm, 1); }
static inline void por(bvm* vm) { be_data_insert(vm, -3); be_pop(vm, 2); }
static inline void m_int(bvm* vm, const char* k, int v)          { be_pushstring(vm, k); be_pushint(vm, v); por(vm); }
static inline void m_real(bvm* vm, const char* k, double v)      { be_pushstring(vm, k); be_pushreal(vm, (breal)v); por(vm); }
static inline void m_txt(bvm* vm, const char* k, const char* v)  { be_pushstring(vm, k); be_pushstring(vm, v ? v : ""); por(vm); }
static inline void m_bool(bvm* vm, const char* k, bool v)        { be_pushstring(vm, k); be_pushbool(vm, v); por(vm); }
static inline void l_real(bvm* vm, double v)      { be_pushreal(vm, (breal)v); be_data_push(vm, -2); be_pop(vm, 1); }
static inline void l_int(bvm* vm, int v)          { be_pushint(vm, v); be_data_push(vm, -2); be_pop(vm, 1); }
static inline void l_txt(bvm* vm, const char* v)  { be_pushstring(vm, v); be_data_push(vm, -2); be_pop(vm, 1); }

// "AA:BB:CC:DD:EE:FF" - the MAC as text (ESP-NOW and the network show it the same way).
static inline void mac_txt(const uint8_t mac[6], char out[18])
{
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// What belongs to the WINDOW (screen, layers, keyboard, camera, beep...) in fundo(): a clear
// error - the background runs with the window closed, there is nowhere to draw or touch.
static inline bool so_janela(bvm* vm, const char* o_que)
{
  berry_prazo_conferir(vm);   // every drawing costs ms: the deadline applies here too
  if (!C->fundo) { return true; }
  char msg[96];
  snprintf(msg, sizeof(msg), "%s e da janela: nao pode ser usado dentro de fundo()", o_que);
  be_raise(vm, "fundo_error", msg);
  return false;
}

// 'tela' outside desenhar(): a clear error instead of drawing nowhere.
static inline bool exige_tela(bvm* vm)
{
  if (!so_janela(vm, "a tela")) { return false; }
  if (C->d) { return true; }
  be_raise(vm, "tela_error", "a tela so pode ser usada dentro de desenhar() (ou numa camada: tela.alvo)");
  return false;
}

// The folder (relative to /sdcard/apps) for an app's data: Berry/<nome>.
static inline void pasta_rel(const char* nome, char* out, size_t tam)
{
  snprintf(out, tam, "%s/%s", BERRY_PASTA, nome);
}

// ---------------------------------------------------------------------------
// WHAT BELONGS TO THE WINDOW (beep, keyboard, screen kept on, camera): there is ONE window on
// screen, so a single state - and only window jobs touch it (the natives
// refuse in the background, see so_janela). The modules only REQUEST; the
// UI applies it, in pedir() (berry_apps.cpp), once the job comes back.
// ---------------------------------------------------------------------------
// The beep requested by the script (sistema.bip). Defined in berry_mod_sistema.cpp.
extern float    s_bip_hz;
extern uint32_t s_bip_ms;

// Keep the screen on (sistema.acordado): -1 = nothing requested. Likewise.
extern int s_acordado_pedido;

// THE SYSTEM'S ON-SCREEN KEYBOARD (just one: only one Berry app is open at a time).
// Defined in berry_mod_tela.cpp.
extern tecvirt_t s_kb;
enum { KB_NADA, KB_ABRIR, KB_FECHAR };
extern int     s_kb_pedido;
extern uint8_t s_kb_tipo;   // the field type of the last teclado.abrir(tipo)

// ---------------------------------------------------------------------------
// WHAT THE ENGINE CALLS INSIDE THE MODULES
// ---------------------------------------------------------------------------
// tela (berry_mod_tela.cpp)
int  altura_livre(void);                // the height left for the app (the open keyboard eats the bottom)
void alvo_tela(void);                   // drawing goes back to the screen
void camadas_liberar(berry_app_t* a);   // the window's layers are returned on closing

// WHAT EACH DRAWING COSTS ('berry tempos'): the time inside each 'tela' function.
enum { P_LIMPAR, P_RETANGULO, P_CIRCULO, P_LINHA, P_TEXTO, P_MEDIR, P_ARREDONDADO, P_TRIANGULO, P_PIXEL, P_BOTAO, P_COLAR, P_N };
extern const char* const P_NOME[P_N];
extern tempo_t s_prim[P_N];

// gnss (berry_mod_gnss.cpp): releases the GPS subscription (the app's, or any one if a == nullptr)
void gnss_soltar(const berry_app_t* a);

// rede (berry_mod_rede.cpp): the app closed - cancels its request and frees the reply
void rede_soltar(const berry_app_t* a);

// camera (berry_mod_camera.cpp)
bool cam_no_ar(void);                     // the preview is on
void cam_aplicar(void);                   // applies what the script asked for (UI only; nothing requested = nothing to do)
void cam_quadro(const berry_app_t* app);  // on every desenhar, before the script
void cam_limpar_fora(const area_t& a);    // the area without the preview strip
void cam_fechar(void);                    // the app closed: everything comes down

// ajustes (berry_mod_ajustes.cpp): volume([v]) 0..100 - musica.volume uses the same
int berry_volume(bvm* vm);
