// PORT: port-written for angband-pico, stage 020.
//
// angband_psramdiag -- the PSRAM bring-up app. It links the platform layer only: no game
// code at all. If a malloc misbehaves here, the fault is in src/platform/psram_heap.c or
// in the SDK, and nowhere else (the tinyrogue_diag / mothpad_diag pattern).
//
// Everything drawn on the panel is also written to USB serial at 115200. A build that
// enables stdio but never calls printf shows an empty terminal forever -- see
// ../mothpad-pico1/specifications.md 11, which cost that project a step.
//
// What it proves, in order:
//
//   1. psram_get_size() is 8388608 and the heap window really is 0x11000000-0x11800000.
//   2. malloc(16) returns an address in PSRAM.
//   3. 7 MB, taken 64 KB at a time, can be written with a per-chunk pattern and read back
//      word for word with no mismatch.
//   4. The overhead probe: 26,136 x malloc(3) then 26,136 x malloc(24) -- two 66x198
//      chunks' worth of square info bitflags and squares (specifications.md 7.1) -- and
//      the _sbrk high-water delta each costs. This turns the 7.1 ASSUMPTION about the
//      device heap into a measured bytes-per-allocation number.
//   5. Write and read bandwidth over 1 MB, for stage 070.
//
// Then it sits in a frame loop, so that a hang is distinguishable from a blank screen.
//
// STACK NOTE. The core-0 stack is the 2 KB the SDK puts in SCRATCH_Y. Every buffer here
// is static or heap; nothing large is automatic.

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "hardware/structs/qmi.h"

#include "platform/lcd.h"
#include "platform/font5x10.h"
#include "platform/psram_heap.h"

// ---------------------------------------------------------------------------------------
// Output. Every line goes to USB serial and to the panel.

#define DIAG_COLS 64
#define DIAG_ROWS 32
#define DIAG_STATUS_ROW (DIAG_ROWS - 1) // reserved for the frame counter

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const uint16_t COL_BG = RGB565(20, 12, 28);
static const uint16_t COL_FG = RGB565(222, 238, 214);
static const uint16_t COL_OK = RGB565(109, 170, 44);
static const uint16_t COL_BAD = RGB565(208, 70, 72);
static const uint16_t COL_NOTE = RGB565(218, 212, 94);

static int diag_row;
static char diag_line[DIAG_COLS + 1];

// Draw one row, space-padded to the full width so the previous line is erased. The walk
// stops at the terminator rather than indexing past it: diag_draw is also called with
// short literals.
static void diag_draw(int row, const char *s, uint16_t fg)
{
    lcd_set_foreground(fg);
    lcd_set_background(COL_BG);

    int x = 0;
    for (; x < DIAG_COLS && s[x]; x++)
        lcd_putc((uint8_t)x, (uint8_t)row, (uint8_t)s[x]);
    for (; x < DIAG_COLS; x++)
        lcd_putc((uint8_t)x, (uint8_t)row, (uint8_t)' ');
}

// Print one line. The panel holds DIAG_STATUS_ROW lines and then starts again from the
// top; the serial log holds everything, and it is the serial log the run log quotes.
static void diag_printf_colour(uint16_t fg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag_line, sizeof(diag_line), fmt, ap);
    va_end(ap);

    printf("%s\n", diag_line);

    if (diag_row >= DIAG_STATUS_ROW)
    {
        diag_row = 0;
        for (int r = 0; r < DIAG_STATUS_ROW; r++)
            diag_draw(r, "", COL_FG);
    }
    diag_draw(diag_row++, diag_line, fg);
}

#define diag_printf(...) diag_printf_colour(COL_FG, __VA_ARGS__)

// ---------------------------------------------------------------------------------------
// 1-3. Size, first malloc, and the 7 MB write-and-verify.

#define CHUNK_BYTES (64u * 1024u)
#define CHUNK_WORDS (CHUNK_BYTES / 4u)
#define VERIFY_BYTES (7u * 1024u * 1024u)
#define VERIFY_CHUNKS (VERIFY_BYTES / CHUNK_BYTES) // 112

static uint32_t *chunks[VERIFY_CHUNKS];

// The pattern carries the chunk index in the top byte and the word counter below it, so
// a mismatch says whether the fault is a wrong chunk or a wrong offset within one.
static inline uint32_t pattern_at(unsigned chunk, unsigned word)
{
    return ((uint32_t)chunk << 24) | (word & 0x00FFFFFFu);
}

