// The system Wi-Fi service - NOT an application.
//
// Why a service and not an app: the status bar needs the state all the time,
// and a future app (Meshtastic, network printing, NTP) will want the connection already
// up. If Wi-Fi died along with the screen of the app that switched it on, every app would
// have to reconnect - seconds of waiting on every screen change.
//
// The user's wish lives in ajustes_wifi() (NVS). This module is the ARM that
// carries out that wish; it decides nothing by itself.
//
// -------------------------------------------------------------------------
// TWO TASKS, AND THE SPLIT IS NOT ARBITRARY
// -------------------------------------------------------------------------
// The C6 rail (WLAN_PWR_EN) sits on an I2C expander, and in this system I2C
// has a single owner: the UI task (see the header of kernel/core.cpp - M5Unified's
// I2C_Class has no mutex). Therefore:
//
//   - tab5_wifi_ligar()/desligar() TOUCH I2C -> they can only be called from
//     inside the UI task. They only touch the rail and signal.
//   - everything else (waiting for the C6 to boot, SDIO, esp_wifi, DHCP) runs on this
//     module's "wifi" task, which never touches any I2C.
//
// Calling tab5_wifi_ligar() from another task doesn't error - it races on I2C, which
// is worse, because it shows up some random day as a wrong battery reading.
//
// -------------------------------------------------------------------------
// POWER-SAVING LEVELS - currently 1, on purpose
// -------------------------------------------------------------------------
// Agreed: start conservative and measure before tightening. The levels are:
//
//   1 (CURRENT) desligar = esp_wifi_stop(). The C6 radio switches off, the SDIO link
//              and the stack stay up. Switching back on is instant and never fails.
//   2          + esp_wifi_deinit() + esp_hosted_deinit(). Frees RAM, but
//              switching back on has to renegotiate SDIO.
//   3          + cut WLAN_PWR_EN. Zero consumption on the C6. It is the only one that also kills
//              BLE, and the only one where switching back on can simply fail.
//
// Measure each one with the Energia app before going up a level. The plan is to compare
// numbers, not guess - the same way it was done with the clock frequency.

#pragma once

#include <stdint.h>
#include <stddef.h>

enum tab5_wifi_estado_t
{
  TAB5_WIFI_DESLIGADO = 0,
  TAB5_WIFI_SUBINDO,      // powering the C6 and negotiating SDIO
  TAB5_WIFI_CONECTANDO,   // scanning/authenticating with the access point
  TAB5_WIFI_CONECTADO,    // has an IP
  TAB5_WIFI_SEM_CREDENCIAL,
  TAB5_WIFI_FALHOU,

  // THE LINK WITH THE C6 EXISTS, BUT IT DOESN'T ANSWER RPC.
  //
  // Its own state because it is a defect of a DIFFERENT kind: it is not a wrong password
  // nor a network that is down, and retrying is pointless. The coprocessor has
  // bad firmware and needs reflashing - physically, through the board's
  // reserved connector.
  //
  // It deserves its own name on screen: "failed" would send the user checking the password
  // for the rest of their life.
  TAB5_WIFI_C6_MUDO
};

// Creates the service task. Does NOT switch the radio on. Call once, at boot, after
// ajustes_iniciar() and tab5_sd_iniciar().
void tab5_wifi_iniciar(void);

// UI task ONLY (they touch the I2C expander). They don't block: the
// slow work happens on the service task.
void tab5_wifi_ligar(void);
void tab5_wifi_desligar(void);

// Brings up ONLY the link with the C6 (esp_hosted), without switching Wi-Fi on or searching
// for a router. For those who use the C6 for another reason - Bluetooth. Powers the radio
// rail (I2C): call from the UI task, like tab5_wifi_ligar().
// Asynchronous: tab5_wifi_pilha_de_pe() says when it is ready.
void tab5_wifi_garantir_pilha(void);

tab5_wifi_estado_t tab5_wifi_estado(void);

