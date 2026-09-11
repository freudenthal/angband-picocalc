#!/usr/bin/env bash
# The suite, part 5, for angband-picocalc (stage 060).
#
#   cd /c/Users/greenblob/Documents/PicoCalc && source tools/pico-env.sh \
#     && angband-pico/tools/port-warnings.sh
#
# compile-sweep.sh only sweeps src/game/, so the port's own sources are not covered by it
# (the stage 040 rule). Stage 050 checked them by hand; this does the same thing
# reproducibly, by asking the configured build tree for each file's real compile command
# and re-running it with -Wall -Wextra -fsyntax-only.
#
# Re-running the recorded command is the point: a hand-written include path picks up the
# SDK's *host* headers and produces pages of warnings that have nothing to do with this
# port. The build tree already knows the right one.
#
# Requires angband-pico/build-pico2 to be configured and built at least once.
#
# Stage 070: the build tree can be named, so the same check runs over the ANGBAND_TURN_LOG
# build where src/platform/turnlog.c is not an empty translation unit:
#
#   PORT_WARNINGS_BUILD=build-pico2-log angband-pico/tools/port-warnings.sh

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$PORT/${PORT_WARNINGS_BUILD:-build-pico2}"

if [ ! -f "$B/build.ninja" ]; then
	echo "port-warnings: no $B/build.ninja -- configure the build tree first"
	exit 2
fi

FILES="src/main.c
src/platform/main-pico.c
src/platform/utf8.c
src/platform/psram_heap.c
src/platform/sd_fs.c
src/platform/syscalls.c
src/platform/turnlog.c"

python - "$B" "$FILES" <<'PYEOF'
import os, re, subprocess, sys

b, files = sys.argv[1], sys.argv[2].split("\n")
os.chdir(b)

targets = subprocess.run(["ninja", "-t", "targets", "all"],
                         capture_output=True, text=True).stdout

total = 0
for f in files:
    obj = None
    for line in targets.split("\n"):
        name = line.split(":")[0]
        if name.endswith(f + ".obj") and ".dir/" in name:
            obj = name
            break
    if not obj:
        print("port-warnings: %-32s NO OBJECT" % f)
        continue

    cmd = subprocess.run(["ninja", "-t", "commands", obj],
                         capture_output=True, text=True).stdout.strip().split("\n")[-1]
    cmd = cmd.replace(" -c ", " -Wall -Wextra -fsyntax-only -c ", 1)

    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    n = len(re.findall(r": warning:", out)) + len(re.findall(r": error:", out))
    total += n
    print("port-warnings: %-32s %d" % (f, n))
    if n:
        print(out)

print("port-warnings: %d warnings or errors in %d port sources" % (total, len(files)))
sys.exit(1 if total else 0)
PYEOF
