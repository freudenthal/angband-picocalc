// PORT: port-written for angband-pico, stage 040. See main-pico.h.
//
// Written after angband/src/main-nds.c -- upstream's own port to a small ARM handheld --
// and angband/src/main-xxx.c, the annotated template. Neither is vendored into this tree;
// both were read from angband/ next door.
//
// STACK NOTE (stage 020): the core-0 stack is the 2 KB the SDK puts in SCRATCH_Y. The span
// buffer below is 6,400 bytes and is therefore static, not automatic.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "font5x10.h"
#include "keyboard.h"
#include "lcd.h"
#include "main-pico.h"
#include "turnlog.h"
#include "utf8.h"

#include "ui-event.h"
#include "ui-term.h"
#include "z-color.h"
#include "z-util.h"

// ---------------------------------------------------------------------------------------
// The term.

typedef struct
{
    term t;
} term_data;

static term_data data[1];

static bool pico_initialised = false;

// angband_color_table (z-color.c) as RGB565, built once at init and again on
// TERM_XTRA_REACT. Entry [0] of each row is unused padding; [1], [2], [3] are R, G, B.
static uint16_t colour_rgb565[MAX_COLORS];

// One span of cells, expanded to pixels for a single lcd_blit. A span can be the full
// width of the term: 64 * 5 * 10 * 2 = 6,400 bytes.
static uint16_t span_pixels[PICO_TERM_COLS * PICO_CELL_W * PICO_CELL_H];

// One span's code points and their font indices, in SRAM. See Term_text_pico: the
// wchar_t array ui-term.c hands over lives in the game's heap, which is PSRAM.
static wchar_t span_cp[PICO_TERM_COLS];
static uint8_t span_glyph[PICO_TERM_COLS];

// The font, copied into SRAM at init (stage 045).
//
// font5x10.c's table is const, so it lives in flash, and the expansion loop reads ten
// bytes of it per cell -- 20,480 flash reads per full screen. That is cheap while the
// loop itself is also executing from flash, because instruction fetch keeps the XIP
// path busy and the font rides along in the same cache. It stops being cheap the moment
// the loop runs from SRAM, which is where stage 045 put it: the font reads become the
// only QMI traffic the loop generates, and each one pays for the round trip. Measured:
// the identical expansion took 4.6 ms on a flash-resident probe and 44 ms in the
// SRAM-resident hook, a gap of almost exactly ten flash reads per cell.
//
// 1,280 bytes buys all of it back. Nothing outside this file uses the copy; lcd.c's own
// lcd_putc() still reads the flash original, and is not on any hot path here.
#define PICO_FONT_GLYPHS 128
static uint8_t font_sram[PICO_FONT_GLYPHS * GLYPH_HEIGHT];

// The glyph row table (stage 045). A glyph row is five bits, so there are 32 possible
// rows; each entry is that row as five pixels in the current fg/bg. Rebuilt only when
// fg or bg change, which is at most once per text_hook call and usually not at all.
// Measured: 2,048 cells expanded in 4.6 ms through the table, 27 ms through the
// per-pixel branch it replaced.
static uint16_t row_px[32][PICO_CELL_W];
static uint16_t row_px_fg = 0xFFFF;
static uint16_t row_px_bg = 0xFFFF;
static bool row_px_valid = false;

// Per-frame counters; see main-pico.h.
static uint32_t paint_us;
static uint32_t paint_blit_us;
static uint32_t paint_read_us;
static uint32_t paint_calls;
static uint32_t paint_cells;
static uint32_t last_fresh_us;
static uint32_t last_fresh_blit_us;
static uint32_t last_fresh_read_us;
static uint32_t last_fresh_calls;
static uint32_t last_fresh_cells;

// The soft cursor. ui-term.c marks the old cursor cell dirty when the cursor moves, so the
// redraw usually erases it for us -- but not when the cursor is switched off, and not when
// nothing else on that row changed. Tracking it here covers both.
//
// The cursor is a one-pixel underline on the tenth pixel row of the cell. lcd.c's own
// cursor uses the same row and says why: the printable glyphs in this font do not reach it,
// so drawing and erasing the cursor cannot corrupt a glyph.
static int cursor_x = -1;
static int cursor_y = -1;

