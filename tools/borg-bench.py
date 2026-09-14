#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 065. ONE OPTIMISATION ROUND, END TO END.
#
#   "C:\Program Files\Python312\python.exe" angband-pico/tools/borg-bench.py \
#       build-pico2-bench <tag> [--compare-to <tag>] [--limit 250]
#
#   ... --sweep                     the four .text placements, back to back, unattended,
#                                   and the error bar written out at the end
#   ... --selftest                  every refusal demonstrated, no device touched
#   ... --error-bar-from a b c d    recompute the error bar from four tags already run
#
# WHAT A ROUND IS. One build tree in, one table out, about twenty unattended minutes, and a
# verdict that can be trusted. The three things that make the verdict trustworthy are not
# the timing code; they are the refusals:
#
#   1. the build tree must be the bench recipe, or the round does not start;
#   2. the edit must not have changed the game, checked ON THE DESK before any device time;
#   3. the device must have played the same 250 commands the host played, checked by
#      lockstep against the host's own SYNC records;
#   4. the capture must have no gap in its record numbering.
#
# A round measured on the wrong binary, or on a binary that changed the game, or reduced
# from a capture with a hole in it, is worse than a round not taken: it produces a number
# that looks like every other number in the run log and is not comparable with any of them.
#
# WHY THE WORKLOAD IS THE BORG AND NOT A PERSON. Optimisation is many rounds of a few per
# cent each, and a few per cent is invisible against a workload that changes every time you
# measure it. angband-pico/build-borg-run/borg-arm.keys is 2,373 key names that play 1,001
# identical commands, for ever, and borg-arm.sync is what the host got for them.
#
# WHY 250 COMMANDS AND NOT 1,001. Thirteen minutes of device time against fifty-one, and
# still well over a hundred depth-1 samples -- far more than a median needs -- and still
# 250 commands of proven-identical play. Flash and boot are about 4.7 minutes on top, so a
# round is about twenty minutes and three fit in an hour. The full 1,001 is run at the end
# of any round that claims a win, and once for endurance (stage 070 item 11).
#
# LANGUAGE AND INTERPRETER. Python, run by the WINDOWS interpreter: four of the seven steps
# are tools/harness/harness.py and one is angband-pico/tools/turnlog.py, all of which drive
# a COM port or read a Windows path. Call it by absolute path -- `py` resolves to the Store
# stub in a non-interactive shell. The desk-side pre-check is the one step that lives in
# WSL, and it gets there the way every other tool in this tree does: through Git Bash, which
# runs tools/host-build.sh, which crosses into WSL itself.
#
# encoding='utf-8' on every file: python on this machine defaults to cp1252 and has
# silently corrupted a file in this project before (stage 020 correction).

import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)                       # angband-pico/
WS = os.path.dirname(PORT)                         # the workspace root

HARNESS = os.path.join(WS, "tools", "harness", "harness.py")
SESSION = os.path.join(WS, "tools", "harness", "sessions", "angband-bench.txt")
TURNLOG = os.path.join(HERE, "turnlog.py")
BENCH = os.path.join(WS, ".llm", "scratch", "bench")
ERROR_BAR_FILE = os.path.join(BENCH, "error-bar.json")

RUNDIR = os.path.join(PORT, "build-borg-run")
KEYS = os.path.join(RUNDIR, "borg-arm.keys")
HOSTSYNC = os.path.join(RUNDIR, "borg-arm.sync")
MASTER = os.path.join(RUNDIR, "HARNESS.master")

GIT_BASH = r"C:\Program Files\Git\bin\bash.exe"

# THE BENCH RECIPE. Anything else is refused. The mirror is OFF and not merely unset:
# CMakeLists.txt refuses ANGBAND_SERIAL_SCREEN with ANGBAND_TURN_LOG anyway, because a
# panel dump served inside check_events()'s idle bracket is subtracted from `tot` and not
# from the wall clock, so a session that polls the screen has honest per-phase figures and
# a dishonest elapsed time with nothing in the capture saying which records were dumped
# over. This check is the second lock on that door, at the point of measurement.
RECIPE = [
    ("ANGBAND_SERIAL_KEYS", "ON"),
    ("ANGBAND_SYNC", "ON"),
    ("ANGBAND_TURN_LOG", "ON"),
    ("ANGBAND_SAVE_RESTORE", "ON"),
    ("ANGBAND_SERIAL_SCREEN", "OFF"),
]

