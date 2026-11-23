// SSH (client) - log into another machine from the Tab5: run a command there and see the
// output, or a line-by-line shell. Engine: libssh2 1.11 with the system
// mbedTLS (components/libssh2).
//
// What already works: password (and "keyboard-interactive", which many servers use
// for the password), stdout and stderr from the other side, the command's exit code, the
// server key fingerprint (SHA256, the same as the PC's 'ssh').
//
// KNOWN SERVERS (like the PC's known_hosts): on the first connection the
// fingerprint is saved in /sdcard/apps/SSH/known_hosts; on later ones
// it has to match. CHANGED = refuse before asking for the password (it could be another
// device in the server's place); tab5_ssh_esquecer() removes the line when the
// change was deliberate (server reinstalled).
//
// Two ways to use it:
//   tab5_ssh_rodar    one command, or a "dumb" line-by-line shell (console,
//                     the Terminal's 'ssh' command);
//   THE SESSION (below) a REAL terminal (xterm-256color: nano, top, vim)
//                     that lives in the service and is SHARED: the Terminal app and
//                     the PC's browser (services/tab5_terminal_web) come and go,
//                     and the session carries on - start on the PC, continue on the device.
// Not yet: private key.
//
// WHO CALLS: blocks (network and crypto) - never from the interface task.
// Memory: libssh2's allocations go to PSRAM; the system mbedTLS already
// allocates there. Stack: the pool's 6 KB slot is enough (the Terminal runs in it).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
  const char* usuario;
  const char* host;
  int         porta;      // 0 = 22
  const char* comando;    // nullptr or "" = line-by-line shell
  // Asks the user for a line - the password (hidden) and, in the shell, what to type.
  // Same contract as console_ler_linha: 1 = line, 0 = not yet, -1 = stop.
  int  (*ler_linha)(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms);
  // What arrives from the other side (stdout and stderr) and the client's messages.
  void (*saida)(const char* d, int n, void* ctx);
  void* ctx;
  bool (*parar)(void);    // Ctrl+C; nullptr = never
} tab5_ssh_t;

// Connects, authenticates, runs and disconnects. Returns the remote command's exit
// code (0 = success; in the shell, that of 'exit'), or -1 with the reason in 'erro'.
int tab5_ssh_rodar(const tab5_ssh_t* s, char* erro, size_t erro_n);

// Forgets the stored fingerprint for host:port. false = there was none.
// Touches the SD card: outside the interface.
bool tab5_ssh_esquecer(const char* host, int porta);

// ===========================================================================
// THE INTERACTIVE SESSION - a real terminal, shared
// ===========================================================================
// ONE at a time, in a pool slot (6 KB): the app and the page are SPECTATORS.
//   - the SCREEN lives here, in an emulator (components/libvterm): the Terminal app reads
//     the cells (tab5_ssh_sessao_celulas) and draws;
//   - the browser gets the RAW STREAM (xterm.js), by subscription, and on joining
//     a snapshot of the current screen as escape sequences (tab5_ssh_sessao_retrato);
//   - the SIZE (columns x rows) is that of whoever typed last, as in
//     tmux: each spectator sends its own with tab5_ssh_sessao_tamanho.
// The connection's questions (new server: accept the fingerprint? the
// password) sit in the state, and any spectator can answer.
// All safe from any task and non-blocking (writes go through a queue);
// subscription callbacks run on the session task and must not take long.
typedef enum {
  TAB5_SSH_LIVRE = 0,     // no session
  TAB5_SSH_CONECTANDO,
  TAB5_SSH_CONFIRMAR,     // new server: waiting for the fingerprint to be accepted
  TAB5_SSH_SENHA,         // waiting for the password
  TAB5_SSH_ABERTA,
  TAB5_SSH_FIM,           // finished (msg says why); opening again starts another
} tab5_ssh_estado_t;

typedef struct {
  tab5_ssh_estado_t estado;
  char     usuario[32], host[64];
  int      porta;
  char     impressao[64];   // SHA256:... (CONFIRMAR and ABERTA)
  char     msg[96];         // what happened last ("senha recusada", "a conexao caiu")
  int      cols, linhas;
  uint32_t versao;          // changes on every state change
  uint32_t tela;            // changes on every screen change (the app redraws)
} tab5_ssh_sessao_info_t;

// Opens (CONECTANDO). false = there is already a session, or the pool gave no task.
bool tab5_ssh_sessao_abrir(const char* usuario, const char* host, int porta, int cols, int linhas);
void tab5_ssh_sessao_info(tab5_ssh_sessao_info_t* out);
void tab5_ssh_sessao_confirmar(bool aceitar);   // in CONFIRMAR
void tab5_ssh_sessao_senha(const char* senha);  // in SENHA (copied and zeroed afterwards)
void tab5_ssh_sessao_escrever(const void* d, size_t n);   // keys (bytes for the PTY)
void tab5_ssh_sessao_tamanho(int cols, int linhas);
void tab5_ssh_sessao_fechar(void);              // ends it (any state)
void tab5_ssh_sessao_esquecer_fim(void);        // FIM -> LIVRE (whoever showed the message)

// The server's raw stream, for whoever has their own emulator (the browser).
typedef void (*tab5_ssh_saida_t)(const char* d, size_t n, void* ctx);
int  tab5_ssh_sessao_assinar(tab5_ssh_saida_t fn, void* ctx);   // id, or -1 (no slot)
void tab5_ssh_sessao_desassinar(int id);

// The current screen as escape sequences (clears, paints cell by cell with the
// colours, places the cursor): whoever joins midway sees what is there. Returns the
// bytes (0 = no open session). 'out' in PSRAM, 'max' of about 32 KB.
size_t tab5_ssh_sessao_retrato(char* out, size_t max);

// The SCREEN to draw (the Terminal app): one row of cells. Colours in
// RGB565, already resolved from the palette. false = row off screen or no session.
typedef struct {
  uint32_t cp;          // 0 = empty; the second half of a wide character is also 0
  uint16_t fg, bg;
  uint8_t  largura;     // 1 or 2
  bool     negrito, sublinhado;
} tab5_ssh_celula_t;
bool tab5_ssh_sessao_celulas(int linha, tab5_ssh_celula_t* out, int max);
// The cursor (row, column) and whether it is showing now.
void tab5_ssh_sessao_cursor(int* linha, int* coluna, bool* visivel);
