// PORT: port-written for angband-pico, picocalc-device-harness stage 055. The lockstep
// witness.
//
// WHAT THIS IS
//
// One line of serial per completed player command saying where the game is and, more
// importantly, where its random number generator is. The harness runs Angband's own borg
// on the PC inside a host build of THIS core, sends the borg's keypresses to the device
// down the stage 030 serial key path, and compares the device's SYNC line for each command
// against the host's. Two simulations fed the same savefile and the same keys must reach
// the same state; the SYNC line is the proof that they did.
//
// Without it, "the borg played the device for 1,000 turns" is a claim that a session which
// desynced at turn 40 would also make.
//
// WHY THE DECLARATIONS ARE HERE AND THE DEFINITIONS ARE IN sync.c
//
// The same rule turnlog.h gives: src/game/ui-game.c holds the only call site, and
// tools/compile-sweep.sh compiles src/game/ with -I src/platform and no SDK include path
// at all. So this header must compile under gnu99 with nothing but <stdint.h>. Everything
// that needs player.h, z-rand.h or a printf lives in sync.c next door.
//
// WHERE THE CALL SITE IS, AND WHY IT IS THE SAME ONE AS THE TURN LOG'S
//
// ui-game.c's play_game() loop:
//
//     while (!player->is_dead && player->upkeep->playing) {
//         pre_turn_refresh();
//         cmd_get_hook(CTX_GAME);
//         TURNLOG_BEGIN();
//         run_game_loop();
//         TURNLOG_END(player->depth, turn);
//         ANGBAND_SYNC_END(turn);
//     }
//
// That is one iteration per completed player command, and it is AFTER run_game_loop() has
// returned -- so the ~5 ms of wire this costs is outside every measured bracket, the same
// property that makes turnlog_end()'s own printf free (harness invariant 2, principle 2).
// It is also the reason the SYNC record number and the TL record number are the same
// number: both counters advance exactly here, so a SYNC line and a TL line for one command
// can be paired in the capture by their first field.
//
// THE HOST BUILD COMPILES THIS TOO, AND THAT IS THE POINT
//
// sync.c is compiled by tools/host-build.sh --borg as well as by the device target, so the
// line the PC writes and the line the device prints come out of ONE formatter. A format
// that could drift between the two sides would make every comparison meaningless. Only the
// last step differs: sync_emit() is a printf on the device and a write to the file
// main-borg.c opened on the host, selected by #ifdef PICOCALC.
//
// ANGBAND_SYNC is a CMake option, OFF in the shipped build and OFF in every stage 070
// measured binary. With it off, the macro below is ((void)0), sync.c is an empty
// translation unit, and the link is byte-identical in .text (harness invariant 2).

#ifndef INCLUDED_SYNC_H
#define INCLUDED_SYNC_H

#include <stdint.h>

#ifdef ANGBAND_SYNC

/**
 * Emit one SYNC line for the command that has just completed.
 *
 * game_turn is ui-game.c's `turn`, passed rather than read so that this header needs no
 * core declaration -- the same trick TURNLOG_END() plays with player->depth.
 */
void sync_end(int32_t game_turn);

#define ANGBAND_SYNC_END(t)	sync_end((t))

#else /* !ANGBAND_SYNC */

#define ANGBAND_SYNC_END(t)	((void)0)

#endif /* ANGBAND_SYNC */

#endif /* INCLUDED_SYNC_H */