// Has the esp_hosted (and esp_wifi) stack already come up this boot?
//
// Different from "connected": the SDIO link with the C6 may be up with Wi-Fi
// on no network at all. Whoever wants to talk to the coprocessor - query the version,
// ask for the MAC, send data over the peer_data channel - needs this, not the connection
// state.
bool tab5_wifi_pilha_de_pe(void);
// Signals when the stack (esp_hosted + esp_wifi) has just come up - so whoever
// depends on the link with the C6 (ESP-NOW) can sleep without asking every second.
// A single function; called from the Wi-Fi task, must not block.
void tab5_wifi_ao_subir(void (*fn)(void));

// The C6 runs OUR firmware (folder c6/) - it answered the notice over the
// peer_data channel this boot. Only then does ESP-NOW exist (services/tab5_espnow): the
// factory firmware doesn't have that channel.
bool tab5_wifi_c6_nosso(void);

// RADIO-ONLY MODE - the radio stays up WITHOUT a router, so ESP-NOW can use its own
// channel (the radio is only on one channel at a time, and when associated it belongs to the
// router). No reconnection, and no giving up that stops the radio away from
// home. false = go back to connecting to the router. Doesn't block (service queue).
void tab5_wifi_modo_so_radio(bool ligar);
bool tab5_wifi_so_radio(void);

// The radio is STARTED (with a router, without one, or scanning). False after
// switching Wi-Fi off in Settings or giving up out of the router's range - which stop the
// radio but leave the C6 powered and the LINK up. It is ESP-NOW's question.
bool tab5_wifi_radio_ativo(void);

// RADIO POWER SAVING. The default is modem sleep: the C6 turns the receiver off between the
// router's beacons (a big saving, and for ordinary Wi-Fi it costs nothing).
// Whoever needs the receiver on ALL THE TIME - ESP-NOW, which loses the frame
// that arrives during a doze - asks with its own bit, and undoes it when no longer
// needed. The radio only saves power with no requests at all. Doesn't block.
//
// SERVICES DON'T USE THIS DIRECTLY: they declare RECURSO_RADIO_ESCUTA (kernel/recursos.h),
// with their own name - registration is per user, and the kernel sets the bit.
#define TAB5_ACORDADO_RECURSOS (1u << 0)   // RECURSO_RADIO_ESCUTA in use
void tab5_wifi_manter_acordado(uint32_t quem, bool sim);
// Has anyone requested the receiver on right now (ESP-NOW in conversation, CSI, sniffing...)?
bool tab5_wifi_receptor_pedido(void);

// NEEDS THE NETWORK CONNECTED, without requesting the receiver on all the time: the
// doze (services/tab5_wifi_cochilo) doesn't switch Wi-Fi off while there is a
// request. For whoever serves over the network with the screen off (the web server: without
// this, 3 min of screen off brought Wi-Fi down and the server with it).
// Modem sleep carries on: only the doze doesn't happen. One bit per requester.
// Services declare RECURSO_REDE (kernel/recursos.h); the kernel sets the bit.
#define TAB5_PRECISA_RECURSOS (1u << 0)
void tab5_wifi_precisar(uint32_t quem, bool sim);
bool tab5_wifi_precisada(void);

// Is the SDIO LINK with the C6 up? Weaker than the question above, and on
// purpose: here it is enough for the transport to have negotiated. A C6 with broken
// firmware may have a link WITHOUT working RPC - and it is in that state that
// flashing new firmware needs to be possible.
bool tab5_wifi_c6_enlace(void);

// REMOVED (layering audit): it did "s_pilha_de_pe ||
// ajustes_wifi()" in here - the HAL asking a service (ajustes.h),
// the wrong direction of dependency (see docs/ESCREVENDO_APPS.md section 8). The only
// caller was sistema_impor_preferencias() in the kernel, which legitimately has
// ajustes_wifi() available - the expression became
// "tab5_wifi_pilha_de_pe() || ajustes_wifi()" directly there, losing nothing.

// Switches the network in use without rereading the card. Called from inside
// tab5_wifi_gravar_credenciais() right after writing the file - reading the file
// back with the C6 link up brings Wi-Fi down (see the comment in
// ler_credenciais). Also exposed for anyone who only wants to update RAM without
// touching the card (no caller does that today, but write/update are
// distinct enough operations not to always force both together).
void tab5_wifi_definir_credenciais(const char* ssid, const char* senha);

