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

# C11, the SDK's own standard. Stage 020: this was 99, which is what the vendored core
# wants, but CMAKE_C_STANDARD applies to the SDK sources an INTERFACE library injects
# into every STATIC library too, and hardware/structs/*.h use C11 static_assert -- so
# angband_platform failed to build with ~200 "expected declaration specifiers before
# 'sizeof'" errors. gnu99 is now a property of angband_core alone; see below.
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE "MinSizeRel")

# The board. Never pico2 or pico2_w: those are the RP2350A with 4 MB flash.
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(angband_pico C CXX ASM)

pico_sdk_init()

# pico-vfs supplies <sys/dirent.h> plus opendir/readdir/closedir over the SD card. Without
# it z-file.c does not compile: newlib's own <dirent.h> is a #error stub. Stage 020 needed
# only the header; stage 030 builds the library too and mounts the card with it.
#
# PICOCALC_SD_BOOT_DIR is a cache variable so a different checkout can be pointed at without
# editing this file, exactly as mothpad/c/pico/CMakeLists.txt does it.
set(PICOCALC_SD_BOOT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../PicoCalc/Code/pico_multi_booter/sd_boot"
    CACHE PATH "Path to the ClockworkPi PicoCalc sd_boot sources, which carry pico-vfs")
set(PICO_VFS_DIR "${PICOCALC_SD_BOOT_DIR}/lib/pico-vfs")
if(NOT EXISTS "${PICO_VFS_DIR}/include/sys/dirent.h")
    message(FATAL_ERROR "pico-vfs headers not found at ${PICO_VFS_DIR}/include")
endif()

# Brings in the blockdevice_*, filesystem_* and blockdevice/filesystem INTERFACE targets.
# It must come after pico_sdk_init(); pico-vfs's own CMakeLists calls SDK functions.
# The binary directory is named explicitly because the source tree is outside this project.
add_subdirectory("${PICO_VFS_DIR}" pico-vfs)

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

