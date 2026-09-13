#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 070. Reads a serial capture from the
# ANGBAND_TURN_LOG build and prints the table stage 070's item 2 asks for. Stage 065 added
# --compare and the sparse-column report.
#
#   python angband-pico/tools/turnlog.py capture.txt
#   python angband-pico/tools/turnlog.py capture.txt --csv turns.csv
#   python angband-pico/tools/turnlog.py --compare <tag-or-path> <tag-or-path>
#
# The input is whatever the terminal saved. Lines that are not "TL " records are ignored,
# so a capture with boot lines, PICO[...] heap lines, SYNC records and typed keys mixed in
# is fine.
#
# WHAT IT REPORTS, AND WHY IT IS MEDIANS
#
# Per depth (0 is the town) and per phase: the median and the 95th percentile, in
# milliseconds, with the count. Medians rather than means because the stage plan's own
# warning is that PICO_STDIO_USB_STDOUT_TIMEOUT_US is 500,000 -- a terminal that stops
# draining injects half-second spikes into the capture that belong to no phase. A median
# is immune to a handful of those; a mean is not. The 95th percentile is printed beside it
# so the spikes are still visible rather than hidden.
#
# THE MEDIAN IS NEAREST-RANK AND THAT IS NOT A DETAIL. With an even sample count a
# nearest-rank median returns one of the two middle samples where an averaging median
# (statistics.median) returns their mean, and on a 20-sample column the two answers differ
# by 2 %. Harness stage 058's run log quoted 8,138.85 ms for the depth-1 `gen` column from
# an averaging median; this tool reports 7,996.13 ms for the same 20 samples, and both are
# the same pair of numbers read two ways. Every figure this file prints is nearest-rank,
# including the ones --compare subtracts, so a comparison is never mixing conventions.
#
# It also prints, per depth:
#   * the leaf sum against the whole turn, which is the answer to "are the seven sweeps
#     most of a turn" -- the question item 2 says to stop and re-plan on if the answer is no
#   * the heap slope over the capture, which is the endurance criterion
#   * any gap in the record numbering, which is the only thing that can quietly invalidate
#     a capture
#
# THE SPARSE COLUMNS, AND THE DEFECT THAT MADE THEM NECESSARY (stage 065). `gen` is
# populated on ONE RECORD PER LEVEL ENTERED -- 39 of the 1,002 records in the 2026-09-13
# capture -- and `frm` only on the records where the panel was actually repainted, 357 of
# 1,002. A median over EVERY record therefore printed 0.00 for both, and the harness run
# log's first reading of that capture concluded the level generator cost nothing. It costs
# eight seconds. So these two columns are reported over their NON-ZERO records, with the
# sample count and the full range beside the median, and a reader cannot make that mistake
# again. A zero is not a measurement of a phase that did not run.
#
# encoding='utf-8' everywhere: python on this machine defaults to cp1252 for file I/O and
# has silently corrupted a file in this project before (stage 020 correction).

import argparse
import io
import json
import os
import sys

# Must match the TLH header printed by src/platform/turnlog.c.
FIELDS = ["n", "turn", "d", "tot", "was", "lit", "upd", "fgn", "mkn", "scn", "trp",
          "mon", "wld", "gen", "frm", "idle", "brk"]

# The seven sweeps of stage 070's table. Disjoint, so they may be summed.
LEAVES = ["was", "lit", "upd", "fgn", "mkn", "scn", "trp"]

# Reported but NOT summed into the leaf total: wld contains fgn/mkn/scn/trp, and tot
# contains everything.
CONTAINERS = ["mon", "wld"]

# Reported over their non-zero records only. See the header.
SPARSE = ["gen", "frm"]

# Where a round's capture and table live, and where the placement sweep writes the error
# bar. Stage 065 item 4. Relative to the workspace root, which is three levels up from
# this file (angband-pico/tools/turnlog.py).
WORKSPACE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BENCH_DIR = os.path.join(WORKSPACE, ".llm", "scratch", "bench")
ERROR_BAR_FILE = os.path.join(BENCH_DIR, "error-bar.json")

# The phases --compare puts in its table, in the order it prints them.
COMPARE_ROWS = ["tot"] + LEAVES + CONTAINERS


def read_records(path):
    records = []
    malformed = 0
    with io.open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("TL "):
                continue
            parts = line.split()[1:]
            if len(parts) != len(FIELDS):
                malformed += 1
                continue
            try:
                records.append(dict(zip(FIELDS, [int(p) for p in parts])))
            except ValueError:
                malformed += 1
    return records, malformed


def pct(values, p):
    """Nearest-rank percentile. p is 50 for the median. See the header on the convention."""
    if not values:
        return 0
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round(p / 100.0 * len(s) + 0.5)) - 1))
    return s[k]


