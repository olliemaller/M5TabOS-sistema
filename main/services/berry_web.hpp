// THE BERRY EDITOR IN THE PC'S BROWSER - the web server routes
// (tab5_web) for the scripts in apps/Berry.
//
// There is a single page (berry_web.html, in flash): the list of .be files on the left and
// a syntax-highlighting editor on the right - the editor (CodeMirror) is loaded by the
// PC's browser, from a CDN; with no internet on the PC, it becomes a plain
// text box. Save, Check (berry_verificar: the error comes back with the line
// marked) and Run (saves, checks and opens the app on the Tab5).
//
// The routes, at http://<ip>/editor/ (the Editor's part of the server):
//   GET  /                   the page
//   GET  /api/scripts        [{"nome":"Ola.be","bytes":123}, ...]
//   GET  /api/script?arq=    the script's text
//   POST /api/script?arq=    writes (body = the whole text); with &auto=1 (the
//                            page's autosave) it does not reread the folder
//   POST /api/verificar      {"ok":false,"linha":3,"erro":"..."}
//   POST /api/rodar?arq=     writes, checks and opens: {"ok":..., "aberto":...}
// The card is via berry_scripts (tab5_sd doors), on the server task.
#pragma once

// Opens the Editor's part of the server (/editor/). From the UI task.
// false = see tab5_web_abrir.
// The server runs in the background: closing the Editor does not turn it off (see tab5_web.hpp).
bool berry_web_ligar(void);
