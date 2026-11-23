// USB-A - the Tab5's host port (the USB flash drive someone plugs in).
//
// HARDWARE (official schematic, see docs/USB_A_E_FONE.md): the USB-A connector
// goes to the P4's HIGH-SPEED OTG 2.0 controller (dedicated pins, its own
// PHY) - a different controller and different pins from the USB-C ones. Nothing
// here clashes with the console, with flash-drive mode (the Tab5 becoming a
// disk for the computer, services/tab5_pendrive) or with the speaker's I2S. The
// connector's 5V comes through a current-limited switch (MT9700) enabled by the
// expander's USB5V_EN - in the power profile, field usb_a_5v, enforced by the
// preference ajustes_usb_a() (kernel/core.cpp).
//
// WHAT THIS SERVICE DOES: plug in a flash drive and it shows up mounted at
// /usb (FAT32 or exFAT - components/fatfs enables exFAT; NTFS and the Mac
// ones are not, and the partition table must be MBR, not GPT), notifies
// the system (tab5_volumes_mudou -> AVISO_VOLUMES) and puts the "Pendrive"
// notice in the status bar, which opens the Music app. Pull it out: whoever was
// reading is told via tab5_volume_disponivel() and has up to 3 s to let go; then
// the disk disappears.
//
// USERS do not call anything here directly: they use services/tab5_volumes
// (mount/unmount by path). The functions below are for the system, the
// console ('usb') and tab5_volumes itself.
//
// TASKS (stacks in PSRAM): "usb_lib" (the USB Host Library), "usb_msc" (the
// class driver - flash-drive reads finish in it, hence the priority above
// the readers) and "usb" (mount, unmount, read the device).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TAB5_USB_RAIZ  "/usb"

// At boot, after sistema_iniciar(). With the setting off, nothing starts.
void tab5_usb_iniciar(void);

// The setting changed (Settings, console). Turns the connector's 5V on/off
// (through the power profile) and, when turning on the first time, starts the
// USB stack. Turning off cuts power: the flash drive goes as if it had been yanked.
void tab5_usb_ligar(bool ligar);

// Holds / releases the flash drive (see tab5_volumes.hpp). false = no flash
// drive mounted (or it is on its way out).
bool tab5_usb_montar(void);
void tab5_usb_desmontar(void);

// A flash drive is mounted at /usb, and it is not on its way out.
bool tab5_usb_presente(void);

// EJECT - before removing a flash drive the system WROTE to (Files copying
// to it). Yanking it mid-write leaves the FAT half done; ejecting waits for
// whoever holds the flash drive to let go (up to 'espera_ms'), unmounts - FatFs
// writes out what it had cached - and says "safe to remove".
// The device stays plugged in and powered, but without /usb: it only mounts
// again when removed and plugged back in (or when the 5V drops during idle and
// comes back). Blocks: never from the UI task.
typedef enum {
  USB_EJETADO = 0,
  USB_EJETAR_SEM_PENDRIVE,   // there was no flash drive mounted
  USB_EJETAR_EM_USO,         // someone held it past the deadline: stays mounted
} tab5_usb_ejetar_t;
tab5_usb_ejetar_t tab5_usb_ejetar(uint32_t espera_ms);

// The flash drive's name (volume label; without a label, the product it claims
// to be); "" with no flash drive.
void tab5_usb_nome(char* out, size_t tam);

// Flash drive space, in bytes. May be slow (FAT counts the free clusters the
// first time): off the UI task.
bool tab5_usb_espaco(uint64_t* total, uint64_t* livre);

// USB-A DURING IDLE (called by the kernel, from the UI: when the screen goes
// off, on each idle cycle and when it comes on). The connector has NO pin that
// signals "something was plugged in": what sees a device arrive is the USB
// controller itself, and only with the 5V on. So, with the screen off and nobody
// reading from the flash drive (no tab5_usb_montar held - music playing from it
// holds it) and nothing other than a plugged-in disk (headphones charging from
// the connector keep it on), the 5V drops: the flash drive unmounts, the device
// disappears and light sleep, which it was blocking, comes back.
// When the screen comes on the 5V returns and the flash drive mounts again by
// itself (~1 s). Something plugged in with the screen off is only seen when it comes on.
void tab5_usb_repouso(bool repouso);
// The 5V is cut by tab5_usb_repouso (the profile does not turn it back on).
bool tab5_usb_cortado(void);

// For the console and the diagnostics screen.
typedef struct {
  bool     ligado;          // USB stack up and 5V requested
  bool     aparelho;        // something is plugged in (any device)
  uint8_t  velocidade;      // 0 low (1.5 Mb/s), 1 full (12), 2 high (480)
  uint16_t vid, pid;
  uint8_t  classe;          // bDeviceClass (0 = each interface states its own)
  char     produto[40];     // what the device claims to be (UTF-8)
  bool     disco;           // the device is a disk (MSC class) and was installed
  bool     montado;         // ... and its FAT mounted at /usb
  bool     ejetado;         // ... and it was ejected (tab5_usb_ejetar): safe to remove
  uint32_t setores, setor_bytes;
  int      refs;            // who is holding the flash drive now
  uint32_t conexoes;        // devices seen since boot
  char     erro[64];        // the last problem ("" = none)
} tab5_usb_estado_t;
void tab5_usb_estado(tab5_usb_estado_t* out);
