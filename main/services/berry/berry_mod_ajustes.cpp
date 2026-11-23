// Berry module 'ajustes' - the person's preferences (services/ajustes.h).
// The script may change brightness and volume (it is what the top bar does); the rest is
// read-only: screen timeout, home position and time zone are decisions for whoever holds the
// device. See berry_apps.cpp (the engine) and berry_interno.hpp.

#include "ajustes.h"
#include "tab5_hora.hpp"
#include "berry_interno.hpp"

// brilho([pct]) - the screen brightness, 0..100 (the system minimum still applies:
// 0 does not turn the screen off). With an argument it applies at once and saves; returns the result.
static int aj_brilho(bvm* vm)
{
  if (be_top(vm) >= 1 && be_isnumber(vm, 1)) {
    int pct = arg_int(vm, 1, 50);
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    ajustes_definir_brilho_pct((uint8_t)pct);
  }
  be_pushint(vm, ajustes_brilho_pct());
  be_return(vm);
}

// volume([v]) - the system volume, 0..100 (the same as Settings). It is the same
// as musica.volume: one calculation for both.
int berry_volume(bvm* vm)
{
  if (be_top(vm) >= 1 && be_isnumber(vm, 1)) {
    int v = arg_int(vm, 1, 50);
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    ajustes_definir_volume((uint8_t)(v * AJUSTES_VOLUME_MAX / 100));
  }
  be_pushint(vm, (bint)(ajustes_volume() * 100 / AJUSTES_VOLUME_MAX));
  be_return(vm);
}

// tela_segundos() - how many seconds without touch until the screen goes off (read-only).
static int aj_tela_segundos(bvm* vm)
{
  be_pushint(vm, ajustes_tela_s());
  be_return(vm);
}

// casa() - [lat, lon] of the home position, or nil if the person has not set it (read-only).
static int aj_casa(bvm* vm)
{
  float lat, lon;
  if (!ajustes_casa(&lat, &lon)) { be_return_nil(vm); }
  lista_nova(vm);
  l_real(vm, lat);
  l_real(vm, lon);
  fim(vm);
  be_return(vm);
}

// fuso() - the time-zone offset in effect NOW, in minutes from UTC (-180 =
// UTC-3; daylight saving included when a region is chosen), read only.
static int aj_fuso(bvm* vm)
{
  be_pushint(vm, tab5_hora_fuso_s() / 60);
  be_return(vm);
}

// relogio_12h() - true when the user wants the 12-hour clock (9:05pm), read only.
static int aj_relogio_12h(bvm* vm)
{
  be_pushbool(vm, ajustes_relogio_12h());
  be_return(vm);
}

const membro_t BERRY_MOD_AJUSTES[] = {
  { "brilho", aj_brilho }, { "volume", berry_volume }, { "tela_segundos", aj_tela_segundos },
  { "casa", aj_casa }, { "fuso", aj_fuso }, { "relogio_12h", aj_relogio_12h },
  { nullptr },
};
