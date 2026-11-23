// Time-zone regions with daylight saving - see tab5_fuso.hpp.
#include "tab5_fuso.hpp"
#include "ajustes.h"
#include "idioma.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// A transition: the Nth (1-4, 5 = last) weekday (0 = Sunday) of a month, at a
// local wall-clock time (minutes after midnight) in the offset in effect just
// BEFORE the change - the way every civil rule is written ("2:00 standard
// time" when summer starts, "3:00 summer time" when it ends).
typedef struct {
  uint8_t  mes;      // 1-12
  uint8_t  semana;   // 1-4, 5 = last
  uint8_t  dia_sem;  // 0 = Sunday
  uint16_t minuto;   // local minutes after midnight
} regra_t;

typedef struct {
  const char* nome;
  int16_t     padrao_min;   // standard offset
  int16_t     verao_min;    // summer offset (== padrao_min: no daylight saving)
  regra_t     inicio;       // summer time starts
  regra_t     fim;          // summer time ends
} zona_t;

#define SEM_VERAO { 0, 0, 0, 0 }, { 0, 0, 0, 0 }
// The common rules (as in force in 2026).
#define R_AU   { 10, 1, 0, 120 }, { 4, 1, 0, 180 }    // south-east Australia
#define R_NZ   { 9, 5, 0, 120 },  { 4, 1, 0, 180 }    // New Zealand
#define R_UE(h) { 3, 5, 0, (uint16_t)((h) * 60) }, { 10, 5, 0, (uint16_t)(((h) + 1) * 60) }   // EU: 01:00 UTC both ways
#define R_EUA  { 3, 2, 0, 120 },  { 11, 1, 0, 120 }   // USA and Canada

static const zona_t ZONAS[] = {
  { nullptr,                                 0,    0, SEM_VERAO },   // manual: ajustes_fuso_min
  { "Melbourne, Sydney, Canberra, Hobart",   600,  660, R_AU },
  { "Adelaide",                              570,  630, R_AU },
  { "Brisbane",                              600,  600, SEM_VERAO },
  { "Darwin",                                570,  570, SEM_VERAO },
  { "Perth",                                 480,  480, SEM_VERAO },
  { "Auckland, Wellington",                  720,  780, R_NZ },
  { "Tokyo, Seoul",                          540,  540, SEM_VERAO },
  { "Beijing, Hong Kong, Singapore",         480,  480, SEM_VERAO },
  { "Bangkok, Jakarta",                      420,  420, SEM_VERAO },
  { "Kathmandu",                             345,  345, SEM_VERAO },
  { "India",                                 330,  330, SEM_VERAO },
  { "Dubai",                                 240,  240, SEM_VERAO },
  { "Athens, Helsinki, Kyiv",                120,  180, R_UE(3) },
  { "Johannesburg, Cairo",                   120,  120, SEM_VERAO },
  { "Paris, Berlin, Madrid, Rome",            60,  120, R_UE(2) },
  { "London, Dublin, Lisbon",                  0,   60, R_UE(1) },
  { "UTC",                                     0,    0, SEM_VERAO },
  { "São Paulo, Buenos Aires",              -180, -180, SEM_VERAO },
  { "New York, Toronto",                    -300, -240, R_EUA },
  { "Mexico City",                          -360, -360, SEM_VERAO },
  { "Chicago",                              -360, -300, R_EUA },
  { "Denver",                               -420, -360, R_EUA },
  { "Phoenix",                              -420, -420, SEM_VERAO },
  { "Los Angeles, Vancouver",               -480, -420, R_EUA },
  { "Anchorage",                            -540, -480, R_EUA },
  { "Honolulu",                             -600, -600, SEM_VERAO },
};
#define N_ZONAS (int)(sizeof(ZONAS) / sizeof(ZONAS[0]))

int tab5_fuso_n(void) { return N_ZONAS; }

