# Angband for the PicoCalc

Angband 4.2.6+ for the ClockworkPi PicoCalc with a **Pimoroni Pico Plus 2 W** (RP2350B,
8 MB PSRAM) fitted. It does not run on a Pico 1, a Pico 2 or a Pico 2 W.

The game code is upstream Angband, commit `00c9414cb`. The port adds a front end for the
320x320 panel and the PicoCalc keyboard, puts the heap in PSRAM, and reads `lib/` from the
SD card. `PORT-NOTES.md` lists every change to an upstream file.

## 1. Build

Source the environment first. Nothing is on the system PATH.

```bash
cd /c/Users/greenblob/Documents/PicoCalc && source tools/pico-env.sh
cmake --build angband-pico/build-pico2 --target angband
```

`build-pico2` is the release build: `MinSizeRel`, every `ANGBAND_*` option off. To configure
it for the first time, use the command in `../.llm/projects/angband-picocalc/specifications.md`
§10. It needs pico-sdk 2.3.0 and picotool 2.3.0.

`ANGBAND_CONSOLE` is the developer console. It is off in the release build. When it is on,
the game has USB and UART serial, waits up to 10 seconds for a USB terminal, and shows the
boot lines, the memory reports and the `init ... heap ... stack` note. The harness and bench
options (`ANGBAND_SERIAL_KEYS`, `ANGBAND_SERIAL_SCREEN`, `ANGBAND_SYNC`, `ANGBAND_TURN_LOG`,
`ANGBAND_SAVE_RESTORE`) need it, and CMake stops if it is off. A console build goes in its
own tree:

```bash
cmake -G Ninja -B angband-pico/build-pico2-console -S angband-pico <the usual flags> -DANGBAND_CONSOLE=ON
cmake --build angband-pico/build-pico2-console --target angband
```

`port-warnings.sh` needs both `build-pico2` and `build-pico2-console` configured.

The suite has five parts. Run all of them before a release:

```bash
angband-pico/tools/compile-sweep.sh
angband-pico/tools/host-build.sh
angband-pico/tools/platform-cmp.sh
angband-pico/tools/screen-sweep.sh
angband-pico/tools/port-warnings.sh
```

## 2. Prepare the SD card

```bash
angband-pico/tools/stage-card.sh
```

This copies `build-pico2/angband.uf2` to `sdcard-pico2/pico2-apps/` and `lib/` to
`sdcard-pico2/angband/lib/`, with empty `user/` directories. It also does these checks, and
it stops with exit 1 if one fails:

* The UF2 family is `rp2350-arm-s`, and the UF2 is newer than the source.
* The build tree is the release build.
* There is no `tiles/`, `sounds/`, `fonts/`, `icons/` or `Makefile`.
* `user/save`, `user/scores`, `user/archive` and `user/panic` are empty.
* No line in `lib/help` or `lib/screens` is longer than 64 bytes.
* The staged `lib/` is the same as `angband-pico/lib/`.

Then copy `sdcard-pico2\` to the card as `../README-pico2w.md` §4 tells you. To make sure
the card has the correct files, put the card in the PC and run:

```bash
angband-pico/tools/stage-card.sh --card E:
```

The UF2 and the `lib/` tree must always change together. If you put a new UF2 on the card
with old `lib/` files, the screens can look wrong.

## 3. Play

1. Hold **Up** and set the power switch to on. The UF2 Loader menu opens.
2. Select **angband** and push Enter.
3. Wait approximately **2 minutes**. The game reads `lib/gamedata` at each start. The
   panel shows the progress.
4. At `[Press any key to continue]`, push a key.

At the next power-on the game starts without the menu.

* **Save:** `Ctrl-S`. The game also saves when you go to a new level.
* **Savefile:** `E:\angband\lib\user\save\PicoCalc`. There is one savefile. To start a new
  character, delete this file, or let the character die.
* **High scores:** `E:\angband\lib\user\scores\scores.raw`.
* **Help:** `?`. **Knowledge menu:** `~`.
* **Keys:** the original Angband keyset. Ctrl-letter, Esc, Tab, Enter, Backspace, Delete, the
  arrows and F1..F10 all work. The keyboard has no numeric keypad. Alt+comma, Alt+period,
  Alt+space and Alt+B control the backlight and the battery display and never reach the game.
* **Screen:** 64 columns by 32 rows. The status panel is at the top of the screen
  (`SIDEBAR_TOP`), so the map uses all 64 columns.
* **No crash save.** If the board stops, you lose the game since the last save.

The release build has no serial console and does not wait for a USB terminal. The panel
stays black for approximately 1 second, then the splash screen shows. If the card does not
mount, or the game cannot start, the panel shows a box with the reason.

A console build (§1) has its console on the case USB-C socket at 115200 baud (CH340), and
waits up to 10 seconds for a USB terminal on the board socket before it starts.

## 4. Licence

Angband is free software. You can use it under the GNU General Public License, version 2, or
under the Angband licence. `copying.txt` has the full text. The files in `src/platform/`
that were copied from other projects keep the licence in their own headers.
