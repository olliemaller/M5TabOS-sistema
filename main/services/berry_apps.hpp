// BERRY APPS - a .be file on the card becomes an app on the home screen.
//
// WHERE: /sdcard/apps/Berry/<nome>.be (the folder is created at boot). Read at
// boot and again when the card comes back from flash-drive mode (or 'berry reler' on the
// console): the new app appears on the home screen without restarting.
//
// THE HEADER (optional, in the file's first lines):
//     # nome: Contador          the name under the icon (default: the file name)
//     # cor: 0x07FF             the icon colour, RGB565 (default: cyan)
//     # periodo: 1000           redraw by itself every N ms (default: 0, only on request)
//     # avisos: espnow, mesh      redraw AT ONCE when the service publishes (and aviso(nome))
//
// THE FUNCTIONS THE SYSTEM CALLS (all optional):
//     abrir()                   the app opened (after the code outside the functions has run)
//     desenhar(tudo)            draw the whole screen: the area arrives clean (black) and the
//                               frame goes live in one go, without flicker. With
//                               sistema.acumular(true) it does NOT arrive clean: tudo =
//                               false asks only for what changed (layers: tela.camada)
//     tocar(x, y)               click, relative to the app's area
//     dedo(x, y, pressionado)   the finger by level (drag), and once on lifting
//     tecla(texto, codigo)      physical keyboard OR the system's virtual one (teclado.abrir)
//     aviso(nome)               a service from '# avisos:' published ("espnow", "mesh", "gps")
//     fechar()                  the window is about to close
//     fundo()                   the BACKGROUND PART: with sistema.fundo(true [, ms]) the system
//                               calls it every ms (default 1000), with the window open or
//                               closed - the same machine, the same variables. No screen,
//                               keyboard, camera or beep; 200 ms deadline per call
//
// THE MODULES (see docs/BERRY.md): 'tela' (drawing, only inside desenhar),
// 'sistema' (redraw, period, time, power, IMU, GPS, network, notice, beep...),
// 'teclado' (the system's virtual one), 'cartao' (files in the app's folder),
// 'espnow', 'mesh', 'camera' and 'gnss' (the GPS satellites). The list is also in testes/checar_be.c and
// testes/berry_falso.be - change it here, change it there.
//
// EACH APP HAS ITS OWN MACHINE: created on opening, deleted on closing - one app does not
// see another's variables, and closing gives back all the memory. With the background
// running, closing the window does NOT delete it: the machine lives until the background stops (and
// garbage collection runs at the moment the window closes). Everything runs on the
// Berry task (berry_vm.hpp), with a deadline: an infinite loop becomes an error on the
// app's screen, it does not hang the device.
#pragma once

#include "app.h"

extern "C" {
#include "berry.h"
}

#define BERRY_APPS_MAX 16
#define BERRY_PASTA    "Berry"   // /sdcard/apps/Berry

// Reads the folder and registers each .be as an app. At boot, after tab5_sd_iniciar
// and before the home screen builds the icon order.
void berry_apps_iniciar(void);

// Rereads the folder. From any task EXCEPT the UI (it reads the card); swapping
// the icons happens later, in berry_apps_atender. With no card, nothing changes.
void berry_apps_reler(void);

// In the UI, with the home screen live: applies the last reread. true =
// the list changed (the caller rebuilds the icon order and redraws).
bool berry_apps_atender(void);

// Does the app_t belong to a Berry app? (the home screen only offers Remover for these)
bool berry_apps_eh_meu(const app_t* app);

// REMOVES the app: in the background, stops its background part, sends the .be and the
// data folder (apps/Berry/<nome>/) to the card's recycle bin - recoverable
// via Files until emptied - and rereads the folder. From the UI (does not block).
void berry_apps_remover(const app_t* app);

// Console ('berry tempos'): how much handing off to the Berry task costs and how long
// the script takes for each event. 'zerar' restarts the count.
void berry_apps_tempos(bool zerar);

// Console ('berry mem'): what each live machine uses (apps with a window or a
// background, and the console's, if any), the layers, and the total against
// Berry's budget (berry_mem.h).
void berry_apps_mem(bvm* console);

// Puts the 'tela' and 'sistema' modules into a new machine (berry_vm_nova calls it).
void berry_modulos_registrar(bvm* vm);
