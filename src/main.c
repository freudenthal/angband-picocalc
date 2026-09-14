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
//
// Stage 090: ANGBAND_CONSOLE. Everything this file says on serial, the boot lines on the
// panel, the 10 s USB wait, the PICO[...] memory reports and the note on the splash row are
// developer helpers and are built only with it. The shipped build keeps the panel half of
// halt_with(), so a mount failure or a quit() is still explained on the glass.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/rand.h"
#ifdef ANGBAND_CONSOLE
#include "pico/stdio_usb.h"
#endif
#include "pico/stdlib.h"

#include "platform/battery.h"
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
#include "message.h"
#include "player-calcs.h"
#include "player.h"
#include "ui-display.h"
#include "ui-game.h"
#include "ui-init.h"
#include "ui-input.h"
#include "ui-output.h"
#include "ui-term.h"
#include "z-file.h"
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

// Serial in a console build; the panel too, but only while the boot rows still belong to
// us. Once the term is live the panel half is dropped -- lcd_putc writes straight at the
// glass, behind ui-term.c's back, and a stray line during play would survive until
// something repainted that row.
static bool panel_is_the_terms = false;

static void boot_say(const char *fmt, ...)
{
    char line[BOOT_LINE_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

#ifdef ANGBAND_CONSOLE
    printf("%s\n", line);
#endif

    if (!panel_is_the_terms && boot_row < PICO_TERM_ROWS)
        boot_panel_line(line, boot_row++);
}

// The boot narration: a line on serial and the panel in a console build, nothing at all in
// the shipped one. boot_say() itself is for what the player must see either way.
#ifdef ANGBAND_CONSOLE
#define console_say(...) boot_say(__VA_ARGS__)
#else
#define console_say(...) ((void)0)
#endif

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
#ifdef ANGBAND_CONSOLE
    printf("\n");
#endif
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
#ifdef ANGBAND_CONSOLE
            if (off == 0)
                printf("%s\n", detail);
#endif
            boot_panel_line(chunk, boot_row++);
        }
    }

    boot_panel_line("halted -- power cycle the PicoCalc", PICO_TERM_ROWS - 1);
#ifdef ANGBAND_CONSOLE
    printf("halted -- power cycle the PicoCalc\n");
    stdio_flush();
#endif

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

#ifdef ANGBAND_CONSOLE
    printf("plog: %s\n", str);
#endif

    // Never on the panel once ui-term.c owns it: the next Term_fresh() only repaints the
    // cells it thinks changed, so a line written behind its back can outlive the warning
    // it reports. Serial is the record in a console build. In both builds a plog() before
    // the term owns the panel is a warning the player should see, so this is boot_say().
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

// The readers are only called from console code. The paint stays in every build: 64 KB of
// stores, once, at boot.
#ifdef ANGBAND_CONSOLE

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

#endif /* ANGBAND_CONSOLE: the stack readers and the memory reports */

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

    console_say("rng seed %08lx", (unsigned long)seed);
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
// ANGBAND_SAVE_RESTORE -- the optimisation bench's savefile restore. Stage 065 item 1.
//
// OFF IN THE SHIPPED BUILD, because it destroys a player's game. Everything below is
// inside the #ifdef and the whole file's .text is byte-identical without it.
//
// WHY IT EXISTS. Angband writes its savefile back while it plays. After a 20-key probe on
// 2026-09-13 the card's PicoCalc had gone from the master's 37,500 B / md5 e6289abd... to
// 37,616 B / 22985b3a..., and although the character was unchanged the RNG state was not:
// si=19 rs=853891f9 became si=2 rs=e9a86912. A second lockstep run off that card diverges
// at command 1 for a reason that has nothing to do with either game. So without this, every
// optimisation round needs a hand on the card to restage the master -- which is the whole
// difference between an optimisation loop and a series of bench visits.
//
// THE CARD IS WRITTEN BY THE DEVICE, which already writes it on every autosave, so the
// harness's invariant 9 (the harness never touches the card) is untouched.
//
// IT IS A COPY AND NOT A RENAME. Stage 030 found rename() returns FR_EXIST on this
// filesystem rather than replacing, so the master would be consumed on the first round and
// the second would run off whatever the first left behind.
//
// COST: 37.5 KB at stage 030's ~145 KB/s is about 0.26 s, once, at boot, and boot is
// outside every bracket the turn log measures. The figure is printed so a run log can
// quote it, and it is also a free re-check of the card write path on every round.
#ifdef ANGBAND_SAVE_RESTORE

