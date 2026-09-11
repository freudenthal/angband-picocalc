// PORT: port-written for angband-pico, stage 040.
//
// angband_termdiag -- the terminal front end.
//
// It links main-pico.c, utf8.c and six units of game code: ui-term.c, z-util.c, z-virt.c,
// z-form.c, z-color.c and buildid.c. Nothing here draws a pixel or reads a key on its own:
// every character on the panel arrives through Term_putstr()/Term_fresh() and every key
// through Term_inkey(), so what passes here is the code path the game takes and not a
// parallel re-implementation. When this is green, stage 050 only has to link the rest.
//
// specifications.md 4 (font and terminal size, text encoding), 6.4, 6.5, 8.
//
// Everything drawn on the panel is also written to USB serial at 115200, in a buffer wider
// than the panel so the serial log -- the record the run logs quote -- is never clipped.
// The whole sequence waits up to 10 s for a USB terminal and re-runs on F1; both are stage
// 020 lessons (specifications.md 6.5). F1 rather than "any key" because on this diag every
// other key is under test.
//
// Stage 045 added the raw transfer probe, the two colour-band exhibits and the glyph
// expansion probe that run before the Term_fresh timings; see the section headed "The raw
// transfer probe". They bypass the term and drive the PL022 directly, and they put the
// driver's format and clock back before the term draws anything.
//
// STACK NOTE. The core-0 stack is the 2 KB the SDK puts in SCRATCH_Y. Every buffer here is
// static; nothing large is automatic.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include <stdlib.h>

#include "hardware/dma.h"
#include "hardware/spi.h"

#include "platform/keyboard.h"
#include "platform/lcd.h"
#include "platform/main-pico.h"
#include "platform/psram_heap.h"
#include "platform/syscalls.h"
#include "platform/utf8.h"

#include "buildid.h"
#include "ui-event.h"
#include "ui-term.h"
#include "z-color.h"
#include "z-util.h"

// ---------------------------------------------------------------------------------------
// The screen map. 64 x 32; every row is spoken for.

#define TD_COLS PICO_TERM_COLS
#define TD_ROWS PICO_TERM_ROWS

#define ROW_RULER_TENS 0
#define ROW_RULER_UNITS 1
#define ROW_TITLE 2
#define ROW_COLOUR_HDR 3
#define ROW_SWATCH 4 /* 4 rows, 8 colours each */
#define ROW_ASCII_HDR 8
#define ROW_ASCII_LO 9
#define ROW_ASCII_HI 10
#define ROW_UTF8 11
#define ROW_UTF8_VERDICT 12
#define ROW_TIMING 13
#define ROW_KEY_HDR 14
#define ROW_KEY_LOG 15 /* 15 rows */
#define KEY_LOG_ROWS 15
#define ROW_PROGRESS 30
#define ROW_STATUS 31

#define CORNER_GLYPH '#'

// A serial line is never clipped to the panel width; see specifications.md 6.5.
static char line[160];

// ---------------------------------------------------------------------------------------
// Output.

static void ser(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
}

// Draw a string at (x, y), clipped to the panel, and echo the whole of it to serial.
static void put(int x, int y, int attr, const char *s)
{
    int room = TD_COLS - x;
    if (room > 0)
    {
        int n = (int)strlen(s);
        if (n > room)
            n = room;
        Term_putstr(x, y, n, attr, s);
    }
    printf("%s\n", s);
}

// Draw only; for the parts of a row that are built piece by piece.
static void putq(int x, int y, int attr, const char *s)
{
    int room = TD_COLS - x;
    if (room <= 0)
        return;
    int n = (int)strlen(s);
    if (n > room)
        n = room;
    Term_putstr(x, y, n, attr, s);
}

static void putf(int x, int y, int attr, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    put(x, y, attr, line);
}

// ---------------------------------------------------------------------------------------
// z-util.c's hooks. Without them quit() calls exit(), which on a board is a silent halt with
// a stale panel -- indistinguishable from a hang (stage 030 correction).

static void diag_plog_hook(const char *str)
{
    printf("plog  %s\n", str ? str : "(null)");
    if (Term)
    {
        putq(0, ROW_STATUS, COLOUR_L_RED, "plog: ");
        putq(6, ROW_STATUS, COLOUR_L_RED, str ? str : "(null)");
        Term_fresh();
    }
}

static void diag_quit_hook(const char *str)
{
    printf("quit  %s\n", str ? str : "(null)");
    if (Term)
    {
        putq(0, ROW_PROGRESS, COLOUR_L_RED, "core called quit() -- halted, press RESET     ");
        putq(0, ROW_STATUS, COLOUR_L_RED, str ? str : "(null)");
        Term_fresh();
    }
    for (;;)
        sleep_ms(1000);
}

// ---------------------------------------------------------------------------------------
// The colour swatches.
//
// z-color.c initialises 29 of MAX_COLORS entries; 29..31 are zero, which is black, and the
// index below is drawn in white so those three are still identifiable. Eight to a row,
// eight columns each: a two-digit index in white, then a three-letter name and three block
// characters in the colour itself.

static const char *const colour_abbrev[MAX_COLORS] = {
    "DRK", "WHT", "SLT", "ORG", "RED", "GRN", "BLU", "UMB",
    "LDK", "LWH", "LPU", "YEL", "LRD", "LGN", "LBL", "LUM",
    "PUR", "VIO", "TEA", "MUD", "LYE", "MAG", "LTE", "LVI",
    "LPK", "MUS", "BSL", "DLB", "SHD", "u29", "u30", "u31"
};

// ---------------------------------------------------------------------------------------
// The key checklist. Every key the stage plan's item 6 asks for, so the device run is a
// matter of clearing a list rather than remembering one.

typedef struct
{
    uint32_t code;  // 0 means "the Ctrl row": satisfied by any code in 0x01..0x1A
    char label[6];
    bool seen;
} keyreq;

#define MAX_REQ 96
static keyreq req[MAX_REQ];
static int n_req;

static void req_add(uint32_t code, const char *label)
{
    if (n_req >= MAX_REQ)
        return;
    req[n_req].code = code;
    req[n_req].seen = false;
    snprintf(req[n_req].label, sizeof(req[n_req].label), "%s", label);
    n_req++;
}

// Every Angband symbol the stage plan names, plus the punctuation the game reads from
// inscriptions. 31 of them.
static const char *const req_symbols = "<>,.?/*'\";:()[]{}-_+=!@#$%^&~|\\";

