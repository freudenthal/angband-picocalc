// PORT: port-written for angband-pico, stage 150 part A. See codediag_loops.h.
//
// EVERY IMPL BELOW IS "static inline __attribute__((always_inline))". That is not a
// performance hint here -- it is the mechanism. A function-scope static (L4's table would
// otherwise be one) or an ordinary out-of-line call would put ONE copy of the shared code
// somewhere the linker chooses, and every "flash/sram/psram" wrapper would then just call
// that one copy -- which is exactly what this stage must NOT measure by accident. With
// always_inline, GCC fully substitutes the impl's body into each of the three wrappers, so
// each wrapper's own placement attribute governs where ITS copy's instructions land. No
// wrapper calls anything (checked with nm and objdump -d as part of this stage's run log),
// so none of the three needs a long-branch veneer and all three are position-correct.

#include "codediag_loops.h"

#define CODEDIAG_FLASH_ATTR __attribute__((noinline))
#define CODEDIAG_SRAM_ATTR __attribute__((noinline, section(".time_critical.codediag")))
#define CODEDIAG_PSRAM_ATTR __attribute__((noinline, section(".psram_initialised.codediag")))

// ---------------------------------------------------------------------------------------
// L1 sum.

static inline __attribute__((always_inline)) uint32_t
codediag_l1_impl(const uint32_t *data, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += data[i];
    return sum;
}

CODEDIAG_FLASH_ATTR uint32_t codediag_l1_sum_flash(const uint32_t *data, size_t n)
{
    return codediag_l1_impl(data, n);
}
CODEDIAG_SRAM_ATTR uint32_t codediag_l1_sum_sram(const uint32_t *data, size_t n)
{
    return codediag_l1_impl(data, n);
}
CODEDIAG_PSRAM_ATTR uint32_t codediag_l1_sum_psram(const uint32_t *data, size_t n)
{
    return codediag_l1_impl(data, n);
}

// ---------------------------------------------------------------------------------------
// L2 store.

static inline __attribute__((always_inline)) uint32_t
codediag_l2_impl(uint32_t *data, size_t n, uint32_t seed)
{
    uint32_t last = seed;
    for (size_t i = 0; i < n; i++)
    {
        last = seed + (uint32_t)i;
        data[i] = last;
    }
    return last;
}

CODEDIAG_FLASH_ATTR uint32_t codediag_l2_store_flash(uint32_t *data, size_t n, uint32_t seed)
{
    return codediag_l2_impl(data, n, seed);
}
CODEDIAG_SRAM_ATTR uint32_t codediag_l2_store_sram(uint32_t *data, size_t n, uint32_t seed)
{
    return codediag_l2_impl(data, n, seed);
}
CODEDIAG_PSRAM_ATTR uint32_t codediag_l2_store_psram(uint32_t *data, size_t n, uint32_t seed)
{
    return codediag_l2_impl(data, n, seed);
}

// ---------------------------------------------------------------------------------------
// L3 walk / L4 table. One shared tokeniser: `table` is NULL for L3, so the lookup and its
// branch are a compile-time-constant-false condition at every L3 call site and always_inline
// lets the compiler fold it away entirely -- L3's inlined copies carry no table code at all.

#define CODEDIAG_FNV_OFFSET 2166136261u
#define CODEDIAG_FNV_PRIME 16777619u

