# Build Angband for the PicoCalc

This document tells you how to build `angband.uf2` from source, how to run the tests, and how
to prepare a card. To install a release, read [INSTALL.md](INSTALL.md). Each change to an
upstream Angband file is listed in [PORT-NOTES.md](PORT-NOTES.md).

## Tools

| Tool | Version | Note |
|---|---|---|
| Arm GNU Toolchain (`arm-none-eabi-gcc`) | 14.2.rel1 | The build uses `-mcpu=cortex-m33`. |
| CMake | 3.13 or newer | Tested with 4.4.3. |
| Ninja | any | Tested with 1.13.2. |
| pico-sdk | **2.3.0** | Version 2.2.0 does not work. It has no `hardware_psram`, no PSRAM start-up code and no PSRAM linker region. |
| picotool | **2.3.0** | pico-sdk 2.3.0 does not accept picotool 2.2.0 at configure time. |
| Git | any | For the clone and the submodules. |
| Python 3 | any | As `python`, `python3` or `py` on `PATH`. For test part 5 and some tools in `tools/`. |
| WSL (Ubuntu) or Linux, with gcc, make and bash | gcc 13.3 tested | Only for the test build on the PC (`tools/host-build.sh` and `tools/screen-sweep.sh`). |

The commands in this document are for Git Bash on Windows. On Linux, use the same commands
without `.exe`.

Put `arm-none-eabi-gcc`, `cmake`, `ninja` and `picotool` on your `PATH`. As an alternative
for the compiler, set `PICO_TOOLCHAIN_PATH` to the toolchain directory.

`picotool_DIR` must be the directory that holds `picotoolConfig.cmake`. For a picotool
2.3.0 that you built and installed yourself, this is `<install prefix>/lib/cmake/picotool`.
It is not the directory that holds `picotool.exe`, unless both files are in the same
directory.

## Get the source

```bash
git clone --recurse-submodules https://github.com/freudenthal/angband-picocalc angband-picocalc
cd angband-picocalc
git submodule update --init --recursive
```

The second command is necessary if you did not use `--recurse-submodules`. It is also
necessary after each `git pull`, because a pull does not update a submodule. pico-vfs is in
`external/pico-vfs`, and it has its own submodule, `vendor/littlefs`.

On Windows, put the clone in a directory with a short path, for example
`C:/src/angband-picocalc`. Some object files of the build have long names. If the full path
of a file is longer than 260 characters, the compiler stops with `fatal error: opening
dependency file ... No such file or directory`. A clone path of 59 characters works. A clone
path of 152 characters does not work.

## Configure

Configure one time for each build directory:

```bash
cmake -G Ninja -B build-pico2 -S . \
  -DPICO_SDK_PATH=<path to pico-sdk 2.3.0> \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 -DPICO_PLATFORM=rp2350 \
  -Dpicotool_DIR=<path to the directory with picotoolConfig.cmake> \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--print-memory-usage"
```

On Windows, give each path with forward slashes, for example `C:/src/pico-sdk`.

The configure output must show `Build type is MinSizeRel`. The board must be
`pimoroni_pico_plus2_w_rp2350`. A different board gives a program that does not start on the
PicoCalc.

## Build the game

```bash
cmake --build build-pico2 --target angband
```

The result is `build-pico2/angband.uf2`. This is the release build. Every `ANGBAND_*` option
is off. At the end of the link, the memory report shows `RAM` at about 88 % and `FLASH` at
about 6 %.

To make sure that the file is for the correct processor, run:

```bash
picotool info build-pico2/angband.uf2
```

The first line must show `family ID 'rp2350-arm-s'`.

A build from a fresh clone gives the same `.text` as a build of the same commit in a
different directory. The compile option `-ffile-prefix-map` removes the checkout path from
the binary. `build-pico2/angband.uf2` is not identical byte for byte between two days,
because `buildid.c` contains the build date. To compare two builds, compare `.text`:

```bash
arm-none-eabi-objcopy -O binary --only-section=.text build-pico2/angband.elf a.bin
```

## Build the developer console

The release build has no serial console. A console build is for development only. Put it in
a different build directory:

```bash
cmake -G Ninja -B build-pico2-console -S . \
  -DPICO_SDK_PATH=<path to pico-sdk 2.3.0> \
  -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 -DPICO_PLATFORM=rp2350 \
  -Dpicotool_DIR=<path to the directory with picotoolConfig.cmake> \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--print-memory-usage" \
  -DANGBAND_CONSOLE=ON
cmake --build build-pico2-console --target angband
```

With `ANGBAND_CONSOLE=ON`, the game:

* has serial output on USB (the USB-C socket on the Pico Plus 2 W) and on UART0 (the USB-C
  socket on the PicoCalc case), at 115200 baud, 8N1;
* waits up to 10 seconds for a USB terminal before it starts;
* shows start-up lines, memory reports, and an `init ... heap ... stack` line on row 0.

Do not open the USB serial port at 1200 baud. This puts the board in BOOTSEL mode.

The options `ANGBAND_SERIAL_KEYS`, `ANGBAND_SERIAL_SCREEN`, `ANGBAND_SYNC`,
`ANGBAND_TURN_LOG` and `ANGBAND_SAVE_RESTORE` are for the test harness and the performance
bench. They need `ANGBAND_CONSOLE=ON`. CMake stops with an error if it is off.
`ANGBAND_SAVE_RESTORE` replaces the savefile at each start. Do not use it with a card that
has a game you want to keep.