static void req_build(void)
{
    char l[2] = {0, 0};

    n_req = 0;

    for (char c = 'a'; c <= 'z'; c++)
    {
        l[0] = c;
        req_add((uint32_t)c, l);
    }
    for (char c = '0'; c <= '9'; c++)
    {
        l[0] = c;
        req_add((uint32_t)c, l);
    }
    for (const char *p = req_symbols; *p; p++)
    {
        l[0] = *p;
        req_add((uint32_t)(unsigned char)*p, l);
    }

    req_add(ARROW_UP, "UP");
    req_add(ARROW_DOWN, "DOWN");
    req_add(ARROW_LEFT, "LEFT");
    req_add(ARROW_RIGHT, "RGHT");
    req_add(ESCAPE, "ESC");
    req_add(KC_ENTER, "RET");
    req_add(KC_TAB, "TAB");
    req_add(KC_BACKSPACE, "BSP");
    req_add(KC_DELETE, "DEL");
    req_add(0, "^A-Z");
}

static void req_mark(uint32_t code)
{
    for (int i = 0; i < n_req; i++)
    {
        if (req[i].code == code && code != 0)
            req[i].seen = true;
        if (req[i].code == 0 && code >= 0x01u && code <= 0x1Au)
            req[i].seen = true;
    }
}

static int req_seen_count(void)
{
    int n = 0;
    for (int i = 0; i < n_req; i++)
        if (req[i].seen)
            n++;
    return n;
}

// ---------------------------------------------------------------------------------------
// The key log: the last KEY_LOG_ROWS events, newest at the bottom.

static char key_log[KEY_LOG_ROWS][80];
static int key_log_used;

static void key_log_push(const char *s)
{
    if (key_log_used < KEY_LOG_ROWS)
    {
        snprintf(key_log[key_log_used++], sizeof(key_log[0]), "%s", s);
    }
    else
    {
        for (int i = 1; i < KEY_LOG_ROWS; i++)
            memcpy(key_log[i - 1], key_log[i], sizeof(key_log[0]));
        snprintf(key_log[KEY_LOG_ROWS - 1], sizeof(key_log[0]), "%s", s);
    }
}

static void key_log_draw(void)
{
    static char pad[TD_COLS + 1];
    memset(pad, ' ', TD_COLS);
    pad[TD_COLS] = '\0';

    for (int i = 0; i < KEY_LOG_ROWS; i++)
    {
        putq(0, ROW_KEY_LOG + i, COLOUR_WHITE, pad);
        if (i < key_log_used)
            putq(0, ROW_KEY_LOG + i, COLOUR_L_GREEN, key_log[i]);
    }
}

// ---------------------------------------------------------------------------------------
// The raw transfer probe (stage 045, item 1).
//
// Bypasses the term entirely. One 320x10 span -- 3,200 pixels, 6,400 bytes, the size of
// main-pico.c's span buffer -- is pushed down the wire 32 times to cover the panel, with
// time_us_64() around the transfer alone and five screens per variant. Variant A is
// lcd_blit() exactly as shipped, which is the "before" number this stage has to move. The
// others try each candidate transfer path from the stage plan WITHOUT editing lcd.c: the
// window is set through lcd_set_window(), then the PL022 is driven directly, and the 8-bit
// mode-0 format and the boot clock are put back before returning. Everything runs under
// lcd_acquire(), and the driver's cursor timer is disabled in any case.
//
// After the timings come two exhibits. The panel is painted with eight colour bands through
// 16-bit frames and held for a few seconds, once at the boot clock and once at the faster
// one, with a serial line saying what to expect. Wrong-endian pixels turn red into blue and
// green into magenta; a clock the ribbon cannot carry shows as speckle. Both are visible from
// across the room and neither needs the term to be redrawn first. The swatches the term
// draws afterwards still go through the shipped path, so they stay the reference.
//
// Then the glyph expansion loop, CPU only: 2,048 cells expanded into the span buffer the way
// text_hook does it today (a branch per pixel) and the way stage plan item 6 proposes (a
// 32-entry table indexed by the 5-bit glyph row, rebuilt once per span). No SPI involved.

#define PROBE_SPAN_PX (WIDTH * PICO_CELL_H)       // 3,200 pixels: one 320x10 blit
#define PROBE_BLITS (HEIGHT / PICO_CELL_H)        // 32 blits cover the panel
#define PROBE_SCREEN_BYTES (WIDTH * HEIGHT * 2u)  // 204,800
#define PROBE_REPS 5
#define PROBE_FAST_HZ 37500000u                   // the one step up from 25 MHz (stage plan item 7)
#define PROBE_HOLD_MS 5000

typedef enum
{
    PROBE_BLIT,        // A: lcd_blit() as built. Stage 040: 8-bit frames, byte repack, 64 B
                       //    chunks. Stage 045 onward: 16-bit frames by DMA in mode 3.
    PROBE_8BIT_WHOLE,  // B: 8-bit frames, the whole pre-swapped 6,400 B span in one write
    PROBE_16BIT,       // C: 16-bit frames, spi_write16_blocking() on the span as it sits
    PROBE_16BIT_DMA,   // D: 16-bit frames by DMA, DREQ paced
    PROBE_16BIT_MODE3  // E: 16-bit frames, CPOL=1/CPHA=1, which the PL022 sends gap-free
} probe_kind;

static uint16_t probe_pixels[PROBE_SPAN_PX] __attribute__((aligned(4)));
static int probe_dma = -1;

// Eight 40-pixel bands, top to bottom. Red, green and blue first because those are the
// three a byte swap scrambles most obviously.
static const int band_colour[8] = {
    COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, COLOUR_WHITE,
    COLOUR_YELLOW, COLOUR_ORANGE, COLOUR_UMBER, COLOUR_L_PURPLE
};
static const char *const band_names = "RED GRN BLU WHT YEL ORG UMB LPU";

// Results, for draw_timing().
static uint32_t probe_us_a;        // lcd_blit as shipped, boot clock
static uint32_t probe_us_best;     // the fastest variant seen
static char probe_best_label[48];
static uint32_t probe_boot_hz;
static uint32_t probe_fast_hz;
static uint32_t expand_us_loop;
static uint32_t expand_us_table;

static void probe_fill(uint16_t colour, bool byteswap)
{
    uint16_t v = byteswap ? (uint16_t)((colour << 8) | (colour >> 8)) : colour;
    for (int i = 0; i < PROBE_SPAN_PX; i++)
        probe_pixels[i] = v;
}

