#include "tab5_wifi.hpp"
#include "idioma.h"
#include "tab5_donos.hpp"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "eh_host_feat_peer_data.h"
#include "tab5_c6.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "esp_mac.h"
#include <unistd.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"   // esp_ptr_external_ram
#include "nvs.h"

#include "tab5_power.hpp"
#include "tab5_sd.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include <atomic>

static const char *TAG = "wifi";

// Time between restoring power to the C6 and talking to it. The C6 has to run its
// own bootloader and bring up the esp_hosted firmware before answering on SDIO.
// Talking too early returns the same 0x108 (ESP_ERR_TIMEOUT) we saw
// when the rail was off - and the error does NOT say "wait longer", it just says
// timeout, so it is an expensive guess to diagnose. 600ms has headroom; if one day
// an intermittent failure on power-up appears, this number is the first suspect.
#define ESPERA_BOOT_C6_MS   600

// Association attempts before giving up and showing FAILED. Without a limit, a
// wrong SSID would make the C6 sweep the spectrum forever - the worst possible
// consumption, on precisely the device where battery life is the problem.
#define MAX_TENTATIVAS      5

enum comando_t { CMD_LIGAR, CMD_DESLIGAR, CMD_SO_RADIO, CMD_ROTEADOR, CMD_ECONOMIA, CMD_PILHA, CMD_PROCURAR, CMD_TROCAR, CMD_RECONECTAR };

static QueueHandle_t s_fila     = nullptr;
static esp_netif_t*  s_netif    = nullptr;
static volatile bool s_pilha_de_pe = false;   // written by the Wi-Fi task, read by others
static volatile bool s_c6_nosso    = false;   // the C6 answered peer_data: our firmware (ESP-NOW available)   // esp_hosted + esp_wifi already initialised this boot

// LINK WITH THE C6, which is NOT the same thing as "Wi-Fi up".
//
// The difference cost dearly. With broken firmware on the C6,
// esp_hosted_connect_to_slave() passed - SDIO transport negotiated, versions
// exchanged - and only esp_wifi_init() died, because it depends on RPC and the
// coprocessor's RPC was stuck. As s_pilha_de_pe only becomes true at the END of
// subir_pilha(), it stayed false, and the C6 recovery commands (which
// need ONLY the link, not Wi-Fi) refused to run.
//
// In other words: the only path to fix the coprocessor was locked
// by the very defect it was supposed to fix.
static bool          s_c6_enlace   = false;
static volatile tab5_wifi_estado_t s_estado = TAB5_WIFI_DESLIGADO;
static volatile int8_t s_rssi   = TAB5_WIFI_RSSI_DESCONHECIDO;
static std::atomic<int> s_tentativas = 0;   // Wi-Fi task and event task
// THE USER WANTS WI-FI. Set in tab5_wifi_ligar/conectar and cleared ONLY in
// tab5_wifi_desligar (Settings, Radar) - giving up away from the router stops the
// radio but does NOT touch this. It is what separates "switched off on purpose" from "gave up
// and should come back by itself when the network appears" without the HAL asking the
// settings (the layer above).
static volatile bool s_querer = false;

// RESUME: after giving up (FAILED), searches again from time to time and
// connects to whichever known network appears. Starts at RETOMAR_MIN_MS and doubles up to
// RETOMAR_MAX_MS: coming home reconnects in ~30 s, and away from home one
// search (~2 s of radio) every 5 min is the cost.
#define RETOMAR_MIN_MS   30000u
#define RETOMAR_MAX_MS  300000u
static uint32_t s_retomar_ms    = 0;   // next search (0 = not scheduled)
static uint32_t s_retomar_passo = 0;
// WHAT THE WI-FI TASK WANTS FROM THE RADIO RIGHT NOW - the only thing the event
// task consults to decide whether a drop is a failure and whether STA_START connects.
// Events arrive AFTER whoever caused them; that is why it is a state that lasts
// until the next request, and never a flag set only around a call.
//   PARADO    switched off on purpose (or gave up): a drop doesn't reconnect
//   PROCURAR  scanning: STA_START doesn't connect (association would abort the scan)
//   SO_RADIO  radio on WITHOUT a router - ESP-NOW on a channel that isn't the
//             router's (the radio is only on one channel at a time); doesn't reconnect
//   CONECTAR  wants network s_ssid: a drop from it counts an attempt and reconnects
enum modo_t { MODO_PARADO, MODO_PROCURAR, MODO_SO_RADIO, MODO_CONECTAR };
static volatile modo_t s_modo = MODO_PARADO;
// THE RADIO IS STARTED (esp_wifi_start) - with or without a router. It is what
// ESP-NOW needs, and it is NOT the same as the link: turning Wi-Fi off in Settings (or
// giving up out of the router's range) stops the radio with the C6 powered and the link
// up. Without this distinction the ESP-NOW app showed "on air" with the radio stopped.
static volatile bool s_radio_ativo = false;
static void (* volatile s_ao_subir)(void) = nullptr;   // see tab5_wifi_ao_subir

// RADIO POWER SAVING - the SYSTEM is in charge, via requests.
// Modem sleep (WIFI_PS_MIN_MODEM) is the default: the C6 turns the receiver off between
// the router's beacons. Whoever needs the receiver on all the time (ESP-NOW
// in conversation - a frame arriving during a doze is lost) ASKS, with its own
// bit; the radio stays awake while there is at least one request. Until now the
// ESP-NOW service called esp_wifi_set_ps(WIFI_PS_NONE) directly, once, and the
// radio never saved power again - whether or not anyone was using ESP-NOW.
static volatile uint32_t s_acordado = 0;   // "receiver always on" requests
static int s_ps_aplicado = -1;             // the last WIFI_PS_* sent to the C6 (-1 = none)

// WHEN IDLE, MAXIMUM SAVING. Measured with 'energia ensaio' (2026-10-16): the
// Wi-Fi connected in minimum modem sleep cost 28 mA of the 74 when idle - 40%
// of the battery with the screen off. With the screen off nobody expects an immediate
// response: WIFI_PS_MAX_MODEM wakes the C6 every LISTEN_INTERVAL beacons (~1 s) instead
// of every beacon, and the router holds whatever arrives in the meantime (the
// Mesh MQTT arrives up to ~1 s late). Awake it goes back to minimum; a
// receiver-on request (ESP-NOW in conversation, CSI, sniffing) beats both.
#define LISTEN_INTERVAL 10
static bool s_ps_repouso = false;   // the idle state aplicar_economia() last saw

// SCREEN ON: CAPACITY WHEN THERE IS TRAFFIC. The same design as phones (the
// "dynamic power save"): with packets flowing, the radio stays awake (no
// power save - lowest latency, highest throughput); TRAFEGO_SEGURA_MS without a packet, it goes back
// to minimum modem sleep (wakes every beacon). The counting is done by esp_hosted's
// transport (CONFIG_ESP_HOSTED_HOST_PKT_STATS: 7 counters, same shape as
// eh_host_pkt_stats.h). Works for any C6: it is just the usual esp_wifi_set_ps.
#if CONFIG_ESP_HOSTED_HOST_PKT_STATS
struct pkt_stats_espelho_t { uint32_t tx_in, tx_trans, tx_out, tx_drop, rx_in, rx_out, stalls; };
extern "C" pkt_stats_espelho_t pkt_stats;
static uint32_t pacotes_agora(void) { return pkt_stats.rx_in + pkt_stats.tx_out; }
#else
static uint32_t pacotes_agora(void) { return 0; }
#endif
static const uint32_t TRAFEGO_PACOTES   = 4;      // in one round (~250 ms): "there is traffic"
static const uint32_t TRAFEGO_SEGURA_MS = 2000;   // no packet for this long: back to power save
static uint32_t s_pacotes_ant = 0;
static uint32_t s_trafego_ate = 0;                // ms (tick) until when it counts as "with traffic"

static uint32_t agora_ms(void) { return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()); }

// Only with esp_wifi started (subir_pilha, or the command with the stack up).
static void aplicar_economia(void)
{
  s_ps_repouso = tab5_power_em_repouso();
  const uint32_t p = pacotes_agora();
  if (p - s_pacotes_ant >= TRAFEGO_PACOTES) { s_trafego_ate = agora_ms() + TRAFEGO_SEGURA_MS; }
  s_pacotes_ant = p;
  const bool trafego = (int32_t)(s_trafego_ate - agora_ms()) > 0;
  const int ps = s_acordado ? (int)WIFI_PS_NONE
               : s_ps_repouso ? (int)WIFI_PS_MAX_MODEM
               : trafego ? (int)WIFI_PS_NONE : (int)WIFI_PS_MIN_MODEM;
  if (ps == s_ps_aplicado) { return; }
  if (esp_wifi_set_ps((wifi_ps_type_t)ps) == ESP_OK) {
    const bool so_trafego = !s_acordado && !s_ps_repouso;   // flips back and forth all the time: debug only
    s_ps_aplicado = ps;
    if (so_trafego) {
      ESP_LOGD(TAG, "radio: %s", trafego ? "awake (traffic)" : "modem sleep (quiet)");
    } else {
      ESP_LOGI(TAG, "radio: %s", s_acordado ? "receiver always on (requested)" : "maximum modem sleep (idle)");
    }
  }
}

