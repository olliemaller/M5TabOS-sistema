#include "tab5_teclado.hpp"

#include <string.h>
#include <stdio.h>
#include <M5Unified.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "tab5_hora.hpp"   // tab5_agora_ms

static const char *TAG = "teclado";

// Registers, copied from the official source (M5Unit-KEYBOARD,
// src/unit/unit_Tab5Keyboard.hpp). Kept with their names from there so that
// comparison with upstream is direct if anything changes.
#define REG_INT_CFG            0x00
#define REG_INT_STAT           0x01
#define REG_EVENT_NUM          0x02
#define REG_MODE_KEYBOARD      0x10
#define REG_HID_EVENT          0x30   // 2 bytes: modifier + HID code
#define REG_FIRMWARE_VERSION   0xFE

// HID MODE, not character mode.
//
// In character mode the STM32 returns TEXT: the ready-made letter, but the special
// keys as their NAME ("enter", five letters). The names are not
// documented anywhere - neither the keyboard firmware nor the official
// library lists them - so each special key had to be discovered by
// pressing it and reading the log, and the arrows, Del and Tab never were.
//
// In HID mode each key comes as its USB HID standard usage code (page
// 0x07), plus the modifier byte. The official library includes the whole 5x14
// matrix captured from the real device in this mode (KEY_MATRIX_HID_BASE and
// KEY_MATRIX_HID_SYM in unit_Tab5Keyboard.cpp):
//
//   row 0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del
//   row 1: ` ! @ # $ % ^ & * ( ) [ ] \
//   row 2: Tab q w e r t y u i o p ; ' Backspace
//   row 3: Sym Aa a s d f g h j k l (up) _ Enter
//   row 4: Ctrl Alt z x c v b n m . (left) (down) (right) Space
//
// Sym and Aa are resolved BY THE FIRMWARE: with Sym held the key already arrives
// with the Sym layer code (! becomes ?, * becomes /, ( becomes <, . becomes ,), and
// Aa arrives as the shift bit. On our side the US HID table is enough.
#define MODO_HID               1

#define FREQ  400000

static bool     s_presente = false;
static uint8_t  s_versao   = 0;
static bool     s_bus_ok   = false;   // the external bus already points at ExtPort1
static uint32_t s_ultima_busca = 0;   // ms; the last search for an absent keyboard

// NO ON/OFF SETTING: the keyboard is found automatically. When absent, it costs one
// I2C transaction every BUSCA_MS with the screen on (none when idle), and the
// first keypress of a freshly attached keyboard is caught immediately via G50 - the
// INT goes low with the queue full and the pull-up holds it high with no keyboard (see
// int_em_baixo and tab5_teclado_pendente). The old setting ("teclado" in NVS)
// is no longer used.

// How often to search for an absent keyboard.
//
// 2s is the compromise: attaching it and seeing it work within two seconds feels
// instant to the user, and one I2C transaction every 2s is negligible
// noise next to the 50 per second the UI already does. Searching every
// cycle (20ms) would be pure waste on a device that spends most of its
// life without a keyboard attached.
#define BUSCA_MS  2000
// With G50 low and no known keyboard, search much sooner - but
// not every round: another accessory holding the pin low must not turn into
// an I2C scan per frame.
#define BUSCA_INT_MS  250


bool tab5_teclado_presente(void) { return s_presente; }
uint8_t tab5_teclado_versao(void) { return s_versao; }

// ---------------------------------------------------------------------------
// TRANSLATION: HID code -> text or special key
// ---------------------------------------------------------------------------
// US table (the keyboard's). Index = HID code from 0x1E; those from 0x04 to
// 0x1D are the letters, handled separately. 0 = code with no character.
static const char US_NORMAL[] = "1234567890" "\0\0\0\0" " -=[]\\\0;'`,./";
static const char US_SHIFT[]  = "!@#$%^&*()" "\0\0\0\0" " _+{}|\0:\"~<>?";
#define US_PRIMEIRO  0x1E
#define US_ULTIMO    0x38

