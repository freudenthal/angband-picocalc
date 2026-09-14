// PORT: port-written for angband-pico, stage 070. The turn-latency instrument.
//
// WHAT THIS IS
//
// One line of serial per player command, giving where the time went. Stage 070's whole
// analysis -- that a player step drives seven complete sweeps of the 13,068-grid level and
// that every grid access in them is a four-deep pointer chase from flash-resident code into
// PSRAM -- is a source read and not a measurement. This file is what turns it into one.
//
// It is step A of the stage plan's item 1: time_us_64() pairs around the seven sweeps plus
// the whole turn, one printf per turn, outside the measured window. The durable version
// (an SRAM ring drained to the card) is step B and is deliberately NOT built until step A
// has been shown to be inadequate on the device.
//
// WHY THE DECLARATIONS ARE HERE AND THE DEFINITIONS ARE IN src/platform/
//
// The stage 045 correction: a core source cannot include an SDK header, because
// tools/compile-sweep.sh compiles src/game/ with no SDK include path. So this header must
// compile under gnu99 with nothing but <stdint.h>, and every use of time_us_64(), printf()
// and the SDK lives in turnlog.c next door. tools/compile-sweep.sh gains
// -I src/platform for this one header; see the note in that script.
//
// WHY IT IS ALL MACROS
//
// ANGBAND_TURN_LOG is a CMake option, OFF in the shipped build. With it off every macro
// below is ((void)0) and turnlog.c compiles to an empty object, so the instrument is not
// in the thing it measures. The check that this is true is tools/compile-sweep.sh's
// text/data/bss line, which must not move.
//
// READ THIS BEFORE COMPARING TWO NUMBERS
//
// The logged build and the shipped build are different binaries and their ABSOLUTE numbers
// are not comparable. Stage 060 moved a boot time by 17 % with a 2,617-byte .text edit that
// touched nothing on the measured path (specifications.md 12), so the instrument's own
// footprint is worth more than several of the wins this stage is chasing. Use the logged
// build for RELATIVE comparisons within one binary.

#ifndef INCLUDED_TURNLOG_H
#define INCLUDED_TURNLOG_H

#include <stdint.h>

/**
 * The phases, in the order they appear on the record line.
 *
 * Six of them are leaves -- disjoint, and they sum to less than the turn. Two are
 * containers and deliberately overlap their leaves:
 *
 *   TURNLOG_WORLD    process_world(), which contains FORGET, NOISE, SCENT and TRAPS
 *   the "tot" field  the whole command, which contains everything
 *
 * That overlap is the point: "the seven sweeps are 80 % of process_world and process_world
 * is 90 % of the turn" is the sentence item 2 of the stage plan needs, and it cannot be
 * written from disjoint figures alone.
 *
 * UPDVIEW is the update_view() main loop only. mark_wasseen() and calc_lighting() are the
 * two phases before it and are bracketed at their call sites inside update_view(), so
 * WASSEEN + LIGHTING + UPDVIEW is the cost of update_view() less its own preamble.
 */
enum {
	TURNLOG_WASSEEN = 0,	/* cave-view.c mark_wasseen -- sweep 1 */
	TURNLOG_LIGHTING,	/* cave-view.c calc_lighting -- sweep 2 */
	TURNLOG_UPDVIEW,	/* cave-view.c update_view main loop -- sweep 3 */
	TURNLOG_FORGET,		/* game-world.c forget_noise -- sweep 4 */
	TURNLOG_NOISE,		/* game-world.c make_noise flood -- sweep 5 */
	TURNLOG_SCENT,		/* game-world.c update_scent -- sweep 6 */
	TURNLOG_TRAPS,		/* game-world.c trap timeouts -- sweep 7 */
	TURNLOG_MONSTERS,	/* mon-move.c process_monsters + reset_monsters */
	TURNLOG_WORLD,		/* game-world.c process_world -- contains 4,5,6,7 */
	TURNLOG_GEN,		/* cave-gen.c prepare_next_level */
	/*
	 * Stage 075 item 1: the unbracketed turn. Every phase below is SELF TIME -- the
	 * window less every other bracketed window (any phase, or idle) that opened and
	 * closed inside it -- because handle_stuff() and friends are reached from inside
	 * process_player(), process_world() and process_monsters(), and an inclusive figure
	 * would count the same microseconds twice. The phases above keep their stage 070
	 * inclusive meaning, so old and new captures compare row for row.
	 */
	TURNLOG_CLEANUP,	/* game-world.c process_player_cleanup */
	TURNLOG_PLAYER,		/* game-world.c process_player, three sites */
	TURNLOG_NOTICE,		/* player-calcs.c notice_stuff */
	TURNLOG_BONUS,		/* player-calcs.c update_stuff to the character_generated test */
	TURNLOG_UPDMON,		/* player-calcs.c update_stuff, both update_monsters calls */
	TURNLOG_PANEL,		/* player-calcs.c update_stuff, the PU_PANEL signal */
	TURNLOG_MAP,		/* ui-display.c update_maps, the prt_map() branch */
	TURNLOG_REDRAW,		/* player-calcs.c redraw_stuff, less map and all else inside */
	TURNLOG_REFRESH,	/* game-world.c run_game_loop, EVENT_REFRESH */
	TURNLOG_ANIMATE,	/* game-world.c run_game_loop, EVENT_ANIMATE */
	TURNLOG_KBPOLL,		/* main-pico.c check_events(false), the non-waiting poll */
	TURNLOG_PHASE_MAX
};

#ifdef ANGBAND_TURN_LOG

/*
 * A mark is the start time and the claimed total at the start. `claimed` is the union of
 * every closed bracket window in the turn; turnlog_end() prints the turn less it as `rst`,
 * which is exact however the brackets nest. See turnlog.c.
 */
typedef struct {
	uint64_t t0;
	uint64_t claimed;
} turnlog_mark_t;

turnlog_mark_t turnlog_mark(void);
void turnlog_acc(int phase, turnlog_mark_t m);
void turnlog_self(int phase, turnlog_mark_t m);
void turnlog_idle(turnlog_mark_t m);
void turnlog_begin(void);
void turnlog_end(int depth, int32_t game_turn);

/** Take a start stamp into a fresh local. Declares the variable. */
#define TURNLOG_MARK(v)			turnlog_mark_t v = turnlog_mark()
/** Add (now - v) to a phase. Calling it twice for one phase accumulates. */
#define TURNLOG_ACC(p, v)		turnlog_acc((p), (v))
/** Add (now - v) less every bracket closed inside it to a phase. Stage 075. */
#define TURNLOG_SELF(p, v)		turnlog_self((p), (v))
/** Add (now - v) to the turn's idle total, which turnlog_end() subtracts. */
#define TURNLOG_IDLE(v)			turnlog_idle((v))
/** Start a turn record. Called before run_game_loop(). */
#define TURNLOG_BEGIN()			turnlog_begin()
/** Close and print a turn record. Called after run_game_loop(), outside the window. */
#define TURNLOG_END(d, t)		turnlog_end((d), (t))

#else /* !ANGBAND_TURN_LOG -- the shipped build */

#define TURNLOG_MARK(v)			((void)0)
#define TURNLOG_ACC(p, v)		((void)0)
#define TURNLOG_SELF(p, v)		((void)0)
#define TURNLOG_IDLE(v)			((void)0)
#define TURNLOG_BEGIN()			((void)0)
#define TURNLOG_END(d, t)		((void)0)

#endif /* ANGBAND_TURN_LOG */

#endif /* INCLUDED_TURNLOG_H */
