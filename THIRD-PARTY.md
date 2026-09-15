# Third-party components

Every file in this repository that did not originate here, with its origin, its licence,
and whether the port changed it. `LICENSE.md` states which licence covers which part at a
summary level; this file is the detail, built from the file headers in `src/platform/`,
`PORT-NOTES.md`'s **The platform layer** table, and `vendor-filter.log`.

| Component | Files | Origin | Licence | Local changes (`PORT:`) |
|---|---|---|---|---|
| Angband | `src/game/`, `src/host/`, `lib/`, `tests/`, `copying.txt` | `https://github.com/angband/angband` at commit `00c9414cb`, filtered by `vendor-filter.log` | GNU GPL v2, or the Angband licence (dual, see `copying.txt`) | Yes. Every edit carries a `PORT:` banner and is listed in `PORT-NOTES.md`'s **PORT: edits** section. |
| PicoCalc LCD driver | `src/platform/lcd.c`, `src/platform/lcd.h` | Copied from Picoware (`Picoware/src/SDK/Picoware/src/system/drivers/lcd.c`, commit `841d9c56`); Picoware's own upstream for these driver files is `https://github.com/BlairLeduc/picocalc-text-starter` | MIT (Blair Leduc). Picoware's own repository licence is GPL-3.0, but these driver files are a vendored MIT component within it, not covered by Picoware's own licence. | Yes. Stage 045 changed the pixel transfer to 16-bit DMA frames (measured 91 -> 44 ms per repaint); the change was made in the sibling `tinyrogue-pico` tree first and copied down, so the file stays byte-identical to its source tree (`tools/platform-cmp.sh`). |
| PicoCalc south-bridge driver | `src/platform/southbridge.c`, `src/platform/southbridge.h` | Same as the LCD driver, above | MIT (Blair Leduc) | No. Byte-identical to `tinyrogue-pico/src/platform/`. |
| 5x10 font | `src/platform/font5x10.c`, `src/platform/font5x10.h` | Same route as the LCD driver above (Picoware, commit `841d9c56`; upstream `picocalc-text-starter`). The glyph data is itself redrawn from the 5x8 font at `https://github.com/idispatch/raster-fonts` (`font-5x8.c`). | MIT (Blair Leduc) for the 5x10 redraw. **ASSUMPTION: the upstream 5x8 font's own licence is not stated.** Checked 2026-09-14: `idispatch/raster-fonts` carries no `LICENSE`/`COPYING` file at its root, and `font-5x8.c` itself has no licence header or copyright notice. Blair Leduc's 5x10 redraw is a defensible derivative distributed under a stated licence (MIT), and this repository relies on that MIT grant; the note here is so a downstream user knows the ultimate origin of the glyph shapes was never itself licensed. | No. Byte-identical to `tinyrogue-pico/src/platform/`. |
| PicoCalc keyboard driver | `src/platform/keyboard.c`, `src/platform/keyboard.h` | Port-written for `tinyrogue-pico`, stage 020 -- not vendored from Picoware. (`keyboard.h`'s own comment explains why Picoware's `keyboard.c` was not used: this port's queue and modifier-latch model differs.) | Same terms as this repository's own port-written code: GNU GPL v2, or the Angband licence (see `LICENSE.md`). | N/A -- written for this project's sibling ports, not vendored from a third party. |
| pico-vfs | `external/pico-vfs/` (git submodule) | `https://github.com/oyama/pico-vfs` at commit `4b71f274acae7de9a3696d3345992294fa9e034e`. Pin proven (stage 110) against the ClockworkPi PicoCalc firmware's own vendored copy at `PicoCalc/Code/pico_multi_booter/sd_boot/lib/pico-vfs`: `diff -rq` between the two trees prints only `.git` entries. | BSD-3-Clause (Hiroyuki Oyama) | Not vendored and not modified; a submodule, built from its own sources at configure time. |
| pico-vfs's FatFs (ff15) | `external/pico-vfs/vendor/ff15/` (inside the pico-vfs submodule) | `http://elm-chan.org/fsw/ff/` (ChaN), vendored by pico-vfs itself | BSD-2-Clause (ChaN) | Not modified by this repository. |
| pico-vfs's littlefs | `external/pico-vfs/vendor/littlefs/` (nested git submodule, inside pico-vfs) | `https://github.com/littlefs-project/littlefs` at commit `8ed63b27be79ab59ee1cd15a950ddd64e7a602f7` | BSD-3-Clause | Not modified by this repository. |
| UF2 Loader | Not in this repository. Shipped as a separate release asset, `uf2loader-2.5-pimoroni_pico_plus2_w.zip` (`bootloader_pimoroni_pico_plus2_w_rp2350.uf2`, `BOOT2350.uf2`), made by `tools/make-release.sh` | `https://github.com/pelrun/uf2loader`, version 2.5, commit `5c44a4b`, built with `PICO_BOARD=pimoroni_pico_plus2_w_rp2350` (James Churchill) | GNU GPL v3 | No. Binaries built from the unmodified source; the asset's `README.txt` names the source commit. |
| pico-sdk | Not vendored; not part of this repository | `https://github.com/raspberrypi/pico-sdk`, version 2.3.0 | BSD-3-Clause (Raspberry Pi (Trading) Ltd) | N/A -- a build dependency, fetched separately per `BUILDING.md`. |
| picotool | Not vendored; not part of this repository | `https://github.com/raspberrypi/picotool`, version 2.3.0 | BSD-3-Clause (Raspberry Pi (Trading) Ltd) | N/A -- a build dependency. |

## Port-written files with no third-party origin

These are not vendored from anywhere and carry this repository's own licence (GNU GPL v2,
or the Angband licence -- see `LICENSE.md`): `src/main.c`, `src/link/*.incl`,
`src/platform/psram_heap.c/.h`, `src/platform/sd_fs.c/.h`, `src/platform/syscalls.c/.h`,
`src/platform/main-pico.c/.h`, `src/platform/utf8.c/.h`, `src/platform/turnlog.c/.h`,
`src/platform/battery.c/.h`, `src/platform/sync.h`, `src/platform/sync-fmt.h`, and
everything under `tools/`.