static void build_colour_table(void)
{
    for (int i = 0; i < MAX_COLORS; i++)
        colour_rgb565[i] = RGB(angband_color_table[i][1],
                               angband_color_table[i][2],
                               angband_color_table[i][3]);
}

uint16_t pico_term_colour(int a)
{
    return colour_rgb565[(unsigned)a % MAX_COLORS];
}

uint32_t pico_term_last_fresh_us(void) { return last_fresh_us; }
uint32_t pico_term_last_fresh_blit_us(void) { return last_fresh_blit_us; }
uint32_t pico_term_last_fresh_read_us(void) { return last_fresh_read_us; }
uint32_t pico_term_last_fresh_calls(void) { return last_fresh_calls; }
uint32_t pico_term_last_fresh_cells(void) { return last_fresh_cells; }

static void row_table_build(uint16_t fg, uint16_t bg)
{
    if (row_px_valid && row_px_fg == fg && row_px_bg == bg)
        return;
    for (int bits = 0; bits < 32; bits++)
        for (int i = 0; i < PICO_CELL_W; i++)
            row_px[bits][i] = (bits & (0x10 >> i)) ? fg : bg;
    row_px_fg = fg;
    row_px_bg = bg;
    row_px_valid = true;
}

// lcd_blit with the time it took added to the per-frame counter.
static void blit_timed(uint16_t *pixels, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint64_t t0 = time_us_64();
    lcd_blit(pixels, x, y, w, h);
    paint_blit_us += (uint32_t)(time_us_64() - t0);
}

// ---------------------------------------------------------------------------------------
// Drawing.

static void cursor_forget(void)
{
    cursor_x = -1;
    cursor_y = -1;
}

static void cursor_erase(void)
{
    if (cursor_x < 0)
        return;

    lcd_solid_rectangle(colour_rgb565[COLOUR_DARK],
                        (uint16_t)(cursor_x * PICO_CELL_W),
                        (uint16_t)(cursor_y * PICO_CELL_H + PICO_CELL_H - 1),
                        PICO_CELL_W, 1);
    cursor_forget();
}

/**
 * Draw n glyphs at (x, y) in attribute a.
 *
 * The attribute carries a background set above MULT_BG (z-color.h): BG_BLACK, BG_SAME and
 * BG_DARK. main-nds.c decodes all three and so does this, even though the plan asked only
 * for a black background -- a term that ignores the field paints the wrong background the
 * first time the game asks for one, and the cost here is a switch.
 */
// In SRAM (__not_in_flash_func), for the reason lcd.c's pixel path is: flash and PSRAM
// share the QMI, and alternating between them costs a chip-select cycle each way.
static errr __not_in_flash_func(Term_text_pico)(int x, int y, int n, int a, const wchar_t *s)
{
    if (n <= 0 || y < 0 || y >= PICO_TERM_ROWS || x < 0 || x >= PICO_TERM_COLS)
        return 0;
    if (n > PICO_TERM_COLS - x)
        n = PICO_TERM_COLS - x;

    uint64_t t0 = time_us_64();

    uint16_t fg = colour_rgb565[(unsigned)a % MAX_COLORS];
    uint16_t bg;

    switch ((unsigned)a / MULT_BG)
    {
    case BG_SAME: bg = fg; break;
    case BG_DARK: bg = colour_rgb565[COLOUR_SHADE]; break;
    default:      bg = colour_rgb565[COLOUR_DARK]; break;
    }

    const int stride = n * PICO_CELL_W;

    row_table_build(fg, bg);

    // Pass 1: get the span out of PSRAM in one sequential burst, and resolve the font
    // indices while the characters are in SRAM.
    //
    // Doing the lookup inline with the expansion below reads one PSRAM word, then ten
    // font bytes from flash, then one PSRAM word again -- 2,048 alternations across a
    // full screen, each costing a QMI chip-select cycle (see lcd.c's note). Stage 045
    // measured that loop at 53 ms against the 4.6 ms the identical expansion takes when
    // its input is already in SRAM. memcpy() is the right shape for PSRAM: stage 020
    // measured 14.29 MB/s sequential and 5.72 MB/s word at a time.
    //
    // The ASCII test is inline because pico_utf8_glyph() lives in utf8.c, i.e. in flash,
    // and calling it per cell would put the alternation back. Every code point the game
    // draws is ASCII bar the fourteen accented vowels in lib/ (specifications.md 6.4).
    uint64_t tr0 = time_us_64();
    memcpy(span_cp, s, (size_t)n * sizeof(wchar_t));
    paint_read_us += (uint32_t)(time_us_64() - tr0);

    for (int i = 0; i < n; i++)
    {
        uint32_t cp = (uint32_t)span_cp[i];
        int g;

        if (cp >= 0x20u && cp < 0x7Fu)
            g = (int)cp;
        else
        {
            g = pico_utf8_glyph(cp);
            if (g < 0)
                g = PICO_FALLBACK_GLYPH;
        }
        span_glyph[i] = (uint8_t)g;
    }

    // Pass 2: expand. SRAM and flash only; no PSRAM is touched below this line.
    for (int i = 0; i < n; i++)
    {
        const uint8_t *rows = &font_sram[span_glyph[i] * GLYPH_HEIGHT];
        uint16_t *cell = span_pixels + i * PICO_CELL_W;

        for (int r = 0; r < PICO_CELL_H; r++)
        {
            const uint16_t *px = row_px[rows[r] & 0x1F];
            uint16_t *o = cell + r * stride;

            o[0] = px[0];
            o[1] = px[1];
            o[2] = px[2];
            o[3] = px[3];
            o[4] = px[4];
        }
    }

    blit_timed(span_pixels,
               (uint16_t)(x * PICO_CELL_W), (uint16_t)(y * PICO_CELL_H),
               (uint16_t)stride, PICO_CELL_H);

    // The blit covers the cursor's underline row if the cursor sat in this span.
    if (cursor_y == y && cursor_x >= x && cursor_x < x + n)
        cursor_forget();

    paint_us += (uint32_t)(time_us_64() - t0);
    paint_calls++;
    paint_cells += (uint32_t)n;
    return 0;
}