def ms(us):
    return us / 1000.0


def gaps(records):
    """Breaks in the record numbering. An empty list is the acceptance criterion."""
    out = []
    for a, b in zip(records, records[1:]):
        if b["n"] != a["n"] + 1:
            out.append((a["n"], b["n"]))
    return out


def by_depth(records):
    out = {}
    for r in records:
        out.setdefault(r["d"], []).append(r)
    return out


# ------------------------------------------------------------------------------------
# The single-capture report.

def report_depth(depth, rows):
    print("")
    print("depth %d -- %d records, game turns %d..%d"
          % (depth, len(rows), rows[0]["turn"], rows[-1]["turn"]))
    print("  %-6s %10s %10s %8s" % ("phase", "median ms", "p95 ms", "% of tot"))

    tot_med = pct([r["tot"] for r in rows], 50)

    def line(name, key):
        v = [r[key] for r in rows]
        med = pct(v, 50)
        share = (100.0 * med / tot_med) if tot_med else 0.0
        print("  %-6s %10.2f %10.2f %7.1f%%" % (name, ms(med), ms(pct(v, 95)), share))

    line("tot", "tot")
    print("  -- the seven sweeps (disjoint) --")
    for k in LEAVES:
        line(k, k)

    leaf_sum_med = pct([sum(r[k] for k in LEAVES) for r in rows], 50)
    share = (100.0 * leaf_sum_med / tot_med) if tot_med else 0.0
    print("  %-6s %10.2f %10s %7.1f%%" % ("SWEEPS", ms(leaf_sum_med), "", share))

    print("  -- containers (overlap the above) --")
    for k in CONTAINERS:
        line(k, k)

    # The sparse columns. A median over every record is 0.00 for both of these and says
    # nothing; the count is part of the figure and is printed with it.
    print("  -- sparse: reported over NON-ZERO records only, count and range beside --")
    for k in SPARSE:
        v = [r[k] for r in rows if r[k] > 0]
        if not v:
            print("  %-6s %10s   %s" % (k, "--", "0 of %d records non-zero" % len(rows)))
            continue
        print("  %-6s %10.2f %10.2f   n=%d of %d   %.2f..%.2f ms"
              % (k, ms(pct(v, 50)), ms(pct(v, 95)), len(v), len(rows),
                 ms(min(v)), ms(max(v))))

    print("  -- outside the turn --")
    line("idle", "idle")

    brk = [r["brk"] for r in rows]
    print("  heap brk %d KB -> %d KB over %d records  (%+d KB)"
          % (brk[0], brk[-1], len(rows), brk[-1] - brk[0]))

    # The endurance criterion, phrased the way the stage plan phrases it.
    if len(rows) >= 200:
        early = pct([r["brk"] for r in rows[:100]], 50)
        late = pct([r["brk"] for r in rows[-100:]], 50)
        if early:
            print("  heap median first 100 = %d KB, last 100 = %d KB  (%+.1f %%)"
                  % (early, late, 100.0 * (late - early) / early))


def report(path, csv_out=None):
    records, malformed = read_records(path)

    print("turnlog: %s" % path)
    print("turnlog: %d records, %d malformed lines skipped" % (len(records), malformed))
    if not records:
        print("turnlog: nothing to report -- was the build configured -DANGBAND_TURN_LOG=ON?")
        return 1

    g = gaps(records)
    if g:
        print("turnlog: *** %d GAPS in the record numbering -- the capture is incomplete ***"
              % len(g))
        for a, b in g[:10]:
            print("turnlog:     %d -> %d  (%d lines lost)" % (a, b, b - a - 1))
    else:
        print("turnlog: record numbering unbroken, %d..%d"
              % (records[0]["n"], records[-1]["n"]))

    depths = by_depth(records)
    for depth in sorted(depths):
        report_depth(depth, depths[depth])

    if csv_out:
        with io.open(csv_out, "w", encoding="utf-8", newline="\n") as f:
            f.write(",".join(FIELDS) + "\n")
            for r in records:
                f.write(",".join(str(r[k]) for k in FIELDS) + "\n")
        print("")
        print("turnlog: wrote %s" % csv_out)

    return 0


# ------------------------------------------------------------------------------------
# --compare. Stage 065's deliverable, and the rule it enforces.

def resolve(arg):
    """A path, or a bench tag standing for .llm/scratch/bench/<tag>.txt."""
    if os.path.isfile(arg):
        return arg
    candidate = os.path.join(BENCH_DIR, arg + ".txt")
    if os.path.isfile(candidate):
        return candidate
    return arg


