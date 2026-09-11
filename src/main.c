// PORT: port-written for angband-pico, stage 050.
//
// The game's entry point on the PicoCalc. Replaces upstream src/main.c, which is the UNIX
// front-end chooser and lives in src/host/ here, built only by tools/host-build.sh.
//
// Written after angband/src/main-nds.c's main() -- upstream's own port to a small ARM
// handheld with a FAT card and no operating system, which is very nearly this board's
// situation. That file is read from angband/ next door and is never vendored.
//
// specifications.md 6.2 (the PSRAM heap), 6.3 (the card and the clock), 6.4 (the front
// end), 7.1 (the memory model), 11 (execution), 12 (known gaps).
//
// The order of the first four calls is not arbitrary and is the accumulated result of
// stages 020 and 030:
//
//   1. stack_paint()       before anything else pushes a frame worth measuring.
//   2. syscalls_init()     BY NAME. syscalls.c holds strong _gettimeofday/_getpid/_kill
//                          over the SDK's __weak ones, and a strong definition sitting
//                          unreferenced in a static library is never pulled in. Must
//                          precede anything that reads the clock.
//   3. psram_heap_stats()  BY NAME, for exactly the same reason: it is what opens
//                          psram_heap.c.obj and so what puts _sbrk -- and therefore the
//                          whole heap -- in PSRAM. Must precede the first mem_alloc.
//   4. sd_fs_mount()       before init_file_paths(); the game reads 1.3 MB off the card
//                          during init_angband().

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "platform/lcd.h"
#include "platform/main-pico.h"
#include "platform/psram_heap.h"
#include "platform/sd_fs.h"
#include "platform/syscalls.h"

#include "angband.h"
#include "buildid.h"
#include "cmd-core.h"
#include "game-event.h"
#include "game-world.h"
#include "init.h"
#include "ui-display.h"
#include "ui-game.h"
#include "ui-init.h"
#include "ui-input.h"
#include "ui-output.h"
#include "ui-term.h"
#include "z-rand.h"
#include "z-util.h"

// ---------------------------------------------------------------------------------------
// Boot-time serial and panel output.
//
// Two rules from stage 020's first device run (specifications.md 6.5), and they apply to
// the game as much as to a diagnostic: format the serial line in its own buffer rather
// than a panel-width one, and wait for the USB CDC host before the first printf so the
// header is not lost to a port that is not open yet.

#define BOOT_LINE_MAX 160

// Where the next boot line lands on the panel. init_angband() signals EVENT_ENTER_INIT
// almost immediately, and ui-display.c's splashscreen handler then owns the screen, so
// these rows are only ever seen before that and on the failure paths.
static uint8_t boot_row = 0;

static void boot_panel_line(const char *text, uint8_t row)
{
    for (uint8_t col = 0; col < PICO_TERM_COLS; col++)
        lcd_putc(col, row, (uint8_t)(*text ? *text++ : ' '));
}

// Serial always; the panel too, but only while the boot rows still belong to us. Once the
// term is live the panel half is dropped -- lcd_putc writes straight at the glass, behind
// ui-term.c's back, and a stray line during play would survive until something repainted
// that row.
static bool panel_is_the_terms = false;

