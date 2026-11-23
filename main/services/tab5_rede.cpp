// NETWORK TOOLS - see tab5_rede.hpp.
#include "tab5_rede.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "recursos.h"   // the transfer declares what it uses: idle mode respects it
#include <atomic>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/etharp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/priv/tcpip_priv.h"

static esp_netif_t* sta(void) { return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }
static bool pare(tab5_rede_parar_t p) { return p && p(); }
static uint32_t ms_desde(int64_t t0) { return (uint32_t)((esp_timer_get_time() - t0) / 1000); }

// ---------------------------------------------------------------------------
// THE DEVICE ON THE NETWORK
// ---------------------------------------------------------------------------
bool tab5_rede_ip(tab5_rede_ip_t* o)
{
  memset(o, 0, sizeof(*o));
  esp_netif_t* nif = sta();
  if (!nif) { return false; }
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
    o->conectado = true;
    esp_ip4addr_ntoa(&ip.ip, o->ip, sizeof(o->ip));
    esp_ip4addr_ntoa(&ip.netmask, o->mascara, sizeof(o->mascara));
    esp_ip4addr_ntoa(&ip.gw, o->gateway, sizeof(o->gateway));
  }
  for (int k = 0; k < 2; ++k) {
    esp_netif_dns_info_t d;
    if (esp_netif_get_dns_info(nif, k ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &d) != ESP_OK) { continue; }
    if (d.ip.type == ESP_IPADDR_TYPE_V4 && d.ip.u_addr.ip4.addr) {
      esp_ip4addr_ntoa(&d.ip.u_addr.ip4, o->dns[k], sizeof(o->dns[k]));
    }
  }
  esp_ip6_addr_t l6;
  if (esp_netif_get_ip6_linklocal(nif, &l6) == ESP_OK) {
    snprintf(o->ipv6, sizeof(o->ipv6), IPV6STR, IPV62STR(l6));
  }
  esp_netif_get_mac(nif, o->mac);
  const char* h = nullptr;
  if (esp_netif_get_hostname(nif, &h) == ESP_OK && h) { snprintf(o->nome, sizeof(o->nome), "%s", h); }
  return true;
}

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------
int tab5_rede_dns(const char* nome, char ips[][TAB5_REDE_IP_TXT], int max, uint32_t* ms)
{
  const int64_t t0 = esp_timer_get_time();
  int n = 0;
  // Two queries: lwIP returns one address per family.
  const int FAM[2] = { AF_INET, AF_INET6 };
  for (int f = 0; f < 2 && n < max; ++f) {
    struct addrinfo dica = { };
    dica.ai_family = FAM[f];
    dica.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(nome, nullptr, &dica, &res) != 0 || !res) { continue; }
    for (struct addrinfo* r = res; r && n < max; r = r->ai_next) {
      const void* a = r->ai_family == AF_INET ? (const void*)&((struct sockaddr_in*)r->ai_addr)->sin_addr
                                              : (const void*)&((struct sockaddr_in6*)r->ai_addr)->sin6_addr;
      if (inet_ntop(r->ai_family, a, ips[n], TAB5_REDE_IP_TXT)) { ++n; }
    }
    freeaddrinfo(res);
  }
  if (ms) { *ms = ms_desde(t0); }
  return n;
}

// The first IPv4 of 'nome' (or itself, if it is already a number).
static bool ipv4_de(const char* nome, struct sockaddr_in* out)
{
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  if (inet_pton(AF_INET, nome, &out->sin_addr) == 1) { return true; }
  struct addrinfo dica = { };
  dica.ai_family = AF_INET;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(nome, nullptr, &dica, &res) != 0 || !res) { return false; }
  out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return true;
}

// ---------------------------------------------------------------------------
// ARP - the table belongs to lwIP: read and changed only on its task (tcpip_api_call).
// ---------------------------------------------------------------------------
typedef struct {
  struct tcpip_api_call_data base;
  bool                 perguntar;   // true = etharp_request(ip); false = read the table
  struct netif*        nif;
  ip4_addr_t           ip;
  tab5_rede_vizinho_t* out;
  int                  max, n;
} arp_msg_t;

