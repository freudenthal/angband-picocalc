#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 070 item 3. PROVE A PLACEMENT, PER FILE.
#
# NOT PART OF THE RELEASE. A desk-side check for the SRAM-placement optimisation work in the
# development workspace; it is not needed to build or play the game.
#
#   "C:\Program Files\Python312\python.exe" angband-pico/tools/sram-check.py <build-tree>
#
# Reads ANGBAND_SRAM_OBJECTS out of CMakeLists.txt, lists every defined function and
# read-only object in each of those .c.obj files (arm-none-eabi-nm on the object), and looks
# every one of them up in the linked angband.elf. A symbol at 0x2000xxxx is SRAM; anything
# else is a failure. Stage 045's correction is why this exists: a placement that silently
# fails looks exactly like one that worked, and "nm is the check" only holds if nm is run on
# every symbol rather than a sample.
#
# Exit 0 if every text/rodata symbol of every listed object is in SRAM, 1 otherwise.
# Symbols the linker garbage-collected (--gc-sections) are reported as dropped, not failed.

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
NM = "arm-none-eabi-nm"


def sram_objects():
    with open(os.path.join(PORT, "CMakeLists.txt"), encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"set\(ANGBAND_SRAM_OBJECTS\s+([^)]*)\)", text)
    if not m:
        sys.exit("sram-check: no ANGBAND_SRAM_OBJECTS in CMakeLists.txt")
    return m.group(1).split()


def nm(path):
    out = subprocess.run([NM, path], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            yield parts[0], parts[1], parts[2]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__ or "usage: sram-check.py <build-tree>")
    tree = os.path.join(PORT, sys.argv[1])
    elf = {}
    for addr, kind, name in nm(os.path.join(tree, "angband.elf")):
        # Keyed by name AND binding: a global in one of our objects must not be confused
        # with a same-named static elsewhere (pico-vfs's fat.c has a static `format`).
        elf.setdefault((name, kind.isupper()), []).append(int(addr, 16))

    bad = 0
    for obj in sram_objects():
        path = os.path.join(tree, "CMakeFiles", "angband_core.dir", "src", "game",
                            obj + ".c.obj")
        syms = [(k, n) for _, k, n in nm(path) if k in "TtRr" and not n.startswith("$")]
        placed = dropped = 0
        lo, hi = None, None
        for kind, name in syms:
            addrs = elf.get((name, kind.isupper()))
            if not addrs:
                dropped += 1
                continue
            # A static name can exist in several objects; it is enough that one copy is in
            # SRAM only if every copy is -- so demand all of them for non-static symbols.
            ok = [a for a in addrs if 0x20000000 <= a < 0x20080000]
            if kind in "TR" and len(ok) != len(addrs):
                print(f"FLASH  {obj:12s} {name} at {', '.join(hex(a) for a in addrs)}")
                bad += 1
            elif not ok:
                print(f"FLASH  {obj:12s} {name} at {', '.join(hex(a) for a in addrs)}")
                bad += 1
            else:
                placed += 1
                lo = min(ok) if lo is None else min(lo, min(ok))
                hi = max(ok) if hi is None else max(hi, max(ok))
        span = f"{lo:#010x}..{hi:#010x}" if lo is not None else "-"
        print(f"sram-check: {obj:12s} {placed:4d} in SRAM  {dropped:3d} dropped  {span}")
    print(f"sram-check: {len(sram_objects())} objects, {bad} symbols outside SRAM")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