static char s_ssid[33] = { 0 };
static char s_senha[65] = { 0 };
static char s_ip[16]   = "0.0.0.0";

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------
// A text file on the card instead of compiled into the binary: that way you change
// network without recompiling, and the binary carries no password at all.
// Deliberately dumb format - the first useful line is the SSID, the second is the
// password. Empty lines and lines starting with # are ignored.
//
//     # office network
//     MinhaRede
//     minhasenha123
//
// Open network: leave the second line empty... can't, an empty line is ignored.
// For an open network write the password as a line containing just "-" (handled below).
//
// PASSWORD ENCRYPTED IN THE FILE. The SSID is not a secret; the password should not sit in
// plain text on the card. What this solves is ONLY this: someone with just the CARD
// (in an SD reader, for example) can't read the password. It does NOT protect against someone with
// the DEVICE: the key comes from data on the chip itself, and the flash can be read
// with esptool. Secure boot and flash encryption are left out on purpose -
// they burn eFuses, are irreversible and lock out reflashing over USB.
//
// THE KEY: 16 RANDOM BYTES, STORED IN FLASH (NVS "seg"/"chave"),
// drawn once on first boot (tab5_wifi_chave_preparar, called by
// ajustes_iniciar). So what's on the card is useless without this device's
// flash - and whoever reads the flash would have everything anyway.
//
// PREVIOUSLY the key came from a fixed label + the factory MAC. The label is in
// the public code and the MAC is not secret (USB-Serial-JTAG hands it out as a
// serial number to any computer; and even without that, with Espressif's prefix
// 2^24 guesses remain - seconds on a PC). Anyone with just the
// card could read the password. That format is still READ (chave_legada) and rewritten in
// the new one straight away: in wifi.txt (ler_credenciais) and in the MQTT list
// (ajustes.cpp, mq_carregar).
//
// The new format starts with "k2" (outside the hex alphabet: never confused with
// the old one); the rest is the same - IV + ciphertext in hex.
#define CIFRA_PREFIXO "k2"
static bool chave_legada(uint8_t chave[16])
{
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) != ESP_OK) { return false; }
  static const char ROTULO[] = "M5tabOS/wifi.txt/v1";
  uint8_t dig[32];
  mbedtls_sha256_context h;
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts(&h, 0);
  mbedtls_sha256_update(&h, (const uint8_t*)ROTULO, sizeof(ROTULO) - 1);
  mbedtls_sha256_update(&h, mac, sizeof(mac));
  mbedtls_sha256_finish(&h, dig);
  mbedtls_sha256_free(&h);
  memcpy(chave, dig, 16);
  return true;
}

static uint8_t       s_chave[16];
static volatile bool s_chave_ok = false;

// Reads the key from NVS, or draws and stores it. Writes to flash: only from a task with
// its stack in internal RAM (boot) - see the warning in ajustes.cpp (pilha_na_psram).
bool tab5_wifi_chave_preparar(void)
{
  if (s_chave_ok) { return true; }
  int aqui = 0;
  if (esp_ptr_external_ram(&aqui)) { return false; }
  nvs_handle_t h;
  if (nvs_open("seg", NVS_READWRITE, &h) != ESP_OK) { return false; }
  uint8_t k[16];
  size_t t = sizeof(k);
  bool ok = nvs_get_blob(h, "chave", k, &t) == ESP_OK && t == sizeof(k);
  if (!ok) {
    // The chip's RNG, mixed with the clock and the MAC: even if one source
    // is weak at boot, the key doesn't become predictable.
    uint8_t sorteio[32];
    esp_fill_random(sorteio, sizeof(sorteio));
    const int64_t agora = esp_timer_get_time();
    uint8_t mac[6] = { };
    esp_efuse_mac_get_default(mac);
    uint8_t dig[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, sorteio, sizeof(sorteio));
    mbedtls_sha256_update(&c, (const uint8_t*)&agora, sizeof(agora));
    mbedtls_sha256_update(&c, mac, sizeof(mac));
    mbedtls_sha256_finish(&c, dig);
    mbedtls_sha256_free(&c);
    memcpy(k, dig, sizeof(k));
    memset(sorteio, 0, sizeof(sorteio));
    memset(dig, 0, sizeof(dig));
    ok = nvs_set_blob(h, "chave", k, sizeof(k)) == ESP_OK && nvs_commit(h) == ESP_OK;
    if (ok) { ESP_LOGI(TAG, "password key created"); }
  }
  nvs_close(h);
  if (ok) { memcpy(s_chave, k, sizeof(k)); s_chave_ok = true; }
  memset(k, 0, sizeof(k));
  if (!ok) { ESP_LOGE(TAG, "no password key in NVS - encrypting in the old format"); }
  return ok;
}

#define CIFRA_BLOCO   16
#define CIFRA_MAX_BRUTO 80   // sizeof(s_senha)-1 (64) rounded up to the next 16-byte block
#define CIFRA_MAX_HEX ((CIFRA_BLOCO + CIFRA_MAX_BRUTO) * 2 + 3)   // "k2" + IV + ciphertext, in hex

// true only when 'texto' really can be the output of cifrar_senha() - used to
// tell an ENCRYPTED password (written by this firmware from
// ) apart from a PLAIN TEXT password left by an earlier version. Without
// this, the first boot after this change would read the old password as if
// it were encrypted hex, fail to decrypt and Wi-Fi would stop working with no
// warning at all until the user rewrote wifi.txt. Real hex has at least
// one block (IV=16B) + one ciphertext block (16B) = 32 bytes = 64 characters,
// always even, always in [0-9a-f] - a real password rarely meets all three
// conditions at once (an uppercase letter, a symbol or an odd length is enough to
// never confuse them).
static bool parece_cifrado(const char* texto)
{
  if (!strncmp(texto, CIFRA_PREFIXO, 2)) { texto += 2; }
  const size_t n = strlen(texto);
  if (n < CIFRA_BLOCO * 2 * 2 || (n % 2) != 0) { return false; }
  for (size_t i = 0; i < n; ++i) {
    const char c = texto[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
  }
  return true;
}

static bool cifrar_senha(const char* claro, char* saida_hex, size_t tam_saida)
{
  const size_t n = strlen(claro);
  if (n > CIFRA_MAX_BRUTO - CIFRA_BLOCO) { return false; }   // should never happen - s_senha holds 65

  const size_t pad   = CIFRA_BLOCO - (n % CIFRA_BLOCO);      // PKCS7: 1..16, never 0
  const size_t bruto = n + pad;

  uint8_t buf[CIFRA_MAX_BRUTO];
  memcpy(buf, claro, n);
  memset(buf + n, (uint8_t)pad, pad);

  uint8_t iv[CIFRA_BLOCO];
  esp_fill_random(iv, sizeof(iv));
  uint8_t iv_trabalho[CIFRA_BLOCO];
  memcpy(iv_trabalho, iv, sizeof(iv));   // mbedtls consumes/updates the IV it receives - keep a copy

  uint8_t chave[16];
  const bool nova = tab5_wifi_chave_preparar();
  if (nova) { memcpy(chave, s_chave, sizeof(chave)); }
  else if (!chave_legada(chave)) { return false; }
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, chave, 128);
  memset(chave, 0, sizeof(chave));
  uint8_t cifrado[CIFRA_MAX_BRUTO];
  const int r = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, bruto, iv_trabalho, buf, cifrado);
  mbedtls_aes_free(&ctx);
  if (r != 0 || tam_saida < (size_t)(sizeof(iv) + bruto) * 2 + 3) { return false; }

  // IV in front, ciphertext behind - all in hex, on a single line.
  size_t p = 0;
  if (nova) { p = (size_t)snprintf(saida_hex, tam_saida, "%s", CIFRA_PREFIXO); }
  for (size_t i = 0; i < sizeof(iv); ++i) { p += (size_t)snprintf(saida_hex + p, tam_saida - p, "%02x", iv[i]); }
  for (size_t i = 0; i < bruto;       ++i) { p += (size_t)snprintf(saida_hex + p, tam_saida - p, "%02x", cifrado[i]); }
  return true;
}

