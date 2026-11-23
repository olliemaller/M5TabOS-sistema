// CORE - sound: speaker idling (releases the I2S PM lock), the pins the
// speaker shares with USB, streamed sound from the apps' background part,
// and the notification beep with a cold codec. Split from core.cpp (the
// engine); what the parts share is in core_interno.hpp.

#include "tab5_mutex.hpp"
#include "tab5_hora.hpp"
#include "notificacao.h"
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "core_interno.hpp"

static const char *TAG = "core";

// SPEAKER IDLING - found while investigating why the
// "i2s_driver" PM lock (APB_FREQ_MAX, see command "energia travas")
// never released, even with the device idle for a long time.
//
// Root cause, found by reading the library (managed_components/m5stack__
// m5unified/src/utility/Speaker_Class.cpp): the speaker's internal task
// (spk_task) only calls i2s_stop() in the idle branch when M5UNIFIED_I2S_ADC_DAC
// is defined - that is, when the chip has the internal analogue DAC of the
// classic ESP32 family. The Tab5 uses an ESP32-P4 with the external ES8388 codec over
// "real" I2S, it has no such DAC, so that branch NEVER compiles here - and
// the I2S channel, once opened by the session's first tone()/playWav(), stays
// on (and the clock lock with it) forever, playing sound or not.
//
// It is not worth patching the vendored library: a patch is lost on the
// next component update. The fix lives entirely on our side,
// through the public API: if the speaker is on (isRunning()) but idle
// (!isPlaying()) for SPEAKER_OCIOSO_MS in a row, we call M5.Speaker.end() -
// which actually deletes the I2S channel (i2s_del_channel), releasing the lock. The
// next tone()/playWav() reopens it by itself: begin() is already lazy by
// nature (same mechanism as "first use turns on the ES8388", see the note in
// ajustes.h/app_config.cpp), so no extra care is needed here.
//
// SAFE to call M5.Speaker.end()/isRunning()/isPlaying() HERE: it is the same
// UI task that is already the only one calling M5.Speaker.tone() (see the
// "BEEP DE NOTIFICACAO NOVA" block further down) - the rule is "only the UI
// task speaks I2C", and end()/begin() also talk to the codec over I2C.
//
// The 4s slack (well over the 2s BARRA_MS step used to check)
// exists so as not to turn the channel on/off between two close beeps (e.g.:
// camera shutter followed by a notification).
const uint32_t SPEAKER_OCIOSO_MS = 4000;
uint32_t       s_speaker_ocioso_desde = 0;  // 0 = playing or off

// NEW NOTIFICATION BEEP + TURNING OFF IDLE I2S,
// extended to also apply WHEN IDLE, and to respect
// muting PER SOURCE (Trilha muted and Mesh playing, for example). The question "was there news worth a
// sound?" lives entirely in notif_houve_novidade_sonora() (notificacao.cpp) -
// this file does not know, and need not know, how many sources there are, their
// names, or which are muted. This used to compare the
// aggregated TOTAL against a cache right here (s_notif_total_beep) - it had to
// change because the total does not say WHICH source went up, and without that there is no way
// to mute one without muting all.
//
// This only ran with the screen on: playing a sound held the
// I2S PM lock forever (see the big note at s_speaker_ocioso_desde
// above), which would defeat the very reason idle exists. With the
// watchdog below (M5.Speaker.end() after SPEAKER_OCIOSO_MS without playing anything), the
// idle state stopped being incompatible with the beep: it plays, and the watchdog itself
// turns it off again a few seconds later - even with the screen off. That is
// why this function became a separate function instead of staying inline in the
// repaint block: it is ALSO called from the idle branch, where there is no
// screen to draw on.
//
// NEVER draws anything here, on purpose - with the DSI torn down (idle),
// any call to M5.Display would be, at best, useless.
//
// SAFE to call M5.Speaker.tone()/isRunning()/isPlaying()/end() HERE: this is
// the UI task, the only one that may speak I2C (turns on the ES8388 on
// first use - see the big note in ajustes.h/app_config.cpp) both
// awake and idle (the INA226 and the IMU already do I2C when idle, see
// vTaskInterface further down).
// BEEP WITH A COLD CODEC. Measured on the device: the first beep after the
// speaker had been turned off (SPEAKER_OCIOSO_MS) came out silent, even at
// maximum volume; the second, within the 4 s window, played. begin() turns on the
// ES8388 and the amplifier, but the DAC only becomes audible after the codec's
// "click-free" ramp - a whole 80-90 ms tone fits inside it.
//
// Solution without blocking the UI: with a cold codec, play a single block
// "silence + tone" (the silence covers the warm-up). It lives in PSRAM and is only
// allocated on first use. Two alternating blocks, so a second request
// can swap the sound in progress without rewriting what the speaker is reading.
// SOM_AQUECER_MS is the knob: if the start is still lost, increase it.
static const uint32_t SOM_AQUECER_MS = 180;
static const uint32_t SOM_FS         = 8000;   // tones up to ~2 kHz: plenty of room
static const size_t   SOM_AMOSTRAS   = SOM_FS * 3 / 10;   // 300 ms
static int16_t*       s_som_bloco[2] = {nullptr, nullptr};
static int            s_som_vez      = 0;
static uint32_t       s_som_quente_em = 0;   // when the codec is ready