DEFAULT_LIMIT = 250

# The four placements of stage 065 item 4. The XIP cache's set index is address bits
# [12:3] (specifications.md 6.4, derived from the SDK headers), so the mapping repeats
# every 8 KB and 0/2/4/6 KB is one full period.
SWEEP_TREES = [
    ("build-pico2-bench", "pad0", 0),
    ("build-pico2-bench-pad2k", "pad2k", 2048),
    ("build-pico2-bench-pad4k", "pad4k", 4096),
    ("build-pico2-bench-pad6k", "pad6k", 6144),
]

# The start of one boot in a capture, printed by src/main.c's boot_say().
BOOT_MARK = re.compile(r"on the PicoCalc -- stage 050 bring-up")
SAVE_RESTORE = re.compile(r"save-restore: (\w+)[^0-9]*(\d+) B in (\d+) ms")
INIT_TOOK = re.compile(r"init_angband\(\) took (\d+) ms")
LEADING_SYNC = re.compile(r"^SYNC 1 y=\S+ x=\S+ chp=\S+ au=\S+ dl=\S+ t=\S+ "
                          r"si=(?P<si>\d+) rv=\S+ rs=(?P<rs>[0-9a-f]+) d=\S+", re.M)
HOST_SAVED_RS = re.compile(r"rng: playing\s+si=(\d+)\s+rs=([0-9a-f]+)\s+\(saved\)")


class Refusal(Exception):
    """A round that must not produce a number. Exit 2, and say which of the four."""


def say(text=""):
    print(text)
    sys.stdout.flush()


def rule(text):
    say("")
    say("=" * 78)
    say(text)
    say("=" * 78)


def posix(path):
    """C:\\Users\\x -> /c/Users/x, which is what Git Bash wants."""
    p = os.path.abspath(path).replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        p = "/" + p[0].lower() + p[2:]
    return p


def tee(argv, cwd=WS, env=None):
    """Run a command, echo its output as it arrives, return (returncode, text)."""
    say("$ " + " ".join(argv if isinstance(argv, list) else [argv]))
    proc = subprocess.Popen(
        argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)
    out = []
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        out.append(line)
    proc.wait()
    return proc.returncode, "".join(out)


def bash(command):
    """Run one Git Bash command line from the workspace root."""
    return tee([GIT_BASH, "-c", "cd '%s' && %s" % (posix(WS), command)])


# ------------------------------------------------------------------------------------
# Refusal 1: the build tree must be the bench recipe.

def read_cache(build_dir):
    path = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.isfile(path):
        raise Refusal("no CMakeCache.txt in %s -- that is not a configured build tree"
                      % build_dir)
    out = {}
    with io.open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line.strip())
            if m:
                out[m.group(1)] = m.group(2)
    return out


def check_tree(build_dir):
    """Refusal 1. Returns the quoted CMakeCache lines, which the run log wants."""
    cache = read_cache(build_dir)
    wrong = [(k, want, cache.get(k, "<unset>"))
             for k, want in RECIPE if cache.get(k, "OFF") != want]
    if wrong:
        detail = "; ".join("%s is %s, the bench needs %s" % (k, got, want)
                           for k, want, got in wrong)
        raise Refusal(
            "%s is not the bench recipe: %s. A round measured on the wrong binary is "
            "worse than a round not taken." % (os.path.basename(build_dir), detail))

    quoted = ["%s:%s" % (k, v) for k, v in sorted(cache.items())
              if k.startswith("ANGBAND_")]
    return cache, quoted


def check_fresh(build_dir):
    """Invariant 10: nothing measured may be older than the tree it came from."""
    uf2 = os.path.join(build_dir, "angband.uf2")
    if not os.path.isfile(uf2):
        raise Refusal("%s has no angband.uf2 -- build the target first" % build_dir)

    newest, newest_name = 0.0, None
    for root, dirs, files in os.walk(os.path.join(PORT, "src")):
        for name in files:
            if not name.endswith((".c", ".h", ".S", ".ld")):
                continue
            t = os.path.getmtime(os.path.join(root, name))
            if t > newest:
                newest, newest_name = t, os.path.join(root, name)
    for name in ("CMakeLists.txt",):
        t = os.path.getmtime(os.path.join(PORT, name))
        if t > newest:
            newest, newest_name = t, os.path.join(PORT, name)

    if os.path.getmtime(uf2) < newest:
        raise Refusal(
            "%s is older than %s. Invariant 10: nothing measured is older than the tree "
            "it came from -- stage 070's six staged UF2s were all invalidated by exactly "
            "this, and the placement experiment compares .text addresses, so a stale pad "
            "poisons the thing it exists to measure."
            % (os.path.relpath(uf2, WS), os.path.relpath(newest_name, WS)))
    return uf2


