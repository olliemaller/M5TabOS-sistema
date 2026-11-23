#include "tab5_espnow.hpp"
#include "idioma.h"
#include "tab5_farejo.hpp"   // relays PRONTO (the C6's version and capabilities)
#include "tab5_donos.hpp"
#include "tab5_wifi.hpp"
#include "tab5_sd.hpp"
#include "tab5_power.hpp"   // tab5_power_em_repouso
#include "notificacao.h"
#include "ajustes.h"
#include "app.h"
#include "tab5_hora.hpp"

#include "eh_host_feat_peer_data.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static const char* TAG = "espnow";

#define ESPNOW_PASTA     "ESPNOW"
#define ESPNOW_ARQUIVO   "historico.txt"
#define REPEDIR_MS       5000     // no PRONTO: ask again
#define CANAL_MS         10000    // the channel follows the router: check from time to time

// ---------------------------------------------------------------------------
// STATE - written by the service task, read by the UI (copy, lock)
// ---------------------------------------------------------------------------
static EXT_RAM_BSS_ATTR espnow_msg_t s_hist[ESPNOW_HIST];   // ring: s_ini is the oldest
static int      s_n = 0, s_ini = 0;
static uint32_t s_total = 0;
// Stamped by THIS session's clock (the SD card ones aren't): only these are
// corrected when the clock jumps - see ao_saltar_relogio().
static bool     s_da_sessao[ESPNOW_HIST];
static bool     s_lendo_cartao = false;
static int      s_nao_lidas = 0;
static SemaphoreHandle_t s_trava = nullptr;

static volatile bool    s_pronto = false;
static volatile uint8_t s_canal  = 0;
static uint8_t          s_mac[6] = { };
static int              s_notif  = -1;

// Devices heard (only to count "how many"), and the last frame.
#define VISTOS_MAX 16
static uint8_t  s_vistos[VISTOS_MAX][6];
static int      s_n_vistos = 0;
static int64_t  s_ultimo_rx_us = 0;

// POWER. The C6's receiver only stays on all the time when
// ESP-NOW is actually in use - see quer_receptor_ligado(). Otherwise, modem
// sleep, like any WiFi.
#define CONVERSA_VIVA_US (5LL * 60 * 1000000)   // 5 min since the last message
static int64_t       s_ultimo_tx_us = 0;
static volatile bool s_janela = false;          // the ESP-NOW app is open