// THE SPEAKER AND USB SHARE PINS. The speaker's I2S comes out on GPIO 26
// (data) and 27 (BCK) - on the P4, the pins of USB PHY 1. ESP-IDF assumes that
// PHY always belongs to OTG (hal/esp32p4/.../gpio_ll.h: gpio_ll_func_sel and
// gpio_ll_pullup_dis): when configuring these pins it TURNS OFF the OTG pad
// (usb_pad_enable = 0) and the D+ pull-up. During flash drive mode OTG is on
// USB-C (PHY 0, swapped - see tab5_pendrive.hpp), so a beep in the middle of the
// session pulled the disk from the computer: the Mac saw the cable go (with no reset
// at all) and the Tab5 kept waiting for the next command. The end of the speaker
// task (spk_task: pinMode on GPIO 26) does the same.
//
// While flash drive mode asks, the speaker stays off and silent. What
// turns it off is this task (the only one that talks to the codec); flash drive mode waits for the
// confirmation before bringing USB up.
static volatile bool s_som_reservado = false;   // flash drive mode asked for the pins
static volatile bool s_som_parado    = false;   // the UI has already turned the speaker off
// Given by the UI when the speaker has stopped: whoever asked WAITS ON IT, and
// wakes at the instant of confirmation (it was a loop asking every 10 ms -
// the same pattern kernel/parada.hpp retired in the services).
SemaphoreHandle_t s_som_parou = nullptr;   // created in sistema_iniciar

// STREAMED SOUND (see app.h) - music from an app's background part, on channel 0
// of the speaker. The beep (tone/playRaw with channel -1) takes the HIGHEST free
// channel, so the two never compete for the same channel: the music carries on and the beep
// plays over it.
//
// THE "ONLY THE UI SPEAKS I2C" RULE STILL APPLIES: playRaw() calls
// begin() internally, and begin() with the speaker off brings up the ES8388
// over I2C. That is why the background only feeds with s_fluxo_ativo (the UI turned it on
// and nobody turned it off since), and whoever turns it off - flash drive mode's reservation and the
// idle watchdog - turns it off under the SAME lock the background holds when
// enqueuing: end() never happens in the middle of a playRaw().
static const uint8_t     SOM_CANAL_FLUXO = 0;
static volatile bool     s_fluxo_pedido = false;   // the background wants the speaker on
static volatile bool     s_fluxo_ativo  = false;   // on for the stream (the background may enqueue)
SemaphoreHandle_t s_fluxo_pronto = nullptr; // the UI has served it (created in sistema_iniciar)
static Trava s_som_trava;   // stream playRaw vs end()