#define SAVE_RESTORE_MASTER "/angband/lib/user/save/PicoCalc.master"

// 4 KB, static rather than automatic, for the reason init_files() gives about its path
// buffer: nothing large belongs in a frame that then calls play_game(). It costs 4 KB of
// bss in a build that already carries the turn log and the SYNC emitter, and none at all
// in the shipped build.
#define SAVE_RESTORE_CHUNK 4096
static char save_restore_buf[SAVE_RESTORE_CHUNK];

static void restore_master_savefile(void)
{
    ang_file *src, *dst;
    uint64_t t0, t1;
    size_t total = 0;
    bool ok = true;
    int n;

    // Not an error. A card with no master is a card meant to be played from, and a build
    // with the option on must still boot on one.
    if (!file_exists(SAVE_RESTORE_MASTER))
    {
        console_say("save-restore: no PicoCalc.master on the card, leaving the savefile alone");
        return;
    }

    t0 = time_us_64();

    src = file_open(SAVE_RESTORE_MASTER, MODE_READ, FTYPE_RAW);
    if (!src)
    {
        console_say("save-restore: cannot open %s", SAVE_RESTORE_MASTER);
        return;
    }

    // FTYPE_RAW and NOT FTYPE_SAVE. MODE_WRITE with FTYPE_SAVE is the exclusive create
    // (z-file.c:1012, and stage 030 wrote the PICOCALC half of it): it refuses a name that
    // already exists, which is exactly the name this has to overwrite. FTYPE_RAW takes the
    // plain fopen(..., "wb") branch, which truncates.
    dst = file_open(savefile, MODE_WRITE, FTYPE_RAW);
    if (!dst)
    {
        file_close(src);
        console_say("save-restore: cannot write %s", savefile);
        return;
    }

    while ((n = file_read(src, save_restore_buf, sizeof(save_restore_buf))) > 0)
    {
        if (!file_write(dst, save_restore_buf, (size_t)n))
        {
            ok = false;
            break;
        }
        total += (size_t)n;
    }

    // file_read() returns -1 on a read error and 0 at end of file, so a short copy that
    // looks clean is a real possibility and has to be said out loud: a half-written
    // savefile diverges at command 1 and looks like an optimisation bug.
    if (n < 0)
        ok = false;

    if (!file_close(dst))
        ok = false;
    file_close(src);

    t1 = time_us_64();

    console_say("save-restore: %s %u B in %lu ms (boot only, outside every measured bracket)",
             ok ? "copied" : "FAILED after", (unsigned)total,
             (unsigned long)((t1 - t0) / 1000u));
}

#endif /* ANGBAND_SAVE_RESTORE */

// ---------------------------------------------------------------------------------------
// Stage 100. The battery: the status-row redraw while idle, the two warnings, and the one
// automatic save. The field itself is drawn by ui-display.c's prt_battery(); the logic
// both use is in platform/battery.h, and the I2C read is in platform/battery.c.

/**
 * main-pico.c's pico_battery_hook: the register changed while the game waits for a key.
 *
 * The flag is always set, so the next ordinary redraw draws the new value. The redraw is
 * done HERE only at the top-level command prompt (inkey_flag) with no saved screen on top
 * of the map: the game is inside Term_inkey() and nothing else will draw until a key
 * arrives, and at that prompt handle_stuff() has already run, so redraw_stuff() has only
 * the status row to do. Anywhere else -- a menu, a -more-, a store, birth -- it waits.
 * The cursor is put back exactly, "useless" flag included, because the prompt that is
 * waiting owns it.
 */