static inline void travar(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static inline void destravar(void) { xSemaphoreGive(s_trava); }

// ---------------------------------------------------------------------------
// THE SERVICE QUEUE - just one, for what arrives from the C6 and what the UI asks for
// ---------------------------------------------------------------------------
// peer_data notifications arrive in esp_hosted's receive task, which
// can't wait for anything (it delivers ALL the WiFi traffic). The callback
// only copies to the queue; the thinking is done by the service task. Sending comes from the
// UI via the SAME queue, because eh_host_peer_data_send() BLOCKS until the C6
// replies (up to 5 s) - on the UI that would be a frozen screen.
typedef struct {
  uint32_t tipo;                         // C6_MSG_* received, or EV_ENVIAR
  uint16_t n;
  uint8_t  mac[6];                       // destination for EV_ENVIAR/EV_VARRER (FF.. = broadcast)
  uint8_t  dados[7 + C6_TEXTO_MAX + 1];
} evento_t;

#define EV_ENVIAR 0x1000
#define EV_CANAL  0x1001   // dados[0] = fixed channel (0 = the router's)
#define EV_NADA   0x1002   // just wakes the loop (re-evaluate the receiver)
#define EV_VARRER 0x1003   // mac = destination, dados = text: sends on channels 1 to 13

static volatile uint8_t s_canal_fixo = 0;
static int s_notif_canal = -1;   // grey pill while WiFi is detached from the router

static QueueHandle_t s_fila = nullptr;

static void ao_peer_data(uint32_t msg_id, const uint8_t* dados, size_t n, void*)
{
  if (!s_fila) { return; }
  // STATIC: this callback runs in esp_hosted's receive task, whose
  // stack isn't ours - 270 more bytes on it is a gamble. Only that task calls
  // here (one at a time), and xQueueSend copies before returning.
  static evento_t ev;
  ev.tipo = msg_id;
  ev.n = (uint16_t)(n > sizeof(ev.dados) ? sizeof(ev.dados) : n);
  if (dados && ev.n) { memcpy(ev.dados, dados, ev.n); }
  xQueueSend(s_fila, &ev, 0);   // never waits: losing a frame is better than stalling WiFi
}

// ---------------------------------------------------------------------------
// HISTORY
// ---------------------------------------------------------------------------
static void publicar_notif(void)
{
  if (s_notif < 0) { return; }
  char resumo[NOTIF_RESUMO];
  if (s_nao_lidas > 0) {
    // Who sent the last one: the last two bytes of the MAC are enough to tell apart
    // the devices on a table.
    const espnow_msg_t* m = &s_hist[(s_ini + s_n - 1) % ESPNOW_HIST];
    snprintf(resumo, sizeof(resumo), tr("%d de %02X%02X"), s_nao_lidas, m->mac[4], m->mac[5]);
  } else {
    resumo[0] = 0;
  }
  notif_publicar(s_notif, s_nao_lidas, NOTIF_INFO, resumo);
}

// With the lock held. Returns the index (in the ring) of the stored message.
static int guardar(const espnow_msg_t* m)
{
  int k;
  if (s_n < ESPNOW_HIST) { k = (s_ini + s_n) % ESPNOW_HIST; ++s_n; }
  else                   { k = s_ini; s_ini = (s_ini + 1) % ESPNOW_HIST; }
  s_hist[k] = *m;
  s_da_sessao[k] = !s_lendo_cartao;
  ++s_total;
  return k;
}

// One line per message: epoch, MAC, flags and the text - with no TAB or line
// break inside it. Flags: m/r (mine/received), d/u (broadcast/direct) - IN
// UPPER CASE (D/U) when it came in the Espressif format, which keeps readable the
// history written before the dialect existed -, and the send state.
// Through the system writer (tab5_sd_anexar, urgent): the service task no longer
// mounts the SD card for every message.
static void gravar_no_cartao(const espnow_msg_t* m)
{
  static char txt[sizeof(m->texto)];   // static: only the service task calls
  size_t k = 0;
  for (const char* c = m->texto; *c && k < sizeof(txt) - 1; ++c) {
    // The record is ONE line: a tab becomes a space and a line break
    // (the conversation has lines) goes as 0x1F, which becomes \n again when read.
    txt[k++] = *c == '\n' ? '\x1f' : (*c == '\t' || *c == '\r') ? ' ' : *c;
  }
  txt[k] = 0;
  static char linha[sizeof(txt) + 64];
  snprintf(linha, sizeof(linha), "%lu\t%02x%02x%02x%02x%02x%02x\t%c%c%u\t%s\n", (unsigned long)m->quando,
           m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5],
           m->minha ? 'm' : 'r',
           m->espressif ? (m->difusao ? 'D' : 'U') : (m->difusao ? 'd' : 'u'),
           (unsigned)m->envio, txt);
  tab5_sd_anexar(ESPNOW_PASTA, ESPNOW_ARQUIVO, linha, nullptr, true);
}