def elf_sizes(build_dir):
    """text/data/bss beside every round. Stage 060's binding correction."""
    elf = os.path.join(build_dir, "angband.elf")
    size = shutil.which("arm-none-eabi-size") or os.path.join(
        WS, "arm-gnu-toolchain-14.2.rel1", "bin", "arm-none-eabi-size.exe")
    if not os.path.isfile(elf) or not os.path.isfile(size):
        return None
    try:
        out = subprocess.run([size, elf], capture_output=True, text=True).stdout
        parts = out.strip().splitlines()[-1].split()
        return {"text": int(parts[0]), "data": int(parts[1]), "bss": int(parts[2])}
    except (IndexError, ValueError, OSError):
        return None


# ------------------------------------------------------------------------------------
# Refusal 2: the desk-side semantic pre-check.

def precheck(rebuild=True, reference=HOSTSYNC):
    """Refusal 2, and the best twenty seconds in this project.

    Every one of stage 070's src/game/ edits is compiled into the PC host too, because
    host-build.sh --borg --arm builds the same src/game/. So an edit that changes
    behaviour changes the host's .sync as well, and that is visible ON THE DESK before the
    board is touched. 1,001 commands of identical RNG digests is a regression test for an
    optimisation that no unit test in this tree comes close to.

    Returns the host's saved RNG state, which is what the device's savefile has to be at.
    """
    rule("PRE-CHECK (desk side, no device): does this tree still play the same game?")

    if rebuild:
        rc, _ = bash("angband-pico/tools/host-build.sh --borg --arm")
        if rc != 0:
            raise Refusal("the host borg build failed (exit %d). Nothing can be compared "
                          "until it builds." % rc)

    rc, out = bash("HOST_TAG=arm angband-pico/tools/borg-run.sh replay chk borg-arm.keys 1000")
    if rc != 0:
        raise Refusal("the host replay failed (exit %d)" % rc)

    m = HOST_SAVED_RS.search(out)
    saved = {"si": m.group(1), "rs": m.group(2)} if m else None

    chk = os.path.join(RUNDIR, "chk.sync")
    if not os.path.isfile(chk):
        raise Refusal("the replay wrote no chk.sync")

    with io.open(chk, "rb") as f:
        a = f.read()
    with io.open(reference, "rb") as f:
        b = f.read()

    if a != b:
        first = 0
        for i, (x, y) in enumerate(zip(a.splitlines(), b.splitlines()), 1):
            if x != y:
                first = i
                break
        raise Refusal(
            "THE EDIT CHANGED THE GAME. chk.sync differs from %s, first at record %d. "
            "For a linker or -O change that is impossible by construction, so it means "
            "something else moved; for a cave.h / cave-view.c / game-world.c edit it is a "
            "BUG IN THE OPTIMISATION and it has been caught for twenty seconds instead of "
            "an hour. Fix it, or -- if the change is intended and justified -- regenerate "
            "the keystream and say so in the run log, knowing that every earlier round's "
            "table is then no longer comparable."
            % (os.path.relpath(reference, WS), first or 1))

    say("")
    say("pre-check: PASS -- %d records identical. The edit is behaviour-preserving over "
        "1,001 commands of real play." % len(b.splitlines()))
    if saved:
        say("pre-check: the savefile the host loaded is at si=%s rs=%s. The device has to "
            "start there too, which is what ANGBAND_SAVE_RESTORE is for."
            % (saved["si"], saved["rs"]))
    return saved


# ------------------------------------------------------------------------------------
# The device half.

def flash_and_boot(uf2, capture):
    rule("FLASH AND BOOT (about 4.7 minutes)")
    rc, out = tee([sys.executable, HARNESS, "flash", uf2,
                   "--out", capture, "--script", SESSION])
    if rc != 0:
        raise Refusal("harness flash/boot exited %d -- there is no booted game to measure"
                      % rc)
    return out


