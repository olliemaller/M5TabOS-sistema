// MESH IN THE PC'S BROWSER - the web server (tab5_web) routes for chatting
// over the mesh: channels and direct conversations on the left, messages
// on the right, a box to write in. The page (tab5_mesh_web.html, in flash)
// polls /api/estado every 2 s and only re-reads what changed.
//
// Only an interface over the tab5_mesh service: conversations, history (from
// the SD card, by key), sending and delivery state are the service's - the
// same as the Mesh app shows. Reading a conversation on the PC marks it read, as in the app.
//
// The routes, at http://<ip>/mesh/ (conversation = c=1&idx=<channel> or no=<number>):
//   GET  /                   the page
//   GET  /api/estado         {"eu","eu_nome","no_ar","conv","msgs","envios","nao_lidas"}
//   GET  /api/conversas      [{"canal","idx","no","nome","sub","nao_lidas","ultima"}]
//   GET  /api/canais         [{"idx","nome","visivel"}]
//   GET  /api/nos            [{"num","id","nome","longo","hw","visto","idade","saltos","snr10",
//                              "bat","mv","mqtt","fav","lat","lon"}]   (lat/lon: graus x 1e7)
//   GET  /api/msgs?conversa  [{"de","nome","texto","quando","minha","envio","erro","mqtt","saltos","snr10","rssi"}]
//   GET  /api/saude          {"uso_canal10","airtime10","ruido","online","total","tx","rx",...,"enlace"}
//   POST /api/enviar?conversa   body = the text (up to MESH_PAYLOAD_MAX bytes)
//   POST /api/favorito?no=N&sim=1|0
// Numbers with decimals go as integers (snr10 = SNR x 10): no %f, which is
// what uses the most printf stack (the httpd one has overflowed once already).
#pragma once

// Opens the Mesh part of the server (/mesh/). From the interface task.
// false = see tab5_web_abrir.
bool tab5_mesh_web_ligar(void);
