#include "nmea.h"

// nmea.c is a PURE parser - it knows nothing of M5GFX or hardware. FreeRTOS
// comes in only for the tick clock, used to stamp WHEN a position was observed.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

// PERSISTENT satellite TABLE, keyed by (constellation, PRN).
//
// The previous version had a serious bug: it cleared the accumulator whenever
// a sentence with msgNum == 1 arrived. But EACH CONSTELLATION has its own
// numbering - GLONASS's $GLGSV,2,1 has msgNum 1 and wiped everything the
// $GPGSV had just filled in. The map flipped between "GPS only" and "GLONASS
// only", with satellites appearing and vanishing every second.
//
// Now each constellation has its own round, and the table keeps them all
// together. A satellite that stops being reported disappears after a few
// rounds, instead of being stuck on screen for ever.
#define AUSENCIAS_PARA_SUMIR 3

typedef struct {
    SatelliteInfo_t sat;
    uint8_t ausencias;         // consecutive rounds it did not appear in
    bool    visto_na_rodada;   // set during the talker's current round
    bool    ocupado;
} entrada_t;

static entrada_t s_tab[MAX_SATS_TRACKED];

// Constellation from the talker prefix: $GPGSV, $GLGSV, $GAGSV, $GBGSV...
static GnssType tipo_do_talker(const char *sentence)
{
    const char a = sentence[1];
    const char b = sentence[2];
    if (a == 'G' && b == 'P') { return GPS;     }
    if (a == 'G' && b == 'L') { return GLONASS; }
    if (a == 'G' && b == 'A') { return GALILEO; }
    if (a == 'G' && b == 'B') { return BEIDOU;  }
    if (a == 'B' && b == 'D') { return BEIDOU;  }
    if (a == 'G' && b == 'Q') { return QZSS;    }
    if (a == 'Q' && b == 'Z') { return QZSS;    }
    return UNKNOWN;
}

static entrada_t* achar_ou_criar(GnssType tipo, uint8_t prn)
{
    entrada_t *livre = NULL;
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].ocupado) {
            if (s_tab[i].sat.type == tipo && s_tab[i].sat.prn == prn) { return &s_tab[i]; }
        } else if (livre == NULL) {
            livre = &s_tab[i];
        }
    }
    if (livre) {
        memset(livre, 0, sizeof(*livre));
        livre->ocupado   = true;
        livre->sat.type  = tipo;
        livre->sat.prn   = prn;
        livre->sat.valid = true;
    }
    return livre;   // NULL = table full, ignore this satellite
}

void nmea_reset(void)
{
    memset(s_tab, 0, sizeof(s_tab));
}

bool nmea_checksum_ok(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$') { return false; }

    uint8_t calc = 0;
    const char *p = sentence + 1;          // skip the '$'
    while (*p && *p != '*') { calc ^= (uint8_t)*p++; }
    if (*p != '*') { return false; }       // sentence without a checksum field

    // The two characters after the '*' are the checksum in hexadecimal.
    char hex[3] = { p[1], p[2], 0 };
    if (hex[0] == 0 || hex[1] == 0) { return false; }
    return (uint8_t)strtol(hex, NULL, 16) == calc;
}

void nmea_campo(const char *sentence, int indice, char *out, size_t out_tam)
{
    out[0] = '\0';
    int atual = 0;
    const char *inicio = sentence;
    const char *p = sentence;

    while (*p) {
        if (*p == ',' || *p == '*') {
            if (atual == indice) {
                size_t len = (size_t)(p - inicio);
                if (len >= out_tam) { len = out_tam - 1; }
                strncpy(out, inicio, len);
                out[len] = '\0';
                return;
            }
            atual++;
            inicio = p + 1;
        }
        p++;
    }
    // The last field of a sentence without a "*hh" checksum ends at the end of
    // the string (or at CR/LF): it used to come back empty.
    if (atual == indice) {
        size_t len = strcspn(inicio, "\r\n");
        if (len >= out_tam) { len = out_tam - 1; }
        memcpy(out, inicio, len);
        out[len] = '\0';
    }
}