static void pico_battery_changed(void)
{
    int cx, cy;
    bool cu;

    // The raw byte on serial in a console build, so a session can set it beside the field.
    console_say("battery: register 0x%02x", (unsigned)battery_raw);

    if (!character_generated || !player || !player->upkeep)
        return;

    player->upkeep->redraw |= PR_STATUS;

    if (!inkey_flag || screen_save_depth || player->is_dead)
        return;

    cx = Term->scr->cx;
    cy = Term->scr->cy;
    cu = Term->scr->cu;

    redraw_stuff(player);

    Term->scr->cx = cx;
    Term->scr->cy = cy;
    Term->scr->cu = cu;
    Term_fresh();
}

static struct battery_warn battery_warning;

/**
 * A warning is due. Out of line and in flash: it runs at most a few times per boot.
 *
 * The save is upstream's own save_game(), the one Ctrl-S reaches, called from the same
 * place in process_player() that a Ctrl-S command runs from -- after handle_stuff(), before
 * the next command is taken. Once per boot, and not in an arena: that is a temporary level
 * the effect handlers special-case throughout, and the port has no reason to find out how
 * a savefile taken inside one reloads. The warning is still given there.
 */
static void pico_battery_warn(int threshold, int percent)
{
    if (threshold == BATTERY_WARN_CRITICAL && !battery_warning.saved
        && !player->upkeep->arena_level)
    {
        battery_warning.saved = true;
        msg("Your battery is at %d%%. The game will now be saved.", percent);
        save_game();
        return;
    }

    msg("Your battery is at %d%%.", percent);
}

/**
 * EVENT_REFRESH, signalled from process_player() before each command. In SRAM like the
 * code that signals it, because it runs every turn: two inline reads of .bss and a return,
 * with no call into flash unless a warning is due.
 *
 * NOT EVENT_PLAYERMOVED, which the stage plan named: update_stuff() signals that from
 * inside its PU_PANEL branch, and save_game() calls handle_stuff() -- a save from there
 * would re-enter the update it was called from.
 */
static void __not_in_flash_func(pico_battery_refresh)(game_event_type type,
                                                      game_event_data *data, void *user)
{
    int percent, threshold;
    bool charging;

    (void)type;
    (void)data;
    (void)user;

    if (!character_generated || player->is_dead || !battery_read(&percent, &charging))
        return;

    threshold = battery_warn_step(&battery_warning, percent, charging);
    if (threshold)
        pico_battery_warn(threshold, percent);
}

// ---------------------------------------------------------------------------------------