/**
 * Fill a rectangle of whole cells with one colour, in a single blit.
 *
 * lcd_solid_rectangle() would do this too, but it issues one lcd_blit -- and therefore one
 * CASET/RASET/RAMWR window setup -- per pixel row. A 64-cell wipe is 10 window setups
 * through it and 1 through here.
 */
static void fill_cells(uint16_t colour, int x, int y, int n, int rows)
{
    const int w = n * PICO_CELL_W;
    const int h = rows * PICO_CELL_H;

    for (int i = 0; i < w * h; i++)
        span_pixels[i] = colour;

    blit_timed(span_pixels,
               (uint16_t)(x * PICO_CELL_W), (uint16_t)(y * PICO_CELL_H),
               (uint16_t)w, (uint16_t)h);
}

/**
 * Erase n cells at (x, y).
 */
static errr Term_wipe_pico(int x, int y, int n)
{
    if (n <= 0 || y < 0 || y >= PICO_TERM_ROWS || x < 0 || x >= PICO_TERM_COLS)
        return 0;
    if (n > PICO_TERM_COLS - x)
        n = PICO_TERM_COLS - x;

    uint64_t t0 = time_us_64();

    fill_cells(colour_rgb565[COLOUR_DARK], x, y, n, 1);

    if (cursor_y == y && cursor_x >= x && cursor_x < x + n)
        cursor_forget();

    paint_us += (uint32_t)(time_us_64() - t0);
    paint_calls++;
    paint_cells += (uint32_t)n;
    return 0;
}

/**
 * Move the soft cursor to (x, y).
 */
static errr Term_curs_pico(int x, int y)
{
    if (x < 0 || x >= PICO_TERM_COLS || y < 0 || y >= PICO_TERM_ROWS)
        return 0;

    cursor_erase();

    lcd_solid_rectangle(colour_rgb565[COLOUR_WHITE],
                        (uint16_t)(x * PICO_CELL_W),
                        (uint16_t)(y * PICO_CELL_H + PICO_CELL_H - 1),
                        PICO_CELL_W, 1);

    cursor_x = x;
    cursor_y = y;
    return 0;
}

// ---------------------------------------------------------------------------------------
// Keys.
//
// keyboard.c hands over (scan code, shift, ctrl, alt) with the modifiers latched at the
// moment the key was queued. It has already folded Shift into the character for printable
// keys -- press Shift and comma and the south bridge reports '<' -- and has already turned
// the firmware's LF into CR.

