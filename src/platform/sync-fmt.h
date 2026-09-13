// PORT: port-written for angband-pico, picocalc-device-harness stage 055. THE SYNC LINE,
// formatted. See sync.h for what the record is for and where it is emitted from.
//
// ONE FORMATTER, TWO SIDES, AND NO TRANSLATION UNIT OF ITS OWN
//
// This file is included by exactly one source on each side: src/platform/main-pico.c on the
// device, and src/host/main-borg.c in the PC-side borg build. Everything in it is `static`,
// so each side gets its own copy and nothing is exported. That is the whole arrangement:
// the device prints the line and the host writes it to a file, but the BYTES ARE PRODUCED BY
// THE SAME CODE, and two sides that could disagree about the format would make every
// comparison between them worthless.
//
// WHY A HEADER AND NOT A .c FILE, WHICH WOULD BE THE OBVIOUS THING
//
// Harness invariant 2: with every harness option OFF the game's .text must be BYTE-identical
// to the tree before the edit. A new .c file on the `angband` target breaks that even when
// it is an empty translation unit -- measured 2026-09-12 on exactly this code. The .text
// size did not move (965,744 B either way) and FLASH and RAM did not move, but 894 bytes
// differed, and a disassembly diff showed every one of them was a long-branch veneer:
// __memcpy_veneer moved from 0x1009c408 to 0x1009c3f8 and __memset_veneer from 0x1009c388
// to 0x1009c430. The linker places veneers after the input sections and one more object in
// the list is enough to shuffle them.
//
// That is not a triviality to wave through. Stage 070's own finding is that PLACEMENT
// MOVES THE MEASUREMENT: a 2,617-byte .text edit that touched nothing on the measured path
// moved a boot time by 17 %. An instrument that relocates two thunks in the binary it is
// supposed to be absent from is exactly the thing invariant 2 was written against.
//
// So the implementation lives in a file that is already in the link. sync.h's macro is
// ((void)0) with the option off, main-pico.c's #ifdef block compiles to nothing, and the
// object list does not change. Checked: .text byte-identical, cmp clean.

#ifndef INCLUDED_SYNC_FMT_H
#define INCLUDED_SYNC_FMT_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game-world.h"
#include "player.h"
#include "z-rand.h"

// The record number. Advances in sync_end() only, which is the only caller.
static uint32_t sync_n;

/**
 * CRC-32, IEEE 802.3, reflected, polynomial 0xEDB88320, initial and final xor 0xFFFFFFFF.
 *
 * The same one the stage 045 screen dump uses (harness specifications.md 8.5) and the same
 * one Python's zlib.crc32 computes, so every line that reaches the driver checks the
 * device's arithmetic for free. Bitwise and table-free: this runs twice per command,
 * outside the measured bracket, and a 1 KB table in flash would cost more than the eight
 * shifts per byte ever will.
 */
static uint32_t sync_crc32(uint32_t crc, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	size_t i;
	int b;

	crc = ~crc;
	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (b = 0; b < 8; b++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
		}
	}
	return ~crc;
}

/**
 * CRC32 over the RNG's complex-generator state: state_i, then all 32 words of STATE[],
 * each as four little-endian bytes.
 *
 * THIS IS THE FIELD THAT MATTERS. Angband's normal generator is the complex one --
 * load.c:393-415 sets Rand_quick = false after reading the savefile -- and in that mode
 * Rand_value does not move at all; it is the SIMPLE generator's seed, meaningful only
 * during flavour and randart generation. STATE[] is what advances, once per Rand_div()
 * call, so a mismatch here means the two sides have made a different number of RNG calls,
 * in a different order, or got different answers. Everything a game-state field could
 * catch, this catches first and usually several commands earlier.
 *
 * Byte order is pinned by hand rather than by a memcpy of the array, because this figure is
 * compared across two machines and "whatever the compiler laid out" is not a format. Both
 * are little-endian today; if one ever is not, this loop is why the number still matches.
 */
static uint32_t sync_rng_digest(void)
{
	unsigned char buf[4];
	uint32_t crc = 0;
	uint32_t w;
	int i;

	for (i = -1; i < RAND_DEG; i++) {
		w = (i < 0) ? state_i : STATE[i];
		buf[0] = (unsigned char)(w & 0xffu);
		buf[1] = (unsigned char)((w >> 8) & 0xffu);
		buf[2] = (unsigned char)((w >> 16) & 0xffu);
		buf[3] = (unsigned char)((w >> 24) & 0xffu);
		crc = sync_crc32(crc, buf, 4);
	}
	return crc;
}

/**
 * Format one SYNC line into buf, newline included. Returns its length, or -1.
 *
 *   SYNC 1042 y=31 x=104 chp=41 au=318 dl=1 t=3457 si=17 rv=8f3a91c2 rs=b21c04de d=4e7b1a05
 *
 *   1042   the record number. Advances once per completed player command, at the same call
 *          site as turnlog_end()'s own counter, so SYNC 1042 and TL 1042 are the same
 *          command and a capture carrying both can be paired on this field.
 *   y= x=  player->grid, chp= player->chp, au= player->au, dl= player->depth. These are
 *          not the check, they are what makes a divergence report readable by a human.
 *   t=     game-world.c's `turn`, the same field TL carries.
 *   si= rv= state_i and Rand_value, printed for the human; see sync_rng_digest() above.
 *   rs=    the RNG fingerprint. The strong field.
 *   d=     CRC32 over THE FIELD BLOCK ONLY -- "y=" to the end of "rs=xxxxxxxx", record
 *          number excluded -- so one comparison decides whether two lines describe the
 *          same state.
 *
 * THE RECORD NUMBER IS DELIBERATELY OUTSIDE `d`. The device counts commands from its boot
 * and the PC counts them from the first key of the keystream, and the two do not have to
 * agree: a session script that sends a key of its own before the stream starts shifts the
 * device's numbering for the rest of the run. Measured on the host side of exactly that on
 * 2026-09-12 -- starting the borg with Ctrl-Z completes one play_game() iteration of its
 * own, so that run carried one extra leading record while every field of all 1,000 later
 * records matched. A digest covering the number would have called that a divergence at
 * command 1, and it was not one.
 *
 * The field block is built first, then hashed, then the line is assembled -- so a parser
 * can check `d` by hashing the text between the record number and " d=" itself. No field
 * may contain a space for that to work, and none does.
 *
 * The line is NOT a hash of the whole game state. A whole-state hash would catch a little
 * more and diagnose nothing; these fields name what went wrong.
 */
static int sync_format(char *buf, size_t cap, uint32_t n, int32_t game_turn)
{
	char fields[128];
	int  flen, len;
	uint32_t d;

	flen = snprintf(fields, sizeof(fields),
		"y=%d x=%d chp=%d au=%ld dl=%d t=%ld si=%lu rv=%08lx rs=%08lx",
		(int)player->grid.y,
		(int)player->grid.x,
		(int)player->chp,
		(long)player->au,
		(int)player->depth,
		(long)game_turn,
		(unsigned long)state_i,
		(unsigned long)Rand_value,
		(unsigned long)sync_rng_digest());

	if (flen < 0 || (size_t)flen >= sizeof(fields)) return -1;

	d = sync_crc32(0, fields, (size_t)flen);

	len = snprintf(buf, cap, "SYNC %lu %s d=%08lx\n",
		(unsigned long)n, fields, (unsigned long)d);
	if (len < 0 || (size_t)len >= cap) return -1;

	return len;
}

#endif /* INCLUDED_SYNC_FMT_H */
