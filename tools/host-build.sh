#!/usr/bin/env bash
# The suite, part 2, for angband-picocalc (specifications.md sections 6.6 and 9).
#
# Run from Git Bash on the Windows host:
#
#   cd <workspace root> && angband-pico/tools/host-build.sh
#   cd <workspace root> && angband-pico/tools/host-build.sh --borg
#
# It shells out to WSL, builds the vendored core natively with gcc, links it against the
# vendored upstream main.c + main-test.c, runs the end-to-end tests under tests/, and then
# runs tools/heap-probe.in under an LD_PRELOAD malloc counter.
#
# Why not the upstream CMake build: it forces the X11 front end when no graphical front end
# is chosen, and this WSL has no X11 headers. The file list is compiled directly instead.
#
# Expected output:
#   host-build: utf8-test 38/38 checks passed
#   host-build: battery-test N/N checks passed
#   Total: 5/5
#   depth: 50  followed by  HEAP[L50] live=...
#
# --borg (picocalc-device-harness stage 055) builds a SECOND, separate binary instead:
# build-host-borg/angband-borg, which is src/game/ plus src/game/borg/ plus the port-written
# front end src/host/main-borg.c, with -D ALLOW_BORG and
# -D ANGBAND_SYNC. It is the PC half of lockstep: Angband's own borg plays a savefile here,
# every keypress it feeds the game is written to a keystream file, and one SYNC line per
# completed command is written beside it. The harness sends the keys to the device and
# compares the device's SYNC lines against these. Nothing in src/game/borg/ is compiled for the
# device; see CMakeLists.txt, which does not mention it.
#
# The two builds share NOTHING but the source tree: separate build directory, separate
# object directory, separate binary. --borg does not run the end-to-end tests or the heap
# probe, so the suite result quoted in a run log always comes from the plain invocation.
#
# HOST_CC / HOST_LDFLAGS / HOST_TAG (harness stage 056) cross-compile the --borg binary for
# another architecture; see the block beside HOST_OPT below. Unset, nothing changes.
#
# --arm (harness stage 057) is a NAMED SHORTHAND for the four variables that make this build
# agree with the device. It is not a new capability: it is the one combination stage 056
# measured, written down once so that a run log can quote a flag instead of a paragraph and
# so that getting it wrong takes an effort. A caller that has already set any of the four
# keeps its own value -- the shorthand fills in, it does not override.
#
# Everything is built under angband-pico/build-host*/, which is gitignored. The WSL side
# works in the same directory through /mnt/c, so no state lives in the WSL home.

set -u

BORG=0
ARM=0
for a in "$@"; do
	case "$a" in
		--borg) BORG=1 ;;
		--arm)  ARM=1 ;;
		*) echo "host-build: unknown argument '$a' (only --borg, --arm)"; exit 2 ;;
	esac
done

# --arm: the stage 056 combination, by name. := and not = , so a caller that set one of
# these on the command line still gets its own value and can vary one axis at a time --
# HOST_OPT=-Os ... --arm is the device's optimisation level on the device's data model.
if [ "$ARM" = 1 ]; then
	: "${HOST_TAG:=arm}"
	: "${HOST_CC:=arm-linux-gnueabihf-gcc}"
	: "${HOST_LDFLAGS:=-static}"
	: "${HOST_OPT:=-O1 -mthumb -fshort-enums}"
	export HOST_TAG HOST_CC HOST_LDFLAGS HOST_OPT
	echo "host-build: --arm -> HOST_TAG=$HOST_TAG HOST_CC=$HOST_CC HOST_LDFLAGS=$HOST_LDFLAGS HOST_OPT=$HOST_OPT"
	# --arm without --borg would cross-compile the end-to-end suite, whose tests/run-tests
	# runs the binary directly on this WSL. Refuse rather than fail in the test harness.
	if [ "$BORG" != 1 ]; then
		echo "host-build: --arm needs --borg (the cross build is the borg binary only)"
		exit 2
	fi
fi

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPORT="$(echo "$PORT" | sed 's|^/\([a-zA-Z]\)/|/mnt/\1/|')"

echo "host-build: port tree $PORT"
echo "host-build: as seen from WSL: $WPORT"
[ "$BORG" = 1 ] && echo "host-build: --borg, building build-host-borg/angband-borg only"

wsl.exe -e bash -lc "PORT='$WPORT' BORG='$BORG' HOST_OPT='${HOST_OPT:-}' HOST_CC='${HOST_CC:-}' HOST_LDFLAGS='${HOST_LDFLAGS:-}' HOST_TAG='${HOST_TAG:-}' bash -s" <<'WSLEOF'
set -u
cd "$PORT" || exit 2

