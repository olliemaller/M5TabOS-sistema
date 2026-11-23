// TIME-ZONE REGIONS WITH DAYLIGHT SAVING.
//
// The setting used to be a fixed offset (ajustes_fuso_min) that people in
// places with daylight saving had to change by hand twice a year. A REGION
// carries the standard offset AND the rule for when summer time starts and
// ends, so the clock moves by itself (Melbourne goes to UTC+11 on the first
// Sunday of October and back to UTC+10 on the first Sunday of April).
//
// Region 0 is "manual": the fixed offset from ajustes_fuso_min, exactly as
// before - nothing changes for anyone who never picks a region.
//
// As before, ONLY DISPLAY uses this. The RTC, the logs, the mesh and every
// stored timestamp stay in plain UTC.
#pragma once

#include <stdint.h>
#include <stddef.h>

#define TAB5_FUSO_MANUAL 0

// How many regions there are (including the manual one at index 0).
int tab5_fuso_n(void);

// Display name of region 'i' ("Melbourne, Sydney, Hobart"), or nullptr.
const char* tab5_fuso_nome(int i);

// Standard (winter) offset of region 'i', in minutes from UTC. For the manual
// region this is ajustes_fuso_min().
int tab5_fuso_padrao_min(int i);

// True when region 'i' has daylight saving at all.
bool tab5_fuso_tem_verao(int i);

// The offset in effect at 'epoch_utc', in seconds, for the region chosen in
// Settings (ajustes_fuso_zona). This is what tab5_hora_fuso_s() returns.
int tab5_fuso_deslocamento_s(uint32_t epoch_utc);

// True when daylight saving is in effect at 'epoch_utc' in the chosen region.
bool tab5_fuso_em_verao(uint32_t epoch_utc);

// "UTC+11:00 (summer time)" for the chosen region at 'epoch_utc'.
void tab5_fuso_texto(uint32_t epoch_utc, char* out, size_t n);