static inline void probe_spi_idle(void)
{
    while (spi_get_hw(LCD_SPI)->sr & SPI_SSPSR_BSY_BITS)
        tight_loop_contents();
}

// A transmit-only transfer leaves the receive FIFO full and the overrun flag set. Neither
// stalls the transmitter, but lcd.c's spi_write_blocking() calls drain the FIFO, so leave
// it the way they expect to find it.
static void probe_spi_drain(void)
{
    while (spi_is_readable(LCD_SPI))
        (void)spi_get_hw(LCD_SPI)->dr;
    spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;
}

// One 320x10 span through the chosen path. Returns the microseconds the transfer took.
// Variant A times lcd_blit() whole, window setup included; the others time the pixel push
// only, but the window setup is six command bytes and is the same for every variant.
static uint32_t probe_push(probe_kind kind, int blit)
{
    const uint16_t y0 = (uint16_t)(blit * PICO_CELL_H);
    uint64_t t0, t1;

    if (kind == PROBE_BLIT)
    {
        t0 = time_us_64();
        lcd_blit(probe_pixels, 0, y0, WIDTH, PICO_CELL_H);
        return (uint32_t)(time_us_64() - t0);
    }

    lcd_acquire();
    lcd_set_window(0, y0, WIDTH - 1, (uint16_t)(y0 + PICO_CELL_H - 1));
    probe_spi_idle();

    t0 = time_us_64();
    switch (kind)
    {
    case PROBE_8BIT_WHOLE:
        gpio_put(LCD_DCX, 1);
        gpio_put(LCD_CSX, 0);
        spi_write_blocking(LCD_SPI, (const uint8_t *)probe_pixels, sizeof(probe_pixels));
        break;

    case PROBE_16BIT:
        spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_put(LCD_DCX, 1);
        gpio_put(LCD_CSX, 0);
        spi_write16_blocking(LCD_SPI, probe_pixels, PROBE_SPAN_PX);
        break;

    case PROBE_16BIT_MODE3:
        spi_set_format(LCD_SPI, 16, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
        gpio_put(LCD_DCX, 1);
        gpio_put(LCD_CSX, 0);
        spi_write16_blocking(LCD_SPI, probe_pixels, PROBE_SPAN_PX);
        break;

    case PROBE_16BIT_DMA:
    {
        dma_channel_config c = dma_channel_get_default_config(probe_dma);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);

        spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_put(LCD_DCX, 1);
        gpio_put(LCD_CSX, 0);
        dma_channel_configure(probe_dma, &c, &spi_get_hw(LCD_SPI)->dr, probe_pixels,
                              PROBE_SPAN_PX, true);
        dma_channel_wait_for_finish_blocking(probe_dma);
        break;
    }

    default:
        break;
    }

    // The channel or the write call has handed the last frame to the FIFO; the shifter is
    // still running. CS must not rise until it stops.
    probe_spi_idle();
    probe_spi_drain();
    t1 = time_us_64();

    gpio_put(LCD_CSX, 1);
    gpio_put(LCD_DCX, 0);

    // Back to what lcd.c's command writes assume.
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    lcd_release();

    return (uint32_t)(t1 - t0);
}

// One screen: 32 blits, the panel painted in the eight bands. Returns transfer microseconds.
static uint32_t probe_screen(probe_kind kind)
{
    uint32_t total = 0;
    for (int b = 0; b < PROBE_BLITS; b++)
    {
        probe_fill(pico_term_colour(band_colour[b / 4]), kind == PROBE_8BIT_WHOLE);
        total += probe_push(kind, b);
    }
    return total;
}

static uint32_t probe_mean_screen_us(probe_kind kind)
{
    uint64_t total = 0;
    for (int rep = 0; rep < PROBE_REPS; rep++)
        total += probe_screen(kind);
    return (uint32_t)(total / PROBE_REPS);
}

// Decimal megabytes, as the stage plan's 3.125 MB/s for 25 MHz is.
static void probe_report(const char *label, uint32_t us, uint32_t hz)
{
    uint32_t kbps = (uint32_t)((uint64_t)PROBE_SCREEN_BYTES * 1000ull / us); // kB/s
    uint32_t wire_kbps = hz / 8000u;
    unsigned pct = (unsigned)(((uint64_t)kbps * 100u + wire_kbps / 2) / wire_kbps);

    ser("probe  %-44s %4lu.%01lu ms/screen  %lu.%02lu MB/s  %3u%% of wire",
        label, (unsigned long)(us / 1000u), (unsigned long)((us % 1000u) / 100u),
        (unsigned long)(kbps / 1000u), (unsigned long)((kbps % 1000u) / 10u), pct);

    if (probe_us_best == 0 || us < probe_us_best)
    {
        probe_us_best = us;
        snprintf(probe_best_label, sizeof(probe_best_label), "%s", label);
    }
}

static void probe_report_wire(uint32_t hz, uint32_t asked)
{
    uint32_t wire_kbps = hz / 8000u;
    uint32_t screen_us = (uint32_t)((uint64_t)PROBE_SCREEN_BYTES * 8000000ull / hz);

    if (asked)
        ser("probe  spi now %lu Hz (asked %lu): wire allows %lu.%03lu MB/s, %lu.%01lu ms/screen",
            (unsigned long)hz, (unsigned long)asked,
            (unsigned long)(wire_kbps / 1000u), (unsigned long)(wire_kbps % 1000u),
            (unsigned long)(screen_us / 1000u), (unsigned long)((screen_us % 1000u) / 100u));
    else
        ser("probe  spi %lu Hz: wire allows %lu.%03lu MB/s, %lu.%01lu ms/screen",
            (unsigned long)hz,
            (unsigned long)(wire_kbps / 1000u), (unsigned long)(wire_kbps % 1000u),
            (unsigned long)(screen_us / 1000u), (unsigned long)((screen_us % 1000u) / 100u));
}

