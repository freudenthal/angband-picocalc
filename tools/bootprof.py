#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 140. Reduces a BOOTPROF serial capture (from an
# ANGBAND_BOOT_PROFILE build, src/platform/bootprof.c) to the tables the stage plan asks for.
#
# NOT PART OF THE RELEASE. A desk-side reduction tool for the boot-profiling work in the
# development workspace; it is not needed to build or play the game.
#
#   python angband-pico/tools/bootprof.py <capture> <build-tree>
#   python angband-pico/tools/bootprof.py --selftest
#
# <build-tree> is relative to angband-pico/ (e.g. build-pico2-prof), matching sram-check.py's
# own convention. It must hold a linked angband.elf and its .map (pico_add_extra_outputs()
# writes both next to each other).
#
# THE INPUT IS WHATEVER THE TERMINAL SAVED. Lines that are not "BOOTPROF " lines are
# ignored, so a capture with boot lines, PICO[...] heap lines and typed keys mixed in is
# fine -- specifications.md 13's rule, read records by prefix, not by field count.
#
# HOW ADDRESSES ARE RESOLVED, AND WHY TWO DIFFERENT TOOLS. Per the stage plan: symbol names,
# addresses and sizes come from `arm-none-eabi-nm -S -n` on the tree's angband.elf; the
# OWNING OBJECT FILE for a given address comes from the tree's own angband.elf.map, because
# nm has no notion of which .o or archive member a symbol came from. The map file's input
# sections keep their original name (".text.<function>", or ".time_critical.<function>" for
# anything __not_in_flash_func()-tagged) even where the SDK's SRAM-placement machinery has
# gathered them into the .data OUTPUT section -- so a function moved to SRAM by
# CMakeLists.txt's ANGBAND_SRAM_OBJECTS/ANGBAND_SRAM_LIBRARY_MEMBERS exclusion is still found
# under its original heading and resolves to the same object it always did.
#
# encoding='utf-8' everywhere: python on this machine defaults to cp1252 for file I/O and has
# silently corrupted a file in this project before (stage 020 correction).

import argparse
import bisect
import io
import os
import re
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
NM = "arm-none-eabi-nm"

# Must match BOOTPROF_BUCKET_SHIFT in src/platform/bootprof.c: every "BOOTPROF pc"/"BOOTPROF
# pair" address is a bucket's BASE address, standing for this many bytes starting there.
BOOTPROF_BUCKET_SIZE = 16

# ---------------------------------------------------------------------------------------
# Capture parsing.


def parse_capture(lines):
    """BOOTPROF lines to (phases, parsers, pc_counts, pairs, stats). Unknown or malformed
    lines are skipped rather than raising -- a serial capture can carry a half-written line
    from a dropped byte, and one bad line must not lose the rest of the reduction."""
    phases = []
    parsers = []
    pc_counts = defaultdict(int)
    pairs = []
    stats = {}

    for raw in lines:
        line = raw.strip()
        if not line.startswith("BOOTPROF "):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        kind = parts[1]
        try:
            if kind == "phase" and len(parts) >= 4:
                phases.append((parts[2], int(parts[3])))
            elif kind == "parser" and len(parts) >= 6:
                parsers.append((parts[2], int(parts[3]), int(parts[4]), int(parts[5])))
            elif kind == "pc" and len(parts) >= 4:
                pc_counts[int(parts[2], 16)] += int(parts[3])
            elif kind == "pair" and len(parts) >= 5:
                pairs.append((int(parts[2], 16), int(parts[3], 16), int(parts[4])))
            elif kind == "samples" and len(parts) >= 7:
                stats["samples"] = int(parts[2])
                stats["overflow"] = int(parts[4])
                stats["hz"] = int(parts[6])
            elif kind == "window" and len(parts) >= 3:
                stats["window"] = int(parts[2])
            elif kind == "parser_total" and len(parts) >= 3:
                stats["parser_total"] = int(parts[2])
            elif kind == "parser_remainder" and len(parts) >= 3:
                stats["parser_remainder"] = int(parts[2])
            # "gate", "gate armed", "gate_failed ...", "parser_dropped N", "end" and
            # "aggregate_failed ..." carry no table data; ignored on purpose.
        except ValueError:
            continue

    return phases, parsers, dict(pc_counts), pairs, stats


