# angband-pico — port notes

Angband for the ClockworkPi PicoCalc with a Pimoroni Pico Plus 2 W (RP2350B) fitted.

**Keep this file current.** Every edit to a vendored file must appear under
[PORT: edits](#port-edits) with its reason.

Project documents live in `../.llm/projects/angband-picocalc/`: `readme.md` for purpose and
principles, `specifications.md` for layout, systems, memory model and build commands,
`todo.md` for the stage tracker.

## Source

| | |
|---|---|
| Upstream | https://github.com/angband/angband |
| Commit | `00c9414cb` — "Convert signal_count to a volatile sig_atomic_t", 2026-08-25 |
| Relation to a release | 184 commits after tag `4.2.6` (2025-12-15); maintenance only, savefile-compatible |
| Local clone | `../angband/`. **Never edited.** `git -C angband status --porcelain` must print nothing. |
| Vendor commit here | `2cf1b4a` — "vendor: Angband 00c9414cb src/, lib/, tests/, licence (filtered, unedited)" |

Vendored with:

```
git -c core.autocrlf=false -C angband archive 00c9414cb src lib tests run-tests docs/copying.rst \
  | tar -x -C angband-pico/vendor-tmp
```

`core.autocrlf=false` matters. This machine's global `core.autocrlf` is `true`, and
`git archive` honours it: the first attempt produced a whole tree of CRLF files. That
compiles, but `/bin/sh` will not exec a shebang line ending in CR, so `tests/run-tests`
died with "required file not found", and the trailing CR on `player-birth Dwarf Priest`
made the class lookup fail. Every vendored file here is byte-identical to upstream;
`.gitattributes` (`* -text`) keeps git from converting them on checkout.

## Layout

| Path | Contents |
|---|---|
| `src/game/` | 150 `.c` and 166 `.h` — the whole core. Built for the device. |
| `src/host/` | Upstream `main.c` and `main-test.c`, plus the port-written `main-borg.c`. **WSL harness only**; never compiled for the device. |
| `src/game/borg/` | Angband's borg, 118 files, vendored **unmodified**. **HOST ONLY** — no device target mentions it. Under `src/game/` and not `src/` because every borg header includes `"../angband.h"`, which resolves only from there. Harness stage 055. |
| `src/platform/` | The platform layer. `lcd.c/.h`, `font5x10.c/.h`, `southbridge.c/.h` copied byte-identical from `../tinyrogue-pico/src/platform/`; `psram_heap.c/.h`, `sd_fs.c/.h`, `syscalls.c/.h` port-written. Stages 020, 030. |
| `src/main.c` | **The game's entry point.** Stage 050. Port-written, after upstream `src/main-nds.c`. Not to be confused with `src/host/main.c`, which is upstream's UNIX front-end chooser. |
| `src/link/` | Two pico-sdk linker-script overrides. Stage 050; see **The stack** below. |
| `src/psramdiag.c` | The stage 020 PSRAM diagnostic. Port-written. |
| `src/fsdiag.c` | The stage 030 SD filesystem diagnostic. Port-written. |
| `src/termdiag.c` | The stage 040 terminal diagnostic. Port-written. |
| `lib/` | Game data. Goes on the SD card at `/angband/lib/`. |
| `tests/` | Upstream end-to-end tests plus the top-level `run-tests` runner, moved to `tests/run-tests`. |
| `tools/` | The suite, the heap probe, the screen sweep and `rewrap-help.py`. Port-written. |
| `copying.txt` | Upstream `docs/copying.rst`, renamed. |
| `build-sweep/`, `build-host/`, `build-pico2/` | Generated. Gitignored. |

## What was filtered out

Directories deleted from `src/` (other front ends, the borg, the unit tests, and the
upstream build systems, none of which the port uses):

| Path | Files |
|---|---|
| ~~`src/borg/`~~ | ~~118~~ — **re-vendored at `src/game/borg/` by harness stage 055**, unmodified, for the PC-side borg only. See the layout table above. |
| `src/tests/` | 113 |
| `src/win/` | 26 |
| `src/nds/` | 22 |
| `src/cmake/` | 20 |
| `src/doc/` | 14 |
| `src/cocoa/` | 13 |
| `src/sdl2/` | 7 |
| `src/stats/` | 3 |

Files deleted from `src/`: `Makefile`, `Makefile.3ds`, `Makefile.blocksds`,
`Makefile.blocksds.arm7`, `Makefile.blocksds.arm9`, `Makefile.ibm`, `Makefile.inc`,
`Makefile.nds`, `Makefile.nds.arm7`, `Makefile.nds.arm9`, `Makefile.nmake`, `Makefile.osx`,
`Makefile.src`, `Makefile.std`, `HEADER`, `angband.man`, `gen-coverage`, `.splintrc`.

Front ends and sound drivers dropped (`main-*.c`, `main-*.m`, `snd-*`): `main-gcu.c`,
`main-ibm.c`, `main-nds-arm7.c`, `main-nds.c`, `main-sdl.c`, `main-sdl2.c`, `main-spoil.c`,
`main-stats.c`, `main-win.c`, `main-x11.c`, `main-xxx.c`, `main-cocoa.m`, `snd-sdl.c`,
`snd-sdl.h`, `snd-win.h`. `main.h` **stays** in `src/game/`: the core includes it.
`sound-core.c` and `sound.h` also stay; they are the platform-independent half.

Moved to `src/host/`: `main.c`, `main-test.c`.

Deleted from `lib/`: `tiles/` (28 files, ~20 MB), `sounds/` (214 files, ~3.4 MB),
`fonts/` (25 files), `icons/` (11 files), and the nine `Makefile`s under `lib/`,
`lib/customize/`, `lib/gamedata/`, `lib/help/`, `lib/screens/`, `lib/user/`,
`lib/user/panic/`, `lib/user/save/`, `lib/user/scores/`.

`tests/` is kept whole.

<a id="port-edits"></a>
## PORT: edits

**Twenty** vendored files are edited, and `ui-game.c` carries one more hunk since harness
stage 055 (section 6). Stage 070 run 3 added `cave.h`, `cave.c` and `cave-square.c` and
further hunks in `cave-view.c` and `game-world.c` (section 7) — **the first edits in this
tree that change what the game computes**, each proved by the desk pre-check and a lockstep
bench round. Each edit is wrapped in a `PORT:` banner; every hunk of
`git diff 2cf1b4a HEAD -- src/game src/host` contains the string `PORT:`. **The 118 files of
`src/game/borg/` are not among them and must stay that way** — they were vendored unmodified
and all 59 sources compiled clean against this core on the first attempt.

Four of them are the platform, the file layer, the host harness and one SRAM placement
(sections 1 to 4). Ten are stage 060's 64-column layout (section 5), and **every one of
those is conditional on `Term->wid < 80`**, so at 80 columns the code that runs is
upstream's. That is not an assertion: `tools/host-build.sh` runs the end-to-end tests at 80
columns and `tools/screen-sweep.sh` renders thirty-two screens at 80 and at 64 side by side.

The other three are stage 070's turn instrument and, in `ui-game.c`, harness stage 055's
lockstep witness (section 6). **They are guarded by a compile option and not by a width, and
with the option off they generate no code at all** — `tools/compile-sweep.sh` reports the
same `text` / `data` / `bss` before and after, and the linked game's `.text` is byte-identical
by `objcopy`/`cmp`.

**`-fsigned-char` is on every build since harness stage 055** — `CMakeLists.txt`,
`tools/host-build.sh` and `tools/compile-sweep.sh`, edited together or not at all. ARM GCC
defaults `char` to unsigned and x86-64 GCC to signed, and nothing here used to say which, so
a `char` compared against zero took a different value in the device build and in the host
build of the same source. That is invisible until two builds of one game are asked to agree,
which is exactly what lockstep does. It is not free: the shipped build's `.text` went
965,360 → **965,744 B (+384)** and `compile-sweep.sh`'s total 769,928 → **770,282 (+354)**,
spread over 37 of the 150 core sources and no more than 40 B in any one of them. Every figure
in this file measured before 2026-09-12 predates it.

```
git diff --stat 2cf1b4a HEAD -- src/game src/host
```

### 1. `src/game/h-basic.h` — the `PICOCALC` platform macro

Two hunks.

**a. `PICOCALC` joins the `UNIX` exclusion list**, beside `WINDOWS`, `GAMEBOY` and `NDS`.
This is the same mechanism upstream uses for the Nintendo DS. With `UNIX` undefined:

| Site | What it did |
|---|---|
| `z-file.c:131`–163 | `path_process()`: `~` and `~user` expansion through `getpwnam`/`getpwuid`. |
| `z-file.c:198` | `path_build()`: treats a leading `~` as an absolute path. |
| `z-file.c:437` | `path_normalize()`: resolves a relative or `~user` path through `getpwnam` and `getcwd`. The port takes the plain-absolute branch. |
| `z-file.c:1056`, `1072` | `file_lock()`/`file_unlock()` `fcntl` advisory locks; both are `HAVE_FCNTL_H && UNIX`. |
| `z-rand.c:140`, `584` | Mutates the `time(NULL)` seed with `getpid()`. The `#else` branch uses time alone. |
| `ui-signals.c:32` | `#include <sys/types.h>` inside the signal block. |
| `config.h:68` | `PRIVATE_USER_PATH`, i.e. `~/.angband`. |
| `h-basic.h:125` | `#include <pwd.h>`, `<sys/stat.h>`, `<unistd.h>`. |

**b. A `PICOCALC` block defines `HAVE_DIRENT_H`, `HAVE_STAT`, `HAVE_MKDIR`**, which were
otherwise only set under `UNIX`. pico-vfs supplies all three over the SD card, and
`z-file.c` includes `<sys/stat.h>` for itself under `HAVE_STAT`, so it compiles unchanged.
`HAVE_SIGACTION` and `HAVE_SIGPROCMASK` are deliberately **not** defined: there are no
signals.

`HAVE_FCNTL_H` is not set by this block but is defined unconditionally higher up in
`h-basic.h` (line 31, in the non-autoconf branch). That is harmless: every use of it in
`z-file.c` is also gated on `UNIX`, so no lock code is compiled.

### 2. `src/game/z-file.c` — two pico-vfs semantics differences

Stage 030. pico-vfs's FAT back end is not POSIX in two places that matter to `savefile.c`,
and both were read out of its source and then confirmed by `src/fsdiag.c` on the device.
Each edit is inside `#ifdef PICOCALC`, so the host build and every other platform keep
upstream's code exactly.

**a. `file_open(..., MODE_WRITE, FTYPE_SAVE)` — `O_EXCL` emulated, `O_TRUNC` added.**
Upstream opens with `O_CREAT | O_EXCL | O_WRONLY`, meaning "create, and fail if the name is
taken". pico-vfs (`src/filesystem/fat.c`, `file_open()`) never looks at `O_EXCL`, and maps a
bare `O_CREAT` to FatFs `FA_OPEN_ALWAYS` — which neither fails nor truncates. Left alone, a
save over a `<name>.new` left behind by a crash would look successful while writing the new
save into the front of the old one and leaving the old tail behind; `savefile.c` would then
rename that hybrid over the good savefile. The port does the exclusive check by hand with
`file_exists()` and adds `O_TRUNC` (which is what makes pico-vfs choose `FA_CREATE_ALWAYS`).
Upstream semantics are preserved exactly. There is no race: one process, one writer.

**b. `file_move()` — the target is removed before `rename()`.** FatFs `f_rename()` returns
`FR_EXIST` when the new name is already in use (`vendor/ff15/source/ff.c`, the "name
collision" test), so pico-vfs's `rename()` does not replace. `savefile.c` depends on
replacing twice per save. The port calls `remove()` on the target first, guarded by a
`strcmp` so that renaming a name onto itself cannot turn into a delete.

### 3. `src/host/main-test.c` — six harness probes and two term options

Stages 050 and 060. Host-only; this file never reaches the firmware.

| Added | Stage | What it is for |
|---|---|---|
| `heap? [tag]` | 050 | ask `tools/heapshim.c` for live/peak/allocation counts. Declared weak, so the binary still runs without the `LD_PRELOAD` shim. |
| `depth?` | 050 | print the current dungeon level. |
| `jump N` | 050 | queue `CMD_WIZ_JUMP_LEVEL`, bypassing the `Ctrl-A` confirmation. |
| `screen? [tag]` | 060 | dump the whole `Term` grid as text, one `SCR\|` line per row. |
| `sidebar N` | 060 | set `angband_term[0]->sidebar_mode` and `do_cmd_redraw()`. |
| `size?` | 060 | print the term size, so a capture carries its geometry. |
| `key escape` | 060 | `ESCAPE`, which leaves every screen the layout sweep visits. |
| `-s WxH` | 060 | the term geometry. Default 80x24, so `tests/` is unaffected. |
| `-b N` | 060 | the initial sidebar mode. Default `SIDEBAR_LEFT`, as `term_init()` leaves it. |

Plus `#include "cmd-core.h"` and `#include "ui-command.h"`.

**`screen?` is what stage 060 was written from.** The stage's job was to find what a
64-column term throws away, and `Term_putstr()` clips at `Term->wid` and reports nothing —
so the only way to see the damage is to render the screen and look at it. Rendering it at
80 as well, and printing the text that lives at column 64 or beyond, turns "what is cut
off" into a list. `tools/screen-sweep.sh` does exactly that.


### 4. `src/game/ui-term.c` — `Term_fresh` and its row scan run from SRAM

Stage 045. **Placement only. No logic, no control flow and no behaviour is changed**, and on
every platform except `PICOCALC` the macro expands to nothing at all:

```c
#ifdef PICOCALC
#define PICO_TERM_HOT(f) __attribute__((section(".time_critical." #f))) f
#else
#define PICO_TERM_HOT(f) f
#endif
```

applied to `Term_fresh()` and `Term_fresh_row_text()`.

**Why.** The RP2350's flash and its 8 MB of PSRAM hang off one QMI, on chip selects 0 and 1,
and CS1 carries a `MAX_SELECT` / `MIN_DESELECT` / `COOLDOWN` timing contract that the SDK's
`psram.c` programs. Every switch between the two costs a deselect and a reselect. This port's
heap is PSRAM, so a `term_win`'s four planes are in PSRAM — while `ui-term.c` itself executes
from flash. `Term_fresh_row_text()` therefore alternates chip selects on **every cell it
scans**.

It is not a small effect. `angband_termdiag` measured 8,192 words summed three ways:

| Source | Per access |
|---|---|
| SRAM | 113 ns |
| PSRAM, sequential | 351 ns |
| PSRAM alternating with flash | **7,766 ns** |

A 64x32 `Term_fresh()` was spending about **40 ms** outside the front end's drawing hooks,
against roughly 4 ms of actual comparing and copying. That is 2,048 cells at very close to the
alternating rate, and it was the single largest item left in the frame after the LCD transfer
was fixed.

**Not the SDK's `__not_in_flash_func`.** `tools/compile-sweep.sh` compiles `src/game/` with no
SDK include path, so `pico/platform.h` is not reachable from a core source. The section name
is what the SDK's linker script places in RAM, and the bare attribute is all that macro
expands to, so the effect is identical and the sweep still compiles 150 of 150.

**Cost:** about 1.5 KB of SRAM, of which this port has roughly 480 KB spare.

**If this is ever reverted**, the frame time goes back up by about 36 ms and nothing else
changes. It is safe to drop on a part where the heap and the code share one memory.

### 5. Ten `src/game/ui-*.c` files — the 64-column layout

Stage 060. The term is 64x32 and upstream assumes 80x24 (`specifications.md` §4). Ten user
interface files put something at a column past 63, and at 64 `Term_putstr()` clips it and
says nothing, so the loss is silent. **Every hunk below is guarded by `Term->wid < 80`**,
except the one in `ui-init.c`, which is guarded by `PICOCALC` and is explained there.

The port gets eight rows back in exchange for sixteen columns, and the narrow layouts spend
them: what upstream puts side by side, they put one above the other.

| File | What moves under 64 columns |
|---|---|
| `ui-display.c` | `update_topbar()` takes **three rows, not two** — level and experience, armour class, gold and the race and class title on the first, the five stats on the second, HP, mana, the health bar, speed and depth on the third. Upstream's first row is 84 columns at its maximum, so armour class and gold fell off it. `update_statusline()` follows to row 4. `show_splashscreen()` clamps `text_out_indent` at 0; `(Term->wid - 80) / 2` is **-8** here, and a negative column is silently refused, which put the whole of `news.txt` in one cell. |
| `ui-term.h` | `ROW_MAP` allows for the third top-bar row. Everything derived from the map's top row goes through that macro, so it is the only place that has to know. |
| `ui-player.c` | The character sheet, the one screen stage 050 found genuinely broken. The stat table moves from row 2 column 42 to row 9 column 26; the combat and skills panels come down below the panels they used to sit beside; the history wraps at `Term->wid - 1`; the prompt goes on the last row rather than row 23. Page two's four 19-column resistance panels need 76 columns, so they become **two panels on each of two pages** — `INFO_SCREENS` 3 instead of 2 — and the per-region entry cap is taken against `Term->hgt` instead of a hardcoded 22. `write_character_dump()` follows the same layout. |
| `ui-birth.c` | The point-buy Cost and Total columns follow the stat table. The menu instructions and three centred prompts are said shortly enough to fit, and `prt_centred_prompt()` never asks for a negative column — `Term->wid / 2 - strlen(prompt) / 2` is -3 for a 71-character prompt at 64, which is the splash-screen defect again. The menu hint wraps to two rows, which `clear_question()` and `birth_menu_handler()` already allow for. |
| `ui-knowledge.c` | `know_col()` moves every fixed right-hand column left by `80 - Term->wid`, so the block keeps its internal spacing and the header still sits over its data: the object list's Ignore, Inscribed and symbol, the monster list's symbol, kills and "fully known", the feature and trap lists' lighting symbols, and the visual-mode readout. `know_prt_name()` then clips the name so it cannot run into the block. The browser's key prompt has a short form. |
| `ui-object.c` | `item_menu()`'s `ex_offset` accounts for the three columns `ui-menu.c` spends on the `a) ` tag. Upstream leaves them out, so the last three characters of the last extra field fall off: inventory weights read `3.0 l`. |
| `ui-spell.c` | A 24-column spell-name field, not 30, and a header to match. The menu is `Term->wid - 15` wide and loses three more to the tag, so ` difficult` was arriving as `dif`. |
| `ui-death.c` | The death menu goes at `Term->wid - 17`; at upstream's column 51 its longest entry runs to 66. |
| `ui-input.c` | A short form of the character-name prompt, which is 54 columns and has the name typed after it. |
| `ui-init.c` | **The only one not conditional on width.** `plog("Main window is too small")` is `#ifndef PICOCALC`. It fired on every boot, the game walked straight past it — it is a `plog()`, not a `quit()` — and stage 050 lost several minutes to it mid-session because it reads like a blocker. Now that every screen has a 64-column layout the warning is wrong. The guard is on the platform and not on `Term->wid` because on a front end that can be resized the warning is still true: a user who has dragged a window to 64 columns can undo it, and a soldered 320x320 panel cannot. |

### 6. `cave-view.c`, `game-world.c` and `ui-game.c` — the stage 070 turn instrument

Stage 070. Measurement only: **no logic changes anywhere in these three files**, and with
`ANGBAND_TURN_LOG` unset every `TURNLOG_*` macro is `((void)0)`. The declarations are in
`src/platform/turnlog.h`, which needs nothing but `<stdint.h>` — a core source cannot
include an SDK header (stage 045: `tools/compile-sweep.sh` compiles `src/game/` with no SDK
include path) — and the definitions are in `src/platform/turnlog.c`.

| File | What is bracketed |
|---|---|
| `ui-game.c` | `TURNLOG_BEGIN()` / `TURNLOG_END(player->depth, turn)` around `run_game_loop()` in `play_game()`, and since harness stage 055 `ANGBAND_SYNC_END(turn)` on the line after -- the lockstep witness, under `ANGBAND_SYNC`, `((void)0)` with the option off, declared in `src/platform/sync.h` under the same `<stdint.h>`-only rule as `turnlog.h`. **This is the only place in the tree where one player command is exactly one iteration**: the loop is `pre_turn_refresh(); cmd_get_hook(CTX_GAME); run_game_loop();`, so the blocking wait for a key is in `cmd_get_hook()` and outside the bracket, and the `printf` is in `turnlog_end()` and therefore after `run_game_loop()` has returned. |
| `cave-view.c` | Three of the seven per-step sweeps: `mark_wasseen()` and `calc_lighting()` at their call sites inside `update_view()`, and the `update_view` main loop. |
| `game-world.c` | Four more: `forget_noise()` and the `make_noise()` flood (kept **disjoint** by closing the `make_noise` bracket before `forget_noise()` and reopening it after), `update_scent()` and the trap-timeout loop in `process_world()`. Plus three containers: `process_world()` itself, all four `process_monsters()`/`reset_monsters()` call sites in `run_game_loop()`, and `prepare_next_level()`. |

`src/platform/main-pico.c` is edited too — it is port-written, not vendored, so it is not
one of the seventeen. `check_events(wait)` is the only place this program can block on the
keyboard and `TERM_XTRA_DELAY` the only place it deliberately sleeps; both report through
`turnlog_idle()`, and `turnlog_end()` subtracts the total. Without that a turn's wall clock
would be mostly the player's thinking time.

**The mark for `make_noise` is deliberately above `q_new()`**, because that call mallocs and
frees `cave->height * cave->width` ints — 52,276 bytes of PSRAM — on every player step, and
stage 070 item 5 wants the cost measured before it is hoisted.

*(Stage 070 item 5 has since hoisted that queue; see section 7.)*

### 7. `cave.h`, `cave.c`, `cave-square.c`, `cave-view.c`, `game-world.c` — stage 070 speed edits

Stage 070 run 3, 2026-09-13. **These are the only edits in this tree that change what the
game code does rather than where it runs or what it draws**, and each one was held to two
checks before any device time: the desk pre-check (`host-build.sh --borg --arm`, replay
`borg-arm.keys`, `cmp` the `.sync` — identical over 1,001 commands for every one), and a
bench round in lockstep. Item 10's two also passed a host self-test described below.

| Item | File | Edit |
|---|---|---|
| 4 | `cave.h`, `cave.c`, `cave-square.c` | `struct square`'s `bitflag *info` becomes `bitflag info[SQUARE_SIZE]`. The per-grid `mem_zalloc` in `cave_new()` and the `mem_free` in `cave_free()` go (and the loop variable `x` with them); `square()` returns `struct square *`, not `const`, because an array member of a const struct decays to `const bitflag *`. `load.c` and `save.c` index the array and are untouched, so the save format is unchanged. `struct connector`'s own `info` pointer is a different thing and is untouched. |
| 5 | `game-world.c` | `make_noise()` keeps one `struct queue` for the program instead of `q_new()`/`q_free()` of `height * width` entries per step, and empties it on entry. `forget_noise()` zeroes each interior row with one `memset`. |
| 10a | `cave.h`, `cave-view.c` | `struct chunk` gains `view_valid`, `view_min`, `view_max` — the box the last `update_view()` worked in, zeroed by `cave_new()`. `mark_wasseen()` sweeps that box, and the `update_view()` main loop sweeps the hull of it and the new box (the player's grid ± `z_info->max_sight`). An invalid box is the whole level, which is upstream's loop exactly. |
| 10b | `cave-view.c` | `calc_lighting()`'s permanent-light pass runs over the new box plus one grid; the light indicator is redrawn unconditionally when the player's grid was outside the last box. |

**Why 10a is exact.** Only `update_view()` sets `SQUARE_VIEW`, `SQUARE_SEEN`,
`SQUARE_CLOSE_PLAYER` and `SQUARE_WASSEEN` (grep: every other writer clears them), and it can
set them only inside the new box, because `update_view_one()` returns at once when
`distance() > max_sight` and `distance()` is never less than the larger of |dx| and |dy|.
`update_one()` does nothing to a grid with neither `SEEN` nor `WASSEEN`. So the loop visits,
in upstream's row-major order, every grid where it can have an effect. Every chunk the game
can make current comes out of `cave_new()`, so a loaded, generated or copied chunk gets one
full sweep first.

**Why 10b is exact where it is read.** A grid's `light` is read only at a `SEEN` grid
(`map_info()`), at the player's grid (`ui-display.c`, `player-spell.c`), and inside
`update_view()` at grids of the box. Grids outside keep stale values that nothing reads
before a later pass resets them. The one read that can see a stale value is `old_light`
after a teleport, and that only decides whether `PR_LIGHT` is set; setting it anyway redraws
the indicator with its correct value.

**The self-test**, scaffolding that is not committed: a host build in which every
`update_view()` call snapshots all squares, runs the full sweep, restores, runs the bounded
one, and compares the four flags on every grid and `light` on every grid of the box. Over
1,001 replayed commands: **1,649 calls, 1,609 of them bounded, 0 differing flags, 0
differing light values.** The same harness with the lighting box deliberately shrunk by four
grids reported 913 bad calls and 3,008 differing light values, so it can see a fault. The
script is `.llm/scratch/s070r3/view-selftest.c`.

**The data files are 64 columns too.** `lib/screens/news.txt` is re-drawn (its colour markup
counts against the line length, so the art had to lose about sixteen columns and the quote
is re-wrapped); `dead.txt` and `retire.txt` are trimmed; `lib/help/*.txt` is re-wrapped by
`tools/rewrap-help.py`, which re-flows prose paragraphs and turns the two- and three-column
key tables into one entry per line. `awk 'length > 64' lib/help/*.txt lib/screens/*.txt`
prints nothing.

## The platform layer

`src/platform/` is not vendored from Angband. It is the PicoCalc hardware layer, shared with
the sibling ports (`../.llm/projects/angband-picocalc/specifications.md` §4, platform layer
source: copy from `zangband-pico/` when the file exists there, else from `tinyrogue-pico/`;
never write a second LCD driver).

| File | Origin | Stage |
|---|---|---|
| `lcd.c`, `lcd.h` | copied byte-identical from `../tinyrogue-pico/src/platform/`; **changed there by stage 045 and re-copied** | 020, 045 |
| `font5x10.c`, `font5x10.h` | copied byte-identical from `../tinyrogue-pico/src/platform/` | 020 |
| `southbridge.c`, `southbridge.h` | copied byte-identical from `../tinyrogue-pico/src/platform/` | 020 |
| `psram_heap.c`, `psram_heap.h` | port-written here | 020 |
| `sd_fs.c`, `sd_fs.h` | port-written here | 030 |
| `syscalls.c`, `syscalls.h` | port-written here | 030 |
| `keyboard.c`, `keyboard.h` | copied byte-identical from `../tinyrogue-pico/src/platform/` | 040 |
| `main-pico.c`, `main-pico.h` | port-written here | 040 |
| `utf8.c`, `utf8.h` | port-written here | 040 |

Nothing was added to `src/platform/` by stage 050: the game's entry point is
`src/main.c`, not a platform file, because it is Angband's `main()` and not hardware.

The four copied pairs carry tinyrogue's own `PORT:` banners, which name their upstream
(Picoware `841d9c56`, whose own upstream is
https://github.com/BlairLeduc/picocalc-text-starter, MIT) and every change tinyrogue made:
the PIO transport replaced by hardware SPI on `spi1` at 25 MHz, and `pico/multicore.h`
dropped. **Nothing is edited in this tree.** `tools/platform-cmp.sh` is the check — it
compares every copied file against its source tree and exits non-zero on a difference.

**`lcd.c/.h` were changed by angband-pico's stage 045 (2026-09-10), and the change was
made in `../tinyrogue-pico/src/platform/` first and copied down.** That is Route A of the
two the stage plan set out, taken with the user's agreement: the panel, the driver and
the defect are the same in both ports, so the fix belongs upstream of both, and
`platform-cmp.sh` stays at 8 of 8 with no deviation to record. The change is the pixel
transfer — `lcd_write16_buf()` now sends 16-bit frames by DMA in SPI mode 3 instead of
repacking bytes into a 64-byte buffer and calling `spi_write_blocking()` 3,200 times per
screen — plus `LCD_BAUDRATE`, which is now chosen per part: **37.5 MHz on the RP2350,
25 MHz unchanged on the RP2040.** The clock is split because no RP2040 is fitted to the
PicoCalc today, so raising tinyrogue's would be an untestable change to a shipping port;
the transfer rewrite is what both parts share. `lcd.c` now needs `hardware_dma`, which
was added to `angband_platform` here and to `tinyrogue_platform` there. The measurements
that drove it are in `specifications.md` §6.4.
`zangband-pico/` had no `src/platform/` when stage 020 ran, and still had none when stage 040
copied `keyboard.c/.h`, so tinyrogue was the source for all four pairs; when the Zangband
port reaches its own stage 020, these files and `psram_heap.c/.h` are what it should copy.

`keyboard.c` is tinyrogue's, unchanged, and it is deliberately Angband-unaware: it reports
`(scan code, shift, ctrl, alt)` with the modifiers latched at the moment the key was queued,
and `main-pico.c` decides what that means. Its header says why Picoware's own keyboard driver
was not vendored -- it folds Shift into the character and then throws the modifier away in a
file-private static.

`main-pico.c` is the file upstream calls `main-xxx.c`: the four Term hooks, the key map and
`init_pico()`. `utf8.c` is the text-encoding half. Both include core headers, so neither is a
member of `angband_platform` -- they are sources on the executables that link core code. The
front end is specified in `specifications.md` 6.4; the two decisions in it that are not
obvious from the code are that the UTF-8 decoder never returns -1 (a strict one would make
`Term_addstr()` draw nothing at all for a line containing one bad byte) and that Ctrl+letter
is encoded as `KTRL(c)` with `KC_MOD_CONTROL` *cleared*, following `ui-event.h:82` and
`ui-event.c`'s `STORE()` macro rather than the stage plan's wording.

`psram_heap.c` is port-written and has no upstream. It defines a strong `_sbrk` that
overrides the SDK's `__weak` one in `pico_clib_interface/newlib_interface.c`, so the whole
newlib heap lives in the 8 MB PSRAM. **It is in a static library, so the linker only pulls it
in if something references one of its symbols** — an executable that wants the PSRAM heap
must call `psram_heap_stats()` (or another function from that header) at least once.
`src/psramdiag.c` does, and the link map for `angband_psramdiag` confirms the result: the
SDK's `.text._sbrk` under "Discarded input sections", and this one at `0x10004fec`.

`syscalls.c` is the same trap in a second place, and stage 030 walked into it deliberately.
Its `_gettimeofday`, `_getpid` and `_kill` all override `__weak` SDK definitions in the same
`newlib_interface.c`, so an executable that wants a plausible `time()` must call
`syscalls_init()` — which does nothing at run time and exists only to be a name the linker
can see. `src/fsdiag.c` calls it, and its link map shows the SDK's `.text._gettimeofday`
under "Discarded input sections" with `libangband_platform.a(syscalls.c.obj)` pulled in.
**Stage 050's `main()` must call both `syscalls_init()` and a `psram_heap.h` function**,
before the first `mem_alloc` and before anything reads the clock.

`sd_fs.c` needs no such hook: `sd_fs_mount()` is called by name.

The clock `sd_fs.c` asks for is Mothpad's `125000000 / 2 / 4` = 15,625,000 Hz, and it is
kept as a literal rather than recomputed, so this port runs the card at the rate Mothpad
already proved on this wiring. It is not the rate the SPI block ends up at: `clk_peri` here
is 150 MHz, not the 125 MHz that expression assumes, and `spi_set_baudrate()` can only
divide by an even prescale times a post-divide. `sd_fs_effective_hz()` reads the achieved
rate back out of the hardware, and that is the number `specifications.md` §6.3 records.

## The stack, and the two linker-script overrides

Stage 050. `src/link/sections_stack.incl` and `src/link/section_end.incl` replace the
pico-sdk 2.3.0 files of the same names, through the SDK's own
`pico_add_linker_script_override_path()`. They apply to the `angband` executable and to
nothing else; the three diagnostics still link with the stock script.

**Why.** The SDK puts the core-0 stack at the top of `SCRATCH_Y`, which on the RP2350 is a
fixed **4 KB** memory region — so `PICO_STACK_SIZE` cannot be raised past `0x1000` whatever
is asked for, and every link in this tree before stage 050 reported `SCRATCH_Y 2 KB of 4 KB,
50.00%`. `specifications.md` §12 recorded that as the gap stage 050 had to close, and the
game cannot live in 2 KB: one frame of `ui-output.c`'s `text_out_to_screen()` is a
1024-entry `wchar_t` buffer (4 KB) plus `av[256]` and `cv[256]` (2 KB), and `init.c`,
`savefile.c` and `z-file.c` carry 1024-byte path and line buffers several calls apart.

**What.** `sections_stack.incl` sends `.stack_dummy` to `RAM` instead of `SCRATCH_Y`; that
is its only change from the SDK's copy. `section_end.incl` then recomputes `__StackTop`,
`__StackBottom` and `__StackLimit`, which the SDK's copy hard-codes to `SCRATCH_Y` and the
end of `RAM`, and replaces the `__StackLimit >= __HeapLimit` assertion — unsatisfiable once
the stack is inside `RAM`, since `section_heap.incl` sets `__HeapLimit` to the end of the
region — with the check that the stack fits. `PICO_STACK_SIZE` is then free to set the size,
and `CMakeLists.txt` sets it to `0x10000`. The core-1 stack is left in `SCRATCH_X`, untouched
and unused: this port is single-core.

**Why the linker and not an MSP switch inside `main()`.** The stage plan offered both. The
linker route means the whole program runs on the big stack **from reset**, including the
SDK's own runtime init and every interrupt; it needs no naked assembly; and the placement is
visible in the map file and in `nm`, where an MSP switch would be invisible to both.

**Why 64 KB and not the 32 KB the stage plan proposed.** The true figure was unknown until
it was measured, and on a first bring-up a stack overflow is indistinguishable from any other
hard fault. `src/main.c` paints the unused stack with `0xC5C5C5C5` at boot and reports the
high-water mark on every level; that number is what a later stage should trim this to. 64 KB
of 520 KB.

**If this is ever reverted**, the stack goes back to 2 KB in `SCRATCH_Y` and the game faults
during `init_angband()`. It is not optional on this part.

## The hot code in SRAM: two more, generated, overrides

Stage 070 item 3 (2026-09-13). `src/link/default_text_excludes.incl.in` (which also carries
item 8's `.text` pad) and `src/link/default_rodata_excludes.incl.in` replace the SDK's
one-line files of the same names. CMake writes both into `<build>/link-gen/` with every
object in `ANGBAND_SRAM_OBJECTS` (as `*/src/game/<name>.c.obj`) and every pattern in
`ANGBAND_SRAM_LIBRARY_MEMBERS` added to the `EXCLUDE_FILE` list. The SDK's own
`section_default_data.incl` already gathers `*(.text*)` and `*(.rodata*)` into `.data`,
which is `> RAM AT> RAM_STORE`, so an excluded object lands in SRAM with no third file.

**Why.** Flash and PSRAM share one QMI, and a loop running from flash while reading the
PSRAM heap pays 6,297-7,949 ns an access (`specifications.md` §6.4) — which is what the whole
game loop is. Moving the code: depth-1 turn 2,430 → 117 ms together with items 4-10 (the
placement alone, four tranches, is most of it). SRAM-resident code also stops moving with
`.text` placement, which is what makes the other items measurable.

**What is in the list**: 55 core objects (the cave, view, world, monster, map, player, UI,
object, projection and message code) and, verbatim, newlib's allocator and string members.
**This toolchain's newlib is `libg.a`, not `libc.a`**, so the SDK's `*libc.a:` exclusion never
matched and `malloc`/`free` ran from flash over PSRAM free lists; moving them took
`init_angband()` from ~204 s to ~115 s as a side effect. RAM is 472 KB of 512 on the bench
build and 468 KB on the shipped one; **the list is bounded by that, not by ambition**.

**Check with `tools/sram-check.py <build-tree>`, never by reading the list.** It looks up
every defined function and read-only symbol of every listed game object in the linked ELF
and fails on any that is not at `0x2000xxxx`. A placement that silently fails looks exactly
like one that worked (stage 045). The library members are checked by `nm` by name.

**Two traps found building it.** A pattern for `cave.c` must carry the directory separator
or it also matches `gen-cave.c` and `cmd-cave.c`. And the generated file's comment must not
name the substitution: every pattern begins with a star and a slash, which closes a C
comment, and `ld` then fails with "ignoring invalid character".

## `USE_PRIVATE_PATHS`

Stage 050, a compile definition on `angband_core` and on `angband`, not a source edit.
`init.c:388` is its only use in the whole tree, and it decides one thing: whether the save,
scores and panic directories hang off `ANGBAND_DIR_USER` (`/angband/lib/user/`) or off the
data path (`/angband/lib/`). `specifications.md` §6.3 and §8 both describe the former, and
the card staging in §8 creates exactly those directories under `user/`, so this is the switch
that makes the code agree with the card. Without it the savefile would land in
`/angband/lib/save/` and the four staged directories would sit unused.

It has nothing to do with `PRIVATE_USER_PATH`, which `config.h` defines only under `UNIX`
and which stays undefined here.

## Known deviations from a clean `PICOCALC` build

* **`<signal.h>` reaches every device translation unit.** Upstream `h-basic.h:113`
  includes it unconditionally, outside every platform guard, because `ui-signals.h`
  declares `volatile sig_atomic_t signal_count` and the core's `check_break()` reads it.
  Guarding the include would need a third `PORT:` edit and would break the build, so it
  stays. It is newlib's own header and compiles clean; `ui-signals.c` is 1,014 bytes of
  text under `PICOCALC` and links against newlib's `signal()` stub. **Stage 050 must not
  call `signals_init()`.** `<pwd.h>`, `<langinfo.h>` and `<locale.h>` reach nothing.
* **Every path handed to the core must be absolute.** `path_normalize()` (`z-file.c:221`)
  has three branches — Windows, UNIX, and an `#else` at line 571 that the port takes. That
  branch has no working-directory lookup and rejects any path not starting with `/`. The
  three `DEFAULT_*_PATH` compile definitions are `"/angband/lib/"`, so this is satisfied;
  stages 030 and 050 must not introduce a relative one.

## Building

See `../.llm/projects/angband-picocalc/specifications.md` §10. In short:

```
cd /c/Users/greenblob/Documents/PicoCalc && source tools/pico-env.sh
angband-pico/tools/compile-sweep.sh      # the suite, part 1: cross-compile sweep
angband-pico/tools/host-build.sh         # the suite, part 2: WSL host build, tests, heap probe
angband-pico/tools/platform-cmp.sh       # src/platform/ still byte-identical to its source tree
angband-pico/tools/screen-sweep.sh       # the suite, part 4: every screen rendered at 80 and 64
angband-pico/tools/port-warnings.sh      # the suite, part 5: the port's own sources at -Wall -Wextra
angband-pico/tools/host-build.sh --borg  # harness stage 055: the PC-side borg, host only
angband-pico/tools/borg-run.sh birth     # and the savefile a lockstep run loads on both sides
cmake --build angband-pico/build-pico2 --target angband_core
cmake --build angband-pico/build-pico2 --target angband_psramdiag
cmake --build angband-pico/build-pico2 --target angband_fsdiag
cmake --build angband-pico/build-pico2 --target angband_termdiag
cmake --build angband-pico/build-pico2 --target angband        # the game
```