def check_boot(capture, saved):
    """The assertion that item 1 is working, before a single key of the keystream.

    TWO CHECKS, THE MECHANISM AND ITS RESULT.

    1. The device's own `save-restore: copied N B in M ms` line, with N checked against
       HARNESS.master's size. That says the copy happened.
    2. THE LEADING SYNC RECORD, whose si=/rs= must equal the host's
       `rng: playing ... (saved)` figure. That says the game the device loaded is at the
       state the host's game was at. The first play_game() pass after the savefile loads
       emits a record with no key sent -- `SYNC 1 ... dl=0 t=1 si=19 rs=853891f9` --
       and sessions/angband-bench.txt waits for it by name.

    An earlier version of this function said that record did not exist, and the first
    device round (s065-r1, 2026-09-13) proved otherwise the expensive way: the record
    arrived after lockstep had started, was paired with the host's record 1, and was
    reported as a divergence at command 1 while the device's record 2 matched the host's
    record 1 byte for byte.
    """
    rule("BOOT CHECK: did the savefile restore itself? (stage 065 item 1)")
    text = slice_last_boot(read_text(capture))

    m = SAVE_RESTORE.search(text)
    if not m:
        raise Refusal(
            "no `save-restore:` line in the boot. Either the binary was not built with "
            "-DANGBAND_SAVE_RESTORE=ON -- which check_tree() should have caught -- or the "
            "card has no /angband/lib/user/save/PicoCalc.master on it. Without the "
            "restore, this round starts from whatever the LAST round left on the card, "
            "and a savefile difference is a divergence by definition.")

    verb, nbytes, took = m.group(1), int(m.group(2)), int(m.group(3))
    if verb != "copied":
        raise Refusal("the restore reported `%s` and moved %d B -- a short or failed copy "
                      "diverges at command 1 and looks like an optimisation bug"
                      % (verb, nbytes))

    want = os.path.getsize(MASTER) if os.path.isfile(MASTER) else None
    if want is not None and nbytes != want:
        raise Refusal("the restore copied %d B and %s is %d B. The card's master is not "
                      "the master the host borg played." % (nbytes, os.path.basename(MASTER), want))

    say("save-restore: copied %d B in %d ms -- at boot, OUTSIDE every measured bracket."
        % (nbytes, took))
    if want is not None:
        say("save-restore: matches %s exactly (%d B)." % (os.path.basename(MASTER), want))
    lead = LEADING_SYNC.search(text)
    if not lead:
        raise Refusal(
            "no leading SYNC record in the boot slice. The first play_game() pass after the "
            "savefile loads emits one with no key sent, and sessions/angband-bench.txt waits "
            "for it by name -- so if it is missing, the script did not run to the end, and "
            "if lockstep is started now it will pair that record with the host's record 1 "
            "and call the round a divergence.")
    say("leading record: %s" % lead.group(0).strip())
    if saved:
        if lead.group("si") != saved["si"] or lead.group("rs") != saved["rs"]:
            raise Refusal(
                "the device loaded its savefile at si=%s rs=%s and the host loaded its copy at "
                "si=%s rs=%s. They are not the same savefile, and every record after this one "
                "would diverge for a reason that has nothing to do with the binary."
                % (lead.group("si"), lead.group("rs"), saved["si"], saved["rs"]))
        say("leading record: si=%s rs=%s EQUALS the host's saved state. The device starts "
            "from the game the host played." % (saved["si"], saved["rs"]))
    else:
        say("leading record: no pre-check ran, so there is no host figure to compare it "
            "with. Lockstep's record 1 is the only check this round has.")

    m = INIT_TOOK.search(text)
    if m:
        say("boot: init_angband() took %s ms" % m.group(1))
    return nbytes, took


def lockstep(capture, limit):
    rule("LOCKSTEP: %d keys, the host's keys, the host's answers" % limit)
    argv = [sys.executable, HARNESS, "lockstep", KEYS, HOSTSYNC, "--out", capture]
    if limit:
        argv += ["--limit", str(limit)]
    rc, out = tee(argv)
    if rc != 0:
        raise Refusal(
            "lockstep exited %d. Exit 4 is a divergence and exit 5 a level-change timeout; "
            "either way the binary under test did not play the game the host played, and "
            "NO TIMING NUMBER FROM THIS RUN MEANS ANYTHING. The round ends here." % rc)
    return out


# ------------------------------------------------------------------------------------
# Reduction.