static void probe_transfer(void)
{
    uint32_t us;

    probe_boot_hz = lcd_get_baudrate();
    probe_us_best = 0;
    probe_best_label[0] = '\0';

    if (probe_dma < 0)
        probe_dma = dma_claim_unused_channel(true);

    ser("probe  raw transfer: 32 blits of 320x10 (6400 B) = %lu B per screen, mean of %d screens, transfer time only",
        (unsigned long)PROBE_SCREEN_BYTES, PROBE_REPS);
    probe_report_wire(probe_boot_hz, 0);

    us = probe_mean_screen_us(PROBE_BLIT);
    probe_us_a = us;
    probe_report("A lcd_blit as built, boot clock", us, probe_boot_hz);
    us = probe_mean_screen_us(PROBE_8BIT_WHOLE);
    probe_report("B 8-bit frames, one 6400 B spi_write_blocking", us, probe_boot_hz);
    us = probe_mean_screen_us(PROBE_16BIT);
    probe_report("C 16-bit frames, spi_write16_blocking", us, probe_boot_hz);
    us = probe_mean_screen_us(PROBE_16BIT_DMA);
    probe_report("D 16-bit frames, DMA", us, probe_boot_hz);
    us = probe_mean_screen_us(PROBE_16BIT_MODE3);
    probe_report("E 16-bit frames, mode 3 (SPH=1, no frame gap)", us, probe_boot_hz);

    probe_fast_hz = spi_set_baudrate(LCD_SPI, PROBE_FAST_HZ);
    probe_report_wire(probe_fast_hz, PROBE_FAST_HZ);

    us = probe_mean_screen_us(PROBE_16BIT);
    probe_report("F 16-bit frames, blocking, faster clock", us, probe_fast_hz);
    us = probe_mean_screen_us(PROBE_16BIT_DMA);
    probe_report("G 16-bit frames, DMA, faster clock", us, probe_fast_hz);
    us = probe_mean_screen_us(PROBE_16BIT_MODE3);
    probe_report("H 16-bit frames, mode 3, faster clock", us, probe_fast_hz);
    us = probe_mean_screen_us(PROBE_BLIT);
    probe_report("I lcd_blit as built, PROBE_FAST_HZ", us, probe_fast_hz);

    uint32_t back = spi_set_baudrate(LCD_SPI, probe_boot_hz);
    ser("probe  spi restored to %lu Hz%s", (unsigned long)back,
        (back == probe_boot_hz) ? "" : "  (MISMATCH -- lcd_get_baudrate() is now wrong)");
    ser("probe  fastest: %s, %lu us/screen", probe_best_label, (unsigned long)probe_us_best);
}

// The two exhibits. Each paints one screen of bands through 16-bit frames and holds it.
static void probe_exhibit(void)
{
    ser("LOOK   painting 8 bands top to bottom %s with 16-bit frames at %lu Hz, holding %d s.",
        band_names, (unsigned long)probe_boot_hz, PROBE_HOLD_MS / 1000);
    ser("LOOK   red on top and blue third = MSB-first is right. Blue on top and red third = byte-swapped.");
    (void)probe_screen(PROBE_16BIT);
    sleep_ms(PROBE_HOLD_MS);

    uint32_t hz = spi_set_baudrate(LCD_SPI, PROBE_FAST_HZ);
    ser("LOOK   the same bands by DMA at %lu Hz, holding %d s. Speckle, streaks or wrong colours = the ribbon cannot carry it.",
        (unsigned long)hz, PROBE_HOLD_MS / 1000);
    (void)probe_screen(PROBE_16BIT_DMA);
    sleep_ms(PROBE_HOLD_MS);
    spi_set_baudrate(LCD_SPI, probe_boot_hz);
    ser("LOOK   done; the term redraws the diag screen next.");
}

// Where the time actually goes: flash, SRAM and PSRAM through the QMI (stage 045, run 2).
//
// Run 2 found the same 32 lcd_blit calls costing 45 ms from the probe loop and 78 ms from
// inside text_hook, and the identical glyph expansion costing 4.6 ms on SRAM input and 53 ms
// on the term's own input. The difference in both cases is that the term's data is in PSRAM
// while the code is in flash, and the two share the QMI on chip selects 1 and 0. This probe
// prices that directly, so the explanation is measured and not argued.
//
// Four loops, same work each time -- sum 8,192 32-bit words:
//   SRAM      : the floor.
//   PSRAM seq : one PSRAM stream, no alternation.
//   PSRAM+fl  : one PSRAM word, then one byte of the font in flash, alternating.
//   flash only: SRAM code reading only flash -- the expansion loop's own pattern, and
//               the one variant run 3 was missing. If this is far above the SRAM floor
//               then a flash-resident font is expensive to a SRAM-resident loop, which
//               is why main-pico.c now keeps its own copy.
//   memcpy    : PSRAM to SRAM in bursts, then the sum from SRAM -- what the front end does now.

#define MEMPROBE_WORDS 8192

static uint32_t memprobe_sram[MEMPROBE_WORDS];
static uint32_t memprobe_us_sram;
static uint32_t memprobe_us_psram;
static uint32_t memprobe_us_mixed;
static uint32_t memprobe_us_memcpy;
static uint32_t memprobe_us_flash;

static uint32_t memprobe_sum;

// Sum the font out of flash, from SRAM-resident code: 8,192 byte reads, no PSRAM.
static uint32_t __not_in_flash_func(memprobe_flash)(void)
{
    uint32_t sum = 0;
    uint64_t t0 = time_us_64();

    for (int i = 0; i < MEMPROBE_WORDS; i++)
        sum += font_5x10.glyphs[i & 0x3FF];

    memprobe_sum += sum;
    return (uint32_t)(time_us_64() - t0);
}

static uint32_t __not_in_flash_func(memprobe_run)(const uint32_t *p, bool mix)
{
    uint32_t sum = 0;
    uint64_t t0 = time_us_64();

    for (int i = 0; i < MEMPROBE_WORDS; i++)
    {
        sum += p[i];
        if (mix)
            sum += font_5x10.glyphs[i & 0x3FF];
    }

    memprobe_sum += sum;
    return (uint32_t)(time_us_64() - t0);
}

