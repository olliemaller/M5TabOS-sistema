/* M5tabOS: Berry's memory, in PSRAM, with a BUDGET.
 *
 * Berry makes MANY small allocations (strings, lists, closures). With the
 * default malloc they would land in internal RAM, the most contended memory
 * in the system (see docs/ARQUITETURA.md). PSRAM has 32 MB and is slower - for
 * a script interpreter, the right trade-off.
 *
 * THE BUDGET: all the VMs together (apps, background apps, console) never go
 * past BERRY_ORCAMENTO. Once it is exceeded, berry_malloc returns NULL - Berry
 * then collects garbage, tries again and, with nothing to spare, raises
 * "memory_error" INSIDE the script (the caller's pcall receives it). A
 * 'while true l.push(...)' becomes an error on the app's screen instead of
 * eating the PSRAM of the camera, the mesh and the stacks. */
#include <stddef.h>
#include "esp_heap_caps.h"
#include "berry_mem.h"

static volatile size_t s_usada = 0;
static volatile size_t s_pico  = 0;

static void somar(size_t n)
{
  const size_t u = __atomic_add_fetch(&s_usada, n, __ATOMIC_RELAXED);
  if (u > s_pico) { s_pico = u; }   /* diagnostics only: a race here only gets the peak wrong */
}

static int cabe(size_t n)
{
  return s_usada + n <= BERRY_ORCAMENTO;
}

void* berry_malloc(size_t n)
{
  if (!cabe(n)) { return NULL; }
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (p) { somar(heap_caps_get_allocated_size(p)); }
  return p;
}

void berry_free(void* p)
{
  if (!p) { return; }
  __atomic_sub_fetch(&s_usada, heap_caps_get_allocated_size(p), __ATOMIC_RELAXED);
  heap_caps_free(p);
}

void* berry_realloc(void* p, size_t n)
{
  if (!p) { return berry_malloc(n); }
  const size_t antes = heap_caps_get_allocated_size(p);
  if (n > antes && !cabe(n - antes)) { return NULL; }   /* the old block is still valid */
  void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
  if (!q) { return NULL; }
  __atomic_sub_fetch(&s_usada, antes, __ATOMIC_RELAXED);
  somar(heap_caps_get_allocated_size(q));
  return q;
}

size_t berry_mem_usada(void) { return s_usada; }
size_t berry_mem_pico(void)  { return s_pico; }
int    berry_mem_cabe(size_t n) { return cabe(n); }
