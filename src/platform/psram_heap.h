// PORT: port-written for angband-pico, stage 020.
//
// The newlib heap, moved to the 8 MB PSRAM on the Pimoroni Pico Plus 2 W.
//
// specifications.md 4 (heap placement): one rule, no per-array decisions. _sbrk returns
// PSRAM, so every malloc/calloc/realloc -- and therefore every mem_alloc/mem_zalloc/
// mem_realloc in z-virt.c -- lands there. SRAM keeps .data, .bss and the stack.
//
// LINK NOTE. _sbrk here is a strong definition that overrides the SDK's __weak one in
// pico_clib_interface/newlib_interface.c. This file lives in a STATIC library, so the
// linker only pulls it in if something references one of its symbols. Any executable that
// wants the PSRAM heap must therefore call psram_heap_stats() (or another function here)
// at least once. Every diagnostic and the game do this at start-up; that is deliberate,
// not incidental. psram_heap_in_psram() is the cheapest way to satisfy it.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uintptr_t base;       // first byte _sbrk can hand out
        uintptr_t limit;      // one past the last byte it can hand out
        uintptr_t brk;        // the current break
        uintptr_t high_water; // the highest break ever reached
        size_t psram_size;    // psram_get_size(), 0 if the PSRAM did not come up
        bool in_psram;        // false if the heap fell back to SRAM
    } psram_heap_stats_t;

    // Fill *out with the current state of the heap. Never fails.
    void psram_heap_stats(psram_heap_stats_t *out);

    // Bytes between base and the current break. This is what newlib has taken from the
    // OS, not what the program has live: free() returns blocks to newlib's free lists,
    // which almost never shrink the break. It is the right number for measuring
    // per-allocation overhead.
    size_t psram_heap_used(void);

    // The high-water mark of the break, in bytes above base.
    size_t psram_heap_high_water(void);

    // Bytes between the current break and the limit.
    size_t psram_heap_free(void);

    // True when the heap really is in PSRAM.
    bool psram_heap_in_psram(void);

#ifdef __cplusplus
}
#endif
