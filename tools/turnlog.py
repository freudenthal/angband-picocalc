#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 070. Reads a serial capture from the
# ANGBAND_TURN_LOG build and prints the table stage 070's item 2 asks for.
#
#   python angband-pico/tools/turnlog.py capture.txt
#   python angband-pico/tools/turnlog.py capture.txt --csv turns.csv
#
# The input is whatever the terminal saved. Lines that are not "TL " records are ignored,
# so a capture with boot lines, PICO[...] heap lines and typed keys mixed in is fine.
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
# It also prints, per depth:
#   * the leaf sum against the whole turn, which is the answer to "are the seven sweeps
#     most of a turn" -- the question item 2 says to stop and re-plan on if the answer is no
#   * the heap slope over the capture, which is the endurance criterion
#   * any gap in the record numbering, which is the only thing that can quietly invalidate
#     a capture
#
# encoding='utf-8' everywhere: python on this machine defaults to cp1252 for file I/O and
# has silently corrupted a file in this project before (stage 020 correction).

import io
import sys

# Must match the TLH header printed by src/platform/turnlog.c.
FIELDS = ["n", "turn", "d", "tot", "was", "lit", "upd", "fgn", "mkn", "scn", "trp",
          "mon", "wld", "gen", "frm", "idle", "brk"]

# The seven sweeps of stage 070's table. Disjoint, so they may be summed.
LEAVES = ["was", "lit", "upd", "fgn", "mkn", "scn", "trp"]

# Reported but NOT summed into the leaf total: wld contains fgn/mkn/scn/trp, and tot
# contains everything.
CONTAINERS = ["mon", "wld", "gen"]


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
    """Nearest-rank percentile. p is 50 for the median."""
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
    print("  -- outside the turn --")
    for k in ["frm", "idle"]:
        line(k, k)

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


def main(argv):
    if len(argv) < 2:
        print(__doc__ or "usage: turnlog.py CAPTURE [--csv OUT]")
        return 2

    path = argv[1]
    csv_out = None
    if "--csv" in argv:
        csv_out = argv[argv.index("--csv") + 1]

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

    by_depth = {}
    for r in records:
        by_depth.setdefault(r["d"], []).append(r)

    for depth in sorted(by_depth):
        report_depth(depth, by_depth[depth])

    if csv_out:
        with io.open(csv_out, "w", encoding="utf-8", newline="\n") as f:
            f.write(",".join(FIELDS) + "\n")
            for r in records:
                f.write(",".join(str(r[k]) for k in FIELDS) + "\n")
        print("")
        print("turnlog: wrote %s" % csv_out)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