def read_capture(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return parse_capture(f)


# ---------------------------------------------------------------------------------------
# Address classification and symbol/object resolution.


def region_of(addr):
    if 0x10000000 <= addr < 0x11000000:
        return "flash"
    if 0x20000000 <= addr < 0x20080000:
        return "sram"
    if addr < 0x00010000:
        return "rom"
    return "other"


def load_symbols(elf):
    out = subprocess.run([NM, "-S", "-n", elf], capture_output=True, text=True,
                          check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4:
            addr_s, size_s, kind, name = parts
        elif len(parts) == 3:
            addr_s, kind, name = parts
            size_s = "0"
        else:
            continue
        if kind not in "tTwW":
            continue
        try:
            syms.append((int(addr_s, 16), int(size_s, 16), name))
        except ValueError:
            continue
    syms.sort()
    return syms


def find_symbol(syms, starts, addr):
    """A single point lookup -- used for LR resolution, where a real return address is a
    genuine point, not a bucket."""
    idx = bisect.bisect_right(starts, addr) - 1
    if idx < 0:
        return None
    start, size, name = syms[idx]
    if size == 0:
        return (name, start, size) if addr == start else None
    return (name, start, size) if start <= addr < start + size else None


def resolve_overlaps(items, starts, lo, hi):
    """How the byte range [lo, hi) -- a BOOTPROF_BUCKET_SIZE-byte device sample bucket --
    is covered by a sorted [(addr, size, name), ...] table. Returns [(name_or_None, bytes)]:
    None marks a span no item covers at all (a genuine gap, counted as unresolved).

    THIS IS THE FIX FOR A REAL DEFECT THE STAGE 140 SELF-TEST FOUND ON THE DEVICE, NOT A
    THEORETICAL ONE. bootprof.c's device-side buckets are BOOTPROF_BUCKET_SIZE bytes (16,
    matching BOOTPROF_BUCKET_SHIFT there) -- coarser than some of the tiny, tightly packed
    hardware_timer.c functions this port's SRAM-placement moves next to each other with no
    gap. A bucket that starts a few bytes before a small function's end and runs into the
    NEXT function's first bytes used to resolve as a single POINT (the bucket's base
    address), crediting the entire bucket to whichever function contained that one address
    -- even when most of the bucket's real bytes belonged to its neighbour. The self-test's
    planted busy loop calls time_us_64() constantly and showed up mostly credited to
    timer_hardware_alarm_claim(), a function it never calls, at up to 47 % of the sample
    total; masking every NVIC IRQ across the burn changed nothing (ruling out a real
    interrupt) and timer_hardware_alarm_claim()'s only actual caller,
    alarm_pool_irq_handler(), never appeared in the same capture. Splitting a bucket's count
    proportionally by how many of its bytes each overlapping item actually owns is the
    correct fix, in the tool that measures (specifications.md 13), not a change to the
    device-side bucket width, which is sized against item 4's dump-duration budget.
    """
    n = len(items)
    idx = bisect.bisect_right(starts, lo)
    # Step back while an earlier item's own range could still reach into [lo, hi).
    while idx > 0:
        pstart, psize, _ = items[idx - 1]
        pend = pstart + psize if psize else pstart + 1
        if pend > lo:
            idx -= 1
        else:
            break

    pieces = []
    cursor = lo
    i = idx
    while cursor < hi:
        if i < n and items[i][0] <= cursor:
            start, size, name = items[i]
            end = start + size if size else start + 1
            piece_end = min(hi, end)
            if piece_end > cursor:
                pieces.append((name, piece_end - cursor))
                cursor = piece_end
            i += 1
            continue
        nxt = items[i][0] if i < n else hi
        piece_end = min(hi, nxt)
        if piece_end > cursor:
            pieces.append((None, piece_end - cursor))
        cursor = piece_end
        if i >= n and piece_end >= hi:
            break
    return pieces


# A heading with no data on the same line ("  .text.add_light") is the split-per-function
# form (-ffunction-sections): the next line carries the address, size and object. A heading
# WITH data on the same line ("  .text    0xADDR   0xSIZE  libgcc.a(_arm_addsubdf3.o)") is a
# whole-object section from a library built without -ffunction-sections -- libgcc.a and
# libg.a members, seen directly in this project's own map file.
HEADING_RE = re.compile(
    r'^\s*\.(?:text|rodata|time_critical)(?:\.\S+)?'
    r'(?:\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*))?\s*$'
)
DATA_RE = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*)$')


def normalize_objfile(path):
    """A short, stable name: 'libgcc.a(_arm_addsubdf3.o)' or
    'CMakeFiles/angband_core.dir/src/game/cave.c.obj', never the toolchain's absolute path."""
    path = path.replace("\\", "/")
    m = re.search(r'([^/]+\.a)(\([^)]*\))?$', path)
    if m:
        return m.group(1) + (m.group(2) or "")
    idx = path.find("CMakeFiles/")
    return path[idx:] if idx >= 0 else path


def load_map(mapfile):
    """(entries, obj_totals). entries is a sorted [(addr, size, obj), ...] for address ->
    owning-object lookup; obj_totals sums EVERY .text/.rodata/.time_critical byte found for
    each object, which is "the unit the linker can move" the stage plan's item 5 asks for --
    not just the sampled functions' share of it."""
    entries = []
    obj_totals = defaultdict(int)
    pending = False

    with open(mapfile, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            hm = HEADING_RE.match(line)
            if hm:
                if hm.group(1):
                    addr, size = int(hm.group(1), 16), int(hm.group(2), 16)
                    obj = normalize_objfile(hm.group(3))
                    if size:
                        entries.append((addr, size, obj))
                        obj_totals[obj] += size
                    pending = False
                else:
                    pending = True
                continue
            if pending:
                dm = DATA_RE.match(line)
                if dm:
                    addr, size = int(dm.group(1), 16), int(dm.group(2), 16)
                    obj = normalize_objfile(dm.group(3))
                    if size:
                        entries.append((addr, size, obj))
                        obj_totals[obj] += size
                pending = False
                continue
            pending = False

    entries.sort()
    return entries, dict(obj_totals)


def find_object(entries, starts, addr):
    idx = bisect.bisect_right(starts, addr) - 1
    if idx < 0:
        return None
    start, size, obj = entries[idx]
    return obj if start <= addr < start + size else None


# ---------------------------------------------------------------------------------------
# The reduction. A pure function of its inputs so --selftest can drive it with a hand-built
# world and no device, no ELF, no subprocess.


def reduce_and_print(phases, parsers, pc_counts, pairs, stats, syms, starts, entries,
                      obj_starts, obj_totals, out=None):
    if out is None:
        out = sys.stdout

    total_samples = sum(pc_counts.values())

    print("== Phases ==", file=out)
    if phases:
        t0 = phases[0][1]
        span = max(1, phases[-1][1] - t0)
        for name, us in phases:
            ms = (us - t0) / 1000.0
            pct = 100.0 * (us - t0) / span
            print(f"  {name:24s} {ms:12.3f} ms  {pct:6.2f}% of main-to-prompt", file=out)
    else:
        print("  (none captured)", file=out)
    print(file=out)

    print("== Parsers ==", file=out)
    window = stats.get("window")
    for name, us, before, after in sorted(parsers, key=lambda p: -p[1]):
        pct = (100.0 * us / window) if window else float("nan")
        print(f"  {name:24s} {us / 1000.0:10.3f} ms  {pct:6.2f}%  "
              f"heap {before:6d} -> {after:6d} KB", file=out)
    if "parser_total" in stats:
        print(f"  {'(bracketed total)':24s} {stats['parser_total'] / 1000.0:10.3f} ms",
              file=out)
    if "parser_remainder" in stats:
        print(f"  {'(unbracketed remainder)':24s} "
              f"{stats['parser_remainder'] / 1000.0:10.3f} ms  -- not assumed to be zero",
              file=out)
    print(file=out)

    # Every "BOOTPROF pc"/"BOOTPROF pair" address is a bucket's BASE address, standing for
    # BOOTPROF_BUCKET_SIZE bytes -- not a point. resolve_overlaps() splits each bucket's
    # count proportionally across every symbol (or object) it actually overlaps, which is
    # what a bucket coarser than some of the functions it covers requires (see
    # resolve_overlaps()'s own comment for the device capture that proved a point lookup
    # wrong). func_size is looked up once per name, from the real symbol table, not from a
    # bucket -- a bucket's overlap length is not a function's size.
    sym_size = {}
    for s, sz, n in syms:
        sym_size.setdefault(n, sz)

    func_counts = defaultdict(float)
    func_region = {}
    obj_counts = defaultdict(float)
    obj_region = {}
    resolved = 0.0

    for addr, count in pc_counts.items():
        lo, hi = addr, addr + BOOTPROF_BUCKET_SIZE
        if syms:
            for name, span in resolve_overlaps(syms, starts, lo, hi):
                share = count * (span / BOOTPROF_BUCKET_SIZE)
                if name is not None:
                    func_counts[name] += share
                    func_region.setdefault(name, region_of(addr))
                    resolved += share
        if entries:
            for obj, span in resolve_overlaps(entries, obj_starts, lo, hi):
                if obj is not None:
                    obj_counts[obj] += count * (span / BOOTPROF_BUCKET_SIZE)
                    obj_region.setdefault(obj, region_of(addr))

    print("== Functions (top 40) ==", file=out)
    top_funcs = sorted(func_counts.items(), key=lambda kv: -kv[1])[:40]
    for name, count in top_funcs:
        pct = 100.0 * count / total_samples if total_samples else 0.0
        print(f"  {name:32s} {count:8.1f} {pct:6.2f}%  {func_region[name]:6s}  "
              f".text {sym_size.get(name, 0):6d} B", file=out)
    print(file=out)

    print("== Objects (>= 0.5% of samples) ==", file=out)
    threshold = 0.005 * total_samples
    for obj, count in sorted(obj_counts.items(), key=lambda kv: -kv[1]):
        if count < threshold:
            continue
        pct = 100.0 * count / total_samples if total_samples else 0.0
        print(f"  {obj:56s} {count:8.1f} {pct:6.2f}%  {obj_region[obj]:6s}  "
              f".text+.rodata {obj_totals.get(obj, 0):8d} B", file=out)
    print(file=out)

    print("== Callers (of the top 10 functions, from BOOTPROF pair lines) ==", file=out)
    for name, _ in sorted(func_counts.items(), key=lambda kv: -kv[1])[:10]:
        lo = hi = None
        for s, sz, n in syms:
            if n == name:
                lo, hi = s, s + (sz if sz else 1)
                break
        caller_counts = defaultdict(float)
        if lo is not None:
            for pc, lr, count in pairs:
                # Interval overlap, not point containment -- a pair's pc is also a bucket
                # base address, for the same reason the Functions table above is bucketed.
                pc_hi = pc + BOOTPROF_BUCKET_SIZE
                overlap = min(hi, pc_hi) - max(lo, pc)
                if overlap > 0:
                    caller_hit = find_symbol(syms, starts, lr)
                    caller_name = caller_hit[0] if caller_hit else f"0x{lr:08x}"
                    caller_counts[caller_name] += count * (overlap / BOOTPROF_BUCKET_SIZE)
        print(f"  {name}:", file=out)
        for caller, count in sorted(caller_counts.items(), key=lambda kv: -kv[1])[:5]:
            print(f"      <- {caller:32s} {count:8.1f}", file=out)
        if not caller_counts:
            print("      <- (no BOOTPROF pair line landed in this function)", file=out)
    print(file=out)

    coverage = (100.0 * resolved / total_samples) if total_samples else 100.0
    defect = coverage < 98.0
    print(f"Coverage: {resolved:.1f}/{total_samples} samples resolved to a symbol "
          f"= {coverage:.2f}%" + ("   *** DEFECT: under 98% ***" if defect else ""),
          file=out)
    if "overflow" in stats and stats["overflow"]:
        print(f"Overflow: {stats['overflow']} samples dropped (buffer full)", file=out)
    if "hz" in stats:
        print(f"Sample rate achieved: {stats['hz']} Hz", file=out)

    return coverage


# ---------------------------------------------------------------------------------------
# --selftest: a synthetic world, known answers, no device and no build tree.


def run_selftest():
    syms = [(0x10000000, 0x100, "flash_func"), (0x20000000, 0x80, "sram_func")]
    starts = [s[0] for s in syms]
    entries = [(0x10000000, 0x100, "core.c.obj"), (0x20000000, 0x80, "sram.c.obj")]
    obj_starts = [e[0] for e in entries]
    obj_totals = {"core.c.obj": 0x100, "sram.c.obj": 0x80}

    # 75 samples in flash_func, 24 in sram_func, 1 nobody can resolve -- coverage is 99 %
    # of 100 ON PURPOSE, so the test also proves the tool does not silently claim 100 %.
    pc_counts = {0x10000004: 75, 0x20000004: 24, 0xDEADBEE0: 1}
    pairs = [(0x10000004, 0x10000300, 75), (0x20000004, 0x10000310, 24)]
    phases = [("main", 1000), ("splash_key", 2000)]
    parsers = [("fake.txt", 500, 100, 120)]
    stats = {"window": 1000, "parser_total": 500, "parser_remainder": 500,
             "samples": 100, "overflow": 0, "hz": 1000}

    ok = True

    buf = io.StringIO()
    coverage = reduce_and_print(phases, parsers, pc_counts, pairs, stats, syms, starts,
                                 entries, obj_starts, obj_totals, out=buf)
    text = buf.getvalue()

    if abs(coverage - 99.0) > 0.01:
        print(f"bootprof --selftest: FAIL coverage {coverage:.2f}%, expected 99.00%")
        ok = False
    else:
        print(f"bootprof --selftest: coverage {coverage:.2f}% as expected")

    if "flash_func" not in text or "sram_func" not in text:
        print("bootprof --selftest: FAIL expected function names missing from the report")
        ok = False
    flash_line = next((ln for ln in text.splitlines() if "flash_func" in ln), "")
    if "75.0" not in flash_line.split():
        print(f"bootprof --selftest: FAIL flash_func's sample count is not 75.0: {flash_line!r}")
        ok = False

    # THE PLANTED WRONG ANSWER. A symbol table shifted 16 B late so neither sample address
    # (4 B into each function) falls inside either range any more: everything that should
    # resolve now does not, and coverage must collapse. specifications.md 13: "a check that
    # cannot see the thing changed is not a check of that change" -- this is the mutation
    # that proves this one can.
    broken_syms = [(a + 0x10, sz, n) for a, sz, n in syms]
    broken_starts = [s[0] for s in broken_syms]
    buf2 = io.StringIO()
    broken_coverage = reduce_and_print(phases, parsers, pc_counts, pairs, stats, broken_syms,
                                        broken_starts, entries, obj_starts, obj_totals,
                                        out=buf2)
    if broken_coverage >= 99.0:
        print(f"bootprof --selftest: FAIL the deliberately broken symbol table still scored "
              f"{broken_coverage:.2f}% -- this check cannot see the thing it is meant to "
              f"catch")
        ok = False
    else:
        print(f"bootprof --selftest: deliberately broken symbol table correctly scores "
              f"lower ({broken_coverage:.2f}% < 99.00%)")

    print("bootprof --selftest: " + ("PASS" if ok else "FAIL"))
    return ok


# ---------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Reduce a stage 140 BOOTPROF serial capture to phase/parser/function/"
                     "object/caller tables."
    )
    ap.add_argument("capture", nargs="?", help="a saved BOOTPROF serial capture")
    ap.add_argument("tree", nargs="?",
                     help="build tree holding angband.elf/.map, relative to angband-pico/")
    ap.add_argument("--selftest", action="store_true",
                     help="run the reduction on a synthetic capture with known answers, "
                          "no device")
    args = ap.parse_args(argv)

    if args.selftest:
        return 0 if run_selftest() else 1

    if not args.capture or not args.tree:
        ap.error("capture and build-tree are required unless --selftest is given")

    phases, parsers, pc_counts, pairs, stats = read_capture(args.capture)

    tree = os.path.join(PORT, args.tree)
    elf = os.path.join(tree, "angband.elf")
    mapfile = elf + ".map"
    if not os.path.isfile(elf):
        sys.exit(f"bootprof: no angband.elf in {tree}")
    if not os.path.isfile(mapfile):
        sys.exit(f"bootprof: no {mapfile}")

    syms = load_symbols(elf)
    starts = [s[0] for s in syms]
    entries, obj_totals = load_map(mapfile)
    obj_starts = [e[0] for e in entries]

    coverage = reduce_and_print(phases, parsers, pc_counts, pairs, stats, syms, starts,
                                 entries, obj_starts, obj_totals)
    return 1 if coverage < 98.0 else 0


if __name__ == "__main__":
    sys.exit(main())
