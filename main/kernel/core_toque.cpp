// CORE - touch: the GT911 watchdog (anomalies and the magnifier), continuous reading
// while typing, the fingers of the last sample and the bench that simulates touch,
// gestures and app opening from the console. Split from core.cpp (the engine); what
// the parts share is in core_interno.hpp.

#include "esp_log.h"
#include "driver/gpio.h"
#include "core_interno.hpp"

// ---------------------------------------------------------------------------
// TOUCH WATCHDOG (touch went deaf for a while, came back by itself, and the
// log showed nothing)
// ---------------------------------------------------------------------------
// Touch could go deaf WITHOUT any error: the M5GFX driver does not log I2C
// failures, and when the read fails it silently returns the LAST data read. Three
// ways to go deaf, and by eye there is no telling which:
//   1. the GT911's I2C failing (internal bus contended or stuck);
//   2. the GT911 with a report waiting that nobody reads (the INT pin does not
//      pulse again until its buffer is cleared);
//   3. pulses arriving and the read coming back with no finger.
// Once a second, with the screen on, the watchdog reads the GT911 status register
// DIRECTLY (0x814E - bit 7 "report ready", bits 0-3 fingers), cross-checks with
// what the system saw and with the ISR's pulses. An anomaly becomes a LOG (W, which goes
// to the card immediately) and the recovery is the cheapest there is: force the
// next read over I2C, which clears the GT911's buffer. Same task that reads
// touch - never two reads of it at the same time.
uint32_t s_vig_falhas = 0, s_vig_presos = 0, s_vig_surdos = 0, s_vig_forcados = 0;
uint32_t s_vig_reinicios = 0;
uint8_t  s_vig_status = 0;

// MAGNIFIER. The "pulses without finger" warnings only came with the screen
// changing non-stop (Camera, Tasks) and with the finger on the screen - and the GT911 saying
// "zero fingers" with I2C fine. Suspicion: panel noise, and the GT911's filter
// throwing the touch away. To prove it, the magnifier looks at the status byte on EVERY
// turn for 3 s after an anomaly (read only - the driver still does the clearing)
// and counts: report with finger, empty report, bit 6 ("large touch"
// - the GT911 marks this way what it rejects as palm/noise), and no report.
static uint32_t s_lupa_ate = 0;
static uint32_t s_lupa_com = 0, s_lupa_vazio = 0, s_lupa_grande = 0, s_lupa_nada = 0, s_lupa_falha = 0;

void lupa_toque(uint32_t t, bool tocando)
{
  if (!s_lupa_ate || !tab5_painel_toque_gt911()) { return; }
  if ((int32_t)(t - s_lupa_ate) >= 0) {
    ESP_LOGW("toque", "magnifier 3 s: %lu report(s) with finger, %lu empty, %lu marked 'large/noise', "
                      "%lu turn(s) without report, %lu I2C failure(s)",
             (unsigned long)s_lupa_com, (unsigned long)s_lupa_vazio, (unsigned long)s_lupa_grande,
             (unsigned long)s_lupa_nada, (unsigned long)s_lupa_falha);
    s_lupa_ate = 0;
    return;
  }
  auto* tp = M5.Display.touch();
  if (!tp) { return; }
  const auto c = tp->config();
  const uint8_t reg[2] = { 0x81, 0x4E };
  uint8_t st = 0;
  if (!lgfx::i2c::transactionWriteRead(c.i2c_port, c.i2c_addr, reg, 2, &st, 1, c.freq).has_value()) { ++s_lupa_falha; return; }
  if (!(st & 0x80))      { ++s_lupa_nada; }
  else if (st & 0x40)    { ++s_lupa_grande; }
  else if (st & 0x0F)    { ++s_lupa_com; }
  else                   { ++s_lupa_vazio; }
  (void)tocando;
}

// GT911 MUTE (13-14/11/2026): it started pulsing INT ~10x/s and the status byte
// NEVER showed a report ready (the magnifier: 0 with finger, 0 empty, ~120
// turns without a report in 3 s). Forcing a read did not help; the device
// went idle like that and touch no longer woke the screen - it only came back
// after restarting everything. The recovery is the boot one: the chip's reset pin, in the HAL
// (tab5_toque_reiniciar). At most once every 30 s, so a genuinely
// faulty chip does not become a restart loop.
static void reiniciar_gt911(uint32_t t, const char* onde, uint32_t pulsos)
{
  static uint32_t ultimo = 0;
  if (ultimo && (t - ultimo) < 30000) { return; }
  ultimo = t;
  ++s_vig_reinicios;
  ESP_LOGW("toque", "GT911 mute (%s): %lu pulse(s) with no report at all - restarting the chip (%lu time(s) since boot)",
           onde, (unsigned long)pulsos, (unsigned long)s_vig_reinicios);
  tab5_toque_reiniciar();
  s_toque_pulso = true;   // the next turn already reads the new chip
}

