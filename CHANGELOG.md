# Changelog

## 1.0.0 — not yet released

The first public release.

The game is upstream Angband commit `00c9414cb`: version 4.2.6 and 184 later commits. The
game rules, monsters, objects and dungeon generator are not changed.

### What the port gives you

* The full game: character creation, the town, the stores, the dungeon, saving and loading,
  the high scores, the knowledge menus and the in-game help.
* A 64x32 text screen on the PicoCalc. The status bar is at the top, so the map uses
  all 64 columns. Screens that need 80 columns on a PC have a 64-column layout.
* The original Angband keyset on the PicoCalc keyboard, with the arrow keys for movement.
* The game data and one savefile on the card, in `angband\lib\`.
* The battery charge on the status row, a message at 15 % and at 5 %, and an automatic save
  at 5 %.
* A message on the screen if the card does not mount or the game cannot start.
* The splash screen shows `PicoCalc port 1.0.0`.

### Known limitations

* It needs a Pimoroni Pico Plus 2 W. A Pico 1, a Pico 2 or a Pico 2 W does not work.
* A start takes 2 to 2.5 minutes.
* A new dungeon level takes about 2.5 seconds at depth 1.
* There are no tiles, no sound and no subwindows.
* There is no numeric keypad. Alt+comma, Alt+period, Alt+space and Alt+B do not go to the
  game.
* There is no crash save.