/**
 * Map one south bridge event onto an Angband keycode and push it into the term's queue.
 *
 * Ctrl follows ui-event.h's rule, not the stage plan's. The plan and specifications.md 6.4
 * both said "KTRL(c) with KC_MOD_CONTROL"; ui-event.h says the two are alternatives, and
 * ui-event.c's own STORE() macro normalises KTRL-able codes to (KTRL(c), no modifier)
 * before storing a keymap trigger. Sending both would mean no keymap loaded from a pref
 * file could ever match a Ctrl keystroke typed here. main-sdl2.c, main-x11.c and
 * nds-keyboard.c all clear the bit as well.
 */
static void push_key(const kbd_event_t *e)
{
    keycode_t code;
    uint8_t mods = 0;

    switch (e->code)
    {
    case KEY_ESC:       code = ESCAPE; break;
    case KEY_UP:        code = ARROW_UP; break;
    case KEY_DOWN:      code = ARROW_DOWN; break;
    case KEY_LEFT:      code = ARROW_LEFT; break;
    case KEY_RIGHT:     code = ARROW_RIGHT; break;
    case KEY_RETURN:    code = KC_ENTER; break;
    case KEY_ENTER:     code = KC_ENTER; break;
    case KEY_TAB:       code = KC_TAB; break;
    case KEY_BACKSPACE: code = KC_BACKSPACE; break;
    case KEY_DEL:       code = KC_DELETE; break;
    case KEY_INSERT:    code = KC_INSERT; break;
    case KEY_HOME:      code = KC_HOME; break;
    case KEY_END:       code = KC_END; break;
    case KEY_PAGE_UP:   code = KC_PGUP; break;
    case KEY_PAGE_DOWN: code = KC_PGDOWN; break;
    case KEY_BREAK:     code = KC_BREAK; break;
    case KEY_F1:        code = KC_F1; break;
    case KEY_F2:        code = KC_F2; break;
    case KEY_F3:        code = KC_F3; break;
    case KEY_F4:        code = KC_F4; break;
    case KEY_F5:        code = KC_F5; break;
    case KEY_F6:        code = KC_F6; break;
    case KEY_F7:        code = KC_F7; break;
    case KEY_F8:        code = KC_F8; break;
    case KEY_F9:        code = KC_F9; break;
    case KEY_F10:       code = KC_F10; break;

    default:
        if (e->code >= 0x20 && e->code < 0x7F)
            code = e->code;
        else
            return; // nothing the game has a code for; drop it rather than invent one
        break;
    }

    // Shift is only a modifier when it has not already been folded into the character.
    // Passing it as well would make Shift+comma arrive as ('<', SHIFT), and ui-event.c's
    // keymap comparison looks at both fields.
    const bool printable = (code >= 0x20 && code < 0x7F);

    if (e->shift && !printable)
        mods |= KC_MOD_SHIFT;

    if (e->ctrl)
    {
        keycode_t u = code;

        // Ctrl+a and Ctrl+A are the same command; upstream's NDS port folds case here for
        // the same reason.
        if (u >= 'a' && u <= 'z')
            u -= 0x20;

        if (ENCODE_KTRL(u))
            code = KTRL(u);
        else
            mods |= KC_MOD_CONTROL;
    }

    if (e->alt)
        mods |= KC_MOD_ALT;

    Term_keypress(code, mods);
}