// 'legado' (optional) = true when it came in the old format: the reader rewrites it.
static bool decifrar_senha(const char* hex, char* saida, size_t tam_saida, bool* legado = nullptr)
{
  const bool nova = !strncmp(hex, CIFRA_PREFIXO, 2);
  if (nova) { hex += 2; }
  if (legado) { *legado = !nova; }
  const size_t hl = strlen(hex);
  const size_t bruto_bytes = hl / 2;
  if (hl < CIFRA_BLOCO * 2 * 2 || (hl % 2) != 0
   || bruto_bytes > (size_t)(CIFRA_BLOCO + CIFRA_MAX_BRUTO)) { return false; }

  uint8_t todos[CIFRA_BLOCO + CIFRA_MAX_BRUTO];
  for (size_t i = 0; i < bruto_bytes; ++i) {
    unsigned int b;
    if (sscanf(hex + i * 2, "%2x", &b) != 1) { return false; }
    todos[i] = (uint8_t)b;
  }

  uint8_t iv[CIFRA_BLOCO];
  memcpy(iv, todos, sizeof(iv));
  const uint8_t* cifrado    = todos + CIFRA_BLOCO;
  const size_t   n_cifrado  = bruto_bytes - CIFRA_BLOCO;
  if (n_cifrado == 0 || (n_cifrado % CIFRA_BLOCO) != 0) { return false; }

  uint8_t claro[CIFRA_MAX_BRUTO];
  uint8_t chave[16];
  if (nova) {
    if (!tab5_wifi_chave_preparar()) { return false; }
    memcpy(chave, s_chave, sizeof(chave));
  } else if (!chave_legada(chave)) { return false; }
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, chave, 128);
  memset(chave, 0, sizeof(chave));
  const int r = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, n_cifrado, iv, cifrado, claro);
  mbedtls_aes_free(&ctx);
  if (r != 0) { return false; }

  const uint8_t pad = claro[n_cifrado - 1];
  if (pad == 0 || pad > CIFRA_BLOCO || pad > n_cifrado) { return false; }   // invalid PKCS7
  const size_t texto_n = n_cifrado - pad;
  if (texto_n >= tam_saida) { return false; }
  memcpy(saida, claro, texto_n);
  saida[texto_n] = 0;
  return true;
}
// PASSWORD MANGLED BY OLD FIRMWARE - and the fix, when possible.
//
// A version from before the random key ("k2" format) doesn't recognise the
// new format: it reads "k2<hex>" as a PLAIN TEXT password, truncates it at 64
// characters (its buffer) and rewrites wifi.txt encrypting THAT with the old
// cipher. Back on a new version, the decrypted password comes out as "k2<62 hex>" -
// and the router refuses it (reason 15, handshake). It happened during a bisection on
// 2026-10-21; it happens to anyone who goes back to an old version and then updates.
//
// The "k2<hex>" is the REAL password encrypted in the new format, minus the last
// byte (the 2 characters cut off). A password of up to 15 characters fits in a single
// block: the missing byte is found by trial (256), and the right one is the one that decrypts with
// valid padding and readable text - if there is ONLY one. A longer password lost a
// whole block: there's no way back, and the log asks for it to be typed again.
static bool hex_minusculo(const char* t)
{
  for (; *t; ++t) { if (!((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'f'))) { return false; } }
  return true;
}

static bool consertar_senha_cortada(const char* estragada, char* saida, size_t tam_saida)
{
  if (strncmp(estragada, CIFRA_PREFIXO, 2) != 0 || strlen(estragada) != 64 || !hex_minusculo(estragada + 2)) {
    return false;
  }
  char tentativa[70], achada[65] = "";
  int achadas = 0;
  for (int b = 0; b < 256 && achadas < 2; ++b) {
    snprintf(tentativa, sizeof(tentativa), "%s%02x", estragada, b);
    char claro[65];
    if (!decifrar_senha(tentativa, claro, sizeof(claro))) { continue; }
    bool legivel = claro[0] != 0;
    for (const char* c = claro; *c && legivel; ++c) { legivel = (unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F; }
    if (legivel) { snprintf(achada, sizeof(achada), "%s", claro); ++achadas; }
    memset(claro, 0, sizeof(claro));
  }
  memset(tentativa, 0, sizeof(tentativa));
  if (achadas != 1) { memset(achada, 0, sizeof(achada)); return false; }
  snprintf(saida, tam_saida, "%s", achada);
  memset(achada, 0, sizeof(achada));
  return true;
}

// ---------------------------------------------------------------------------
// KNOWN NETWORKS - several, in the same wifi.txt
// ---------------------------------------------------------------------------
// The file stores PAIRS of lines (SSID, password), one pair per network. The old
// single-network file is just the one-pair case. The password is encrypted (see
// above); in plain text it is accepted and encrypted on the next boot. On power-up, the
// system scans the surrounding networks and connects to the known one with the strongest signal.
static tab5_wifi_rede_t s_redes[TAB5_WIFI_MAX_REDES];   // .senha is plain text: RAM only
static char             s_senhas[TAB5_WIFI_MAX_REDES][65];
static int              s_n_redes = 0;
static bool             s_lido = false;              // the card has already been read this boot
// The FILE was actually read (or, with the card present, didn't exist).
// Only then can it be REWRITTEN: rewriting from a list that didn't
// come from it would erase the other stored networks (card absent, or lent
// to the computer in USB flash drive mode).
static bool             s_arquivo_lido = false;

// Networks seen in the last scan (for the Settings screen).
static tab5_wifi_visto_t s_vistas[TAB5_WIFI_MAX_VISTAS];
static tab5_wifi_visto_t s_vistas_novas[TAB5_WIFI_MAX_VISTAS];   // the scan builds here (see procurar_redes)
static volatile int      s_n_vistas = 0;
static volatile bool     s_procurando = false;
static std::atomic<uint32_t> s_redes_ver = 1;
static char              s_alvo[33];                 // CMD_TROCAR: to which network

// The list is touched by the Wi-Fi task (reading on power-up) and by whoever saves a new
// network (Settings worker, console). Recursive: saving reads first.
static SemaphoreHandle_t s_trava_redes = nullptr;
struct trava_redes_t {
  trava_redes_t()  { if (s_trava_redes) { xSemaphoreTakeRecursive(s_trava_redes, portMAX_DELAY); } }
  ~trava_redes_t() { if (s_trava_redes) { xSemaphoreGiveRecursive(s_trava_redes); } }
};

static int achar_rede(const char* ssid)
{
  for (int i = 0; i < s_n_redes; ++i) { if (strcmp(s_redes[i].ssid, ssid) == 0) { return i; } }
  return -1;
}

// The network in use becomes known network 'i'.
static void usar_rede(int i)
{
  snprintf(s_ssid, sizeof(s_ssid), "%s", s_redes[i].ssid);
  snprintf(s_senha, sizeof(s_senha), "%s", s_senhas[i]);
}

// WRITES THE FILE with all the networks, passwords ALREADY ENCRYPTED (or "-", open
// network), through the system port (tab5_sd_substituir): never leaves the device
// with half a file, zeroes the old one before deleting it (SIGILOSO - on FAT deleting
// only marks the sectors as free, and an old password would still be readable by
// recovery) and, with the card lent to the computer, goes to the journal.
static bool escrever_arquivo(void)
{
  if (!s_arquivo_lido) {
    ESP_LOGW(TAG, "%s was not read (no card?) - the change only lasts until restart", TAB5_WIFI_ARQUIVO);
    return false;
  }
  static const char CAB[] = "# redes Wi-Fi: SSID e senha, um par de linhas por rede.\n"
                            "# senhas cifradas (AES-128-CBC, chave do aparelho) - para trocar,\n"
                            "# escreva a senha nova em texto: ela e cifrada no proximo boot.\n";
  const size_t cap = sizeof(CAB) + (size_t)TAB5_WIFI_MAX_REDES * (sizeof(s_redes[0].ssid) + CIFRA_MAX_HEX + 2);
  char* txt = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!txt) { return false; }
  size_t k = (size_t)snprintf(txt, cap, "%s", CAB);
  bool ok = true;
  for (int i = 0; ok && i < s_n_redes && k < cap; ++i) {
    char linha[CIFRA_MAX_HEX];
    if (s_senhas[i][0] == 0) { snprintf(linha, sizeof(linha), "-"); }
    else if (!cifrar_senha(s_senhas[i], linha, sizeof(linha))) { ok = false; break; }
    k += (size_t)snprintf(txt + k, cap - k, "%s\n%s\n", s_redes[i].ssid, linha);
  }
  ok = ok && k < cap && tab5_sd_substituir("Sistema", "wifi.txt", txt, k, TAB5_SD_SIGILOSO);
  heap_caps_free(txt);
  if (!ok) { ESP_LOGE(TAG, "could not write %s", TAB5_WIFI_ARQUIVO); }
  return ok;
}

