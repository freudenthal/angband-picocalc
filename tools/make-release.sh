#!/usr/bin/env bash
# Stage 130: package a release from the card staging.
#
#   tools/make-release.sh <tag>        (STAGE_DIR, PICOTOOL, LOADER_DIR optional)
#
# Refuses, with exit 1, when:
#   * the working tree is dirty (untracked files and submodules included);
#   * <tag> does not exist or does not name the current HEAD;
#   * tools/stage-card.sh exits non-zero (it is run first, so a build-pico2 that is not the
#     shipped recipe, a stale UF2, or a lib/ that differs from the checkout is refused here);
#   * the two UF2 Loader files in LOADER_DIR are missing or are not the pinned files.
#
# Writes release/ (and nothing else):
#   angband-picocalc-<tag>.zip                  pico2-apps/angband.uf2 and angband/lib/**,
#                                               read from the staging, never the build tree;
#                                               INSTALL.md, LICENSE.md, THIRD-PARTY.md and
#                                               copying.txt from the checkout
#   uf2loader-2.5-pimoroni_pico_plus2_w.zip     the two loader files and a README.txt
#   SHA256SUMS                                  for both zips; check with
#                                               (cd release && sha256sum -c SHA256SUMS)
#
# The zips are reproducible: sorted entries, every timestamp set to the tagged commit's
# date, fixed permissions. Two runs on one tag give the same sums.

set -u

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$PORT/.." && pwd)"
STAGE="${STAGE_DIR:-$ROOT/sdcard-pico2}"
LOADER="${LOADER_DIR:-$ROOT/uf2loader/output}"
OUT="$PORT/release"

# The loader asset INSTALL.md names: UF2 Loader 2.5 (commit 5c44a4b), unmodified, built with
# PICO_BOARD=pimoroni_pico_plus2_w_rp2350. Pinned so that a rebuilt loader is not shipped by
# accident.
LOADER_ZIP="uf2loader-2.5-pimoroni_pico_plus2_w.zip"
LOADER_SUMS="5c4c5437662c619d50755de88e0172dddda1b532f43294f04204d272a5e39e3a  BOOT2350.uf2
fe38d5884effce86e00ab66e7d148ef7f843847c81d1412b31814e21c4d958e3  bootloader_pimoroni_pico_plus2_w_rp2350.uf2"

refuse() { echo "make-release: REFUSED $*"; echo "make-release: exit 1"; exit 1; }
ok()     { echo "make-release: ok   $*"; }

TAG="${1:-}"
[ -n "$TAG" ] || { echo "usage: tools/make-release.sh <tag>"; exit 2; }

PY="$(command -v python || command -v python3 || command -v py)"
[ -n "$PY" ] || { echo "make-release: no python, python3 or py on PATH"; exit 2; }

cd "$PORT" || exit 2

# --- the tree and the tag -----------------------------------------------------------------
dirty="$(git status --porcelain --ignore-submodules=none)"
[ -z "$dirty" ] || refuse "the working tree is dirty: ${dirty//$'\n'/; }"
ok "working tree clean"

tagc="$(git rev-parse -q --verify "refs/tags/$TAG^{commit}")" || refuse "no tag $TAG"
head="$(git rev-parse HEAD)"
[ "$tagc" = "$head" ] || refuse "$TAG is ${tagc:0:7}, HEAD is ${head:0:7}"
ok "$TAG is HEAD ($(git describe --tags --exact-match HEAD))"

# --- the staging --------------------------------------------------------------------------
STAGE_DIR="$STAGE" "$PORT/tools/stage-card.sh" || refuse "stage-card.sh exited non-zero"

# --- the loader ---------------------------------------------------------------------------
[ -d "$LOADER" ] || refuse "no loader directory $LOADER (set LOADER_DIR)"
(cd "$LOADER" && echo "$LOADER_SUMS" | sha256sum -c --quiet -) \
	|| refuse "the loader files in $LOADER are not the pinned UF2 Loader 2.5 files"
