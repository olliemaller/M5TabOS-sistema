#include "tab5_gps.hpp"
#include "tab5_mutex.hpp"
#include "tab5_donos.hpp"
#include "pool_tarefas.h"
#include "parada.hpp"      // cooperative task stop (kernel)
#include "tab5_grove.hpp"
#include "recursos.h"      // sistema_recurso: holds off light sleep while the UART is receiving

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include <string.h>

static const char* TAG = "gps";

// ---- GPS wiring on the Grove (the Tab5's Port A) -------------------------
// MOVED from app_azimutal.cpp - the module is the same, only who brings up
// the UART changed from "the app, on opening" to "the service, when the
// Grove has a real GPS".
static const uart_port_t GPS_UART   = UART_NUM_1;
static const int         GPS_RX_PIN = 54;   // GPIO54 = Port A SCL, receives the GPS's TX
static const int         GPS_TX_PIN = 53;   // GPIO53 = Port A SDA
static const int         GPS_BAUD   = 9600;

static bool     s_servico_ativo = false;
static parada_t s_parada = { };   // stop request and task exit confirmation

static nmea_posicao_t s_pos   = { };
static nmea_tempo_t   s_tempo = { };
static uint32_t       s_ultima_sentenca_ms = 0;
static uint32_t       s_iniciado_em_ms     = 0;   // see the notice in tab5_gps_silencio_ms()

static QueueHandle_t s_assinante = nullptr;   // whoever wants raw sentences (Azimutal)

// LOCK FOR s_pos/s_tempo - found while reviewing "ready for more than one
// consumer at once": tab5_gps_posicao()/tab5_gps_tempo() ARE already
// documented (see the .hpp) as "any app can ask where am I", but they
// returned the whole struct (two doubles, two floats, flags - does not fit in
// one instruction) with no protection against the GPS task writing mid-copy.
// With ONE reader at a time this is hardly ever noticed (the position is
// wrong for a single frame); the stated intention is several apps reading at
// once, and then the chance of getting a "torn" value (new latitude with old
// longitude) goes up. Lazily created and never destroyed, SAME TEMPLATE as
// services/notificacao.cpp - first use always comes before any real
// contention (boot calls tab5_gps_iniciar before any app can read), so
// creating it here is safe.
static Trava s_trava;


static inline void travar(void)   { s_trava.travar(); }
static inline void destravar(void) { s_trava.destravar(); }

nmea_posicao_t tab5_gps_posicao(void)
{
  travar();
  nmea_posicao_t copia = s_pos;
  destravar();
  return copia;
}

nmea_tempo_t tab5_gps_tempo(void)
{
  travar();
  nmea_tempo_t copia = s_tempo;
  destravar();
  return copia;
}

bool tab5_gps_servico_ativo(void) { return s_servico_ativo; }

bool tab5_gps_no_ar(void)
{
  return s_ultima_sentenca_ms != 0 && (tab5_agora_ms() - s_ultima_sentenca_ms) < 15000;
}

uint32_t tab5_gps_silencio_ms(void)
{
  if (s_ultima_sentenca_ms == 0) {
    // NO sentence YET - but that may be because the service has only just
    // come up, not because the module has gone. Bug seen on hardware:
    // returning UINT32_MAX here unconditionally makes the Grove watchdog
    // (kernel/core.cpp, "GPS mute for Xs") fire on the FIRST tick (up to 2s)
    // after ANY restart - before the module even had the CHANCE to speak
    // within the 5s timeout the code thought it was giving. Result: service
    // comes up, is knocked down 2s later as "mute", comes up again, is
    // knocked down again - an endless loop, never giving the GPS real time to
    // respond.
    //
    // The right count is from WHEN THE SERVICE CAME UP, not a fixed sentinel
    // value - that way the caller (the watchdog) sees the silence really
    // growing and only gets suspicious after the WHOLE timeout has passed,
    // just as already happens when it speaks and then disappears.
    return s_servico_ativo ? (tab5_agora_ms() - s_iniciado_em_ms) : UINT32_MAX;
  }
  return tab5_agora_ms() - s_ultima_sentenca_ms;
}

bool tab5_gps_assinar(QueueHandle_t fila)
{
  if (s_assinante != nullptr) { return false; }   // one subscriber only, see the header
  s_assinante = fila;
  return true;
}

