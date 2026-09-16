// PORT: port-written for angband-pico, stage 150 part A.
//
// angband_codediag -- does code executing from PSRAM escape the QMI toll?
// specifications.md 6.2, 6.4, 7.1, 12.1 (the "for stage 150" paragraph).
//
// This links angband_platform only, like angband_psramdiag and angband_fsdiag: a failure
// here is the platform's or the SDK's fault, not the game's. It does not link angband_core.
//
// WHAT IT DOES, IN ORDER
//
//   1. Installs a HardFault handler (a naked trampoline into an ordinary C function, the
//      same shape src/platform/bootprof.c's SysTick handler uses) that prints the stacked
//      PC/LR and SCB->CFSR/HFSR over a POLLED UART write -- not through buffered stdio,
//      which a fault mid-flush cannot be trusted to finish -- then halts. Installed before
//      anything below runs.
//   2. Proves the PSRAM copy is there: a canary array holds identical bytes in flash
//      .rodata and in PSRAM .psram_initialised data; if the SDK's psram_load memcpy
//      (specifications.md 6.2) ran before main(), the two memcmp equal. Then calls a
//      trivial function placed in PSRAM and checks its return value.
//   3. Reads lib/gamedata/monster.txt off the card into an SRAM buffer, copies it into a
//      PSRAM buffer, and fills two small numeric buffers (SRAM and PSRAM) with an index
//      pattern.
//   4. Times the {flash, sram, psram} code x {sram, psram} data matrix for L1-L4
//      (codediag_loops.c): 3 warm-up calls, then the median of 15 timed calls. Twice, 2 s
//      apart, to show the figures are stable within one boot.
//   5. Prints "CODEDIAG end".
//
// USB AND UART STDIO ARE BOTH UNCONDITIONAL HERE, UNLIKE THE OTHER THREE DIAGNOSTICS. Those
// keep USB only (specifications.md 10); this one keeps both, because the case UART (COM7)
// is what the device harness drives and it is powered independently of the Pico's own USB
// port and survives every reset (tools/harness/readme.md) -- the harness session for this
// stage runs over --console uart, the default.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/exception.h"
#include "hardware/structs/scb.h"
#include "hardware/uart.h"

#include "platform/lcd.h"
#include "platform/font5x10.h"
#include "platform/southbridge.h"
#include "platform/psram_heap.h"
#include "platform/sd_fs.h"
#include "platform/syscalls.h"

#include "codediag_loops.h"

// ---------------------------------------------------------------------------------------
// Panel, copied in shape from fsdiag.c/psramdiag.c (specifications.md 6.5: every
// diagnostic prints to USB serial and the LCD).

#define DIAG_COLS 64
#define DIAG_ROWS 32
#define DIAG_LOG_ROWS (DIAG_ROWS - 2)
#define DIAG_HINT_ROW (DIAG_ROWS - 2)
#define DIAG_STATUS_ROW (DIAG_ROWS - 1)

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const uint16_t COL_BG = RGB565(20, 12, 28);
static const uint16_t COL_FG = RGB565(222, 238, 214);
static const uint16_t COL_OK = RGB565(109, 170, 44);
static const uint16_t COL_BAD = RGB565(208, 70, 72);
static const uint16_t COL_NOTE = RGB565(218, 212, 94);

static char diag_line[200];
static int diag_row;

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

// Unlike fsdiag.c/termdiag.c, this program links no src/game/ unit at all (not even
// z-util.c), so there is no plog_aux/quit_aux to set -- nothing here can call either.

// ---------------------------------------------------------------------------------------
// Item 3 of the stage plan: a HardFault handler installed before anything below runs. The
// print is a POLLED write straight to UART0's hardware (not printf/stdio, which buffers and
// which a fault is not guaranteed to leave in a state that can still flush). The naked
// trampoline is bootprof.c's SysTick handler shape, adapted to HARDFAULT_EXCEPTION: EXC_RETURN
// bit 2 says whether the faulting code was on MSP or PSP, and "b", not "bl", leaves LR alone
// so the C function's own epilogue performs the exception return.

static void codediag_uart_puts_polled(const char *s)
{
    while (*s)
    {
        while (!uart_is_writable(uart0))
        {
        }
        uart_putc_raw(uart0, *s++);
    }
}

