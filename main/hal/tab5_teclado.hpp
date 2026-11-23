// Tab5 Keyboard (M5Stack Tab5 Keyboard), on the ExtPort1 side connector.
//
// Specification (docs.m5stack.com/en/tab5/Tab5_Keyboard and the source of
// M5Unit-KEYBOARD): STM32F030C8T6 controller, 14x5 matrix (70 keys),
// I2C at address 0x6D.
//
//   ExtPort1:  G0 = SDA | G1 = SCL | G50 = INT | EXT 5V | GND
//              (the connector's G49 is not used by the keyboard)
//
// We do NOT use the M5Unit-KEYBOARD library: it belongs to the M5UnitUnified framework,
// which would drag in a world of dependencies to read three registers. The protocol
// is simple and fully documented down here.
//
// -------------------------------------------------------------------------
// WHICH BUS, AND WHY
// -------------------------------------------------------------------------
// The P4 has 2 high-power I2C controllers and 1 low-power one. And MIND THE
// NUMBERING, which is the opposite of what intuition suggests and cost me a
// serious regression:
//
//   I2C_NUM_1 = INTERNAL bus (GPIO 32/31). Carries the INA226, the
//               expanders AND THE TOUCH (M5GFX.cpp: cfg.i2c_port = 1 for the
//               GT911). M5Unified.cpp:2095 - "in_port = I2C_NUM_1". DO NOT TOUCH.
//   I2C_NUM_0 = EXTERNAL bus, the Grove (54/53).
//               M5Unified.cpp:2080 - "ex_port = I2C_NUM_0".
//
// Pointing the keyboard at port 1 thinking it was the external one has happened. Result: the
// device went idle and NEVER responded to touch again, because the
// touch controller had lost the bus. If one day touch, the battery
// or the expanders start failing together, suspect this file first.
//
// We repoint the EXTERNAL one to the keyboard pins. It costs nothing today: nothing in this
// system uses the Grove as I2C - Azimutal uses it as the GPS UART. And it solves,
// as a bonus, the conflict that cost us time, because with Ex_I2C off 53/54
// nobody reserves those pins any more.
//
// If one day you need an I2C Unit on the Grove, the keyboard moves to the
// LP controller (LP_I2C_NUM_0): GPIO 0 and 1 are inside the 0-15 range that
// is the only LP/RTC one on the P4, so it is feasible - it just wasn't needed now.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/i2c_types.h"

#define TAB5_TECLADO_PORTA      I2C_NUM_0   // the EXTERNAL one - see the warning above
#define TAB5_TECLADO_ENDERECO   0x6D
#define TAB5_TECLADO_SDA        0      // ExtPort1 G0
#define TAB5_TECLADO_SCL        1      // ExtPort1 G1
#define TAB5_TECLADO_INT        50     // ExtPort1 G50: goes low on every event (wakes the screen, kernel/core)

// SPECIAL KEYS, already translated.
//
// The driver translates each key once and delivers the ready-made event (see ao_teclar
// in app.h): text for what is typed, a code for the rest. Each app
// recognising the keyboard on its own would be the same table copied
// everywhere, with the new app forgetting a case.
#define TAB5_TECLA_NENHUMA   0   // the event is TEXT
#define TAB5_TECLA_ENTER     1
#define TAB5_TECLA_APAGAR    2   // Backspace: deletes BEFORE the cursor
#define TAB5_TECLA_ESC       3
#define TAB5_TECLA_TAB       4
#define TAB5_TECLA_ESQUERDA  5   // arrows
#define TAB5_TECLA_DIREITA   6
#define TAB5_TECLA_CIMA      7
#define TAB5_TECLA_BAIXO     8
#define TAB5_TECLA_DEL       9   // Del: deletes AFTER the cursor

// HELD KEY REPEATS - the same timings on both keyboards (the touch one,
// kernel/teclado_virtual, and the physical one): after REPETE_INICIO_MS held,
// once every REPETE_MS. On the physical one, delete (Backspace and Del) and
// the arrows repeat; letters don't.
#define TAB5_TECLA_REPETE_INICIO_MS  450
#define TAB5_TECLA_REPETE_MS          70

// Modifiers (the event's 'mod' field).
#define TAB5_TECLA_CTRL   0x01
#define TAB5_TECLA_SHIFT  0x02
#define TAB5_TECLA_ALT    0x04

// One key, translated. 'texto' is NUL-terminated UTF-8 (may be 2 bytes:
// accented letter composed by the dead key) and comes EMPTY when 'codigo' says
// what it is. With Ctrl or Alt held the text comes anyway - whoever doesn't use
// shortcuts ignores it (the conversation ignores it).
typedef struct {
  char    texto[8];
  uint8_t codigo;   // TAB5_TECLA_*
  uint8_t mod;      // TAB5_TECLA_CTRL/SHIFT/ALT
} tab5_tecla_t;

// Repoints the external I2C and puts the keyboard into HID mode. Returns false if
// nobody responds at 0x6D - without a keyboard the system carries on normally.
// Call from the UI task (it is the one in charge of I2C).
bool tab5_teclado_iniciar(void);

// Is a keyboard attached now? (follows attach and removal with the device on)
bool tab5_teclado_presente(void);

// Reads ONE key, if any, already translated. false = none.
// UI task ONLY.
bool tab5_teclado_ler(tab5_tecla_t* ev);

// Is a repeating key (delete, arrow) held right now? While there is,
// the reader needs to read at the repeat rate, not the typing rate.
bool tab5_teclado_segurando(void);

// TO WAKE FROM IDLE: is a key waiting in the queue? (one I2C transaction,
// consumes nothing). With no known keyboard and G50 low, first searches for a
// freshly attached one. false without a keyboard.
bool tab5_teclado_pendente(void);

// Throws away the whole queue and anything half-done (dead key, held
// key). For the key that WOKE the screen, which must not become a letter.
void tab5_teclado_descartar(void);

// FOR ASCII-ONLY FIELDS (Wi-Fi password, node name): undoes the letter the
// dead key composed, returning the two keys pressed ("á" -> "'a"). A
// password with an apostrophe followed by a vowel must not turn into an accented letter and
// vanish. Text without composition comes out unchanged.
void tab5_teclado_ascii(const char* texto, char* saida, size_t tam);

// Keyboard STM32 firmware version (0 if not read).
uint8_t tab5_teclado_versao(void);