static bool ler_credenciais(void)
{
  // ALREADY READ THIS BOOT? Then do NOT go to the card.
  //
  // This is not an optimisation, it is a fix. The P4's SDMMC controller is ONE ONLY and
  // multiplexes between the two slots: the card on 0, the Wi-Fi ESP32-C6 on 1.
  // Mounting or unmounting the card touches shared state - with the C6 link
  // up, that has already brought Wi-Fi down and restarted the board. That is why the card
  // is read ONCE per boot, before any link with the C6 exists; whoever
  // saves a new network (Settings, console) updates RAM at the same time.
  trava_redes_t trava;
  if (s_lido) { return s_n_redes > 0; }

  if (!tab5_sd_montar()) {
    ESP_LOGE(TAG, "no card: cannot read %s", TAB5_WIFI_ARQUIVO);
    return false;
  }
  // Via the system read: reads the latest version (a flash drive session's journal goes in
  // first) and completes a swap interrupted by a power cut.
  FILE* f = tab5_sd_abrir("Sistema", "wifi.txt", "r");
  if (!f) {
    ESP_LOGE(TAG, "file %s does not exist", TAB5_WIFI_ARQUIVO);
    tab5_sd_desmontar();
    s_lido = true;   // no file: a new network comes in via Settings (and creates the file)
    s_arquivo_lido = true;
    return false;
  }

  char linha[CIFRA_MAX_HEX + 4];
  char ssid[33] = "";
  bool tem_ssid = false, tinha_texto_puro = false, alguma_ilegivel = false, tinha_legado = false;
  s_n_redes = 0;
  while (fgets(linha, sizeof(linha), f) && s_n_redes < TAB5_WIFI_MAX_REDES) {
    // strips \r and \n from the end - a file saved on Windows has \r and it would end up
    // inside the SSID, which then never matches the real network.
    size_t n = strlen(linha);
    while (n > 0 && (linha[n-1] == '\n' || linha[n-1] == '\r')) { linha[--n] = 0; }
    if (n == 0 || linha[0] == '#') { continue; }
    if (!tem_ssid) { snprintf(ssid, sizeof(ssid), "%s", linha); tem_ssid = true; continue; }
    tem_ssid = false;

    char senha[65] = "";
    if (strcmp(linha, "-") == 0) {
      // open network
    } else if (parece_cifrado(linha)) {
      bool legado = false;
      const bool ok_dec = decifrar_senha(linha, senha, sizeof(senha), &legado);
      if (ok_dec && legado) { tinha_legado = true; }
      // Decrypted, but the "password" is the new format truncated by an old firmware.
      if (ok_dec && !strncmp(senha, CIFRA_PREFIXO, 2) && strlen(senha) == 64 && hex_minusculo(senha + 2)) {
        char certa[65];
        if (consertar_senha_cortada(senha, certa, sizeof(certa))) {
          snprintf(senha, sizeof(senha), "%s", certa);
          memset(certa, 0, sizeof(certa));
          tinha_legado = true;   // rewrites in the new format, just below
          ESP_LOGW(TAG, "%s: the password for \"%s\" had been mangled by old firmware - recovered",
                   TAB5_WIFI_ARQUIVO, ssid);
        } else {
          ESP_LOGE(TAG, "%s: the password for \"%s\" was mangled by old firmware and cannot be fixed - "
                        "choose the network in Settings > Wi-Fi and type it again", TAB5_WIFI_ARQUIVO, ssid);
          alguma_ilegivel = true;
          continue;
        }
      }
      if (!ok_dec) {
        // Encrypted by ANOTHER device (the key is per Tab5) or a corrupted
        // file: the network is left out, and the log says how to fix it.
        ESP_LOGE(TAG, "%s: the password for \"%s\" is not from this device - write it again as "
                      "plain text in the file, or choose the network in Settings > Wi-Fi", TAB5_WIFI_ARQUIVO, ssid);
        alguma_ilegivel = true;
        continue;
      }
    } else {
      snprintf(senha, sizeof(senha), "%s", linha);   // plain text: encrypted just below
      tinha_texto_puro = true;
    }
    if (achar_rede(ssid) >= 0) { continue; }   // repeated in the file: the first one wins
    snprintf(s_redes[s_n_redes].ssid, sizeof(s_redes[0].ssid), "%s", ssid);
    s_redes[s_n_redes].aberta = (senha[0] == 0);
    snprintf(s_senhas[s_n_redes], sizeof(s_senhas[0]), "%s", senha);
    ++s_n_redes;
  }
  fclose(f);
  s_arquivo_lido = true;

  // AUTOMATIC MIGRATION: a plain-text password becomes encrypted right here. It is
  // SAFE to touch the card at this point for the same reason as this whole function:
  // it runs once per boot, BEFORE the link with the C6.
  // The same for the old cipher (key derived from the MAC): rewrites with the new one.
  if (tinha_texto_puro || tinha_legado) {
    if (escrever_arquivo()) { ESP_LOGW(TAG, "%s: password %s - now encrypted with the new key", TAB5_WIFI_ARQUIVO,
                                       tinha_texto_puro ? "in plain text" : "in the old cipher"); }
    else                    { ESP_LOGE(TAG, "%s: plain-text password and could NOT encrypt the file", TAB5_WIFI_ARQUIVO); }
  }
  (void)alguma_ilegivel;
  tab5_sd_desmontar();
  s_lido = true;

  if (s_n_redes > 0) { usar_rede(0); }
  ESP_LOGI(TAG, "%d known network(s)", s_n_redes);
  return s_n_redes > 0;
}

// Scans the surrounding networks (the radio must be started). Stores in
// s_vistas, one entry per SSID (the strongest), sorted by signal.
static bool procurar_redes(void)
{
  wifi_scan_config_t sc = { };
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) { return false; }
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  wifi_ap_record_t* aps = n ? (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t)) : nullptr;
  if (n && !aps) { esp_wifi_clear_ap_list(); return false; }
  if (n) { esp_wifi_scan_get_ap_records(&n, aps); }
  // Builds into the WORKING list and only swaps at the end, under the networks lock: the
  // screen reads s_vistas (tab5_wifi_vista) at any time, and before it could catch
  // a half-written name (W2 of the 22/10 review).
  tab5_wifi_visto_t* v = s_vistas_novas;
  int k = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const char* nome = (const char*)aps[i].ssid;
    if (!nome[0]) { continue; }
    int j = 0;
    while (j < k && strcmp(v[j].ssid, nome) != 0) { ++j; }
    if (j < k) { if (aps[i].rssi > v[j].rssi) { v[j].rssi = aps[i].rssi; } continue; }
    if (k >= TAB5_WIFI_MAX_VISTAS) { continue; }
    snprintf(v[k].ssid, sizeof(v[k].ssid), "%s", nome);
    v[k].rssi = aps[i].rssi;
    v[k].canal = aps[i].primary;
    v[k].aberta = (aps[i].authmode == WIFI_AUTH_OPEN);
    ++k;
  }
  free(aps);
  for (int i = 1; i < k; ++i) {   // strongest first
    tab5_wifi_visto_t t = v[i];
    int j = i - 1;
    while (j >= 0 && v[j].rssi < t.rssi) { v[j + 1] = v[j]; --j; }
    v[j + 1] = t;
  }
  trava_redes_t trava;
  for (int i = 0; i < k; ++i) { v[i].conhecida = achar_rede(v[i].ssid) >= 0; }
  memcpy(s_vistas, v, sizeof(tab5_wifi_visto_t) * (size_t)k);
  s_n_vistas = k;
  ++s_redes_ver;
  return true;
}