# -fsigned-char: harness stage 055. ARM GCC defaults char to unsigned and x86-64 GCC to
# signed, and lockstep asks the device and this build to agree on every value the game
# computes. CMakeLists.txt and tools/compile-sweep.sh carry the same flag; all three are
# edited together or none is.
# HOST_OPT (harness stage 055): the optimisation level, so that a divergence can be tested
# against the device's -Os without editing this file. Undefined behaviour resolves
# differently under different optimisation, and the device builds at MinSizeRel.
# HOST_CC, HOST_LDFLAGS, HOST_TAG (harness stage 056): the ARCHITECTURE knob. Stage 055's
# divergence survived every non-architectural setting the x86 host could change, so the
# remaining suspects -- 32-bit word width and the ARM instruction set -- need a host build
# that is not x86-64. With the cross-compiler installed in this WSL that is:
#
#   HOST_TAG=arm HOST_CC=arm-linux-gnueabihf-gcc HOST_LDFLAGS=-static \
#     HOST_OPT="-O1 -mthumb -fshort-enums" angband-pico/tools/host-build.sh --borg
#
# -static because the binary is run under qemu-arm-static, which would otherwise need an
# armhf sysroot for the dynamic loader. -fshort-enums because the device does the same
# (arm-none-eabi-readelf -A says Tag_ABI_enum_size: small) and lockstep asks the two sides
# to agree on every value the game computes; it is the same class of pin as -fsigned-char.
#
# HOST_TAG suffixes the build directory, so an ARM build never overwrites the x86 one: the
# x86 binary is still what the end-to-end suite runs. With all three unset this file behaves
# exactly as it did before stage 056 -- the acceptance criterion for the change is a replay
# that is byte-identical to the one taken before it.
: "${HOST_OPT:=-O1}"
: "${HOST_CC:=gcc}"
: "${HOST_LDFLAGS:=}"
: "${HOST_TAG:=}"
CFLAGS="$HOST_OPT -g -std=gnu99 -fsigned-char -DUSE_TEST -DHAVE_DIRENT_H -DHAVE_STAT -DHAVE_MKDIR -DHAVE_FCNTL_H -w -I$PORT/src/game -I$PORT/src/platform"

