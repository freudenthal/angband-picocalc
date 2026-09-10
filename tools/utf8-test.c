/*
 * PORT: port-written for angband-pico, stage 040.
 *
 * Host-side unit checks for src/platform/utf8.c, run by tools/host-build.sh as part 2 of
 * the suite. It needs no device and no core: it stubs z-util.c's four hook pointers and
 * links utf8.c on its own.
 *
 * The decoder is the one piece of the front end whose failure mode is silent -- a wrong
 * wide-character count shortens or lengthens a line by a cell and nothing reports it -- so
 * it gets an automated test rather than an eyeball on the panel. specifications.md 6.6.
 *
 *   gcc -std=gnu99 -Isrc/platform -o utf8-test tools/utf8-test.c src/platform/utf8.c
 */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "utf8.h"

/* z-util.c's four hook pointers, stubbed so utf8.c links without the core. */
size_t (*text_mbcs_hook)(wchar_t *dest, const char *src, int n) = 0;
int (*text_wctomb_hook)(char *s, wchar_t wchar) = 0;
int (*text_wcsz_hook)(void) = 0;
int (*text_iswprint_hook)(wint_t wc) = 0;

static int fails;

static void ck(int ok, const char *what)
{
    if (!ok) { printf("FAIL %s\n", what); fails++; }
    else printf("ok   %s\n", what);
}

int main(void)
{
    wchar_t w[32];
    char b[32];
    size_t n;

    /* Counting mode. */
    ck(pico_text_mbcs(NULL, "Oss\xC3\xAB", 0) == 4, "count Osse+diaeresis == 4");
    ck(pico_text_mbcs(NULL, "Manw\xC3\xAB", 0) == 5, "count Manwe == 5");
    ck(pico_text_mbcs(NULL, "", 0) == 0, "count empty == 0");
    ck(pico_text_mbcs(NULL, "abc", 0) == 3, "count abc == 3");

    /* Convert with room to spare: NUL written, not counted. */
    memset(w, 0x7f, sizeof w);
    n = pico_text_mbcs(w, "Oss\xC3\xAB", 32);
    ck(n == 4, "convert returns 4");
    ck(w[0] == L'O' && w[1] == L's' && w[2] == L's' && w[3] == 0xEB, "code points");
    ck(w[4] == 0, "NUL written past the 4");

    /* Exactly n: no NUL. */
    memset(w, 0x7f, sizeof w);
    n = pico_text_mbcs(w, "abcd", 4);
    ck(n == 4 && w[3] == L'd' && w[4] == (wchar_t)0x7f7f7f7f, "n reached, no NUL");

    /* The ui-entry.c terminator idiom. */
    memset(w, 0x7f, sizeof w);
    n = pico_text_mbcs(w, "", 1);
    ck(n == 0 && w[0] == 0, "empty string writes one NUL, returns 0");

    /* parser.c's single-symbol idiom. */
    n = pico_text_mbcs(w, "*", 1);
    ck(n == 1 && w[0] == L'*', "single symbol");

    /* Malformed input never returns -1. */
    n = pico_text_mbcs(NULL, "a\xFF" "b", 0);
    ck(n == 3, "stray 0xFF counts as one replacement char");
    n = pico_text_mbcs(w, "a\xFF" "b", 32);
    ck(n == 3 && w[1] == 0xFFFD, "stray 0xFF decodes to U+FFFD");
    n = pico_text_mbcs(w, "a\xC3", 32);
    ck(n == 2 && w[1] == 0xFFFD, "truncated sequence at NUL, no overrun");
    n = pico_text_mbcs(w, "\xC0\xAF", 32);
    ck(n == 2 && w[0] == 0xFFFD && w[1] == 0xFFFD, "overlong slash rejected");
    n = pico_text_mbcs(w, "\xED\xA0\x80", 32);
    ck(n == 3, "surrogate rejected, resyncs one byte at a time");

    /* Three and four byte forms. */
    n = pico_text_mbcs(w, "\xE2\x98\x83", 32);
    ck(n == 1 && w[0] == 0x2603, "U+2603 three-byte");
    n = pico_text_mbcs(w, "\xF0\x9F\x82\xA1", 32);
    ck(n == 1 && w[0] == 0x1F0A1, "U+1F0A1 four-byte");

    /* wctomb. */
    ck(pico_text_wctomb(b, L'A') == 1 && b[0] == 'A', "wctomb ASCII");
    ck(pico_text_wctomb(b, (wchar_t)0xEB) == 2 &&
       (unsigned char)b[0] == 0xC3 && (unsigned char)b[1] == 0xAB, "wctomb U+00EB");
    ck(pico_text_wctomb(b, (wchar_t)0x2603) == 3, "wctomb U+2603");
    ck(pico_text_wctomb(b, (wchar_t)0x1F0A1) == 4, "wctomb U+1F0A1");
    ck(pico_text_wctomb(b, (wchar_t)0) == 1 && b[0] == 0, "wctomb 0");
    ck(pico_text_wctomb(b, (wchar_t)0xD800) == -1, "wctomb surrogate rejected");
    ck(pico_text_wcsz() == 4, "wcsz == 4");

    /* Round trip of every mapped code point. */
    {
        int bad = 0;
        for (unsigned cp = 1; cp < 0x110000u; cp++) {
            int nb;
            if (cp >= 0xD800u && cp <= 0xDFFFu) continue;
            nb = pico_text_wctomb(b, (wchar_t)cp);
            if (nb < 1) { bad++; continue; }
            b[nb] = 0;
            if (pico_text_mbcs(w, b, 32) != 1 || (unsigned)w[0] != cp) bad++;
        }
        ck(bad == 0, "wctomb/mbcs round trip over all of Unicode");
    }

    /* Glyph mapping. */
    ck(pico_utf8_glyph('A') == 'A', "glyph ASCII");
    ck(pico_utf8_glyph(0x1F) == -1, "glyph control char has none");
    ck(pico_utf8_glyph(0x7F) == -1, "glyph 0x7F has none");
    ck(pico_utf8_glyph(0xEB) == 'e', "glyph e-diaeresis folds to e");
    ck(pico_utf8_glyph(0xC9) == 'E', "glyph E-acute folds to E");
    ck(pico_utf8_glyph(0xA0) == ' ', "glyph NBSP folds to space");
    ck(pico_utf8_glyph(0x2603) == -1, "glyph snowman has none");

    /* iswprint. */
    ck(pico_text_iswprint('A') != 0, "iswprint A");
    ck(pico_text_iswprint(' ') != 0, "iswprint space");
    ck(pico_text_iswprint(0x1F) == 0, "iswprint control");
    ck(pico_text_iswprint(0xEB) != 0, "iswprint e-diaeresis");
    ck(pico_text_iswprint(0x2603) == 0, "iswprint snowman");

    /* Every non-ASCII code point the vendored lib/ tree contains must be drawable. */
    {
        static const unsigned used[] = {
            0x00A0, 0x00C9, 0x00E1, 0x00E2, 0x00E4, 0x00E9, 0x00EB,
            0x00ED, 0x00EE, 0x00F3, 0x00F4, 0x00F6, 0x00FA, 0x00FB
        };
        int bad = 0;
        for (unsigned i = 0; i < sizeof used / sizeof used[0]; i++)
            if (pico_utf8_glyph(used[i]) < 0) bad++;
        ck(bad == 0, "every code point in lib/ has a glyph");
    }

    printf("%s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