// $GPGSV,3,1,11,10,63,137,42,07,61,098,45,...*7F
//         |  | |  |------ sat 1 ------|
//         |  | total satellites in the constellation
//         |  number of this sentence
//         total sentences in the round
bool nmea_parse_rmc(const char *sentence, nmea_tempo_t *saida)
{
    char campo[16];

    // Field 1: hhmmss.ss - only the first six digits matter.
    nmea_campo(sentence, 1, campo, sizeof(campo));
    if (strlen(campo) < 6) { return false; }

    const int hora = (campo[0]-'0')*10 + (campo[1]-'0');
    const int min  = (campo[2]-'0')*10 + (campo[3]-'0');
    const int seg  = (campo[4]-'0')*10 + (campo[5]-'0');

    // Field 2: status. 'A' = valid position, 'V' = warning. We keep the state
    // but do NOT require 'A' to accept the date: many receivers already give
    // the correct time and date before getting a fix, and that is enough to
    // name the day's file.
    nmea_campo(sentence, 2, campo, sizeof(campo));
    const bool fix = (campo[0] == 'A');

    // Field 9: ddmmyy.
    nmea_campo(sentence, 9, campo, sizeof(campo));
    if (strlen(campo) < 6) { return false; }

    const int dia = (campo[0]-'0')*10 + (campo[1]-'0');
    const int mes = (campo[2]-'0')*10 + (campo[3]-'0');
    const int ano = (campo[4]-'0')*10 + (campo[5]-'0');

    // Physical ranges. A noisy line gets past the checksum now and then, and a
    // month 47 would become an absurd file name that stays there for ever.
    if (hora > 23 || min > 59 || seg > 60) { return false; }
    if (dia < 1 || dia > 31 || mes < 1 || mes > 12) { return false; }

    saida->valida = true;
    saida->fix    = fix;
    saida->ano    = (uint16_t)(2000 + ano);   // NMEA gives a 2-digit year
    saida->mes    = (uint8_t)mes;
    saida->dia    = (uint8_t)dia;
    saida->hora   = (uint8_t)hora;
    saida->min    = (uint8_t)min;
    saida->seg    = (uint8_t)seg;
    return true;
}

// Converts an NMEA coordinate field (ddmm.mmmm or dddmm.mmmm) to signed
// decimal degrees. 'digitos_grau' is 2 for latitude, 3 for longitude - that is
// exactly the difference between the two formats, which is why the function
// asks for the number instead of guessing from the string length.
static double nmea_coord(const char *campo, char hemisferio, int digitos_grau)
{
    if ((int)strlen(campo) <= digitos_grau) { return 0.0; }

    char graus_txt[4] = { 0, 0, 0, 0 };
    memcpy(graus_txt, campo, (size_t)digitos_grau);
    const double graus   = atof(graus_txt);
    const double minutos = atof(campo + digitos_grau);

    double v = graus + minutos / 60.0;
    if (hemisferio == 'S' || hemisferio == 'W') { v = -v; }
    return v;
}

bool nmea_parse_gga(const char *sentence, nmea_posicao_t *saida)
{
    char lat[16], lat_h[4], lon[16], lon_h[4];
    char qual[4], nsat[4], hdop[16], alt[16];

    nmea_campo(sentence, 2, lat,  sizeof(lat));
    nmea_campo(sentence, 3, lat_h, sizeof(lat_h));
    nmea_campo(sentence, 4, lon,  sizeof(lon));
    nmea_campo(sentence, 5, lon_h, sizeof(lon_h));
    nmea_campo(sentence, 6, qual, sizeof(qual));
    nmea_campo(sentence, 7, nsat, sizeof(nsat));
    nmea_campo(sentence, 8, hdop, sizeof(hdop));
    nmea_campo(sentence, 9, alt,  sizeof(alt));

    // The quality field is the ONLY thing GGA is guaranteed to always send,
    // fix or no fix. Without it, the sentence is truncated or not a real GGA -
    // none of the other fields can be trusted.
    if (qual[0] == '\0') { return false; }

    saida->qualidade   = (uint8_t)atoi(qual);
    saida->sats_usados = (nsat[0] != '\0') ? (uint8_t)atoi(nsat) : 0;
    saida->hdop         = (hdop[0] != '\0') ? (float)atof(hdop)  : 0.0f;
    saida->altitude_m   = (alt[0]  != '\0') ? (float)atof(alt)   : 0.0f;

    // WITHOUT A FIX, LAT/LON DO NOT REALLY EXIST.
    //
    // With quality 0 the receiver sends EMPTY position fields, not "0.0" - the
    // same logic as tem_posicao in GSV (see nmea.h). Zeroing here instead of
    // letting atof("") slip through stops a (0,0) - the middle of the Atlantic
    // Ocean - being mistaken for a real reading by whoever draws.
    if (saida->qualidade > 0 && lat[0] && lon[0]) {
        saida->lat = nmea_coord(lat, lat_h[0], 2);
        saida->lon = nmea_coord(lon, lon_h[0], 3);
    } else {
        saida->lat = 0.0;
        saida->lon = 0.0;
    }

    saida->valida = true;
    return true;
}

