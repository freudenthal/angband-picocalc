// PORT: port-written for angband-pico, stage 140. The boot profiler.
//
// WHAT THIS IS
//
// specifications.md 12 has carried a hypothesis since stage 060: "about 99 % of the boot is
// parsing, the parser is slow because it runs from flash over the PSRAM heap; nothing has
// been measured inside the parser." This file is what turns it into a measurement: a SysTick
// PC/LR sampler bracketing init_angband(), a per-run_parser() timing bracket, and a dump that
// prints both plus the aggregated samples for tools/bootprof.py to resolve against the ELF.
//
// WHY THE DECLARATIONS ARE HERE AND THE DEFINITIONS ARE IN src/platform/bootprof.c
//
// The stage 045 correction, same as turnlog.h: a core source (src/game/datafile.c) cannot
// include an SDK header, because tools/compile-sweep.sh compiles src/game/ with no SDK
// include path. This header compiles under gnu99 with nothing but <stdint.h>; every use of
// time_us_64(), the SysTick registers and printf() lives in bootprof.c next door.
//
// WHY IT IS ALL MACROS
//
// ANGBAND_BOOT_PROFILE is a CMake option, OFF in the shipped build and refused without
// ANGBAND_CONSOLE (the developer console the dump prints on). With it off every macro below
// is ((void)0) and bootprof.c is not even a source of the `angband` target -- CMakeLists.txt
// adds it to the target's sources only when the option is ON, unlike turnlog.c, which is
// always a source and an empty object when its option is off. One more object on the link,
// even an empty one, moves the linker's long-branch veneers (specifications.md 13's
// ANGBAND_SYNC precedent), so build-pico2's .text is byte-identical to a tree with no
// knowledge of this file at all only because the file is entirely out of the link -- checked
// with objcopy -O binary --only-section=.text and cmp.
//
// WHAT IT DOES NOT DO
//
// It does not move, optimise or edit any parser code for speed, and it does not change the
// shipped build. It answers "where did the time go", which is stage 150's input.

#ifndef INCLUDED_BOOTPROF_H
#define INCLUDED_BOOTPROF_H

#include <stdint.h>

#ifdef ANGBAND_BOOT_PROFILE

// NOT an #error guard here, unlike turnlog.h's cousin file bootprof.c. This header is also
// included by src/game/datafile.c, which compiles as part of the angband_core OBJECT
// library -- a target that never receives ANGBAND_CONSOLE (specifications.md 10: "no
// src/game/ source does"), by design, even when it is compiled into a console build. The
// guard lives in bootprof.c instead, where ANGBAND_CONSOLE is visible, exactly where
// turnlog.c's own #error lives and not in turnlog.h.

/**
 * A phase mark: a name and time_us_64() at the moment it was taken. bootprof_dump() prints
 * these in the order they were taken as "BOOTPROF phase <name> <us since reset>".
 */
void bootprof_mark(const char *name);

/**
 * Called during the console build's existing 10 s USB wait (src/main.c), once per poll.
 * Reads whatever byte is waiting on either stdio transport with getchar_timeout_us(0) --
 * the same non-blocking idiom src/platform/main-pico.c's ANGBAND_SERIAL_KEYS poll uses --
 * and arms the run-time gate on the byte 'P'. The gate, once armed, stays armed; polling
 * after it is already armed costs one getchar_timeout_us(0) call and nothing else.
 *
 * WHY A RUN-TIME GATE AND NOT A SECOND BUILD. Item 8 of the stage plan needs the SAME
 * BINARY booted with the sampler on and off, because specifications.md 13's rule --
 * "measure the same binary twice before attributing a change" -- means two builds would
 * put the placement error bar (12.7 % across four placements, stage 065) on top of
 * whatever the sampler costs, and the two could not be told apart.
 */
void bootprof_gate_poll(void);

