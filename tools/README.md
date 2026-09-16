# tools/

Every file in this directory (`ls tools | wc -l` is 22; this table has 22 rows, this file
included). "Suite" means it is one of the five parts of the regression suite
(`specifications.md` §9); "bench" means it is part of the optimisation-round tooling from
stage 065 onward and needs a device and the development workspace's
`picocalc-device-harness` project (a sibling of this repository, not shipped with it);
"release" means it is part of building or staging a release; "data" means it is not a
program, but a file another tool reads.

| Tool | What it does | Needs | Group |
|---|---|---|---|
| `README.md` | This file. | Nothing. | Documentation |
| `compile-sweep.sh` | Cross-compiles every `src/game/` unit for the RP2350 with the project's flags; reports `N of M compiled`. | Nothing but the ARM toolchain (`tools/pico-env.sh`). | Suite (part 1) |
| `host-build.sh` | Builds the vendored core natively in WSL, links it against upstream's `main.c`/`main-test.c`, runs the end-to-end tests under `tests/`, and runs the heap probe. `--borg` builds the separate PC-side borg binary instead (bench prerequisite). | WSL. | Suite (part 2) |
| `heapshim.c` | An `LD_PRELOAD` malloc counter, compiled and used by `host-build.sh`. Not run directly. | WSL, via `host-build.sh`. | Suite (data/support) |
| `heap-probe.in` | A stdin script for `build-host/angband-test -mtest`: birth, jump through several depths, `heap?` at each. Read by `host-build.sh`. | WSL, via `host-build.sh`. | Suite (data) |
| `utf8-test.c` | Host unit checks for `src/platform/utf8.c`, built and run by `host-build.sh`; reports `utf8-test N/M checks passed`. | WSL, via `host-build.sh`. | Suite (part 2) |
| `battery-test.c` | Host unit checks for `src/platform/battery.h`, built and run by `host-build.sh`; reports `battery-test N/N checks passed`. | WSL, via `host-build.sh`. | Suite (part 2) |
| `platform-cmp.sh` | Compares every copied file in `src/platform/` against its source tree (`zangband-pico/`, then `tinyrogue-pico/`); reports `N of M identical`. Exits 0 with a `skipped: no sibling tree` line when run from a clone with neither sibling present. | Nothing; the sibling trees are optional. | Suite (part 3) |
| `screen-sweep.sh` | Renders every screen at 80x32 and 64x32 through the host harness and reports the text the 64-column term throws away. | WSL, and `host-build.sh` must have run first (reuses `build-host/`). | Suite (part 4) |
| `screens.in` | A stdin script for `build-host/angband-test -mtest`, read by `screen-sweep.sh`. | Read by `screen-sweep.sh`. | Suite (data) |
| `port-warnings.sh` | Recompiles the port's own sources with `-Wall -Wextra`, using each file's real compile command out of a configured build tree; reports `N warnings or errors in N port sources`. | A configured `build-pico2` and `build-pico2-console` (or `PORT_WARNINGS_BUILD`). | Suite (part 5) |
| `stage-card.sh` | Stages the release UF2 and `lib/` into `sdcard-pico2/`, with the checks listed in `BUILDING.md` (**Prepare a card**). `STAGE_DIR` and `PICOTOOL` override the staging directory and the picotool binary. `--card E:` also diffs a real card, read-only. | A built `build-pico2/angband.uf2`; `--card` needs the SD card in the PC. | Release |
| `make-release.sh` | `make-release.sh <tag>`: runs `stage-card.sh`, then writes `release/angband-picocalc-<tag>.zip`, `release/uf2loader-2.5-pimoroni_pico_plus2_w.zip` and `release/SHA256SUMS`, with the checks listed in `BUILDING.md` (**Make a release**). Refuses a dirty tree, a tag that is not HEAD, and loader files that are not the pinned ones. | A clean checkout with the tag on HEAD, a built `build-pico2`, Python 3, and the two loader files (`LOADER_DIR`). | Release |
| `rewrap-help.py` | Re-wraps `lib/help/*.txt` and re-flows key tables for a narrower column count. A one-off maintenance tool from stage 060, not run by any other tool or by the suite. | Nothing; a local Python 3. | Maintenance |
| `build-bench.sh` | Builds the four bench binaries (`.text` pad 0/2/4/6 KB) from the current tree, all built with the bench recipe. | The ARM toolchain and pico-sdk/picotool paths (`tools/pico-env.sh`); not the device itself. | Bench |
| `borg-bench.py` | Runs one optimisation round end to end: checks the build tree is the bench recipe, runs the desk-side pre-check, flashes, boots, proves the savefile restored, plays commands in lockstep, reduces the capture, writes the table. `--selftest` demonstrates its four refusals without a device. | A device, and the development workspace's `picocalc-device-harness` project (`tools/harness/` at the workspace root). **Not part of the release.** | Bench |
| `borg-run.sh` | Drives the WSL-built PC-side borg (`build-host-borg/angband-borg`) from Git Bash, for the desk-side pre-check that a `src/game/` edit has not changed the game's behaviour. | WSL. **Not part of the release.** | Bench |
| `borg.txt` | The borg's own default settings, written out from `src/game/borg/borg-init.c` so the harness's dependency on them is explicit rather than inherited. Read by the harness project. | Read by the workspace harness. | Bench (data) |
| `turnlog.py` | Reduces an `ANGBAND_TURN_LOG` serial capture to per-phase medians and percentiles; `--compare` diffs two captures. | A device capture, and the development workspace's own convention for where captured bench tags are stored. **Not part of the release.** | Bench |
| `serial-capture.py` | Saves the board's serial output to a file while it happens, for later reduction by `turnlog.py`. | A device and its serial port. **Not part of the release.** | Bench |
| `sram-check.py` | Reads `ANGBAND_SRAM_OBJECTS` out of `CMakeLists.txt` and proves every symbol of every listed object linked into a given build tree is at an SRAM address. | A configured build tree; no device. **Not part of the release.** | Bench |
| `bootprof.py` | Reduces an `ANGBAND_BOOT_PROFILE` serial capture to phase, parser, function, object and caller tables, resolving addresses with `arm-none-eabi-nm` and the tree's `.map` file. `--selftest` proves the reduction itself against a synthetic capture, no device. | A device capture and the matching build tree (`angband.elf`/`.map`); no device for `--selftest`. **Not part of the release.** | Bench |

None of the "not part of the release" tools are needed to build or play the game; they say
so in their own header comment. A standalone clone can ignore them entirely and still pass
the five-part suite.
