// 32-bit FNV-1a HASH - the system's only copy. Used for cache keys,
// "has it changed?" signatures and data checks; it is not cryptography.
#pragma once

#include <stdint.h>
#include <stddef.h>

#define TAB5_FNV1A_INICIO 2166136261u

// Arbitrary bytes. 'h' chains: the hash of one part can be the start of the next.
static inline uint32_t tab5_fnv1a(const void* dados, size_t n, uint32_t h = TAB5_FNV1A_INICIO)
{
  const uint8_t* p = (const uint8_t*)dados;
  for (size_t i = 0; i < n; ++i) { h = (h ^ p[i]) * 16777619u; }
  return h;
}

// Zero-terminated text. 'sem_caixa' = A-Z count as a-z ("Queen" and "queen"
// give the same hash).
static inline uint32_t tab5_fnv1a_txt(const char* s, uint32_t h = TAB5_FNV1A_INICIO, bool sem_caixa = false)
{
  for (; *s; ++s) {
    uint8_t c = (uint8_t)*s;
    if (sem_caixa && c >= 'A' && c <= 'Z') { c += 32; }
    h = (h ^ c) * 16777619u;
  }
  return h;
}
