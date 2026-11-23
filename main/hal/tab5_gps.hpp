// GPS service - UART reading and NMEA parsing on the Grove port, regardless
// of which app is on screen.
//
// SAME TEMPLATE as tab5_mesh: the KERNEL turns this service on and off
// (Grove hot-swap - see kernel/core.cpp), not the app. Previously, GPS
// reading lived INSIDE the Azimutal app - UART installed in ao_abrir(),
// uninstalled in ao_fechar(). It worked, but only Azimutal could see the
// position: no other app could ask "where am I" (Mesh needed that for live
// location, and that triggered the change).
//
// Now it is this service that reads the UART and parses RMC/GGA (the
// RECEIVER's time and position). Azimutal still owns what only it needs -
// the constellation skyplot (GSV) and the history saved to the card - and
// for that it subscribes to receive the raw sentences (see tab5_gps_assinar).
//
// GROVE OWNERSHIP: exactly the same arbitration as always
// (tab5_grove_tomar/soltar, GROVE_DONO_GPS) - except that now the taker is
// the service, at boot or on module swap, and no longer the app on opening.

#pragma once

#include "nmea.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// Takes the Grove port (GROVE_DONO_GPS) and brings up the UART + the read
// task. Call only with the Grove free and a real GPS detected - normally it
// is the kernel that decides that (see tab5_grove_detectar() in kernel/core.cpp).
bool tab5_gps_iniciar(void);

// Brings the task down, closes the UART and releases the Grove. false = could
// not finish in time (same semantics as tab5_mesh_parar) - nothing was torn
// down in that case.
bool tab5_gps_parar(void);

bool tab5_gps_servico_ativo(void);

// Last known position of the RECEIVER (not of the satellites - that is
// Azimutal's). 'valida' stays false until the first GGA arrives; 'qualidade'
// 0 means "the receiver is talking, but no fix yet".
nmea_posicao_t tab5_gps_posicao(void);

// UTC date/time from the GPS itself, and whether there is a FIX (RMC field A).
nmea_tempo_t tab5_gps_tempo(void);

// Is there a recent sentence? (same spirit as tab5_mesh_no_ar()).
bool tab5_gps_no_ar(void);

// How many ms since ANY sentence arrived - used by the Grove watchdog in
// kernel/core.cpp to know when the module has gone from the cable, just as
// tab5_mesh_silencio_ms() already did for the radio.
uint32_t tab5_gps_silencio_ms(void);

// RAW SENTENCE SUBSCRIPTION - for whoever needs more than position/time
// (today, only Azimutal, for GSV/skyplot).
//
// ONE SUBSCRIBER ONLY, on purpose: the same spirit as the Grove's "whoever
// takes must release" - simplicity over generality nobody has asked for yet.
// If one day more than one needs it, the way is a small list, not
// complicating this now without real use.
//
// The queue receives EVERY valid sentence (checksum OK), including those the
// service itself already processed (RMC/GGA) - filtering by prefix is the
// subscriber's job, as Azimutal already did before this change.
bool tab5_gps_assinar(QueueHandle_t fila);
void tab5_gps_dessinscrever(void);