static err_t arp_na_tcpip(struct tcpip_api_call_data* c)
{
  arp_msg_t* m = (arp_msg_t*)c;
  if (m->perguntar) { return etharp_request(m->nif, &m->ip); }
  for (size_t i = 0; i < ARP_TABLE_SIZE && m->n < m->max; ++i) {
    ip4_addr_t* ip = nullptr;
    struct netif* nf = nullptr;
    struct eth_addr* eth = nullptr;
    if (!etharp_get_entry(i, &ip, &nf, &eth)) { continue; }
    tab5_rede_vizinho_t& v = m->out[m->n++];
    ip4addr_ntoa_r(ip, v.ip, sizeof(v.ip));
    memcpy(v.mac, eth->addr, 6);
  }
  return ERR_OK;
}

int tab5_rede_arp(tab5_rede_vizinho_t* out, int max)
{
  arp_msg_t m = { };
  m.out = out;
  m.max = max;
  tcpip_api_call(arp_na_tcpip, &m.base);
  return m.n;
}

int tab5_rede_arp_varrer(tab5_rede_achou_t achou, void* ctx, tab5_rede_parar_t parar)
{
  esp_netif_t* nif = sta();
  esp_netif_ip_info_t ip;
  if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || !ip.ip.addr) { return -1; }
  struct netif* lw = (struct netif*)esp_netif_get_netif_impl(nif);
  if (!lw) { return -1; }
  // Only the device's /24, even on a bigger network: 254 queries, ~15 s.
  const uint32_t base = ntohl(ip.ip.addr) & 0xFFFFFF00u, eu = ntohl(ip.ip.addr) & 0xFF;
  uint32_t vistos[8] = { };   // one bit per address ending
  int achados = 0;
  tab5_rede_vizinho_t tab[ARP_TABLE_SIZE];
  // In batches the size of the ARP table (10 in this lwIP): asking more than
  // that at once pushes out whoever has just answered.
  const int LOTE = ARP_TABLE_SIZE - 2;
  for (uint32_t h = 1; h < 255 && !pare(parar); h += LOTE) {
    for (uint32_t k = h; k < h + LOTE && k < 255; ++k) {
      if (k == eu) { continue; }
      arp_msg_t m = { };
      m.perguntar = true;
      m.nif = lw;
      m.ip.addr = htonl(base | k);
      tcpip_api_call(arp_na_tcpip, &m.base);
    }
    vTaskDelay(pdMS_TO_TICKS(400));
    const int n = tab5_rede_arp(tab, ARP_TABLE_SIZE);
    for (int i = 0; i < n; ++i) {
      ip4_addr_t a;
      if (!ip4addr_aton(tab[i].ip, &a) || (ntohl(a.addr) & 0xFFFFFF00u) != base) { continue; }
      const uint32_t fim = ntohl(a.addr) & 0xFF;
      if (vistos[fim / 32] & (1u << (fim % 32))) { continue; }
      vistos[fim / 32] |= 1u << (fim % 32);
      ++achados;
      if (achou) { achou(&tab[i], ctx); }
    }
  }
  return achados;
}

