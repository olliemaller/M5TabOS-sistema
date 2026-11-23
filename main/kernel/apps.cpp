// THE APPLICATION TABLE.
//
// Nobody edits this file. The fixed list comes from apps_lista.inc, which
// main/CMakeLists.txt generates on every build from the main/app_<nome>/ folders:
// each one becomes a line APP(APP_<NOME>). Creating an app means creating the folder and
// defining 'extern const app_t APP_<NOME>' in it; removing it means deleting the folder.
//
// After the fixed ones, boot adds the apps read from the card (the Berry
// scripts, services/berry_apps) with sistema_apps_registrar() - before the home screen
// builds the icon order. After boot, only the UI touches it (the card's apps
// reread when returning from USB flash drive mode).

#include "app.h"

#define APP(simbolo) extern const app_t simbolo;
#include "apps_lista.inc"
#undef APP

static const app_t* const FIXOS[] = {
#define APP(simbolo) &simbolo,
#include "apps_lista.inc"
#undef APP
};

const app_t* SISTEMA_APPS[SISTEMA_APPS_MAX] = {
#define APP(simbolo) &simbolo,
#include "apps_lista.inc"
#undef APP
};

int SISTEMA_NUM_APPS = sizeof(FIXOS) / sizeof(FIXOS[0]);

bool sistema_apps_registrar(const app_t* app)
{
  if (!app || SISTEMA_NUM_APPS >= SISTEMA_APPS_MAX) { return false; }
  SISTEMA_APPS[SISTEMA_NUM_APPS++] = app;
  return true;
}

void sistema_apps_truncar(int n)
{
  const int fixos = sizeof(FIXOS) / sizeof(FIXOS[0]);
  if (n < fixos) { n = fixos; }
  if (n < SISTEMA_NUM_APPS) { SISTEMA_NUM_APPS = n; }
}
