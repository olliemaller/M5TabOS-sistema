// THE BRIDGE between the core and the apps that are not compiled in (kernel/app.h,
// "APPS DE FORA", third-party apps). It is the ONLY file outside services/berry_* that knows about
// Berry: in the berry variant (M5_COM_BERRY=1) it wires up the .be apps on the card; in
// the full variant the functions do nothing, and Berry does not even go into the binary.
// See docs/VARIANTES.md.
#include "app.h"

#if M5_COM_BERRY
#include "berry_apps.hpp"

void sistema_apps_de_fora_iniciar(void) { berry_apps_iniciar(); }
bool sistema_apps_de_fora_atender(void) { return berry_apps_atender(); }
void sistema_apps_de_fora_reler(void)   { berry_apps_reler(); }
bool sistema_apps_de_fora_removivel(const app_t* app) { return berry_apps_eh_meu(app); }
void sistema_apps_de_fora_remover(const app_t* app)   { berry_apps_remover(app); }

#else

void sistema_apps_de_fora_iniciar(void) {}
bool sistema_apps_de_fora_atender(void) { return false; }
void sistema_apps_de_fora_reler(void)   {}
bool sistema_apps_de_fora_removivel(const app_t*) { return false; }
void sistema_apps_de_fora_remover(const app_t*)   {}

#endif
