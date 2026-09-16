// PORT: port-written for angband-pico, stage 150 part A.
//
// Four leaf loops, each compiled into three placements: flash .text (the default),
// SRAM (.time_critical, the same section pico_platform's __not_in_flash_func uses), and
// PSRAM (.psram_initialised, the section the SDK's own sections_psram.incl collects and
// copies from flash before main() -- specifications.md 6.2). Every loop touches no globals,
// calls nothing, and is a leaf, so it carries no long-branch veneer and is position-correct
// in any of the three copies (specifications.md's "Background" section for this stage).
//
// codediag_loops.c defines each one exactly once, as a small always_inline "impl" plus three
// thin wrappers whose only difference is a placement attribute -- so there is one body to
// get right, not three, and the compiler (not a human) keeps the three copies identical.
//
// L4's 256-entry lookup table exists three times too, one per placement, each a separate
// object at file scope in codediag_loops.c: a function-local static would be ONE instance
// shared by every inlined copy, which would defeat the whole point of the placement test.

#ifndef INCLUDED_CODEDIAG_LOOPS_H
#define INCLUDED_CODEDIAG_LOOPS_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------------------
// L1: sum `n` uint32_t words out of `data`.
uint32_t codediag_l1_sum_flash(const uint32_t *data, size_t n);
uint32_t codediag_l1_sum_sram(const uint32_t *data, size_t n);
uint32_t codediag_l1_sum_psram(const uint32_t *data, size_t n);

// L2: store `n` uint32_t words into `data` (data[i] = seed + i). Returns the last value
// written, so a caller can sanity-print it; the stores themselves are through a pointer
// argument and cannot be dropped as dead code.
uint32_t codediag_l2_store_flash(uint32_t *data, size_t n, uint32_t seed);
uint32_t codediag_l2_store_sram(uint32_t *data, size_t n, uint32_t seed);
uint32_t codediag_l2_store_psram(uint32_t *data, size_t n, uint32_t seed);

// L3: walk `len` bytes of `text` as '\n'-separated lines, split each line on ':' into
// tokens, FNV-1a hash every token, and write one 16-byte record into
// out[(line_index % out_count) * 16 .. +16) per line. Returns the number of lines walked.
uint32_t codediag_l3_walk_flash(const char *text, size_t len, uint8_t *out, size_t out_count);
uint32_t codediag_l3_walk_sram(const char *text, size_t len, uint8_t *out, size_t out_count);
uint32_t codediag_l3_walk_psram(const char *text, size_t len, uint8_t *out, size_t out_count);

// L4: the same walk, plus a 256-entry table lookup per token (indexed by the token's own
// FNV-1a hash), folded into the record so the read cannot be optimised away. Each placement
// reads its own copy of the table -- see codediag_loops.c.
uint32_t codediag_l4_table_flash(const char *text, size_t len, uint8_t *out, size_t out_count);
uint32_t codediag_l4_table_sram(const char *text, size_t len, uint8_t *out, size_t out_count);
uint32_t codediag_l4_table_psram(const char *text, size_t len, uint8_t *out, size_t out_count);

// ---------------------------------------------------------------------------------------
// Item 3.1's two PSRAM-is-really-there proofs.

// A trivial function placed in PSRAM (.psram_initialised), called once at startup. Returns
// a fixed constant so codediag.c can print "expected X, got Y" rather than merely "it did
// not crash".
#define CODEDIAG_PSRAM_TRIVIAL_MAGIC 0x50524931u /* "PR1" packed into 4 bytes */
uint32_t codediag_psram_trivial(void);

// A small canary: identical byte patterns declared once in flash-resident initialised
// .rodata (codediag_flash_canary) and once in PSRAM-resident initialised data
// (codediag_psram_canary, .psram_initialised). If the SDK's psram_load copy
// (specifications.md 6.2) ran before main(), memcmp of the two is zero.
#define CODEDIAG_CANARY_BYTES 64
extern const uint8_t codediag_flash_canary[CODEDIAG_CANARY_BYTES];
extern const uint8_t codediag_psram_canary[CODEDIAG_CANARY_BYTES];

#endif // INCLUDED_CODEDIAG_LOOPS_H