static inline __attribute__((always_inline)) uint32_t
codediag_walk_impl(const char *text, size_t len, uint8_t *out, size_t out_count,
                    const uint8_t *table)
{
    size_t i = 0;
    uint32_t lines = 0;
    size_t out_i = 0;

    while (i < len)
    {
        size_t line_start = i;
        while (i < len && text[i] != '\n')
            i++;
        size_t line_end = i;
        if (i < len)
            i++; // skip the '\n'

        uint32_t hash = CODEDIAG_FNV_OFFSET;
        uint32_t tokens = 0;
        uint32_t table_sum = 0;
        size_t j = line_start;

        while (j < line_end)
        {
            uint32_t tok_hash = CODEDIAG_FNV_OFFSET;
            while (j < line_end && text[j] != ':')
            {
                tok_hash ^= (uint8_t)text[j];
                tok_hash *= CODEDIAG_FNV_PRIME;
                j++;
            }
            if (table)
                table_sum += table[tok_hash & 0xFFu];
            hash ^= tok_hash;
            tokens++;
            if (j < line_end)
                j++; // skip the ':'
        }
        if (table)
            hash ^= table_sum;

        uint8_t *rec = out + (out_i % out_count) * 16u;
        rec[0] = (uint8_t)(lines >> 24);
        rec[1] = (uint8_t)(lines >> 16);
        rec[2] = (uint8_t)(lines >> 8);
        rec[3] = (uint8_t)(lines);
        rec[4] = (uint8_t)(tokens >> 24);
        rec[5] = (uint8_t)(tokens >> 16);
        rec[6] = (uint8_t)(tokens >> 8);
        rec[7] = (uint8_t)(tokens);
        rec[8] = (uint8_t)(hash >> 24);
        rec[9] = (uint8_t)(hash >> 16);
        rec[10] = (uint8_t)(hash >> 8);
        rec[11] = (uint8_t)(hash);
        rec[12] = (uint8_t)(table_sum >> 24);
        rec[13] = (uint8_t)(table_sum >> 16);
        rec[14] = (uint8_t)(table_sum >> 8);
        rec[15] = (uint8_t)(table_sum);

        out_i++;
        lines++;
    }

    return lines;
}

// L3: no table.
static inline __attribute__((always_inline)) uint32_t
codediag_l3_impl(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_walk_impl(text, len, out, out_count, (const uint8_t *)0);
}

CODEDIAG_FLASH_ATTR uint32_t
codediag_l3_walk_flash(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_l3_impl(text, len, out, out_count);
}
CODEDIAG_SRAM_ATTR uint32_t
codediag_l3_walk_sram(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_l3_impl(text, len, out, out_count);
}
CODEDIAG_PSRAM_ATTR uint32_t
codediag_l3_walk_psram(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_l3_impl(text, len, out, out_count);
}

// L4: one 256-entry table per placement, each a separate file-scope object -- NOT a
// function-local static, which would be one instance shared by every inlined copy and would
// defeat the point (codediag_loops.h). Content does not matter, only that it is real
// initialised data that the linker actually has to place and the SDK's psram_load actually
// has to copy; a simple permutation of the index is enough.
#define CODEDIAG_L4_TABLE(x) \
    ((uint8_t)(((x) * 167u + 41u) ^ ((x) >> 3)))
#define CODEDIAG_L4_TABLE_INIT \
    { \
        CODEDIAG_L4_TABLE(0), CODEDIAG_L4_TABLE(1), CODEDIAG_L4_TABLE(2), CODEDIAG_L4_TABLE(3), \
        CODEDIAG_L4_TABLE(4), CODEDIAG_L4_TABLE(5), CODEDIAG_L4_TABLE(6), CODEDIAG_L4_TABLE(7) \
    }

// The table is built at compile time from CODEDIAG_L4_TABLE(i) for i in 0..255 by way of an
// X-macro so the three copies cannot drift from each other by a hand-typed mistake.
#define CODEDIAG_L4_ROW(base) \
    CODEDIAG_L4_TABLE((base) + 0), CODEDIAG_L4_TABLE((base) + 1), CODEDIAG_L4_TABLE((base) + 2), CODEDIAG_L4_TABLE((base) + 3), \
    CODEDIAG_L4_TABLE((base) + 4), CODEDIAG_L4_TABLE((base) + 5), CODEDIAG_L4_TABLE((base) + 6), CODEDIAG_L4_TABLE((base) + 7)
