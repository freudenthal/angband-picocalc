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
//   3. The overhead probe: 26,136 x malloc(3) then 26,136 x malloc(24) -- two 66x198
//      chunks' worth of square info bitflags and squares (specifications.md 7.1) -- and
//      what each costs. This turns the 7.1 ASSUMPTION about the device heap into a measured
//      bytes-per-allocation number. It runs FIRST, on the coldest heap available; see the
//      comment on test_overhead().
//   4. 7 MB, taken 64 KB at a time, written with a per-chunk pattern and read back word for
//      word with no mismatch.
//   5. Write and read bandwidth over 1 MB, three ways, for stage 070.
//
// Then it sits in a frame loop. A stuck board shows a frozen counter and a dead one shows
// nothing, which is the point -- and any key on the PicoCalc keyboard re-runs the whole
// sequence, so a capture that began with the terminal closed can simply be taken again.
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
#include "pico/stdio_usb.h"
#include "hardware/psram.h"
#include "hardware/structs/qmi.h"

#include "platform/lcd.h"
#include "platform/font5x10.h"
#include "platform/southbridge.h"
#include "platform/psram_heap.h"

// ---------------------------------------------------------------------------------------
// Output. Every line goes to USB serial in full and to the panel truncated to 64 columns.

#define DIAG_COLS 64
#define DIAG_ROWS 32
#define DIAG_LOG_ROWS (DIAG_ROWS - 2) // 0..29
#define DIAG_HINT_ROW (DIAG_ROWS - 2) // 30
#define DIAG_STATUS_ROW (DIAG_ROWS - 1) // 31, the frame counter

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const uint16_t COL_BG = RGB565(20, 12, 28);
static const uint16_t COL_FG = RGB565(222, 238, 214);
static const uint16_t COL_OK = RGB565(109, 170, 44);
static const uint16_t COL_BAD = RGB565(208, 70, 72);
static const uint16_t COL_NOTE = RGB565(218, 212, 94);

// The serial log is the record the run logs quote, so it must not be clipped to the
// panel's width. 160 is enough for every line here with room to spare.
static char diag_line[160];
static int diag_row;

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

// Print one line. Serial gets all of it; the panel gets the first 64 columns of the last
// 30 lines, and starts again from the top after that.
static void diag_printf_colour(uint16_t fg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag_line, sizeof(diag_line), fmt, ap);
    va_end(ap);

    printf("%s\n", diag_line);

    if (diag_row >= DIAG_LOG_ROWS)
    {
        diag_row = 0;
        for (int r = 0; r < DIAG_LOG_ROWS; r++)
            diag_draw(r, "", COL_FG);
    }
    diag_draw(diag_row++, diag_line, fg);
}

#define diag_printf(...) diag_printf_colour(COL_FG, __VA_ARGS__)

// ---------------------------------------------------------------------------------------
// The overhead probe.
//
// 66x198 = 13,068 squares per cave chunk, and cave_new() builds two of them (cave and
// player->cave). Each square is a 24-byte struct and each carries one 3-byte malloc for its
// info bitflags. 26,136 of each is therefore exactly what one dungeon level costs in the
// two allocation sizes that dominate the game's heap.
//
// MEASURING THIS IS FIDDLIER THAN IT LOOKS. The first device run reported 0.00 B/alloc for
// both sizes, because the probe ran after the 7 MB test and read the _sbrk high-water mark.
// newlib trims the break back on free() -- after the 7 MB test the break was at 4 KB -- but
// the high-water mark is monotonic, so 52,272 small allocations all fitted underneath it and
// the delta was genuinely zero. Three changes fix it:
//
//   1. The probe runs FIRST, before anything has taken 7 MB.
//   2. It resets the high-water mark after allocating its pointer arrays, so the mark and
//      the break agree at the start of the measured phase.
//   3. It reports the modal address stride between consecutive allocations as well. That is
//      independent of the break entirely -- it reads the chunk spacing newlib actually chose
//      -- so it stays true even on a re-run over a used heap, and the two numbers agreeing
//      is what makes either believable.

