// Modulo Berry 'ajustes' - as preferencias da pessoa (services/ajustes.h).
// Brilho e volume o script pode mudar (e o que a barra de cima faz); o resto e
// so leitura: tempo da tela, casa e fuso sao decisoes de quem segura o
// aparelho. Ver berry_apps.cpp (o motor) e berry_interno.hpp.

#include "ajustes.h"
#include "tab5_hora.hpp"
#include "berry_interno.hpp"

// brilho([pct]) - o brilho da tela, 0..100 (o minimo do sistema segue valendo:
// 0 nao apaga a tela). Com argumento aplica na hora e grava; devolve como ficou.
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

// volume([v]) - o volume do sistema, 0..100 (o mesmo de Ajustes). E o mesmo
// do musica.volume: uma conta so para os dois.
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

// tela_segundos() - quantos segundos sem toque ate a tela apagar (so leitura).
static int aj_tela_segundos(bvm* vm)
{
  be_pushint(vm, ajustes_tela_s());
  be_return(vm);
}

// casa() - [lat, lon] da casa, ou nil se a pessoa nao configurou (so leitura).
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
