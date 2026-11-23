// THE "NO PC" (PC ACCESS) PANEL - the web server's screen (services/tab5_web) in an app.
//
// Those that use the PC's browser (the Editor, Mesh) show the SAME thing: with the
// app's part closed, what it is and the Ligar (Turn on) button (and who else is open);
// open, the PORTAL address (http://<ip>/, which opens straight on this app), the
// password, who connected and Desligar (Turn off) - which closes only its own part. The app only gives the rectangle, who the owner is and how to open
// its routes.
//
// All on the UI task. Redraw: when painel_web_assinatura()
// changes (the app declares AVISO_WEB and compares in ao_aviso or in the drawing).
#pragma once

#include <M5Unified.h>
#include <stdint.h>

// Draws the panel in the rectangle (screen coordinates). Clears the rectangle.
void painel_web_desenhar(lgfx::LovyanGFX& d, int x, int y, int l, int a, const char* dono);

// Did the touch (tx, ty, same frame of reference as the rectangle) land on the button? Then
// opens (with 'ligar', the app's routes) or closes the part of 'dono', and reports
// via a bar message when it failed (no Wi-Fi, the server did not come up). true = it was the button.
bool painel_web_tocar(int x, int y, int l, int a, int tx, int ty, const char* dono, bool (*ligar)(void));

// Changes when what the panel shows changes (turned on, turned off, someone connected).
uint32_t painel_web_assinatura(void);
