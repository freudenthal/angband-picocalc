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

# ---------------------------------------------------------------------------------------
# ANGBAND_TURN_LOG (stage 070) -- the turn-latency instrument. OFF in the shipped build.
#
#   cmake -B build-pico2-log -S . ... -DANGBAND_TURN_LOG=ON
#
# With it ON, src/platform/turnlog.c prints one line of serial per player command giving
# the whole-turn time and each of the seven per-step sweeps of the level. With it OFF every
# TURNLOG_* macro in src/platform/turnlog.h is ((void)0), turnlog.c is an empty translation
# unit, and tools/compile-sweep.sh's text/data/bss line does not move -- which is the check
# that the instrument is not in the thing it measures.
#
# READ THIS BEFORE COMPARING A LOGGED NUMBER WITH A SHIPPED ONE. They are different
# binaries. Stage 060 moved a boot time by 17 % with a 2,617-byte .text edit that touched
# nothing on the measured path (specifications.md 12), so the instrument's own footprint is
# worth more than several of the wins stage 070 is chasing. The logged build is for
# RELATIVE comparisons within one binary; an absolute claim about the shipped build has to
# be taken on the shipped build.
#
# Build it into a SEPARATE tree, never by reconfiguring build-pico2: the placement
# experiment (stage 070 item 8) needs the shipped binary to stay byte-stable.
option(ANGBAND_TURN_LOG "Stage 070: per-turn latency log on serial" OFF)

# ANGBAND_TEXT_PAD (stage 070 item 8) -- the placement experiment.
#
#   cmake -B build-pico2-pad2k -S . ... -DANGBAND_TEXT_PAD=2048
#
# Bytes of dead space inserted immediately before the bulk of flash .text, so that four
# otherwise identical binaries sit at four different alignments. 0 is the default and the
# shipped build; the link is then byte-equivalent to one without the override.
#
# WHY THIS EXISTS. Stage 060 edited only layout -- no parser source, byte-identical
# gamedata -- and the boot came down 17 %, from 278,571 ms to 230,438 ms. The only thing
# that changed for the parser was its flash address. Until that swing is bounded, every
# "we got N % from X" in this stage is meaningless unless N is much larger than it.
#
# THE PERIOD IS 8 KB AND IS DERIVED, NOT GUESSED: hardware/xip_cache.h gives
# XIP_CACHE_SIZE 16 KB and XIP_CACHE_LINE_SIZE 8 B, and hardware/regs/xip.h's
# XIP_CTRL_SPLIT_WAYS describes setting it as partitioning the cache into "two half-sized
# direct-mapped regions" -- so at its reset value of 0 the cache is two-way set
# associative, 1,024 sets of 8 B per way, and the set index is address bits [12:3]. Four
# points across one period are 0, 2 KB, 4 KB and 6 KB.
set(ANGBAND_TEXT_PAD 0 CACHE STRING "Stage 070 item 8: bytes of .text pad before the game")

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
    # Stage 070, for one header: src/platform/turnlog.h. A core source cannot include an
    # SDK header (the stage 045 correction -- tools/compile-sweep.sh compiles src/game/
    # with no SDK include path), so the turn instrument's declarations live in a header
    # that needs nothing but <stdint.h> and its definitions live in turnlog.c.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
)

# PICOCALC is the platform macro: h-basic.h keeps UNIX undefined for it, which keeps pwd.h,
# the fcntl file locks, ~ expansion, /dev/urandom seeding and PRIVATE_USER_PATH out of the
# build, and defines HAVE_DIRENT_H / HAVE_STAT / HAVE_MKDIR instead.
#
# The three DEFAULT_*_PATH values are what config.h reads; all three point at the SD card
# mount, so savefiles land in /angband/lib/user/save/. Setting them here avoids editing
# config.h.
# USE_PRIVATE_PATHS (stage 050) is the one switch that decides where the game writes.
# init.c:388 is its only use: with it, the save, scores and panic directories hang off
# ANGBAND_DIR_USER, i.e. /angband/lib/user/; without it they hang off the data path, i.e.
# /angband/lib/save, /angband/lib/scores, /angband/lib/panic. specifications.md 6.3 and 8
# both describe the former, and the card staging in 8 creates exactly those four
# directories under user/, so this is what makes the code agree with the card. It has
# nothing to do with PRIVATE_USER_PATH, which is UNIX-only and stays undefined here.
target_compile_definitions(angband_core PUBLIC
    PICOCALC=1
    USE_PRIVATE_PATHS=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
)

if(ANGBAND_TURN_LOG)
    target_compile_definitions(angband_core PUBLIC ANGBAND_TURN_LOG=1)
endif()

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
    hardware_dma    # stage 045: lcd.c pushes pixels by DMA (lcd_write16_buf)
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