bool nmea_parse_gsv(const char *sentence, SatelliteSnapshot_t *saida)
{
    char campo[16];

    const GnssType tipo = tipo_do_talker(sentence);

    nmea_campo(sentence, 1, campo, sizeof(campo));
    const int total_msgs = atoi(campo);

    nmea_campo(sentence, 2, campo, sizeof(campo));
    const int num_msg = atoi(campo);

    if (total_msgs <= 0 || num_msg <= 0) { return false; }   // malformed sentence

    // Start of THIS constellation's round: unmark only its satellites.
    if (num_msg == 1) {
        for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
            if (s_tab[i].ocupado && s_tab[i].sat.type == tipo) {
                s_tab[i].visto_na_rodada = false;
            }
        }
    }

    // Each GSV carries up to 4 satellites, in blocks of 4 fields from field 4.
    for (int bloco = 0; bloco < 4; ++bloco) {
        const int base = 4 + bloco * 4;

        char prn[8], elev[8], az[8], snr[8];
        nmea_campo(sentence, base + 0, prn,  sizeof(prn));
        nmea_campo(sentence, base + 1, elev, sizeof(elev));
        nmea_campo(sentence, base + 2, az,   sizeof(az));
        nmea_campo(sentence, base + 3, snr,  sizeof(snr));

        if (prn[0] == '\0') { continue; }              // empty block

        entrada_t *e = achar_ou_criar(tipo, (uint8_t)atoi(prn));
        if (e == NULL) { continue; }                   // table full

        // HERE is the missing filter. An empty field is not zero: it is "don't
        // know". We test the STRING, not the atoi result.
        const bool tem_pos = (elev[0] != '\0') && (az[0] != '\0');

        if (tem_pos) {
            const int  el = atoi(elev);
            const int  azi = atoi(az);
            // Discard values outside the physical range - noisy lines produce them.
            if (el >= 0 && el <= 90 && azi >= 0 && azi <= 359) {
                e->sat.elevation   = (uint8_t)el;
                e->sat.azimuth     = (uint16_t)azi;
                e->sat.tem_posicao = true;
                // Stamp ONLY here: it is the only point where the position comes
                // from a real observation.
                e->sat.pos_ms      = pdTICKS_TO_MS(xTaskGetTickCount());
            }
        }
        // If no position came this time, KEEP the last known one. Satellites do
        // not move suddenly, and flickering between "has" and "hasn't" is worse.

        e->sat.snr = (snr[0] == '\0') ? 0 : (uint8_t)atoi(snr);
        e->visto_na_rodada = true;
        e->ausencias = 0;
    }

    if (num_msg != total_msgs) { return false; }   // round not closed yet

    // End of this constellation's round: age those that did not appear and
    // remove those missing for several rounds.
    for (int i = 0; i < MAX_SATS_TRACKED; ++i) {
        if (!s_tab[i].ocupado || s_tab[i].sat.type != tipo) { continue; }
        if (s_tab[i].visto_na_rodada) { continue; }
        if (++s_tab[i].ausencias >= AUSENCIAS_PARA_SUMIR) {
            memset(&s_tab[i], 0, sizeof(s_tab[i]));
        }
    }

    // Publish the WHOLE table - all constellations together, not just the one
    // that has just closed its round.
    uint8_t n = 0;
    memset(saida, 0, sizeof(*saida));
    for (int i = 0; i < MAX_SATS_TRACKED && n < MAX_SATS_TRACKED; ++i) {
        if (s_tab[i].ocupado) { saida->satellites[n++] = s_tab[i].sat; }
    }
    saida->count = n;
    return true;
}
