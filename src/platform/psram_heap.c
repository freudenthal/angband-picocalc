// PORT: port-written for angband-pico, stage 020. See psram_heap.h for the link note.
//
// _sbrk over the PSRAM.
//
// The SDK brings the chip up before main() in runtime_init_setup_psram: CS on GP47 and
// 8 MB, both from the pimoroni_pico_plus2_w_rp2350 board header (PICO_PSRAM_CS_PIN 47,
// PICO_PSRAM_SIZE_BYTES 8 * 1024 * 1024). Nothing here initialises anything.
//
// The window this hands out is [&__psram_end__, &__psram_start__ + psram_get_size()).
// __psram_start__ is ORIGIN(PSRAM) = 0x11000000 and __psram_end__ is the end of any
// __in_psram / __uninitialized_psram data, both from the SDK's sections_psram.incl. This
// is exactly what micropython/ports/rp2/main.c does for its GC heap, and it is why the
// heap start is __psram_end__ and not __psram_start__: a later stage that puts a buffer
// in PSRAM with __in_psram must not have malloc hand the same bytes out again.
//
// The linker symbols are unconditional in SDK 2.3.0's default memmap -- memmap_default.incl
// includes memory_psram.incl and sections_psram.incl for every rp2350 link, whether or not
// the program uses PSRAM -- so the plan's "if the symbols are absent" fallback to a literal
// 0x11000000 is not needed and is not compiled in. What IS handled is the chip failing to
// come up at run time: psram_get_size() then returns 0 and the heap falls back to the SRAM
// window the SDK's own _sbrk uses, so a board without PSRAM still boots and the diag can
// say so instead of hard-faulting on the first malloc.

#include <errno.h>

#include "pico/stdlib.h"
#include "hardware/psram.h"

#include "psram_heap.h"

// From the SDK linker script (pico_standard_link/script_include/sections_psram.incl).
extern uint8_t __psram_start__, __psram_end__;

// The SRAM fallback window, the same pair the SDK's __weak _sbrk uses.
extern char end;          // end of .bss
extern char __StackLimit; // bottom of the stack guard

static uintptr_t heap_base;
static uintptr_t heap_limit;
static uintptr_t heap_brk;
static uintptr_t heap_high_water;
static bool heap_in_psram;

// Called from _sbrk on the first allocation, and from every stats call so that a program
// that only asks for numbers still gets true ones.
static void heap_lazy_init(void)
{
    if (heap_base)
        return;

    size_t psram_size = psram_get_size(); // 0 if the PSRAM did not come up

    if (psram_size)
    {
        heap_base = (uintptr_t)&__psram_end__;
        heap_limit = (uintptr_t)&__psram_start__ + psram_size;
        heap_in_psram = true;
    }
    else
    {
        heap_base = (uintptr_t)&end;
        heap_limit = (uintptr_t)&__StackLimit;
        heap_in_psram = false;
    }

    heap_brk = heap_base;
    heap_high_water = heap_base;
}

// The strong override. Signature matches the SDK's __weak void *_sbrk(int incr).
//
// incr can be negative: newlib's malloc trims the top of the heap back on free() when the
// top chunk grows past M_TRIM_THRESHOLD. Walking the break backwards is fine; the
// high-water mark is what the overhead probe reads and it never comes down.
void *_sbrk(int incr)
{
    heap_lazy_init();

    uintptr_t prev = heap_brk;

    if (incr < 0)
    {
        uintptr_t dec = (uintptr_t)(-(int64_t)incr);
        if (dec > heap_brk - heap_base)
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        heap_brk -= dec;
        return (void *)prev;
    }

    uintptr_t inc = (uintptr_t)incr;
    if (inc > heap_limit - heap_brk)
    {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_brk += inc;
    if (heap_brk > heap_high_water)
        heap_high_water = heap_brk;

    return (void *)prev;
}

void psram_heap_stats(psram_heap_stats_t *out)
{
    heap_lazy_init();

    if (!out)
        return;

    out->base = heap_base;
    out->limit = heap_limit;
    out->brk = heap_brk;
    out->high_water = heap_high_water;
    out->psram_size = psram_get_size();
    out->in_psram = heap_in_psram;
}

size_t psram_heap_used(void)
{
    heap_lazy_init();
    return (size_t)(heap_brk - heap_base);
}

size_t psram_heap_high_water(void)
{
    heap_lazy_init();
    return (size_t)(heap_high_water - heap_base);
}

void psram_heap_reset_high_water(void)
{
    heap_lazy_init();
    heap_high_water = heap_brk;
}

size_t psram_heap_free(void)
{
    heap_lazy_init();
    return (size_t)(heap_limit - heap_brk);
}

bool psram_heap_in_psram(void)
{
    heap_lazy_init();
    return heap_in_psram;
}