## Put a build on the board

There are three methods:

* **The loader menu.** Copy the UF2 to `pico2-apps\` on the card. Hold **Up** and set the
  power switch to on. Select the program.
* **BOOTSEL mode.** Hold **Down** (or **F3**) and set the power switch to on. Connect the
  USB-C cable to the Pico Plus 2 W. Copy the UF2 to the **RP2350** drive. On the RP2350, the
  UF2 Loader stays in flash.
* **picotool.** This works only when a **console build** is running, because only a console
  build has USB:

  ```bash
  picotool load -x -f build-pico2-console/angband.uf2
  ```

  The release build has no USB. To replace a release build, use the loader menu or BOOTSEL
  mode.

  This command needs a picotool with USB support. `picotool version` shows `compiled without
  USB support` if it has none. A picotool without USB support can configure the build and
  run `picotool info`, but it cannot run `picotool load`.

`angband.uf2` and `lib/` must always go on the card together. If they come from different
commits, some screens can look wrong.

## Run the tests

The test suite has five parts. Run all five before a release. Run them from the root of the
clone.

| Part | Command | Needs | Result when it passes |
|---|---|---|---|
| 1 | `tools/compile-sweep.sh` | `arm-none-eabi-gcc` on `PATH` | `150 of 150 compiled`, 0 warnings |
| 2 | `tools/host-build.sh` | WSL | `utf8-test 38/38`, `battery-test 43/43`, 5 end-to-end tests `Passed`, `host-build: exit 0` |
| 3 | `tools/platform-cmp.sh` | Nothing | `skipped: no sibling tree` in a standalone clone |
| 4 | `tools/screen-sweep.sh` | WSL, and part 2 first | `screen-sweep: exit 0` |
| 5 | `tools/port-warnings.sh` | Python 3, and `build-pico2` and `build-pico2-console` configured | `0 warnings or errors` |

* **Part 1** compiles each file in `src/game/` for the RP2350.
* **Part 2** builds the game for the PC in WSL, runs the unit checks and the end-to-end tests
  in `tests/`, and runs a heap probe.
* **Part 3** compares the files copied into `src/platform/` with the sibling projects they
  came from. A standalone clone does not have these projects, so the check skips.
* **Part 4** draws each screen at 80x32 and at 64x32, and prints the text that the 64-column
  screen does not show. The number of rows it prints changes between runs, because each run
  makes a different dungeon.
* **Part 5** compiles the port's own sources with `-Wall -Wextra`.

`tools/README.md` describes each file in `tools/`.

## Prepare a card

```bash
tools/stage-card.sh
```

This copies `build-pico2/angband.uf2` to `sdcard-pico2/pico2-apps/` and `lib/` to
`sdcard-pico2/angband/lib/`. The `sdcard-pico2` directory is beside the clone. Set
`STAGE_DIR` to use a different directory. Set `PICOTOOL` if `picotool` is not on your `PATH`.

The script stops with exit 1 if one of these checks fails:

* The UF2 family is `rp2350-arm-s`, and the UF2 is newer than each source file.
* `build-pico2` is the release build, with every `ANGBAND_*` option off.
* There is no `tiles/`, `sounds/`, `fonts/`, `icons/` or `Makefile` in the staged `lib/`.
* `user/save`, `user/scores`, `user/archive` and `user/panic` are empty.
* No line in `lib/help` or `lib/screens` is longer than 64 bytes.
* The staged `lib/` is the same as `lib/` in the clone.

Copy the contents of the staging directory to the root of the card. Then, with the card in
the PC, compare the card with the staging. The script does not write to the card:

```bash
tools/stage-card.sh --card E:
```

## Make a release

```bash
git tag -a v1.0.0 -m "Angband for the PicoCalc 1.0.0"
tools/make-release.sh v1.0.0
```

The script runs `tools/stage-card.sh` first. Then it writes three files to `release/`:

* `angband-picocalc-<tag>.zip`: `pico2-apps/angband.uf2` and `angband/lib/` from the
  staging directory, and `INSTALL.md`, `LICENSE.md`, `THIRD-PARTY.md` and `copying.txt`.
* `uf2loader-2.5-pimoroni_pico_plus2_w.zip`: `bootloader_pimoroni_pico_plus2_w_rp2350.uf2`,
  `BOOT2350.uf2` and a `README.txt` with the source of the UF2 Loader.
* `SHA256SUMS`: the SHA-256 sums of the two zips.

The script stops with exit 1 if one of these checks fails:

* The working tree has no changes and no untracked files.
* The tag exists and is on the current commit.
* `tools/stage-card.sh` exits 0.
* The two UF2 Loader files are the files of UF2 Loader 2.5 that the script knows by their
  SHA-256 sums. Set `LOADER_DIR` to the directory with the two files. The default is
  `uf2loader/output` beside the clone.

The script makes the same zips each time for the same tag. Each file in a zip has the date of
the tagged commit. To check the sums:

```bash
cd release && sha256sum -c SHA256SUMS
```

## Where to read more

* [PORT-NOTES.md](PORT-NOTES.md): the upstream commit, the files that were removed, and each
  change to an upstream file, with the reason.
* [LICENSE.md](LICENSE.md) and [THIRD-PARTY.md](THIRD-PARTY.md): the licences.
* The comment at the top of `CMakeLists.txt`: the build targets.