const char* tab5_fuso_nome(int i)
{
  if (i == TAB5_FUSO_MANUAL) { return tr("Manual (deslocamento fixo)"); }
  return (i > 0 && i < N_ZONAS) ? ZONAS[i].nome : nullptr;
}

int tab5_fuso_padrao_min(int i)
{
  if (i <= TAB5_FUSO_MANUAL || i >= N_ZONAS) { return ajustes_fuso_min(); }
  return ZONAS[i].padrao_min;
}

bool tab5_fuso_tem_verao(int i)
{
  return i > TAB5_FUSO_MANUAL && i < N_ZONAS && ZONAS[i].verao_min != ZONAS[i].padrao_min;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
static int64_t dias_desde_1970(int ano, unsigned mes, unsigned dia)
{
  ano -= mes <= 2;
  const int64_t era = (ano >= 0 ? ano : ano - 399) / 400;
  const unsigned yoe = (unsigned)(ano - era * 400);
  const unsigned doy = (153 * (mes + (mes > 2 ? -3 : 9)) + 2) / 5 + dia - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static bool bissexto(int a) { return (a % 4 == 0 && a % 100 != 0) || a % 400 == 0; }

// The UTC instant of a rule in a given year, with 'antes_min' the offset in
// effect before the change.
static int64_t instante(int ano, const regra_t& r, int antes_min)
{
  static const uint8_t DIAS_MES[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  const int64_t d1 = dias_desde_1970(ano, r.mes, 1);
  const int sem1 = (int)((d1 % 7 + 7 + 4) % 7);   // 1970-01-01 was a Thursday
  int dia = 1 + (r.dia_sem - sem1 + 7) % 7;         // first such weekday
  const int n_dias = DIAS_MES[r.mes - 1] + (r.mes == 2 && bissexto(ano));
  if (r.semana >= 5) {
    while (dia + 7 <= n_dias) { dia += 7; }
  } else {
    dia += 7 * (r.semana - 1);
  }
  return dias_desde_1970(ano, r.mes, (unsigned)dia) * 86400 + (int64_t)r.minuto * 60 - (int64_t)antes_min * 60;
}

static bool verao_em(const zona_t& z, uint32_t epoch)
{
  if (z.verao_min == z.padrao_min || !z.inicio.mes) { return false; }
  const time_t local = (time_t)epoch + z.padrao_min * 60;
  struct tm t;
  gmtime_r(&local, &t);
  const int ano = t.tm_year + 1900;
  const int64_t ini = instante(ano, z.inicio, z.padrao_min);
  const int64_t fim = instante(ano, z.fim, z.verao_min);
  const int64_t e = (int64_t)epoch;
  // Northern hemisphere: summer inside the year; southern: it wraps New Year.
  return ini < fim ? (e >= ini && e < fim) : (e >= ini || e < fim);
}

static int zona_escolhida(void)
{
  const int z = ajustes_fuso_zona();
  return (z > 0 && z < N_ZONAS) ? z : TAB5_FUSO_MANUAL;
}

bool tab5_fuso_em_verao(uint32_t epoch_utc)
{
  const int z = zona_escolhida();
  return z != TAB5_FUSO_MANUAL && verao_em(ZONAS[z], epoch_utc);
}

int tab5_fuso_deslocamento_s(uint32_t epoch_utc)
{
  const int z = zona_escolhida();
  if (z == TAB5_FUSO_MANUAL) { return ajustes_fuso_min() * 60; }
  const zona_t& zz = ZONAS[z];
  return (verao_em(zz, epoch_utc) ? zz.verao_min : zz.padrao_min) * 60;
}

void tab5_fuso_texto(uint32_t epoch_utc, char* out, size_t n)
{
  const int m = tab5_fuso_deslocamento_s(epoch_utc) / 60;
  snprintf(out, n, "UTC%c%d:%02d%s", m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60,
           tab5_fuso_em_verao(epoch_utc) ? tr(" (verão)") : "");
}
