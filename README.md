# Angband for the PicoCalc

This is Angband 4.2.6+, the roguelike game, for the ClockworkPi PicoCalc. The game runs on
the PicoCalc screen and keyboard, and it keeps its data and your savefile on the card.

## Hardware you need

* A ClockworkPi **PicoCalc**.
* A **Pimoroni Pico Plus 2 W** in the PicoCalc, in place of the Pico that came with it.
* A microSD card, formatted FAT32. This document calls it "the card".

**These three boards do not work: the Raspberry Pi Pico (Pico 1), the Pico 2 and the Pico 2 W.**
The game needs about 5 MB of memory while it plays. The Pico 2 and the Pico 2 W have 520 KB
of RAM and no PSRAM. The Pico Plus 2 W has 8 MB of PSRAM, and the game uses it.

## What you see

The screen shows 64 columns and 32 rows. This is the town at the start of a game. It is a
text capture from the test build on a PC, which draws the same 64x32 screen. The battery
field is not in this capture, because the PC build has no battery.

```
You can learn 1 more spell.
L:1 NXT:10 AC:0 AU:334 Human Novice
S:14 I:18/10 W:10 D:10 C:14
HP:10/10 SP:2/2 Town
Light 1  Study (1) Fed 89 % Down staircase
           ###                   ####                ####
          ##.##                 ##..##              ##..#####
      ### #.:.##       ####     #....##         ### #....##.#
     ##.###.::.#### ####.@### ###.....#####    ##.##::...#..##
     #......:...#.:##.::....###..##.......######......:.:....#
    ##........##:...:.:......#...##.......##..##...:.:##:....##
    #.........##....:.....................#........:.:..:.....#
    #.........:.....:.::............................::.....####
    ###.................................................####
      ####..............................................##
        #.......................t......##7#..##8#........#
        #......5##.##4.....#3##...#1#..####..####..#6##..###
        #......###.###.....####...###..####..####..####....#
       ###.....###.###.....####...###..............####...##
       #.....................................::...........#
       ##....................###..............:.:..::.##.#:
        #....................2##.............:##..::.:##.#
        ##.##...###.....t.#..###.............: ::.....::##
         ####..## #.....###..................# :.:::.:..#
            ####  #.....####............###..# ####.:####
                  #......# #............# ####    ###
                  ######## ##############
```

## How this is different from Angband on a PC

* **The screen is 64 by 32 characters.** Some screens put their parts one above the other,
  where the PC version puts them side by side.
* **The status bar is at the top of the screen.** The map uses all 64 columns.
* **There are no graphic tiles and no sound.**
* **The keys are the original Angband keyset.** Move with the arrow keys. The PicoCalc keyboard
  has no numeric keypad.
* **There is one savefile.** The game loads it at each start.
* **A start takes 2 to 2.5 minutes.** The game reads all of its data files from the card at
  each start.
* **There is no crash save.** If the PicoCalc stops or loses power, you lose the game since
  the last save.
* **The status row shows the battery charge,** for example `Bat 87%`.

## Install

1. Put the UF2 Loader on the Pico Plus 2 W and on the card, one time.
2. Unzip the release zip to the root of the card.
3. Hold **Up**, set the power switch to on, and select **angband**.

[INSTALL.md](INSTALL.md) gives each step, how to update, and what to do if the game does not start.

## Play

* **Help:** push `?`.
* **Knowledge menu:** push `~`.
* **Save:** push `Ctrl-S`. The game also saves when you go to a new level.
* **Savefile:** `angband\lib\user\save\PicoCalc` on the card.
* **High scores:** `angband\lib\user\scores\scores.raw` on the card.
* **Battery:** the right end of the status row shows `Bat 87%` on battery and `Chg 87%` while
  the battery charges. The field is green at 50 % and above, yellow at 20 % to 49 %, red below
  20 %, and blue while the battery charges. A full battery with the charger connected shows
  `Bat100%`.
* **Low battery:** at 15 % or less, the game shows `Your battery is at 15%.` At 5 % or less,
  it shows `Your battery is at 5%. The game will now be saved.` and saves the game. The game
  does this save one time for each start.

## Known limitations

* **The start takes 2 to 2.5 minutes.** Most of this time is the parser for the data files.
* **A new dungeon level takes about 2.5 seconds** at depth 1.
* **The RAM is 88 % full.** There is no space for a second window or subwindows.
* **Four `Alt` keys do not go to the game.** Alt+comma, Alt+period and Alt+space change the
  backlights. Alt+B shows the battery charge on the keyboard LED. The keyboard controller
  uses these keys itself.
* **The text screens in `lib/screens` and `lib/help` are cut to 64 columns.** They are not
  the same as the upstream files.

## Build from source

[BUILDING.md](BUILDING.md) tells you how to build the game, run the tests and prepare a card.

## Licence

Angband is free software. You can use it under the GNU General Public License, version 2, or
under the Angband licence. [LICENSE.md](LICENSE.md) tells you which licence covers each part.
[THIRD-PARTY.md](THIRD-PARTY.md) lists each file that came from a different project. [CHANGELOG.md](CHANGELOG.md) lists the
changes in each release.

## Credits

* The Angband team, for the game (<https://github.com/angband/angband>).
* Blair Leduc, for the PicoCalc LCD, south-bridge and font drivers
  (<https://github.com/BlairLeduc/picocalc-text-starter>).
* Hiroyuki Oyama, for pico-vfs (<https://github.com/oyama/pico-vfs>).
* James Churchill, for the UF2 Loader (<https://github.com/pelrun/uf2loader>).
* ClockworkPi, for the PicoCalc.