def read_text(path):
    with io.open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def slice_last_boot(text):
    """Everything from the last boot banner on.

    A capture can hold several boots -- the driver opens them in append mode on purpose,
    so that one file is the whole record of a session -- and a table reduced from two
    boots of one binary is two workloads averaged together.
    """
    lines = text.splitlines(True)
    start = 0
    for i, line in enumerate(lines):
        if BOOT_MARK.search(line):
            start = i
    return "".join(lines[start:])


def reduce_round(capture, tag):
    """Refusal 4, and the table."""
    rule("REDUCE: the last boot out of the capture, through turnlog.py")

    out_txt = os.path.join(BENCH, tag + ".txt")
    with io.open(out_txt, "w", encoding="utf-8", newline="\n") as f:
        f.write(slice_last_boot(read_text(capture)))

    # The numbering is checked BEFORE the table is printed, not after. turnlog.py reports
    # a gap and then goes on to print medians anyway -- which is right for a human reading
    # a capture by hand and wrong here, because a table that reaches the run log is a
    # table someone will compare with another one.
    numbers = [int(l.split()[1]) for l in read_text(out_txt).splitlines()
               if l.startswith("TL ") and len(l.split()) >= 18]
    # Stage 075: turnlog.c appends columns after the stage 070 seventeen; the prefix is
    # unchanged, so a record is one with AT LEAST the stage 070 fields.
    holes = [(a, b) for a, b in zip(numbers, numbers[1:]) if b != a + 1]
    if holes:
        raise Refusal(
            "the turn log has %d gap(s) in its record numbering (%s). A dropped record is "
            "a dropped command, so the capture cannot be compared with one without a gap, "
            "and the round ends here rather than producing a table that looks like every "
            "other table in the run log."
            % (len(holes), ", ".join("%d->%d" % h for h in holes[:5])))
    if not numbers:
        raise Refusal("no TL records in the boot slice of %s -- was the build configured "
                      "-DANGBAND_TURN_LOG=ON, and did the game reach a command prompt?"
                      % os.path.relpath(capture, WS))

    rc, out = tee([sys.executable, TURNLOG, out_txt])
    if rc != 0:
        raise Refusal("turnlog.py exited %d on %s" % (rc, out_txt))

    out_table = os.path.join(BENCH, tag + ".table")
    with io.open(out_table, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    say("")
    say("borg-bench: wrote %s" % os.path.relpath(out_txt, WS))
    say("borg-bench: wrote %s" % os.path.relpath(out_table, WS))
    return out_txt, out


def median_of(path, depth, key="tot"):
    """The one number the sweep compares. Nearest-rank, as turnlog.py's is."""
    vals = []
    fields = ["n", "turn", "d", "tot", "was", "lit", "upd", "fgn", "mkn", "scn", "trp",
              "mon", "wld", "gen", "frm", "idle", "brk"]
    for line in io.open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line.startswith("TL "):
            continue
        parts = line.split()[1:]
        if len(parts) < len(fields):	# stage 075 columns follow the stage 070 prefix
            continue
        rec = dict(zip(fields, parts))
        if int(rec["d"]) == depth:
            vals.append(int(rec[key]))
    if not vals:
        return None
    s = sorted(vals)
    k = max(0, min(len(s) - 1, int(round(0.5 * len(s) + 0.5)) - 1))
    return s[k] / 1000.0


# ------------------------------------------------------------------------------------
# One round.

def one_round(build_dir, tag, limit, compare_to, rebuild_host, do_precheck):
    started = time.time()
    if not os.path.isabs(build_dir):
        build_dir = os.path.join(PORT, build_dir)
    os.makedirs(BENCH, exist_ok=True)

    rule("ROUND %s -- %s" % (tag, os.path.basename(build_dir)))

    cache, quoted = check_tree(build_dir)
    uf2 = check_fresh(build_dir)
    sizes = elf_sizes(build_dir)
    say("build tree: %s" % os.path.relpath(build_dir, WS))
    for line in quoted:
        say("  CMakeCache.txt: %s" % line)
    if sizes:
        say("  text %d  data %d  bss %d" % (sizes["text"], sizes["data"], sizes["bss"]))

    saved = precheck(rebuild=rebuild_host) if do_precheck else None
    if not do_precheck:
        say("")
        say("borg-bench: --no-precheck. The desk-side check did not run, so this round "
            "does NOT know whether the edit changed the game. Say so in the run log.")

    capture = os.path.join(BENCH, tag + "-capture.txt")
    if os.path.exists(capture):
        os.remove(capture)

    flash_and_boot(uf2, capture)
    restored, restore_ms = check_boot(capture, saved)
    lockstep(capture, limit)
    out_txt, table = reduce_round(capture, tag)

    elapsed = time.time() - started
    meta = {
        "tag": tag,
        "build": os.path.relpath(build_dir, WS).replace("\\", "/"),
        "options": quoted,
        "sizes": sizes,
        "pad": int(cache.get("ANGBAND_TEXT_PAD", "0")),
        "limit": limit,
        "restore_bytes": restored,
        "restore_ms": restore_ms,
        "town_median_ms": median_of(out_txt, 0),
        "dungeon_median_ms": median_of(out_txt, 1),
        "round_seconds": round(elapsed, 1),
    }
    with io.open(os.path.join(BENCH, tag + ".json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, sort_keys=True)

    rule("ROUND %s DONE in %.1f min" % (tag, elapsed / 60.0))
    say("town median    %s ms" % meta["town_median_ms"])
    say("dungeon median %s ms" % meta["dungeon_median_ms"])

    if compare_to:
        rule("COMPARE %s against %s" % (tag, compare_to))
        tee([sys.executable, TURNLOG, "--compare", tag, compare_to])

    return meta


# ------------------------------------------------------------------------------------
# The sweep, and the error bar it exists to produce.

PHASES = ["tot", "was", "lit", "upd", "fgn", "mkn", "scn", "trp", "mon", "wld", "SWEEPS"]
TL_FIELDS = ["n", "turn", "d", "tot", "was", "lit", "upd", "fgn", "mkn", "scn", "trp",
             "mon", "wld", "gen", "frm", "idle", "brk"]
TL_LEAVES = ["was", "lit", "upd", "fgn", "mkn", "scn", "trp"]


def phase_medians(tag):
    """{depth: {phase: nearest-rank median in ms}} for one round's reduced capture."""
    rows = {}
    for line in io.open(os.path.join(BENCH, tag + ".txt"), encoding="utf-8", errors="replace"):
        parts = line.split()
        if not line.startswith("TL ") or len(parts) < len(TL_FIELDS) + 1:  # stage 075 appends
            continue
        rec = dict(zip(TL_FIELDS, map(int, parts[1:])))
        rec["SWEEPS"] = sum(rec[k] for k in TL_LEAVES)
        rows.setdefault(rec["d"], []).append(rec)

    def med(v):
        s = sorted(v)
        k = max(0, min(len(s) - 1, int(round(0.5 * len(s) + 0.5)) - 1))
        return s[k] / 1000.0

    return {d: {k: med([r[k] for r in rs]) for k in PHASES} for d, rs in rows.items()}


def write_error_bar(metas):
    """The spread of the medians across the placements IS the error bar on every
    flash-resident measurement in this project -- PER DEPTH AND PER PHASE.

    THE FIRST VERSION OF THIS FUNCTION WROTE ONE NUMBER, AND THE SWEEP SHOWED WHY THAT IS
    WRONG (2026-09-13). Depth-1 `tot` spread 2.30 % across the four pads, but only because
    the phases moved in opposite directions and cancelled: `lit` fell 6.8 %, `upd` rose
    5.5 %, `mkn` moved 21 %, and in the town `lit` rose 47 % at one placement. A single
    2.3 % applied to every row would have called a 5 % `lit` edit a win that placement alone
    can produce. So every (depth, phase) gets its own bar, and turnlog.py --compare uses the
    bar for the row it is judging.

    Each bar is the spread as a percentage of the SMALLEST median, which is the conservative
    reading: the largest amount the same source at a different .text address was seen to
    differ by. `pct` keeps the depth-1 `tot` figure as the headline.

    PASS THE REPEAT RUNS TOO, NOT ONLY ONE ROUND PER PAD. A bar from four placements alone
    does not contain run-to-run noise on the small phases: the town's `trp` is 5 ms over
    107 records, spread 3.9 % across the four pads and 6.2 % between two runs of ONE
    binary. The bar is the spread over every round given, so it bounds placement and
    repetition together:

        borg-bench.py --error-bar-from s065-r1 s065-r2 s065-pad0 s065-pad2k s065-pad4k s065-pad6k
    """
    if len(metas) < 2:
        raise Refusal("an error bar needs at least two placements; got %d" % len(metas))

    per_leg = {m["tag"]: phase_medians(m["tag"]) for m in metas}
    depths = sorted(set.intersection(*[set(v) for v in per_leg.values()]))

    bars = {}
    for d in depths:
        bars[str(d)] = {}
        for k in PHASES:
            vals = [per_leg[t][d][k] for t in per_leg]
            lo, hi = min(vals), max(vals)
            # Unrounded: a stored 5.54 against a measured 5.5406 marked the very leg that
            # defines the bar as outside it.
            bars[str(d)][k] = (100.0 * (hi - lo) / lo) if lo else None

    vals = [per_leg[t][1]["tot"] for t in per_leg if 1 in per_leg[t]]
    lo, hi = min(vals), max(vals)
    pct = 100.0 * (hi - lo) / lo

    data = {
        "pct": pct,
        "bars": bars,
        "measured": time.strftime("%Y-%m-%d"),
        "note": ("spread of the medians across %d .text placements of one source; the "
                 "headline is depth-1 tot, %.2f..%.2f ms, and every (depth, phase) has its "
                 "own bar in `bars`" % (len(metas), lo, hi)),
        "legs": [{"tag": m["tag"], "pad": m["pad"],
                  "text": (m.get("sizes") or {}).get("text"),
                  "bss": (m.get("sizes") or {}).get("bss"),
                  "medians": {str(d): per_leg[m["tag"]][d] for d in depths}}
                 for m in metas],
    }
    with io.open(ERROR_BAR_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)

    rule("THE PLACEMENT ERROR BAR, PER DEPTH AND PER PHASE")
    for d in depths:
        say("")
        say("depth %d  %s" % (d, "  ".join("%9s" % m["tag"] for m in metas)) + "     bar")
        for k in PHASES:
            say("  %-6s %s   %6.2f %%"
                % (k, "  ".join("%9.2f" % per_leg[m["tag"]][d][k] for m in metas),
                   bars[str(d)][k] or 0.0))
    say("")
    say("headline: depth-1 tot spread %.2f ms on %.2f ms = %.2f %%" % (hi - lo, lo, pct))
    say("")
    say("A round whose delta on a phase is inside THAT PHASE'S bar has NOT demonstrated")
    say("anything -- it has measured where .text happened to land. Code in SRAM has no")
    say("placement sensitivity at all, which is the strongest argument there is for stage")
    say("070 item 3.")
    say("")
    say("borg-bench: wrote %s" % os.path.relpath(ERROR_BAR_FILE, WS))
    return data


def sweep(prefix, limit, rebuild_host, do_precheck):
    metas = []
    for i, (tree, leg, pad) in enumerate(SWEEP_TREES):
        tag = "%s-%s" % (prefix, leg)
        # The pre-check is about the SOURCE, and all four legs are the same source. Run it
        # once, on the first leg, and say so rather than spending three more minutes of a
        # sweep re-proving it.
        metas.append(one_round(tree, tag, limit, None, rebuild_host and i == 0,
                               do_precheck and i == 0))
    return write_error_bar(metas)


# ------------------------------------------------------------------------------------
# --selftest: every refusal, demonstrated, with no device attached.

def selftest():
    rule("SELFTEST: the four refusals, no device touched")
    failures = []

    def expect_refusal(what, fn):
        say("")
        say("--- %s" % what)
        try:
            fn()
        except Refusal as exc:
            say("REFUSED (correct): %s" % exc)
            return
        say("*** NOT REFUSED -- this is a defect in borg-bench.py ***")
        failures.append(what)

    # 1. A build tree with the wrong options: the SHIPPED build, where every bench option
    #    is off. This is the real check on a real tree.
    expect_refusal("1. a build tree that is not the bench recipe (build-pico2, shipped)",
                   lambda: check_tree(os.path.join(PORT, "build-pico2")))

    # 2. A failed pre-check, without spending 55 s proving the tree is fine: chk.sync from
    #    the last real replay against the X86 reference, which is a different game by
    #    construction (harness stage 057 -- the two backends build different caves from one
    #    savefile because the generator reads its RNG from unsequenced argument lists).
    x86 = os.path.join(RUNDIR, "borg.sync")
    if os.path.isfile(os.path.join(RUNDIR, "chk.sync")) and os.path.isfile(x86):
        expect_refusal("2. a desk-side pre-check whose .sync does not match the reference",
                       lambda: precheck(rebuild=False, reference=x86))
    else:
        say("")
        say("--- 2. SKIPPED: needs build-borg-run/chk.sync and borg.sync. Run the "
            "pre-check once first.")
        failures.append("2 (skipped)")

    # 3. A non-zero lockstep exit. The decision is the real one; the exit code is
    #    synthesised, because producing a genuine divergence needs a board and a
    #    deliberately broken binary.
    def fake_lockstep():
        rc = 4
        if rc != 0:
            raise Refusal(
                "lockstep exited %d. Exit 4 is a divergence and exit 5 a level-change "
                "timeout; either way the binary under test did not play the game the host "
                "played, and NO TIMING NUMBER FROM THIS RUN MEANS ANYTHING." % rc)
    expect_refusal("3. a non-zero lockstep exit (exit code synthesised, decision real)",
                   fake_lockstep)

    # 4. A break in the turn-log numbering, on a real capture with one record cut out.
    src = os.path.join(WS, ".llm", "scratch", "captures", "s058-longrun-run.txt")
    if os.path.isfile(src):
        os.makedirs(BENCH, exist_ok=True)
        holed = os.path.join(BENCH, "selftest-gap-capture.txt")
        lines = [l for l in io.open(src, encoding="utf-8", errors="replace")]
        with io.open(holed, "w", encoding="utf-8", newline="\n") as f:
            dropped = 0
            for l in lines:
                if l.startswith("TL 40 ") and not dropped:
                    dropped = 1
                    continue
                f.write(l)
        expect_refusal("4. a break in the turn-log record numbering (one TL line removed)",
                       lambda: reduce_round(holed, "selftest-gap"))
    else:
        say("")
        say("--- 4. SKIPPED: %s is not there" % src)
        failures.append("4 (skipped)")

    rule("SELFTEST: %s" % ("ALL FOUR REFUSALS DEMONSTRATED"
                           if not failures else "FAILED: " + ", ".join(failures)))
    return 0 if not failures else 1


# ------------------------------------------------------------------------------------

def main(argv):
    p = argparse.ArgumentParser(
        description="One optimisation round on the PicoCalc, end to end, with refusals.")
    p.add_argument("build", nargs="?", help="the bench build tree (a name under "
                                            "angband-pico/, or a path)")
    p.add_argument("tag", nargs="?", help="the round's name; files land in "
                                          ".llm/scratch/bench/<tag>.*")
    p.add_argument("--limit", type=int, default=DEFAULT_LIMIT,
                   help="KEYS to send, not commands (default %d = 229 commands; "
                        "1001 keys = 849 commands; the whole stream is 1,184 keys = 1,001 "
                        "commands)" % DEFAULT_LIMIT)
    p.add_argument("--compare-to", metavar="TAG",
                   help="print turnlog.py --compare against this earlier tag")
    p.add_argument("--sweep", metavar="PREFIX", nargs="?", const="sweep",
                   help="run the four .text placements back to back and write the error bar")
    p.add_argument("--no-precheck", action="store_true",
                   help="skip the desk-side semantic check; the run log must say so")
    p.add_argument("--no-host-rebuild", action="store_true",
                   help="pre-check against the host borg already built")
    p.add_argument("--selftest", action="store_true",
                   help="demonstrate every refusal; touches no device")
    p.add_argument("--error-bar-from", nargs="+", metavar="TAG",
                   help="recompute the error bar from tags already run")
    args = p.parse_args(argv[1:])

    try:
        if args.selftest:
            return selftest()

        if args.error_bar_from:
            metas = []
            for tag in args.error_bar_from:
                with io.open(os.path.join(BENCH, tag + ".json"), encoding="utf-8") as f:
                    metas.append(json.load(f))
            write_error_bar(metas)
            return 0

        if args.sweep:
            sweep(args.sweep, args.limit, not args.no_host_rebuild, not args.no_precheck)
            return 0

        if not args.build or not args.tag:
            p.print_help()
            return 2

        one_round(args.build, args.tag, args.limit, args.compare_to,
                  not args.no_host_rebuild, not args.no_precheck)
        return 0

    except Refusal as exc:
        say("")
        say("!" * 78)
        say("borg-bench: REFUSED -- %s" % exc)
        say("!" * 78)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
