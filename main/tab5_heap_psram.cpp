// THE PSRAM HEAP AT BOOT - see tab5_heap_psram.hpp.

#include "tab5_heap_psram.hpp"

#include <stddef.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "riscv/rv_utils.h"   // rv_utils_set_watchpoint: the watchpoint straight in hardware
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/startup_internal.h"   // ESP_SYSTEM_INIT_FN

static const char *TAG = "heap_psram";

// THE GUARD. IDF builds the PSRAM heap starting from the END of the static
// variables that live in it (.ext_ram.bss) - without aligning to the cache line (128
// bytes on the P4). This guard (noinit section, which IDF places AFTER bss, and
// CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY in sdkconfig.defaults) makes the
// heap start on a cache line of its own. A mitigation, NOT the cause: a build
// with only Wi-Fi code changed went back to crashing on every boot with it in place.
EXT_RAM_NOINIT_ATTR __attribute__((aligned(128), used)) static uint8_t s_guarda[128];

// The heap header is the heap_t from components/heap/multi_heap.c, which IDF
// writes at the start of the region - here, right after the guard.
struct Cabecalho {
    void*  lock;      // written once (multi_heap_set_lock) and never again
    size_t livre;     // free_bytes: changes on every malloc/free
    size_t minimo;    // minimum_free_bytes: likewise
    size_t tamanho;   // pool_size: fixed
    void*  dados;     // heap_data, the TLSF control: fixed (header + 20)
};
extern "C" uint8_t _ext_ram_noinit_end;
static Cabecalho* cabecalho() { return (Cabecalho*)&_ext_ram_noinit_end; }

// Copy in internal RAM, taken right after mounting. dados == nullptr: the
// heap is not where we expect and neither the sentinel nor the check apply.
static Cabecalho s_retrato;

// Survives esp_restart (RTC_NOINIT). [0] = magic, [1] = consecutive clean
// restarts done by the check.
static constexpr uint32_t MAGIA = 0x48505352;   // "HPSR"
RTC_NOINIT_ATTR static uint32_t s_reinicios[2];
static uint32_t s_reinicios_antes;

// THE SENTINEL. lock and heap_data are written at mount time and never again: a
// write watchpoint on them makes the CPU stop AT THE INSTRUCTION that corrupts them
// ("Debug exception" panic with the PC, in the core dump). It is per core, hence
// armed on both. Slots 0 and 1 (FreeRTOS no longer uses 2: see
// WATCHPOINT_END_OF_STACK in sdkconfig.defaults, 06/11/2026). If the
// header gets corrupted without it firing, it was not the CPU: DMA, cache or the PSRAM.
static void armar(void)
{
    if (s_retrato.dados == nullptr) {
        return;
    }
    // STRAIGHT TO THE HARDWARE, not via esp_cpu_set_watchpoint(): that one asks whether
    // there is a debugger (assist_debug_ll_is_debugger_active) and, with the bit set,
    // uses semihosting - an ebreak that, with no debugger answering, is a panic. The
    // bit stays set after OpenOCD (and survives reset): it became a boot-loop
    // panic (06/11/2026). Without a debugger IDF does exactly this.
    rv_utils_set_watchpoint(0, (uint32_t)&cabecalho()->dados, 4, false, true);
    rv_utils_set_watchpoint(1, (uint32_t)&cabecalho()->lock, 4, false, true);
}

// Core 0: right after add_psram_to_heap (103) and BEFORE the static
// constructors (do_global_ctors), which the old sentinel, armed in app_main, did not
// cover.
ESP_SYSTEM_INIT_FN(heap_psram_sentinela_n0, CORE, BIT(0), 104)
{
    const Cabecalho* c = cabecalho();
    if (c->dados == (uint8_t*)c + sizeof(Cabecalho) && c->lock != nullptr) {
        s_retrato = *c;
    }
    armar();
    return ESP_OK;
}

#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
// Core 1: only runs code from the secondary stage on, so arming here
// covers everything it does.
ESP_SYSTEM_INIT_FN(heap_psram_sentinela_n1, SECONDARY, BIT(1), 100)
{
    armar();
    return ESP_OK;
}
#endif

