// THE TERMINAL'S SSH SCREEN - the shared SSH session (services/tab5_ssh) on the
// device. Interface only: the session, the screen (the emulator) and the connection belong to the
// service; here we draw the grid, ask what the connection requests and
// send what is typed. The same session appears in the PC browser
// (services/tab5_terminal_web): the "No PC" button opens the /terminal/ part.
//
// What opens the session is the 'ssh user@host' command typed in the Terminal (on the
// device or in the browser); the app shows this screen WHILE it is up
// (connecting, new server, password, open) and goes back to the commands when it
// ends - there is no switch button.
#pragma once

#include "app.h"

void ssh_tela_abrir(void);     // the screen came in (the session came up, or the app opened on it)
void ssh_tela_fechar(void);    // it left (the session ended or the app closed): the session carries on in the service
void ssh_tela_desenhar(lgfx::LovyanGFX& d, const area_t& a, bool tudo);   // 'a' = the app area
void ssh_tela_tocar(int x, int y);   // relative to the area
void ssh_tela_tocar_nivel(int x, int y, bool pressionado);
void ssh_tela_teclar(const char* txt, uint8_t codigo, uint8_t mod);
void ssh_tela_aviso(void);     // AVISO_SSH / AVISO_WEB: the screen or the state changed
