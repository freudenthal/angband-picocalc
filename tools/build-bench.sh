#!/bin/bash
# Stage 065 item 2: build the four bench binaries from THIS tree, all identical but for
# the .text pad. Invariant 10 -- nothing measured is older than the tree it came from.
#
#   bash angband-pico/tools/build-bench.sh
#
# Every configure passes -S (a bare `cmake -B dir -D...` does not reach the cache; cross-
# stage note 070). The recipe is the bench recipe and borg-bench.py refuses anything else:
# keys ON, SYNC ON, turn log ON, save-restore ON, mirror OFF.
set -e

cd /c/Users/greenblob/Documents/PicoCalc
source tools/pico-env.sh

SDK="$(cygpath -m "$PWD/micropython/lib/pico-sdk")"
PTOOL="$(cygpath -m "$PWD/picotool/picotool-2.3.0/picotool")"

for pad in 0 2048 4096 6144; do
    case "$pad" in
        0)    dir=build-pico2-bench ;;
        2048) dir=build-pico2-bench-pad2k ;;
        4096) dir=build-pico2-bench-pad4k ;;
        6144) dir=build-pico2-bench-pad6k ;;
    esac

    echo "=== configure $dir (ANGBAND_TEXT_PAD=$pad)"
    cmake -G Ninja -B "angband-pico/$dir" -S angband-pico \
        -DPICO_SDK_PATH="$SDK" \
        -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 -DPICO_PLATFORM=rp2350 \
        -Dpicotool_DIR="$PTOOL" \
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,--print-memory-usage" \
        -DANGBAND_SERIAL_KEYS=ON -DANGBAND_SYNC=ON -DANGBAND_TURN_LOG=ON \
        -DANGBAND_SAVE_RESTORE=ON -DANGBAND_TEXT_PAD="$pad" > "/tmp/cfg-$dir.log" 2>&1 \
        || { tail -30 "/tmp/cfg-$dir.log"; exit 1; }

    echo "=== build $dir"
    cmake --build "angband-pico/$dir" --target angband 2>&1 | tail -9
done

echo ""
echo "=== CMakeCache lines that prove the options took"
for dir in build-pico2-bench build-pico2-bench-pad2k build-pico2-bench-pad4k build-pico2-bench-pad6k; do
    echo "--- angband-pico/$dir/CMakeCache.txt"
    grep -E '^ANGBAND_[A-Z_]*:' "angband-pico/$dir/CMakeCache.txt"
done

echo ""
echo "=== sizes"
for dir in build-pico2-bench build-pico2-bench-pad2k build-pico2-bench-pad4k build-pico2-bench-pad6k; do
    arm-none-eabi-size "angband-pico/$dir/angband.elf" | tail -1
done
