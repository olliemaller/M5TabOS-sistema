// The journal of deferred writes in internal flash - see hal/reserva_nucleo.hpp
// for the format and rules, and hal/tab5_sd.hpp for WHEN it is used (the SD
// card lent to the computer). The only thing that talks to this is tab5_sd.
//
// Lives in the "reserva" partition (partitions.csv): 2 MB after the app. It is
// empty almost always - it only holds records while the card is away, and is
// erased as soon as they go to the card. Wear is negligible: a few KB per
// USB flash drive session, against ~100k erase cycles per sector.
//
// Safe from any task (one lock inside).
#pragma once

#include "reserva_nucleo.hpp"

// Finds the partition and scans whatever was left from before (a restart in
// the middle of a flash drive session). Call once at boot, before any write.
bool tab5_reserva_iniciar(void);

bool tab5_reserva_gravar(const reserva_reg_t* reg);

// There are records waiting to go to the card.
bool tab5_reserva_pendente(void);

// Applies the pending ones in order (see reserva_aplicar). Returns how many.
int  tab5_reserva_aplicar(reserva_aplicar_fn fn, void* ctx);

// Usage, for the screen: bytes used, size, pending records.
void tab5_reserva_uso(uint32_t* usado, uint32_t* total, uint32_t* pendentes);