static void memprobe(void)
{
    uint32_t *ps = malloc(MEMPROBE_WORDS * sizeof(uint32_t));

    if (!ps)
    {
        ser("memory probe: malloc failed, skipped");
        return;
    }

    // The heap is PSRAM (specifications.md 7.1); say so rather than assume it.
    bool in_psram = ((uintptr_t)ps >= 0x11000000u);

    for (int i = 0; i < MEMPROBE_WORDS; i++)
    {
        ps[i] = (uint32_t)i;
        memprobe_sram[i] = (uint32_t)i;
    }

    memprobe_us_sram = memprobe_run(memprobe_sram, false);
    memprobe_us_psram = memprobe_run(ps, false);
    memprobe_us_mixed = memprobe_run(ps, true);
    memprobe_us_flash = memprobe_flash();

    uint64_t t0 = time_us_64();
    for (int off = 0; off < MEMPROBE_WORDS; off += 64)
        memcpy(memprobe_sram + off, ps + off, 64 * sizeof(uint32_t));
    memprobe_us_memcpy = memprobe_run(memprobe_sram, false) +
                         (uint32_t)(time_us_64() - t0);

    free(ps);

    ser("memory %d words at 0x%08lX (%s): SRAM %lu us, PSRAM seq %lu us, PSRAM+flash alternating %lu us, memcpy then SRAM %lu us",
        MEMPROBE_WORDS, (unsigned long)(uintptr_t)ps, in_psram ? "PSRAM" : "SRAM",
        (unsigned long)memprobe_us_sram, (unsigned long)memprobe_us_psram,
        (unsigned long)memprobe_us_mixed, (unsigned long)memprobe_us_memcpy);
    ser("memory flash only, SRAM code: %lu us, %lu ns per byte read (no PSRAM in this loop)",
        (unsigned long)memprobe_us_flash,
        (unsigned long)(memprobe_us_flash * 1000u / MEMPROBE_WORDS));
    ser("memory per word: SRAM %lu ns, PSRAM seq %lu ns, alternating %lu ns  (checksum %lu)",
        (unsigned long)(memprobe_us_sram * 1000u / MEMPROBE_WORDS),
        (unsigned long)(memprobe_us_psram * 1000u / MEMPROBE_WORDS),
        (unsigned long)(memprobe_us_mixed * 1000u / MEMPROBE_WORDS),
        (unsigned long)memprobe_sum);
}

// The glyph expansion loop, CPU only. 2,048 cells into the span buffer, 32 spans of 64.

static uint16_t expand_row_px[32][PICO_CELL_W];

static void expand_table_build(uint16_t fg, uint16_t bg)
{
    for (int bits = 0; bits < 32; bits++)
        for (int i = 0; i < PICO_CELL_W; i++)
            expand_row_px[bits][i] = (bits & (0x10 >> i)) ? fg : bg;
}

static uint32_t probe_expand(bool table)
{
    const uint16_t fg = pico_term_colour(COLOUR_WHITE);
    const uint16_t bg = pico_term_colour(COLOUR_DARK);
    const int stride = TD_COLS * PICO_CELL_W;
    uint64_t t0 = time_us_64();

    for (int y = 0; y < TD_ROWS; y++)
    {
        if (table)
            expand_table_build(fg, bg); // once per span, as text_hook would

        for (int i = 0; i < TD_COLS; i++)
        {
            int g = 0x21 + ((i + y) % 94);
            const uint8_t *rows = &font_5x10.glyphs[g * GLYPH_HEIGHT];
            uint16_t *cell = probe_pixels + i * PICO_CELL_W;

            for (int r = 0; r < PICO_CELL_H; r++)
            {
                uint8_t bits = rows[r];
                uint16_t *o = cell + r * stride;

                if (table)
                {
                    const uint16_t *s = expand_row_px[bits & 0x1F];
                    o[0] = s[0];
                    o[1] = s[1];
                    o[2] = s[2];
                    o[3] = s[3];
                    o[4] = s[4];
                }
                else
                {
                    o[0] = (bits & 0x10) ? fg : bg;
                    o[1] = (bits & 0x08) ? fg : bg;
                    o[2] = (bits & 0x04) ? fg : bg;
                    o[3] = (bits & 0x02) ? fg : bg;
                    o[4] = (bits & 0x01) ? fg : bg;
                }
            }
        }
    }

    return (uint32_t)(time_us_64() - t0);
}

static void probe_expansion(void)
{
    expand_us_loop = probe_expand(false);
    expand_us_table = probe_expand(true);
    ser("expand 2048 cells into the span buffer, CPU only: per-bit loop %lu us, 32-entry row table %lu us",
        (unsigned long)expand_us_loop, (unsigned long)expand_us_table);
}

// ---------------------------------------------------------------------------------------
// The timing test. The acceptance criterion is the first of the two: a full-screen
// Term_clear() plus 32 rows of Term_putstr(), with time_us_64() around Term_fresh() alone.
//
// The second is the same 32 rows changed one character at a time -- no Term_clear, so no
// total_erase and no TERM_XTRA_CLEAR. That is the worst frame the game can actually ask
// for, and the difference between the two is the cost of the redundant clear.

static uint32_t fresh_us_with_clear;
static uint32_t fresh_us_no_clear;
static uint32_t fresh_hook_us_with_clear;
static uint32_t fresh_hook_us_no_clear;
static uint32_t fresh_cells_with_clear;
static uint32_t fresh_cells_no_clear;
static uint32_t fresh_blit_us_with_clear;
static uint32_t fresh_blit_us_no_clear;
static uint32_t fresh_read_us_with_clear;
static uint32_t fresh_read_us_no_clear;
static uint32_t fresh_read_us_clear_line;
static uint32_t fresh_calls_with_clear;
static uint32_t fresh_calls_no_clear;
// The third frame: Term_clear() and then one short line. The guarded clear has to blank
// every cell the redraw will skip, and this is the frame where nearly all of them are.
static uint32_t fresh_us_clear_line;
static uint32_t fresh_hook_us_clear_line;
static uint32_t fresh_blit_us_clear_line;
static uint32_t fresh_calls_clear_line;
static uint32_t fresh_cells_clear_line;

static void fill_pattern(int phase)
{
    static char row[TD_COLS + 1];

    for (int y = 0; y < TD_ROWS; y++)
    {
        for (int x = 0; x < TD_COLS; x++)
            row[x] = (char)(0x21 + ((x + y + phase) % 94));
        row[TD_COLS] = '\0';
        Term_putstr(0, y, TD_COLS, (y % (MAX_COLORS - 1)) + 1, row);
    }
}