// PORT: picocalc-device-harness stage 045 -------------------------------------------------
#ifdef ANGBAND_SERIAL_SCREEN
/**
 * The screen mirror: the term grid as text, on request, over the console.
 *
 * The driver sends one 0x1C and gets back one framed block (harness specifications.md 8.5):
 *
 *     SCR BEGIN <seq> <cols> <rows>
 *     SCR <rr> |<64 characters>|
 *     SCR END <seq> <cells> <substituted> <crc32>
 *
 * WHERE IT IS SERVED, AND WHY NOT Term_fresh(). This runs from check_events(), inside
 * TURNLOG_MARK(tl_idle)/TURNLOG_IDLE(tl_idle), so its ~200 ms of wire at 115200 is
 * subtracted from the turn's `tot` exactly as the stage 030 key path is. The obvious place
 * -- Term_xtra_pico's TERM_XTRA_FRESH case, a few lines down -- is inside the measured
 * command: Term_fresh() is called from the game's command processing, so a block emitted
 * there would add about 195 ms to every frame against a town command of 1.53-1.64 s. That
 * is a 12 % instrument in the thing it measures.
 *
 * scr, NOT old. check_events() is reached only after Term_fresh() has returned, so at this
 * instant scr and old agree and scr is the game's own answer to "what is on the screen". A
 * later stage that serves a dump from anywhere else has to revisit that.
 *
 * THE ROW IS COPIED BEFORE IT IS SCANNED, for the reason TERM_XTRA_CLEAR below gives:
 * the term's cells live in PSRAM and this code runs from flash, so touching them cell by
 * cell alternates QMI chip selects at the 7,766 ns measured in the Angband project's
 * stage 045. One memcpy per row, then the scan runs entirely in SRAM.
 *
 * Attributes are not sent. If a stage ever needs to tell a red D from a white one, SCRA
 * rows carrying one hex digit per cell are added beside these without changing them.
 */

static uint32_t screen_seq = 0;

// One row out of PSRAM, and the ASCII it becomes. Static, not automatic: the file's stack
// note applies, and 64 wchar_t is 256 bytes.
static wchar_t screen_row_cp[PICO_TERM_COLS];
static char screen_row[PICO_TERM_COLS + 1];

/**
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), bitwise so it needs no 1 KB table.
 *
 * This is what zlib.crc32 computes, which is what harness/screen.py checks every dump
 * with: crc32_update(0xFFFFFFFF, "123456789", 9) ^ 0xFFFFFFFF == 0xCBF43926. Bitwise costs
 * 8 iterations per byte over 2,048 bytes once per request, inside the idle bracket.
 */
static uint32_t crc32_update(uint32_t crc, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        crc ^= (uint8_t)buf[i];

        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }

    return crc;
}

/**
 * Emit one dump. Called from push_serial_key() and from nowhere else.
 */
static void dump_screen(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t substituted = 0;

    screen_seq++;

    printf("SCR BEGIN %lu %d %d\n", (unsigned long)screen_seq,
           PICO_TERM_COLS, PICO_TERM_ROWS);

    for (int y = 0; y < PICO_TERM_ROWS; y++)
    {
        memcpy(screen_row_cp, Term->scr->c[y], sizeof(screen_row_cp));

        for (int x = 0; x < PICO_TERM_COLS; x++)
        {
            wchar_t cp = screen_row_cp[x];

            // Printable ASCII goes as itself -- '|' included, which is why the parser
            // splits on the FIRST and LAST bar of a row and not on any bar. Everything
            // else becomes '?' and is counted, so a font or term change that starts
            // producing non-ASCII is visible in the END line rather than silent.
            if (cp < 0x20 || cp > 0x7E)
            {
                screen_row[x] = '?';
                substituted++;
            }
            else
            {
                screen_row[x] = (char)cp;
            }
        }

        screen_row[PICO_TERM_COLS] = '\0';
        crc = crc32_update(crc, screen_row, PICO_TERM_COLS);
        printf("SCR %02d |%s|\n", y, screen_row);
    }

    // The CRC covers the row payloads concatenated, with no bars and no row numbers, so
    // both sides compute it over the same bytes.
    printf("SCR END %lu %d %lu %08lx\n", (unsigned long)screen_seq,
           PICO_TERM_COLS * PICO_TERM_ROWS, (unsigned long)substituted,
           (unsigned long)(crc ^ 0xFFFFFFFFu));
}
#endif
// -----------------------------------------------------------------------------------------

// PORT: picocalc-device-harness stage 030 -------------------------------------------------
#ifdef ANGBAND_SERIAL_KEYS
/**
 * Map one byte from either stdio transport onto an Angband keycode and push it.
 *
 * The table is the harness specifications.md 8.3 and both sides implement exactly it; the
 * driver's own tests/test_keys.py checks the same rows from the PC end. Returns true if a
 * keypress was pushed, false if the byte was dropped.
 *
 * Everything unmapped is dropped rather than turned into something, which is also what
 * protects the game when the case cable is out: the CH340 is unpowered then, GP1 floats or
 * sits at the pad's reset pull, and the UART can deliver 0x00 and break bytes. 0x0A is
 * dropped so that a sender may write CR LF and the game sees one Enter. Arrow keys are
 * deliberately not mapped: a VT100 sequence begins with ESC, which is itself a key the game
 * needs, and Angband's original keyset moves on the digits.
 */