int main(void)
{
#ifdef ANGBAND_CONSOLE
    uint64_t t0, t1;
#endif

    stack_paint();

    // By name, and before the clock and the first allocation respectively. See the header
    // comment; specifications.md 6.2 and 6.3 carry the whole reasoning.
    syscalls_init();
    (void)psram_heap_in_psram();

#ifdef ANGBAND_CONSOLE
    stdio_init_all();
#endif

    // The front end first, so there is a panel to print a mount failure on. init_pico()
    // brings up the LCD, the keyboard, the UTF-8 hooks and the one 64x32 term, and leaves
    // Term pointing at it (specifications.md 6.4).
    if (init_pico())
        halt_with("No terminal initialised", NULL);

    // z-util.c's hooks, before the first call into core code.
    plog_aux = hook_plog;
    quit_aux = hook_quit;

#ifdef ANGBAND_CONSOLE
    boot_say("Angband %s on the PicoCalc", buildver);

    // specifications.md 6.5, and the rule the user restated for this stage: the board is
    // powered up before the USB lead goes in, so nothing may be printed for the first ten
    // seconds unless a terminal is already listening. Never blocks -- the game must still
    // run headless. Console builds only (stage 090): the shipped build has no USB stdio
    // and starts the mount straight away.
    boot_say("waiting up to 10 s for a USB terminal...");
    for (unsigned i = 0; i < 100 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    sleep_ms(300);

    boot_say("Angband %s on the PicoCalc -- stage 050 bring-up", buildver);
    boot_say("stack %u B at %p..%p", (unsigned)stack_size(),
             (void *)&__StackBottom, (void *)&__StackTop);
#endif

    if (!sd_fs_mount())
        halt_with("SD card mount failed", sd_fs_last_error());

    console_say("card mounted, SPI %lu Hz (asked %lu)",
                (unsigned long)sd_fs_effective_hz(), (unsigned long)sd_fs_requested_hz());

    ANGBAND_SYS = "pico";

    init_files();
    console_say("savefile %s", savefile);

    // Stage 065 item 1. After init_files(), because it needs `savefile` set and
    // create_needed_dirs() to have made user/save/; before play_game(GAME_LOAD), which is
    // what reads the file. OFF in the shipped build.
#ifdef ANGBAND_SAVE_RESTORE
    restore_master_savefile();
#endif

    seed_rng();

    cmd_get_hook = textui_get_cmd;

    init_display();
#ifdef ANGBAND_CONSOLE
    event_add_handler(EVENT_NEW_LEVEL_DISPLAY, pico_on_new_level, NULL);

    // 1.3 MB of gamedata through file_getl at ~460 KB/s (specifications.md 6.3) plus the
    // parsing. Say so before it starts: init_angband() signals EVENT_ENTER_INIT straight
    // away and ui-display.c's splashscreen handler then narrates it on the panel, but the
    // serial log needs a marker either side to get the total.
    boot_say("loading gamedata, this takes a few seconds...");
    stdio_flush();
#endif
    panel_is_the_terms = true;

#ifdef ANGBAND_CONSOLE
    t0 = time_us_64();
    init_angband();
    t1 = time_us_64();

    printf("PICO[init] init_angband() took %lu ms\n", (unsigned long)((t1 - t0) / 1000u));
    report_memory("init");
#else
    init_angband();
#endif

    textui_init();

    // Stage 100. The battery field, the warnings and the automatic save.
    pico_battery_hook = pico_battery_changed;
    event_add_handler(EVENT_REFRESH, pico_battery_refresh, NULL);

#ifdef ANGBAND_CONSOLE
    // The same three numbers on the panel, on the row pause_line() is about to sit under.
    // Serial is the record, but the board is often run with no lead in it and these are
    // the figures specifications.md 7.1 and 12 want off the first boot. prt() rather than
    // lcd_putc(): the term owns the glass from here on. Console builds only (stage 090).
    {
        char note[PICO_TERM_COLS + 1];

        strnfmt(note, sizeof(note), "init %lu ms   heap %u KB   stack %u B",
                (unsigned long)((t1 - t0) / 1000u),
                (unsigned)(psram_heap_used() / 1024u),
                (unsigned)stack_high_water());
        prt(note, 0, 0);
    }
#endif

    // main-nds.c pauses here too. It is the last chance to read the splash screen, and on
    // this board it also proves a key reaches the game before anything depends on one.
    pause_line(Term);

    // GAME_LOAD with no savefile births a character: ui-game.c's start_game() only calls
    // savefile_load() when file_exists(savefile), and leaves player->is_dead set
    // otherwise, which takes it into textui_do_birth(). Confirmed by reading
    // ui-game.c:700-740; no file_exists test is needed here.
    play_game(GAME_LOAD);

#ifdef ANGBAND_CONSOLE
    report_memory("exit");
#endif

    textui_cleanup();
    cleanup_angband();

    quit(NULL);
    return 0;
}