// The known network with the strongest signal among those seen; -1 if none shows up.
static int melhor_conhecida(void)
{
  for (int i = 0; i < s_n_vistas; ++i) {   // s_vistas already comes strongest first
    const int r = achar_rede(s_vistas[i].ssid);
    if (r >= 0) { return r; }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
static void ao_evento(void* arg, esp_event_base_t base, int32_t id, void* dados)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
  {
    // Only connect if a network is wanted (see s_modo). In SO_RADIO the state has to
    // leave SUBINDO: otherwise the status bar would say "starting" forever.
    if (s_modo == MODO_SO_RADIO) { s_estado = TAB5_WIFI_DESLIGADO; return; }
    if (s_modo != MODO_CONECTAR) { return; }
    s_estado = TAB5_WIFI_CONECTANDO;
    esp_wifi_connect();
  }
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
  {
    s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
    strcpy(s_ip, "0.0.0.0");

    // A drop without wanting a network (switch off, scan, radio only): not a failure, and it
    // doesn't reconnect. Without this guard, switching Wi-Fi off would trigger a burst of
    // reconnection attempts - exactly the opposite of what was asked.
    //
    // AND IT DOESN'T CLEAR THE REASON. This event arrives AFTER whoever caused it: when
    // giving up, FALHOU is already set and CMD_DESLIGAR stops the radio - and the
    // drop that stop generates arrived here and swapped FALHOU for DESLIGADO. The
    // resume (which only looked at FALHOU) never searched again: coming home
    // didn't reconnect, and only the scan in Settings > Wi-Fi unstuck it.
    if (s_modo != MODO_CONECTAR) {
      if (s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO) { s_estado = TAB5_WIFI_DESLIGADO; }
      return;
    }

    // THE OLD NETWORK'S DROP, ON A SWITCH. tratar_trocar() lets go of the old network and
    // connects straight to the new one; this drop arrives AFTER, with the mode already CONECTAR.
    // The event says which network it dropped from: one other than the target is not a failure.
    const wifi_event_sta_disconnected_t* q = (const wifi_event_sta_disconnected_t*)dados;
    if (q && q->ssid_len > 0 && s_estado == TAB5_WIFI_CONECTANDO
     && (q->ssid_len != strnlen(s_ssid, sizeof(s_ssid)) || memcmp(q->ssid, s_ssid, q->ssid_len) != 0)) {
      ESP_LOGI(TAG, "left \"%.*s\" (network switch) - not a failure", (int)q->ssid_len, (const char*)q->ssid);
      return;
    }

    ++s_tentativas;
    if (s_tentativas > MAX_TENTATIVAS)
    {
      ESP_LOGW(TAG, "gave up after %d attempts - stopping the radio",
               MAX_TENTATIVAS);
      s_estado = TAB5_WIFI_FALHOU;
      // Giving up without stopping the radio was the worst of all worlds: the C6 stayed on and
      // associated with nothing, silently draining the battery until the next boot.
      // Queued instead of calling esp_wifi_stop() here: we are INSIDE the
      // Wi-Fi event handler, and stopping the stack from within itself is
      // asking for a deadlock.
      comando_t c = CMD_DESLIGAR;
      xQueueSend(s_fila, &c, 0);
      return;
    }
    ESP_LOGI(TAG, "dropped, attempt %d/%d", s_tentativas.load(), MAX_TENTATIVAS);
    s_estado = TAB5_WIFI_CONECTANDO;
    // NO SLEEPING HERE. This runs on ESP-IDF's DEFAULT event task - the
    // same one for all system events (IP, Wi-Fi, esp_hosted). The
    // vTaskDelay(1000) that used to live here froze all of them for 1 s on every
    // attempt. The wait and the reconnect go to the Wi-Fi's own task.
    comando_t c = CMD_RECONECTAR;
    if (xQueueSend(s_fila, &c, 0) != pdTRUE) { esp_wifi_connect(); }   // queue full: right away, without the pause
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t* e = (ip_event_got_ip_t*) dados;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_tentativas = 0;
    s_estado = TAB5_WIFI_CONECTADO;
    ESP_LOGI(TAG, "connected, IP %s", s_ip);
    tab5_mem_marco("WiFi: IP received");
  }
}

// ---------------------------------------------------------------------------
// Bring-up and teardown (service task)
// ---------------------------------------------------------------------------
static bool subir_pilha(void)
{
  // The rail has already been switched on by the UI task before queueing the
  // command; here we only wait for the C6 to finish booting.
  vTaskDelay(pdMS_TO_TICKS(ESPERA_BOOT_C6_MS));

  // In the two failures below, the RAW SDIO log is silenced (see
  // tab5_wifi_iniciar). So the message here has to be self-sufficient:
  // say what happened, what probably caused it, and how to see the raw log.
  tab5_mem_marco("C6: before esp_hosted");
  // Slot 1 comes up on the controller the card shares (see tab5_sd.hpp).
  tab5_sd_controlador_travar();
  int rc = esp_hosted_init();
  tab5_mem_marco("esp_hosted_init (SDIO)");
  if (rc != 0) {
    ESP_LOGE(TAG, "esp_hosted_init failed: %d - the C6 did not answer on SDIO. "
                  "To see the raw log: 'log eh_sdio 4' and 'log sdmmc_io 4'", rc);
    tab5_sd_controlador_soltar();
    return false;
  }

  rc = esp_hosted_connect_to_slave();   // slot 1 card_init: still holding the lock
  tab5_sd_controlador_soltar();
  if (rc == 0) {
    // Set HERE, and not at the end: from here down everything depends on RPC, and that is
    // exactly what may be missing on a C6 with bad firmware. The link already
    // exists, and it is what firmware flashing uses.
    s_c6_enlace = true;
    tab5_mem_marco("C6: link + RPC");
  }
  if (rc != 0) {
    ESP_LOGE(TAG, "connection to the C6 failed: %d.", rc);
    ESP_LOGE(TAG, "If you have just flashed firmware to the C6, IT RECOVERS "
                  "BY ITSELF: without confirmation within %d s it reverts to the previous "
                  "image. Leave it ON until then - every restart rearms the "
                  "countdown from zero.", C6_PRAZO_HOST_S);
    return false;
  }

  // THIS CALL HAS BECOME A GATE, not just a diagnostic curiosity.
  //
  // It is the cheapest RPC there is - it just returns three numbers. If IT doesn't
  // come back, no other will, and insisting is expensive: each pending RPC
  // leaves esp_hosted's sdio_write task spinning, and it even trips the
  // P4's task watchdog (log, "CPU 0: sdio_write").
  //
  // Before this we kept going and hit another timeout in
  // esp_wifi_init - ten seconds of storm instead of five, and it also created
  // a netif that would only get in the way of the next attempt.
  //
  // Stopping here is quicker, quieter, and tells the truth on the screen.
  esp_hosted_coprocessor_fwver_t ver = { };
  if (esp_hosted_get_coprocessor_fwversion(&ver) != 0)
  {
    ESP_LOGE(TAG, "the C6 accepted the link but DOES NOT ANSWER RPC.");
    ESP_LOGE(TAG, "Its firmware needs to be reflashed through the board's "
                  "reserved connector (docs.m5stack.com, restore_factory). "
                  "The rest of the system keeps working - only Wi-Fi goes down.");
    s_estado = TAB5_WIFI_C6_MUDO;
    return false;
  }
  ESP_LOGI(TAG, "C6 up, firmware %d.%d.%d",
           (int)ver.major1, (int)ver.minor1, (int)ver.patch1);

  // esp_netif_init() and the event loop may already exist (another service).
  // ESP_ERR_INVALID_STATE here means "already ready", not an error.
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { return false; }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { return false; }

  // REUSES THE NETIF IF IT ALREADY EXISTS. This is the fix for a BLUE SCREEN.
  //
  // subir_pilha() was not idempotent, and it CAN run more than once: when
  // it fails, s_pilha_de_pe stays false and the next attempt to switch Wi-Fi on
  // comes in here again. But the netif created on the previous pass stayed
  // alive - the error path undid nothing.
  //
  // esp_netif_create_default_wifi_sta() doesn't return an error in that case: it
  // ASSERTS and brings the device down.
  //
  //   E esp_netif_lwip: Failed to configure netif (duplicate key)
  //   assert failed: esp_netif_create_default_wifi_sta wifi_default.c:422
  //
  // It happened with the C6 silent: the first bring-up created the netif and
  // died in esp_wifi_init; switching Wi-Fi back on from Settings brought the whole P4 down.
  // A coprocessor that doesn't respond MUST NOT bring down the host.
  //
  // Same solution esp_hosted itself uses on the coprocessor side
  // (eh_cp_idem_wraps.c): look up before creating.
  if (!s_netif) { s_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }
  if (!s_netif) { s_netif = esp_netif_create_default_wifi_sta(); }

  // ESP_ERR_INVALID_STATE here is "already initialised", not an error - same logic as the
  // netif above, for the same reason: this function can run twice.
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  // CSI ENABLED AT INIT, and it is THIS side that decides, not the C6.
  //
  // The C6's esp_hosted (req_wifi_init + get_merged_init_config) calls its
  // esp_wifi_init with the fields that come from here, and csi_enable is one
  // of them ("always use value from host"). The C6 driver stores that value
  // (wifi_menuconfig_init) and esp_wifi_set_csi* checks it: if zero, it logs
  // "CSI not enabled in menuconfig!" and returns ESP_FAIL. The C6 built with
  // CONFIG_ESP_WIFI_CSI_ENABLED is not enough: its default is never used.
  //
  // In code and not via CONFIG_WIFI_RMT_CSI_ENABLED: CSI belongs to the C6's
  // firmware, the P4 doesn't call any CSI API. Cost: the C6 reserves about 1 KB
  // per static RX buffer (16 -> ~16 KB of its RAM), paid even with CSI
  // stopped; enabling it only on request would require restarting Wi-Fi.
  cfg.csi_enable = 1;
  const esp_err_t erri = esp_wifi_init(&cfg);
  if (erri != ESP_OK && erri != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_wifi_init: %s - the C6 answered the link but not RPC. "
                  "If you have just flashed firmware to it, LEAVE IT ALONE: without "
                  "resetting the C6, it reverts by itself to the previous image in "
                  "%d s. Restarting the P4 RESETS the C6 and rearms that countdown.",
             esp_err_to_name(erri), C6_PRAZO_HOST_S);
    return false;
  }

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ao_evento, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ao_evento, nullptr, nullptr);

  // RAM and not NVS: the one keeping the preference here is the ajustes module, and the
  // file on the card is the source of truth for credentials. Letting esp_wifi
  // save its own copy in NVS would create a second truth, which one day
  // disagrees with the first.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  tab5_mem_marco("esp_wifi_init + netif");

  // Modem sleep: the C6 turns the radio off between beacons. It is a big saving with no
  // downside for our use (we are not a server, nobody looks for us) -
  // UNLESS someone has requested the receiver on (see RADIO POWER SAVING).
  s_ps_aplicado = -1;   // new stack: the C6 knows nothing yet
  aplicar_economia();

  // CONFIRMS THE LINK WITH THE C6 - this is what cancels the automatic rollback.
  //
  // If the C6 is running OUR freshly flashed firmware, it came up with a
  // grace period armed: without this notice within the deadline, it points the boot
  // back to the previous image itself and restarts. This send is the proof that the
  // SDIO link closed and the host is talking.
  //
  // LAST, and because of the clock. esp_hosted's synchronous RPC waits up to
  // 5 s for a response (DEFAULT_RPC_RSP_TIMEOUT), and the 1.4.1 factory firmware
  // doesn't know this message - so with it the call spends the 5 s and fails.
  // At the start of subir_pilha() that would delay the whole radio by 5 s on every
  // boot; here at the end, Wi-Fi is already configured and the wait holds nothing up.
  // With our firmware on the C6 the response is immediate and the cost vanishes.
  eh_host_feat_peer_data_init();
  if (eh_host_peer_data_send(C6_MSG_HOST_VIVO, nullptr, 0) == ESP_OK) {
    s_c6_nosso = true;
    ESP_LOGI(TAG, "C6 with M5tabOS firmware (ESP-NOW available)");
  } else {
    ESP_LOGI(TAG, "C6 with factory firmware (normal Wi-Fi; no ESP-NOW)");
  }

  return true;
}

