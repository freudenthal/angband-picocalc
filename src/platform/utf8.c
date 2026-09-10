// PORT: port-written for angband-pico, stage 040. See utf8.h for why this file exists.

#include "utf8.h"

#include "z-util.h"

// ---------------------------------------------------------------------------------------
// Decoding one character.
//
// THE DECODER NEVER FAILS. mbstowcs() returns (size_t)-1 on a malformed sequence and
// Term_addstr() turns that into "return -1" -- which draws nothing at all and reports
// nothing, so a single stray byte in a data file would silently blank a line of the game
// and look exactly like a hang. A malformed byte becomes U+FFFD here instead, which
// text_hook draws as the fallback glyph: visible, local to the one cell, and impossible to
// mistake for correct output. specifications.md 6.4 records the deviation.
//
// Rejected as malformed: continuation bytes and 0xF8..0xFF as a lead byte, truncated
// sequences, overlong encodings, UTF-16 surrogates, and anything above U+10FFFF. On a
// reject the reader advances by exactly one byte, so it resynchronises on the next lead
// byte and can never run past the terminating NUL (a NUL fails the continuation test).

static const unsigned char *decode_one(const unsigned char *p, uint32_t *cp)
{
    unsigned char c = *p;

    if (c < 0x80)
    {
        *cp = c;
        return p + 1;
    }

    int extra;
    uint32_t v;
    uint32_t least;

    if ((c & 0xE0) == 0xC0)
    {
        extra = 1;
        v = (uint32_t)(c & 0x1F);
        least = 0x80u;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        extra = 2;
        v = (uint32_t)(c & 0x0F);
        least = 0x800u;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        extra = 3;
        v = (uint32_t)(c & 0x07);
        least = 0x10000u;
    }
    else
    {
        *cp = PICO_UTF8_REPLACEMENT;
        return p + 1;
    }

    for (int i = 0; i < extra; i++)
    {
        unsigned char cc = p[1 + i];
        if ((cc & 0xC0) != 0x80)
        {
            *cp = PICO_UTF8_REPLACEMENT;
            return p + 1;
        }
        v = (v << 6) | (uint32_t)(cc & 0x3F);
    }

    if (v < least || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu))
    {
        *cp = PICO_UTF8_REPLACEMENT;
        return p + 1;
    }

    *cp = v;
    return p + 1 + extra;
}

// ---------------------------------------------------------------------------------------
// text_mbcs_hook. mbstowcs() semantics, which the callers in ui-entry.c and parser.c rely
// on precisely:
//
//   * dest == NULL: count the wide characters the string would produce, ignore n.
//   * dest != NULL: write at most n wide characters. If the terminating NUL is reached
//     first it is written too and is NOT counted in the return value; if n is reached
//     first, no NUL is written and n is returned.
//
// ui-entry.c calls it as text_mbstowcs(p, "", 1) purely to append a terminator and checks
// only for (size_t)-1, so the empty-string case has to return 0 and write one NUL.

size_t pico_text_mbcs(wchar_t *dest, const char *src, int n)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t count = 0;
    uint32_t cp;

    if (!dest)
    {
        while (*p)
        {
            p = decode_one(p, &cp);
            count++;
        }
        return count;
    }

    if (n <= 0)
        return 0;

    while (count < (size_t)n)
    {
        if (!*p)
        {
            dest[count] = (wchar_t)0;
            return count;
        }
        p = decode_one(p, &cp);
        dest[count++] = (wchar_t)cp;
    }

    return count;
}

// ---------------------------------------------------------------------------------------
// text_wctomb_hook. One code point to UTF-8. Returns the byte count, or -1 for a code point
// that cannot be encoded. z-util.c's contract: the buffer is text_wcsz() bytes and the
// result is not NUL-terminated, except for wc == 0 which writes a single NUL byte.

int pico_text_wctomb(char *s, wchar_t wc)
{
    uint32_t v = (uint32_t)wc;

    if (v == 0)
    {
        s[0] = '\0';
        return 1;
    }
    if (v < 0x80u)
    {
        s[0] = (char)v;
        return 1;
    }
    if (v < 0x800u)
    {
        s[0] = (char)(0xC0u | (v >> 6));
        s[1] = (char)(0x80u | (v & 0x3Fu));
        return 2;
    }
    if (v >= 0xD800u && v <= 0xDFFFu)
        return -1;
    if (v < 0x10000u)
    {
        s[0] = (char)(0xE0u | (v >> 12));
        s[1] = (char)(0x80u | ((v >> 6) & 0x3Fu));
        s[2] = (char)(0x80u | (v & 0x3Fu));
        return 3;
    }
    if (v <= 0x10FFFFu)
    {
        s[0] = (char)(0xF0u | (v >> 18));
        s[1] = (char)(0x80u | ((v >> 12) & 0x3Fu));
        s[2] = (char)(0x80u | ((v >> 6) & 0x3Fu));
        s[3] = (char)(0x80u | (v & 0x3Fu));
        return 4;
    }

    return -1;
}

