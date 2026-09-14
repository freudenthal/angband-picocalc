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
# Stage 090: TWO TREES BY DEFAULT, one per half of every #ifdef ANGBAND_CONSOLE --
# build-pico2 (the shipped build, console OFF) and build-pico2-console (console ON). Both
# must be configured. A -D on the release tree's command is not enough: the console half of
# src/main.c includes pico/stdio_usb.h, whose include path only a console tree has.
#
# Stage 070: one build tree can be named instead, so the same check runs over the
# ANGBAND_TURN_LOG build where src/platform/turnlog.c is not an empty translation unit:
#
#   PORT_WARNINGS_BUILD=build-pico2-log angband-pico/tools/port-warnings.sh
#
# Proven to fail (stage 090): a -Wsign-compare planted in each half of main.c in turn gives
# 1 on that tree's line and exit 1. Before stage 090 it could not fail at all -- see the
# comment on subprocess.run below.

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREES="${PORT_WARNINGS_BUILD:-build-pico2 build-pico2-console}"

for t in $TREES; do
	if [ ! -f "$PORT/$t/build.ninja" ]; then
		echo "port-warnings: no $PORT/$t/build.ninja -- configure the build tree first"
		exit 2
	fi
done

FILES="src/main.c
src/platform/main-pico.c
src/platform/utf8.c
src/platform/psram_heap.c
src/platform/sd_fs.c
src/platform/syscalls.c
src/platform/turnlog.c
src/platform/battery.c"

python - "$PORT" "$TREES" "$FILES" <<'PYEOF'
import os, re, subprocess, sys

port, trees, files = sys.argv[1], sys.argv[2].split(), sys.argv[3].split("\n")

total = 0
for tree in trees:
    os.chdir(os.path.join(port, tree))
    targets = subprocess.run(["ninja", "-t", "targets", "all"],
                             capture_output=True, text=True).stdout

    for f in files:
        obj = None
        for line in targets.split("\n"):
            name = line.split(":")[0]
            if name.endswith(f + ".obj") and ".dir/" in name:
                obj = name
                break
        if not obj:
            print("port-warnings: %-20s %-26s NO OBJECT" % (tree, f))
            total += 1
            continue

        cmd = subprocess.run(["ninja", "-t", "commands", obj],
                             capture_output=True, text=True).stdout.strip().split("\n")[-1]
        cmd = cmd.replace(" -c ", " -Wall -Wextra -fsyntax-only -c ", 1)

        # NOT shell=True on Windows. cmd.exe refuses a line over 8,191 characters ("The
        # command line is too long.", exit 1) and main.c's line is longer, so until stage 090
        # this script compiled nothing, found no "warning:" in cmd.exe's complaint, and
        # printed 0. A string with shell=False goes straight to CreateProcess (32,767), which
        # is what ninja itself does. And a non-zero exit with no diagnostic is a failure.
        r = subprocess.run(cmd, shell=(os.name != "nt"), capture_output=True, text=True)
        out = (r.stdout + r.stderr).strip()
        n = len(re.findall(r": warning:", out)) + len(re.findall(r": error:", out))
        if r.returncode != 0 and n == 0:
            n = 1
        total += n
        print("port-warnings: %-20s %-26s %d" % (tree, f, n))
        if n:
            print(out)

print("port-warnings: %d warnings or errors in %d port sources over %s"
      % (total, len(files), " and ".join(trees)))
sys.exit(1 if total else 0)
PYEOF