// THE SPEAKER'S SLACK (06/11/2026, measured on the Tab5). M5Unified hands the
// sound to I2S in its own task ('spk_task') at priority 2 with a DMA of
// 256 x 8 frames at 48 kHz = ~43 ms of slack. When an app opens the UI
// redraws everything and the PSRAM bus goes to the screen (HIGH priority,
// hal/tab5_barramento): measured, the Music task (prio 5) took 396 ms on a
// block that takes ~44 ms, with 0 us of card time - the CPU almost stopped for audio.
// The block queue (~0.8 s) held ('fila secou 0'); what does not hold is the
// spk_task with 43 ms: the stutter when leaving and returning to the app. Here:
//   priority 6    above the UI (4) and Music (5): it only copies and mixes
//   DMA 512 x 8   ~85 ms of slack (+~9 KB of internal RAM: DMA and stack)
// Before any begin(), once; the watchdog's end()/begin() keeps the config.
void som_configurar(void)
{
  auto c = M5.Speaker.config();
  c.task_priority = 6;
  c.dma_buf_len   = 512;
  c.dma_buf_count = 8;
  M5.Speaker.config(c);
  ESP_LOGI(TAG, "speaker: task prio %u, DMA %u x %u (~%u ms of slack)",
           (unsigned)c.task_priority, (unsigned)c.dma_buf_len, (unsigned)c.dma_buf_count,
           (unsigned)(c.dma_buf_len * c.dma_buf_count * 1000u / (c.sample_rate ? c.sample_rate : 48000u)));
}

void som_atender_fluxo(void)
{
  if (!s_fluxo_pedido) { return; }
  s_fluxo_pedido = false;
  if (!s_som_reservado) {
    const bool frio = !M5.Speaker.isRunning();
    if (M5.Speaker.begin()) {
      if (frio) { s_som_quente_em = tab5_agora_ms() + SOM_AQUECER_MS; }
      s_speaker_ocioso_desde = 0;
      s_fluxo_ativo = true;
    }
  }
  if (s_fluxo_pronto) { xSemaphoreGive(s_fluxo_pronto); }
}

bool sistema_som_fluxo_abrir(uint32_t espera_ms)
{
  if (s_som_reservado || !s_fluxo_pronto) { return false; }
  if (!s_fluxo_ativo) {
    xSemaphoreTake(s_fluxo_pronto, 0);   // leftover from an earlier request
    s_fluxo_pedido = true;
    sistema_acordar_interface();
    xSemaphoreTake(s_fluxo_pronto, pdMS_TO_TICKS(espera_ms));
    if (!s_fluxo_ativo) { return false; }
  }
  // Freshly powered codec: the start of the first block would come out silent.
  const int32_t falta = (int32_t)(s_som_quente_em - tab5_agora_ms());
  if (falta > 0) { vTaskDelay(pdMS_TO_TICKS(falta)); }
  return true;
}

bool sistema_som_fluxo_livre(void)
{
  return s_fluxo_ativo && M5.Speaker.isPlaying(SOM_CANAL_FLUXO) < 2;
}

int sistema_som_fluxo_na_fila(void)
{
  return s_fluxo_ativo ? (int)M5.Speaker.isPlaying(SOM_CANAL_FLUXO) : 0;
}

bool sistema_som_fluxo_tocar(const int16_t* pcm, size_t quadros, uint32_t hz, bool estereo)
{
  s_som_trava.travar();
  const bool ok = s_fluxo_ativo && !s_som_reservado;
  if (ok) {
    M5.Speaker.playRaw(pcm, quadros * (estereo ? 2 : 1), hz, estereo, 1, SOM_CANAL_FLUXO, false);
  }
  s_som_trava.destravar();
  return ok;
}

void sistema_som_fluxo_calar(void)
{
  s_som_trava.travar();
  if (s_fluxo_ativo) { M5.Speaker.stop(SOM_CANAL_FLUXO); }
  s_som_trava.destravar();
}

void sistema_som_fluxo_fechar(void)
{
  s_som_trava.travar();
  if (s_fluxo_ativo) { M5.Speaker.stop(SOM_CANAL_FLUXO); }
  s_fluxo_ativo = false;   // the idle watchdog turns it off after SPEAKER_OCIOSO_MS
  s_som_trava.destravar();
}