/**
 * Arm the sampler: install the SysTick exception handler if it is not installed yet, and
 * start SysTick counting at BOOTPROF_SAMPLE_HZ (bootprof.c; 500 Hz, halved from the 1 kHz
 * item 2 first tried after item 8.4 measured 5.7 % overhead at 1 kHz on the device -- the
 * dump's own "BOOTPROF samples ... hz" line always states the achieved rate, so nothing
 * downstream has to know the constant). A no-op if the run-time gate (bootprof_gate_poll) was
 * never armed -- so BOOTPROF_START()/BOOTPROF_STOP() can bracket init_angband()
 * unconditionally in src/main.c and the phase marks and parser brackets still run with the
 * sampler off, exactly as item 3 of the stage plan asks.
 */
void bootprof_start(void);

/** Stop SysTick. Safe to call whether or not bootprof_start() actually armed anything. */
void bootprof_stop(void);

/**
 * The two halves of one src/game/datafile.c run_parser() call. Because run_parser() is
 * never reentrant in this tree (every struct file_parser table calls it in sequence, never
 * from inside another run_parser() call), one set of "current call" statics is enough --
 * no stack is kept. bootprof_parser_end() reads psram_heap_used() again and stores the
 * elapsed microseconds and the two KB figures under fp->name, which is always a string
 * literal with program lifetime, so the pointer is kept rather than copied.
 */
void bootprof_parser_begin(void);
void bootprof_parser_end(const char *name);

/**
 * Print everything: the phase marks, the parser records (and the unbracketed remainder of
 * init_angband(), which is not assumed to be zero), then -- after all of that, so the
 * aggregation cannot perturb any timing that has already been printed -- the samples
 * aggregated into PC buckets and (PC, LR) pairs, then the sample/overflow/rate summary and
 * "BOOTPROF end". Called once, from src/main.c, after the splash-key phase mark and before
 * the game waits for the key that dismisses the splash -- so a harness session sees the
 * dump with no key needed first.
 */
void bootprof_dump(void);

#ifdef ANGBAND_BOOT_PROFILE_SELFTEST
/**
 * Item 6's proof-before-trust apparatus. Skips the run-time gate -- the self-test tree has
 * nothing driving the UART -- so src/main.c's own two planted burn functions run under the
 * sampler unconditionally. THE BURN FUNCTIONS ARE NOT HERE: bootprof.c.obj is excluded into
 * SRAM whole (CMakeLists.txt, the same rule turnlog.c.obj is held to), so a function that
 * must end up in FLASH cannot be defined in this file -- it would be swept along with
 * everything else. src/main.c defines both: one an ordinary function, wherever main.c.obj
 * (never SRAM-swept) places it, and one forced into SRAM with __not_in_flash_func(), the
 * same attribute it already uses for pico_battery_refresh().
 */
void bootprof_force_arm(void);

#define BOOTPROF_FORCE_ARM()      bootprof_force_arm()
#endif

#define BOOTPROF_MARK(name)          bootprof_mark(name)
#define BOOTPROF_GATE_POLL()         bootprof_gate_poll()
#define BOOTPROF_START()             bootprof_start()
#define BOOTPROF_STOP()              bootprof_stop()
#define BOOTPROF_PARSER_BEGIN()      bootprof_parser_begin()
#define BOOTPROF_PARSER_END(name)    bootprof_parser_end(name)
#define BOOTPROF_DUMP()              bootprof_dump()

#else /* !ANGBAND_BOOT_PROFILE -- the shipped build, and every build without the option */

#define BOOTPROF_MARK(name)          ((void)0)
#define BOOTPROF_GATE_POLL()         ((void)0)
#define BOOTPROF_START()             ((void)0)
#define BOOTPROF_STOP()              ((void)0)
#define BOOTPROF_PARSER_BEGIN()      ((void)0)
#define BOOTPROF_PARSER_END(name)    ((void)0)
#define BOOTPROF_DUMP()              ((void)0)

#endif /* ANGBAND_BOOT_PROFILE */

#endif /* INCLUDED_BOOTPROF_H */