void tab5_gps_dessinscrever(void) { s_assinante = nullptr; }

// ---------------------------------------------------------------------------
// THE TASK - reads bytes from the UART, assembles sentences, parses RMC/GGA
// and passes the raw sentence on to the subscriber. It is the SAME
// per-character state machine app_azimutal used (laco_gps + laco_parser's
// dispatch), except that it is now a single loop: there is no longer a queue
// between "read" and "parse" because the SAME task does both.
// THE TASK SLEEPS ON THE UART EVENT QUEUE, not on a short timeout. It used to
// be uart_read_bytes with 50 ms: 20 wake-ups per second for a module that
// sends one burst per second at 9600 baud. The driver signals (UART_DATA)
// when the FIFO fills or the line goes quiet - a few signals per burst - and
// the task reads everything that has arrived in one go. To stop,
// tab5_gps_parar nudges the queue (gps_cutucar) and the task sees the
// request straight away.
static QueueHandle_t s_uart_ev = nullptr;   // created by the UART driver

static void gps_cutucar(void)
{
  if (!s_uart_ev) { return; }
  uart_event_t e = { };
  e.type = UART_EVENT_MAX;   // not a driver event: just wakes the task
  xQueueSend(s_uart_ev, &e, 0);
}

static void vTaskGPS_corpo(void*)
{
  char buffer[NMEA_MAX_LENGTH];
  int  idx = 0;
  bool gravando = false;

  static uint8_t entrada[256];   // block read - see the comment that used to
                                  // be in laco_gps about the per-byte cost

  while (!parada_pedida(&s_parada)) {
    uart_event_t ev;
    // No timeout: new data or the stop request (gps_cutucar) arrive on the
    // queue itself - waking every second for nothing is pointless.
    if (xQueueReceive(s_uart_ev, &ev, portMAX_DELAY) != pdTRUE) { continue; }
    if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
      // Nobody read in time: what is in the buffer is a truncated sentence.
      uart_flush_input(GPS_UART);
      xQueueReset(s_uart_ev);
      gravando = false; idx = 0;
      continue;
    }
    if (ev.type != UART_DATA) { continue; }   // the stop nudge, or another event
    int lidos;
    while ((lidos = uart_read_bytes(GPS_UART, entrada, sizeof(entrada), 0)) > 0) {   // everything that has arrived
      for (int k = 0; k < lidos; ++k) {
        const char c = (char)entrada[k];

        if (c == '$') {
          gravando = true;
          idx = 0;
          buffer[idx++] = c;
          continue;
        }
        if (!gravando) { continue; }
        if (idx >= NMEA_MAX_LENGTH - 1) { gravando = false; idx = 0; continue; }

        buffer[idx++] = c;

        if (c == '\n') {
          buffer[idx] = '\0';
          gravando = false;
          idx = 0;

          if (!nmea_checksum_ok(buffer)) {
            ESP_LOGD(TAG, "invalid checksum: %s", buffer);
            continue;
          }

          s_ultima_sentenca_ms = tab5_agora_ms();

          // The first 2 characters after the '$' vary by constellation
          // (GP=GPS, GL=GLONASS...); so we compare from the 3rd onwards.
          if (strncmp(buffer + 3, "RMC", 3) == 0) {
            nmea_tempo_t t;
            if (nmea_parse_rmc(buffer, &t)) { travar(); s_tempo = t; destravar(); }
          } else if (strncmp(buffer + 3, "GGA", 3) == 0) {
            nmea_posicao_t p;
            if (nmea_parse_gga(buffer, &p)) { travar(); s_pos = p; destravar(); }
          }

          // EVERY valid sentence goes to the subscriber, including the RMC/GGA
          // we have just processed - Azimutal filters GSV itself, as it always
          // has; it is not worth this service knowing what each subscriber wants.
          if (s_assinante) {
            NMEARawSentence_t crua;
            strncpy(crua.sentence, buffer, NMEA_MAX_LENGTH - 1);
            crua.sentence[NMEA_MAX_LENGTH - 1] = '\0';
            xQueueSend(s_assinante, &crua, 0);   // no wait: losing 1 sentence
                                                  // beats stalling the reading
          }
        }
      }
    }
  }

  parada_confirmar(&s_parada);
  // Returns: the pool slot releases the owner and goes back to sleep (kernel/pool_tarefas.h).
}

