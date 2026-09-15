# Licence

This repository holds code under more than one licence. This file states which licence
covers which part. `THIRD-PARTY.md` lists every vendored or copied file by name, with its
origin and its licence in full.

## The game and the port

The Angband game code in `src/game/`, `src/host/`, `lib/` and `tests/` is upstream Angband,
vendored unchanged. It is free software: you can use it under the GNU General Public
License, version 2, or under the Angband licence. Both are in `copying.txt`, in full.

The port-written code -- everything else that is not listed below as a separate component --
is offered under the same terms: the GNU General Public License, version 2, or the Angband
licence, at your choice. This is the code in `src/main.c`, `src/link/`, and the platform
files that have no upstream copy: `psram_heap.c/.h`, `sd_fs.c/.h`, `syscalls.c/.h`,
`main-pico.c/.h`, `utf8.c/.h`, `turnlog.c/.h` and `battery.c/.h`.

## Vendored and copied components

These files came from somewhere else and keep the licence they arrived under. Full detail,
including local changes, is in `THIRD-PARTY.md`.

| Component | Files | Licence |
|---|---|---|
| The PicoCalc LCD, south-bridge and keyboard drivers | `src/platform/lcd.c/.h`, `src/platform/southbridge.c/.h` | MIT, Blair Leduc |
| The 5x10 font | `src/platform/font5x10.c/.h` | MIT, Blair Leduc (redrawn from a font of unstated licence; see `THIRD-PARTY.md`) |
| pico-vfs | `external/pico-vfs/` (git submodule, not vendored) | BSD-3-Clause, Hiroyuki Oyama |
| pico-sdk | not part of this repository | BSD-3-Clause, Raspberry Pi (Trading) Ltd |

## Documentation and simplified technical English

Angband, ASD-STE100 -- Simplified Technical English -- is a controlled-language standard
for aircraft maintenance documentation. It is named here because the operator-facing
documentation in this repository (`README.md`, and `INSTALL.md`/`BUILDING.md` from stage
120) is written to its spirit: short sentences, one instruction per step, a small
approved-word vocabulary. ASD-STE100 itself is a specification of the ASD (AeroSpace and
Defence Industries Association of Europe); it is not reproduced here and this repository
claims no licence to it, only a writing style influenced by it.
