# Install Angband on the PicoCalc

This document tells you how to put Angband on a PicoCalc, how to update it, and what to do
if it does not start.

## What you need

* A ClockworkPi PicoCalc with a **Pimoroni Pico Plus 2 W** in it. A Pico 1, a Pico 2 or a
  Pico 2 W does not work.
* A microSD card, formatted FAT32. This document calls it "the card".
* A PC with a card reader and a USB-C cable.
* From the release page, two downloads:
  * `angband-picocalc-<version>.zip`: the game.
  * `uf2loader-2.5-pimoroni_pico_plus2_w.zip`: the UF2 Loader for this board. It holds
    `bootloader_pimoroni_pico_plus2_w_rp2350.uf2` and `BOOT2350.uf2`.

The UF2 Loader is a menu that starts programs from the card. It is by James Churchill
(<https://github.com/pelrun/uf2loader>, GPL-3.0). Its own release page has no file for the
Pico Plus 2 W, so this project gives you one. The two files were built from UF2 Loader
version 2.5 (commit `5c44a4b`), with no change to the source, and with
`PICO_BOARD=pimoroni_pico_plus2_w_rp2350`.

If the UF2 Loader is already on your Pico Plus 2 W and `BOOT2350.uf2` is already on the
card, go to [Install the game](#install-the-game).

## Install the UF2 Loader (one time)

### Program the board

1. Set the PicoCalc power switch to off.
2. Disconnect all USB cables from the PicoCalc.
3. Push and hold the **BOOTSEL** button on the Pico Plus 2 W.
4. Connect the USB-C cable from the PC to the USB-C socket on the Pico Plus 2 W. Do not use
   the USB-C socket on the PicoCalc case.
5. Release the BOOTSEL button.
6. Make sure that a drive with the name **RP2350** opens on the PC.
7. Copy `bootloader_pimoroni_pico_plus2_w_rp2350.uf2` to the RP2350 drive.
8. The board restarts and the drive closes.
9. Disconnect the USB-C cable.

### Put the menu on the card

1. Put the card in the PC.
2. Copy `BOOT2350.uf2` to the root of the card.

## Install the game

1. Put the card in the PC.
2. Unzip `angband-picocalc-<version>.zip` to the root of the card.
3. Make sure that these two parts are on the card:
   * `pico2-apps\angband.uf2`
   * `angband\lib\`, with the folders `customize`, `gamedata`, `help` and `screens` in it.
4. Remove the card from the PC. Put the card in the PicoCalc.
5. Hold the **Up** key. Set the power switch to on. The menu opens. You can also hold **F1**
   or **F5**.
6. Select **angband** with the arrow keys. Push **Enter**.
7. Wait. The first start takes 2 to 2.5 minutes. The screen shows the progress while the game
   reads its data files.
8. At `[Press any key to continue]`, push a key.

The game stays in flash. When you set the power switch to on the next time, the game starts
immediately, without the menu. To open the menu again, hold **Up** when you set the power
switch to on.

The zip also puts `INSTALL.md`, `LICENSE.md`, `THIRD-PARTY.md` and `copying.txt` on the
card. The game does not use them. You can delete them.

## Update the game

Always replace the two parts together: `pico2-apps\angband.uf2` and `angband\lib\`. If they
come from different releases, some screens can look wrong.

1. Put the card in the PC.
2. Copy the folder `angband\lib\user` to the PC. This folder holds your savefile and your
   high scores.
3. Delete `pico2-apps\angband.uf2` and the folder `angband\lib` from the card.
4. Unzip the new `angband-picocalc-<version>.zip` to the root of the card.
5. Copy the `user` folder from the PC back to `angband\lib\user` on the card.
6. Put the card in the PicoCalc. Hold **Up**, set the power switch to on, and select
   **angband**.

You must select **angband** in the menu after an update. If you do not, the old game in
flash starts.

## Start a new character

1. Put the card in the PC.
2. Delete the file `angband\lib\user\save\PicoCalc`.
3. Put the card in the PicoCalc and set the power switch to on.

If your character dies, the game also starts a new character.

## Remove the game

1. Delete `pico2-apps\angband.uf2` from the card.
2. Delete the folder `angband` from the card. This also deletes your savefile and your high
   scores.
3. Hold **Up**, set the power switch to on, and select a different program. Until you do this,
   the game stays in flash.

## If the game does not start

| Symptom | Cause | Action |
|---|---|---|
| The menu does not open. The PC shows an **RP2350** drive. | The UF2 Loader cannot find the card or `BOOT2350.uf2`. | Make sure that the card is in the PicoCalc and that `BOOT2350.uf2` is at the root of the card. |
| The menu does not show **angband**. | `angband.uf2` is not in `pico2-apps\`. | Put `angband.uf2` in the folder `pico2-apps` at the root of the card. |
| The game starts at power-on and the menu does not open. | This is normal. The last program stays in flash. | Hold **Up**, **F1** or **F5** when you set the power switch to on. |
| The screen stays dark after you select **angband**. The first screen usually shows after 1 second. | The board is not a Pico Plus 2 W, or the file is damaged. | Make sure that the board is a Pimoroni Pico Plus 2 W. Copy `angband.uf2` from the zip again. |
| The screen shows `*** SD card mount failed ***` and a reason. | The game cannot read the card. | Make sure that the card is FAT32 and that it is fully in the slot. Set the power switch to off and then on. |
| The screen shows `*** Angband quit ***` and a file name. | The game cannot find or read a data file. | Make sure that `angband\lib\` is at the root of the card and that it has all its folders. Copy the two parts from the zip again. |
| Some screens look wrong: text is cut off or in the wrong place. | `angband.uf2` and `angband\lib\` come from different releases. | Do the steps in [Update the game](#update-the-game) again, with one zip. |
| The game stops, or the PicoCalc loses power. | There is no crash save. | Set the power switch to off and then on. The game loads the last save. |

To put the board in BOOTSEL mode without the button, hold **Down** or **F3** when you set the
power switch to on.