#define PROBE_N 26136u

// Histogram of consecutive-pointer differences, so the common chunk stride is reported
// rather than an average smeared by the occasional jump to a fresh sbrk region.
#define STRIDE_SLOTS 8

static void probe_size(void **slots, size_t sz, const char *label)
{
    psram_heap_reset_high_water();
    size_t brk0 = psram_heap_used();

    unsigned got = 0;
    for (unsigned i = 0; i < PROBE_N; i++)
    {
        slots[i] = malloc(sz);
        if (!slots[i])
            break;
        got++;
    }

    size_t brk1 = psram_heap_used();
    size_t hw1 = psram_heap_high_water();

    if (got < PROBE_N)
        diag_printf_colour(COL_BAD, "FAIL   %s only %u of %u", label, got, PROBE_N);

    long stride_val[STRIDE_SLOTS];
    unsigned stride_cnt[STRIDE_SLOTS];
    unsigned stride_used = 0;
    unsigned outliers = 0;

    for (unsigned i = 1; i < got; i++)
    {
        long d = (long)((uintptr_t)slots[i] - (uintptr_t)slots[i - 1]);
        unsigned j = 0;
        for (; j < stride_used; j++)
            if (stride_val[j] == d)
            {
                stride_cnt[j]++;
                break;
            }
        if (j == stride_used)
        {
            if (stride_used < STRIDE_SLOTS)
            {
                stride_val[stride_used] = d;
                stride_cnt[stride_used] = 1;
                stride_used++;
            }
            else
            {
                outliers++;
            }
        }
    }

    long modal = 0;
    unsigned modal_cnt = 0;
    for (unsigned j = 0; j < stride_used; j++)
        if (stride_cnt[j] > modal_cnt)
        {
            modal_cnt = stride_cnt[j];
            modal = stride_val[j];
        }

    // Two decimal places without pulling in floating-point printf.
    size_t delta = brk1 - brk0;
    unsigned per100 = got ? (unsigned)((delta * 100u) / got) : 0;

    diag_printf_colour(COL_NOTE,
                       "%s %u x malloc(%u): brk +%u B = %u.%02u B/alloc, stride %ld B x%u",
                       label, got, (unsigned)sz, (unsigned)delta,
                       per100 / 100u, per100 % 100u, modal, modal_cnt);
    diag_printf("%s high water +%u B, %u distinct strides, %u past the histogram",
                label, (unsigned)(hw1 - brk0), stride_used, outliers);
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

    diag_printf("probe  pointer arrays %u KB (outside the measured window), brk %u KB",
                (unsigned)(2u * PROBE_N * sizeof(void *) / 1024u),
                (unsigned)(psram_heap_used() / 1024u));

    size_t brk_before = psram_heap_used();
    probe_size(slots3, 3, "bitflags");
    probe_size(slots24, 24, "squares ");
    size_t brk_after = psram_heap_used();

    diag_printf_colour(COL_NOTE,
                       "probe  one 66x198 level pair costs %u KB in these two sizes",
                       (unsigned)((brk_after - brk_before) / 1024u));

    for (unsigned i = 0; i < PROBE_N; i++)
    {
        free(slots3[i]);
        free(slots24[i]);
    }
    free(slots3);
    free(slots24);

    diag_printf("probe  after free: brk %u KB (newlib trims the top chunk)",
                (unsigned)(psram_heap_used() / 1024u));
}

// ---------------------------------------------------------------------------------------
// The 7 MB write-and-verify.

#define CHUNK_BYTES (64u * 1024u)
#define CHUNK_WORDS (CHUNK_BYTES / 4u)
#define VERIFY_BYTES (7u * 1024u * 1024u)
#define VERIFY_CHUNKS (VERIFY_BYTES / CHUNK_BYTES) // 112

