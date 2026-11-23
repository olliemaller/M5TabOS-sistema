// THE PORTAL - the root of the web server (http://<ip>/), the Tab5's
// "desktop" in the PC's browser.
//
// A single page (tab5_web_portal.html, from flash): the sidebar with the apps
// that have a PC page (app_t.no_pc - open and closed) and the device
// overview (battery, network, memory, uptime). Choosing an app
// shows its page right there, without leaving the portal (the address keeps the
// choice: http://<ip>/#Terminal); a closed app opens with one click - the
// password has already been given, and whoever has it has seen the Tab5's screen.
//
// The routes belong to the ROOT, not to a part: the server (tab5_web) dispatches
// them before the parts, with the same gatekeeper.
//   GET  /               the page
//   GET  /api/sistema    the overview and the apps (JSON)
//   POST /api/abrir?app=Terminal   opens the app's part (via the catalogue)
//   POST /api/desligar   turns the whole server off
#pragma once

#include "tab5_web.hpp"

// The root routes (the array lives forever). 'n' = how many.
const tab5_web_rota_t* tab5_web_portal_rotas(int* n);
