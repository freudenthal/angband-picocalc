// PORT: port-written for angband-pico, stage 070. See turnlog.h for what this is and why
// the declarations live in a header src/game/ can reach.
//
// THE INSTRUMENT MUST NOT APPEAR IN THE THING IT MEASURES. Three rules follow from that
// and all three are visible below.
//
//   1. Nothing on the hot path formats, divides or allocates. turnlog_acc() is one
//      time_us_64(), one subtract and one add into a static array in .bss, which is SRAM.
//      No QMI traffic at all.
//   2. The printf is in turnlog_end(), which ui-game.c calls AFTER run_game_loop() has
//      returned. The stage plan's warning is the reason: the SDK's
//      PICO_STDIO_USB_STDOUT_TIMEOUT_US is 500,000, so a terminal that has stopped draining
//      injects half-second spikes. Outside the bracket they land in no phase.
//   3. Time spent waiting for a key is subtracted, not included. main-pico.c's
//      check_events(wait) is the only place this program can block on the keyboard, and
//      TERM_XTRA_DELAY is the only place it deliberately sleeps; both report through
//      turnlog_idle().
//
// This file is compiled into the `angband` target unconditionally. Without
// ANGBAND_TURN_LOG it is an empty translation unit, which is what keeps it in
// tools/port-warnings.sh's list and keeps the option honest: compile-sweep's
// text/data/bss must not move when the option is off.

#include "turnlog.h"

#ifdef ANGBAND_TURN_LOG

#include <stdio.h>

#include "pico/stdlib.h"

#include "main-pico.h"
#include "psram_heap.h"

// The per-turn accumulators. .bss, so SRAM, so reading and writing them costs 93-113 ns
// and not the 6,297-7,949 ns specifications.md 6.4 prices a flash/PSRAM alternation at.
static uint64_t phase_us[TURNLOG_PHASE_MAX];
static uint64_t turn_t0;
static uint64_t idle_us;
static uint32_t record_n;
static bool header_done;

// Stage 075: the union of every closed bracket window this turn, idle included. A window
// that closes sets it to (claimed at its open) + (its length), which absorbs everything
// that closed inside it, so nested and sibling brackets are each counted once. A bracket
// whose close is skipped (an early return) leaves its window unclaimed, so it shows in rst.
static uint64_t claimed_us;

turnlog_mark_t turnlog_mark(void)
{
	turnlog_mark_t m;

	m.t0 = time_us_64();
	m.claimed = claimed_us;
	return m;
}

// Stage 070 semantics: inclusive of anything bracketed inside it.
void turnlog_acc(int phase, turnlog_mark_t m)
{
	uint64_t e = time_us_64() - m.t0;

	if (phase >= 0 && phase < TURNLOG_PHASE_MAX)
		phase_us[phase] += e;
	claimed_us = m.claimed + e;
}

// Stage 075 semantics: self time, less every window that closed inside this one.
void turnlog_self(int phase, turnlog_mark_t m)
{
	uint64_t e = time_us_64() - m.t0;
	uint64_t inner = claimed_us - m.claimed;

	if (phase >= 0 && phase < TURNLOG_PHASE_MAX)
		phase_us[phase] += (e > inner) ? e - inner : 0;
	claimed_us = m.claimed + e;
}

void turnlog_idle(turnlog_mark_t m)
{
	uint64_t e = time_us_64() - m.t0;

	idle_us += e;
	claimed_us = m.claimed + e;
}

void turnlog_begin(void)
{
	int i;

	for (i = 0; i < TURNLOG_PHASE_MAX; i++)
		phase_us[i] = 0;

	idle_us = 0;
	claimed_us = 0;
	turn_t0 = time_us_64();
}

