/**
 * \file z-bitflag.h
 * \brief Low-level bit vector manipulation
 *
 * Copyright (c) 2009 William L Moore
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#ifndef INCLUDED_Z_BITFLAG_H
#define INCLUDED_Z_BITFLAG_H

#include "h-basic.h"
#include "z-form.h"
#include "z-virt.h"

/* The basic datatype of bitflags */
typedef uint8_t bitflag;
#define FLAG_WIDTH        (sizeof(bitflag)*8)

/**
 * Enum flag value of the first valid flag in a set
 * Enums must be manually padded with the number of dummy elements
 */
#define FLAG_START        1

/**
 * Sentinel value indicates no more flags present for va-arg functions
 */
#define FLAG_END          (FLAG_START - 1)

/**
 * The array size necessary to hold "n" flags
 */
#define FLAG_SIZE(n)      (((n) + FLAG_WIDTH - 1) / FLAG_WIDTH)

/**
 * The highest flag value plus one in an array of size "n"
 */
#define FLAG_MAX(n)       (int)((n) * FLAG_WIDTH + FLAG_START)

/**
 * Convert a sequential flag enum value to its array index
 */
#define FLAG_OFFSET(id)   (((id) - FLAG_START) / FLAG_WIDTH)

/**
 * Convert a sequential flag enum value to its binary flag value.
 */
#define FLAG_BINARY(id)   (1 << ((id) - FLAG_START) % FLAG_WIDTH)


/*
 * PORT: angband-picocalc stage 075 item 3, 2026-09-13. flag_has(), flag_on() and flag_off()
 * are static inline here and no longer defined in z-bitflag.c. The core has no LTO, so every
 * sqinfo_has() in the SRAM hot loops was an out-of-line call into another translation unit
 * (93 M calls in the host replay's profile). Bodies unchanged, asserts kept; nothing in the
 * tree takes their address, so no out-of-line copy is needed.
 */
static inline bool flag_has(const bitflag *flags, const size_t size, const int flag)
{
	const size_t flag_offset = FLAG_OFFSET(flag);
	const int flag_binary = FLAG_BINARY(flag);

	(void)size;
	if (flag == FLAG_END) return false;

	assert(flag_offset < size);

	if (flags[flag_offset] & flag_binary) return true;

	return false;
}

static inline bool flag_on(bitflag *flags, const size_t size, const int flag)
{
	const size_t flag_offset = FLAG_OFFSET(flag);
	const int flag_binary = FLAG_BINARY(flag);

	(void)size;
	assert(flag_offset < size);

	if (flags[flag_offset] & flag_binary) return false;

	flags[flag_offset] |= flag_binary;

	return true;
}

static inline bool flag_off(bitflag *flags, const size_t size, const int flag)
{
	const size_t flag_offset = FLAG_OFFSET(flag);
	const int flag_binary = FLAG_BINARY(flag);

	(void)size;
	assert(flag_offset < size);

	if (!(flags[flag_offset] & flag_binary)) return false;

	flags[flag_offset] &= ~flag_binary;

	return true;
}

int  flag_next      (const bitflag *flags, const size_t size, const int flag);
int  flag_count     (const bitflag *flags, const size_t size);
bool flag_is_empty  (const bitflag *flags, const size_t size);
bool flag_is_full   (const bitflag *flags, const size_t size);
bool flag_is_inter  (const bitflag *flags1, const bitflag *flags2,
					 const size_t size);
bool flag_is_subset (const bitflag *flags1, const bitflag *flags2,
					 const size_t size);
bool flag_is_equal  (const bitflag *flags1, const bitflag *flags2,
					 const size_t size);
void flag_wipe      (bitflag *flags, const size_t size);
void flag_setall    (bitflag *flags, const size_t size);
void flag_negate    (bitflag *flags, const size_t size);
void flag_copy      (bitflag *flags1, const bitflag *flags2, const size_t size);
bool flag_union     (bitflag *flags1, const bitflag *flags2, const size_t size);
bool flag_inter     (bitflag *flags1, const bitflag *flags2, const size_t size);
bool flag_diff      (bitflag *flags1, const bitflag *flags2, const size_t size);

bool flags_test     (const bitflag *flags, const size_t size, ...);
bool flags_test_all (const bitflag *flags, const size_t size, ...);
bool flags_clear    (bitflag *flags, const size_t size, ...);
bool flags_set      (bitflag *flags, const size_t size, ...);
void flags_init     (bitflag *flags, const size_t size, ...);
bool flags_mask     (bitflag *flags, const size_t size, ...);

#ifdef NDEBUG
#define flag_has_dbg(flags, size, flag, fi, fl) flag_has(flags, size, flag)
#define flag_on_dbg(flags, size, flag, fi, fl) flag_on(flags, size, flag)
#else
bool flag_has_dbg   (const bitflag *flags, const size_t size, const int flag,
					 const char *fi, const char *fl);
bool flag_on_dbg    (bitflag *flags, const size_t size, const int flag,
					 const char *fi, const char *fl);
#endif

#endif
