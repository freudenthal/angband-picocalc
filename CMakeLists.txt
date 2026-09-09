# angband-pico — Angband 4.2.6+ (upstream commit 00c9414cb) for the ClockworkPi PicoCalc
# with a Pimoroni Pico Plus 2 W (RP2350B) fitted.
#
# Targets:
#   angband_core       stage 010. Object library over the vendored game sources.
#   angband_platform    stage 020+. LCD, keyboard, PSRAM heap, SD filesystem.
#   angband_psramdiag   stage 020. PSRAM bring-up diagnostic.
#   angband_fsdiag      stage 030. SD card diagnostic.
#   angband_termdiag    stage 040. Terminal front end diagnostic.
#   angband             stage 050. The game.
#
# This project builds against pico-sdk 2.3.0 (micropython/lib/pico-sdk), not the workspace
# 2.2.0, because 2.3.0 carries hardware_psram and the PSRAM linker region. Configure with:
#
#   cd /c/Users/greenblob/Documents/PicoCalc && source tools/pico-env.sh
#   cmake -G Ninja -B angband-pico/build-pico2 -S angband-pico \
#     -DPICO_SDK_PATH="$(cygpath -m "$PWD/micropython/lib/pico-sdk")" \
#     -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 -DPICO_PLATFORM=rp2350 \
#     -Dpicotool_DIR="$(cygpath -m "$PWD/picotool/picotool-2.3.0/picotool")" \
#     -DCMAKE_EXE_LINKER_FLAGS="-Wl,--print-memory-usage"
#   cmake --build angband-pico/build-pico2 --target angband_core

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE "MinSizeRel")

# The board. Never pico2 or pico2_w: those are the RP2350A with 4 MB flash.
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(angband_pico C CXX ASM)

pico_sdk_init()

# pico-vfs supplies <sys/dirent.h> plus opendir/readdir/closedir over the SD card. Without
# it z-file.c does not compile: newlib's own <dirent.h> is a #error stub. The mount itself
# is stage 030; the core only needs the header now.
set(PICO_VFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../PicoCalc/Code/pico_multi_booter/sd_boot/lib/pico-vfs")
if(NOT EXISTS "${PICO_VFS_DIR}/include/sys/dirent.h")
    message(FATAL_ERROR "pico-vfs headers not found at ${PICO_VFS_DIR}/include")
endif()

# ---------------------------------------------------------------------------------------
# The core: the vendored game, Angband commit 00c9414cb filtered per PORT-NOTES.md.
# src/host/ is deliberately absent: upstream main.c and main-test.c are the WSL harness
# only. tools/compile-sweep.sh compiles exactly this set outside CMake and is part 1 of
# the project's regression suite.
file(GLOB ANGBAND_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.c")

add_library(angband_core OBJECT ${ANGBAND_GAME_SOURCES})

target_include_directories(angband_core PUBLIC
    ${PICO_VFS_DIR}/include                 # <sys/dirent.h>, ahead of newlib's stub
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
)

# PICOCALC is the platform macro: h-basic.h keeps UNIX undefined for it, which keeps pwd.h,
# the fcntl file locks, ~ expansion, /dev/urandom seeding and PRIVATE_USER_PATH out of the
# build, and defines HAVE_DIRENT_H / HAVE_STAT / HAVE_MKDIR instead.
#
# The three DEFAULT_*_PATH values are what config.h reads; all three point at the SD card
# mount, so savefiles land in /angband/lib/user/save/. Setting them here avoids editing
# config.h.
target_compile_definitions(angband_core PUBLIC
    PICOCALC=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
)

# -w here; tools/compile-sweep.sh is the build that runs with -Wall and counts warnings.
target_compile_options(angband_core PRIVATE -w -ffunction-sections -fdata-sections)

# pico_stdlib_headers, NOT pico_stdlib. The SDK ships INTERFACE libraries that add SDK
# sources to every consumer, so linking pico_stdlib into an OBJECT library compiles
# stdio.c and friends into it and then again into every executable that also links
# pico_stdlib: ~40 "multiple definition" errors at link. tinyrogue-pico hit this in its
# stage 050. The core needs the SDK include paths and definitions, nothing more.
target_link_libraries(angband_core PUBLIC pico_stdlib_headers)
