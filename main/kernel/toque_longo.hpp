// LONG PRESS - the finger held still in the same place for TOQUE_LONGO_MS.
//
// In the app's ao_tocar_nivel, on every sample:
//   if (toque_longo(&s_lp, x, y, pressionado)) { ...action...; sistema_consumir_toque(); }
// Returns true ONCE per touch. If it moves more than TOQUE_LONGO_FOLGA, it becomes
// a drag and does not fire again until the finger lifts. sistema_consumir_toque() stops
// the release from becoming a click.
#pragma once

#include "tab5_hora.hpp"
#include <stdlib.h>

#define TOQUE_LONGO_MS    550
#define TOQUE_LONGO_FOLGA 24

typedef struct { int x, y; uint32_t desde; bool ativo, feito; } toque_longo_t;

static inline bool toque_longo(toque_longo_t* t, int x, int y, bool pressionado)
{
  if (!pressionado) { t->ativo = false; return false; }
  if (!t->ativo) { *t = { x, y, tab5_agora_ms(), true, false }; return false; }
  if (t->feito) { return false; }
  if (abs(x - t->x) > TOQUE_LONGO_FOLGA || abs(y - t->y) > TOQUE_LONGO_FOLGA) { t->feito = true; return false; }
  if (tab5_agora_ms() - t->desde < TOQUE_LONGO_MS) { return false; }
  t->feito = true;
  return true;
}