// Sends s_ssid/s_senha to the C6 (the network chosen by usar_rede).
static void aplicar_config(void)
{
  wifi_config_t wc = { };
  // The 802.11 fields need no terminator: a 32-character SSID and a 64-digit hex
  // PSK fill them completely (strncpy with "- 1" silently dropped the last one).
  memcpy(wc.sta.ssid,     s_ssid,  strnlen(s_ssid,  sizeof(wc.sta.ssid)));
  memcpy(wc.sta.password, s_senha, strnlen(s_senha, sizeof(wc.sta.password)));
  wc.sta.listen_interval = LISTEN_INTERVAL;   // only applies in WIFI_PS_MAX_MODEM (idle)
  esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void tratar_ligar(void)
{
  if (s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO) { return; }

  s_estado = TAB5_WIFI_SUBINDO;

  // Credentials FIRST, before bringing up the stack. Two reasons, and the second was
  // learnt the hard way:
  //
  // 1. If there is no wifi.txt, there's no point waking the C6 - saves power and the user sees
  //    the reason immediately, instead of waiting for the whole negotiation to fail.
  // 2. Reading the credentials MOUNTS THE CARD, and mounting the card touches SDMMC. With
  //    the card on slot 0 and the C6 on slot 1 this no longer brings the link down
  //    (see the big comment in tab5_sd.cpp), but accessing the card
  //    BEFORE the link exists removes the overlap completely. Defence in
  //    depth: if someone ever touches the slots, it still won't break here.
  if (!ler_credenciais()) { s_estado = TAB5_WIFI_SEM_CREDENCIAL; return; }

  if (!s_pilha_de_pe)
  {
    if (!subir_pilha()) {
      // Preserves C6_MUDO: it says WHAT to do (reflash the coprocessor), and
      // "failed" would send the user off checking the password forever.
      if (s_estado != TAB5_WIFI_C6_MUDO) { s_estado = TAB5_WIFI_FALHOU; }
      return;
    }
    s_pilha_de_pe = true;
    if (s_ao_subir) { s_ao_subir(); }   // whoever waits for the link wakes now
  }

  s_tentativas = 0;
  const modo_t depois = (s_modo == MODO_SO_RADIO) ? MODO_SO_RADIO : MODO_CONECTAR;

  // THE STRONGEST AMONG THE KNOWN NETWORKS. With more than one network saved,
  // scan before connecting: the radio comes up WITHOUT connecting (PROCURAR mode
  // holds STA_START), scans, and only then chooses. With just one, or if the
  // scan fails, it goes straight to the first (it may be a hidden network).
  s_modo = MODO_PROCURAR;
  esp_err_t err = esp_wifi_start();
  s_radio_ativo = (err == ESP_OK);
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    s_modo = MODO_PARADO;
    ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    s_estado = TAB5_WIFI_FALHOU;
    return;
  }
  if (procurar_redes()) {
    const int r = melhor_conhecida();
    if (r >= 0) { usar_rede(r); ESP_LOGI(TAG, "strongest known network: \"%s\"", s_ssid); }
    else        { ESP_LOGW(TAG, "no known network in sight - trying \"%s\"", s_ssid); }
  }
  s_modo = depois;

  aplicar_config();
  if (s_modo == MODO_SO_RADIO) { s_estado = TAB5_WIFI_DESLIGADO; return; }   // ESP-NOW on a fixed channel: no router
  s_estado = TAB5_WIFI_CONECTANDO;
  esp_wifi_connect();
}

static bool ligado_ou_ligando(void)
{
  return s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO || s_estado == TAB5_WIFI_SUBINDO;
}

// THE SCAN FOUND A KNOWN NETWORK AND THE USER WANTS WI-FI, but there is no connection (giving
// up stopped the radio, or it was out of range): connect to it. The
// radio must be started. true = connecting.
static bool conectar_conhecida_vista(void)
{
  if (!s_querer || s_modo == MODO_SO_RADIO || ligado_ou_ligando()) { return false; }
  const int r = melhor_conhecida();
  if (r < 0) { return false; }
  usar_rede(r);
  aplicar_config();
  s_tentativas = 0;
  s_modo = MODO_CONECTAR;
  s_radio_ativo = true;
  s_estado = TAB5_WIFI_CONECTANDO;
  ESP_LOGI(TAG, "known network in sight: connecting to \"%s\"", s_ssid);
  esp_wifi_connect();
  return true;
}

// SCAN (requested by the screen, or the resume). Connected, it scans without dropping the
// network. Without a connection: if a known one appears and Wi-Fi is on in
// Settings, connect to it; otherwise, the radio that was switched on just to scan is
// switched off again.
static void tratar_procurar(void)
{
  s_procurando = true;
  ++s_redes_ver;
  if (s_pilha_de_pe) {
    const bool estava = s_radio_ativo;
    const modo_t antes = s_modo;
    if (!estava) { s_modo = MODO_PROCURAR; esp_wifi_start(); }
    if (!procurar_redes()) { ESP_LOGW(TAG, "network scan refused (connecting?)"); }
    s_modo = antes;
    const bool conectou = conectar_conhecida_vista();   // connecting: the radio stays on
    if (!estava && !conectou) {
      if (s_modo == MODO_SO_RADIO || ligado_ou_ligando()) { s_radio_ativo = true; }
      else                                   { esp_wifi_stop(); }
    }
  }
  s_procurando = false;
  ++s_redes_ver;
}

// Called on every round of the task. Acts when Wi-Fi is ON IN SETTINGS and,
// even so, there is no network and no attempt in progress: after giving up
// (FALHOU) or any path that left the state at DESLIGADO without
// the user asking (s_querer is still set). On, connecting or no saved
// network cancels the schedule (the next time starts again from the short step).
//
// WAKING THE SCREEN SEARCHES IMMEDIATELY: whoever turns the Tab5 on wants the network now, and
// not at the end of a step of up to 5 min that began away from home.
static void retomar_se_preciso(void)
{
  static bool dormia = false;
  const bool dorme = tab5_power_em_repouso();
  const bool acordou = dormia && !dorme;
  dormia = dorme;
  if (!s_querer || !s_pilha_de_pe || s_modo == MODO_SO_RADIO
   || (s_estado != TAB5_WIFI_FALHOU && s_estado != TAB5_WIFI_DESLIGADO)) {
    s_retomar_ms = 0;
    return;
  }
  const uint32_t agora = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
  if (acordou) {
    s_retomar_passo = RETOMAR_MIN_MS / 2;   // doubles to the minimum just below
    s_retomar_ms = agora;
  } else if (!s_retomar_ms) {
    s_retomar_passo = RETOMAR_MIN_MS;
    s_retomar_ms = agora + s_retomar_passo;
    return;
  }
  if ((int32_t)(agora - s_retomar_ms) < 0) { return; }
  ESP_LOGI(TAG, "resume: searching for known networks");
  tratar_procurar();
  s_retomar_passo = s_retomar_passo * 2 > RETOMAR_MAX_MS ? RETOMAR_MAX_MS : s_retomar_passo * 2;
  s_retomar_ms = agora + s_retomar_passo;
}

// SWITCH TO NETWORK s_alvo (already known - the requester saved it first).
static void tratar_trocar(void)
{
  const int r = achar_rede(s_alvo);
  if (r < 0) { return; }
  usar_rede(r);
  if (!s_pilha_de_pe) { tratar_ligar(); return; }
  s_tentativas = 0;
  s_modo = MODO_CONECTAR;
  // CONECTANDO before the disconnect: it is what makes ao_evento recognise the drop
  // from the old network, which may arrive before this function finishes.
  s_estado = TAB5_WIFI_CONECTANDO;
  if (!s_radio_ativo) {
    aplicar_config();
    s_radio_ativo = (esp_wifi_start() == ESP_OK);   // STA_START connects
  } else {
    esp_wifi_disconnect();      // the old network's drop doesn't count (see ao_evento)
    aplicar_config();
    esp_wifi_connect();
  }
  ESP_LOGI(TAG, "switching to \"%s\"", s_ssid);
}

// RADIO ONLY: lets go of the router and leaves the radio free for another channel.
// esp_wifi_start() covers the case where the radio was stopped (giving up away
// from the router): ESP-NOW needs it STARTED, just not associated.
// ONLY THE LINK WITH THE C6, NO ROUTER. Whoever needs the C6 but not
// Wi-Fi - Bluetooth - brings up the stack this way. Same path as LIGAR
// (subir_pilha, on the Wi-Fi task, which owns esp_hosted), without reading
// credentials and without esp_wifi_start: the Wi-Fi state stays as it was.
static void tratar_pilha(void)
{
  if (s_pilha_de_pe) { return; }
  if (subir_pilha()) {
    s_pilha_de_pe = true;
    if (s_ao_subir) { s_ao_subir(); }   // whoever waits for the link wakes now
  } else if (s_estado != TAB5_WIFI_C6_MUDO) {
    ESP_LOGE(TAG, "C6 stack did not come up - the requester (Bluetooth) has no radio");
  }
}

static void tratar_so_radio(void)
{
  if (!s_pilha_de_pe) { return; }
  s_modo = MODO_SO_RADIO;
  s_tentativas = 0;
  esp_wifi_start();       // already started: benign error
  s_radio_ativo = true;
  esp_wifi_disconnect();
  s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
  strcpy(s_ip, "0.0.0.0");
  s_estado = TAB5_WIFI_DESLIGADO;
  ESP_LOGI(TAG, "radio only: router released, radio up (ESP-NOW on its own channel)");
}