void tab5_heap_psram_conferir(void)
{
    { static void* volatile g = s_guarda; (void)g; }   // unused, the linker would throw the guard away

    if (s_reinicios[0] != MAGIA) {   // just powered on: RTC_NOINIT holds garbage
        s_reinicios[0] = MAGIA;
        s_reinicios[1] = 0;
    }
    s_reinicios_antes = s_reinicios[1];

    if (s_retrato.dados == nullptr) {
        ESP_LOGW(TAG, "PSRAM heap not where expected - no sentinel and no check");
        return;
    }
    const Cabecalho* c = cabecalho();
    const Cabecalho  v = *c;   // a single read; rereading can mask it (it has vanished before with checks)
    if (v.dados == s_retrato.dados && v.lock == s_retrato.lock &&
        v.tamanho == s_retrato.tamanho && v.livre <= v.tamanho && v.minimo <= v.livre) {
        s_reinicios[1] = 0;
        return;
    }

    ESP_EARLY_LOGE(TAG, "HEADER CORRUPTED at %p: lock %p/%p free 0x%x minimum 0x%x size 0x%x/0x%x data %p/%p",
                   c, v.lock, s_retrato.lock, (unsigned)v.livre, (unsigned)v.minimo,
                   (unsigned)v.tamanho, (unsigned)s_retrato.tamanho, v.dados, s_retrato.dados);
    if (s_reinicios[1] >= 3) {
        ESP_EARLY_LOGE(TAG, "already restarted %lu times in a row - carrying on (it will panic)",
                       (unsigned long)s_reinicios[1]);
        s_reinicios[1] = 0;
        return;
    }
    s_reinicios[1]++;
    esp_restart();
}

uint32_t tab5_heap_psram_reinicios(void)
{
    return s_reinicios_antes;
}

// CAUGHT IN THE ACT. The app_main check never caught anything: the header gets corrupted
// INSIDE M5.begin, between the first and second 1.8 MB frame that
// esp_lcd allocates (num_fbs = 2 in Panel_DSI). This one looks at the header's whole
// cache line at the suspect points - before each large allocation
// in PSRAM and after each large cache sync. Corrupted: shows the
// line as the CPU sees it, INVALIDATES the line (discards what the cache holds) and
// shows it again. If the second read comes out right, the PSRAM was fine and the error
// was in the cache; if it comes out the same, the wrong data was already in PSRAM.
#include "esp_rom_sys.h"
#include "esp_cache.h"
extern "C" esp_err_t __real_esp_cache_msync(void* addr, size_t size, int flags);

static DRAM_ATTR uint32_t s_flagrantes = 0;

static void IRAM_ATTR mostrar_linha(const char* rotulo, const uint32_t* w)
{
    esp_rom_printf("heap_psram: %s\n", rotulo);
    for (int i = 0; i < 32; i += 8) {
        esp_rom_printf("  +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4,
                       w[i], w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
    }
}

extern "C" void IRAM_ATTR tab5_heap_psram_checar(const char* onde, size_t tam)
{
    if (s_retrato.dados == nullptr) {
        return;
    }
    volatile uint32_t* linha = (volatile uint32_t*)cabecalho();
    uint32_t w[32];
    for (int i = 0; i < 32; ++i) { w[i] = linha[i]; }
    const Cabecalho* v = (const Cabecalho*)w;
    if (v->dados == s_retrato.dados && v->lock == s_retrato.lock && v->tamanho == s_retrato.tamanho) {
        return;
    }
    ++s_flagrantes;
    esp_rom_printf("heap_psram: FLAGRANTE #%u %s (%u bytes): esperado lock %p tamanho 0x%x dados %p\n",
                   (unsigned)s_flagrantes, onde, (unsigned)tam, s_retrato.lock,
                   (unsigned)s_retrato.tamanho, s_retrato.dados);
    mostrar_linha("linha como a CPU ve", w);
    __real_esp_cache_msync((void*)linha, 128, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    for (int i = 0; i < 32; ++i) { w[i] = linha[i]; }
    mostrar_linha("after invalidating (what the PSRAM holds)", w);
    // Restart cleanly instead of panicking (the same restart count as app_main).
    if (s_reinicios[0] == MAGIA && s_reinicios[1] < 3) {
        s_reinicios[1]++;
        esp_restart();
    }
}

// Every PSRAM allocation of 1 MB or more comes through here (--wrap in
// main/CMakeLists.txt): checks the header BEFORE TLSF reads it.
extern "C" void* __real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);

extern "C" void* IRAM_ATTR __wrap_heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if ((caps & MALLOC_CAP_SPIRAM) && n * size >= (1u << 20)) {
        tab5_heap_psram_checar("before calloc", n * size);
    }
    return __real_heap_caps_calloc(n, size, caps);
}