def load_error_bar(override):
    """The placement error bar, as a percentage, and where it came from.

    Returns (pct_or_None, provenance string). None is not a failure: it is the honest
    state of a project that has not run the placement sweep yet, and --compare says so on
    every row rather than quietly marking small deltas as wins.
    """
    if override is not None:
        return override, "--error-bar on the command line"
    try:
        with io.open(ERROR_BAR_FILE, encoding="utf-8") as f:
            data = json.load(f)
    except (IOError, OSError, ValueError):
        return None, None
    return (data.get("pct"),
            "%s, measured %s: %s"
            % (os.path.relpath(ERROR_BAR_FILE, WORKSPACE).replace("\\", "/"),
               data.get("measured", "date not recorded"),
               data.get("note", "")))


def compare_depth(depth, a_rows, b_rows, bar):
    print("")
    print("depth %d -- A %d records, B %d records"
          % (depth, len(a_rows), len(b_rows)))
    print("  %-6s %12s %12s %12s %9s   %s"
          % ("phase", "A median", "B median", "delta ms", "delta %", "verdict"))

    for key in COMPARE_ROWS:
        a = pct([r[key] for r in a_rows], 50)
        b = pct([r[key] for r in b_rows], 50)
        delta = a - b
        dpct = (100.0 * delta / b) if b else 0.0

        if bar is None:
            verdict = "no error bar recorded"
        elif abs(dpct) <= bar:
            verdict = "not distinguishable"
        elif delta < 0:
            verdict = "A FASTER"
        else:
            verdict = "A SLOWER"

        print("  %-6s %12.2f %12.2f %+12.2f %+8.1f%%   %s"
              % (key, ms(a), ms(b), ms(delta), dpct, verdict))

    a_sw = pct([sum(r[k] for k in LEAVES) for r in a_rows], 50)
    b_sw = pct([sum(r[k] for k in LEAVES) for r in b_rows], 50)
    d_sw = a_sw - b_sw
    p_sw = (100.0 * d_sw / b_sw) if b_sw else 0.0
    if bar is None:
        verdict = "no error bar recorded"
    elif abs(p_sw) <= bar:
        verdict = "not distinguishable"
    else:
        verdict = "A FASTER" if d_sw < 0 else "A SLOWER"
    print("  %-6s %12.2f %12.2f %+12.2f %+8.1f%%   %s"
          % ("SWEEPS", ms(a_sw), ms(b_sw), ms(d_sw), p_sw, verdict))


def compare(a_arg, b_arg, bar_override):
    a_path, b_path = resolve(a_arg), resolve(b_arg)
    a_rec, a_bad = read_records(a_path)
    b_rec, b_bad = read_records(b_path)

    print("turnlog: compare")
    print("  A  %-14s %s   (%d records, %d malformed)"
          % (a_arg, a_path, len(a_rec), a_bad))
    print("  B  %-14s %s   (%d records, %d malformed)"
          % (b_arg, b_path, len(b_rec), b_bad))

    if not a_rec or not b_rec:
        print("turnlog: one side has no records -- nothing to compare")
        return 1

    for name, rec in (("A", a_rec), ("B", b_rec)):
        g = gaps(rec)
        if g:
            print("turnlog: *** %s has %d GAPS in its record numbering. A capture with a "
                  "gap cannot be compared with one without. ***" % (name, len(g)))
            return 1

    bar, provenance = load_error_bar(bar_override)
    print("")
    if bar is None:
        print("  ERROR BAR: NOT RECORDED. Run the placement sweep (stage 065 item 4) --")
        print("  until it exists no delta below it can be told from where .text happened")
        print("  to land, and every verdict below says so.")
    else:
        print("  error bar: +/- %.1f %% of the dungeon median (%s)" % (bar, provenance))
        print("  A delta inside it is NOT A RESULT. Write it down as not distinguishable.")

    a_by, b_by = by_depth(a_rec), by_depth(b_rec)
    for depth in sorted(set(a_by) & set(b_by)):
        compare_depth(depth, a_by[depth], b_by[depth], bar)

    only = sorted(set(a_by) ^ set(b_by))
    if only:
        print("")
        print("  depths present on one side only and not compared: %s"
              % ", ".join(str(d) for d in only))

    return 0


def main(argv):
    p = argparse.ArgumentParser(
        description="Reduce an ANGBAND_TURN_LOG capture, or compare two of them.")
    p.add_argument("capture", nargs="?", help="the capture to reduce")
    p.add_argument("--csv", metavar="OUT", help="write every record out as CSV")
    p.add_argument("--compare", nargs=2, metavar=("A", "B"),
                   help="two captures or bench tags; A is the round, B the reference")
    p.add_argument("--error-bar", type=float, metavar="PCT",
                   help="override the placement error bar from the sweep")
    args = p.parse_args(argv[1:])

    if args.compare:
        return compare(args.compare[0], args.compare[1], args.error_bar)
    if not args.capture:
        p.print_help()
        return 2
    return report(resolve(args.capture), args.csv)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