// At service boot: the last ESPNOW_HIST messages from the SD card come back into
// RAM, and the conversation opens with what has already been said.
//
// ONLY THE END OF THE FILE, AND WITHOUT THE LOCK DURING THE READ. The
// history only grows, and it used to be read WHOLE with the lock held - the
// UI, which asks for the messages on every app draw, was left
// waiting for the SD card, more and more as the file grew. Now:
// skip straight to the last ~ESPNOW_HIST x 300 bytes (a line doesn't exceed
// ~290: epoch, MAC, flags and up to 250 of text), discard the first line
// (probably cut in the middle) and lock only for the instant of storing each one.
#define ESPNOW_CAUDA_BYTES ((long)ESPNOW_HIST * 300)
static void carregar_do_cartao(void)
{
  if (!tab5_sd_montar()) { return; }
  s_lendo_cartao = true;
  FILE* f = tab5_sd_abrir(ESPNOW_PASTA, ESPNOW_ARQUIVO, "r");
  if (f) {
    static char linha[400];   // static: the stack belongs to the SD card mount
    int lidas = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
      const long tam = ftell(f);
      if (tam > ESPNOW_CAUDA_BYTES && fseek(f, tam - ESPNOW_CAUDA_BYTES, SEEK_SET) == 0) {
        fgets(linha, sizeof(linha), f);   // the piece of the cut line
      } else {
        fseek(f, 0, SEEK_SET);
      }
    }
    while (fgets(linha, sizeof(linha), f)) {
      static espnow_msg_t m;
      memset(&m, 0, sizeof(m));
      unsigned long quando = 0;
      unsigned int mac[6] = { };
      char quem = 0, dif = 0;
      unsigned int envio = 0;
      int pos = 0;
      if (sscanf(linha, "%lu\t%2x%2x%2x%2x%2x%2x\t%c%c%u\t%n", &quando,
                 &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5],
                 &quem, &dif, &envio, &pos) < 10 || pos <= 0) { continue; }
      m.quando = (uint32_t)quando;
      for (int i = 0; i < 6; ++i) { m.mac[i] = (uint8_t)mac[i]; }
      m.minha = (quem == 'm');
      m.difusao = (dif == 'd' || dif == 'D');
      m.espressif = (dif == 'D' || dif == 'U');   // without this, after boot everything became "plain text"
      // A send left pending when the device switched off will never be
      // confirmed: show it as failed, not as "sending" forever.
      m.envio = (uint8_t)((envio == ESPNOW_ENVIO_PENDENTE) ? ESPNOW_ENVIO_FALHOU : envio);
      snprintf(m.texto, sizeof(m.texto), "%s", linha + pos);
      const size_t n = strlen(m.texto);
      if (n && m.texto[n - 1] == '\n') { m.texto[n - 1] = 0; }
      for (char* c = m.texto; *c; ++c) { if (*c == '\x1f') { *c = '\n'; } }   // the line break (see gravar_no_cartao)
      // The SD card only keeps the text: the "raw" of what came from there is the text itself.
      m.n = (uint16_t)strlen(m.texto);
      memcpy(m.bruto, m.texto, m.n);
      travar();
      guardar(&m);
      destravar();
      ++lidas;
    }
    fclose(f);
    if (lidas) { ESP_LOGI(TAG, "SD card: %d message(s) in the history", lidas); }
  }
  s_lendo_cartao = false;
  tab5_sd_desmontar();
}

static void contar_visto(const uint8_t mac[6])
{
  for (int i = 0; i < s_n_vistos; ++i) { if (memcmp(s_vistos[i], mac, 6) == 0) { return; } }
  if (s_n_vistos < VISTOS_MAX) { memcpy(s_vistos[s_n_vistos++], mac, 6); }
}

static uint32_t agora_epoch(void) { return tab5_hora_epoch(); }

// ---------------------------------------------------------------------------
// THE TASK
// ---------------------------------------------------------------------------
static void tratar(const evento_t* ev);

// ESP-NOW dropped (radio stopped, C6 without a link): a pending send will never be
// confirmed - the C6 only answers for what it has in its own queue. It becomes
// a failure, just like carregar_do_cartao does at boot.
static void falhar_pendentes(void)
{
  for (;;) {
    bool achou = false;
    travar();
    for (int j = 0; j < s_n && !achou; ++j) {
      const espnow_msg_t* m = &s_hist[(s_ini + j) % ESPNOW_HIST];
      achou = m->minha && m->envio == ESPNOW_ENVIO_PENDENTE;
    }
    destravar();
    if (!achou) { return; }
    static evento_t falha;   // same path as the C6's ESTADO: marks and writes to the SD card
    memset(&falha, 0, sizeof(falha));
    falha.tipo = C6_MSG_ESTADO;
    falha.n = 7;
    memset(falha.dados, 0xFF, 6);
    tratar(&falha);
  }
}

// Back to the router: the channel becomes its channel (the loop checks).
static void voltar_ao_roteador(void)
{
  tab5_wifi_modo_so_radio(false);
  notif_limpar(s_notif_canal);
  ESP_LOGI(TAG, "channel: the router's");
}