static uint32_t *chunks[VERIFY_CHUNKS];

// The pattern carries the chunk index in the top byte and the word counter below it, so a
// mismatch says whether the fault is a wrong chunk or a wrong offset within one.
static inline uint32_t pattern_at(unsigned chunk, unsigned word)
{
    return ((uint32_t)chunk << 24) | (word & 0x00FFFFFFu);
}

static void test_verify_7mb(void)
{
    unsigned got = 0;

    psram_heap_reset_high_water();

    uint64_t t0 = time_us_64();
    for (unsigned c = 0; c < VERIFY_CHUNKS; c++)
    {
        chunks[c] = (uint32_t *)malloc(CHUNK_BYTES);
        if (!chunks[c])
            break;
        got++;
    }
    uint64_t t_alloc = time_us_64() - t0;

    diag_printf("alloc  %u x 64 KB = %u KB in %u ms, brk %u KB",
                got, (unsigned)(got * CHUNK_BYTES / 1024u),
                (unsigned)(t_alloc / 1000u),
                (unsigned)(psram_heap_used() / 1024u));

    if (got < VERIFY_CHUNKS)
        diag_printf_colour(COL_BAD, "FAIL   only %u of %u chunks, errno %d",
                           got, (unsigned)VERIFY_CHUNKS, errno);

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

    diag_printf("free   brk back to %u KB, high water was %u KB",
                (unsigned)(psram_heap_used() / 1024u),
                (unsigned)(psram_heap_high_water() / 1024u));
}

// ---------------------------------------------------------------------------------------
// Bandwidth.
//
// Three numbers, not two. The first device run measured memset at 9.06 MB/s against a
// word-sum read at 14.16 MB/s, which is the wrong way round for a memory this is supposed to
// be; a write should not be slower than a read. memset's inner loop is newlib's, and whether
// it stores bytes or words is not this port's choice -- so an explicit 32-bit store loop is
// timed beside it. If the word loop is much faster than memset, the gap is newlib's memset
// and not the QMI, and stage 070 should supply its own fill.

#define BW_BYTES (1024u * 1024u)
#define BW_PASSES 4u

static volatile uint32_t bw_sink; // keeps the read loop from being optimised away

static void report_bw(const char *label, uint64_t us)
{
    // MB moved is BW_PASSES, so MB/s * 100 = 100e6 * passes / us.
    unsigned x100 = us ? (unsigned)((100000000ull * BW_PASSES) / us) : 0;
    diag_printf_colour(COL_NOTE, "bandw  %s %u.%02u MB/s over %u MB in %u us",
                       label, x100 / 100u, x100 % 100u, BW_PASSES, (unsigned)us);
}

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
    report_bw("memset  ", time_us_64() - t0);

    volatile uint32_t *w = (volatile uint32_t *)buf;

    t0 = time_us_64();
    for (unsigned p = 0; p < BW_PASSES; p++)
        for (unsigned i = 0; i < BW_BYTES / 4u; i++)
            w[i] = (uint32_t)(i + p);
    report_bw("word st ", time_us_64() - t0);

    uint32_t sum = 0;
    t0 = time_us_64();
    for (unsigned p = 0; p < BW_PASSES; p++)
        for (unsigned i = 0; i < BW_BYTES / 4u; i++)
            sum += w[i];
    report_bw("word sum", time_us_64() - t0);
    bw_sink = sum;

    free(buf);
}

// ---------------------------------------------------------------------------------------

