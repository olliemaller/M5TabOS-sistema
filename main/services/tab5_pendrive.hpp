// USB DRIVE MODE: the Tab5's SD card appears on the computer as a USB disk.
//
// -------------------------------------------------------------------------
// HOW, IN HARDWARE
// -------------------------------------------------------------------------
// The Tab5's USB-C goes to the P4's full-speed PHY 0, which normally belongs to
// the USB-Serial-JTAG (the console, flashing). The P4 has a second controller,
// the full-speed OTG (the "USB_WRAP"), and a selector that switches which of the two
// controllers talks to which PHY (LP_SYS.usb_ctrl.sw_usb_phy_sel). Turning on the
// drive = JTAG releases PHY 0, OTG takes it, and TinyUSB announces itself to the
// computer as mass storage. Turning off undoes it, and the console comes back.
// It is the same recipe as M5Launcher (src/massStorage.cpp), which already runs on the Tab5.
//
// IN USB DRIVE MODE THERE IS NO CONSOLE nor flashing over the cable: the computer sees
// a disk, not the serial port. The log carries on being written to the card as soon
// as it comes back (tab5_sd_anexar holds whatever arrives).
//
// -------------------------------------------------------------------------
// THE SD CARD
// -------------------------------------------------------------------------
// Never two owners at once - see tab5_sd_emprestar(). The session below
// lends the card BEFORE turning USB on and only takes it back AFTER turning it off.
//
// The computer receives SECTORS, not files: it reads and writes straight to the card,
// with no cache at all on the Tab5 side (each write is already on the card when the
// computer gets the "ok"). What can be left halfway is the COMPUTER'S cache -
// which is why the right thing is to eject there before pulling the cable. Settings >
// USB says so and only takes the card back without ejecting if the person insists.
#pragma once

#include <stdint.h>
#include "sd_protocol_types.h"

// ---------------------------------------------------------------------------
// THE SESSION (in the background - the Tab5 stays usable during USB drive mode)
// ---------------------------------------------------------------------------
// Turning on: the card is lent (tab5_sd_emprestar: pending data is written, the
// snapshot is taken, the write gates start going to the journal in flash) and
// USB comes up. The status bar shows the notice "Cartao no computador" (Card on
// computer) for as long as it lasts; tapping it opens Settings > USB.
//
// It ends in two ways: the computer EJECTS (the card comes back by itself - nothing
// left in cache on that side) or the person asks for it back in Settings > USB (the
// screen confirms first, if the computer still has the disk mounted). USB
// goes first, the card comes back, the journal is applied and the result stays on
// screen and, if there was a conflict, in the status bar.
//
// System rules, not an app's:
//   - does not start with an app open that REQUIRES the card (app_t.exige_sd), and that
//     app does not open while it lasts (the kernel shows the reason);
//   - with the disk mounted on the computer the screen does not go idle
//     (RECURSO_TELA_ACESA, kernel/recursos.h);
//   - in USB drive mode the cable is neither console nor flashing, and the CPU stays at
//     maximum clock while the computer is talking;
//   - bus idle (cable pulled without ejecting, computer asleep) for
//     more than 2 s: the session carries on, but the screen may turn off and the clock drops -
//     they come back by themselves with the computer. On with no computer at all, the
//     clock drops after 10 s.
typedef enum {
  PENDRIVE_PARADO = 0,
  PENDRIVE_PREPARANDO,   // lending the card, bringing USB up
  PENDRIVE_ATIVO,        // the computer has the card
  PENDRIVE_DEVOLVENDO,   // USB going down, card coming back, journal being applied
} tab5_pendrive_fase_t;

// Why the last attempt did not turn on (PENDRIVE_OK = it did, or never tried).
typedef enum {
  PENDRIVE_OK = 0,
  PENDRIVE_RECUSA_APP,         // an open app requires or holds the card (close it)
  PENDRIVE_RECUSA_EM_USO,      // a service held the card beyond the timeout
  PENDRIVE_RECUSA_SEM_CARTAO,  // no card, or it did not respond
  PENDRIVE_RECUSA_SEM_DIARIO,  // the "reserva" partition is missing (flash the new table)
  PENDRIVE_RECUSA_USB,         // USB did not come up (the card is already back)
} tab5_pendrive_motivo_t;

// Asks to turn on. Does not block (the session has its own task). false = there
// was already a session, or refused on the spot (the reason is in the state).
bool tab5_pendrive_iniciar(void);