// Adds (or updates) the network in wifi.txt, encrypting the password, and updates
// RAM (tab5_wifi_definir_credenciais). The other saved networks stay.
// Writes through the system port (tab5_sd_substituir): mounts the card by itself,
// and with it lent to the computer goes to the journal. Blocks - from the worker,
// never from the UI task. A password of "-" or empty saves an open network.
bool tab5_wifi_gravar_credenciais(const char* ssid, const char* senha);

// ---------------------------------------------------------------------------
// NETWORKS: several known ones, scan and switch
// ---------------------------------------------------------------------------
// wifi.txt stores several networks (SSID/password pairs, passwords encrypted). On power-up,
// the system scans around and connects to the known one with the strongest signal.
#define TAB5_WIFI_MAX_REDES   8
#define TAB5_WIFI_MAX_VISTAS  24

typedef struct { char ssid[33]; bool aberta; } tab5_wifi_rede_t;

typedef struct {
  char   ssid[33];
  int8_t rssi;
  uint8_t canal;
  bool   aberta;
  bool   conhecida;   // it is in wifi.txt
} tab5_wifi_visto_t;

// Scans the surrounding networks (asynchronous; powers the C6 if needed - I2C: call
// from the UI task). The result comes out in tab5_wifi_vista().
void     tab5_wifi_procurar(void);
bool     tab5_wifi_procurando(void);
int      tab5_wifi_n_vistas(void);
bool     tab5_wifi_vista(int i, tab5_wifi_visto_t* out);   // from strongest signal to weakest
int      tab5_wifi_n_conhecidas(void);
bool     tab5_wifi_conhecida(const char* ssid);
// Saved network number 'i' (0..n_conhecidas-1), with the password in plain text (RAM only;
// empty = open network). For whoever wants to COPY the networks (Mesh brings the
// Tab5's ones over to the node's Wi-Fi). false = index outside the list.
bool     tab5_wifi_rede_guardada(int i, char* ssid, size_t tam_ssid, char* senha, size_t tam_senha);
// The same cipher as wifi.txt (AES with this device's random key, which lives
// in NVS), so other places can store passwords without plain text.
// 'hex' needs TAB5_WIFI_CIFRA_HEX bytes. 'legado' (optional) = true if the
// text came in the old cipher (key derived from the MAC): rewrite it with cifrar_texto.
#define TAB5_WIFI_CIFRA_HEX 200
bool     tab5_wifi_cifrar_texto(const char* claro, char* hex, size_t tam_hex);
bool     tab5_wifi_decifrar_texto(const char* hex, char* claro, size_t tam_claro, bool* legado = nullptr);
// Reads (or draws and stores, on first boot) the password key. Writes to
// flash: call from boot, before any encryption (ajustes_iniciar).
bool     tab5_wifi_chave_preparar(void);
uint32_t tab5_wifi_redes_versao(void);   // changes with every scan or network saved/forgotten

// Connects to an ALREADY KNOWN network (saved with tab5_wifi_gravar_credenciais).
void     tab5_wifi_conectar(const char* ssid);
// Removes the network from wifi.txt (same port and same rules as saving above).
bool     tab5_wifi_esquecer(const char* ssid);

// Short text for the status bar ("WiFi", "conectando", the SSID...).
const char* tab5_wifi_descricao(void);

const char* tab5_wifi_ssid(void);
const char* tab5_wifi_ip(void);

// Signal unavailable: no connection, or the coprocessor didn't fill in the field.
// Do NOT use 0 as "no signal" - zero is a valid RSSI value in theory and
// would mean maximum signal, the opposite of what's intended.
#define TAB5_WIFI_RSSI_DESCONHECIDO  (-127)

// TAB5_WIFI_RSSI_DESCONHECIDO when there is no connection. Reads from the cache fed by the service task:
// esp_wifi_sta_get_ap_info() is a trip to the C6 over SDIO, too expensive to
// call on every status bar redraw.
int8_t tab5_wifi_rssi(void);

// Path of the credentials file, for the Settings app to show when it is missing.
#define TAB5_WIFI_ARQUIVO "/sdcard/apps/Sistema/wifi.txt"