static void codediag_uart_puthex32_polled(uint32_t v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[9];
    for (int i = 7; i >= 0; i--)
    {
        buf[i] = hexd[v & 0xFu];
        v >>= 4;
    }
    buf[8] = '\0';
    codediag_uart_puts_polled(buf);
}

// frame[] is the eight words the Cortex-M33 exception entry stacks: r0 r1 r2 r3 r12 lr pc
// xpsr (bootprof.c's own comment explains the same layout). __attribute__((used)): its only
// reference is the "b" in the naked trampoline below, invisible to dead-code analysis.
static void __attribute__((used)) codediag_hardfault_isr_c(uint32_t *frame)
{
    codediag_uart_puts_polled("\r\nCODEDIAG HARDFAULT pc=0x");
    codediag_uart_puthex32_polled(frame[6]);
    codediag_uart_puts_polled(" lr=0x");
    codediag_uart_puthex32_polled(frame[5]);
    codediag_uart_puts_polled(" cfsr=0x");
    codediag_uart_puthex32_polled(scb_hw->cfsr);
    codediag_uart_puts_polled(" hfsr=0x");
    codediag_uart_puthex32_polled(scb_hw->hfsr);
    codediag_uart_puts_polled("\r\n");
    for (;;)
        __asm volatile("wfi");
}

static void __attribute__((naked)) codediag_hardfault_isr(void)
{
    __asm volatile(
        "tst lr, #4         \n"
        "ite eq             \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "b codediag_hardfault_isr_c \n"
        :
        :
        : "r0");
}

// ---------------------------------------------------------------------------------------
// Data buffers. Static, never malloc'd: item 6's background note says PSRAM code and data
// placement needs no heap at all for this probe, and a static placement's address is fixed
// and printable, which matters for item 3.1's proof. The SRAM copies alone are ~333 KB of
// .bss; this is a standalone diagnostic (no angband_core, no game heap) so the budget is the
// whole 512 KB SRAM region, not the ~45 KB the shipped game has left (specifications.md's
// "Background" section for this stage; do not confuse the two).

#define CODEDIAG_L12_WORDS 8192u

static uint32_t codediag_sram_words[CODEDIAG_L12_WORDS];
static uint32_t __attribute__((section(".psram_uninitialised.codediag_words")))
codediag_psram_words[CODEDIAG_L12_WORDS];

// monster.txt is ~294 KB (specifications.md 6.3); round up with margin.
#define CODEDIAG_TEXT_CAP (300u * 1024u)
#define CODEDIAG_OUT_RECORDS 64u
#define CODEDIAG_OUT_RECORD_BYTES 16u
#define CODEDIAG_OUT_BYTES (CODEDIAG_OUT_RECORDS * CODEDIAG_OUT_RECORD_BYTES)

static char codediag_sram_text[CODEDIAG_TEXT_CAP];
static char __attribute__((section(".psram_uninitialised.codediag_text")))
codediag_psram_text[CODEDIAG_TEXT_CAP];

static uint8_t codediag_sram_out[CODEDIAG_OUT_BYTES];
static uint8_t __attribute__((section(".psram_uninitialised.codediag_out")))
codediag_psram_out[CODEDIAG_OUT_BYTES];

static size_t codediag_text_len;

#define GAMEDATA_MONSTER_TXT "/angband/lib/gamedata/monster.txt"

static bool codediag_load_monster_txt(void)
{
    FILE *f = fopen(GAMEDATA_MONSTER_TXT, "rb");
    if (!f)
    {
        diag_printf_colour(COL_BAD, "FAIL  fopen(\"%s\") failed", GAMEDATA_MONSTER_TXT);
        return false;
    }

    size_t got = fread(codediag_sram_text, 1, CODEDIAG_TEXT_CAP - 1, f);
    fclose(f);

    codediag_text_len = got;
    memcpy(codediag_psram_text, codediag_sram_text, got);

    diag_printf("monster.txt: %lu B read into SRAM and copied to PSRAM (cap %lu)",
                (unsigned long)got, (unsigned long)(CODEDIAG_TEXT_CAP - 1));
    return got > 0;
}

static void codediag_fill_words(void)
{
    for (uint32_t i = 0; i < CODEDIAG_L12_WORDS; i++)
    {
        codediag_sram_words[i] = i;
        codediag_psram_words[i] = i;
    }
    memset(codediag_sram_out, 0, sizeof(codediag_sram_out));
    memset(codediag_psram_out, 0, sizeof(codediag_psram_out));
}