static bool push_serial_key(int c)
{
    keycode_t code;

    if (c >= 0x20 && c <= 0x7E)
        code = c;
    else if (c == 0x0D)
        code = KC_ENTER;
    else if (c == 0x0A)
    {
        // Dropped, so that a sender may write CR LF and the game sees one Enter. It has to
        // be taken BEFORE the Ctrl range below: 0x0A is inside 0x01..0x1A, and without this
        // branch it becomes KTRL('J'), which Angband opens its command menu on. Measured on
        // the device 2026-09-11, which is the only reason this line exists.
        return false;
    }
    else if (c == 0x09)
        code = KC_TAB;
    else if (c == 0x08 || c == 0x7F)
        code = KC_BACKSPACE;
    else if (c == 0x1B)
        code = ESCAPE;
#ifdef ANGBAND_SERIAL_SCREEN
    else if (c == 0x1C)
    {
        // PORT: picocalc-device-harness stage 045. A screen dump request, not a key.
        // Its own branch ahead of the Ctrl range for the reason the 0x0A branch above
        // gives, even though 0x1C is outside 0x01..0x1A and would fall to the drop at
        // the bottom: the order is the thing that gets written wrong.
        //
        // It returns FALSE. A dump is not a keypress, so check_events() must not set
        // `got` for it -- setting it would end a waiting check_events() early and
        // change when the game runs, which is the bug class specifications.md 8.3's
        // note was written about.
        dump_screen();
        return false;
    }
#endif
    else if (c >= 0x01 && c <= 0x1A)
    {
        // 0x08, 0x09, 0x0A and 0x0D were all taken above -- the spec's "except the four
        // above" -- so what is left here is Ctrl-A .. Ctrl-Z. Ctrl follows push_key()'s
        // rule and sets one of the two forms, never both; the modifier branch cannot be
        // reached from this range ('A'+c-1 is 0x41..0x5A) and is here because push_key()
        // is the model, not a special case.
        keycode_t u = (keycode_t)('A' + c - 1);

        if (!ENCODE_KTRL(u))
        {
            Term_keypress(u, KC_MOD_CONTROL);
            return true;
        }

        code = KTRL(u);
    }
    else
    {
        // 0x1C is here too when ANGBAND_SERIAL_SCREEN is off, which is what makes a
        // driver that sends it harmless against a build without the mirror.
        return false; // 0x00, 0x1C-0x1F, 0x80-0xFF
    }

    Term_keypress(code, 0);
    return true;
}
#endif
// -----------------------------------------------------------------------------------------

/**
 * Drain the south bridge into the term's key queue.
 *
 * Returns 0 if at least one key was pushed, 1 if not -- the convention TERM_XTRA_EVENT
 * wants. With wait set it does not return until something arrives.
 */
static errr check_events(bool wait)
{
    kbd_event_t e;
    bool got = false;

    // PORT: stage 070. This loop is the only place the program can block on the keyboard,
    // so it is the only place a turn's wall clock can contain time that is not the game's
    // fault. turnlog_end() subtracts what is reported here. With ANGBAND_TURN_LOG unset
    // both macros are ((void)0).
    TURNLOG_MARK(tl_idle);

    for (;;)
    {
        keyboard_poll();

        while (keyboard_next(&e))
        {
            push_key(&e);
            got = true;
        }

#ifdef ANGBAND_SERIAL_KEYS
        // PORT: picocalc-device-harness stage 030. Inside the idle bracket by design: an
        // injected key must land in the same subtracted time a typed one does, or the
        // instrument appears in the thing it measures. getchar_timeout_us(0) returns
        // PICO_ERROR_TIMEOUT at once when neither transport has a byte waiting.
        int c = getchar_timeout_us(0);

        if (c != PICO_ERROR_TIMEOUT && push_serial_key(c))
            got = true;
#endif

        if (got || !wait)
            break;

        sleep_ms(1);
    }

    if (wait)
        TURNLOG_IDLE(tl_idle);

    return got ? 0 : 1;
}

// ---------------------------------------------------------------------------------------