// Console request (another task): only the flag; what restarts it is the
// UI, in vigiar_toque_repouso (runs on every turn, screen on or not).
static std::atomic<bool> s_pedir_reinicio{false};
void sistema_pedir_reinicio_toque(void)
{
  s_pedir_reinicio = true;
  sistema_acordar_interface();
}

// GT911 X-RAY (console 'toque regs'): what the chip says about itself. To
// separate "chip alive but not feeling the finger" from "broken configuration" and from
// "wire/panel". Reads, writes nothing (the configuration lives in the GT911's flash).
//   0x8140..43 "911\0"  identification      0x8144..45 firmware version
//   0x8146..49 X/Y resolution               0x814A     sensor vendor
//   0x8047..0x80FE configuration (184 bytes), 0x80FF its checksum
//   0x814E status: bit 7 report ready, bits 0-3 fingers
static std::atomic<bool> s_pedir_regs{false};
void sistema_pedir_regs_toque(void)
{
  s_pedir_regs = true;
  sistema_acordar_interface();
}

static bool gt911_ler(uint16_t reg, uint8_t* buf, size_t n)
{
  auto* tp = M5.Display.touch();
  if (!tp) { return false; }
  const auto c = tp->config();
  const uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
  return lgfx::i2c::transactionWriteRead(c.i2c_port, c.i2c_addr, r, 2, buf, n, c.freq).has_value();
}

static void gt911_raio_x(void)
{
  if (!tab5_painel_toque_gt911()) { ESP_LOGW("toque", "regs: the screen is not GT911"); return; }
  const auto c = M5.Display.touch()->config();
  uint8_t id[11] = {0};
  if (!gt911_ler(0x8140, id, 11)) {
    ESP_LOGW("toque", "regs: GT911 did NOT respond on I2C (address 0x%02X, INT pin=%d)",
             c.i2c_addr, gpio_get_level(GPIO_NUM_23));
    return;
  }
  ESP_LOGW("toque", "regs: id '%c%c%c%c' firmware 0x%02X%02X resolution %ux%u vendor 0x%02X (address 0x%02X, INT pin=%d)",
           id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?', id[3] ? id[3] : '?',
           id[5], id[4], id[6] | (id[7] << 8), id[8] | (id[9] << 8), id[10],
           c.i2c_addr, gpio_get_level(GPIO_NUM_23));
  uint8_t cfg[185] = {0};
  if (!gt911_ler(0x8047, cfg, sizeof(cfg))) { ESP_LOGW("toque", "regs: reading the configuration failed"); return; }
  uint8_t soma = 0;
  for (int i = 0; i < 184; ++i) { soma += cfg[i]; }
  soma = (uint8_t)(~soma + 1);
  ESP_LOGW("toque", "regs: configuration v%u, output %ux%u, %u finger(s), module 0x%02X, checksum %s (read 0x%02X, calculated 0x%02X)",
           cfg[0], cfg[1] | (cfg[2] << 8), cfg[3] | (cfg[4] << 8), cfg[5] & 0x0F, cfg[6],
           soma == cfg[184] ? "OK" : "WRONG", cfg[184], soma);
  // The whole configuration, 16 bytes per line (to compare with another Tab5).
  for (int i = 0; i < 184; i += 16) {
    char linha[16 * 3 + 1];
    int n = 0;
    for (int j = i; j < i + 16 && j < 184; ++j) { n += snprintf(linha + n, sizeof(linha) - n, "%02X ", cfg[j]); }
    ESP_LOGW("toque", "regs: 0x%04X: %s", 0x8047 + i, linha);
  }
  uint8_t st = 0;
  gt911_ler(0x814E, &st, 1);
  ESP_LOGW("toque", "regs: status 0x%02X, %lu pulse(s) since boot", st, (unsigned long)s_pulsos);
}

static void lupa_ligar(uint32_t t)
{
  if (s_lupa_ate) { return; }
  s_lupa_ate = t + 3000;
  s_lupa_com = s_lupa_vazio = s_lupa_grande = s_lupa_nada = s_lupa_falha = 0;
}