#define CODEDIAG_L4_TABLE_256 \
    { \
        CODEDIAG_L4_ROW(0), CODEDIAG_L4_ROW(8), CODEDIAG_L4_ROW(16), CODEDIAG_L4_ROW(24), \
        CODEDIAG_L4_ROW(32), CODEDIAG_L4_ROW(40), CODEDIAG_L4_ROW(48), CODEDIAG_L4_ROW(56), \
        CODEDIAG_L4_ROW(64), CODEDIAG_L4_ROW(72), CODEDIAG_L4_ROW(80), CODEDIAG_L4_ROW(88), \
        CODEDIAG_L4_ROW(96), CODEDIAG_L4_ROW(104), CODEDIAG_L4_ROW(112), CODEDIAG_L4_ROW(120), \
        CODEDIAG_L4_ROW(128), CODEDIAG_L4_ROW(136), CODEDIAG_L4_ROW(144), CODEDIAG_L4_ROW(152), \
        CODEDIAG_L4_ROW(160), CODEDIAG_L4_ROW(168), CODEDIAG_L4_ROW(176), CODEDIAG_L4_ROW(184), \
        CODEDIAG_L4_ROW(192), CODEDIAG_L4_ROW(200), CODEDIAG_L4_ROW(208), CODEDIAG_L4_ROW(216), \
        CODEDIAG_L4_ROW(224), CODEDIAG_L4_ROW(232), CODEDIAG_L4_ROW(240), CODEDIAG_L4_ROW(248) \
    }

static const uint8_t codediag_l4_tbl_flash[256] = CODEDIAG_L4_TABLE_256;
static const uint8_t __attribute__((section(".time_critical.codediag_l4"))) codediag_l4_tbl_sram[256] =
    CODEDIAG_L4_TABLE_256;
static const uint8_t __attribute__((section(".psram_initialised.codediag_l4"))) codediag_l4_tbl_psram[256] =
    CODEDIAG_L4_TABLE_256;

CODEDIAG_FLASH_ATTR uint32_t
codediag_l4_table_flash(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_walk_impl(text, len, out, out_count, codediag_l4_tbl_flash);
}
CODEDIAG_SRAM_ATTR uint32_t
codediag_l4_table_sram(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_walk_impl(text, len, out, out_count, codediag_l4_tbl_sram);
}
CODEDIAG_PSRAM_ATTR uint32_t
codediag_l4_table_psram(const char *text, size_t len, uint8_t *out, size_t out_count)
{
    return codediag_walk_impl(text, len, out, out_count, codediag_l4_tbl_psram);
}

// ---------------------------------------------------------------------------------------
// Item 3.1: prove the PSRAM copy is there before trusting any of the above.

CODEDIAG_PSRAM_ATTR uint32_t codediag_psram_trivial(void)
{
    return CODEDIAG_PSRAM_TRIVIAL_MAGIC;
}

#define CODEDIAG_CANARY_INIT \
    { \
        0x43, 0x4f, 0x44, 0x45, 0x44, 0x49, 0x41, 0x47, 0x2d, 0x31, 0x35, 0x30, 0x2d, 0x63, 0x61, 0x6e, \
        0x61, 0x72, 0x79, 0x2d, 0x61, 0x2d, 0x62, 0x2d, 0x63, 0x2d, 0x64, 0x2d, 0x65, 0x2d, 0x66, 0x2d, \
        0x67, 0x2d, 0x68, 0x2d, 0x69, 0x2d, 0x6a, 0x2d, 0x6b, 0x2d, 0x6c, 0x2d, 0x6d, 0x2d, 0x6e, 0x2d, \
        0x6f, 0x2d, 0x70, 0x2d, 0x71, 0x2d, 0x72, 0x2d, 0x73, 0x2d, 0x74, 0x2d, 0x75, 0x2d, 0x76, 0x00 \
    }

const uint8_t codediag_flash_canary[CODEDIAG_CANARY_BYTES] = CODEDIAG_CANARY_INIT;
const uint8_t __attribute__((section(".psram_initialised.codediag_canary")))
codediag_psram_canary[CODEDIAG_CANARY_BYTES] = CODEDIAG_CANARY_INIT;