/**
 * The TERM_XTRA_* actions.
 *
 * Handled: EVENT, FLUSH, CLEAR, SHAPE, DELAY, REACT, FRESH.
 * Accepted and ignored: FROSH, NOISE, BORED, ALIVE, LEVEL.
 *
 * There is nothing else in ui-term.h's list -- the constants run 1..13 with 8 unused. NOISE
 * would be a beep and this board has no buzzer wired; ALIVE and LEVEL only matter to a front
 * end that shares a screen with other programs or drives more than one term, and this one
 * does neither. FROSH is never called: never_frosh is left false but ui-term.c only calls it
 * per row when a front end asks for it. BORED is switched off by never_bored.
 */
static errr Term_xtra_pico(int n, int v)
{
    switch (n)
    {
    case TERM_XTRA_EVENT:
        return check_events(v != 0);

    case TERM_XTRA_FLUSH:
    {
        kbd_event_t e;
        keyboard_poll();
        while (keyboard_next(&e))
            ;
        return 0;
    }

    case TERM_XTRA_CLEAR:
    {
        // ui-term.c calls this from exactly one place: the total_erase branch of
        // Term_fresh(). That branch then resets every cell of `old` to a white space and
        // redraws every row -- but Term_fresh_row_text() skips a cell whose `scr` contents
        // equal its `old` contents, so a cell that holds a WHITE SPACE in scr is never
        // repainted after a clear. It is relying on this hook having blanked it; that is
        // what ui-term.c:1624's "must erase the entire screen" is for. Every other cell IS
        // repainted, as text (any glyph, or a space in a non-white attribute) or wiped (a
        // space in COLOUR_DARK).
        //
        // Stage 040 painted the whole panel here and the redraw then painted it again --
        // 115 ms of every clearing frame. Stage 045 paints exactly the cells the redraw will
        // skip: the runs of white spaces in scr, read straight out of the term. On a mostly
        // blank screen that is most of the panel and the redraw is small; on a full screen
        // it is nothing and the redraw is everything. Either way the panel is painted once.
        //
        // Should a later stage shrink the term so it no longer covers the panel (it does now:
        // 64 * 5 == 32 * 10 == 320), the margin outside the grid has to be blanked as well.
        uint64_t t0 = time_us_64();
        const uint16_t black = colour_rgb565[COLOUR_DARK];
        uint32_t cells = 0;

        cursor_forget();

        // Each row is copied out of PSRAM in two bursts before it is scanned, for the
        // reason Term_text_pico gives: fill_cells() below runs from flash and writes
        // SRAM, so scanning PSRAM cell by cell between calls alternates chip selects.
        static int scan_a[PICO_TERM_COLS];

        for (int y = 0; y < PICO_TERM_ROWS; y++)
        {
            int x = 0;

            memcpy(scan_a, Term->scr->a[y], sizeof(scan_a));
            memcpy(span_cp, Term->scr->c[y], sizeof(span_cp));

            while (x < PICO_TERM_COLS)
            {
                if (span_cp[x] != L' ' || scan_a[x] != COLOUR_WHITE)
                {
                    x++;
                    continue;
                }
                int x0 = x;
                while (x < PICO_TERM_COLS && span_cp[x] == L' ' && scan_a[x] == COLOUR_WHITE)
                    x++;
                fill_cells(black, x0, y, x - x0, 1);
                cells += (uint32_t)(x - x0);
                paint_calls++;
            }
        }

        if (PICO_TERM_COLS * PICO_CELL_W < WIDTH)
            lcd_solid_rectangle(black, PICO_TERM_COLS * PICO_CELL_W, 0,
                                WIDTH - PICO_TERM_COLS * PICO_CELL_W, HEIGHT);
        if (PICO_TERM_ROWS * PICO_CELL_H < HEIGHT)
            lcd_solid_rectangle(black, 0, PICO_TERM_ROWS * PICO_CELL_H,
                                WIDTH, HEIGHT - PICO_TERM_ROWS * PICO_CELL_H);

        paint_us += (uint32_t)(time_us_64() - t0);
        paint_cells += cells;
        return 0;
    }

    case TERM_XTRA_SHAPE:
        // v == 0 hides the cursor. ui-term.c calls this instead of the cursor hook when the
        // cursor becomes invisible, so it is the only chance to rub the underline out.
        if (!v)
            cursor_erase();
        return 0;

    case TERM_XTRA_DELAY:
        // PORT: stage 070. A deliberate visual pause the game asked for, not computation.
        // It is counted as idle so a turn that animates does not read as a slow turn.
        if (v > 0)
        {
            TURNLOG_MARK(tl_delay);
            sleep_ms((uint32_t)v);
            TURNLOG_IDLE(tl_delay);
        }
        return 0;

    case TERM_XTRA_REACT:
        build_colour_table();
        row_px_valid = false;
        return 0;

    case TERM_XTRA_FRESH:
        // Nothing to flush: lcd_blit writes straight down the SPI bus. Used only to close
        // off the per-frame counters.
        last_fresh_us = paint_us;
        last_fresh_blit_us = paint_blit_us;
        last_fresh_read_us = paint_read_us;
        last_fresh_calls = paint_calls;
        last_fresh_cells = paint_cells;
        paint_us = 0;
        paint_blit_us = 0;
        paint_read_us = 0;
        paint_calls = 0;
        paint_cells = 0;
        return 0;

    case TERM_XTRA_FROSH:
    case TERM_XTRA_NOISE:
    case TERM_XTRA_BORED:
    case TERM_XTRA_ALIVE:
    case TERM_XTRA_LEVEL:
        return 0;
    }

    return 1;
}