static uint8_t codigo_especial(uint8_t hid)
{
  switch (hid) {
    case 0x28: return TAB5_TECLA_ENTER;
    case 0x29: return TAB5_TECLA_ESC;
    case 0x2A: return TAB5_TECLA_APAGAR;     // Backspace
    case 0x2B: return TAB5_TECLA_TAB;
    case 0x4C: return TAB5_TECLA_DEL;        // Delete
    case 0x4F: return TAB5_TECLA_DIREITA;
    case 0x50: return TAB5_TECLA_ESQUERDA;
    case 0x51: return TAB5_TECLA_BAIXO;
    case 0x52: return TAB5_TECLA_CIMA;
    default:   return TAB5_TECLA_NENHUMA;
  }
}

// The key's ASCII character, or 0.
static char caractere(uint8_t hid, bool shift)
{
  if (hid >= 0x04 && hid <= 0x1D) { return (char)((shift ? 'A' : 'a') + (hid - 0x04)); }
  if (hid >= US_PRIMEIRO && hid <= US_ULTIMO) {
    return (shift ? US_SHIFT : US_NORMAL)[hid - US_PRIMEIRO];
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ACCENTS VIA DEAD KEYS (the "US international" layout of Portuguese writers)
// ---------------------------------------------------------------------------
// The keyboard is US: it has no accent or cedilla key. As in computers'
// international layout, four keys become DEAD - they don't type
// immediately, they wait for the next letter:
//
//   '  + a e i o u c  ->  á é í ó ú ç     (and uppercase)
//   `  + a            ->  à
//   ~  + a o          ->  ã õ             (~ is Sym + `)
//   ^  + a e o        ->  â ê ô           (^ is shift + 6)
//
// Followed by a space, the mark itself comes out (' + space = '). Followed by any other
// letter, both come out (' + x = 'x) - nothing is lost. The double quote
// (shift + ') is NOT dead.
//
// To disable: TECLADO_ACENTOS 0.
#define TECLADO_ACENTOS 1

typedef struct { char base; const char* utf8; } composta_t;
static const composta_t AGUDO[] = {
  {'a',"\xC3\xA1"},{'e',"\xC3\xA9"},{'i',"\xC3\xAD"},{'o',"\xC3\xB3"},{'u',"\xC3\xBA"},{'c',"\xC3\xA7"},
  {'A',"\xC3\x81"},{'E',"\xC3\x89"},{'I',"\xC3\x8D"},{'O',"\xC3\x93"},{'U',"\xC3\x9A"},{'C',"\xC3\x87"},{0,0} };
static const composta_t CRASE[] = { {'a',"\xC3\xA0"},{'A',"\xC3\x80"},{0,0} };
static const composta_t TIL[]   = { {'a',"\xC3\xA3"},{'o',"\xC3\xB5"},{'A',"\xC3\x83"},{'O',"\xC3\x95"},{0,0} };
static const composta_t CIRC[]  = {
  {'a',"\xC3\xA2"},{'e',"\xC3\xAA"},{'o',"\xC3\xB4"},{'A',"\xC3\x82"},{'E',"\xC3\x8A"},{'O',"\xC3\x94"},{0,0} };

static const composta_t* tabela_morta(char ch)
{
  switch (ch) {
    case '\'': return AGUDO;
    case '`':  return CRASE;
    case '~':  return TIL;
    case '^':  return CIRC;
    default:   return nullptr;
  }
}

void tab5_teclado_ascii(const char* texto, char* saida, size_t tam)
{
  if (!saida || tam == 0) { return; }
  snprintf(saida, tam, "%s", texto ? texto : "");
  if (!texto || !texto[0]) { return; }
  static const char MORTAS[] = { '\'', '`', '~', '^' };
  for (char m : MORTAS) {
    for (const composta_t* c = tabela_morta(m); c->base; ++c) {
      if (strcmp(texto, c->utf8) == 0) { snprintf(saida, tam, "%c%c", m, c->base); return; }
    }
  }
}

// ---------------------------------------------------------------------------
// HELD KEY: REPEAT
// ---------------------------------------------------------------------------
// In HID mode the firmware sends one event on PRESS and another, with code 0, on
// RELEASE (unit_Tab5Keyboard.hpp: "keycode ... 0 on key release"). Between the
// two the key is held, and the ones that repeat (backspace, arrows) repeat here,
// with the touch keyboard's timings.
//
// TWO SAFEGUARDS, because deleting on its own is the worst possible failure (text vanishes):
//  - only repeats after having seen ONE release since the keyboard was
//    attached. If a firmware doesn't send releases, repeat never switches on - the
//    held key simply doesn't repeat;
//  - if the firmware ITSELF repeats (the same code again with no release in
//    between), ours switches off: two repeats combined would delete twice as fast.
// And a 10 s ceiling on holding, against a release lost on the bus.
#define SEGURADA_MAX_MS  10000

static uint8_t      s_seg_hid = 0;        // held HID code that repeats (0 = none)
static tab5_tecla_t s_seg_ev;             // the event it repeats
static uint32_t     s_seg_desde = 0, s_seg_ultimo = 0;
static bool         s_seg_repetiu = false;
static bool         s_soltura_vista = false;
static bool         s_fw_repete = false;

static bool repete(uint8_t codigo)
{
  return codigo == TAB5_TECLA_APAGAR || codigo == TAB5_TECLA_DEL
      || codigo == TAB5_TECLA_ESQUERDA || codigo == TAB5_TECLA_DIREITA
      || codigo == TAB5_TECLA_CIMA || codigo == TAB5_TECLA_BAIXO;
}

bool tab5_teclado_segurando(void) { return s_presente && s_seg_hid != 0 && s_soltura_vista && !s_fw_repete; }

static char s_morta = 0;            // the dead key waiting for the letter (0 = none)
static tab5_tecla_t s_guardada;     // event that arrived with the dead key and goes out on the next read
static bool s_tem_guardada = false;

// INT pin state (see 'THE PIN BEFORE THE BUS', in ler_hid).
static bool     s_int_confiavel = false;   // G50 has proven it signals
static bool     s_int_traiu     = false;   // ...or has proven it doesn't
static bool     s_pode_ter_mais = true;    // the last read found an event
static uint32_t s_conferido_ms  = 0;

// ---------------------------------------------------------------------------

static bool int_em_baixo(void);

// Tries to find and configure the keyboard. Called at boot AND periodically
// while it is absent - that is what allows attaching it with the device on.
static bool procurar(bool falar)
{
  if (!M5.Ex_I2C.scanID(TAB5_TECLADO_ENDERECO, FREQ))
  {
    // Not an error: the keyboard is an accessory, and absence is the normal state.
    if (falar) {
      ESP_LOGW(TAG, "no keyboard at 0x%02X - will keep searching every %dms",
               TAB5_TECLADO_ENDERECO, BUSCA_MS);
    }
    s_presente = false;
    return false;
  }

  s_versao = M5.Ex_I2C.readRegister8(TAB5_TECLADO_ENDERECO, REG_FIRMWARE_VERSION, FREQ);

  // HID mode. Switching mode leaves events in the OLD format in the queue, which
  // would be misread: like the official source, we empty the queue (writing 0
  // to the counter clears the queue and releases INT).
  if (!M5.Ex_I2C.writeRegister8(TAB5_TECLADO_ENDERECO, REG_MODE_KEYBOARD, MODO_HID, FREQ))
  {
    ESP_LOGE(TAG, "found the keyboard but could not put it into HID mode");
    s_presente = false;
    return false;
  }
  M5.Ex_I2C.writeRegister8(TAB5_TECLADO_ENDERECO, REG_EVENT_NUM, 0x00, FREQ);
  s_presente = true;
  s_morta = 0;
  s_tem_guardada = false;
  s_seg_hid = 0;
  s_soltura_vista = false;
  s_fw_repete = false;
  s_pode_ter_mais = true;   // first read after attaching: without looking at the pin
  // The pin is relearnt on every attach: an event read with INT high right
  // after attaching (the pin still settling) used to disable trust until the
  // next boot - seen on a reattach, with the keyboard signalling correctly.
  s_int_confiavel = false;
  s_int_traiu = false;

  ESP_LOGI(TAG, "keyboard ATTACHED at 0x%02X (firmware %u), HID mode",
           TAB5_TECLADO_ENDERECO, (unsigned)s_versao);
  return true;
}

bool tab5_teclado_iniciar(void)
{
  // The bus is ALWAYS repointed, with or without a keyboard: it is what moves
  // Ex_I2C off the Grove pins, and Azimutal relies on that.

  // Repoints the external bus to ExtPort1. See why in the header.
  // This is done ONCE; only the search for the keyboard repeats.
  M5.Ex_I2C.release();
  if (!M5.Ex_I2C.begin(TAB5_TECLADO_PORTA, TAB5_TECLADO_SDA, TAB5_TECLADO_SCL))
  {
    ESP_LOGE(TAG, "could not open I2C on SDA=%d SCL=%d",
             TAB5_TECLADO_SDA, TAB5_TECLADO_SCL);
    return false;
  }
  s_bus_ok = true;
  s_ultima_busca = tab5_agora_ms();
  // G50 as an input with pull-up (the kernel already configures it with the
  // interrupt; this only ensures a readable level if that fails). Doesn't touch
  // the interrupt type.
  gpio_set_direction((gpio_num_t)TAB5_TECLADO_INT, GPIO_MODE_INPUT);
  gpio_pullup_en((gpio_num_t)TAB5_TECLADO_INT);

  return procurar(true);
}

// THE PIN BEFORE THE BUS. The keyboard INT (G50, active low) goes low
// when there is an event in the queue and only goes high when it is read. With the screen on
// the UI reads the keyboard every 60 ms: that was 16 I2C transactions per second with
// the keyboard sitting on the desk. Now, with the pin high, there is nothing to read - and
// reading the pin level costs nothing. Three precautions, because losing a key is
// worse than spending bus time:
//  - the pin only counts after being SEEN working (an event read with
//    it low); an event read with it high disables trust for good
//    (firmware that doesn't drive INT, loose wire) and goes back to the usual reads;
//  - after an event it reads again without looking at the pin (the queue may hold more than
//    one, and INT may go high already on the first read), until it sees the queue empty;
//  - with the pin high, one real read per second anyway: that is
//    what notices the keyboard being PULLED OUT (the pull-up leaves the pin high).
#define TECLADO_CONFERIR_MS 1000   // (the pin variables live up above, with the state)

static bool int_em_baixo(void) { return gpio_get_level((gpio_num_t)TAB5_TECLADO_INT) == 0; }

// Reads the next raw HID event from the queue. false = queue empty (or keyboard gone).
static bool ler_hid(uint8_t* mod, uint8_t* hid)
{
  const uint32_t t = tab5_agora_ms();
  const bool int_baixo = int_em_baixo();
  if (s_int_confiavel && !int_baixo && !s_pode_ter_mais && s_seg_hid == 0
   && (t - s_conferido_ms) < TECLADO_CONFERIR_MS) {
    return false;   // nothing in the queue, and the pin says so for free
  }
  s_conferido_ms = t;

  // One transaction per cycle when there is no key: reads only the event counter.
  //
  // We read with readRegister (which returns success) and not readRegister8 (which
  // returns the value): without knowing whether the transaction failed, a keyboard PULLED OUT
  // would look like a keyboard with no keys pressed, forever.
  uint8_t n = 0;
  if (!M5.Ex_I2C.readRegister(TAB5_TECLADO_ENDERECO, REG_EVENT_NUM, &n, 1, FREQ))
  {
    ESP_LOGW(TAG, "keyboard REMOVED (stopped responding)");
    s_presente = false;
    s_ultima_busca = t;
    return false;
  }
  if (n == 0) { s_pode_ter_mais = false; return false; }
  // Learns the pin on the first read of a burst (with s_pode_ter_mais the
  // INT may already have gone high from the previous read - that one doesn't count).
  if (!s_pode_ter_mais && !s_int_traiu) {
    if (int_baixo) {
      if (!s_int_confiavel) { ESP_LOGI(TAG, "the keyboard INT (G%d) signals: reading only when there is a key", TAB5_TECLADO_INT); }
      s_int_confiavel = true;
    } else {
      s_int_confiavel = false;
      s_int_traiu = true;
      ESP_LOGW(TAG, "event with INT (G%d) high - the pin doesn't signal, periodic reads", TAB5_TECLADO_INT);
    }
  }
  s_pode_ter_mais = true;

  uint8_t buf[2] = { 0xFF, 0xFF };
  if (!M5.Ex_I2C.readRegister(TAB5_TECLADO_ENDERECO, REG_HID_EVENT, buf, 2, FREQ)) { return false; }

  // Releases the INT signal. We do this even without using the pin yet: without the
  // clear the STM32 keeps the interrupt active and, on the day we enable
  // G50, it would start out stuck.
  M5.Ex_I2C.writeRegister8(TAB5_TECLADO_ENDERECO, REG_INT_STAT, 0x00, FREQ);

  if (buf[0] == 0xFF && buf[1] == 0xFF) { return false; }   // queue empty (firmware sentinel)
  *mod = buf[0];
  *hid = buf[1];
  return true;
}

bool tab5_teclado_ler(tab5_tecla_t* ev)
{
  if (!ev) { return false; }
  memset(ev, 0, sizeof(*ev));
  if (!s_bus_ok) { return false; }

  if (!s_presente)
  {
    // Absent: searches from time to time, or RIGHT AWAY if G50 went low (someone
    // attached it and pressed a key). This is what makes the keyboard work when
    // attached with the device on, without restarting.
    const uint32_t t = tab5_agora_ms();
    if (t - s_ultima_busca < (int_em_baixo() ? BUSCA_INT_MS : BUSCA_MS)) { return false; }
    s_ultima_busca = t;
    if (!procurar(false)) { return false; }
  }

  // The event left behind when a dead key was undone by a special
  // key: goes out now, in the order it was pressed.
  if (s_tem_guardada) { *ev = s_guardada; s_tem_guardada = false; return true; }

  const uint32_t agora = tab5_agora_ms();
  uint8_t m = 0, hid = 0;
  if (!ler_hid(&m, &hid)) {
    // Queue empty: the held key repeats, if it's its turn.
    if (tab5_teclado_segurando()) {
      if (agora - s_seg_desde > SEGURADA_MAX_MS) { s_seg_hid = 0; return false; }
      const uint32_t passo = s_seg_repetiu ? TAB5_TECLA_REPETE_MS : TAB5_TECLA_REPETE_INICIO_MS;
      if (agora - s_seg_ultimo >= passo) {
        s_seg_ultimo = agora;
        s_seg_repetiu = true;
        *ev = s_seg_ev;
        return true;
      }
    }
    return false;
  }
  if (hid == 0) {
    // RELEASE (or just a modifier, Ctrl/Alt alone): nothing to deliver, and
    // the held key is no longer held.
    s_seg_hid = 0;
    if (!s_soltura_vista) { s_soltura_vista = true; ESP_LOGI(TAG, "the keyboard sends releases: held key repeats"); }
    return false;
  }
  if (hid == s_seg_hid && !s_fw_repete) {
    // The same code again, with no release in between: it is the firmware repeating.
    s_fw_repete = true;
    ESP_LOGI(TAG, "the keyboard firmware already repeats the held key - our repeat stays off");
  }

  // HID modifiers: left in bits 0-2, right in 4-6.
  const uint8_t mm = (uint8_t)((m | (m >> 4)) & 0x07);
  ev->mod = mm;
  const bool shift = (mm & TAB5_TECLA_SHIFT) != 0;

  ev->codigo = codigo_especial(hid);
  if (ev->codigo == TAB5_TECLA_NENHUMA) {
    const char ch = caractere(hid, shift);
    if (!ch) {
      ESP_LOGW(TAG, "untranslated key: HID 0x%02X mod 0x%02X - ignored", hid, m);
      return false;
    }
    ev->texto[0] = ch;
  }

  if (repete(ev->codigo)) {
    s_seg_hid = hid;
    s_seg_ev = *ev;
    s_seg_desde = s_seg_ultimo = agora;
    s_seg_repetiu = false;
  } else {
    s_seg_hid = 0;
  }

#if TECLADO_ACENTOS
  // With Ctrl/Alt it is a shortcut, not text: the dead key stays out of it.
  const bool atalho = (mm & (TAB5_TECLA_CTRL | TAB5_TECLA_ALT)) != 0;
  if (!atalho) {
    if (s_morta) {
      const char morta = s_morta;
      s_morta = 0;
      if (ev->codigo == TAB5_TECLA_NENHUMA) {
        const char ch = ev->texto[0];
        if (ch == ' ') { ev->texto[0] = morta; return true; }        // ' + space = '
        for (const composta_t* c = tabela_morta(morta); c->base; ++c) {
          if (c->base == ch) { snprintf(ev->texto, sizeof(ev->texto), "%s", c->utf8); return true; }
        }
        ev->texto[0] = morta; ev->texto[1] = ch; ev->texto[2] = 0;    // ' + x = 'x
        return true;
      }
      // Special after the dead key: the mark goes out first, the special on the next one.
      s_guardada = *ev;
      s_tem_guardada = true;
      memset(ev, 0, sizeof(*ev));
      ev->texto[0] = morta;
      return true;
    }
    if (ev->codigo == TAB5_TECLA_NENHUMA && tabela_morta(ev->texto[0])) {
      s_morta = ev->texto[0];   // wait for the letter
      return false;
    }
  }
#endif
  return true;
}

bool tab5_teclado_pendente(void)
{
  if (!s_bus_ok) { return false; }
  // Absent with G50 low: a keyboard attached while idle, with a key
  // in the queue. Search now - and the key wakes the screen like any other.
  // The search empties the queue (mode switch), so the key that pulled the
  // pin low never gets read: finding the keyboard is itself the "there is a key".
  if (!s_presente) {
    const uint32_t t = tab5_agora_ms();
    if (!int_em_baixo() || t - s_ultima_busca < BUSCA_INT_MS) { return false; }
    s_ultima_busca = t;
    return procurar(false);
  }
  // When idle too: pin high and nothing pending = no key, no I2C.
  if (s_int_confiavel && !int_em_baixo() && !s_pode_ter_mais) { return false; }
  uint8_t n = 0;
  if (!M5.Ex_I2C.readRegister(TAB5_TECLADO_ENDERECO, REG_EVENT_NUM, &n, 1, FREQ)) { return false; }
  return n > 0;
}

void tab5_teclado_descartar(void)
{
  s_morta = 0;
  s_tem_guardada = false;
  s_seg_hid = 0;
  if (!s_bus_ok || !s_presente) { return; }
  // Writing 0 to the counter clears the queue and releases INT (datasheet).
  M5.Ex_I2C.writeRegister8(TAB5_TECLADO_ENDERECO, REG_EVENT_NUM, 0x00, FREQ);
  M5.Ex_I2C.writeRegister8(TAB5_TECLADO_ENDERECO, REG_INT_STAT, 0x00, FREQ);
}