static void measure_fresh(void)
{
    uint64_t t0;

    // Stage 045: the raw probe first, so the transfer path is measured on its own before
    // the term's numbers are read. The exhibits leave bands on the panel; the Term_clear()
    // below sets total_erase and the first Term_fresh() repaints every cell over them.
    probe_transfer();
    probe_exhibit();
    memprobe();
    probe_expansion();

    Term_clear();
    fill_pattern(0);
    t0 = time_us_64();
    Term_fresh();
    fresh_us_with_clear = (uint32_t)(time_us_64() - t0);
    fresh_hook_us_with_clear = pico_term_last_fresh_us();
    fresh_blit_us_with_clear = pico_term_last_fresh_blit_us();
    fresh_read_us_with_clear = pico_term_last_fresh_read_us();
    fresh_calls_with_clear = pico_term_last_fresh_calls();
    fresh_cells_with_clear = pico_term_last_fresh_cells();

    fill_pattern(1);
    t0 = time_us_64();
    Term_fresh();
    fresh_us_no_clear = (uint32_t)(time_us_64() - t0);
    fresh_hook_us_no_clear = pico_term_last_fresh_us();
    fresh_blit_us_no_clear = pico_term_last_fresh_blit_us();
    fresh_read_us_no_clear = pico_term_last_fresh_read_us();
    fresh_calls_no_clear = pico_term_last_fresh_calls();
    fresh_cells_no_clear = pico_term_last_fresh_cells();

    // The panel is full of pattern characters. A clear and a one-line redraw must leave
    // none of them: this is the stage 045 acceptance test for the guarded clear.
    Term_clear();
    Term_putstr(6, 15, -1, COLOUR_L_GREEN,
                "clear test: only this line; the rest of the panel is black");
    t0 = time_us_64();
    Term_fresh();
    fresh_us_clear_line = (uint32_t)(time_us_64() - t0);
    fresh_hook_us_clear_line = pico_term_last_fresh_us();
    fresh_blit_us_clear_line = pico_term_last_fresh_blit_us();
    fresh_read_us_clear_line = pico_term_last_fresh_read_us();
    fresh_calls_clear_line = pico_term_last_fresh_calls();
    fresh_cells_clear_line = pico_term_last_fresh_cells();
    ser("LOOK   panel: black except one green line on row 15. Any pattern characters left"
        " anywhere = stale pixels after Term_clear (guard defect). Holding %d s.", PROBE_HOLD_MS / 1000);
    sleep_ms(PROBE_HOLD_MS);
}

// ---------------------------------------------------------------------------------------
// The static screen.

static unsigned n_pass;
static unsigned n_fail;

static void grade(bool ok, const char *what)
{
    if (ok)
        n_pass++;
    else
        n_fail++;
    ser("%s  %s", ok ? "pass" : "FAIL", what);
}

static void draw_rulers(void)
{
    static char row[TD_COLS + 1];

    // Tens, with a corner marker at each end of the top row.
    for (int c = 0; c < TD_COLS; c++)
        row[c] = (char)('0' + ((c / 10) % 10));
    row[0] = CORNER_GLYPH;
    row[TD_COLS - 1] = CORNER_GLYPH;
    row[TD_COLS] = '\0';
    put(0, ROW_RULER_TENS, COLOUR_L_DARK, row);

    for (int c = 0; c < TD_COLS; c++)
        row[c] = (char)('0' + (c % 10));
    row[TD_COLS] = '\0';
    put(0, ROW_RULER_UNITS, COLOUR_L_DARK, row);
}

static void draw_swatches(void)
{
    put(0, ROW_COLOUR_HDR, COLOUR_WHITE,
        "colours: index in white, name+blocks in the colour itself");

    for (int i = 0; i < MAX_COLORS; i++)
    {
        int row = ROW_SWATCH + (i / 8);
        int col = (i % 8) * 8;
        char idx[3];
        char body[8];

        snprintf(idx, sizeof(idx), "%2d", i);
        snprintf(body, sizeof(body), "%s###", colour_abbrev[i]);

        putq(col, row, COLOUR_WHITE, idx);
        putq(col + 2, row, i, body);
    }

    // The panel shows them side by side; serial gets the numbers, which is what the run log
    // needs when someone asks later whether two swatches really were different.
    for (int r = 0; r < 4; r++)
    {
        int used = 0;
        line[0] = '\0';
        for (int i = r * 8; i < r * 8 + 8; i++)
            used += snprintf(line + used, sizeof(line) - used, "%2d:%s=%04X ",
                             i, colour_abbrev[i], pico_term_colour(i));
        printf("%s\n", line);
    }
}

static void draw_ascii(void)
{
    static char row[TD_COLS + 1];
    int i;

    put(0, ROW_ASCII_HDR, COLOUR_WHITE,
        "printable ASCII 0x20-0x7E, then the fallback glyph for U+2603");

    for (i = 0; i < 64; i++)
        row[i] = (char)(0x20 + i);
    row[64] = '\0';
    put(0, ROW_ASCII_LO, COLOUR_L_WHITE, row);

    for (i = 0; i < 31; i++)
        row[i] = (char)(0x60 + i);
    row[31] = '\0';
    put(0, ROW_ASCII_HI, COLOUR_L_WHITE, row);

    putq(33, ROW_ASCII_HI, COLOUR_WHITE, "U+2603 ->");
    Term_putch(43, ROW_ASCII_HI, COLOUR_YELLOW, (wchar_t)0x2603);
    printf("U+2603 -> fallback glyph 0x%02X\n", PICO_FALLBACK_GLYPH);
}

// The four non-ASCII names the vendored lib/ tree actually contains, written as explicit
// UTF-8 bytes so this file's own encoding cannot change what is tested.
//   Osse    = O s s U+00EB          5 bytes, 4 code points
//   Manwe   = M a n w U+00EB        6 bytes, 5 code points
//   Grima   = G r U+00ED m a        6 bytes, 5 code points
//   Feanor  = F U+00E9 a n o r      7 bytes, 6 code points
static const char *const utf8_osse = "Oss\xC3\xAB";
static const char *const utf8_manwe = "Manw\xC3\xAB";
static const char *const utf8_grima = "Gr\xC3\xAD" "ma";
static const char *const utf8_feanor = "F\xC3\xA9""anor";