// Asks to give the card back to the Tab5. CONFIRMATION is the caller's job (the screen):
// if the computer still has the disk mounted and has not ejected, ask
// first. Does not block.
void tab5_pendrive_devolver(void);

// Where the USB drive is waiting now (to say where it stopped, when it stops).
typedef enum {
  PENDRIVE_ESPERA_NADA = 0,     // the computer has not done anything yet
  PENDRIVE_ESPERA_COMPUTADOR,   // everything answered: the computer's turn to ask
  PENDRIVE_ESPERA_USB,          // a read started and the data/status have not gone out yet
  PENDRIVE_ESPERA_CARTAO,       // inside a card read/write
} tab5_pendrive_espera_t;

// USB internals, for the screen.
typedef struct {
  bool     ligado;        // USB is acting as a USB drive
  bool     montado;       // the computer enumerated the disk
  bool     suspenso;      // the bus stopped: cable pulled or computer asleep
  bool     ejetado;       // the computer ejected (START STOP UNIT with "eject")
  bool     ativo;         // there was a read/write in the last 2 s
  uint64_t lidos;         // bytes read by the computer since turning on
  uint64_t gravados;      // bytes written by the computer since turning on
  uint32_t erros;         // reads/writes the card refused
  uint32_t ms_sem_escrita;// since the last write (UINT32_MAX = none)
  // DIAGNOSTICS - how far the computer got, in the order it does things:
  uint32_t n_config;      // recognised the device (configured USB)
  uint32_t n_inquiry;     // asked what it is
  uint32_t n_capacidade;  // asked the size
  uint32_t n_leituras;    // read (the FAT table comes first)
  uint8_t  op_recusado;   // last refused SCSI command (0 = none)
  uint32_t leitura_max_ms;// slowest card read
  uint32_t n_tur;         // asked whether the disk is ready (TEST UNIT READY)
  uint32_t n_sense;       // asked what the error was (REQUEST SENSE)
  uint32_t n_respostas;   // commands with the response delivered (other than read/write)
  uint8_t  op_respondido; // the last of them
  uint32_t n_soltou;      // deconfigured: bus reset or cable
  uint32_t n_leituras_ok; // read commands with data and status delivered
  uint32_t n_cmd_leitura; // read commands received
  uint32_t ultimo_setor;  // last chunk requested from the card
  uint32_t parado_ms;     // since the last sign of life (computer or card)
  tab5_pendrive_espera_t onde;
} tab5_pendrive_usb_t;
void tab5_pendrive_usb(tab5_pendrive_usb_t* u);

typedef struct {
  tab5_pendrive_fase_t   fase;
  tab5_pendrive_motivo_t motivo;
  bool                   por_ejecao;   // the last session ended by ejecting on the computer
  bool                   sem_ejetar;   // ...or with the disk mounted, without ejecting: the computer
                                       // will check the card next time (slow)
  bool                   oferta;       // computer plugged into the cable, waiting for the person to decide
  tab5_pendrive_usb_t    usb;
} tab5_pendrive_estado_t;
void tab5_pendrive_estado(tab5_pendrive_estado_t* e);
// The reason for a refusal in words (the same text as the log), for the console.
const char* tab5_pendrive_motivo_texto(tab5_pendrive_motivo_t m);

// Something for the person to see in Settings > USB (session in progress, computer
// offered, conflict from the last round) - Settings opens straight on that page.
bool tab5_pendrive_pede_atencao(void);
// The person saw the result: the status bar notice goes away.
void tab5_pendrive_resultado_visto(void);

// Is a computer on the other end of the cable RIGHT NOW? With the USB drive off,
// the USB-Serial-JTAG sees it (the SOF frames the computer sends every
// 1 ms); a wall charger sends no SOF - does not count.
bool tab5_pendrive_pc_presente(void);

// THE CONSOLE RE-ANNOUNCES ITSELF: releases the D+ pull-up for a moment and reconnects - to
// the computer it is like unplugging and replugging the cable. For when the cable arrived with
// the chip in light sleep: USB did not answer enumeration and the Mac gave up on the port. Does
// nothing with the USB drive on or with the computer already seeing the console.
// Blocks ~150 ms. From the interface task (services/tab5_bateria calls it).
void tab5_pendrive_console_reanunciar(void);

// Boot, after the system: registers the notice and starts the cable watch (which turns
// the USB drive on by itself when the setting asks - ajustes_pendrive()).
void tab5_pendrive_servico_iniciar(void);
