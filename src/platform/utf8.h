// PORT: port-written for angband-pico, stage 040.
//
// The text-encoding half of the front end. specifications.md 4 (text encoding) and 6.4.
//
// Upstream's src/main.c refuses to start unless setlocale() finds a UTF-8 locale, and then
// leans on the C library's mbstowcs()/wctomb()/iswprint(). This port replaces main.c, and
// newlib on a bare-metal target has only the C locale, where mbstowcs() is byte-for-byte --
// so "Osse" with a diaeresis would arrive at Term_text as two wide characters and paint two
// wrong glyphs. z-util.c exposes four function pointers for exactly this case; utf8.c fills
// all four and pico_utf8_init() installs them.
//
// The fourth one, text_wcsz_hook, is not in the stage plan's list. It is installed anyway:
// text_wcsz() otherwise returns MB_LEN_MAX, and every caller (obj-randart.c, ui-command.c,
// ui-player.c, z-textblock.c, ...) sizes a malloc with it before calling text_wctomb(). It
// happens to be 8 in this newlib (newlib.h: _MB_LEN_MAX 8) and so happens to be safe, but
// the safety is a coincidence of the toolchain rather than a property of this code. The
// hook makes it 4, which is what pico_text_wctomb() can actually emit.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // The glyph drawn for a code point the 5x10 font cannot represent: font index 0x01, a
    // filled diamond. 0x7F is blank in this font and would be indistinguishable from a
    // space, which is the one thing a fallback glyph must not be.
#define PICO_FALLBACK_GLYPH 0x01

    // U+FFFD REPLACEMENT CHARACTER. What a malformed byte decodes to; see utf8.c.
#define PICO_UTF8_REPLACEMENT 0xFFFDu

    // Installs text_mbcs_hook, text_wctomb_hook, text_wcsz_hook and text_iswprint_hook.
    // init_pico() calls this; nothing else needs to.
    void pico_utf8_init(void);

    // The four hook bodies, exposed so a diagnostic can call them directly.
    size_t pico_text_mbcs(wchar_t *dest, const char *src, int n);
    int pico_text_wctomb(char *s, wchar_t wc);
    int pico_text_wcsz(void);
    int pico_text_iswprint(wint_t wc);

    // Code point -> 5x10 font index, or -1 when the font has nothing for it.
    //
    // The font is 128 glyphs, indexed by ASCII code. Latin-1 letters are folded onto their
    // unaccented ASCII base -- "Osse" with a diaeresis draws as four cells reading "Osse"
    // rather than three letters and a lozenge -- because every non-ASCII code point in
    // lib/gamedata is a Latin-1 accented vowel in a proper name (specifications.md 6.4
    // lists them). Anything else returns -1 and the caller draws PICO_FALLBACK_GLYPH.
    int pico_utf8_glyph(uint32_t cp);

#ifdef __cplusplus
}
#endif