void vigiar_toque(uint32_t t, bool tocando)
{
  // The registers read here are the GT911's. On a TDDI screen (ST7123/ST7121)
  // the touch address is another chip, with another map: reading 0x814E there would be
  // writing a meaningless register pointer into the screen controller.
  if (!tab5_painel_toque_gt911()) { return; }
  static uint32_t prox = 0, pulsos_ant = 0, dedo_desde = 0, log_falha_ms = 0;
  static bool viu_dedo = false, avisou_dedo_longo = false;
  static int presos_seguidos = 0;
  static int mudos_seguidos = 0;

  // BOOT GRACE. While coming up, the GT911 driver itself uses the
  // INT pin as an OUTPUT (the wakeup() pulse that chooses the I2C address) and the
  // panel is mounted right after: the ISR counted 282 edges in one second without
  // any finger, and the magnifier confirmed it - zero reports from the GT911. Start-up
  // noise, not touch. The watchdog only starts 5 s after the first turn, with the
  // pulse count zeroed at that instant.
  static bool armado = false;
  if (!armado) {
    if (!prox) { prox = t + 5000; }
    if ((int32_t)(t - prox) < 0) { return; }
    armado = true;
    pulsos_ant = s_pulsos;
    viu_dedo = tocando;
    prox = t + 1000;
    return;
  }

  if (tocando) { viu_dedo = true; if (!dedo_desde) { dedo_desde = t; } }
  else         { dedo_desde = 0; avisou_dedo_longo = false; }
  if ((int32_t)(t - prox) < 0) { return; }
  prox = t + 1000;

  const uint32_t total = s_pulsos;
  const uint32_t pulsos = total - pulsos_ant;
  pulsos_ant = total;
  const bool dedo_na_janela = viu_dedo;
  viu_dedo = tocando;

  auto* tp = M5.Display.touch();
  if (!tp) { return; }
  const auto c = tp->config();
  const uint8_t reg[2] = { 0x81, 0x4E };
  uint8_t st = 0;
  const bool ok = lgfx::i2c::transactionWriteRead(c.i2c_port, c.i2c_addr, reg, 2, &st, 1, c.freq).has_value();
  const int pino = gpio_get_level(GPIO_NUM_23);
  s_vig_status = st;

  if (!ok) {
    ++s_vig_falhas;
    s_toque_pulso = true;   // try the driver's path too
    if (s_vig_falhas == 1 || (t - log_falha_ms) >= 10000) {
      log_falha_ms = t;
      ESP_LOGW("toque", "GT911 I2C did NOT respond (%lu failure(s) since boot; INT pin=%d, %lu pulse(s)/s)",
               (unsigned long)s_vig_falhas, pino, (unsigned long)pulsos);
    }
    return;
  }

  // 2. Report WITH a finger sitting in the GT911, and the system with no finger - two consecutive
  // probes (a touch that just came down between the read and the probe is normal).
  if ((st & 0x80) && (st & 0x0F) && !tocando) {
    if (++presos_seguidos >= 2) {
      ++s_vig_presos; ++s_vig_forcados;
      s_toque_pulso = true;
      lupa_ligar(t);
      ESP_LOGW("toque", "GT911 with %u finger(s) waiting and the system without a finger for %d s "
                    "(status %02X, INT pin=%d, %lu pulse(s)/s) - forcing a read",
               (unsigned)(st & 0x0F), presos_seguidos, st, pino, (unsigned long)pulsos);
    }
  } else {
    presos_seguidos = 0;
  }

  // 3. Pulses arriving (the GT911 is seeing a finger) and no finger read for the whole
  // second. A normal touch pulses ~10-100x/s; a release, a few times.
  if (pulsos >= 10 && !dedo_na_janela) {
    ++s_vig_surdos; ++s_vig_forcados;
    s_toque_pulso = true;
    lupa_ligar(t);
    ESP_LOGW("toque", "%lu pulse(s) in 1 s and NO finger read (status %02X, INT pin=%d) - forcing a read",
             (unsigned long)pulsos, st, pino);
  }

  // 4. MUTE (see reiniciar_gt911): pulses, no finger and no report
  // ready, three seconds in a row. The restart pulses INT: the count
  // starts again from there, so its pulses do not count in the next window.
  if (pulsos >= 10 && !dedo_na_janela && !(st & 0x80)) {
    if (++mudos_seguidos >= 3) {
      reiniciar_gt911(t, "tela acesa", pulsos);
      mudos_seguidos = 0;
      pulsos_ant = s_pulsos;
    }
  } else {
    mudos_seguidos = 0;
  }

  // The reverse: finger "on the screen" non-stop - stale read repeated (the driver
  // returns the last data when it fails). One warning per episode.
  if (dedo_desde && !avisou_dedo_longo && (t - dedo_desde) > 15000) {
    avisou_dedo_longo = true;
    ESP_LOGW("toque", "finger read NON-STOP for 15 s (status %02X, INT pin=%d, %lu pulse(s)/s) - ghost finger?",
             st, pino, (unsigned long)pulsos);
  }
}