static void test_verify_7mb(void)
{
    unsigned got = 0;

    uint64_t t0 = time_us_64();
    for (unsigned c = 0; c < VERIFY_CHUNKS; c++)
    {
        chunks[c] = (uint32_t *)malloc(CHUNK_BYTES);
        if (!chunks[c])
            break;
        got++;
    }
    uint64_t t_alloc = time_us_64() - t0;

    diag_printf("alloc  %u x 64 KB = %u KB in %u ms, high water %u KB",
                got, (unsigned)(got * CHUNK_BYTES / 1024u),
                (unsigned)(t_alloc / 1000u),
                (unsigned)(psram_heap_high_water() / 1024u));

    if (got < VERIFY_CHUNKS)
    {
        diag_printf_colour(COL_BAD, "FAIL   only %u of %u chunks, errno %d",
                           got, (unsigned)VERIFY_CHUNKS, errno);
    }

    t0 = time_us_64();
    for (unsigned c = 0; c < got; c++)
        for (unsigned w = 0; w < CHUNK_WORDS; w++)
            chunks[c][w] = pattern_at(c, w);
    uint64_t t_write = time_us_64() - t0;

    uintptr_t bad_addr = 0;
    uint32_t bad_want = 0, bad_have = 0;

    t0 = time_us_64();
    for (unsigned c = 0; c < got && !bad_addr; c++)
    {
        for (unsigned w = 0; w < CHUNK_WORDS; w++)
        {
            uint32_t want = pattern_at(c, w);
            uint32_t have = chunks[c][w];
            if (have != want)
            {
                bad_addr = (uintptr_t)&chunks[c][w];
                bad_want = want;
                bad_have = have;
                break;
            }
        }
    }
    uint64_t t_read = time_us_64() - t0;

    unsigned kb = (unsigned)(got * CHUNK_BYTES / 1024u);
    diag_printf("write  %u KB in %u ms", kb, (unsigned)(t_write / 1000u));
    diag_printf("read   %u KB in %u ms", kb, (unsigned)(t_read / 1000u));

    if (bad_addr)
        diag_printf_colour(COL_BAD, "verify FAIL at 0x%08lx want 0x%08lx got 0x%08lx",
                           (unsigned long)bad_addr, (unsigned long)bad_want,
                           (unsigned long)bad_have);
    else
        diag_printf_colour(COL_OK, "verify OK   %u KB, no mismatch", kb);

    for (unsigned c = 0; c < got; c++)
        free(chunks[c]);

    diag_printf("free   back to brk %u KB, high water %u KB",
                (unsigned)(psram_heap_used() / 1024u),
                (unsigned)(psram_heap_high_water() / 1024u));
}

// ---------------------------------------------------------------------------------------
// 4. The overhead probe.
//
// 66x198 = 13,068 squares per cave chunk, and cave_new() builds two of them (cave and
// player->cave). Each square is a 24-byte struct and each carries one 3-byte malloc for
// its info bitflags. 26,136 of each is therefore exactly what one dungeon level costs in
// the two allocation sizes that dominate the game's heap.
//
// The pointer arrays are allocated first and their cost is outside the measured window:
// each delta is a high-water difference taken across the small allocations alone.

#define PROBE_N 26136u

static void probe_size(void **slots, size_t sz, const char *label)
{
    size_t hw0 = psram_heap_high_water();

    unsigned got = 0;
    for (unsigned i = 0; i < PROBE_N; i++)
    {
        slots[i] = malloc(sz);
        if (!slots[i])
            break;
        got++;
    }

    size_t hw1 = psram_heap_high_water();
    size_t delta = hw1 - hw0;

    if (got < PROBE_N)
        diag_printf_colour(COL_BAD, "FAIL   %s only %u of %u", label, got, PROBE_N);

    // Two decimal places without pulling in floating-point printf.
    unsigned per100 = got ? (unsigned)((delta * 100u) / got) : 0;
    diag_printf_colour(COL_NOTE, "%s  %u x malloc(%u): +%u B, %u.%02u B/alloc",
                       label, got, (unsigned)sz, (unsigned)delta,
                       per100 / 100u, per100 % 100u);
}

static void test_overhead(void)
{
    void **slots3 = (void **)malloc(PROBE_N * sizeof(void *));
    void **slots24 = (void **)malloc(PROBE_N * sizeof(void *));

    if (!slots3 || !slots24)
    {
        diag_printf_colour(COL_BAD, "FAIL   no room for the probe's pointer arrays");
        free(slots3);
        free(slots24);
        return;
    }

    size_t hw_before = psram_heap_high_water();
    diag_printf("probe  arrays %u KB, high water %u KB before",
                (unsigned)(2u * PROBE_N * sizeof(void *) / 1024u),
                (unsigned)(hw_before / 1024u));

    probe_size(slots3, 3, "bitflags");
    probe_size(slots24, 24, "squares ");

    size_t hw_after = psram_heap_high_water();
    diag_printf("probe  one 66x198 level pair costs %u KB in these two sizes",
                (unsigned)((hw_after - hw_before) / 1024u));

    for (unsigned i = 0; i < PROBE_N; i++)
    {
        free(slots3[i]);
        free(slots24[i]);
    }
    free(slots3);
    free(slots24);

    diag_printf("probe  after free: brk %u KB, high water %u KB",
                (unsigned)(psram_heap_used() / 1024u),
                (unsigned)(psram_heap_high_water() / 1024u));
}

