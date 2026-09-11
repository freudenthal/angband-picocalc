#!/usr/bin/env bash
# The suite, part 4, for angband-picocalc (stage 060).
#
#   cd /c/Users/greenblob/Documents/PicoCalc && angband-pico/tools/screen-sweep.sh
#
# Runs tools/screens.in through build-host/angband-test twice -- once at 80x32 and once at
# 64x32 -- and writes one capture file per width under build-host/screens/. Then, for every
# screen, it reports the text that lives at column 64 or beyond in the 80-column rendering:
# that text is exactly what the 64-column term throws away, because Term_putstr() clips at
# Term->wid and says nothing.
#
# Both runs use the same height so the only variable is the width.
#
# tools/host-build.sh must have run first; this reuses its build-host/ tree.
#
# Exit 0 always: this is a measurement, not a gate. Read the "OVERFLOW" lines.

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPORT="$(echo "$PORT" | sed 's|^/\([a-zA-Z]\)/|/mnt/\1/|')"

echo "screen-sweep: port tree $PORT"

wsl.exe -e bash -lc "PORT='$WPORT' bash -s" <<'WSLEOF'
set -u
cd "$PORT" || exit 2

B="$PORT/build-host"
if [ ! -x "$B/angband-test" ]; then
	echo "screen-sweep: no $B/angband-test -- run tools/host-build.sh first"
	exit 2
fi

mkdir -p "$B/screens"
cd "$B" || exit 2

for size in 80x32:0 64x32:1; do
	rm -rf ~/.angband/PicoCalcProbe lib/save/* lib/panic/*
	./angband-test -mtest -- "-s${size%%:*}" "-b${size##*:}" < "$PORT/tools/screens.in" \
		2>&1 | grep -E '^(SCR\||screen:|size:|depth:|jump:)' > "screens/${size%%:*}.txt"
	echo "screen-sweep: $size -> build-host/screens/${size%%:*}.txt ($(grep -c '^screen: ' "screens/${size%%:*}.txt") markers)"
done

echo
echo "screen-sweep: text at column 64 or beyond in the 80-column rendering"
awk '
	/^screen: end/       { tag = ""; next }
	/^screen: /          { tag = $2; row = -1; next }
	/^SCR\|/ {
		row++
		if (tag == "") next
		line = substr($0, 5)
		sub(/\|$/, "", line)
		if (length(line) > 64) {
			tail = substr(line, 65)
			printf "OVERFLOW %-22s row %2d  col 64+: |%s|\n", tag, row, tail
			n++
		}
	}
	END { printf "screen-sweep: %d overflowing rows\n", n + 0 }
' "screens/80x32.txt"
WSLEOF

echo
echo "screen-sweep: exit $?"
exit 0
