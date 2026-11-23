// SINGLE LISTENER FOR AN EVENT, with removal that WAITS for the call in flight.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// Three modules deliver events to ONE registered listener: the BLE scan
// (tab5_ble), the Wi-Fi sniffer and CSI. All three kept just a volatile
// pointer: the caller copies and calls, the remover writes nullptr. Copying
// avoids the null pointer - but does NOT say when a call that has already
// started finishes. Whoever removes the listener in order to then destroy
// what it uses (the Radar's queue) could not know whether it was safe. The
// Radar "solved" it by waiting 40 ms on close - a guess in the UI task, and a
// use-after-free for the first listener slower than the guess.
//
// Here removal (setting nullptr) only RETURNS once no call is in flight.
// From then on, the remover can destroy whatever it likes.
//
// -------------------------------------------------------------------------
// HOW, without a lock
// -------------------------------------------------------------------------
// The caller COUNTS ITSELF in flight BEFORE reading the pointer; the remover
// swaps the pointer BEFORE looking at the count. With atomics (sequential
// consistency), there are only two cases: either the call read the old
// pointer - and so was already counted when the removal looked, and the
// removal waits -, or it read null and does not call.
// The wait is for one short call (listeners just enqueue and return): in
// practice the removal never goes round the loop.
//
// Do NOT remove the listener from INSIDE itself: the removal would wait for
// its own call for ever.

#pragma once

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

template <typename F>
struct tab5_ouvinte_t {
  std::atomic<F>   fn{nullptr};
  std::atomic<int> em_voo{0};
};

// On the event side (any task).
template <typename F, typename... A>
inline void tab5_ouvinte_chamar(tab5_ouvinte_t<F>* o, A... args)
{
  o->em_voo.fetch_add(1);
  const F f = o->fn.load();
  if (f) { f(args...); }
  o->em_voo.fetch_sub(1);
}

// Swaps the listener (nullptr removes) and only returns once the call in
// flight, if there was one, has finished.
template <typename F>
inline void tab5_ouvinte_definir(tab5_ouvinte_t<F>* o, F f)
{
  o->fn.store(f);
  while (o->em_voo.load() != 0) { vTaskDelay(1); }
}
