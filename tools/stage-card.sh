#!/usr/bin/env bash
# Stage 080: put the release build and the lib tree into sdcard-pico2/, and prove it.
#
#   cd /c/Users/greenblob/Documents/PicoCalc && angband-pico/tools/stage-card.sh [--card E:]
#
# Writes (and replaces) only these two things in sdcard-pico2/:
#   pico2-apps/angband.uf2      from angband-pico/build-pico2/angband.uf2 (the shipped build)
#   angband/lib/                from angband-pico/lib/, with user/ re-created empty
#
# Then checks, and exits 1 on any failure:
#   * the UF2 family is rp2350-arm-s and the UF2 is not older than the newest source;
#   * the build tree is the shipped recipe (every ANGBAND_* option off, no text pad);
#   * no tiles/ sounds/ fonts/ icons/ and no Makefile in the staged lib;
#   * user/save, user/scores, user/archive, user/panic exist and are empty;
#   * no line in the staged lib/help or lib/screens is over 64 bytes (specifications.md 6.4);
#   * diff -rq between angband-pico/lib and the staged lib, user/ aside, is empty.
#
# --card E: adds the same diff against E:\angband\lib and compares the card's UF2 with the
# staged one. It reads the card and never writes to it.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT="$ROOT/angband-pico"
BUILD="$PORT/build-pico2"
STAGE="$ROOT/sdcard-pico2"
PICOTOOL="$ROOT/picotool/picotool/picotool.exe"

CARD=""
if [ "${1:-}" = "--card" ]; then
	CARD="${2:?--card needs a drive, e.g. E:}"
	case "$CARD" in */*) ;; *) CARD="/$(echo "$CARD" | tr 'A-Z' 'a-z' | tr -d ':')" ;; esac
fi

fail=0
bad() { echo "stage-card: FAIL $*"; fail=1; }
ok()  { echo "stage-card: ok   $*"; }

# --- the build tree is the shipped one -----------------------------------------------------
UF2="$BUILD/angband.uf2"
[ -f "$UF2" ] || { echo "stage-card: no $UF2 -- build the angband target first"; exit 2; }

opts="$(grep -E '^ANGBAND_[A-Z_]+:BOOL=ON' "$BUILD/CMakeCache.txt")"
pad="$(grep -E '^ANGBAND_TEXT_PAD:' "$BUILD/CMakeCache.txt" | cut -d= -f2)"
if [ -n "$opts" ] || [ "${pad:-0}" != "0" ]; then
	bad "build-pico2 is not the shipped recipe: ${opts//$'\n'/ } pad=$pad"
else
	ok "build-pico2 has every ANGBAND_* option off and pad 0"
fi

newest="$(find "$PORT/src" "$PORT/CMakeLists.txt" -type f -newer "$UF2" | head -1)"
[ -z "$newest" ] && ok "angband.uf2 is newer than every source" \
	|| bad "angband.uf2 is older than $newest"

family="$("$PICOTOOL" info "$UF2" | head -1)"
case "$family" in
	*"'rp2350-arm-s'"*) ok "family rp2350-arm-s" ;;
	*) bad "family: $family" ;;
esac

# --- copy ---------------------------------------------------------------------------------
mkdir -p "$STAGE/pico2-apps"
cp -p "$UF2" "$STAGE/pico2-apps/angband.uf2"

rm -rf "$STAGE/angband/lib"
mkdir -p "$STAGE/angband"
cp -rp "$PORT/lib" "$STAGE/angband/lib"
rm -rf "$STAGE/angband/lib/user"
for d in save scores archive panic; do mkdir -p "$STAGE/angband/lib/user/$d"; done
ok "copied $(stat -c %s "$UF2") B of UF2 and $(find "$STAGE/angband/lib" -type f | wc -l) lib files"

# --- checks on what was staged ------------------------------------------------------------
L="$STAGE/angband/lib"
for d in tiles sounds fonts icons; do
	[ -e "$L/$d" ] && bad "lib/$d is present"
done
mk="$(find "$L" -iname 'Makefile*')"
[ -z "$mk" ] && ok "no tiles/ sounds/ fonts/ icons/ or Makefile" || bad "Makefile staged: $mk"

ufail=0
for d in save scores archive panic; do
	if [ ! -d "$L/user/$d" ]; then bad "user/$d missing"; ufail=1
	elif [ -n "$(ls -A "$L/user/$d")" ]; then bad "user/$d not empty"; ufail=1; fi
done
[ "$ufail" = 0 ] && ok "user/save, user/scores, user/archive, user/panic exist and are empty"

long="$(awk 'length > 64 { print FILENAME ":" FNR ": " length }' "$L"/help/* "$L"/screens/*)"
[ -z "$long" ] && ok "no line over 64 bytes in lib/help or lib/screens" || bad "over 64: $long"

d="$(diff -rq -x user "$PORT/lib" "$L")"
[ -z "$d" ] && ok "diff -rq angband-pico/lib sdcard-pico2/angband/lib (user/ aside): identical" \
	|| bad "staged lib differs: $d"
[ "$(ls "$L" | tr '\n' ' ')" = "customize gamedata help readme.txt screens user " ] \
	&& ok "6 top-level entries, $(ls "$L/gamedata" | wc -l) files in gamedata/" \
	|| bad "top level: $(ls "$L" | tr '\n' ' ')"

# --- the card, read only ------------------------------------------------------------------
if [ -n "$CARD" ]; then
	if [ ! -d "$CARD/angband/lib" ]; then
		bad "no $CARD/angband/lib"
	else
		d="$(diff -rq -x user "$L" "$CARD/angband/lib")"
		[ -z "$d" ] && ok "card lib (user/ aside) identical to the staging" || bad "card lib differs: $d"
		cmp -s "$STAGE/pico2-apps/angband.uf2" "$CARD/pico2-apps/angband.uf2" \
			&& ok "card angband.uf2 identical to the staging" || bad "card angband.uf2 differs"
	fi
fi

[ "$fail" = 0 ] && echo "stage-card: exit 0" || echo "stage-card: exit 1"
exit "$fail"