/**
 * Close the record and print it. Everything expensive in this function is deliberate and
 * is outside every bracket.
 *
 * The format is one header line and then one record per command, space separated, so that
 * tools/turnlog.py can read it and so that a human watching a terminal can see the shape
 * without a tool. Every time is MICROSECONDS; brk is KB.
 *
 *   n     record number, +1 every command. A gap means a dropped line, which is the only
 *         thing that can quietly invalidate a capture -- hence the acceptance criterion
 *         asking for unbroken numbering.
 *   turn  the game's own turn counter. It moves by 10 per player step at normal speed and
 *         does not move at all for a command that used no energy.
 *   d     player->depth. 0 is the town.
 *   tot   the whole command, less idle: wall clock from just before run_game_loop() to
 *         just after, minus every microsecond spent blocked on the keyboard or asleep in
 *         TERM_XTRA_DELAY.
 *   was lit upd fgn mkn scn trp   the seven sweeps.
 *   mon   process_monsters() plus reset_monsters(), all call sites.
 *   wld   process_world(), which CONTAINS fgn, mkn, scn and trp.
 *   gen   prepare_next_level(), normally 0.
 *   frm   pico_term_last_fresh_us(), the last Term_fresh() in this command. Stage 045 put
 *         a full redraw at 68 ms and a clearing frame at 81 ms; a plain step changes a
 *         handful of cells, so this is the number that says whether the panel is a
 *         rounding error, and the stage plan says to confirm that once and stop thinking
 *         about it.
 *   idle  what was subtracted. A sanity figure: on a held key it is near zero.
 *   brk   psram_heap_used() / 1024. This is the endurance criterion -- heap after 1,000
 *         turns against heap after 100 on the same level -- and it costs nothing here.
 *
 * Stage 075 appends, all SELF time (see turnlog.h):
 *   cln ply ntc bon upm pan map rdr evr anm kbp   the unbracketed turn, bracketed
 *   rst   the turn less the union of every bracket window and idle: what no bracket
 *         covers, exact however the brackets nest.
 */
void turnlog_end(int depth, int32_t game_turn)
{
	uint64_t now = time_us_64();
	uint64_t raw = now - turn_t0;
	uint64_t tot = (raw > idle_us) ? raw - idle_us : 0;
	uint64_t rst = (raw > claimed_us) ? raw - claimed_us : 0;

	if (!header_done) {
		printf("TLH n turn d tot was lit upd fgn mkn scn trp mon wld gen frm idle brk"
		       " cln ply ntc bon upm pan map rdr evr anm kbp rst\n");
		header_done = true;
	}

	record_n++;

	// Two printfs, one line: the stage 070 columns unchanged, then stage 075's.
	printf("TL %lu %ld %d %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %u",
	       (unsigned long)record_n,
	       (long)game_turn,
	       depth,
	       (unsigned long)tot,
	       (unsigned long)phase_us[TURNLOG_WASSEEN],
	       (unsigned long)phase_us[TURNLOG_LIGHTING],
	       (unsigned long)phase_us[TURNLOG_UPDVIEW],
	       (unsigned long)phase_us[TURNLOG_FORGET],
	       (unsigned long)phase_us[TURNLOG_NOISE],
	       (unsigned long)phase_us[TURNLOG_SCENT],
	       (unsigned long)phase_us[TURNLOG_TRAPS],
	       (unsigned long)phase_us[TURNLOG_MONSTERS],
	       (unsigned long)phase_us[TURNLOG_WORLD],
	       (unsigned long)phase_us[TURNLOG_GEN],
	       (unsigned long)pico_term_last_fresh_us(),
	       (unsigned long)idle_us,
		       (unsigned)(psram_heap_used() / 1024u));
	printf(" %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu\n",
	       (unsigned long)phase_us[TURNLOG_CLEANUP],
	       (unsigned long)phase_us[TURNLOG_PLAYER],
	       (unsigned long)phase_us[TURNLOG_NOTICE],
	       (unsigned long)phase_us[TURNLOG_BONUS],
	       (unsigned long)phase_us[TURNLOG_UPDMON],
	       (unsigned long)phase_us[TURNLOG_PANEL],
	       (unsigned long)phase_us[TURNLOG_MAP],
	       (unsigned long)phase_us[TURNLOG_REDRAW],
	       (unsigned long)phase_us[TURNLOG_REFRESH],
	       (unsigned long)phase_us[TURNLOG_ANIMATE],
	       (unsigned long)phase_us[TURNLOG_KBPOLL],
	       (unsigned long)rst);
}

#else /* !ANGBAND_TURN_LOG */

// An ISO C translation unit needs at least one declaration. This one costs nothing and
// keeps the file in tools/port-warnings.sh's list whichever way the option is set.
typedef int turnlog_not_compiled_in;

#endif /* ANGBAND_TURN_LOG */