// Puts the radio on channel c (1 to 13) and returns whether the C6 confirmed. Doesn't write the
// setting or touch the user's "fixed" channel: the caller decides (EV_CANAL
// writes, EV_VARRER just passes through).
static bool ir_para_canal(uint8_t c)
{
  // The radio only changes channel DETACHED from the router: ask and wait for it to let go.
  tab5_wifi_modo_so_radio(true);
  for (int k = 0; k < 30 && tab5_wifi_estado() == TAB5_WIFI_CONECTADO; ++k) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // CHECK ON THE C6, and repeat. A request in the middle of a
  // reconnection (quick tap router -> 1), the C6 silently refuses the change and
  // the radio stays on the previous channel - seen in the log: "canal 1 -> 11". The
  // only truth is the channel the C6 says it is on.
  uint8_t real = 0; wifi_second_chan_t sec;
  for (int tent = 0; tent < 5; ++tent) {
    vTaskDelay(pdMS_TO_TICKS(200));
    eh_host_peer_data_send(C6_MSG_HOST_CANAL, &c, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (esp_wifi_get_channel(&real, &sec) == ESP_OK && real == c) { break; }
  }
  s_canal = real ? real : c;
  if (real == c) { ESP_LOGI(TAG, "fixed channel %u (WiFi detached from the router)", (unsigned)c); }
  else           { ESP_LOGW(TAG, "fixed channel %u requested, but the C6 stayed on %u", (unsigned)c, (unsigned)real); }
  return real == c;
}

// VISIBLE IN THE SYSTEM: without this, detached WiFi looked like broken WiFi.
static void publicar_canal_solto(void)
{
  char resumo[NOTIF_RESUMO];
  snprintf(resumo, sizeof(resumo), tr("canal %u - sem WiFi"), (unsigned)s_canal);
  notif_publicar(s_notif_canal, 1, NOTIF_ESTADO, resumo);
}

// The C6 didn't even receive the send request: there will be no ESTADO, so the oldest
// pending message becomes a failure by the same path as ESTADO.
static void falhar_envio(void)
{
  static evento_t falha;
  memset(&falha, 0, sizeof(falha));
  falha.tipo = C6_MSG_ESTADO;
  falha.n = 7;
  memset(falha.dados, 0xFF, 6);
  falha.dados[6] = 0;
  tratar(&falha);
}

// Stores in the history the message about to go out (destination in ev->mac, text in
// ev->dados). Pending; already failed if ESP-NOW is off air.
static void guardar_envio(const evento_t* ev, espnow_msg_t* m)
{
  static const uint8_t TODOS[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  memset(m, 0, sizeof(*m));
  memcpy(m->mac, ev->mac, 6);
  m->minha = true;
  m->difusao = memcmp(ev->mac, TODOS, 6) == 0;
  m->quando = agora_epoch();
  snprintf(m->texto, sizeof(m->texto), "%.*s", (int)ev->n, (const char*)ev->dados);
  m->n = (uint16_t)strlen(m->texto);
  memcpy(m->bruto, m->texto, m->n);
  m->envio = s_pronto ? ESPNOW_ENVIO_PENDENTE : ESPNOW_ENVIO_FALHOU;
  s_ultimo_tx_us = esp_timer_get_time();

  travar();
  guardar(m);
  destravar();
  sistema_avisar(AVISO_ESPNOW);
}

// Asks the C6 to send (destination MAC + text). false = the request didn't even reach it.
static bool mandar_ao_c6(const uint8_t mac[6], const char* texto)
{
  static uint8_t buf[6 + C6_TEXTO_MAX];
  memcpy(buf, mac, 6);
  const size_t t = strlen(texto);
  memcpy(buf + 6, texto, t);
  return eh_host_peer_data_send(C6_MSG_HOST_ENVIAR, buf, 6 + t) == ESP_OK;
}

static void tratar(const evento_t* ev)
{
  switch (ev->tipo) {
    case C6_MSG_PRONTO: {
      if (ev->n < 7) { return; }
      memcpy(s_mac, ev->dados, 6);
      s_canal = ev->dados[6];
      // PRONTO v2 carries the contract version and capabilities; sniffing needs
      // to know whether this C6 can sniff/CSI/FTM/802.15.4.
      tab5_farejo_anotar_pronto(ev->dados, ev->n);
      if (!s_pronto) {
        s_pronto = true;
        ESP_LOGI(TAG, "on air: MAC %02X:%02X:%02X:%02X:%02X:%02X, channel %u",
                 s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5], (unsigned)s_canal);
        // THE FIXED CHANNEL SURVIVES THE RADIO COMING BACK. Switching WiFi off and on
        // (or giving up) stops the radio, and the C6 comes back on its default channel -
        // the app kept saying "channel N fixed" with the radio on another.
        if (s_canal_fixo) {
          static evento_t canal;
          memset(&canal, 0, sizeof(canal));
          canal.tipo = EV_CANAL;
          canal.n = 1;
          canal.dados[0] = s_canal_fixo;
          tratar(&canal);
        }
      }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_RECEBIDO: {
      if (ev->n < 8) { return; }
      static espnow_msg_t m;   // static: this task only; the stack is left for the SD card
      memset(&m, 0, sizeof(m));
      memcpy(m.mac, ev->dados, 6);
      m.difusao   = (ev->dados[6] & 1) != 0;
      m.espressif = (ev->dados[6] & 2) != 0;   // bit 1: the C6 stripped the Espressif header (see c6/ DOIS DIALETOS)
      m.minha   = false;
      m.envio   = ESPNOW_ENVIO_NADA;
      m.quando  = agora_epoch();
      size_t t = ev->n - 7;
      if (t > C6_TEXTO_MAX) { t = C6_TEXTO_MAX; }
      memcpy(m.texto, ev->dados + 7, t);
      m.texto[t] = 0;
      // The bytes as they arrived, BEFORE the treatment below (which erases control characters
      // and cuts at the '\0'): it is what 'espnow msgs' shows in hex.
      m.n = (uint16_t)t;
      memcpy(m.bruto, ev->dados + 7, t);
      // Text from another device may come with garbage at the end (a '\0' from C, a
      // line break from its keyboard): whatever isn't printable at the end goes.
      size_t n = strlen(m.texto);
      while (n && (unsigned char)m.texto[n - 1] < 0x20) { m.texto[--n] = 0; }
      // In the MIDDLE, a control character becomes a space: a format that isn't text (a struct
      // with a number after the text, for example) doesn't become a broken character in the
      // bubble. Cutting at the first '\0' already did the rest: from "struct {char
      // texto[50]; int valor;}" the text shows.
      for (size_t k = 0; k < n; ++k) {
        if ((unsigned char)m.texto[k] < 0x20) { m.texto[k] = ' '; }
      }

      travar();
      guardar(&m);
      ++s_nao_lidas;
      contar_visto(m.mac);
      s_ultimo_rx_us = esp_timer_get_time();
      publicar_notif();
      destravar();
      gravar_no_cartao(&m);
      // The text doesn't go to the log: the daily log lives on the SD card, and the conversation already
      // has its own history (gravar_no_cartao).
      ESP_LOGI(TAG, "from %02X%02X: %u character(s)", m.mac[4], m.mac[5], (unsigned)strlen(m.texto));
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_ESTADO: {
      if (ev->n < 7) { return; }
      // Sends go out in a queue, one at a time: the state that arrives is for the
      // oldest one still pending.
      travar();
      static espnow_msg_t copia;
      memset(&copia, 0, sizeof(copia));
      bool achou = false;
      for (int j = 0; j < s_n && !achou; ++j) {
        espnow_msg_t* m = &s_hist[(s_ini + j) % ESPNOW_HIST];
        if (m->minha && m->envio == ESPNOW_ENVIO_PENDENTE) {
          m->envio = ev->dados[6] ? ESPNOW_ENVIO_OK : ESPNOW_ENVIO_FALHOU;
          copia = *m;
          achou = true;
        }
      }
      destravar();
      if (achou) { gravar_no_cartao(&copia); }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case EV_ENVIAR: {
      static espnow_msg_t m;
      guardar_envio(ev, &m);
      if (!s_pronto) { gravar_no_cartao(&m); break; }
      if (!mandar_ao_c6(ev->mac, m.texto)) {
        falhar_envio();   // the C6 didn't even receive the request: there will be no ESTADO
        return;
      }
      break;
    }

    case EV_VARRER: {
      static espnow_msg_t m;
      guardar_envio(ev, &m);   // ONE message in the history, not 13
      if (!s_pronto) { gravar_no_cartao(&m); break; }
      // The previous channel: 0 = the router's. Does NOT write the setting - the sweep is
      // just passing through, what the user chose still applies.
      const uint8_t antes = s_canal_fixo;
      int enviados = 0;
      for (uint8_t c = 1; c <= 13; ++c) {
        // Without the C6 confirming the channel, the send would go out on the wrong channel: skip.
        if (!ir_para_canal(c)) { continue; }
        if (mandar_ao_c6(ev->mac, m.texto)) { ++enviados; }
        vTaskDelay(pdMS_TO_TICKS(60));   // the frame goes out before the radio changes channel
      }
      if (antes == 0) { voltar_ao_roteador(); }
      else            { ir_para_canal(antes); publicar_canal_solto(); }
      ESP_LOGI(TAG, "sweep 1-13: %d channels sent", enviados);
      if (enviados == 0) { falhar_envio(); }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case EV_CANAL: {
      const uint8_t c = ev->dados[0];
      s_canal_fixo = c;
      ajustes_definir_espnow_canal(c);   // survives a restart (only writes if it changed)
      if (c == 0) {
        voltar_ao_roteador();
      } else {
        ir_para_canal(c);
        publicar_canal_solto();
      }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    default:
      break;
  }
}

// THE RECEIVER ON NON-STOP IS EXPENSIVE, and only worth it when someone might be
// talking to the Tab5. Dozing between beacons (modem sleep), the C6 loses the
// ESP-NOW frame that arrives during the doze. Asks for the receiver on when:
//   - the channel is FIXED: the user chose ESP-NOW mode (and without a router
//     modem sleep doesn't even work);
//   - the app is open with the screen on: the conversation is in front of them;
//   - there was a message, received or sent, in the last 5 min: a live conversation,
//     and the reply has to arrive even with the app closed or the screen off.
// Otherwise the radio saves power. A message arriving during a doze may be
// lost - and the Espressif format repeats the frame, which helps.
static bool quer_receptor_ligado(void)
{
  if (!s_pronto) { return false; }
  if (s_canal_fixo) { return true; }
  if (s_janela && !tab5_power_em_repouso()) { return true; }
  const int64_t agora = esp_timer_get_time();
  const int64_t ult = (s_ultimo_rx_us > s_ultimo_tx_us) ? s_ultimo_rx_us : s_ultimo_tx_us;
  return ult && (agora - ult) < CONVERSA_VIVA_US;
}

static void vTaskEspnow(void*)
{
  // Waits for our firmware to respond: without it there is no ESP-NOW to switch on.
  // Meanwhile, a send from the UI fails immediately (doesn't hang).
  bool registrado = false;
  bool carregado = false;
  int64_t pedido_us = 0, canal_us = 0;
  static evento_t ev;

  for (;;) {
    const bool enlace = tab5_wifi_c6_nosso() && tab5_wifi_c6_enlace();
    // ESP-NOW needs the RADIO STARTED, not just the link: switching WiFi off in
    // Ajustes (Settings) stops the radio with the C6 powered and the link up.
    const bool no_ar = enlace && tab5_wifi_radio_ativo();

    if (enlace && !registrado) {
      eh_host_peer_data_register(C6_MSG_PRONTO,   ao_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_RECEBIDO, ao_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_ESTADO,   ao_peer_data, nullptr);
      registrado = true;
    }
    if (enlace && !carregado) { carregar_do_cartao(); carregado = true; sistema_avisar(AVISO_ESPNOW); }

    // Radio stopped (WiFi off in Ajustes, gave up out of the router's range): ESP-NOW
    // went down with it. When the radio comes back, the request below brings a
    // new PRONTO - and with it the fixed channel, if any.
    if (!no_ar && s_pronto) {
      s_pronto = false;
      falhar_pendentes();
      ESP_LOGI(TAG, "radio stopped - ESP-NOW off air");
      sistema_avisar(AVISO_ESPNOW);
    }

    // ASKS TO SWITCH ON, and repeats until PRONTO arrives. The C6 only switches ESP-NOW on
    // when the host says so (see "LIGAR O ESP-NOW E DECISAO DO HOST" in
    // c6/main/app_main.c) - and the host only says so after WiFi is up.
    const int64_t agora = esp_timer_get_time();
    if (no_ar && !s_pronto && (agora - pedido_us) >= (int64_t)REPEDIR_MS * 1000) {
      pedido_us = agora;
      eh_host_peer_data_send(C6_MSG_HOST_ESPNOW, nullptr, 0);
    }

    // The channel follows the router (a reconnection may change it): check from time to
    // time - while awake. When idle nobody sees the channel, and each check is
    // a trip to the C6 over SDIO; on waking, the next round checks.
    if (s_pronto && !tab5_power_em_repouso() && (agora - canal_us) >= (int64_t)CANAL_MS * 1000) {
      canal_us = agora;
      uint8_t c = 0; wifi_second_chan_t sec;
      if (esp_wifi_get_channel(&c, &sec) == ESP_OK && c && c != s_canal) {
        ESP_LOGI(TAG, "channel %u -> %u", (unsigned)s_canal, (unsigned)c);
        s_canal = c;
        sistema_avisar(AVISO_ESPNOW);
      }
    }

    // The request only goes to WiFi when it CHANGES (the API already ignores repeats).
    sistema_recurso(RECURSO_RADIO_ESCUTA, "ESP-NOW", quer_receptor_ligado());

    // NO LINK WITH THE C6 (Wi-Fi never came up, or the C6 doesn't have our
    // firmware) nothing here changes until the stack comes up - and Wi-Fi notifies at that point
    // (tab5_wifi_ao_subir -> espnow_cutucar). So it sleeps 30 s instead of
    // waking every second to ask.
    uint32_t espera_ms = enlace ? 1000 : 30000;
    // WHEN IDLE, WITH ESP-NOW READY: the channel is only checked while awake and
    // REPEDIR only applies without PRONTO - the only deadline left is the end of the
    // live conversation (the receiver goes back to dozing). Sleep until then, at most
    // 30 s, instead of waking every second. Messages, sends and Wi-Fi coming up
    // arrive through the queue and wake it immediately.
    if (enlace && s_pronto && tab5_power_em_repouso()) {
      espera_ms = 30000;
      const int64_t ult = (s_ultimo_rx_us > s_ultimo_tx_us) ? s_ultimo_rx_us : s_ultimo_tx_us;
      const int64_t falta_us = ult ? ult + CONVERSA_VIVA_US - esp_timer_get_time() : 0;
      if (falta_us > 0 && falta_us / 1000 + 1 < espera_ms) { espera_ms = (uint32_t)(falta_us / 1000 + 1); }
    }
    if (xQueueReceive(s_fila, &ev, pdMS_TO_TICKS(espera_ms)) == pdTRUE) { tratar(&ev); }
  }
}

// ---------------------------------------------------------------------------
// THE CLOCK JUMPED (see tab5_hora.hpp): this session's messages were
// stamped with the old time and move along with it. RAM only - a line that has already gone to
// the SD card keeps the time it was written.
static void ao_saltar_relogio(int32_t delta)
{
  if (!s_trava) { return; }
  int n = 0;
  travar();
  for (int j = 0; j < s_n; ++j) {
    const int k = (s_ini + j) % ESPNOW_HIST;
    if (!s_da_sessao[k] || s_hist[k].quando == 0) { continue; }
    s_hist[k].quando = (uint32_t)((int64_t)s_hist[k].quando + delta);
    ++n;
  }
  if (n) { ++s_total; }   // the screen rereads and redraws with the right days
  destravar();
  if (n) { sistema_avisar(AVISO_ESPNOW); }
}

// API
// ---------------------------------------------------------------------------
// Wi-Fi brought the stack up: wake the loop to check the link immediately.
static void espnow_cutucar(void)
{
  if (!s_fila) { return; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.tipo = EV_NADA;
  xQueueSend(s_fila, &ev, 0);
}

void tab5_espnow_iniciar(void)
{
  if (s_fila) { return; }
  s_trava = xSemaphoreCreateMutex();
  s_fila  = xQueueCreateWithCaps(12, sizeof(evento_t), MALLOC_CAP_SPIRAM);
  s_notif = notif_registrar("ESP-NOW");
  s_notif_canal = notif_registrar("ESP-NOW canal");
  notif_associar_app(s_notif_canal, "ESP-NOW");
  // It is a STATE (WiFi detached from the router), not a message: changing channel
  // must not sound like a new notification.
  notif_silenciar(s_notif_canal, true);
  if (!s_trava || !s_fila) { ESP_LOGE(TAG, "out of memory - service off air"); return; }
  // The channel chosen last time. Not applied here: what applies it is the C6's
  // PRONTO (it reapplies the fixed channel every time the radio comes back), so it's enough
  // for the wish to be in s_canal_fixo before that.
  s_canal_fixo = ajustes_espnow_canal();
  tab5_hora_ao_saltar(ao_saltar_relogio);
  tab5_wifi_ao_subir(espnow_cutucar);   // without a link the task sleeps 30 s; Wi-Fi wakes it
  // 6 KB, not 4: this task MOUNTS THE SD CARD (write/load the history),
  // and FATFS's f_mount goes deep into the stack. With 4 KB the monitor saw 140
  // bytes free and the next send overflowed the stack in the middle of the mount
  // (Guru Meditation in sdmmc_read_sectors) - peak of ~4 KB. With 8 KB, two
  // days of pilhas-*.csv gave ~2.6 KB. 6 KB covers the mount's peak with 2 KB
  // to spare; the big buffers here are static (PSRAM), not stack.
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskEspnow, "espnow", 6144, nullptr, 3, &h, tskNO_AFFINITY);
    tab5_dono_registrar(h, DONO_SERVICO, "ESP-NOW");   // see hal/tab5_donos.hpp
  }
  ESP_LOGI(TAG, "service on air (waiting for our firmware on the C6)");
}

bool    tab5_espnow_pronto(void)          { return s_pronto; }
uint8_t tab5_espnow_canal(void)           { return s_canal; }
void    tab5_espnow_meu_mac(uint8_t out[6]) { memcpy(out, s_mac, 6); }
bool    tab5_espnow_c6_compativel(void)   { return tab5_wifi_c6_nosso(); }

int tab5_espnow_n(void)
{
  if (!s_trava) { return 0; }
  travar(); const int n = s_n; destravar();
  return n;
}

bool tab5_espnow_msg(int i, espnow_msg_t* out)
{
  if (!s_trava || !out) { return false; }
  travar();
  const bool ok = (i >= 0 && i < s_n);
  if (ok) { *out = s_hist[(s_ini + i) % ESPNOW_HIST]; }
  destravar();
  return ok;
}

uint32_t tab5_espnow_total(void)   { return s_total; }
int      tab5_espnow_vistos(void)  { return s_n_vistos; }

uint32_t tab5_espnow_ms_desde_rx(void)
{
  if (!s_ultimo_rx_us) { return UINT32_MAX; }
  return (uint32_t)((esp_timer_get_time() - s_ultimo_rx_us) / 1000);
}

uint8_t tab5_espnow_canal_fixo(void) { return s_canal_fixo; }

// LOCAL, not static: the comment said "only the UI
// calls", but the console (espnow canal/enviar commands) calls from ITS task -
// two tasks on the same static buffer could send a message with the other's
// text. 266 bytes fit comfortably in both stacks (8 and 12 KB).
bool tab5_espnow_definir_canal(uint8_t canal)
{
  if (!s_fila || canal > 13) { return false; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.tipo = EV_CANAL;
  ev.n = 1;
  ev.dados[0] = canal;
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

// A single enqueueing logic: the destination and text go in the event.
static bool enfileirar(uint32_t tipo, const uint8_t* mac_ou_null, const char* texto)
{
  if (!s_fila || !texto || !texto[0]) { return false; }
  evento_t ev;   // local: see tab5_espnow_definir_canal
  ev.tipo = tipo;
  if (mac_ou_null) { memcpy(ev.mac, mac_ou_null, 6); }
  else             { memset(ev.mac, 0xFF, 6); }   // broadcast
  size_t n = strlen(texto);
  if (n > C6_TEXTO_MAX) { n = C6_TEXTO_MAX; }
  ev.n = (uint16_t)n;
  memcpy(ev.dados, texto, n);
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

bool tab5_espnow_enviar_para(const uint8_t mac[6], const char* texto)
{
  return mac && enfileirar(EV_ENVIAR, mac, texto);
}

bool tab5_espnow_enviar(const char* texto) { return enfileirar(EV_ENVIAR, nullptr, texto); }

bool tab5_espnow_enviar_todos_canais(const uint8_t* mac_ou_null, const char* texto)
{
  return enfileirar(EV_VARRER, mac_ou_null, texto);
}

bool tab5_espnow_mac_de_texto(const char* txt, uint8_t mac[6])
{
  if (!txt || strlen(txt) != 17) { return false; }
  for (int i = 0; i < 6; ++i) {
    const char* p = txt + i * 3;
    if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) { return false; }
    if (i < 5 && p[2] != ':' && p[2] != '-') { return false; }
    char par[3] = { p[0], p[1], 0 };
    mac[i] = (uint8_t)strtoul(par, nullptr, 16);
  }
  return true;
}

int tab5_espnow_nao_lidas(void) { return s_nao_lidas; }

void tab5_espnow_janela(bool aberta)
{
  s_janela = aberta;
  if (s_fila) {   // re-evaluate now, without waiting for the loop's 1 s round
    evento_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.tipo = EV_NADA;
    xQueueSend(s_fila, &ev, 0);
  }
}

void tab5_espnow_marcar_lidas(void)
{
  if (!s_trava) { return; }
  travar();
  if (s_nao_lidas) { s_nao_lidas = 0; publicar_notif(); }
  destravar();
}