// text_wcsz_hook. The largest pico_text_wctomb() result. See the note in utf8.h.
int pico_text_wcsz(void)
{
    return 4;
}

// ---------------------------------------------------------------------------------------
// Code point to font index.
//
// The 5x10 font is 128 glyphs indexed by ASCII code (font5x10.c runs 0x00..0x7F). 0x20..0x7E
// are the printable ASCII glyphs. Everything above is folded here.
//
// The Latin-1 letters fold to their unaccented base. Every non-ASCII code point in
// lib/gamedata is one of these -- 14 distinct ones, all accented vowels in proper names
// (Osse, Manwe, Grond of Feanor) plus one non-breaking space -- so folding turns what would
// be a row of lozenges into readable, if unaccented, names. The three punctuation entries
// below are not used by the vendored lib/ tree; they are there because a future help file
// with a curly quote in it should not paint a lozenge.

int pico_utf8_glyph(uint32_t cp)
{
    if (cp >= 0x20u && cp < 0x7Fu)
        return (int)cp;

    switch (cp)
    {
    // C1 / Latin-1 punctuation.
    case 0x00A0u: return ' ';   // NO-BREAK SPACE -- 19 of them, in flavor.txt and trap.txt
    case 0x00ABu: return '<';
    case 0x00BBu: return '>';
    case 0x00D7u: return 'x';
    case 0x00F7u: return '/';

    // Latin-1 capitals.
    case 0x00C0u: case 0x00C1u: case 0x00C2u: case 0x00C3u:
    case 0x00C4u: case 0x00C5u: return 'A';
    case 0x00C6u: return 'A';   // AE ligature; one cell, so the A half
    case 0x00C7u: return 'C';
    case 0x00C8u: case 0x00C9u: case 0x00CAu: case 0x00CBu: return 'E';
    case 0x00CCu: case 0x00CDu: case 0x00CEu: case 0x00CFu: return 'I';
    case 0x00D0u: return 'D';
    case 0x00D1u: return 'N';
    case 0x00D2u: case 0x00D3u: case 0x00D4u: case 0x00D5u:
    case 0x00D6u: case 0x00D8u: return 'O';
    case 0x00D9u: case 0x00DAu: case 0x00DBu: case 0x00DCu: return 'U';
    case 0x00DDu: return 'Y';
    case 0x00DEu: return 'P';   // thorn
    case 0x00DFu: return 's';   // sharp s

    // Latin-1 lower case.
    case 0x00E0u: case 0x00E1u: case 0x00E2u: case 0x00E3u:
    case 0x00E4u: case 0x00E5u: return 'a';
    case 0x00E6u: return 'a';
    case 0x00E7u: return 'c';
    case 0x00E8u: case 0x00E9u: case 0x00EAu: case 0x00EBu: return 'e';
    case 0x00ECu: case 0x00EDu: case 0x00EEu: case 0x00EFu: return 'i';
    case 0x00F0u: return 'd';
    case 0x00F1u: return 'n';
    case 0x00F2u: case 0x00F3u: case 0x00F4u: case 0x00F5u:
    case 0x00F6u: case 0x00F8u: return 'o';
    case 0x00F9u: case 0x00FAu: case 0x00FBu: case 0x00FCu: return 'u';
    case 0x00FDu: case 0x00FFu: return 'y';
    case 0x00FEu: return 'p';

    // General punctuation a help file might carry.
    case 0x2010u: case 0x2011u: case 0x2012u:
    case 0x2013u: case 0x2014u: return '-';
    case 0x2018u: case 0x2019u: return '\'';
    case 0x201Cu: case 0x201Du: return '"';

    default: return -1;
    }
}

// ---------------------------------------------------------------------------------------
// text_iswprint_hook. ui-output.c is the only caller: it substitutes a space for anything
// this rejects. So "printable" here means "the font can draw it as itself" -- a code point
// that would come out as the fallback lozenge is better shown as a space in flowing text.

int pico_text_iswprint(wint_t wc)
{
    return pico_utf8_glyph((uint32_t)wc) >= 0;
}

// ---------------------------------------------------------------------------------------

void pico_utf8_init(void)
{
    text_mbcs_hook = pico_text_mbcs;
    text_wctomb_hook = pico_text_wctomb;
    text_wcsz_hook = pico_text_wcsz;
    text_iswprint_hook = pico_text_iswprint;
}
