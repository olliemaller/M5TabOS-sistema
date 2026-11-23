// Discovering what is plugged into the Grove port.
//
// The Tab5 has a single Grove port and it takes one device at a time. Today
// the contenders are:
//   - the GPS module (NMEA, 9600 baud)
//   - the M5 Unit C6L running Meshtastic (framed protobuf, 115200)
// and in future anything else. Instead of the user choosing by hand which app
// may open, the system ASKS THE WIRE.
//
// Both speak UART, but they give themselves away from afar:
//
//   GPS         9600    ASCII lines starting with '$' (e.g. $GPGSV,...)
//   Meshtastic  115200  binary frames starting with 0x94 0xC3 + length
//
// The baud difference is what makes this reliable: at the wrong baud the
// other one shows up as garbage, which matches no signature.
//
// AN IMPORTANT ASYMMETRY: the GPS talks by itself, unprompted. Meshtastic in
// PROTO mode stays SILENT until a client asks - so its probe is active (we
// send a ToRadio{want_config_id} and see whether a frame comes back).
// Searching only by listening would find the GPS and never the C6L.

#pragma once

typedef enum {
  GROVE_NADA = 0,       // nobody answered
  GROVE_GPS,            // NMEA module
  GROVE_MESHTASTIC,     // Unit C6L in PROTO mode
  GROVE_DESCONHECIDO,   // bytes arrived, but match no signature
} grove_tipo_t;

const char* grove_nome(grove_tipo_t t);

// ---------------------------------------------------------------------------
// GROVE OWNERSHIP
// ---------------------------------------------------------------------------
// The Grove has ONE pair of pins and takes ONE device. Today the Meshtastic
// service (permanent, while the C6L is plugged in) and the Azimutal app
// (while it is open) compete for it. Both want the same UART.
//
// Without an arbiter, the second to arrive got "UART driver already
// installed" and failed - which is what happened when opening Azimutal with
// Meshtastic up. Fixing that inside each app would scatter the rule; here it
// lives in one place.
//
// Whoever takes MUST release. The service takes at start-up and never
// releases; the app takes in ao_abrir() and releases in ao_fechar().
typedef enum { GROVE_DONO_NINGUEM = 0, GROVE_DONO_MESH, GROVE_DONO_GPS } grove_dono_t;

// Returns false if there is already another owner. Does NOT take it from
// anyone: first come stays, and the latecomer must tell the user.
bool         tab5_grove_tomar(grove_dono_t quem);
void         tab5_grove_soltar(grove_dono_t quem);
grove_dono_t tab5_grove_dono(void);
const char*  grove_dono_nome(grove_dono_t d);

// Runs the probe. Blocks for a few seconds (turns on the rail, waits for the
// module to boot, tests both bauds). ONLY from the UI task, and ONLY with the
// Grove free - if an app is using the UART, the result is worthless.
grove_tipo_t tab5_grove_detectar(void);

// Listens at a specific baud and DUMPS the raw bytes (hex + ASCII) to the
// console.
//
// The byte count is not enough to diagnose: the probe found "GPS" on a Grove
// where the C6L was, because 209 bytes of garbage contained a '$' followed by
// two capitals. Seeing the content settles any doubt - NMEA is readable text,
// framed protobuf starts with 94 C3, and garbage is neither.
void tab5_grove_despejar(int baud, int ms);

// Last result, without probing again.
grove_tipo_t tab5_grove_tipo(void);

// Is there ANY module on the cable? A level read of the data pin: cheap
// enough to run in a loop, unlike the probe. Does not say WHO it is.
// ONLY valid with the port free - with the UART installed the pin belongs to
// the driver.
bool tab5_grove_presenca(void);

// Clears the detected type, for when the cable goes empty.
void tab5_grove_esquecer(void);