// ---------------------------------------------------------------------------
// TRACEROUTE - ICMP echo with growing TTL: each router on the path returns
// "time exceeded" when the TTL hits zero there, and the target returns the echo.
// ---------------------------------------------------------------------------
int tab5_rede_traceroute(const char* alvo, int max_saltos, tab5_rede_salto_cb_t cb, void* ctx,
                         tab5_rede_parar_t parar)
{
  struct sockaddr_in dst;
  if (!ipv4_de(alvo, &dst)) { return -1; }
  const int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (s < 0) { return -1; }
  struct timeval tv = { 0, 200 * 1000 };   // wait in slices: Ctrl+C gets through quickly
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  const uint16_t id = (uint16_t)esp_random();
  uint16_t seq = 0;
  uint8_t pkt[sizeof(struct icmp_echo_hdr) + 24];
  uint8_t buf[160];
  int chegou = 0;

  for (int ttl = 1; ttl <= max_saltos && !chegou && !pare(parar); ++ttl) {
    tab5_rede_salto_t sl = { };
    sl.ttl = ttl;
    for (int k = 0; k < 3; ++k) {
      sl.ms[k] = -1;
      if (pare(parar)) { break; }
      int t = ttl;
      setsockopt(s, IPPROTO_IP, IP_TTL, &t, sizeof(t));
      struct icmp_echo_hdr* e = (struct icmp_echo_hdr*)pkt;
      memset(pkt, 0, sizeof(pkt));
      e->type = ICMP_ECHO;
      e->code = 0;
      e->id = htons(id);
      e->seqno = htons(++seq);
      memcpy(pkt + sizeof(*e), "M5tabOS traceroute......", 24);
      e->chksum = inet_chksum(pkt, sizeof(pkt));
      const int64_t t0 = esp_timer_get_time();
      if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&dst, sizeof(dst)) < 0) { continue; }
      while (ms_desde(t0) < 1000 && !pare(parar)) {
        struct sockaddr_in de;
        socklen_t dl = sizeof(de);
        const int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&de, &dl);
        if (n <= 0) { continue; }
        const int hl = (buf[0] & 0x0F) * 4;
        if (n < hl + 8) { continue; }
        const uint8_t tipo = buf[hl];
        const struct icmp_echo_hdr* r = nullptr;
        if (tipo == ICMP_ER) {
          r = (const struct icmp_echo_hdr*)(buf + hl);
        } else if (tipo == ICMP_TE || tipo == ICMP_DUR) {
          // Inside the error comes the start of OUR packet: IP + 8 bytes of the echo.
          const uint8_t* dentro = buf + hl + 8;
          const int ihl = (dentro[0] & 0x0F) * 4;
          if (n < hl + 8 + ihl + 8) { continue; }
          r = (const struct icmp_echo_hdr*)(dentro + ihl);
        }
        if (!r || r->id != htons(id) || r->seqno != htons(seq)) { continue; }   // someone else's (a ping, an old probe)
        sl.ms[k] = (int)ms_desde(t0);
        inet_ntop(AF_INET, &de.sin_addr, sl.ip, sizeof(sl.ip));
        if (tipo == ICMP_ER || (tipo == ICMP_DUR && de.sin_addr.s_addr == dst.sin_addr.s_addr)) { sl.fim = true; }
        break;
      }
    }
    if (cb) { cb(&sl, ctx); }
    if (sl.fim) { chegou = ttl; }
  }
  close(s);
  return chegou;
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------
int tab5_rede_conectar(const char* host, int porta, uint32_t timeout_ms, uint32_t* ms)
{
  struct sockaddr_in dst;
  if (!ipv4_de(host, &dst)) { return -1; }
  dst.sin_port = htons((uint16_t)porta);
  const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) { return -errno; }
  const int modo = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, modo | O_NONBLOCK);
  const int64_t t0 = esp_timer_get_time();
  int r = connect(s, (struct sockaddr*)&dst, sizeof(dst));
  if (r < 0 && errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = { (long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000 };
    r = select(s + 1, nullptr, &w, nullptr, &tv);
    if (r > 0) {
      int e = 0;
      socklen_t l = sizeof(e);
      getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
      r = e ? -e : 0;
    } else {
      r = -ETIMEDOUT;
    }
  } else if (r < 0) {
    r = -errno;
  }
  if (ms) { *ms = ms_desde(t0); }
  if (r < 0) { close(s); return r; }
  fcntl(s, F_SETFL, modo);   // back to blocking
  return s;
}