if [ "$BORG" = 1 ]; then
	B="$PORT/build-host-borg${HOST_TAG:+-$HOST_TAG}"
	rm -rf "$B"
	mkdir -p "$B/obj"

	# ALLOW_BORG turns on the hooks the core already carries for it: inkey_hack in
	# ui-input.c, the Ctrl-Z binding in ui-game.c, NOSCORE_BORG in player.h, the
	# do_cmd_borg dispatch in cmd-misc.c. None of them changes OPT_MAX or the savefile
	# layout, so a savefile written without it loads here unchanged -- which matters,
	# because the device build does NOT define it and lockstep needs one savefile.
	#
	# ANGBAND_SYNC turns on the ANGBAND_SYNC_END() call beside TURNLOG_END() in
	# ui-game.c and main-borg.c's sync_end(), which is built on src/platform/sync-fmt.h --
	# the same header src/platform/main-pico.c includes. So this build emits the same SYNC
	# line the device does, out of one formatter.
	BCFLAGS="$CFLAGS -DALLOW_BORG -DANGBAND_SYNC -I$PORT/src/game/borg"

	echo "host-build: compiling (borg)"
	start=$SECONDS
	# src/host/main.c and src/host/main-test.c are NOT in this list: main-borg.c has its
	# own main(), so the upstream front-end chooser is replaced rather than extended and
	# neither vendored file is edited.
	ls src/game/*.c src/game/borg/*.c src/host/main-borg.c > "$B/files.txt"
	nsrc=$(wc -l < "$B/files.txt")

	xargs -P 12 -n 1 -I{} sh -c \
	  "$HOST_CC"' '"$BCFLAGS"' -c "$1" -o "'"$B"'/obj/$(basename "${1%.c}").o" 2>> "'"$B"'/cc.log" || echo "FAILED $1" >> "'"$B"'/fail.log"' \
	  _ {} < "$B/files.txt"

	nobj=$(ls "$B/obj" | wc -l)
	echo "host-build: $nobj of $nsrc objects ($((SECONDS - start))s)"
	if [ -s "$B/fail.log" ]; then
		echo "host-build: compile failures:"
		cat "$B/fail.log"
		grep -m 30 "error:" "$B/cc.log"
		exit 1
	fi

	echo "host-build: linking angband-borg"
	"$HOST_CC" $HOST_LDFLAGS -o "$B/angband-borg" "$B"/obj/*.o -lm || exit 1
	ls -l "$B/angband-borg"
	echo "host-build: borg build ok"
	exit 0
fi

B="$PORT/build-host${HOST_TAG:+-$HOST_TAG}"
rm -rf "$B"
mkdir -p "$B/obj"

echo "host-build: compiling"
start=$SECONDS
# src/host/main-borg.c is NOT in this list, and "src/host/*.c" would put it there. It has
# its own main(), which would collide with main.c's, and it needs ALLOW_BORG, which this
# build does not define. It is built only by --borg above.
ls src/game/*.c src/host/main.c src/host/main-test.c > "$B/files.txt"
nsrc=$(wc -l < "$B/files.txt")

# One gcc per source, 12 at a time. The -n1 keeps a single failure from hiding the rest.
xargs -P 12 -n 1 -I{} sh -c \
  "$HOST_CC"' '"$CFLAGS"' -c "$1" -o "'"$B"'/obj/$(basename "${1%.c}").o" 2>> "'"$B"'/cc.log" || echo "FAILED $1" >> "'"$B"'/fail.log"' \
  _ {} < "$B/files.txt"

nobj=$(ls "$B/obj" | wc -l)
echo "host-build: $nobj of $nsrc objects ($((SECONDS - start))s)"
if [ -s "$B/fail.log" ]; then
	echo "host-build: compile failures:"
	cat "$B/fail.log"
	grep -m 20 "error:" "$B/cc.log"
	exit 1
fi

echo "host-build: linking"
"$HOST_CC" $HOST_LDFLAGS -o "$B/angband-test" "$B"/obj/*.o -lm || exit 1

# Stage 040. The front end's UTF-8 decoder, checked on the host because its failure mode on
# the device is silent: a wrong wide-character count just shifts a line by a cell.
echo
echo "host-build: front end unit checks (utf8.c)"
gcc -O1 -std=gnu99 -fsigned-char -Wall -Wextra -I"$PORT/src/platform" -I"$PORT/src/game" \
	-o "$B/utf8-test" "$PORT/tools/utf8-test.c" "$PORT/src/platform/utf8.c" || exit 1
if "$B/utf8-test" > "$B/utf8.log" 2>&1; then
	echo "host-build: utf8-test $(grep -c '^ok ' "$B/utf8.log")/$(grep -c '^ok \|^FAIL ' "$B/utf8.log") checks passed"
else
	echo "host-build: utf8-test FAILED"
	cat "$B/utf8.log"
	exit 1
fi
echo

# Stage 100. The battery field's logic (src/platform/battery.h): the register decode, the
# colour level, the field width and the 15 % / 5 % warning state machine. The host has no
# PICOCALC and cannot draw the field, and a device session cannot reach 5 %, so this is the
# check of the logic. Header-only: battery.c needs the SDK and is not compiled here.
echo "host-build: front end unit checks (battery.h)"
gcc -O1 -std=gnu99 -fsigned-char -Wall -Wextra -Werror -I"$PORT/src/platform" \
	-o "$B/battery-test" "$PORT/tools/battery-test.c" || exit 1
if "$B/battery-test" > "$B/battery.log" 2>&1; then
	echo "host-build: battery-test $(grep -c '^ok ' "$B/battery.log")/$(grep -c '^ok \|^FAIL ' "$B/battery.log") checks passed"
else
	echo "host-build: battery-test FAILED"
	cat "$B/battery.log"
	exit 1
fi
echo

echo "host-build: building heapshim.so"
gcc -O2 -fPIC -shared -o "$B/heapshim.so" "$PORT/tools/heapshim.c" -ldl || exit 1

# run-tests wants tests/ and lib/ in the working directory.
cp -r "$PORT/lib" "$B/lib"
cp -r "$PORT/tests" "$B/tests"
# Belt and braces: the tree is vendored with LF (git archive under core.autocrlf=false),
# but a re-vendor with the machine's default core.autocrlf=true gives every test file CRLF,
# and then /bin/sh refuses the shebang line and the test inputs carry a trailing CR that
# makes player-birth reject its class argument. Normalise the copies, never the originals.
find "$B/tests" -type f -exec sed -i 's/\r$//' {} +
chmod +x "$B/tests/run-tests" "$B/tests/run-test"
find "$B/tests" -name matcher -type f -exec chmod +x {} +

echo
echo "host-build: end-to-end tests"
cd "$B" || exit 2
tests/run-tests ./angband-test
rc=$?

echo
echo "host-build: heap probe"
rm -rf ~/.angband/PicoCalcProbe
# stdbuf must come after env: it works by prepending libstdbuf.so to LD_PRELOAD, and
# setting LD_PRELOAD afterwards would drop it, leaving stdout block-buffered so that the
# depth: lines all arrive after the HEAP[ lines.
env LD_PRELOAD="$B/heapshim.so" stdbuf -oL -eL ./angband-test -mtest \
	< "$PORT/tools/heap-probe.in" 2>&1 \
	| grep -E "^(depth:|HEAP\[|jump:)"

exit $rc
WSLEOF

rc=$?
echo
echo "host-build: exit $rc"
exit $rc
