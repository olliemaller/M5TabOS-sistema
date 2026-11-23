// NMEA parsing - PURE LOGIC.
//
// On purpose, this file and nmea.cpp do not include M5GFX, M5Unified or
// anything screen-related: just strings and structs. That makes the parser
// testable off the board, reusable in another project (Meshtastic will want
// something similar) and makes it obvious, looking at the folder, what is
// business logic and what is interface.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define NMEA_MAX_LENGTH   128
// With several constellations at once, 32 is tight. Visible GPS(12) +
// GLONASS(10) + Galileo(10) + BeiDou easily exceed it.
#define MAX_SATS_TRACKED  48

typedef enum {
    GPS, GLONASS, GALILEO, BEIDOU, QZSS, SBAS, UNKNOWN
} GnssType;

// A raw, complete NMEA sentence, checksum already validated.
typedef struct {
    char sentence[NMEA_MAX_LENGTH];
} NMEARawSentence_t;

// Data for ONE satellite, already parsed - this is what the skyplot consumes.
typedef struct {
    GnssType type;
    uint8_t  prn;         // satellite ID (1-32 GPS, etc)
    uint8_t  elevation;   // 0-90 degrees (0 = horizon, 90 = zenith)
    uint16_t azimuth;     // 0-359 degrees (0 = North, 90 = East)
    uint8_t  snr;         // 0-99 dB (0 = not acquired yet)

    // The receiver KNOWS this satellite exists but has no position for it
    // yet: in NMEA that arrives as EMPTY elevation/azimuth fields. Since
    // atoi("") returns 0, without this flag they would all land on the same
    // point of the plot (elevation 0 = horizon, azimuth 0 = north) and pile up.
    // Whoever draws must skip those with tem_posicao == false.
    bool     tem_posicao;

    // WHEN the position above was actually updated (ms of uptime).
    //
    // It exists because "tem_posicao" cannot tell a fresh observation from an
    // old echo. The parser keeps the last known position when a GSV arrives
    // without elevation/azimuth - which is RIGHT for the screen (stops the
    // satellite flickering in and out) and WRONG for the file log.
    //
    // Without it, the app wrote 2047 lines over 2h45m that
    // collapsed into 23 positions: the receiver stopped sending positions, the
    // parser held on to the last one, and the logger wrote the same point every
    // minute as if it were a new observation. Whoever WRITES must check this
    // age; whoever DRAWS can ignore it.
    uint32_t pos_ms;

    bool     valid;
} SatelliteInfo_t;

// Complete snapshot of the visible constellation.
typedef struct {
    SatelliteInfo_t satellites[MAX_SATS_TRACKED];
    uint8_t count;
} SatelliteSnapshot_t;

// UTC date and time from the GPS itself.
//
// Why this matters: system time is uptime, which resets to zero on every
// reboot. To name a file by day and to trace orbits across sessions, only
// satellite time will do - it is the same for everyone, always.
typedef struct {
    bool     valida;    // both date AND time are known
    bool     fix;       // the receiver declared a valid position (RMC field A)
    uint16_t ano;       // 2026
    uint8_t  mes, dia;
    uint8_t  hora, min, seg;
} nmea_tempo_t;

// Parses an RMC sentence. Returns true if it extracted date AND time.
//
// $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
//         |     |                                    |
//         |     status: A=valid V=warning            ddmmyy
//         hhmmss.ss UTC
bool nmea_parse_rmc(const char *sentence, nmea_tempo_t *saida);

// Position of the RECEIVER - the difference between "where the satellites
// are" (GSV, already there) and "where I am" (GGA, the missing piece).
//
// GGA is the canonical position sentence: it is the only one that carries
// ALTITUDE along with latitude/longitude, plus HDOP (how good the geometry of
// the satellites used is) and the fix QUALITY (plain GPS, DGPS, RTK...) -
// data neither RMC nor GSV provide.
typedef struct {
    bool     valida;        // the sentence arrived and had the expected fields

    // DECIMAL DEGREES, hemisphere sign already applied: negative is
    // South/West, positive is North/East. It is the format anything that
    // plots maps (including Google Maps' own search bar) accepts directly,
    // without the reader doing degree/minute arithmetic in their head.
    double   lat;
    double   lon;

    float    altitude_m;    // above sea level (WGS84/geoid reference)
    float    hdop;          // horizontal dilution of precision - lower is better

    // 0=no fix 1=GPS 2=DGPS 4=RTK fixed 5=RTK float 6=estimated (dead reckoning)
    // With NO FIX (0), lat/lon are ZEROED on purpose - see nmea_parse_gga().
    uint8_t  qualidade;

    uint8_t  sats_usados;   // how many WENT INTO the fix (!= GSV "in view")
} nmea_posicao_t;

// $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
//               |lat    |  |lon     |  |  |  |  |altitude
//               |       |  |        |  |  |  |  M = metres
//               |       |  |        |  |  |  HDOP
//               |       |  |        |  |  sats used in the fix
//               |       |  |        |  quality (0=no fix)
//               |       N/S|        E/W
// Returns true when the sentence had the quality field (it always does, even
// without a fix - which is why this parser does not require a fix to return true).
bool nmea_parse_gga(const char *sentence, nmea_posicao_t *saida);

// NMEA checksum: XOR of everything between '$' and '*'.
bool nmea_checksum_ok(const char *sentence);

// Extracts the Nth comma-separated field. Returns an empty string if the field
// does not exist - common in NMEA, fields are often missing.
void nmea_campo(const char *sentence, int indice, char *out, size_t out_tam);

// Accumulates a GSV sentence. GSVs arrive as several messages per round, so
// the parser keeps internal state until the round closes.
// Returns true when the snapshot is COMPLETE - only then is it worth drawing.
bool nmea_parse_gsv(const char *sentence, SatelliteSnapshot_t *saida);

// Clears the accumulator. Call it when opening the app, so as not to inherit
// state from a previous session.
void nmea_reset(void);
