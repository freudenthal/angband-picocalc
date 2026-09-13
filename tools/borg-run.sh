#!/usr/bin/env bash
# PORT: port-written for angband-pico, picocalc-device-harness stage 055.
#
# Drive build-host-borg/angband-borg from Git Bash. It is a WSL binary, so every invocation
# has to cross into WSL and every path has to be a /mnt/c one; this script is that wrapper
# and nothing else.
#
#   angband-pico/tools/borg-run.sh birth                  # make the savefile
#   angband-pico/tools/borg-run.sh play  <stem> <turns>    # run the borg, write .keys/.sync
#   angband-pico/tools/borg-run.sh replay <stem> <keys> <turns>
#
# HOST_TAG and RUNNER pick a cross-compiled build and an emulator; see below.
#
# Everything lands under angband-pico/build-borg-run/, which is gitignored. The
# savefile, the borg's own log and the two output files all live there, so one directory is
# the whole record of a run.
#
# WHY THE HOME DIRECTORY MATTERS. init_file_paths() puts the save directory under
# ANGBAND_DIR_USER, which without USE_PRIVATE_PATHS is <lib>/user/. -d points every path at
# the port's own lib/, so the borg reads THE DEVICE'S gamedata -- but it would then also
# write into the tracked lib/ tree. So lib/ is copied into the run directory first and -d
# points at the copy: the borg reads identical data and writes nowhere that git sees.

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPORT="$(echo "$PORT" | sed 's|^/\([a-zA-Z]\)/|/mnt/\1/|')"

MODE="${1:-}"
[ -n "$MODE" ] || { echo "borg-run: birth | play <stem> <turns> | replay <stem> <keys> <turns>"; exit 2; }

# HOST_TAG and RUNNER (harness stage 056). HOST_TAG selects the build directory that
# tools/host-build.sh wrote with the same variable set, and RUNNER prefixes the binary. A
# cross-compiled borg is not executable on this WSL, so it is run under an emulator:
#
#   HOST_TAG=arm RUNNER=qemu-arm-static angband-pico/tools/borg-run.sh replay arm1 borg.keys 5
#
# Both unset, this is the x86 build run directly, exactly as before stage 056. The outputs
# still land in one build-borg-run/ whatever the architecture, because comparing an ARM
# .sync against an x86 .sync is the entire point.
HOST_TAG="${HOST_TAG:-}"
RUNNER="${RUNNER:-}"

BIN="$PORT/build-host-borg${HOST_TAG:+-$HOST_TAG}/angband-borg"
[ -f "$BIN" ] || { echo "borg-run: $BIN not built. Run tools/host-build.sh --borg"; exit 2; }

# NOT under build-host-borg/: tools/host-build.sh --borg does rm -rf on that directory, so a
# rebuild would take the savefile and the run with it. Still build*/ and so still gitignored.
RUN="$PORT/build-borg-run"
mkdir -p "$RUN"

# One copy of lib/, refreshed only when it is missing, so a play and a replay in the same
# session read byte-identical gamedata.
if [ ! -d "$RUN/lib/gamedata" ]; then
	echo "borg-run: staging lib/ into $RUN/lib"
	rm -rf "$RUN/lib"
	cp -r "$PORT/lib" "$RUN/lib"
fi

# ANGBAND_DIR_USER for this run is $RUN/lib/user, and that is where borg-init.c:192 looks
# for borg.txt. Without it the borg prints a three-line warning ending in a -more- BEFORE it
# has installed its keypress hook, so nothing clears the prompt and the run stops there.
# tools/borg.txt is the borg's own default table, written down.
mkdir -p "$RUN/lib/user"
cp "$PORT/tools/borg.txt" "$RUN/lib/user/borg.txt"

# THE MASTER SAVEFILE.
#
# A run does not leave the savefile alone: Angband autosaves on a level change and the borg
# has an autosave of its own (off in tools/borg.txt, but the game's is not an option here).
# A second run from a rewritten savefile is a DIFFERENT GAME, and "the borg is
# deterministic" would then be untestable. So `birth` takes a copy the moment the file is
# written, and every play and replay restores it first.
#
# It is also the file the device's SD card is staged from: lockstep needs the two sides to
# load the same bytes, and this is the copy that is known not to have been played from.
MASTER="$RUN/HARNESS.master"

restore_master() {
	[ -f "$MASTER" ] || { echo "borg-run: no $MASTER. Run 'birth' first."; exit 2; }
	cp "$MASTER" "$RUN/lib/user/save/HARNESS"
	echo "borg-run: savefile restored from $(md5sum "$MASTER" | cut -c1-32)"
}

case "$MODE" in
	birth)
		wsl.exe -e bash -lc "cd '$WPORT/build-borg-run' && $RUNNER $WPORT/build-host-borg${HOST_TAG:+-$HOST_TAG}/angband-borg -uHARNESS -d./lib -n -v"
		rc=$?
		if [ -f "$RUN/lib/user/save/HARNESS" ]; then
			cp "$RUN/lib/user/save/HARNESS" "$MASTER"
			echo "borg-run: master savefile $(md5sum "$MASTER" | cut -c1-32)"
		fi
		exit $rc
		;;
	play)
		STEM="${2:-borg}"; TURNS="${3:-1000}"
		restore_master
		wsl.exe -e bash -lc "cd '$WPORT/build-borg-run' && $RUNNER $WPORT/build-host-borg${HOST_TAG:+-$HOST_TAG}/angband-borg -uHARNESS -d./lib -o'$STEM' -t'$TURNS'"
		;;
	replay)
		STEM="${2:-replay}"; KEYS="${3:-borg.keys}"; TURNS="${4:-1000}"
		restore_master
		wsl.exe -e bash -lc "cd '$WPORT/build-borg-run' && $RUNNER $WPORT/build-host-borg${HOST_TAG:+-$HOST_TAG}/angband-borg -uHARNESS -d./lib -o'$STEM' -r'$KEYS' -t'$TURNS'"
		;;
	*)
		echo "borg-run: unknown mode '$MODE'"; exit 2
		;;
esac
