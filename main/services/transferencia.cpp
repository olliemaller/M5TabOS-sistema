#include "transferencia.h"
#include "tab5_mutex.hpp"
#include "tab5_unicode.hpp"
#include "esp_heap_caps.h"
#include <string.h>

static Trava  s_trava;
static char*  s_buf = nullptr;   // PSRAM, TRANSF_MAX bytes, created on the first copy
static size_t s_n   = 0;

static size_t corte_utf8(const char* s, size_t n) { return (size_t)tab5_utf8_corte(s, (int)n); }

bool transf_copiar(const char* texto)
{
  if (!texto || !texto[0]) { return true; }
  Trava::Guarda g(s_trava);
  if (!s_buf) { s_buf = (char*)heap_caps_malloc(TRANSF_MAX, MALLOC_CAP_SPIRAM); }
  if (!s_buf) { return false; }
  size_t n = strlen(texto);
  if (n > TRANSF_MAX - 1) { n = corte_utf8(texto, TRANSF_MAX - 1); }
  memcpy(s_buf, texto, n);
  s_buf[n] = 0;
  s_n = n;
  return true;
}

size_t transf_colar(char* out, size_t cap)
{
  if (!out || !cap) { return 0; }
  Trava::Guarda g(s_trava);
  size_t n = s_n < cap - 1 ? s_n : corte_utf8(s_buf, cap - 1);
  if (n) { memcpy(out, s_buf, n); }
  out[n] = 0;
  return n;
}

bool transf_vazia(void) { return s_n == 0; }
