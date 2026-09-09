#!/usr/bin/env bash
# The suite, part 1, for angband-picocalc (specifications.md section 9).
#
# Compiles every translation unit under src/game/ for the RP2350 Cortex-M33 with the flag
# set the project has chosen, and reports "N of M compiled". Nothing is linked; the device
# link is stage 050's problem.
#
#   ./tools/compile-sweep.sh
#
# Exits non-zero if any unit fails. Per-unit compiler output lands in build-sweep/*.log.
#
# src/host/ (upstream main.c and main-test.c) is not swept: it is the WSL harness only,
# built by tools/host-build.sh.
#
# The pico-vfs include directory comes first so that <sys/dirent.h> resolves to pico-vfs's
# header rather than newlib's #error stub; without it z-file.c is the one unit that fails.

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$PORT/.." && pwd)"
PICO_VFS="$WS/PicoCalc/Code/pico_multi_booter/sd_boot/lib/pico-vfs"

# Build tools are not on the system PATH on this machine.
# shellcheck source=/dev/null
source "$WS/tools/pico-env.sh" > /dev/null

CC=arm-none-eabi-gcc

command -v "$CC" > /dev/null || { echo "compile-sweep: $CC not found after sourcing tools/pico-env.sh"; exit 2; }
[ -d "$PICO_VFS/include" ] || { echo "compile-sweep: pico-vfs headers not at $PICO_VFS/include"; exit 2; }

FLAGS=(-mcpu=cortex-m33 -mthumb -Os -ffunction-sections -fdata-sections -std=gnu99 -DPICOCALC -Wall -c)
INCLUDES=(-I "$PICO_VFS/include" -I "$PORT/src/game")

OUT="$PORT/build-sweep"
rm -rf "$OUT"
mkdir -p "$OUT"

mapfile -t SOURCES < <(find "$PORT/src/game" -maxdepth 1 -name '*.c' | sort)

total=0
ok=0
warnings=0
failed=()

start=$SECONDS

for src in "${SOURCES[@]}"; do
	total=$((total + 1))
	obj="$OUT/$(basename "${src%.c}").o"

	if "$CC" "${FLAGS[@]}" "${INCLUDES[@]}" "$src" -o "$obj" 2> "$obj.log"; then
		ok=$((ok + 1))
	else
		failed+=("$(basename "$src")")
	fi
	warnings=$((warnings + $(grep -c "warning:" "$obj.log" || true)))
done

elapsed=$((SECONDS - start))

echo
echo "$ok of $total compiled   (${elapsed}s, $warnings warnings)"

if [ ${#failed[@]} -gt 0 ]; then
	echo
	echo "FAILED:"
	for f in "${failed[@]}"; do
		echo "  $f"
		head -5 "$OUT/${f%.c}.o.log" | sed 's/^/      /'
	done
	exit 1
fi

# Report the static footprint. Hand estimates are a lower bound only.
arm-none-eabi-size -t "$OUT"/*.o | tail -1 | awk '{printf "text %d  data %d  bss %d\n", $1, $2, $3}'
exit 0