static void boot_say(const char *fmt, ...)
{
    char line[BOOT_LINE_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    printf("%s\n", line);

    if (!panel_is_the_terms && boot_row < PICO_TERM_ROWS)
        boot_panel_line(line, boot_row++);
}

// The panic path, shared by the mount failure and by z-util.c's quit(). Spreads a message
// over as many panel rows as it needs, flushes serial, and stops. There is nothing to
// return to on a board: main() returning hands control to the SDK's exit(), which halts
// with whatever happened to be on the glass.
static void halt_with(const char *what, const char *detail)
{
    panel_is_the_terms = false;
    boot_row = 0;

    // One blank line to separate the box from whatever was on the wire, and nothing
    // more: boot_say() below does its own printf. The first device run had a printf of
    // the banner here as well, and the serial log carried "*** Angband quit ***" twice.
    printf("\n");
    boot_panel_line("", 0);
    boot_say("*** %s ***", what);

    if (detail && *detail)
    {
        // Wrap the message across the panel rather than clipping it at 64 columns: a
        // quit() message is usually a file path and the interesting half is the tail.
        size_t len = strlen(detail);
        for (size_t off = 0; off < len && boot_row < PICO_TERM_ROWS - 1; off += PICO_TERM_COLS)
        {
            char chunk[PICO_TERM_COLS + 1];
            size_t n = len - off;
            if (n > PICO_TERM_COLS)
                n = PICO_TERM_COLS;
            memcpy(chunk, detail + off, n);
            chunk[n] = '\0';
            if (off == 0)
                printf("%s\n", detail);
            boot_panel_line(chunk, boot_row++);
        }
    }

    boot_panel_line("halted -- power cycle the PicoCalc", PICO_TERM_ROWS - 1);
    printf("halted -- power cycle the PicoCalc\n");
    stdio_flush();

    while (true)
    {
        tight_loop_contents();
    }
}

// ---------------------------------------------------------------------------------------
// z-util.c's hooks.
//
// quit() with no hook calls exit(), which on a board is a silent halt with a stale panel;
// stage 030 learned that through angband_fsdiag and specifications.md 6.5 records it. Set
// both before the first call into any core code.

static void hook_plog(const char *str)
{
    if (!str)
        return;

    printf("plog: %s\n", str);

    // Never on the panel once ui-term.c owns it: the next Term_fresh() only repaints the
    // cells it thinks changed, so a line written behind its back can outlive the warning
    // it reports. Serial is the record.
    if (!panel_is_the_terms)
        boot_say("%s", str);
}

static void hook_quit(const char *str)
{
    halt_with(str ? "Angband quit" : "Angband exited", str);
}

// ---------------------------------------------------------------------------------------
// The stack.
//
// specifications.md 12 recorded that the SDK puts the core-0 stack in the fixed 4 KB
// SCRATCH_Y and that this stage has to move it. It is moved in the linker script --
// src/link/sections_stack.incl and src/link/section_end.incl, registered with
// pico_add_linker_script_override_path() -- so the whole program runs on the big stack
// from reset, rather than being switched over inside main() with the SDK's own start-up
// left on 4 KB. __StackTop and __StackBottom therefore mean what they say and the map
// file shows the placement.
//
// The high-water mark is measured the only way that works without an MPU: paint the
// unused part with a pattern at boot and find the lowest word that still holds it.
//
// The paint stops short of the current stack pointer, so everything the SDK's runtime
// init used before main() counts as used. That is deliberate -- it is genuine usage and
// the figure should include it -- and it means the number is an upper bound on the
// unmeasured part by however much of that frame was already dead. It is about 300 bytes.

extern char __StackTop;
extern char __StackBottom;

#define STACK_PATTERN 0xC5C5C5C5u
#define STACK_PAINT_GUARD 256

static void stack_paint(void)
{
    uint32_t here;
    uint32_t *lo = (uint32_t *)&__StackBottom;
    uint32_t *hi = (uint32_t *)(((uintptr_t)&here - STACK_PAINT_GUARD) & ~(uintptr_t)3);

    if (hi <= lo)
        return;

    while (lo < hi)
        *lo++ = STACK_PATTERN;
}

static size_t stack_size(void)
{
    return (size_t)(&__StackTop - &__StackBottom);
}

static size_t stack_high_water(void)
{
    const uint32_t *lo = (const uint32_t *)&__StackBottom;
    const uint32_t *hi = (const uint32_t *)&__StackTop;
    const uint32_t *p = lo;

    while (p < hi && *p == STACK_PATTERN)
        p++;

    return (size_t)((const char *)hi - (const char *)p);
}

// ---------------------------------------------------------------------------------------
// The device half of the specifications.md 7.1 heap table.
//
// The host figures come from tools/host-build.sh, which runs the same game under a malloc
// shim. The device figures come from here: one line per level the game generates, printed
// on serial, plus the line main() prints when init_angband() returns. Between them they
// are the four points 7.1 asks for -- after init, in town, and on whatever depths the
// player jumps to.
//
// It is hung off EVENT_NEW_LEVEL_DISPLAY rather than polled, so it costs nothing per turn
// and cannot miss a level. ui-display.c registers its own handler for the same event;
// game-event.c keeps a list, so both run.

static void report_memory(const char *tag)
{
    psram_heap_stats_t heap;
    psram_heap_stats(&heap);

    printf("PICO[%s] t=%lu ms  heap brk=%u KB high=%u KB free=%u KB  "
           "stack=%u of %u B  depth=%d\n",
           tag, (unsigned long)(time_us_64() / 1000u),
           (unsigned)(psram_heap_used() / 1024u),
           (unsigned)(psram_heap_high_water() / 1024u),
           (unsigned)(psram_heap_free() / 1024u),
           (unsigned)stack_high_water(), (unsigned)stack_size(),
           (player && character_dungeon) ? (int)player->depth : -1);
    stdio_flush();
}

static void pico_on_new_level(game_event_type type, game_event_data *data, void *user)
{
    char tag[32];

    (void)type;
    (void)data;
    (void)user;

    strnfmt(tag, sizeof(tag), "level %d", player ? (int)player->depth : -1);
    report_memory(tag);
}

// ---------------------------------------------------------------------------------------
// The RNG seed.
//
// specifications.md 6.3: time() here is boot time plus a fixed 2026 epoch, so it reads
// very nearly the same number on every boot -- and z-rand.c's Rand_init() on a non-UNIX
// platform seeds from time() and nothing else. Left alone, every new game would roll the
// same dungeon.
//
// Rand_init() only acts while Rand_quick is set, so seeding here and clearing the flag
// leaves it with nothing to do and this seed stands. get_rand_32() is the SDK's entropy
// source; on the RP2350 it is the hardware TRNG, mixed with the timer either way.

static void seed_rng(void)
{
    uint32_t seed = get_rand_32() ^ (uint32_t)time_us_64();

    Rand_state_init(seed);
    Rand_quick = false;

    boot_say("rng seed %08lx", (unsigned long)seed);
}

// ---------------------------------------------------------------------------------------
// File paths.
//
// Kept out of main() for the reason main-nds.c gives: a 1024-byte path buffer has no
// business sitting in the frame that then calls play_game().
//
// All three roots are the same directory. specifications.md 6.1: with neither WINDOWS nor
// UNIX defined, z-file.c's path_normalize() rejects any path that does not start with
// "/", which these do.

static void init_files(void)
{
    char path[64];

    my_strcpy(path, "/angband/lib/", sizeof(path));
    init_file_paths(path, path, path);

    // create_needed_dirs() is not UNIX-only -- init.c:411 has no guard -- and it makes
    // exactly the five directories the game writes into: user/, user/save/, user/panic/,
    // user/scores/ and user/archive/. Stage 030 proved dir_create() works on this card.
    // It quit_fmt()s on failure, which reaches hook_quit above.
    create_needed_dirs();

    // specifications.md 8: one fixed savefile name. make_safe false -- "PicoCalc" is
    // already a legal FAT name and player_safe_name() would only lower-case it.
    savefile_set_name("PicoCalc", false, false);
}

// ---------------------------------------------------------------------------------------

int main(void)
{
    uint64_t t0, t1;

    stack_paint();

    // By name, and before the clock and the first allocation respectively. See the header
    // comment; specifications.md 6.2 and 6.3 carry the whole reasoning.
    syscalls_init();
    (void)psram_heap_in_psram();

    stdio_init_all();

    // The front end first, so there is a panel to print a mount failure on. init_pico()
    // brings up the LCD, the keyboard, the UTF-8 hooks and the one 64x32 term, and leaves
    // Term pointing at it (specifications.md 6.4).
    if (init_pico())
        halt_with("No terminal initialised", NULL);

    // z-util.c's hooks, before the first call into core code.
    plog_aux = hook_plog;
    quit_aux = hook_quit;

    boot_say("Angband %s on the PicoCalc", buildver);

    // specifications.md 6.5, and the rule the user restated for this stage: the board is
    // powered up before the USB lead goes in, so nothing may be printed for the first ten
    // seconds unless a terminal is already listening. Never blocks -- the game must still
    // run headless.
    boot_say("waiting up to 10 s for a USB terminal...");
    for (unsigned i = 0; i < 100 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    sleep_ms(300);

    boot_say("Angband %s on the PicoCalc -- stage 050 bring-up", buildver);
    boot_say("stack %u B at %p..%p", (unsigned)stack_size(),
             (void *)&__StackBottom, (void *)&__StackTop);

    if (!sd_fs_mount())
        halt_with("SD card mount failed", sd_fs_last_error());

    boot_say("card mounted, SPI %lu Hz (asked %lu)",
             (unsigned long)sd_fs_effective_hz(), (unsigned long)sd_fs_requested_hz());

    ANGBAND_SYS = "pico";

    init_files();
    boot_say("savefile %s", savefile);

    seed_rng();

    cmd_get_hook = textui_get_cmd;

    init_display();
    event_add_handler(EVENT_NEW_LEVEL_DISPLAY, pico_on_new_level, NULL);

    // 1.3 MB of gamedata through file_getl at ~460 KB/s (specifications.md 6.3) plus the
    // parsing. Say so before it starts: init_angband() signals EVENT_ENTER_INIT straight
    // away and ui-display.c's splashscreen handler then narrates it on the panel, but the
    // serial log needs a marker either side to get the total.
    boot_say("loading gamedata, this takes a few seconds...");
    stdio_flush();
    panel_is_the_terms = true;

    t0 = time_us_64();
    init_angband();
    t1 = time_us_64();

    printf("PICO[init] init_angband() took %lu ms\n", (unsigned long)((t1 - t0) / 1000u));
    report_memory("init");

    textui_init();

    // The same three numbers on the panel, on the row pause_line() is about to sit under.
    // Serial is the record, but the board is often run with no lead in it and these are
    // the figures specifications.md 7.1 and 12 want off the first boot. prt() rather than
    // lcd_putc(): the term owns the glass from here on.
    {
        char note[PICO_TERM_COLS + 1];

        strnfmt(note, sizeof(note), "init %lu ms   heap %u KB   stack %u B",
                (unsigned long)((t1 - t0) / 1000u),
                (unsigned)(psram_heap_used() / 1024u),
                (unsigned)stack_high_water());
        prt(note, 0, 0);
    }

    // main-nds.c pauses here too. It is the last chance to read the splash screen, and on
    // this board it also proves a key reaches the game before anything depends on one.
    pause_line(Term);

    // GAME_LOAD with no savefile births a character: ui-game.c's start_game() only calls
    // savefile_load() when file_exists(savefile), and leaves player->is_dead set
    // otherwise, which takes it into textui_do_birth(). Confirmed by reading
    // ui-game.c:700-740; no file_exists test is needed here.
    play_game(GAME_LOAD);

    report_memory("exit");

    textui_cleanup();
    cleanup_angband();

    quit(NULL);
    return 0;
}
