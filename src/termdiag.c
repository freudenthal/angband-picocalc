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
// STACK NOTE. The core-0 stack is the 2 KB the SDK puts in SCRATCH_Y. Every buffer here is
// static; nothing large is automatic.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

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

    Term_clear();
    fill_pattern(0);
    t0 = time_us_64();
    Term_fresh();
    fresh_us_with_clear = (uint32_t)(time_us_64() - t0);
    fresh_hook_us_with_clear = pico_term_last_fresh_us();
    fresh_cells_with_clear = pico_term_last_fresh_cells();

    fill_pattern(1);
    t0 = time_us_64();
    Term_fresh();
    fresh_us_no_clear = (uint32_t)(time_us_64() - t0);
    fresh_hook_us_no_clear = pico_term_last_fresh_us();
    fresh_cells_no_clear = pico_term_last_fresh_cells();
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

    ser("timing  clear+32rows: total %lu us, in hooks %lu us, %lu cells",
        (unsigned long)fresh_us_with_clear, (unsigned long)fresh_hook_us_with_clear,
        (unsigned long)fresh_cells_with_clear);
    ser("timing  32rows only : total %lu us, in hooks %lu us, %lu cells",
        (unsigned long)fresh_us_no_clear, (unsigned long)fresh_hook_us_no_clear,
        (unsigned long)fresh_cells_no_clear);
    ser("timing  lcd spi %lu Hz; one full 320x320 repaint is 204800 bytes",
        (unsigned long)lcd_get_baudrate());
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
         "angband_termdiag  stage 040  run %u  %dx%d cells of %dx%d px  %s",
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
    ser("angband_termdiag -- angband-pico stage 040 -- run %u%s",
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
    snprintf(buf, sizeof(buf), "code 0x%04lX %-3s mods 0x%02X %c%c%c %+5lu ms %s",
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