static void tratar_roteador(void)
{
  if (!s_pilha_de_pe) { return; }
  // Already connecting or connected (a network chosen in Settings got there first):
  // connecting again would only disrupt the attempt in progress.
  if (s_modo == MODO_CONECTAR && ligado_ou_ligando()) { return; }
  s_modo = MODO_CONECTAR;
  s_tentativas = 0;
  esp_wifi_start();
  s_radio_ativo = true;
  s_estado = TAB5_WIFI_CONECTANDO;
  esp_wifi_connect();
  ESP_LOGI(TAG, "returning to the router");
}

static void tratar_desligar(void)
{
  if (!s_pilha_de_pe) { s_estado = TAB5_WIFI_DESLIGADO; return; }

  // SO_RADIO is ESP-NOW's choice (fixed channel): it survives switching off and
  // on again, and never reconnects by itself - for the event, it counts the same as PARADO.
  if (s_modo != MODO_SO_RADIO) { s_modo = MODO_PARADO; }
  esp_wifi_disconnect();
  esp_wifi_stop();       // level 1: stack and SDIO link stay up
  s_radio_ativo = false;

  s_rssi   = TAB5_WIFI_RSSI_DESCONHECIDO;
  strcpy(s_ip, "0.0.0.0");
  // Preserves FALHOU/SEM_CREDENCIAL: when the stop came from giving up, the
  // reason has to stay on the screen. Overwriting with DESLIGADO would make it look
  // like the user switched it off - and they would never learn the password is wrong.
  if (s_estado != TAB5_WIFI_FALHOU && s_estado != TAB5_WIFI_SEM_CREDENCIAL
   && s_estado != TAB5_WIFI_C6_MUDO) {
    s_estado = TAB5_WIFI_DESLIGADO;
  }
  ESP_LOGI(TAG, "WiFi off (the C6 stays powered, quick to switch back on)");
}

// ENDLESS CONNECTING. Each attempt ends in an event (IP or drop), and it is the
// drop that counts attempts up to giving up. If esp_wifi_connect is
// refused or the event is lost on the way from the C6, neither comes: the
// state stayed at CONECTANDO forever, with no giving up and no resume.
// 60 s comfortably covers the 6 attempts (~4 s each, with the pause).
#define CONECTANDO_PRAZO_MS 60000u
static void vigiar_conectando(void)
{
  static uint32_t desde = 0;
  if (s_estado != TAB5_WIFI_CONECTANDO || s_modo != MODO_CONECTAR) { desde = 0; return; }
  const uint32_t agora = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
  if (!desde) { desde = agora | 1u; return; }
  if (agora - desde < CONECTANDO_PRAZO_MS) { return; }
  desde = 0;
  ESP_LOGW(TAG, "%u s connecting with no response - giving up; the resume will search again",
           (unsigned)(CONECTANDO_PRAZO_MS / 1000));
  s_estado = TAB5_WIFI_FALHOU;
  tratar_desligar();
}

// On every round of this task: idle, traffic and requests decide the modem sleep
// (see WHEN IDLE, MAXIMUM SAVING and SCREEN ON). aplicar_economia() only goes to the
// C6 when the mode changes; the RPC to the C6 is from here.
static void seguir_repouso(void)
{
  if (s_pilha_de_pe && s_estado == TAB5_WIFI_CONECTADO) { aplicar_economia(); }
  else if (s_pilha_de_pe && tab5_power_em_repouso() != s_ps_repouso) { aplicar_economia(); }
}

// The pace of this task: with the screen on and connected, 250 ms (dynamic
// power save needs to see traffic arriving); otherwise, the usual 5 s.
static uint32_t prazo_da_volta_ms(void)
{
  return (s_estado == TAB5_WIFI_CONECTADO && !tab5_power_em_repouso()) ? 250 : 5000;
}

static void vTaskWifi(void*)
{
  comando_t c;
  for (;;)
  {
    // Waits with a timeout instead of portMAX_DELAY: the timeout is the heartbeat that
    // updates the RSSI. Going to the C6 over SDIO is expensive, hence 5s - the status bar
    // reads the cache and never goes to the radio.
    if (xQueueReceive(s_fila, &c, pdMS_TO_TICKS(prazo_da_volta_ms())) == pdTRUE)
    {
      if      (c == CMD_LIGAR)    { tratar_ligar(); }
      else if (c == CMD_SO_RADIO) { tratar_so_radio(); }
      else if (c == CMD_ROTEADOR) { tratar_roteador(); }
      else if (c == CMD_ECONOMIA) { if (s_pilha_de_pe) { aplicar_economia(); } }
      else if (c == CMD_PILHA)    { tratar_pilha(); }
      else if (c == CMD_PROCURAR) { tratar_procurar(); }
      else if (c == CMD_TROCAR)   { tratar_trocar(); }
      else if (c == CMD_RECONECTAR) {
        // The 1 s pause between attempts (the router refused, the signal
        // flickered), now outside the event task. If in the meantime
        // someone asked to go down or to release the router, don't reconnect.
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_modo == MODO_CONECTAR && s_estado == TAB5_WIFI_CONECTANDO) { esp_wifi_connect(); }
      }
      else                        { tratar_desligar(); }
      vigiar_conectando();
      retomar_se_preciso();
      seguir_repouso();
      continue;
    }
    vigiar_conectando();
    retomar_se_preciso();
    seguir_repouso();

    // When idle nobody sees the status bar: measuring the signal would be a trip to the C6 over
    // SDIO every 5 s for nothing. On wake, the next round measures. And only
    // every 5 s: the round is 250 ms with the screen on (dynamic power save).
    static uint32_t s_rssi_ms = 0;
    if (s_estado == TAB5_WIFI_CONECTADO && !tab5_power_em_repouso()
     && agora_ms() - s_rssi_ms >= 5000)
    {
      s_rssi_ms = agora_ms();
      wifi_ap_record_t ap = { };
      const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);

      if (err != ESP_OK)
      {
        s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
      }
      else if (ap.rssi == 0)
      {
        // ZERO IS NOT A STRONG SIGNAL, IT IS AN UNFILLED FIELD.
        //
        // 0 dBm would be 1mW arriving at the antenna - only when touching the router. In
        // practice usable Wi-Fi lives between -40 and -85 dBm. When the C6 returns
        // zero it is because the RPC response came back empty, and showing "0 dBm" on screen
        // would be worse than showing nothing: it looks like maximum signal.
        //
        // I suspect the firmware mismatch that appears at boot
        // ("esp-hosted fw versions: host=3.0.6 coprocessor=0.0.0"). This log,
        // once per state change, is what will confirm or refute it.
        if (s_rssi != TAB5_WIFI_RSSI_DESCONHECIDO) {
          ESP_LOGW(TAG, "the C6 returned RSSI=0 (field not filled in) - "
                        "showing as unavailable");
        }
        s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
      }
      else
      {
        s_rssi = ap.rssi;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void tab5_wifi_iniciar(void)
{
  // ---------------------------------------------------------------------
  // SILENCE THE SDIO FLOOD. Born of necessity.
  // ---------------------------------------------------------------------
  // When the link with the C6 dies, esp_hosted's internal task keeps
  // trying forever and each attempt logs TWO lines at ERROR level:
  //
  //   E (23294) eh_sdio: sdio_get_tx_buffer_num: err: 263
  //   E (23295) sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107
  //
  // There are thousands per second. The console becomes unusable - you can't read
  // what happened BEFORE nor type a command to diagnose, precisely
  // when you need it most. That is what happened on the first attempt to
  // flash firmware to the C6: the log that would have explained the failure was buried by
  // its consequence.
  //
  // The first line already says it all; from the thousandth on it is noise that GETS IN THE WAY.
  // We silence both tags and take responsibility for telling the
  // story ourselves - see the single warning in subir_pilha() and the
  // "log" command, which brings the noise back when you want to see the raw output.
  //
  // This does NOT hide defects: 0x107 is ESP_ERR_TIMEOUT, and "the C6 doesn't respond" is
  // one piece of information, not ten thousand.
  esp_log_level_set("eh_sdio",  ESP_LOG_NONE);
  esp_log_level_set("sdmmc_io", ESP_LOG_NONE);

  // "SDMMC host already initialized, skipping init flow": comes out on
  // every card mount (1x/min: log and energy log). The SDMMC controller
  // is ONE for both slots - card on 0, C6 on 1 -, esp_hosted switches it
  // on at boot, and the card mount sees that and SKIPS reinitialisation. That is the
  // CORRECT behaviour (reinitialising would bring Wi-Fi down), and
  // the line just announces it. Warnings and errors for that tag still come out.
  esp_log_level_set("sdmmc_periph", ESP_LOG_WARN);

  s_fila = xQueueCreate(6, sizeof(comando_t));
  s_trava_redes = xSemaphoreCreateRecursiveMutex();

  // Priority 3: below the UI (4), so bringing up Wi-Fi never stutters
  // touch or drawing. Above the registro (1), because an association that
  // is slow to respond delays the indicator on the status bar.
  //
  // 6 KB STACK. With the C6 back, the stack bring-up
  // (esp_hosted: SDIO transport, RPC, auto-init of 'peer_data' and 'wifi')
  // runs ENTIRELY on this task, and the monitor saw 4096 drop to 228 bytes
  // free on the first connection. With 8 KB, two days of pilhas-*.csv (several
  // bring-ups, connections and reconnections) peaked at ~4.3 KB. 6 KB is the peak
  // plus ~40%; an overflow here restarts the device in the middle of a reconnect,
  // so don't go below this without a new measurement.
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskWifi, "wifi", 6144, nullptr, 3, &h, tskNO_AFFINITY);
    tab5_dono_registrar(h, DONO_SERVICO, "WiFi");   // see hal/tab5_donos.hpp
  }

  // Announces itself at boot. Without this line the service comes up invisibly, and a boot
  // log in which Wi-Fi simply doesn't appear can't distinguish "it's off
  // because the user wants it" from "the task wasn't even created". The user's WISH
  // is logged by the kernel, when deciding whether to switch it on (see sistema_iniciar) - the
  // HAL doesn't ask a service just for a log line (layering
  // audit).
  ESP_LOGI(TAG, "service up (radio off until someone asks)");
}