int tab5_rede_tcp(const char* host, int porta, uint32_t timeout_ms, uint32_t* ms)
{
  const int s = tab5_rede_conectar(host, porta, timeout_ms, ms);
  if (s < 0) { return s; }
  close(s);
  return 0;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static esp_err_t ao_evento(esp_http_client_event_t* ev)
{
  tab5_rede_http_t* info = (tab5_rede_http_t*)ev->user_data;
  if (ev->event_id == HTTP_EVENT_ON_HEADER && info && ev->header_key && ev->header_value
   && !strcasecmp(ev->header_key, "Content-Type")) {
    snprintf(info->tipo, sizeof(info->tipo), "%s", ev->header_value);
  }
  return ESP_OK;
}

// A TRANSFER IN PROGRESS MEANS THE NETWORK IS SERVING (kernel/recursos.h), like
// the web server: without this idle mode kicked in mid-download - 40 MHz,
// light sleep, Wi-Fi in maximum power saving with data arriving - and the Tab5 froze
// (reproduced 06/11/2026: 'http -o' of 4 MB over the local network with the screen
// turning off at 30 s). PROCESSING: the TLS handshake at 40 MHz overruns the
// timeout. Counter: the Music cover and a 'wget' can download at the same time.
static std::atomic<int> s_transferindo{0};
static void transferindo(bool sim)
{
  const int n = sim ? ++s_transferindo : --s_transferindo;
  if (sim ? n != 1 : n != 0) { return; }
  sistema_recurso(RECURSO_REDE, "download", sim);
  sistema_recurso(RECURSO_PROCESSAMENTO, "download", sim);
}

static bool http_feito(const char* url, bool so_cabecalho, tab5_rede_dados_t dados, void* ctx,
                       tab5_rede_http_t* info, tab5_rede_parar_t parar);

bool tab5_rede_http(const char* url, bool so_cabecalho, tab5_rede_dados_t dados, void* ctx,
                    tab5_rede_http_t* info, tab5_rede_parar_t parar)
{
  transferindo(true);
  const bool ok = http_feito(url, so_cabecalho, dados, ctx, info, parar);
  transferindo(false);
  return ok;
}

static bool http_feito(const char* url, bool so_cabecalho, tab5_rede_dados_t dados, void* ctx,
                       tab5_rede_http_t* info, tab5_rede_parar_t parar)
{
  tab5_rede_http_t vazio;
  if (!info) { info = &vazio; }
  memset(info, 0, sizeof(*info));
  info->tamanho = -1;
  const int64_t t0 = esp_timer_get_time();

  esp_http_client_config_t c = { };
  c.url = url;
  c.method = so_cabecalho ? HTTP_METHOD_HEAD : HTTP_METHOD_GET;
  c.timeout_ms = 10000;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.user_agent = "M5tabOS (ESP32-P4)";
  c.buffer_size = 2048;
  c.buffer_size_tx = 1024;
  c.event_handler = ao_evento;
  c.user_data = info;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) { snprintf(info->erro, sizeof(info->erro), "URL invalida"); return false; }

  bool ok = false;
  for (int redir = 0; redir <= 5; ++redir) {
    const esp_err_t e = esp_http_client_open(h, 0);
    if (e != ESP_OK) { snprintf(info->erro, sizeof(info->erro), "%s", esp_err_to_name(e)); break; }
    info->tamanho = esp_http_client_fetch_headers(h);
    // A chunked response does not give the size: the 0 here would be a false
    // "empty" - whoever checks that everything arrived would see 577 of 0.
    if (info->tamanho <= 0 && esp_http_client_is_chunked_response(h)) { info->tamanho = -1; }
    info->status = esp_http_client_get_status_code(h);
    const int st = info->status;
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && redir < 5) {
      esp_http_client_set_redirection(h);
      esp_http_client_close(h);
      ++info->redirecionou;
      continue;
    }
    ok = true;
    if (!so_cabecalho) {
      // 16 KB CHUNKS: the writer (wget, http -o) does one write() per
      // chunk, and the card works in 4 KB sectors - with 2 KB each chunk
      // was a small, slow write: ~66 KB/s measured, against 1.2 MB/s for
      // copying in large blocks on the same card (06/11/2026).
      static const int PEDACO = 16 * 1024;
      char* b = (char*)heap_caps_malloc(PEDACO, MALLOC_CAP_SPIRAM);
      int n;
      while (b && !pare(parar) && (n = esp_http_client_read(h, b, PEDACO)) > 0) {
        info->recebido += n;
        if (dados && !dados(b, n, ctx)) { break; }
      }
      heap_caps_free(b);
    }
    break;
  }
  esp_http_client_get_url(h, info->final, sizeof(info->final));
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
  info->ms = ms_desde(t0);
  if (!ok && !info->erro[0]) { snprintf(info->erro, sizeof(info->erro), "redirecionamentos demais"); }
  return ok;
}

typedef struct { char* b; size_t n, max; } junta_t;

static bool juntar(const char* d, int n, void* ctx)
{
  junta_t* j = (junta_t*)ctx;
  const size_t cabe = j->max - j->n < (size_t)n ? j->max - j->n : (size_t)n;
  memcpy(j->b + j->n, d, cabe);
  j->n += cabe;
  return j->n < j->max;
}

char* tab5_rede_http_texto(const char* url, size_t max, tab5_rede_http_t* info, tab5_rede_parar_t parar)
{
  junta_t j = { (char*)heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM), 0, max };
  if (!j.b) { return nullptr; }
  if (!tab5_rede_http(url, false, juntar, &j, info, parar)) { free(j.b); return nullptr; }
  j.b[j.n] = 0;
  return j.b;
}

void tab5_rede_url_codificar(const char* s, char* out, size_t tam)
{
  static const char HEX[] = "0123456789ABCDEF";
  size_t k = 0;
  for (const uint8_t* p = (const uint8_t*)s; *p && k + 4 < tam; ++p) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') { out[k++] = (char)*p; }
    else if (*p == ' ') { out[k++] = '+'; }
    else { out[k++] = '%'; out[k++] = HEX[*p >> 4]; out[k++] = HEX[*p & 15]; }
  }
  out[k] = '\0';
}