// THE SAME MUTE WHEN IDLE. There the watchdog above does not run (the screen off
// must not read I2C every second for nothing), and it was precisely when idle that the defect
// trapped the device. When idle a real touch wakes the screen immediately;
// so pulses arriving for 10 s without any finger is already the sign. Only in that case
// does it read the status byte (one read every 10 s, at most) and, with no report,
// restart the chip. Awake, the window stays zeroed.
void vigiar_toque_repouso(uint32_t t, bool tocando, bool em_repouso)
{
  static uint32_t desde = 0, pulsos_ini = 0;
  static bool viu_dedo = false;
  if (s_pedir_regs.exchange(false)) { gt911_raio_x(); }
  if (s_pedir_reinicio.exchange(false)) {
    ESP_LOGW("toque", "GT911 restart requested by the console");
    if (tab5_toque_reiniciar()) { ++s_vig_reinicios; }
    s_toque_pulso = true;
  }
  if (!em_repouso || !tab5_painel_toque_gt911()) { desde = 0; return; }
  if (!desde) { desde = t; pulsos_ini = s_pulsos; viu_dedo = tocando; return; }
  if (tocando) { viu_dedo = true; }
  if ((t - desde) < 10000) { return; }
  const uint32_t pulsos = s_pulsos - pulsos_ini;
  const bool teve_dedo = viu_dedo;
  desde = t; pulsos_ini = s_pulsos; viu_dedo = false;
  if (pulsos < 5 || teve_dedo) { return; }

  auto* tp = M5.Display.touch();
  if (!tp) { return; }
  const auto c = tp->config();
  const uint8_t reg[2] = { 0x81, 0x4E };
  uint8_t st = 0;
  if (!lgfx::i2c::transactionWriteRead(c.i2c_port, c.i2c_addr, reg, 2, &st, 1, c.freq).has_value()) { return; }
  if (!(st & 0x80)) {
    reiniciar_gt911(t, "repouso", pulsos);
    pulsos_ini = s_pulsos;
  }
}

// CONTINUOUS READING WHILE TYPING ('continuo'). The GT911 driver has
// two paths: read less than 128 ms ago, a simple read; read longer ago,
// it DISCARDS the pending report and waits for the next (up to ~24 ms stalled,
// with the UI task stuck). The discarded report is the FINGER
// GOING DOWN: on a short touch the next one is already the release, and the key vanishes without
// trace. It was the Notes app's Enter that "did not register" (04/11/2026): the letters come out
// in a burst, but Enter comes after a pause - and with more than 500 ms without a
// finger the loop slept waiting for the pin, the read went past 128 ms and the next
// touch fell into the discard.
// Now, WITH THE KEYBOARD OPEN (navigation locked), the read goes to I2C on
// every turn and the turn never sleeps longer than TOQUE_FRESCO_MS: the driver stays
// on the simple path at all times. Without a finger, each turn is a status read (the
// GT911 says "nothing new" and the driver does not wait). Outside the keyboard, as before:
// a still screen sleeps until the pin.
void ajustar_leitura_do_toque(bool continuo)
{
  auto* tp = M5.Display.touch();
  if (!tp || !tab5_painel_toque_gt911()) { return; }   // only the GT911 driver looks at the pin
  const bool pulso = s_toque_pulso;
  s_toque_pulso = false;   // before the read: a pulse during it counts for the next one
  const int pino = (s_dedo_na_tela || pulso || continuo) ? -1 : (int)GPIO_NUM_23;
  auto c = tp->config();
  if (c.pin_int != pino) { c.pin_int = pino; tp->config(c); }
}
// The fingers of the last sample (see sistema_toques in app.h). Only the UI
// task writes and only it reads (inside ao_tocar_nivel): no lock.
sistema_toque_t s_toques[SISTEMA_TOQUES_MAX];
int             s_n_toques = 0;

// The touch in progress has already been handled by level (sistema_consumir_toque, app.h).
// Read and zeroed by the click recogniser, on release.
bool s_toque_consumido = false;
void sistema_consumir_toque(void) { s_toque_consumido = true; }

int sistema_toques(sistema_toque_t* out, int max)
{
  const int n = s_n_toques < max ? s_n_toques : max;
  for (int i = 0; i < n; ++i) { out[i] = s_toques[i]; }
  return n;
}
// BENCH: touch and app opening requested by the console (another task). The
// UI consumes it on the next turn, as if the finger had touched - to
// test a screen over serial without anyone at the device.
volatile int s_sim_x = -1, s_sim_y = -1;
const app_t* volatile s_sim_abrir = nullptr;
volatile bool s_sim_fechar = false;
void sistema_simular_toque(int x, int y) { s_sim_y = y; s_sim_x = x; }
volatile int s_simg[4];
volatile bool s_simg_pedido = false;
void sistema_simular_gesto(int x0, int y0, int x1, int y1)
{
  s_simg[0] = x0; s_simg[1] = y0; s_simg[2] = x1; s_simg[3] = y1;
  s_simg_pedido = true;
  sistema_acordar_interface();
}
void sistema_simular_abrir(const app_t* app) { if (app) { s_sim_abrir = app; } else { s_sim_fechar = true; } }
