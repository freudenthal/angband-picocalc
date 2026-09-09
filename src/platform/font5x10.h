// PORT: vendored 2026-08-29 into tinyrogue-pico by stage 020.
//   Copied from Picoware/src/SDK/Picoware/src/system/drivers/font.h at Picoware 841d9c56.
//   Upstream of that copy: https://github.com/BlairLeduc/picocalc-text-starter
//   The paired header in that tree carries "Author: Blair Leduc / License: MIT License".
//   Picoware's own repository LICENSE is GPL-3.0; these driver files are not covered by
//   it, they are a vendored MIT component. See specifications.md 12.
//   Local changes are marked "PORT:" inline. Nothing else is edited.
//
/*
Author: Blair Leduc
License: MIT License
Source: https://github.com/BlairLeduc/picocalc-text-starter
*/

#pragma once

#include <pico/stdlib.h>

#define GLYPH_HEIGHT 10 // Height of each glyph in pixels

typedef struct
{
    uint8_t width;
    uint8_t glyphs[];
} font_t;

// PORT: font_8x10 is not vendored. The port uses 5x10 only, for 64 columns
// (specifications.md 8), so font-8x10.c was left behind.
extern const font_t font_5x10; // 5x10 pixel font