// ---------------------------------------------------------------------------------------
// 5. Bandwidth.

#define BW_BYTES (1024u * 1024u)
#define BW_PASSES 4u

static volatile uint32_t bw_sink; // keeps the read loop from being optimised away

static void test_bandwidth(void)
{
    uint8_t *buf = (uint8_t *)malloc(BW_BYTES);
    if (!buf)
    {
        diag_printf_colour(COL_BAD, "FAIL   no room for the 1 MB bandwidth buffer");
        return;
    }

    uint64_t t0 = time_us_64();
    for (unsigned p = 0; p < BW_PASSES; p++)
        memset(buf, (int)(p & 0xFF), BW_BYTES);
    uint64_t t_w = time_us_64() - t0;

    volatile uint32_t *w = (volatile uint32_t *)buf;
    uint32_t sum = 0;
    t0 = time_us_64();
    for (unsigned p = 0; p < BW_PASSES; p++)
        for (unsigned i = 0; i < BW_BYTES / 4u; i++)
            sum += w[i];
    uint64_t t_r = time_us_64() - t0;
    bw_sink = sum;

    // MB/s to two decimals: bytes moved is BW_PASSES MB, so MB/s * 100 = 100e6 * passes / us.
    unsigned wr100 = t_w ? (unsigned)((100000000ull * BW_PASSES) / t_w) : 0;
    unsigned rd100 = t_r ? (unsigned)((100000000ull * BW_PASSES) / t_r) : 0;

    diag_printf_colour(COL_NOTE, "bandw  memset %u.%02u MB/s over %u MB in %u us",
                       wr100 / 100u, wr100 % 100u, BW_PASSES, (unsigned)t_w);
    diag_printf_colour(COL_NOTE, "bandw  wordsum %u.%02u MB/s over %u MB in %u us",
                       rd100 / 100u, rd100 % 100u, BW_PASSES, (unsigned)t_r);

    free(buf);
}

// ---------------------------------------------------------------------------------------

int main(void)
{
    stdio_init_all();

    lcd_init();
    lcd_set_font(&font_5x10);
    lcd_enable_cursor(false);
    lcd_set_background(COL_BG);
    lcd_set_foreground(COL_FG);
    lcd_clear_screen();

    // Give a USB CDC host a moment to enumerate, but never block on it: the diag app has
    // to run with no terminal attached.
    sleep_ms(1500);

    printf("\n");

    diag_printf_colour(COL_NOTE, "angband_psramdiag -- angband-pico stage 020");

    psram_heap_stats_t st;
    psram_heap_stats(&st);

    diag_printf("psram_get_size() = %u  (expect 8388608)", (unsigned)st.psram_size);
    diag_printf("heap   %08lx..%08lx  %u KB  %s",
                (unsigned long)st.base, (unsigned long)st.limit,
                (unsigned)((st.limit - st.base) / 1024u),
                st.in_psram ? "PSRAM" : "SRAM FALLBACK");

    // Stage 070 wants the timing the SDK settled on for CS1. M[1] is the PSRAM window.
    diag_printf("qmi    M1 timing 0x%08lx  rfmt 0x%08lx  lcd spi %u Hz",
                (unsigned long)qmi_hw->m[1].timing,
                (unsigned long)qmi_hw->m[1].rfmt,
                (unsigned)lcd_get_baudrate());

    void *first = malloc(16);
    diag_printf_colour((first && ((uintptr_t)first >> 24) == 0x11) ? COL_OK : COL_BAD,
                       "malloc(16) = %p  (expect 0x11xxxxxx)", first);
    free(first);

    if (st.psram_size != 8u * 1024u * 1024u)
        diag_printf_colour(COL_BAD, "FAIL   psram_get_size() is not 8388608");
    if (!st.in_psram)
        diag_printf_colour(COL_BAD, "FAIL   the heap is not in PSRAM");

    test_verify_7mb();
    test_overhead();
    test_bandwidth();

    diag_printf_colour(COL_OK, "done   brk %u KB, high water %u KB, free %u KB",
                       (unsigned)(psram_heap_used() / 1024u),
                       (unsigned)(psram_heap_high_water() / 1024u),
                       (unsigned)(psram_heap_free() / 1024u));

    // The frame loop. A stuck board shows a frozen counter; a dead one shows nothing.
    unsigned long frames = 0;
    while (true)
    {
        snprintf(diag_line, sizeof(diag_line), "frame %lu   up %lu s",
                 frames, (unsigned long)(time_us_64() / 1000000ull));
        diag_draw(DIAG_STATUS_ROW, diag_line, COL_NOTE);

        if ((frames % 10u) == 0u)
            printf("%s\n", diag_line);

        frames++;
        sleep_ms(500);
    }
}