// ---------------------------------------------------------------------------------------

static void Term_init_pico(term *t)
{
    (void)t;
}

static void Term_nuke_pico(term *t)
{
    (void)t;
}

static void term_data_link(void)
{
    term_data *td = &data[0];
    term *t = &td->t;

    memset(td, 0, sizeof(*td));

    term_init(t, PICO_TERM_COLS, PICO_TERM_ROWS, 256);

    // No hardware cursor on this panel: the front end draws it.
    t->soft_cursor = true;

    // Nothing useful to do when the game is idle, and TERM_XTRA_BORED on a board just burns
    // battery polling a keyboard that TERM_XTRA_EVENT already polls.
    t->never_bored = true;

    // Stage 060. The sidebar goes on top, not down the left.
    //
    // SIDEBAR_LEFT costs 13 columns of every row and leaves the map 51x30 = 1,530 cells;
    // SIDEBAR_TOP costs three rows plus the status line and leaves it 64x27 = 1,728, and
    // the 64 columns matter more than the cells because the dungeon is 198 wide and the
    // panel scrolls by half a screen. SIDEBAR_NONE is out on the source alone:
    // ui-display.c's update_sidebar() returns immediately for it, so HP, mana, depth and
    // speed are not drawn anywhere, which the stage plan requires to be visible.
    //
    // The three-row top bar is a PORT: edit in ui-display.c, conditional on Term->wid < 80;
    // upstream's two-row version overflows 64 columns as soon as a stat reaches 18/100.
    // The options menu ('=' then 'o') still cycles all three modes, and save.c/load.c
    // still carry the choice in the savefile.
    t->sidebar_mode = SIDEBAR_TOP;

    t->init_hook = Term_init_pico;
    t->nuke_hook = Term_nuke_pico;

    t->xtra_hook = Term_xtra_pico;
    t->curs_hook = Term_curs_pico;
    t->wipe_hook = Term_wipe_pico;
    t->text_hook = Term_text_pico;

    // bigcurs_hook, pict_hook, view_map_hook and dblh_hook stay NULL: no tiles, no
    // double-height glyphs, no big cursor. ui-term.c falls back to curs_hook for the big
    // cursor on its own (Term_activate does it).

    t->data = (void *)td;

    Term_activate(t);

    angband_term[0] = t;
}

errr init_pico(void)
{
    if (pico_initialised)
        return 0;

    lcd_init();
    lcd_set_font(&font_5x10);
    lcd_enable_cursor(false);

    keyboard_init();
    pico_utf8_init();

    memcpy(font_sram, font_5x10.glyphs, sizeof(font_sram));

    build_colour_table();
    row_px_valid = false;

    lcd_set_background(colour_rgb565[COLOUR_DARK]);
    lcd_set_foreground(colour_rgb565[COLOUR_WHITE]);
    lcd_clear_screen();
    cursor_forget();

    term_data_link();

    pico_initialised = true;
    return 0;
}