# hardware_dma is for the stage 045 raw transfer probe in termdiag.c, which drives a DMA
# channel at the PL022 directly to measure that path without editing lcd.c.
target_link_libraries(angband_termdiag PRIVATE angband_platform hardware_dma)

# specifications.md 6.5: serial at 115200 and the LCD, both.
pico_enable_stdio_usb(angband_termdiag 1)
pico_enable_stdio_uart(angband_termdiag 0)

pico_add_extra_outputs(angband_termdiag)

# ---------------------------------------------------------------------------------------
# angband (stage 050) -- the game.
#
# This is the first target that links angband_core, so it is the first one where the whole
# 150-unit core has to resolve. src/main.c is the entry point; src/host/main.c is upstream's
# UNIX front-end chooser and is never built here.
#
# main-pico.c and utf8.c are sources on the executable rather than members of
# angband_platform, for the reason angband_termdiag gives above: they include core headers,
# and angband_platform is also linked by two diagnostics that have no ui-term.c.
#
# turnlog.c (stage 070) is here and not in angband_platform for the same reason: it reads
# pico_term_last_fresh_us() out of main-pico.c. It is compiled unconditionally and is an
# empty translation unit unless ANGBAND_TURN_LOG is set, which keeps it in
# tools/port-warnings.sh's list either way.
add_executable(angband
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/main-pico.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/turnlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/utf8.c
    $<TARGET_OBJECTS:angband_core>
)

# angband_core's usage requirements taken by hand, not by linking the OBJECT library --
# that would add its objects a second time. src/main.c itself compiles at the project
# default, C11, like every other port-written source.
target_include_directories(angband PRIVATE
    ${PICO_VFS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
)
target_compile_definitions(angband PRIVATE
    PICOCALC=1
    USE_PRIVATE_PATHS=1
    DEFAULT_CONFIG_PATH="/angband/lib/"
    DEFAULT_LIB_PATH="/angband/lib/"
    DEFAULT_DATA_PATH="/angband/lib/"
    $<$<BOOL:${ANGBAND_TURN_LOG}>:ANGBAND_TURN_LOG=1>
)

# THE STACK. specifications.md 12 recorded the SDK's 2 KB core-0 stack in the fixed 4 KB
# SCRATCH_Y as a gap for this stage to close, and the game cannot live in it -- one frame
# of ui-output.c's text_out_to_screen() is 6 KB on its own.
#
# Two halves. The linker script overrides in src/link/ move the .stack_dummy section out of
# SCRATCH_Y and into RAM and recompute __StackTop/__StackBottom/__StackLimit to match;
# PICO_STACK_SIZE then sets how big it is, which in SCRATCH_Y it could not (4 KB is the
# whole region). crt0.S reads PICO_STACK_SIZE and pico_crt0 is an INTERFACE library that
# adds crt0.S as a source to this executable, so a PRIVATE definition here reaches it.
#
# 64 KB, not the 32 KB the stage plan proposed. Deep recursion in the dungeon generator and
# 1 KB path/line buffers nested several calls apart make the true figure unknown until it
# is measured, and on a first bring-up a stack overflow is indistinguishable from any other
# hard fault. src/main.c paints the stack at boot and reports the high-water mark on every
# level, which is the number a later stage should trim this to. 64 KB of 520 KB.
pico_add_linker_script_override_path(angband ${CMAKE_CURRENT_SOURCE_DIR}/src/link
    FILES sections_stack.incl section_end.incl)

# The third override (stage 070 item 8) is GENERATED, because it carries a number CMake
# substitutes. It lives in the build tree, so the two paths are given in order and the SDK
# searches both. See src/link/default_text_excludes.incl.in for what it does and why only
# one override is needed rather than the two the stage plan's table lists.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/link/default_text_excludes.incl.in
               ${CMAKE_CURRENT_BINARY_DIR}/link-gen/default_text_excludes.incl @ONLY)
pico_add_linker_script_override_path(angband ${CMAKE_CURRENT_BINARY_DIR}/link-gen
    FILES default_text_excludes.incl)
target_compile_definitions(angband PRIVATE PICO_STACK_SIZE=0x10000)

# pico_rand for the RNG seed: specifications.md 6.3 says time() here is boot time plus a
# constant, so z-rand.c's Rand_init() would roll the same dungeon on every boot.
target_link_libraries(angband PRIVATE angband_platform hardware_dma pico_rand)

# specifications.md 6.5. The game is not a diagnostic, but every boot line, the
# init_angband() timing and the per-level heap figures go to serial, and a quit() message
# goes to both.
# PORT: picocalc-device-harness stage 020. The case USB-C is the Pico's UART0 console
# through the CH340 and it survives every reset; USB stays on because picotool's reset
# interface rides on it.
pico_enable_stdio_usb(angband 1)
pico_enable_stdio_uart(angband 1)

pico_add_extra_outputs(angband)