static void draw_utf8(void)
{
    size_t n_osse = pico_text_mbcs(NULL, utf8_osse, 0);
    size_t n_manwe = pico_text_mbcs(NULL, utf8_manwe, 0);
    size_t n_grima = pico_text_mbcs(NULL, utf8_grima, 0);
    size_t n_feanor = pico_text_mbcs(NULL, utf8_feanor, 0);

    putq(0, ROW_UTF8, COLOUR_WHITE, "utf8:");
    putq(6, ROW_UTF8, COLOUR_L_UMBER, utf8_osse);
    putq(13, ROW_UTF8, COLOUR_L_UMBER, utf8_manwe);
    putq(21, ROW_UTF8, COLOUR_L_UMBER, utf8_grima);
    putq(29, ROW_UTF8, COLOUR_L_UMBER, utf8_feanor);
    putq(38, ROW_UTF8, COLOUR_SLATE, "(4,5,5,6 cells; no lozenges)");
    ser("utf8: %s %s %s %s  bytes %u/%u/%u/%u  wide %u/%u/%u/%u",
        utf8_osse, utf8_manwe, utf8_grima, utf8_feanor,
        (unsigned)strlen(utf8_osse), (unsigned)strlen(utf8_manwe),
        (unsigned)strlen(utf8_grima), (unsigned)strlen(utf8_feanor),
        (unsigned)n_osse, (unsigned)n_manwe, (unsigned)n_grima, (unsigned)n_feanor);

    // The round trip: decode, re-encode, compare. Proves text_wctomb_hook as well.
    wchar_t wide[16];
    char back[32];
    size_t nw = pico_text_mbcs(wide, utf8_osse, 16);
    int used = 0;
    for (size_t i = 0; i < nw; i++)
        used += pico_text_wctomb(back + used, wide[i]);
    back[used] = '\0';

    bool ok = (n_osse == 4) && (n_manwe == 5) && (n_grima == 5) && (n_feanor == 6) &&
              (nw == 4) && (strcmp(back, utf8_osse) == 0) &&
              (pico_text_wcsz() == 4) &&
              (pico_text_iswprint((wint_t)'A') != 0) &&
              (pico_text_iswprint((wint_t)0x00EB) != 0) &&
              (pico_text_iswprint((wint_t)0x2603) == 0);

    putf(0, ROW_UTF8_VERDICT, ok ? COLOUR_L_GREEN : COLOUR_L_RED,
         "%s utf8 decode %u/%u/%u/%u wide, wctomb round trip %s, wcsz %d",
         ok ? "pass" : "FAIL",
         (unsigned)n_osse, (unsigned)n_manwe, (unsigned)n_grima, (unsigned)n_feanor,
         (strcmp(back, utf8_osse) == 0) ? "exact" : "DIFFERS", pico_text_wcsz());
    if (ok)
        n_pass++;
    else
        n_fail++;
}

static void draw_timing(void)
{
    bool ok = (fresh_us_with_clear < 100000u);

    putf(0, ROW_TIMING, ok ? COLOUR_L_GREEN : COLOUR_L_RED,
         "%s Term_fresh %lu us clear+32rows / %lu us 32rows (want <100000)",
         ok ? "pass" : "FAIL",
         (unsigned long)fresh_us_with_clear, (unsigned long)fresh_us_no_clear);

    if (ok)
        n_pass++;
    else
        n_fail++;

    // "in hooks" counts text_hook, wipe_hook AND TERM_XTRA_CLEAR since stage 045, so a
    // clearing frame reports 4096 cells painted: every cell twice.
    ser("timing  clear+32rows: total %lu us, in hooks %lu us, of which lcd_blit %lu us and PSRAM reads %lu us, %lu calls, %lu cells painted (clear counted)",
        (unsigned long)fresh_us_with_clear, (unsigned long)fresh_hook_us_with_clear,
        (unsigned long)fresh_blit_us_with_clear, (unsigned long)fresh_read_us_with_clear,
        (unsigned long)fresh_calls_with_clear,
        (unsigned long)fresh_cells_with_clear);
    ser("timing  32rows only : total %lu us, in hooks %lu us, of which lcd_blit %lu us and PSRAM reads %lu us, %lu calls, %lu cells painted",
        (unsigned long)fresh_us_no_clear, (unsigned long)fresh_hook_us_no_clear,
        (unsigned long)fresh_blit_us_no_clear, (unsigned long)fresh_read_us_no_clear,
        (unsigned long)fresh_calls_no_clear,
        (unsigned long)fresh_cells_no_clear);
    ser("timing  clear+1 line : total %lu us, in hooks %lu us, of which lcd_blit %lu us and PSRAM reads %lu us, %lu calls, %lu cells painted (clear counted)",
        (unsigned long)fresh_us_clear_line, (unsigned long)fresh_hook_us_clear_line,
        (unsigned long)fresh_blit_us_clear_line, (unsigned long)fresh_read_us_clear_line,
        (unsigned long)fresh_calls_clear_line,
        (unsigned long)fresh_cells_clear_line);
    ser("timing  lcd spi %lu Hz; one full 320x320 repaint is 204800 bytes",
        (unsigned long)lcd_get_baudrate());
    ser("timing  raw probe: lcd_blit as built %lu us/screen at %lu Hz; fastest variant %s %lu us at up to %lu Hz",
        (unsigned long)probe_us_a, (unsigned long)probe_boot_hz, probe_best_label,
        (unsigned long)probe_us_best, (unsigned long)probe_fast_hz);
    ser("timing  glyph expansion, 2048 cells: per-bit loop %lu us, row table %lu us",
        (unsigned long)expand_us_loop, (unsigned long)expand_us_table);
}

static void draw_progress(void)
{
    static char buf[TD_COLS + 1];
    int seen = req_seen_count();
    int used = snprintf(buf, sizeof(buf), "%2d/%2d left:", seen, n_req);

    for (int i = 0; i < n_req && used < TD_COLS; i++)
    {
        if (req[i].seen)
            continue;
        int room = TD_COLS - used;
        int n = snprintf(buf + used, (size_t)room + 1, " %s", req[i].label);
        if (n >= room)
        {
            used = TD_COLS;
            break;
        }
        used += n;
    }
    while (used < TD_COLS)
        buf[used++] = ' ';
    buf[TD_COLS] = '\0';

    putq(0, ROW_PROGRESS, (seen == n_req) ? COLOUR_L_GREEN : COLOUR_YELLOW, buf);
}

static void progress_to_serial(void)
{
    int used = snprintf(line, sizeof(line), "keys %d/%d still to press:",
                        req_seen_count(), n_req);
    for (int i = 0; i < n_req; i++)
    {
        if (req[i].seen)
            continue;
        int room = (int)sizeof(line) - used;
        if (room <= 8)
            break;
        int n = snprintf(line + used, (size_t)room, " %s", req[i].label);
        if (n < 0 || n >= room)
            break;
        used += n;
    }
    printf("%s\n", line);
}

static void draw_status(unsigned run, unsigned long frames)
{
    static char buf[TD_COLS + 1];

    snprintf(buf, sizeof(buf), "run %u  frame %lu  up %lu s  fresh %lu us",
             run, frames, (unsigned long)syscalls_uptime_s(),
             (unsigned long)pico_term_last_fresh_us());

    int n = (int)strlen(buf);
    for (int i = n; i < TD_COLS; i++)
        buf[i] = ' ';
    buf[TD_COLS] = '\0';
    buf[0] = CORNER_GLYPH;
    buf[TD_COLS - 1] = CORNER_GLYPH;

    // The two corner markers are part of this row, so the whole row goes down as one
    // string: buf[0] and buf[TD_COLS - 1] are the bottom-left and bottom-right markers.
    putq(0, ROW_STATUS, COLOUR_L_BLUE, buf);
}

