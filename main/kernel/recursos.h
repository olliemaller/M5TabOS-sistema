// RESOURCES - what each one is using, so the SYSTEM can decide on suspension.
//
// THE RULE (docs/SUSPENSAO_2026-11-05.md): whoever OWNS the hardware declares what
// it is using, inside its own on/off - the camera when turning on the
// sensor, the web server when coming up, Mesh when opening the UART. The app (in C or in
// Berry) never holds off suspension: using the service is enough, and the script that
// turns on the camera appears here because the camera declared itself.
//
// WHAT SUSPENSION PRESERVES for each resource is in a single table
// (POLITICA, in recursos.cpp): light sleep,
// Wi-Fi receiver, Wi-Fi against napping, screen on. The kernel combines the
// policies of everything in use and applies them - the mechanisms below
// (PM locks, Wi-Fi bits) are its detail.
//
// There used to be seven mechanisms, each with its own list of bits, and hardware fixes
// living in apps (the camera hook that was missing in the browser and
// brought the Tab5 down on waking).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  RECURSO_CAMERA = 0,      // sensor, ISP and PPA running
  RECURSO_SOM,             // I2S playing (Music)
  RECURSO_PROCESSAMENTO,   // heavy maths that cannot run at 40 MHz (a command's TLS)
  RECURSO_REDE,            // a service answering the network (web server)
  RECURSO_RADIO_ESCUTA,    // the Wi-Fi receiver on (ESP-NOW chatting, CSI, sniffing)
  RECURSO_SERIAL_GROVE,    // the Grove UART (the Meshtastic node)
  RECURSO_USB_HOST,        // a device on USB-A
  RECURSO_CARTAO,          // long write to the card (copy, move)
  RECURSO_ENERGIA_EXTERNA, // on mains power: saving is not worth the slower response
  RECURSO_TELA_ACESA,      // someone needs the screen on (the PC using the card)
  RECURSO_N
} recurso_t;

// DECLARES ('usando' = true) or RELEASES the use of 'r' by 'quem' ("navegador",
// "Meshtastic", the app name). Per (resource, who) pair: calling again with the
// same value does nothing - whoever reads a state periodically can pass it on
// without counting. From any task, without blocking (the PM and Wi-Fi locks
// change immediately). The idle clock ceiling is fixed, the kernel's (core.cpp).
// 'quem' is copied. false = no slot (RECURSOS_MAX uses at the same time).
bool sistema_recurso(recurso_t r, const char* quem, bool usando);
static inline bool sistema_recurso_usar(recurso_t r, const char* quem)   { return sistema_recurso(r, quem, true); }
static inline void sistema_recurso_soltar(recurso_t r, const char* quem) { sistema_recurso(r, quem, false); }

// WHO USES WHAT, now (console 'energia quem', Tasks, Berry
// sistema.recursos()). Returns how many.
#define RECURSOS_MAX 24
typedef struct {
  recurso_t r;
  char      quem[24];
  uint32_t  desde_ms;   // tab5_agora_ms of when it declared
} recurso_uso_t;
int sistema_recursos(recurso_uso_t* out, int max);

const char* sistema_recurso_nome(recurso_t r);   // "camera", "som"...
// Is anyone using 'r' now? (flash drive mode asks whether there is a cable.)
bool sistema_recurso_em_uso(recurso_t r);
// Is 'quem' using 'r' now? (idle asks whether the open app uses the camera.)
bool sistema_recurso_de(recurso_t r, const char* quem);

// ---- for the kernel ---------------------------------------------------------
// In one line, for the idle log: "camera(navegador) rede(servidor web)".
void recursos_resumo(char* out, size_t n);