void tab5_wifi_ligar(void)
{
  s_querer = true;
  tab5_power_radio(true);        // I2C - that's why it's UI task only
  comando_t c = CMD_LIGAR;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_garantir_pilha(void)
{
  if (s_pilha_de_pe) { return; }
  tab5_power_radio(true);        // I2C - same rule as ligar
  comando_t c = CMD_PILHA;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_desligar(void)
{
  // The rail stays on on purpose (level 1). When we go to level 3,
  // the power cut goes in HERE, and not in the task - it must not touch I2C.
  s_querer = false;
  comando_t c = CMD_DESLIGAR;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_definir_credenciais(const char* ssid, const char* senha)
{
  // RAM only: goes into (or updates) the known list and becomes the network in use.
  if (!ssid || !ssid[0]) { return; }
  trava_redes_t trava;
  const bool aberta = (!senha || senha[0] == 0 || strcmp(senha, "-") == 0);
  int i = achar_rede(ssid);
  if (i < 0) {
    if (s_n_redes >= TAB5_WIFI_MAX_REDES) { i = TAB5_WIFI_MAX_REDES - 1; }   // full: the last one goes
    else                                  { i = s_n_redes++; }
  }
  snprintf(s_redes[i].ssid, sizeof(s_redes[0].ssid), "%s", ssid);
  s_redes[i].aberta = aberta;
  snprintf(s_senhas[i], sizeof(s_senhas[0]), "%s", aberta ? "" : senha);
  usar_rede(i);
  s_lido = true;
  ++s_redes_ver;
  ESP_LOGI(TAG, "network \"%s\" saved (%d known)", ssid, s_n_redes);
}

bool tab5_wifi_gravar_credenciais(const char* ssid, const char* senha)
{
  if (!ssid || ssid[0] == 0) { return false; }
  trava_redes_t trava;
  // Makes sure the card's list is in RAM before rewriting the file - otherwise the
  // new network would erase the others. Without a card, the network is valid for this session and the
  // file stays as it is (escrever_arquivo refuses).
  if (!s_arquivo_lido) { s_lido = false; ler_credenciais(); }
  tab5_wifi_definir_credenciais(ssid, senha);
  // RAM in plain text; only the FILE is encrypted.
  return escrever_arquivo();
}

bool tab5_wifi_esquecer(const char* ssid)
{
  trava_redes_t trava;
  // Forget only what can also be removed from the FILE - otherwise the network would come back
  // on the next boot.
  if (!s_arquivo_lido) { s_lido = false; ler_credenciais(); }
  if (!s_arquivo_lido) { return false; }
  const int i = achar_rede(ssid);
  if (i < 0) { return false; }
  for (int k = i; k + 1 < s_n_redes; ++k) {
    s_redes[k] = s_redes[k + 1];
    memcpy(s_senhas[k], s_senhas[k + 1], sizeof(s_senhas[0]));
  }
  --s_n_redes;
  memset(s_senhas[s_n_redes], 0, sizeof(s_senhas[0]));
  for (int k = 0; k < s_n_vistas; ++k) { s_vistas[k].conhecida = achar_rede(s_vistas[k].ssid) >= 0; }
  ++s_redes_ver;
  return escrever_arquivo();
}

void tab5_wifi_procurar(void)
{
  if (!s_fila) { return; }
  tab5_wifi_garantir_pilha();   // rail I2C: from the UI task
  comando_t c = CMD_PROCURAR;
  s_procurando = true;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_conectar(const char* ssid)
{
  if (!s_fila || !ssid) { return; }
  snprintf(s_alvo, sizeof(s_alvo), "%s", ssid);
  s_querer = true;   // choosing a network means asking for Wi-Fi
  comando_t c = CMD_TROCAR;
  xQueueSend(s_fila, &c, 0);
}

bool     tab5_wifi_procurando(void)  { return s_procurando; }
int      tab5_wifi_n_vistas(void)    { return s_n_vistas; }
bool     tab5_wifi_vista(int i, tab5_wifi_visto_t* out)
{
  trava_redes_t trava;
  if (!out || i < 0 || i >= s_n_vistas) { return false; }
  *out = s_vistas[i];
  return true;
}
int      tab5_wifi_n_conhecidas(void) { return s_n_redes; }
bool     tab5_wifi_conhecida(const char* ssid) { return achar_rede(ssid) >= 0; }
uint32_t tab5_wifi_redes_versao(void) { return s_redes_ver; }

bool tab5_wifi_rede_guardada(int i, char* ssid, size_t tam_ssid, char* senha, size_t tam_senha)
{
  trava_redes_t trava;
  if (i < 0 || i >= s_n_redes) { return false; }
  snprintf(ssid, tam_ssid, "%s", s_redes[i].ssid);
  snprintf(senha, tam_senha, "%s", s_redes[i].aberta ? "" : s_senhas[i]);
  return true;
}

bool tab5_wifi_cifrar_texto(const char* claro, char* hex, size_t tam_hex) { return cifrar_senha(claro, hex, tam_hex); }
bool tab5_wifi_decifrar_texto(const char* hex, char* claro, size_t tam_claro, bool* legado) { return decifrar_senha(hex, claro, tam_claro, legado); }

bool tab5_wifi_pilha_de_pe(void) { return s_pilha_de_pe; }
void tab5_wifi_ao_subir(void (*fn)(void)) { s_ao_subir = fn; }
bool tab5_wifi_c6_enlace(void)    { return s_c6_enlace; }
bool tab5_wifi_c6_nosso(void)     { return s_c6_nosso; }
bool tab5_wifi_so_radio(void)     { return s_modo == MODO_SO_RADIO; }
bool tab5_wifi_radio_ativo(void)  { return s_radio_ativo; }

bool tab5_wifi_receptor_pedido(void) { return __atomic_load_n(&s_acordado, __ATOMIC_ACQUIRE) != 0; }

static uint32_t s_precisam = 0;   // tab5_wifi_precisar: bits of whoever needs the network
void tab5_wifi_precisar(uint32_t quem, bool sim)
{
  if (sim) { __atomic_fetch_or(&s_precisam, quem, __ATOMIC_ACQ_REL); }
  else     { __atomic_fetch_and(&s_precisam, ~quem, __ATOMIC_ACQ_REL); }
}
bool tab5_wifi_precisada(void) { return __atomic_load_n(&s_precisam, __ATOMIC_ACQUIRE) != 0; }

void tab5_wifi_manter_acordado(uint32_t quem, bool sim)
{
  // ATOMIC: ESP-NOW, CSI and Farejo declare RECURSO_RADIO_ESCUTA and the kernel
  // applies receiver-on (kernel/recursos.h); the kernel and the console radio command
  // call this from different tasks. With an ordinary read-modify-write, one would clear the bit the
  // other had just set (CSI lost receiver-on).
  const uint32_t antes = sim ? __atomic_fetch_or(&s_acordado, quem, __ATOMIC_ACQ_REL)
                             : __atomic_fetch_and(&s_acordado, ~quem, __ATOMIC_ACQ_REL);
  const uint32_t depois = sim ? (antes | quem) : (antes & ~quem);
  if (depois == antes || !s_fila) { return; }
  comando_t c = CMD_ECONOMIA;   // the RPC to the C6 belongs to the Wi-Fi task
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_modo_so_radio(bool ligar)
{
  comando_t c = ligar ? CMD_SO_RADIO : CMD_ROTEADOR;
  xQueueSend(s_fila, &c, 0);
}


tab5_wifi_estado_t tab5_wifi_estado(void) { return s_estado; }
int8_t             tab5_wifi_rssi(void)   { return s_rssi; }
const char*        tab5_wifi_ssid(void)   { return s_ssid; }
const char*        tab5_wifi_ip(void)     { return s_ip; }

const char* tab5_wifi_descricao(void)
{
  switch (s_estado)
  {
    case TAB5_WIFI_DESLIGADO:      return "WiFi";
    case TAB5_WIFI_SUBINDO:        return N_("ligando");
    case TAB5_WIFI_CONECTANDO:     return N_("conectando");
    case TAB5_WIFI_CONECTADO:      return s_ssid;
    case TAB5_WIFI_SEM_CREDENCIAL: return N_("sem rede");
    case TAB5_WIFI_FALHOU:         return N_("falhou");
    case TAB5_WIFI_C6_MUDO:        return N_("C6 nao responde");
  }
  return "WiFi";
}
