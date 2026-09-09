#!/usr/bin/env bash
# The suite, part 2, for angband-picocalc (specifications.md sections 6.6 and 9).
#
# Run from Git Bash on the Windows host:
#
#   cd /c/Users/greenblob/Documents/PicoCalc && angband-pico/tools/host-build.sh
#
# It shells out to WSL, builds the vendored core natively with gcc, links it against the
# vendored upstream main.c + main-test.c, runs the end-to-end tests under tests/, and then
# runs tools/heap-probe.in under an LD_PRELOAD malloc counter.
#
# Why not the upstream CMake build: it forces the X11 front end when no graphical front end
# is chosen, and this WSL has no X11 headers. The file list is compiled directly instead.
#
# Expected output:
#   Total: 4/4
#   depth: 50  followed by  HEAP[L50] live=...
#
# Everything is built under angband-pico/build-host/, which is gitignored. The WSL side
# works in the same directory through /mnt/c, so no state lives in the WSL home.

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPORT="$(echo "$PORT" | sed 's|^/\([a-zA-Z]\)/|/mnt/\1/|')"

echo "host-build: port tree $PORT"
echo "host-build: as seen from WSL: $WPORT"

wsl.exe -e bash -lc "PORT='$WPORT' bash -s" <<'WSLEOF'
set -u
cd "$PORT" || exit 2

B="$PORT/build-host"
rm -rf "$B"
mkdir -p "$B/obj"

CFLAGS="-O1 -g -std=gnu99 -DUSE_TEST -DHAVE_DIRENT_H -DHAVE_STAT -DHAVE_MKDIR -DHAVE_FCNTL_H -w -I$PORT/src/game"

echo "host-build: compiling"
start=$SECONDS
ls src/game/*.c src/host/*.c > "$B/files.txt"
nsrc=$(wc -l < "$B/files.txt")

# One gcc per source, 12 at a time. The -n1 keeps a single failure from hiding the rest.
xargs -P 12 -n 1 -I{} sh -c \
  'gcc '"$CFLAGS"' -c "$1" -o "'"$B"'/obj/$(basename "${1%.c}").o" 2>> "'"$B"'/cc.log" || echo "FAILED $1" >> "'"$B"'/fail.log"' \
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
gcc -o "$B/angband-test" "$B"/obj/*.o -lm || exit 1

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
