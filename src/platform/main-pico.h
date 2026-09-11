// PORT: port-written for angband-pico, stage 040.
//
// The Angband "visual module" for the PicoCalc: one term of 64 columns by 32 rows on the
// 320x320 LCD in the vendored 5x10 font, keys from the south bridge through keyboard.c.
// specifications.md 6.4.
//
// This is the file upstream calls main-xxx.c. It carries the four Term hooks and nothing
// else; there is no framebuffer, because ui-term.c already keeps `old` and `scr` and only
// calls the hooks for spans that changed.

#pragma once

#include "h-basic.h"

#ifdef __cplusplus
extern "C"
{
#endif

// The geometry. specifications.md 4: 5x10 font, 64x32, chosen because the user rejected a
// 4 px wide font as unreadable. 64 * 5 == 32 * 10 == 320, the whole panel with no margin.
#define PICO_TERM_COLS 64
#define PICO_TERM_ROWS 32
#define PICO_CELL_W 5
#define PICO_CELL_H 10

    // Brings up the LCD, the keyboard and the text hooks, then creates the term and
    // activates it. Safe to call twice; the second call does nothing.
    //
    // On return Term, angband_term[0] and term_screen all point at the one term.
    errr init_pico(void);

    // The RGB565 the front end paints attribute `a` in. Exposed for diagnostics; the game
    // never needs it. `a` is an Angband attribute, so the background bits above MULT_BG are
    // masked off here exactly as text_hook does.
    uint16_t pico_term_colour(int a);

    // Per-frame counters, read after Term_fresh() (which ends with TERM_XTRA_FRESH, so
    // they describe the frame that just finished). Microseconds inside the three drawing
    // hooks (text, wipe, TERM_XTRA_CLEAR); microseconds of that inside lcd_blit(); hook
    // calls; cells painted. Diagnostics use them to separate the SPI transfer from glyph
    // expansion and from ui-term.c's own bookkeeping; nothing in the game does.
    uint32_t pico_term_last_fresh_us(void);
    uint32_t pico_term_last_fresh_blit_us(void);
    uint32_t pico_term_last_fresh_read_us(void);
    uint32_t pico_term_last_fresh_calls(void);
    uint32_t pico_term_last_fresh_cells(void);

#ifdef __cplusplus
}
#endif