# The core is gnu99 (specifications.md 10). It is set here rather than globally so the
# SDK's own sources keep C11; see the CMAKE_C_STANDARD note at the top.
set_target_properties(angband_core PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

# pico_stdlib_headers, NOT pico_stdlib. The SDK ships INTERFACE libraries that add SDK
# sources to every consumer, so linking pico_stdlib into an OBJECT library compiles
# stdio.c and friends into it and then again into every executable that also links
# pico_stdlib: ~40 "multiple definition" errors at link. tinyrogue-pico hit this in its
# stage 050. The core needs the SDK include paths and definitions, nothing more.
target_link_libraries(angband_core PUBLIC pico_stdlib_headers)

# ---------------------------------------------------------------------------------------
# The platform layer (stage 020). PSRAM heap, LCD, south bridge.
#
# lcd.c/.h, font5x10.c/.h and southbridge.c/.h are copied byte-identical from
# tinyrogue-pico/src/platform/ (readme.md principle 6, specifications.md 4: do not write a
# second LCD driver). They carry tinyrogue's own PORT: banners, which name Picoware
# 841d9c56 / picocalc-text-starter (MIT) as their upstream. specifications.md 7.2 requires
# they stay byte-identical; tools/platform-cmp.sh checks that.
#
# psram_heap.c, sd_fs.c and syscalls.c are port-written.
#
# syscalls.c holds strong _gettimeofday / _getpid / _kill over the SDK's __weak ones. It
# only wins if the executable references syscalls_init() by name -- the stage 020 _sbrk
# trap again. See src/platform/syscalls.h.
# keyboard.c arrived in stage 040 and is copied byte-identical from tinyrogue-pico too. It
# knows nothing about Angband: it reports (scan code, shift, ctrl, alt) and main-pico.c maps
# that onto ui-event.h's keycodes. main-pico.c and utf8.c are NOT here -- they include core
# headers, so they belong to the targets that link core code; see angband_termdiag below.
add_library(angband_platform STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/psram_heap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/sd_fs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/lcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/southbridge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/keyboard.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/font5x10.c
)

target_include_directories(angband_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src        # platform/...
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
)

# pico_stdlib here, not pico_stdlib_headers: this is a STATIC library, so the SDK sources
# an INTERFACE library injects are compiled once into it rather than into every consumer.
# That is the distinction angband_core's comment above draws.
#
# hardware_psram is PUBLIC and lives here, not only on the executables: psram_heap.c
# includes hardware/psram.h and calls psram_get_size().
target_link_libraries(angband_platform PUBLIC
    pico_stdlib
    hardware_spi    # the LCD, on spi1. The vendored driver's PIO transport was replaced;
                    # pioasm is a HOST tool and this machine has no host compiler.
    hardware_i2c    # the south bridge, on i2c1
    hardware_psram  # psram_get_size(), and the PSRAM linker region

    # Stage 030. blockdevice_sd and filesystem_fat are what sd_fs.c calls; filesystem_vfs
    # is the half that matters most, because it is the translation unit carrying _open,
    # _read, _write, _lseek, _stat, _fstat, rename, mkdir, rmdir, _unlink, opendir, readdir
    # and closedir. Without it those resolve to newlib's ENOSYS stubs and every z-file.c
    # call fails at run time with the build still perfectly green.
    blockdevice_sd
    filesystem_fat
    filesystem_vfs
)

# ---------------------------------------------------------------------------------------
# angband_psramdiag (stage 020) -- PSRAM bring-up. Links angband_platform ONLY, never
# angband_core: a failure here can only be the platform's or the SDK's fault.
add_executable(angband_psramdiag ${CMAKE_CURRENT_SOURCE_DIR}/src/psramdiag.c)

target_link_libraries(angband_psramdiag PRIVATE angband_platform hardware_psram)

# specifications.md 6.5: every diagnostic prints to USB serial at 115200 and to the LCD.
# Enabling stdio is not the same as producing diagnostics -- src/psramdiag.c does the
# printf calls.
pico_enable_stdio_usb(angband_psramdiag 1)
pico_enable_stdio_uart(angband_psramdiag 0)

pico_add_extra_outputs(angband_psramdiag)

# ---------------------------------------------------------------------------------------
# angband_fsdiag (stage 030) -- the SD card file layer.
#
# It does NOT link angband_core. The point is to prove the file layer through the very
# functions the game calls, so angband_zfile below compiles the five z-*.c units it needs
# straight from src/game/ and nothing else. A failure is then in pico-vfs, in sd_fs.c, or in
# z-file.c -- 5 translation units of game code, not 150.
#
# z-rand.c is in the list because z-file.c calls Rand_simple() from file_get_tempfile(); the
# stage plan named four units, and the linker named the fifth.
#
# THIS IS AN OBJECT LIBRARY FOR THE SAME REASON angband_core IS, and the reason is the stage
# 020 correction about CMAKE_C_STANDARD. These sources need gnu99. Setting C_STANDARD 99 on
# the *executable* also applies it to every SDK source that pico_stdlib and friends inject
# into that target, and hardware/structs/*.h use C11 static_assert -- the first build of this
# target died on accessctrl.h and sync.c, exactly as angband_platform did in stage 020. An
# OBJECT library that links pico_stdlib_headers only takes no injected sources, so gnu99 is
# safe on it.
add_library(angband_zfile OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-file.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-util.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-virt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-form.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-rand.c
)

# The same definitions angband_core uses. z-file.c behaves differently without PICOCALC: it
# would want the UNIX fcntl file locks and ~ expansion, and it would keep upstream's O_EXCL
# open() rather than the emulation the stage 030 PORT: edit put behind PICOCALC.
target_compile_definitions(angband_zfile PUBLIC
    PICOCALC=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
)
target_include_directories(angband_zfile PUBLIC
    ${PICO_VFS_DIR}/include                 # <sys/dirent.h>, ahead of newlib's stub
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
)
target_compile_options(angband_zfile PRIVATE -w -ffunction-sections -fdata-sections)
set_target_properties(angband_zfile PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_link_libraries(angband_zfile PUBLIC pico_stdlib_headers)

add_executable(angband_fsdiag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fsdiag.c
    $<TARGET_OBJECTS:angband_zfile>
)

# fsdiag.c itself compiles at the project default, C11. It needs angband_zfile's include
# paths and definitions but not its objects a second time, so the usage requirements are
# taken by hand rather than by linking the OBJECT library.
target_include_directories(angband_fsdiag PRIVATE
    ${PICO_VFS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
)
target_compile_definitions(angband_fsdiag PRIVATE
    PICOCALC=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
)

target_link_libraries(angband_fsdiag PRIVATE angband_platform)

# specifications.md 6.5: serial at 115200 and the LCD, both.
pico_enable_stdio_usb(angband_fsdiag 1)
pico_enable_stdio_uart(angband_fsdiag 0)

pico_add_extra_outputs(angband_fsdiag)

# ---------------------------------------------------------------------------------------
# angband_termdiag (stage 040) -- the terminal front end.
#
# Like angband_fsdiag it does NOT link angband_core. angband_zterm below is the six units
# ui-term.c needs to link, compiled straight from src/game/, so a failure is in main-pico.c,
# in utf8.c or in ui-term.c and not somewhere among 150 translation units.
#
# The list was found by linking and reading the errors, in this order: ui-term.c pulled in
# z-virt.c (mem_alloc), z-util.c (quit, plog), z-color.c (angband_color_table) and
# buildid.c (buildid, used by Term_xtra's window title path); z-virt.c pulled in z-form.c
# (vstrnfmt). That is the whole closure -- six units. specifications.md 6.4 records it.
#
# OBJECT library for the same reason angband_zfile is one: these sources need gnu99, and
# C_STANDARD 99 on an executable also applies to every SDK source pico_stdlib injects into
# it, which fails in the SDK's own headers (stage 020 and 030 corrections).
add_library(angband_zterm OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/ui-term.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-util.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-virt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-form.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/z-color.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/buildid.c
)

target_compile_definitions(angband_zterm PUBLIC
    PICOCALC=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
)
target_include_directories(angband_zterm PUBLIC
    ${PICO_VFS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
)
target_compile_options(angband_zterm PRIVATE -w -ffunction-sections -fdata-sections)
set_target_properties(angband_zterm PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_link_libraries(angband_zterm PUBLIC pico_stdlib_headers)

# main-pico.c and utf8.c are the port's own front end. They include core headers, so they
# cannot live in angband_platform (which knows nothing about the game and is linked by
# angband_psramdiag and angband_fsdiag, neither of which has ui-term.c). They compile at the
# project default C11, like termdiag.c itself.
add_executable(angband_termdiag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/termdiag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/main-pico.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/utf8.c
    $<TARGET_OBJECTS:angband_zterm>
)

target_include_directories(angband_termdiag PRIVATE
    ${PICO_VFS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
)
target_compile_definitions(angband_termdiag PRIVATE
    PICOCALC=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
)

target_link_libraries(angband_termdiag PRIVATE angband_platform)

# specifications.md 6.5: serial at 115200 and the LCD, both.
pico_enable_stdio_usb(angband_termdiag 1)
pico_enable_stdio_uart(angband_termdiag 0)

pico_add_extra_outputs(angband_termdiag)
