// PORT: port-written for angband-pico, stage 040. See main-pico.h.
//
// Written after angband/src/main-nds.c -- upstream's own port to a small ARM handheld --
// and angband/src/main-xxx.c, the annotated template. Neither is vendored into this tree;
// both were read from angband/ next door.
//
// STACK NOTE (stage 020): the core-0 stack is the 2 KB the SDK puts in SCRATCH_Y. The span
// buffer below is 6,400 bytes and is therefore static, not automatic.

#include <string.h>

#include "pico/stdlib.h"

#include "font5x10.h"
#include "keyboard.h"
#include "lcd.h"
#include "main-pico.h"
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

    for (;;)
    {
        keyboard_poll();

        while (keyboard_next(&e))
        {
            push_key(&e);
            got = true;
        }

        if (got || !wait)
            break;

        sleep_ms(1);
    }

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
        if (v > 0)
            sleep_ms((uint32_t)v);
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
