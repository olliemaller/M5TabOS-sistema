// THE TERMINAL IN THE PC'S BROWSER - the SAME Terminal as on the device, in the
// /terminal/ part of the web server (tab5_web), on the same pattern as Editor and Mesh.
//
// As in the Terminal app, two things take turns on the same screen:
//   - The system COMMANDS (services/tab5_terminal: the same session as the
//     Tab5 screen - what is typed in one appears in the other). Plain HTTP:
//       GET  api/console?v=<versao>&desde=<linha>  what changed (JSON)
//       POST api/enviar   (body = the line)         runs / answers the prompt
//       POST api/parar                              Ctrl+C
//       POST api/limpar                             Ctrl+L
//   - The shared SSH SESSION (tab5_ssh), which the 'ssh usuario@host' command
//     opens. xterm.js comes from FLASH (services/xterm/*.gz): works with the PC
//     offline. One WebSocket connection (ws) carries everything:
//       Tab5 -> browser    binary = what the SSH server sent (raw, for
//                          xterm.js); text = {"t":"estado",...} when the
//                          state or size changes. On joining, a snapshot of
//                          the current screen.
//       browser -> Tab5    binary = keys; text = {"t":"ola"|"tamanho","c","l"},
//                          {"t":"senha","s"}, {"t":"confirmar","sim"}, {"t":"fechar"}.
// The page shows SSH while the session is up and goes back to commands when
// it ends - the same rule as the Tab5 screen. Closing the tab does not end
// anything: the command and the session belong to the services.
#pragma once

// Opens the Terminal part of the server (/terminal/) and prepares the command
// session. From any task (the portal calls it from within a request).
// false = see tab5_web_abrir.
bool tab5_terminal_web_ligar(void);
