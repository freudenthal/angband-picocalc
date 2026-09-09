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
| `src/platform/` | The platform layer. `lcd.c/.h`, `font5x10.c/.h`, `southbridge.c/.h` copied byte-identical from `../tinyrogue-pico/src/platform/`; `psram_heap.c/.h` port-written. Stage 020. |
| `src/psramdiag.c` | The stage 020 PSRAM diagnostic. Port-written. |
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

Two vendored files are edited. Each edit is wrapped in a `PORT:` banner.
`git diff --stat 2cf1b4a HEAD -- src/game src/host` must list exactly these two.

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

### 2. `src/host/main-test.c` — three harness probes

`heap?`, `depth?` and `jump N`, plus `#include "cmd-core.h"`. They exist so
`tools/host-build.sh` can reproduce the heap table in `specifications.md` §7.1 without a
device. `heapshim_report()` is declared weak, so the binary still runs without the
`LD_PRELOAD` shim. This file is host-only and never reaches the firmware.

## The platform layer

`src/platform/` is not vendored from Angband. It is the PicoCalc hardware layer, shared with
the sibling ports (`../.llm/projects/angband-picocalc/specifications.md` §4, platform layer
source: copy from `zangband-pico/` when the file exists there, else from `tinyrogue-pico/`;
never write a second LCD driver).

| File | Origin | Stage |
|---|---|---|
| `lcd.c`, `lcd.h` | copied byte-identical from `../tinyrogue-pico/src/platform/` | 020 |
| `font5x10.c`, `font5x10.h` | copied byte-identical from `../tinyrogue-pico/src/platform/` | 020 |
| `southbridge.c`, `southbridge.h` | copied byte-identical from `../tinyrogue-pico/src/platform/` | 020 |
| `psram_heap.c`, `psram_heap.h` | port-written here | 020 |

The three copied pairs carry tinyrogue's own `PORT:` banners, which name their upstream
(Picoware `841d9c56`, whose own upstream is
https://github.com/BlairLeduc/picocalc-text-starter, MIT) and every change tinyrogue made:
the PIO transport replaced by hardware SPI on `spi1` at 25 MHz, and `pico/multicore.h`
dropped. **No further change was made here.** `tools/platform-cmp.sh` is the check — it
compares every copied file against its source tree and exits non-zero on a difference.
`zangband-pico/` had no `src/platform/` when stage 020 ran, so tinyrogue was the source for
all three pairs; when the Zangband port reaches its own stage 020, these files and
`psram_heap.c/.h` are what it should copy.

`psram_heap.c` is port-written and has no upstream. It defines a strong `_sbrk` that
overrides the SDK's `__weak` one in `pico_clib_interface/newlib_interface.c`, so the whole
newlib heap lives in the 8 MB PSRAM. **It is in a static library, so the linker only pulls it
in if something references one of its symbols** — an executable that wants the PSRAM heap
must call `psram_heap_stats()` (or another function from that header) at least once.
`src/psramdiag.c` does, and the link map for `angband_psramdiag` confirms the result: the
SDK's `.text._sbrk` under "Discarded input sections", and this one at `0x10004fec`.

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
```
