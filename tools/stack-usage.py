#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 160. Reduces a tree's -fstack-usage output
# (one .su file per compiled object, gcc's own per-function frame sizes) to the tables the
# stage plan's item 2 asks for.
#
# NOT PART OF THE RELEASE. A desk-side reduction tool for the stack-trim work in the
# development workspace; it is not needed to build or play the game.
#
#   python angband-pico/tools/stack-usage.py <build-tree> [--top N] [--functions f1,f2,...]
#
# <build-tree> is relative to angband-pico/ (e.g. build-pico2-stackusage), matching
# bootprof.py's and sram-check.py's own convention. It must have been built with
# -fstack-usage (CMAKE_C_FLAGS), which makes GCC write one <object>.su file next to every
# <object>.c.obj it compiles.
#
# THE .su FORMAT, PER FUNCTION, ONE LINE:
#   <file>:<line>:<col>:<function>\t<frame bytes>\t<qualifier>
# qualifier is "static" (the frame size is exact and fixed), "dynamic" (a VLA or alloca()
# makes it a run-time size -- the printed number is 0 or a lower bound, not the true frame),
# or "dynamic,bound" (dynamic, but gcc could still prove an upper bound). See the GCC manual,
# "-fstack-usage". THIS TOOL KNOWS NOTHING OF CALL CHAINS, RECURSION OR THE COMPILER'S OWN
# INLINING -- it is exactly what the stage plan's background section says about
# -fstack-usage: it tells the coverage session (item 8) which functions to be sure to visit,
# it does not compute a worst path.
#
# A function can appear in more than one .su file's total if a header defines a static
# inline that gets its own frame per translation unit; this tool reports every occurrence
# rather than collapsing them, because two different call sites can still nest to different
# depths.
#
# encoding='utf-8' everywhere: python on this machine defaults to cp1252 for file I/O and has
# silently corrupted a file in this project before (stage 020 correction).

import argparse
import os
import sys


def find_su_files(build_tree):
    out = []
    for root, _dirs, files in os.walk(build_tree):
        for name in files:
            if name.endswith(".su"):
                out.append(os.path.join(root, name))
    return out


def parse_su_file(path):
    """Yields (file, line, col, function, size, qualifier, su_path) tuples."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            raw = raw.rstrip("\n")
            if not raw:
                continue
            # The location field is itself "path:line:col", and on Windows a drive letter
            # adds one more colon ("C:\...") -- split from the right on the two fields that
            # are never part of a path (frame size, qualifier), then split the location
            # field's OWN last two colon-separated pieces off, however many colons the path
            # itself carries.
            parts = raw.split("\t")
            if len(parts) != 3:
                continue
            loc, size_s, qual = parts
            try:
                size = int(size_s)
            except ValueError:
                continue
            loc_parts = loc.rsplit(":", 3)
            if len(loc_parts) != 4:
                # No drive-letter colon: "path:line:col:function" splits into 3 pieces plus
                # the function is actually the col field's neighbour. Fall back to a 3-way
                # rsplit assuming no drive letter.
                loc_parts3 = loc.rsplit(":", 2)
                if len(loc_parts3) != 3:
                    continue
                file_, line_s, func = loc_parts3
                col_s = ""
            else:
                file_, line_s, col_s, func = loc_parts
            yield file_, line_s, col_s, func, size, qual, path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build_tree")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--functions", default="",
                     help="comma-separated function names; prints the largest static frame "
                          "found for each, matched by exact name or by name before the first "
                          "'.' (gcc clone suffixes such as foo.constprop.0)")
    args = ap.parse_args()

    if not os.path.isdir(args.build_tree):
        print("stack-usage: no such build tree: %s" % args.build_tree, file=sys.stderr)
        return 2

    su_files = find_su_files(args.build_tree)
    if not su_files:
        print("stack-usage: no .su files under %s -- was it configured with "
              "-DCMAKE_C_FLAGS=-fstack-usage?" % args.build_tree, file=sys.stderr)
        return 2

    entries = []
    for su in su_files:
        entries.extend(parse_su_file(su))

    print("stack-usage: %d .su files, %d function entries" % (len(su_files), len(entries)))
    print()

    statics = [e for e in entries if e[5] == "static"]
    dynamics = [e for e in entries if e[5] != "static"]

    statics.sort(key=lambda e: e[4], reverse=True)

    print("== top %d static frames ==" % args.top)
    for file_, line_s, _col_s, func, size, qual, su in statics[: args.top]:
        rel = os.path.relpath(file_, start=os.getcwd()) if os.path.isabs(file_) else file_
        print("%8d B  %s  (%s:%s)" % (size, func, rel, line_s))
    print()

    print("== every 'dynamic'/'dynamic,bound' entry (%d) ==" % len(dynamics))
    if not dynamics:
        print("(none)")
    else:
        dynamics.sort(key=lambda e: (e[0], e[3]))
        for file_, line_s, _col_s, func, size, qual, su in dynamics:
            rel = os.path.relpath(file_, start=os.getcwd()) if os.path.isabs(file_) else file_
            print("%8s  %-14s %s  (%s:%s)" % (size, qual, func, rel, line_s))
    print()

    if args.functions.strip():
        names = [n.strip() for n in args.functions.split(",") if n.strip()]
        print("== largest static frame for each named function (%d asked) ==" % len(names))
        by_name = {}
        for file_, line_s, _col_s, func, size, qual, su in statics:
            base = func.split(".", 1)[0]
            for key in (func, base):
                prev = by_name.get(key)
                if prev is None or size > prev[0]:
                    by_name[key] = (size, file_, line_s, qual)
        for name in names:
            hit = by_name.get(name)
            if hit is None:
                print("%8s  %s  (not found in any .su file)" % ("--", name))
            else:
                size, file_, line_s, qual = hit
                rel = os.path.relpath(file_, start=os.getcwd()) if os.path.isabs(file_) else file_
                print("%8d B  %s  (%s:%s)" % (size, name, rel, line_s))

    return 0


if __name__ == "__main__":
    sys.exit(main())
