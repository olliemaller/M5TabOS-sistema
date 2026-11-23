// SYSTEM WEB SERVER - a port opened only on purpose, with a password.
//
// For what the PC does better in a browser: editing Berry scripts
// (services/berry_web), chatting over Mesh (services/tab5_mesh_web), and
// later viewing the camera and uploading/downloading files.
//
// ONE SERVER, ONE FOLDER PER APP. Each app OPENS its own part with a prefix
// ("editor" -> http://<ip>/editor/, "mesh" -> /mesh/, "terminal" -> /terminal/)
// and its routes, relative to the folder ("/" is the page, "/api/x"). Several
// open at the same time, the same password for all. The ROOT (http://<ip>/) is the
// PORTAL (tab5_web_portal): the device overview and every app that has a
// PC page (app_t.no_pc), open or not - one click opens whatever is missing.
// A closed app's page goes back to the portal. The server starts when the first
// part opens and stops when the last one closes. Up to WEB_PARTES_MAX parts.
//
// THE DEFAULT IS CLOSED. No port is open at boot. A part opens
// when the person asks on the device and RUNS IN THE BACKGROUND - leaving the app,
// turning off or locking the screen does not close it. It closes:
//   - when the app closes its own (Turn off button);
//   - after WEB_OCIOSO_MS without any request TO IT (whoever uses the browser
//     saves automatically while editing: when it stops, what was done is already
//     saved) - an idle Mesh does not bring down an Editor in use.
// And the whole server stops if Wi-Fi drops or after WEB_ERROS_MAX
// wrong passwords.
// While on, it declares RECURSO_REDE (kernel/recursos.h): its policy
// forbids light sleep (with the screen off the C6's SDIO does not wake the P4, and the
// browser would be left waiting) and holds Wi-Fi against the doze (which used to
// turn it off after 3 min of screen off, taking the server down with it) - the screen may
// go off - and it publishes a status-bar notice with the open apps: the person always
// sees that a port is open, and tapping it opens the last app that opened.
//
// THE GATEKEEPER (every request, before the route):
//   1. LOCAL NETWORK only: the requester's IPv4 must be on the device's
//      subnet (or be IPv6 link-local);
//   2. the Host must be the device's IP (a domain name in Host is the
//      "DNS rebinding" trick of an outside page);
//   3. PASSWORD: Basic authentication - the browser shows its own dialog. Any
//      user name; the password is short, drawn at random on each start and shown on screen;
//   4. a request that changes something (POST) needs the X-Tab5 header: a page
//      from another site cannot send that header without CORS
//      permission, which the server never grants.
//
// MEMORY: the ESP-IDF httpd, ONE task (internal stack of WEB_PILHA, only
// while on - routes write to the card, and with the card lent out the
// write goes to flash: a stack in PSRAM would trigger the cache assert). Large
// bodies and responses live in the PSRAM of whoever handles the route.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_http_server.h"

#define WEB_OCIOSO_MS  (10 * 60 * 1000)   // no requests at all: turn off
#define WEB_ERROS_MAX  20
// 6 KB OVERFLOWED (28/10/2026: end-of-stack breakpoint in a snprintf, with
// Mesh reading the history from the card). Routes keep large buffers in
// PSRAM; this is the headroom for httpd, the card (FATFS) and printf.
#define WEB_PILHA      8192

#define WEB_PARTES_MAX 4

// A route: only runs after the gatekeeper (above). 'uri' is RELATIVE to the
// app's folder: "/" is its page, "/api/x" answers at /<prefixo>/api/x.
typedef struct {
  const char*    uri;      // exact, relative ("/api/scripts")
  httpd_method_t metodo;   // HTTP_GET, HTTP_POST
  esp_err_t    (*fn)(httpd_req_t* req);
  // WEBSOCKET (method HTTP_GET): the handshake goes through the gatekeeper (local
  // network, password, and the ORIGIN equal to the Tab5's address - WebSocket does
  // not carry X-Tab5) and 'fn' then receives the FRAMES of that connection
  // (httpd_ws_recv_frame). 'ws_fechou' (optional): the connection dropped or the part
  // closed. An open connection counts as use: the part does not time out.
  bool           ws;
  void         (*ws_fechou)(int fd);
} tab5_web_rota_t;

// OPENS the part of 'dono' (the app name, literal: the status-bar notice opens it)
// at /<prefixo>/ with these routes (the array must live until closing). Starts the
// server and draws the password if it is the first. Opening the same part again
// only renews the deadline. false = no Wi-Fi connected, no free slot, or httpd did not
// start. From any task, without blocking - from inside a request too
// (the portal opening an app: the server is already up).
bool tab5_web_abrir(const char* dono, const char* prefixo, const tab5_web_rota_t* rotas, int n);

// CLOSES the part of 'dono'. The last one to close turns the server off. From any
// task, without blocking. AVISO_WEB when it changes.
void tab5_web_fechar(const char* dono);

// TURNS OFF everything (all parts). From any task, without blocking: new
// requests are refused at once and the real stop (which waits for the request
// in progress) runs on a pool slot.
void tab5_web_desligar(void);

bool tab5_web_ligado(void);
bool tab5_web_aberto(const char* dono);       // is this app's part open?
// The open apps (names, in the order they opened). Returns how many.
int  tab5_web_abertos(const char** out, int max);
// "http://192.168.1.7/editor/" (dono = nullptr: the root). false = no IP.
bool tab5_web_endereco(const char* dono, char* out, size_t n);
// The folder of the open part of 'dono' ("terminal"). false = not open.
bool tab5_web_pasta(const char* dono, char* out, size_t n);

// THE CATALOGUE (tab5_web_portal): the apps of this variant with a PC page
// (app_t.no_pc), in table order. Returns how many.
int  tab5_web_catalogo(const char** out, int max);
// Opens the app's part by name ("Terminal"), via the catalogue - the portal and the
// console 'web' command. false = there is no such app on the PC, or it did not open.
bool tab5_web_abrir_app(const char* nome);
void tab5_web_senha(char* out, size_t n);     // "" = off

// The last accepted request (from any part): from whom (IP as text) and when
// (tab5_agora_ms). 'quem' "" = nobody yet.
void tab5_web_ultimo(char* quem, size_t n, uint32_t* quando_ms);

// ---- for the routes ---------------------------------------------------------
// WEBSOCKET: sends a frame (text or binary) to connection 'fd', from
// ANY task - waits for the server to send it. false = the connection has already dropped.
bool tab5_web_ws_enviar(int fd, const void* d, size_t n, bool texto);

// The whole body in 'buf' (with '\0'). Returns the size; -1 = larger than
// tam - 1 (the route has already answered 413) or the connection dropped.
int tab5_web_corpo(httpd_req_t* req, char* buf, size_t tam);

// A URL parameter (?arq=Ola.be), already decoded (%20, +). false = not present.
bool tab5_web_param(httpd_req_t* req, const char* nome, char* out, size_t n);

// JSON response (correct Content-Type, no cache).
esp_err_t tab5_web_json(httpd_req_t* req, const char* json);

// Text -> quoted JSON string ("a\"b\n" -> "\"a\\\"b\\n\""). Truncates at
// the size of 'out', without breaking an escape.
void tab5_web_json_texto(char* out, size_t n, const char* s);