// ---------------------------------------------------------------------------------------

static void draw_all(unsigned run)
{
    Term_clear();

    draw_rulers();

    putf(0, ROW_TITLE, COLOUR_L_BLUE,
         "angband_termdiag  stage 045  run %u  %dx%d cells of %dx%d px  %s",
         run, TD_COLS, TD_ROWS, PICO_CELL_W, PICO_CELL_H, buildid);

    draw_swatches();
    draw_ascii();
    draw_utf8();
    draw_timing();

    put(0, ROW_KEY_HDR, COLOUR_WHITE,
        "keys: press each one below.  F1 re-runs the whole sequence.");

    key_log_used = 0;
    key_log_draw();
    req_build();
    draw_progress();
    progress_to_serial();

    Term_fresh();

    ser("corner markers '%c' at (0,0) (63,0) (0,31) (63,31)", CORNER_GLYPH);
    ser("done   %u pass, %u FAIL   heap brk %u KB in %s",
        n_pass, n_fail, (unsigned)(psram_heap_used() / 1024u),
        psram_heap_in_psram() ? "PSRAM" : "SRAM (PSRAM DID NOT COME UP)");
}

static void run_all(unsigned run)
{
    n_pass = 0;
    n_fail = 0;

    printf("\n");
    ser("angband_termdiag -- angband-pico stage 045 (stage 040 diag + lcd probe) -- run %u%s",
        run, (run == 1) ? "" : " (re-run)");

    measure_fresh();
    draw_all(run);
}

// One key event, echoed to the panel and to serial.
static void report_key(const ui_event *ke, uint32_t gap_ms)
{
    uint32_t code = ke->key.code;
    uint8_t mods = ke->key.mods;

    // The glyph, when there is one worth showing.
    char glyph[8];
    if (code >= 0x20u && code < 0x7Fu)
        snprintf(glyph, sizeof(glyph), "'%c'", (char)code);
    else if (code >= 0x01u && code <= 0x1Au)
        snprintf(glyph, sizeof(glyph), "^%c", (char)('A' + code - 1));
    else
        snprintf(glyph, sizeof(glyph), "---");

    const char *name = "";
    switch (code)
    {
    case ARROW_UP: name = "ARROW_UP"; break;
    case ARROW_DOWN: name = "ARROW_DOWN"; break;
    case ARROW_LEFT: name = "ARROW_LEFT"; break;
    case ARROW_RIGHT: name = "ARROW_RIGHT"; break;
    case ESCAPE: name = "ESCAPE"; break;
    case KC_ENTER: name = "KC_ENTER"; break;
    case KC_TAB: name = "KC_TAB"; break;
    case KC_DELETE: name = "KC_DELETE"; break;
    case KC_BACKSPACE: name = "KC_BACKSPACE"; break;
    case KC_HOME: name = "KC_HOME"; break;
    case KC_END: name = "KC_END"; break;
    case KC_PGUP: name = "KC_PGUP"; break;
    case KC_PGDOWN: name = "KC_PGDOWN"; break;
    case KC_INSERT: name = "KC_INSERT"; break;
    case KC_BREAK: name = "KC_BREAK"; break;
    default: break;
    }

    static char buf[80];
    snprintf(buf, sizeof(buf), "code 0x%04lX %-3s mods 0x%02X %c%c%c %5lu ms %s",
             (unsigned long)code, glyph, mods,
             (mods & KC_MOD_CONTROL) ? 'C' : '-',
             (mods & KC_MOD_SHIFT) ? 'S' : '-',
             (mods & KC_MOD_ALT) ? 'A' : '-',
             (unsigned long)gap_ms, name);

    key_log_push(buf);
    printf("key   %s\n", buf);

    req_mark(code);
}

int main(void)
{
    stdio_init_all();

    // Two named references the linker needs, both for the same reason (stage 020's _sbrk
    // correction, generalised in stage 030): psram_heap.c holds the strong _sbrk and
    // syscalls.c the strong _gettimeofday, and a strong symbol in a STATIC library that
    // nothing names is never pulled in over the SDK's __weak one. term_init() below calls
    // mem_zalloc(), so the heap has to be the PSRAM one before that happens.
    syscalls_init();
    (void)psram_heap_used();

    // Before any core call: quit() with no hook calls exit(), a silent halt (stage 030).
    plog_aux = diag_plog_hook;
    quit_aux = diag_quit_hook;

    init_pico();
    Term_set_cursor(true);

    putq(0, ROW_STATUS, COLOUR_YELLOW, "waiting up to 10 s for a USB terminal...");
    Term_fresh();

    // specifications.md 6.5. Never block: the diag must still run headless.
    for (unsigned i = 0; i < 100 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    sleep_ms(300);

    unsigned run = 1;
    run_all(run);

    unsigned long frames = 0;
    uint64_t last_key_us = time_us_64();

    for (;;)
    {
        ui_event ke;

        // Non-blocking, so the frame counter keeps moving and a hang is visible. Term_inkey
        // still runs the whole ui-term.c path: it calls Term_xtra(TERM_XTRA_EVENT) which is
        // where main-pico.c polls the keyboard and pushes keys.
        while (Term_inkey(&ke, false, true) == 0)
        {
            if (ke.type != EVT_KBRD)
                continue;

            uint64_t now = time_us_64();
            uint32_t gap_ms = (uint32_t)((now - last_key_us) / 1000ull);
            last_key_us = now;

            if (ke.key.code == KC_F1)
            {
                run++;
                run_all(run);
                frames = 0;
                last_key_us = time_us_64();
                continue;
            }

            report_key(&ke, gap_ms);
            key_log_draw();
            draw_progress();
            progress_to_serial();
        }

        draw_status(run, frames);

        // Park the cursor at the end of the log so curs_hook is exercised every frame.
        Term_gotoxy(TD_COLS - 1, ROW_KEY_LOG + (key_log_used ? key_log_used - 1 : 0));
        Term_fresh();

        if ((frames % 100u) == 0u)
            ser("run %u  frame %lu  up %lu s  keys %d/%d",
                run, frames, (unsigned long)syscalls_uptime_s(),
                req_seen_count(), n_req);

        frames++;
        sleep_ms(20);
    }
}