ok "loader files match the pinned sums"

# --- package ------------------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT"
EPOCH="$(git log -1 --format=%ct HEAD)"
ZIP="angband-picocalc-$TAG.zip"

"$PY" - "$(cygpath -m "$STAGE" 2>/dev/null || echo "$STAGE")" \
	"$(cygpath -m "$PORT" 2>/dev/null || echo "$PORT")" \
	"$(cygpath -m "$LOADER" 2>/dev/null || echo "$LOADER")" \
	"$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")" \
	"$ZIP" "$LOADER_ZIP" "$EPOCH" <<'PYEOF'
import os, sys, time, zipfile
stage, port, loader, out, zipname, loadername, epoch = sys.argv[1:]
stamp = time.gmtime(int(epoch))[:6]

def add_file(z, arc, src):
    info = zipfile.ZipInfo(arc, stamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    with open(src, "rb") as f:
        z.writestr(info, f.read())

def add_dir(z, arc):
    info = zipfile.ZipInfo(arc.rstrip("/") + "/", stamp)
    info.external_attr = (0o40755 << 16) | 0x10
    z.writestr(info, b"")

entries = [("pico2-apps/angband.uf2", os.path.join(stage, "pico2-apps", "angband.uf2"))]
dirs = []
lib = os.path.join(stage, "angband", "lib")
for base, subdirs, files in os.walk(lib):
    rel = os.path.relpath(base, stage).replace(os.sep, "/")
    if not subdirs and not files:
        dirs.append(rel)
    for name in files:
        entries.append((rel + "/" + name, os.path.join(base, name)))
for name in ("INSTALL.md", "LICENSE.md", "THIRD-PARTY.md", "copying.txt"):
    entries.append((name, os.path.join(port, name)))

with zipfile.ZipFile(os.path.join(out, zipname), "w") as z:
    for arc, src in sorted(entries):
        add_file(z, arc, src)
    for arc in sorted(dirs):
        add_dir(z, arc)
print("make-release: ok   %s: %d files, %d empty directories" % (zipname, len(entries), len(dirs)))

readme = (
    "UF2 Loader 2.5 for the Pimoroni Pico Plus 2 W\n"
    "\n"
    "The UF2 Loader is by James Churchill and is licensed under GPL-3.0.\n"
    "Source: https://github.com/pelrun/uf2loader at commit 5c44a4b (tag 2.5).\n"
    "These two files were built from that commit with no change to the source,\n"
    "with PICO_BOARD=pimoroni_pico_plus2_w_rp2350.\n"
    "\n"
    "bootloader_pimoroni_pico_plus2_w_rp2350.uf2  copy to the RP2350 drive (BOOTSEL)\n"
    "BOOT2350.uf2                                 copy to the root of the SD card\n"
    "\n"
    "The installation steps are in INSTALL.md in the Angband for the PicoCalc release.\n"
)
with zipfile.ZipFile(os.path.join(out, loadername), "w") as z:
    for name in ("BOOT2350.uf2", "bootloader_pimoroni_pico_plus2_w_rp2350.uf2"):
        add_file(z, name, os.path.join(loader, name))
    info = zipfile.ZipInfo("README.txt", stamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    z.writestr(info, readme.replace("\n", "\r\n"))
print("make-release: ok   %s: 3 files" % loadername)
PYEOF
[ $? = 0 ] || refuse "packaging failed"

(cd "$OUT" && sha256sum "$ZIP" "$LOADER_ZIP" | sed 's/ \*/  /' > SHA256SUMS)
(cd "$OUT" && sha256sum -c --quiet SHA256SUMS) || refuse "SHA256SUMS does not check"

echo
for f in "$ZIP" "$LOADER_ZIP"; do
	printf '%10s B  %s\n' "$(stat -c %s "$OUT/$f")" "release/$f"
done
echo
cat "$OUT/SHA256SUMS"
echo "make-release: exit 0"
