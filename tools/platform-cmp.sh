#!/bin/sh
# angband-pico -- specifications.md 7.2 check, added by stage 020.
#
# The copied platform drivers must stay byte-identical to their source tree, or the
# difference must be recorded in PORT-NOTES.md of both trees. This compares them.
#
# Source tree order matches specifications.md 4 (platform layer source): prefer
# zangband-pico/ when the file exists there, else tinyrogue-pico/.
#
#   cd <workspace root> && angband-pico/tools/platform-cmp.sh

set -u

here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)

# A standalone clone (no zangband-pico or tinyrogue-pico sibling checked out beside it)
# has nothing to compare against. That is not a defect in the clone -- the platform files
# are shipped in src/platform/ either way -- so the check is skipped rather than failed,
# and the suite is not red by construction outside the development workspace.
if [ ! -d "$root/zangband-pico" ] && [ ! -d "$root/tinyrogue-pico" ]; then
    echo "platform-cmp: skipped: no sibling tree (zangband-pico or tinyrogue-pico) beside this checkout"
    exit 0
fi

files="lcd.c lcd.h font5x10.c font5x10.h southbridge.c southbridge.h keyboard.c keyboard.h sd_fs.c sd_fs.h syscalls.c syscalls.h"

checked=0
same=0
differ=0

for f in $files; do
    mine="$here/src/platform/$f"
    [ -f "$mine" ] || continue

    src=""
    for tree in zangband-pico tinyrogue-pico; do
        if [ -f "$root/$tree/src/platform/$f" ]; then
            src="$root/$tree/src/platform/$f"
            break
        fi
    done

    if [ -z "$src" ]; then
        # Port-written (psram_heap.c and anything else with no upstream copy).
        continue
    fi

    checked=$((checked + 1))
    if cmp -s "$mine" "$src"; then
        same=$((same + 1))
        echo "same   $f  <-  ${src#$root/}"
    else
        differ=$((differ + 1))
        echo "DIFFER $f  <-  ${src#$root/}"
    fi
done

echo "platform-cmp: $same of $checked identical, $differ differ"
[ "$differ" -eq 0 ]
