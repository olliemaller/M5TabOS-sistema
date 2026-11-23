// Berry module 'gnss' (the satellites the receiver sees). See berry_apps.cpp
// (the engine) and berry_interno.hpp.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"   // xQueueCreateWithCaps - the gnss queue in PSRAM
#include "esp_heap_caps.h"
#include "tab5_gps.hpp"
#include "nmea.h"
#include "berry_interno.hpp"

// ---------------------------------------------------------------------------
// MODULE 'gnss' - the constellation: the satellites the receiver sees (GSV)
// ---------------------------------------------------------------------------
// sistema.gps() gives the POSITION (the service already parses RMC/GGA). The satellites
// come from the GSV sentences, which only reach whoever SUBSCRIBES to the GPS service - and it
// accepts a single subscriber (today: the C Azimutal, which does not exist in the
// berry variant). gnss.ligar() subscribes; the rest read.
//
// THE SCRIPT ITSELF DRAINS THE QUEUE: each gnss.satelites() (or
// gnss.tempo()) reads what has arrived since last time and passes it through the same parser
// as Azimutal. No new task - but call it at least every second or two
// (desenhar with '# periodo: 1000', or a fundo(1000)): the queue holds ~2 s
// of sentences and, when full, whatever arrives afterwards is lost and the snapshot goes stale.
//
// ONE OWNER: the app that turned it on. Its machine being deleted releases the subscription by itself.
static QueueHandle_t       s_gnss_fila = nullptr;   // created on the first ligar and kept (see gnss_soltar)
static const berry_app_t*  s_gnss_dono = nullptr;
static SatelliteSnapshot_t s_gnss_foto;            // the last complete round
static SatelliteSnapshot_t s_gnss_rodada;          // the one the parser is building
static bool                s_gnss_tem_foto = false;
static uint32_t            s_gnss_versao = 0;      // changes on every new round
static NMEARawSentence_t   s_gnss_crua;

// The queue is NOT deleted on release: the GPS task may be in the middle of an
// xQueueSend into it (it reads the subscriber and sends without a lock). It stays allocated (~3 KB
// of PSRAM) and is reused on the next ligar.
void gnss_soltar(const berry_app_t* a)
{
  if (!s_gnss_dono || (a && s_gnss_dono != a)) { return; }
  tab5_gps_dessinscrever();
  s_gnss_dono = nullptr;
  s_gnss_tem_foto = false;
}

static void gnss_esvaziar(void)
{
  if (!s_gnss_dono || !s_gnss_fila) { return; }
  while (xQueueReceive(s_gnss_fila, &s_gnss_crua, 0) == pdPASS) {
    if (strncmp(s_gnss_crua.sentence + 3, "GSV", 3) != 0) { continue; }   // GP/GL/GA/GB + GSV
    if (nmea_parse_gsv(s_gnss_crua.sentence, &s_gnss_rodada)) {
      s_gnss_foto = s_gnss_rodada;
      s_gnss_tem_foto = true;
      ++s_gnss_versao;
    }
  }
}

static bool gnss_meu(void) { return C->app && s_gnss_dono == C->app; }

// ligar() - true = subscribed. false = no app (console) or the GPS already has another
// subscriber. Without a GPS on the Grove it still turns on: the sentences arrive when
// it comes up.
static int g_ligar(bvm* vm)
{
  bool ok = gnss_meu();
  if (!ok && C->app && !s_gnss_dono) {
    if (!s_gnss_fila) { s_gnss_fila = xQueueCreateWithCaps(24, sizeof(NMEARawSentence_t), MALLOC_CAP_SPIRAM); }
    if (s_gnss_fila) {
      xQueueReset(s_gnss_fila);
      if (tab5_gps_assinar(s_gnss_fila)) {
        nmea_reset();
        memset(&s_gnss_rodada, 0, sizeof(s_gnss_rodada));
        s_gnss_tem_foto = false;
        s_gnss_dono = C->app;
        ok = true;
      }
    }
  }
  be_pushbool(vm, ok);
  be_return(vm);
}

static int g_desligar(bvm* vm) { if (gnss_meu()) { gnss_soltar(C->app); } be_return_nil(vm); }
static int g_ligado(bvm* vm)   { be_pushbool(vm, gnss_meu()); be_return(vm); }

static const char* gnss_sistema(GnssType t)
{
  switch (t) {
    case GPS: return "GPS"; case GLONASS: return "GLONASS"; case GALILEO: return "GALILEO";
    case BEIDOU: return "BEIDOU"; case QZSS: return "QZSS"; case SBAS: return "SBAS";
    default: return "?";
  }
}

// satelites() - list of {sis, prn, elev, azim, snr, pos, idade}: 'pos' false =
// the receiver knows about the satellite but does not yet know where it is (do not draw it);
// 'idade' = seconds since the last observed position (for RECORDING, only the
// fresh ones: the parser holds the last one when a GSV arrives without a position). nil =
// off or no complete round yet.
static int g_satelites(bvm* vm)
{
  if (!gnss_meu()) { be_return_nil(vm); }
  gnss_esvaziar();
  if (!s_gnss_tem_foto) { be_return_nil(vm); }
  const uint32_t agora = pdTICKS_TO_MS(xTaskGetTickCount());
  lista_nova(vm);
  for (uint8_t i = 0; i < s_gnss_foto.count && i < MAX_SATS_TRACKED; ++i) {
    const SatelliteInfo_t& s = s_gnss_foto.satellites[i];
    mapa_novo(vm);
    m_txt(vm, "sis", gnss_sistema(s.type));
    m_int(vm, "prn", s.prn);
    m_int(vm, "elev", s.elevation);
    m_int(vm, "azim", s.azimuth);
    m_int(vm, "snr", s.snr);
    m_bool(vm, "pos", s.tem_posicao);
    m_int(vm, "idade", s.tem_posicao ? (int)((agora - s.pos_ms) / 1000) : -1);
    fim(vm);                 // the map remains: list, list data, map
    be_data_push(vm, -2);    // the map goes into the list's data
    be_pop(vm, 1);
  }
  fim(vm);
  be_return(vm);
}

// versao() - changes on every new complete round (to redraw/record only what is new).
static int g_versao(bvm* vm)
{
  if (gnss_meu()) { gnss_esvaziar(); }
  be_pushint(vm, (bint)s_gnss_versao);
  be_return(vm);
}

// tempo() - the GPS's OWN date and time, in UTC (RMC): {valida, fix, ano,
// mes, dia, hora, min, seg}. It does not depend on the device clock being right.
static int g_tempo(bvm* vm)
{
  const nmea_tempo_t t = tab5_gps_tempo();
  mapa_novo(vm);
  m_bool(vm, "valida", t.valida);
  m_bool(vm, "fix", t.fix);
  m_int(vm, "ano", t.ano); m_int(vm, "mes", t.mes); m_int(vm, "dia", t.dia);
  m_int(vm, "hora", t.hora); m_int(vm, "min", t.min); m_int(vm, "seg", t.seg);
  fim(vm);
  be_return(vm);
}

const membro_t BERRY_MOD_GNSS[] = {
  { "ligar", g_ligar }, { "desligar", g_desligar }, { "ligado", g_ligado },
  { "satelites", g_satelites }, { "versao", g_versao }, { "tempo", g_tempo },
  { nullptr },
};