// THE STRESS TEST ('mem estresse' on the console): the boot mechanism without depending on
// the binary's layout. A cache line in PSRAM with 32 known words;
// each round writes word 1 (like the heap's free_bytes on every malloc),
// zeroes 1.8 MB (like the frame calloc, which sweeps the 256 KB of L2), syncs
// (like esp_lcd) and checks the whole line in a single read. Any word
// out of place is the boot defect happening here, measurably.
#include <string.h>
#include "esp_timer.h"

void tab5_heap_psram_estresse(int voltas, bool zerar)
{
    const size_t TAM = 720 * 1280 * 2;
    uint32_t* linha = (uint32_t*)heap_caps_aligned_alloc(128, 128, MALLOC_CAP_SPIRAM);
    uint8_t*  grande = zerar ? (uint8_t*)heap_caps_aligned_alloc(128, TAM, MALLOC_CAP_SPIRAM) : nullptr;
    if (!linha || (zerar && !grande)) {
        printf("no free PSRAM for the test\n");
        heap_caps_free(linha);
        heap_caps_free(grande);
        return;
    }
    for (int k = 0; k < 32; ++k) { linha[k] = 0xA5000000u | (uint32_t)k; }
    const int64_t t0 = esp_timer_get_time();
    int erros = 0, voltas_com_erro = 0, mostradas = 0;
    for (int i = 0; i < voltas; ++i) {
        linha[1] = (uint32_t)i;
        linha[2] = (uint32_t)i ^ 0x5555u;
        if (zerar) {
            memset(grande, i, TAM);
            __real_esp_cache_msync(grande, TAM, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        uint32_t w[32];
        for (int k = 0; k < 32; ++k) { w[k] = ((volatile uint32_t*)linha)[k]; }
        int e = 0;
        for (int k = 0; k < 32; ++k) {
            const uint32_t esperado = k == 1 ? (uint32_t)i : k == 2 ? ((uint32_t)i ^ 0x5555u) : (0xA5000000u | (uint32_t)k);
            if (w[k] != esperado) { ++e; }
        }
        if (e) {
            erros += e;
            ++voltas_com_erro;
            if (mostradas < 4) {
                ++mostradas;
                printf("round %d: %d word(s) out of place (line %p)\n", i, e, linha);
                for (int k = 0; k < 32; k += 8) {
                    printf("  +%02x: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", k * 4,
                           (unsigned long)w[k], (unsigned long)w[k + 1], (unsigned long)w[k + 2], (unsigned long)w[k + 3],
                           (unsigned long)w[k + 4], (unsigned long)w[k + 5], (unsigned long)w[k + 6], (unsigned long)w[k + 7]);
                }
            }
            for (int k = 0; k < 32; ++k) {   // restore for the next round
                linha[k] = k == 1 ? (uint32_t)i : k == 2 ? ((uint32_t)i ^ 0x5555u) : (0xA5000000u | (uint32_t)k);
            }
        }
        if ((i & 63) == 63) { vTaskDelay(1); }   // let the rest of the system breathe
    }
    printf("stress: %d rounds (%s) in %lld ms - %d with errors, %d wrong words\n",
           voltas, zerar ? "zerando 1,8 MB" : "so a linha",
           (long long)((esp_timer_get_time() - t0) / 1000), voltas_com_erro, erros);
    heap_caps_free(linha);
    heap_caps_free(grande);
}