// ---------------------------------------------------------------------------------------
// Timing. 3 warm-up calls, then the median of 15 (stage plan item 3.2): an insertion sort
// over 15 elements is simpler than justifying a library qsort() call inside a diagnostic
// whose whole point is what is and is not on the hot path.

#define CODEDIAG_WARMUP 3
#define CODEDIAG_SAMPLES 15

static void codediag_sort15(uint32_t *a)
{
    for (int i = 1; i < CODEDIAG_SAMPLES; i++)
    {
        uint32_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v)
        {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

typedef uint32_t (*codediag_l1_fn)(const uint32_t *, size_t);
typedef uint32_t (*codediag_l2_fn)(uint32_t *, size_t, uint32_t);
typedef uint32_t (*codediag_walk_fn)(const char *, size_t, uint8_t *, size_t);

static uint32_t codediag_time_l1(codediag_l1_fn fn, const uint32_t *data, size_t n)
{
    uint32_t samples[CODEDIAG_SAMPLES];
    for (int w = 0; w < CODEDIAG_WARMUP; w++)
        (void)fn(data, n);
    for (int s = 0; s < CODEDIAG_SAMPLES; s++)
    {
        uint64_t t0 = time_us_64();
        (void)fn(data, n);
        samples[s] = (uint32_t)(time_us_64() - t0);
    }
    codediag_sort15(samples);
    return samples[CODEDIAG_SAMPLES / 2];
}

static uint32_t codediag_time_l2(codediag_l2_fn fn, uint32_t *data, size_t n, uint32_t seed)
{
    uint32_t samples[CODEDIAG_SAMPLES];
    for (int w = 0; w < CODEDIAG_WARMUP; w++)
        (void)fn(data, n, seed);
    for (int s = 0; s < CODEDIAG_SAMPLES; s++)
    {
        uint64_t t0 = time_us_64();
        (void)fn(data, n, seed);
        samples[s] = (uint32_t)(time_us_64() - t0);
    }
    codediag_sort15(samples);
    return samples[CODEDIAG_SAMPLES / 2];
}

static uint32_t codediag_time_walk(codediag_walk_fn fn, const char *text, size_t len,
                                   uint8_t *out, size_t out_count)
{
    uint32_t samples[CODEDIAG_SAMPLES];
    for (int w = 0; w < CODEDIAG_WARMUP; w++)
        (void)fn(text, len, out, out_count);
    for (int s = 0; s < CODEDIAG_SAMPLES; s++)
    {
        uint64_t t0 = time_us_64();
        (void)fn(text, len, out, out_count);
        samples[s] = (uint32_t)(time_us_64() - t0);
    }
    codediag_sort15(samples);
    return samples[CODEDIAG_SAMPLES / 2];
}

// ---------------------------------------------------------------------------------------
// The matrix.

static void codediag_report_l1(const char *code, const char *data_label,
                               codediag_l1_fn fn, const uint32_t *data)
{
    uint32_t us = codediag_time_l1(fn, data, CODEDIAG_L12_WORDS);
    uint64_t ns_x10 = ((uint64_t)us * 10000ull) / CODEDIAG_L12_WORDS;
    diag_printf("L1 sum   code=%-5s data=%-5s  %5lu us  %lu.%01lu ns/access",
                code, data_label, (unsigned long)us,
                (unsigned long)(ns_x10 / 10), (unsigned long)(ns_x10 % 10));
}

static void codediag_report_l2(const char *code, const char *data_label,
                               codediag_l2_fn fn, uint32_t *data)
{
    uint32_t us = codediag_time_l2(fn, data, CODEDIAG_L12_WORDS, 0x1000u);
    uint64_t ns_x10 = ((uint64_t)us * 10000ull) / CODEDIAG_L12_WORDS;
    diag_printf("L2 store code=%-5s data=%-5s  %5lu us  %lu.%01lu ns/access",
                code, data_label, (unsigned long)us,
                (unsigned long)(ns_x10 / 10), (unsigned long)(ns_x10 % 10));
}

static void codediag_report_walk(const char *loop_name, const char *code, const char *data_label,
                                 codediag_walk_fn fn, const char *text, size_t len,
                                 uint8_t *out, size_t out_count)
{
    uint32_t us = codediag_time_walk(fn, text, len, out, out_count);
    uint64_t kbs_x10 = us ? ((uint64_t)len * 10000000ull) / ((uint64_t)us * 1024ull) : 0ull;
    diag_printf("%-8s code=%-5s data=%-5s  %5lu.%01lu ms  %lu.%01lu KB/s  (%lu B)",
                loop_name, code, data_label,
                (unsigned long)(us / 1000u), (unsigned long)((us % 1000u) / 100u),
                (unsigned long)(kbs_x10 / 10), (unsigned long)(kbs_x10 % 10),
                (unsigned long)len);
}

static void codediag_run_matrix(unsigned pass)
{
    diag_printf_colour(COL_NOTE, "-- matrix pass %u --", pass);

    // L1, L2: {flash, sram, psram} code x {sram, psram} data.
    codediag_report_l1("flash", "sram", codediag_l1_sum_flash, codediag_sram_words);
    codediag_report_l1("sram", "sram", codediag_l1_sum_sram, codediag_sram_words);
    codediag_report_l1("psram", "sram", codediag_l1_sum_psram, codediag_sram_words);
    codediag_report_l1("flash", "psram", codediag_l1_sum_flash, codediag_psram_words);
    codediag_report_l1("sram", "psram", codediag_l1_sum_sram, codediag_psram_words);
    codediag_report_l1("psram", "psram", codediag_l1_sum_psram, codediag_psram_words);

    codediag_report_l2("flash", "sram", codediag_l2_store_flash, codediag_sram_words);
    codediag_report_l2("sram", "sram", codediag_l2_store_sram, codediag_sram_words);
    codediag_report_l2("psram", "sram", codediag_l2_store_psram, codediag_sram_words);
    codediag_report_l2("flash", "psram", codediag_l2_store_flash, codediag_psram_words);
    codediag_report_l2("sram", "psram", codediag_l2_store_sram, codediag_psram_words);
    codediag_report_l2("psram", "psram", codediag_l2_store_psram, codediag_psram_words);

    // L3, L4: same shape, over the monster.txt copies.
    codediag_report_walk("L3 walk", "flash", "sram", codediag_l3_walk_flash,
                        codediag_sram_text, codediag_text_len, codediag_sram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L3 walk", "sram", "sram", codediag_l3_walk_sram,
                        codediag_sram_text, codediag_text_len, codediag_sram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L3 walk", "psram", "sram", codediag_l3_walk_psram,
                        codediag_sram_text, codediag_text_len, codediag_sram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L3 walk", "flash", "psram", codediag_l3_walk_flash,
                        codediag_psram_text, codediag_text_len, codediag_psram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L3 walk", "sram", "psram", codediag_l3_walk_sram,
                        codediag_psram_text, codediag_text_len, codediag_psram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L3 walk", "psram", "psram", codediag_l3_walk_psram,
                        codediag_psram_text, codediag_text_len, codediag_psram_out, CODEDIAG_OUT_RECORDS);

    codediag_report_walk("L4 table", "flash", "sram", codediag_l4_table_flash,
                        codediag_sram_text, codediag_text_len, codediag_sram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L4 table", "sram", "sram", codediag_l4_table_sram,
                        codediag_sram_text, codediag_text_len, codediag_sram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L4 table", "psram", "sram", codediag_l4_table_psram,
                        codediag_sram_text, codediag_text_len, codediag_sram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L4 table", "flash", "psram", codediag_l4_table_flash,
                        codediag_psram_text, codediag_text_len, codediag_psram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L4 table", "sram", "psram", codediag_l4_table_sram,
                        codediag_psram_text, codediag_text_len, codediag_psram_out, CODEDIAG_OUT_RECORDS);
    codediag_report_walk("L4 table", "psram", "psram", codediag_l4_table_psram,
                        codediag_psram_text, codediag_text_len, codediag_psram_out, CODEDIAG_OUT_RECORDS);
}

// ---------------------------------------------------------------------------------------

static void codediag_prove_psram(void)
{
    diag_printf_colour(COL_NOTE, "-- item 3.1: is the PSRAM copy really there? --");

    diag_printf("flash canary at %p, PSRAM canary at %p",
                (const void *)codediag_flash_canary, (const void *)codediag_psram_canary);

    int cmp = memcmp(codediag_psram_canary, codediag_flash_canary, CODEDIAG_CANARY_BYTES);
    diag_printf_colour(cmp == 0 ? COL_OK : COL_BAD,
                       "%s  psram canary memcmp = %d (0 means the SDK's psram_load copy ran)",
                       cmp == 0 ? "pass" : "FAIL", cmp);

    uint32_t got = codediag_psram_trivial();
    diag_printf_colour(got == CODEDIAG_PSRAM_TRIVIAL_MAGIC ? COL_OK : COL_BAD,
                       "%s  codediag_psram_trivial() at %p returned 0x%08lx (expected 0x%08lx)",
                       got == CODEDIAG_PSRAM_TRIVIAL_MAGIC ? "pass" : "FAIL",
                       (const void *)codediag_psram_trivial,
                       (unsigned long)got, (unsigned long)CODEDIAG_PSRAM_TRIVIAL_MAGIC);
}

int main(void)
{
    // Item 3 of the stage plan: the HardFault handler goes in before anything else runs.
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, codediag_hardfault_isr);

    stdio_init_all();

    // Two named references the linker needs (specifications.md 6.2, 6.3's _sbrk/
    // _gettimeofday lesson): psram_heap.c and syscalls.c both hold strong overrides of SDK
    // __weak symbols, pulled in only if something names them.
    syscalls_init();
    (void)psram_heap_in_psram();

    lcd_init();
    lcd_set_font(&font_5x10);
    lcd_enable_cursor(false);
    lcd_set_background(COL_BG);
    lcd_set_foreground(COL_FG);
    lcd_clear_screen();

    sb_init();

    diag_draw(DIAG_HINT_ROW, "waiting up to 10 s for a USB terminal...", COL_NOTE);
    for (unsigned i = 0; i < 100 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    sleep_ms(300);

    diag_printf_colour(COL_NOTE, "CODEDIAG start -- stage 150 part A");

    psram_heap_stats_t hs;
    psram_heap_stats(&hs);
    diag_printf("psram %s, size %lu KB, base 0x%08lx",
                hs.in_psram ? "present" : "ABSENT", (unsigned long)(hs.psram_size / 1024u),
                (unsigned long)hs.base);

    codediag_prove_psram();

    diag_printf_colour(COL_NOTE, "-- loading test data --");
    bool have_sd = false;
    {
        uint64_t t0 = time_us_64();
        bool ok = sd_fs_mount();
        uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000ull);
        diag_printf_colour(ok ? COL_OK : COL_BAD, "%s  sd_fs_mount() in %lu ms",
                           ok ? "pass" : "FAIL", (unsigned long)ms);
        if (ok)
            have_sd = codediag_load_monster_txt();
    }
    codediag_fill_words();

    if (!have_sd)
    {
        diag_printf_colour(COL_BAD,
                           "FAIL  no monster.txt corpus -- L3/L4 rows below read 0 B and are not a result");
    }

    codediag_run_matrix(1);
    sleep_ms(2000);
    codediag_run_matrix(2);

    diag_printf_colour(COL_NOTE, "CODEDIAG end");

    diag_draw(DIAG_HINT_ROW, "press any key to re-run the whole sequence", COL_NOTE);
    printf("press any key on the PicoCalc to re-run the whole sequence\n");

    unsigned run = 1;
    unsigned long frames = 0;
    while (true)
    {
        uint16_t key = sb_read_keyboard();
        if (((key >> 8) & 0xFFu) == 1u)
        {
            run++;
            diag_printf_colour(COL_NOTE, "CODEDIAG start -- run %u", run);
            codediag_prove_psram();
            codediag_run_matrix(1);
            sleep_ms(2000);
            codediag_run_matrix(2);
            diag_printf_colour(COL_NOTE, "CODEDIAG end");
            diag_draw(DIAG_HINT_ROW, "press any key to re-run the whole sequence", COL_NOTE);
            printf("press any key on the PicoCalc to re-run the whole sequence\n");
            frames = 0;
        }

        snprintf(diag_line, sizeof(diag_line), "run %u   frame %lu   up %lu s", run, frames,
                 (unsigned long)syscalls_uptime_s());
        diag_draw(DIAG_STATUS_ROW, diag_line, COL_NOTE);

        frames++;
        sleep_ms(50);
    }
}
