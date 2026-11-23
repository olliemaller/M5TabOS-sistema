// NETWORK TOOLS - inspecting and querying the network: the device's address, DNS,
// the ARP table (and the local network scan), traceroute, TCP port test and
// HTTP/HTTPS. The Terminal shows them (commands ip, dns, arp, traceroute, tcp, http,
// busca in console.cpp); Berry and other apps use the same engine.
//
// Wi-Fi belongs to the C6 (esp_hosted), but the IP stack (lwIP) runs HERE on the P4: ARP,
// ICMP and sockets are local, and work on whatever network Wi-Fi
// is on (hal/tab5_wifi).
//
// WHO CALLS: everything blocks (network) - never from the interface task. The long
// loops (scan, traceroute, HTTP) check 'parar' on every round and
// deliver the result bit by bit via callback, so whoever displays it can show it as it comes.
// Memory: HTTP buffers in PSRAM (the system mbedTLS already allocates there).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TAB5_REDE_IP_TXT 46   // fits an IPv6 address as text

typedef bool (*tab5_rede_parar_t)(void);   // true = stop (Ctrl+C); nullptr = never

// ---- the device on the network ---------------------------------------------
typedef struct {
  bool     conectado;               // has IPv4
  char     ip[16], mascara[16], gateway[16];
  char     dns[2][TAB5_REDE_IP_TXT];
  char     ipv6[TAB5_REDE_IP_TXT];  // link-local, "" = none
  uint8_t  mac[6];
  char     nome[33];                // the hostname
} tab5_rede_ip_t;
bool tab5_rede_ip(tab5_rede_ip_t* out);   // false = no Wi-Fi interface

// ---- DNS ---------------------------------------------------------------------
// The addresses of 'nome' (IPv4 and IPv6). Returns how many (0 = did not resolve);
// 'ms' (may be nullptr) = how long it took.
int tab5_rede_dns(const char* nome, char ips[][TAB5_REDE_IP_TXT], int max, uint32_t* ms);

// ---- ARP ---------------------------------------------------------------------
typedef struct { char ip[16]; uint8_t mac[6]; } tab5_rede_vizinho_t;
// The ARP table now (who has talked to the device recently). Returns how many.
int tab5_rede_arp(tab5_rede_vizinho_t* out, int max);
// SCANS the local network (the device's /24): asks "who has IP x?" for each
// address and delivers whoever answered. About 15 s. Returns how many it found; -1 =
// no network.
typedef void (*tab5_rede_achou_t)(const tab5_rede_vizinho_t* v, void* ctx);
int tab5_rede_arp_varrer(tab5_rede_achou_t achou, void* ctx, tab5_rede_parar_t parar);

// ---- traceroute --------------------------------------------------------------
// One hop: ttl, who answered ("" = nobody) and the time of each probe (-1 =
// no answer). 'fim' = the target itself answered.
typedef struct { int ttl; char ip[16]; int ms[3]; bool fim; } tab5_rede_salto_t;
typedef void (*tab5_rede_salto_cb_t)(const tab5_rede_salto_t* s, void* ctx);
// Returns how many hops to the target (0 = did not reach; -1 = did not resolve/no network).
int tab5_rede_traceroute(const char* alvo, int max_saltos, tab5_rede_salto_cb_t cb, void* ctx,
                         tab5_rede_parar_t parar);

// ---- TCP ---------------------------------------------------------------------
// Connects with a timeout and returns the socket ALREADY CONNECTED, in blocking mode (the
// user closes it with close()). <0 = failed: -1 did not resolve, -errno from the socket.
int tab5_rede_conectar(const char* host, int porta, uint32_t timeout_ms, uint32_t* ms);
// Just opens and closes: is the port open? 0 = connected; <0 as above.
int tab5_rede_tcp(const char* host, int porta, uint32_t timeout_ms, uint32_t* ms);

// ---- HTTP / HTTPS ------------------------------------------------------------
// GET (or HEAD) on 'url', following redirects (up to 5). The body arrives
// in chunks in 'dados' (may be nullptr); returning false stops reading.
// HTTPS via the system certificate bundle.
typedef struct {
  int      status;          // 200, 404... 0 = did not connect
  int64_t  tamanho;         // Content-Length; -1 = not given
  int64_t  recebido;        // body bytes delivered
  char     tipo[64];        // Content-Type
  char     final[192];      // the URL after redirects
  int      redirecionou;    // how many were followed (0 = the requested URL answered)
  char     erro[64];        // the reason, if status == 0
  uint32_t ms;
} tab5_rede_http_t;
typedef bool (*tab5_rede_dados_t)(const char* d, int n, void* ctx);
bool tab5_rede_http(const char* url, bool so_cabecalho, tab5_rede_dados_t dados, void* ctx,
                    tab5_rede_http_t* info, tab5_rede_parar_t parar);

// Whole GET into a PSRAM buffer (for short JSON: search, APIs). Returns the
// buffer (with '\0'; release with free) or nullptr; 'max' limits the size.
char* tab5_rede_http_texto(const char* url, size_t max, tab5_rede_http_t* info, tab5_rede_parar_t parar);

// Text -> URL ("a b&c" -> "a+b%26c").
void tab5_rede_url_codificar(const char* s, char* out, size_t tam);
