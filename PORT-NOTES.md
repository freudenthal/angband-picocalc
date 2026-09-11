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
| `src/host/` | Upstream `main.c` and `main-test.c`. **WSL harness only**; never compiled for the device. |
| `src/platform/` | The platform layer. `lcd.c/.h`, `font5x10.c/.h`, `southbridge.c/.h` copied byte-identical from `../tinyrogue-pico/src/platform/`; `psram_heap.c/.h`, `sd_fs.c/.h`, `syscalls.c/.h` port-written. Stages 020, 030. |
| `src/psramdiag.c` | The stage 020 PSRAM diagnostic. Port-written. |
| `src/fsdiag.c` | The stage 030 SD filesystem diagnostic. Port-written. |
| `lib/` | Game data. Goes on the SD card at `/angband/lib/`. |
| `tests/` | Upstream end-to-end tests plus the top-level `run-tests` runner, moved to `tests/run-tests`. |
| `tools/` | The suite and the heap probe. Port-written. |
| `copying.txt` | Upstream `docs/copying.rst`, renamed. |
| `build-sweep/`, `build-host/`, `build-pico2/` | Generated. Gitignored. |

## What was filtered out

Directories deleted from `src/` (other front ends, the borg, the unit tests, and the
upstream build systems, none of which the port uses):

| Path | Files |
|---|---|
| `src/borg/` | 118 |
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

Four vendored files are edited. Each edit is wrapped in a `PORT:` banner.
`git diff --stat 2cf1b4a HEAD -- src/game src/host` must list exactly these four.

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

### 3. `src/host/main-test.c` — three harness probes

`heap?`, `depth?` and `jump N`, plus `#include "cmd-core.h"`. They exist so
`tools/host-build.sh` can reproduce the heap table in `specifications.md` §7.1 without a
device. `heapshim_report()` is declared weak, so the binary still runs without the
`LD_PRELOAD` shim. This file is host-only and never reaches the firmware.


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
cmake --build angband-pico/build-pico2 --target angband_core
cmake --build angband-pico/build-pico2 --target angband_psramdiag
cmake --build angband-pico/build-pico2 --target angband_fsdiag
cmake --build angband-pico/build-pico2 --target angband_termdiag
```