static void run_all(unsigned run)
{
    diag_row = 0;
    for (int r = 0; r < DIAG_LOG_ROWS; r++)
        diag_draw(r, "", COL_FG);

    printf("\n");
    diag_printf_colour(COL_NOTE, "angband_psramdiag -- angband-pico stage 020 -- run %u%s",
                       run, (run == 1) ? " (cold heap)" : " (re-run, heap already used)");

    psram_heap_stats_t st;
    psram_heap_stats(&st);

    diag_printf("psram_get_size() = %u  (expect 8388608)", (unsigned)st.psram_size);
    diag_printf("heap   %08lx..%08lx  %u KB  %s",
                (unsigned long)st.base, (unsigned long)st.limit,
                (unsigned)((st.limit - st.base) / 1024u),
                st.in_psram ? "PSRAM" : "SRAM FALLBACK");

    // Stage 070 wants the timing the SDK settled on for CS1. M[1] is the PSRAM window.
    diag_printf("qmi    M1 timing 0x%08lx rfmt 0x%08lx wfmt 0x%08lx  sys %lu Hz  lcd spi %u Hz",
                (unsigned long)qmi_hw->m[1].timing,
                (unsigned long)qmi_hw->m[1].rfmt,
                (unsigned long)qmi_hw->m[1].wfmt,
                (unsigned long)clock_get_hz(clk_sys),
                (unsigned)lcd_get_baudrate());

    void *first = malloc(16);
    diag_printf_colour((first && ((uintptr_t)first >> 24) == 0x11) ? COL_OK : COL_BAD,
                       "malloc(16) = %p  (expect 0x11xxxxxx)", first);
    free(first);

    if (st.psram_size != 8u * 1024u * 1024u)
        diag_printf_colour(COL_BAD, "FAIL   psram_get_size() is not 8388608");
    if (!st.in_psram)
        diag_printf_colour(COL_BAD, "FAIL   the heap is not in PSRAM");

    // The probe goes first, on the coldest heap this run will ever see. See test_overhead().
    test_overhead();
    test_verify_7mb();
    test_bandwidth();

    diag_printf_colour(COL_OK, "done   brk %u KB, high water %u KB, free %u KB",
                       (unsigned)(psram_heap_used() / 1024u),
                       (unsigned)(psram_heap_high_water() / 1024u),
                       (unsigned)(psram_heap_free() / 1024u));
}

int main(void)
{
    stdio_init_all();

    lcd_init();
    lcd_set_font(&font_5x10);
    lcd_enable_cursor(false);
    lcd_set_background(COL_BG);
    lcd_set_foreground(COL_FG);
    lcd_clear_screen();

    sb_init(); // the keyboard, for the re-run key

    diag_draw(DIAG_HINT_ROW, "waiting up to 10 s for a USB terminal...", COL_NOTE);

    // Wait for a USB CDC host, but never block on it: the diag has to run with no terminal
    // attached. The first device run lost its whole header because the port was not open
    // yet, which is what this is for -- and the re-run key below is the other half.
    for (unsigned i = 0; i < 100 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    sleep_ms(300); // let the host finish opening the stream before the first write

    unsigned run = 1;
    run_all(run);

    diag_draw(DIAG_HINT_ROW, "press any key to re-run the whole sequence", COL_NOTE);
    printf("press any key on the PicoCalc to re-run the whole sequence\n");

    unsigned long frames = 0;
    while (true)
    {
        // High byte is the key state, low byte the code (platform/keyboard.h in
        // tinyrogue-pico documents the encoding). A press, not a release, re-runs.
        uint16_t key = sb_read_keyboard();
        if (((key >> 8) & 0xFF) == 1)
        {
            run++;
            run_all(run);
            diag_draw(DIAG_HINT_ROW, "press any key to re-run the whole sequence", COL_NOTE);
            printf("press any key on the PicoCalc to re-run the whole sequence\n");
            frames = 0;
        }

        snprintf(diag_line, sizeof(diag_line), "run %u   frame %lu   up %lu s",
                 run, frames, (unsigned long)(time_us_64() / 1000000ull));
        diag_draw(DIAG_STATUS_ROW, diag_line, COL_NOTE);

        if ((frames % 40u) == 0u)
            printf("%s\n", diag_line);

        frames++;
        sleep_ms(50); // 20 Hz, so a key press is picked up promptly
    }
}