bool tab5_gps_parar(void)
{
  if (!s_servico_ativo) { return true; }

  ESP_LOGI(TAG, "stopping the service...");
  // The task reads the UART in 50 ms slices: exiting costs that in the worst case.
  if (!parada_pedir(&s_parada, 1000, gps_cutucar)) {
    ESP_LOGE(TAG, "the task did not confirm within 1s - still stopping, nothing was torn down");
    return false;
  }

  uart_driver_delete(GPS_UART);
  s_uart_ev = nullptr;   // the event queue goes along with the driver
  tab5_grove_soltar(GROVE_DONO_GPS);
  sistema_recurso(RECURSO_SERIAL_GROVE, "GPS", false);   // release only the GPS's share (counting is by name)

  s_ultima_sentenca_ms = 0;
  travar();
  memset(&s_pos,   0, sizeof(s_pos));
  memset(&s_tempo, 0, sizeof(s_tempo));
  destravar();

  s_servico_ativo = false;
  ESP_LOGI(TAG, "service stopped, Grove free");
  return true;
}

bool tab5_gps_iniciar(void)
{
  if (s_servico_ativo) {
    if (parada_em_curso(&s_parada)) {
      ESP_LOGW(TAG, "still stopping the previous service - will not start on top of it");
    } else {
      ESP_LOGI(TAG, "service already up - nothing to do");
    }
    return true;
  }

  if (!tab5_grove_tomar(GROVE_DONO_GPS)) {
    ESP_LOGE(TAG, "Grove busy - service will not start");
    return false;
  }

  uart_config_t cfg = { };
  cfg.baud_rate  = GPS_BAUD;
  cfg.data_bits  = UART_DATA_8_BITS;
  cfg.parity     = UART_PARITY_DISABLE;
  cfg.stop_bits  = UART_STOP_BITS_1;
  cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  // Do NOT touch M5.Ex_I2C here - see the same notice that used to be in
  // app_azimutal.cpp: the ExtPort1 keyboard is what uses that bus today, and
  // releasing/restarting Ex_I2C would knock it out.
  if (uart_driver_install(GPS_UART, 2048, 0, 16, &s_uart_ev, 0) != ESP_OK
   || uart_param_config(GPS_UART, &cfg) != ESP_OK
   || uart_set_pin(GPS_UART, GPS_TX_PIN, GPS_RX_PIN,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    ESP_LOGE(TAG, "could not open the Grove UART");
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // the event queue goes along with the driver
    tab5_grove_soltar(GROVE_DONO_GPS);
    return false;
  }

  travar();
  memset(&s_pos,   0, sizeof(s_pos));
  memset(&s_tempo, 0, sizeof(s_tempo));
  destravar();
  s_ultima_sentenca_ms = 0;

  if (!parada_preparar(&s_parada)) {
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // the event queue goes along with the driver
    tab5_grove_soltar(GROVE_DONO_GPS);
    ESP_LOGE(TAG, "no memory for the GPS task");
    return false;
  }
  s_iniciado_em_ms = tab5_agora_ms();   // see the notice in tab5_gps_silencio_ms()
  s_servico_ativo  = true;

  // Priority 3, same as the Mesh service: below the UI (4), above logging
  // (1) - losing a serial byte is irreversible.
  if (!pool_criar(nullptr, vTaskGPS_corpo, nullptr, "gps", 4096, 3, DONO_SERVICO, "GPS")) {
    parada_desistir(&s_parada);
    s_servico_ativo = false;
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // the event queue goes along with the driver
    tab5_grove_soltar(GROVE_DONO_GPS);
    ESP_LOGE(TAG, "no memory for the GPS task");
    return false;
  }
  // THE UART DOES NOT RECEIVE WITH THE P4 IN LIGHT SLEEP (same reason as Mesh
  // in services/tab5_mesh.cpp): hold off light sleep while the GPS runs. Only
  // here, with the task up: the failures above never held anything.
  sistema_recurso(RECURSO_SERIAL_GROVE, "GPS", true);
  ESP_LOGI(TAG, "service up (Grove %d baud, RX=%d TX=%d)",
           GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);
  return true;
}