void som_atender_reserva(void)
{
  if (!s_som_reservado || s_som_parado) { return; }
  s_som_trava.travar();
  s_fluxo_ativo = false;   // the background sees it on the next block and stops
  if (M5.Speaker.isRunning()) { M5.Speaker.end(); }
  s_som_trava.destravar();
  s_speaker_ocioso_desde = 0;
  s_som_parado = true;
  if (s_som_parou) { xSemaphoreGive(s_som_parou); }
}

bool sistema_som_ceder_pinos(uint32_t espera_ms)
{
  if (!s_som_parou) { return false; }
  xSemaphoreTake(s_som_parou, 0);   // a leftover from an earlier request does not count for this one
  s_som_parado = false;
  s_som_reservado = true;
  sistema_acordar_interface();
  return xSemaphoreTake(s_som_parou, pdMS_TO_TICKS(espera_ms)) == pdTRUE || s_som_parado;
}

void sistema_som_devolver_pinos(void)
{
  s_som_reservado = false;
  s_som_parado = false;
}

void sistema_beep(float hz, uint32_t ms)
{
  if (s_som_reservado) { return; }   // pins are with USB (see above)
  const uint32_t t = tab5_agora_ms();
  if (!M5.Speaker.isRunning()) { s_som_quente_em = t + SOM_AQUECER_MS; }
  const int32_t falta = (int32_t)(s_som_quente_em - t);

  if (falta <= 0) {
    M5.Speaker.tone(hz, ms);
    return;
  }

  // Codec still coming up. Explicit begin(): the silence must start
  // counting from the instant the codec was turned on, not from a lazy begin()
  // inside playRaw.
  if (!M5.Speaker.begin()) { M5.Speaker.tone(hz, ms); return; }
  const size_t lead = (size_t)falta * SOM_FS / 1000;
  size_t       tom  = (size_t)ms * SOM_FS / 1000;
  if (lead >= SOM_AMOSTRAS) { M5.Speaker.tone(hz, ms); return; }
  if (lead + tom > SOM_AMOSTRAS) { tom = SOM_AMOSTRAS - lead; }

  s_som_vez ^= 1;
  if (s_som_bloco[s_som_vez] == nullptr) {
    s_som_bloco[s_som_vez] = (int16_t*)heap_caps_malloc(SOM_AMOSTRAS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  }
  int16_t* b = s_som_bloco[s_som_vez];
  if (b == nullptr) { M5.Speaker.tone(hz, ms); return; }

  memset(b, 0, lead * sizeof(int16_t));
  const size_t rampa = SOM_FS / 400;   // 2.5 ms fade in/out: no click
  for (size_t i = 0; i < tom; ++i) {
    float g = 1.0f;
    if (i < rampa)            { g = (float)i / (float)rampa; }
    else if (tom - i < rampa) { g = (float)(tom - i) / (float)rampa; }
    b[lead + i] = (int16_t)(30000.0f * g * sinf(2.0f * (float)M_PI * hz * (float)i / (float)SOM_FS));
  }
  M5.Speaker.playRaw(b, lead + tom, SOM_FS, false, 1, -1, true);
}

void atualizar_beep_e_ociosidade_do_som(uint32_t t)
{
  if (notif_houve_novidade_sonora()) {
    sistema_beep(1568.0f, 90);   // G5 - short and high, "notification", not "confirmation"
  }

  // Only gets here if some tone()/playWav() has already run this session
  // (isRunning() == false before that, nothing to do).
  if (M5.Speaker.isRunning()) {
    if (M5.Speaker.isPlaying()) {
      s_speaker_ocioso_desde = 0;
    } else if (s_speaker_ocioso_desde == 0) {
      s_speaker_ocioso_desde = t;
    } else if (t - s_speaker_ocioso_desde >= SPEAKER_OCIOSO_MS && !s_fluxo_ativo) {
      // With the stream open (music between two blocks, or loading the
      // next) the speaker stays: it is the background that says when it is over.
      s_som_trava.travar();
      if (!s_fluxo_ativo) { M5.Speaker.end(); }
      s_som_trava.destravar();
      s_speaker_ocioso_desde = 0;
    }
  }
}
