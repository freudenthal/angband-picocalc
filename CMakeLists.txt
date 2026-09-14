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

# PORT: picocalc-device-harness stage 030.
# ANGBAND_SERIAL_KEYS -- a byte on either stdio transport becomes a keypress. OFF in the
# shipped build.
#
#   cmake -B build-pico2-harness -S . ... -DANGBAND_SERIAL_KEYS=ON
#
# With it ON, check_events() in src/platform/main-pico.c polls getchar_timeout_us(0) beside
# keyboard_poll() and maps the byte per the harness specifications.md 8.3. With it OFF the
# whole block is inside #ifdef and the link is byte-identical in .text -- which is the check
# that the harness is not in the thing it measures (harness invariant 2).
#
# It is an option and not always-on because an input path into a game with a savefile should
# be asked for, and because with the case cable out the CH340 is unpowered and GP1 can
# deliver break bytes; a path that is off by default cannot be fed by noise.
option(ANGBAND_SERIAL_KEYS "Harness stage 030: keys from stdin in check_events()" OFF)

# PORT: picocalc-device-harness stage 045.
# ANGBAND_SERIAL_SCREEN -- the screen mirror. A 0x1C byte on either stdio transport asks
# for the term grid and gets back a framed 64x32 text block (harness specifications.md
# 8.5). OFF in the shipped build and OFF in every stage 070 measured binary.
#
#   cmake -B build-pico2-screen -S . ... -DANGBAND_SERIAL_KEYS=ON -DANGBAND_SERIAL_SCREEN=ON
#
# With it ON, push_serial_key() in src/platform/main-pico.c gains one branch for 0x1C that
# calls dump_screen() and returns false -- false, because a dump request is not a keypress
# and must not set `got` in check_events() or it would end a wait early. With it OFF the
# whole thing is inside #ifdef and the link is byte-identical in .text (harness invariant 2,
# checked on this edit).
#
# The dump is served from inside check_events()'s TURNLOG_MARK(tl_idle)/TURNLOG_IDLE(tl_idle)
# bracket, so its ~200 ms of wire at 115200 is subtracted from `tot`, the same property that
# made the stage 030 key path free. It is NOT emitted from Term_fresh(), which is inside the
# measured command.
option(ANGBAND_SERIAL_SCREEN "Harness stage 045: a term dump on 0x1C in check_events()" OFF)

# PORT: picocalc-device-harness stage 055.
# ANGBAND_SYNC -- the lockstep witness. One SYNC line on the console per completed player
# command, giving where the game is and, more to the point, where its random number
# generator is. OFF in the shipped build and OFF in every stage 070 measured binary.
#
#   cmake -B build-pico2-sync -S . ... -DANGBAND_SERIAL_KEYS=ON -DANGBAND_SYNC=ON
#
# WHAT IT IS FOR. The harness runs Angband's own borg on the PC, inside a host build of
# THIS core (tools/host-build.sh --borg), and sends the borg's keypresses to the device down
# the stage 030 serial key path. Two simulations given the same savefile and the same keys
# must reach the same state; the SYNC line is what proves they did. Without it, "the borg
# played the device for 1,000 turns" is a claim that a session which silently desynced at
# turn 40 would also make.
#
# WHERE IT IS EMITTED. src/game/ui-game.c calls ANGBAND_SYNC_END(turn) on the line after
# TURNLOG_END(), in play_game()'s loop, AFTER run_game_loop() has returned -- so the ~9 ms
# of wire at 115200 is outside every measured bracket, exactly as turnlog_end()'s own printf
# is. sync_end() itself is in src/platform/main-pico.c, inside #ifdef, and NOT in a source
# file of its own: one more object on this target moves the linker's long-branch veneers
# even when it contributes nothing, and invariant 2 asks for .text to be BYTE-identical and
# not merely the same size. The measurement that says so is in src/platform/sync-fmt.h.
#
# THE FORMATTER IS A HEADER, src/platform/sync-fmt.h, AND THE HOST BORG BUILD INCLUDES THE
# SAME ONE -- so the line the PC writes and the line the device prints come out of one piece
# of code and cannot drift apart. Only the last step differs: a printf here, a write to a
# file in src/host/main-borg.c.
option(ANGBAND_SYNC "Harness stage 055: one SYNC line per completed player command" OFF)

# ANGBAND_SAVE_RESTORE (stage 065 item 1) -- the bench's savefile restore. OFF in the
# shipped build, and the run log says so every time this option is quoted.
#
#   cmake -B build-pico2-bench -S . ... -DANGBAND_SAVE_RESTORE=ON
#
# With it ON, src/main.c copies /angband/lib/user/save/PicoCalc.master over
# /angband/lib/user/save/PicoCalc at boot, after init_files() and before play_game().
# With it OFF the whole thing is inside #ifdef and .text is byte-identical -- the same
# rule ANGBAND_SERIAL_KEYS and ANGBAND_SYNC are held to.
#
# WHY. Angband writes its savefile back while it plays (measured 2026-09-13: a 20-key probe
# moved the RNG state from si=19 rs=853891f9 to si=2 rs=e9a86912 with the character
# unchanged), so two lockstep runs from one card are two different games and the second
# diverges at command 1. Without the restore, every optimisation round needs a hand on the
# card. With it, the master is staged once, by hand, ever.
#
# IT IS DANGEROUS AND THAT IS WHY IT IS AN OPTION: on a card carrying a played game it
# destroys that game on the next boot. Nothing in sdcard-pico2/ is built with it.
option(ANGBAND_SAVE_RESTORE "Stage 065: copy PicoCalc.master over the savefile at boot" OFF)

# ANGBAND_CONSOLE (stage 090) -- the developer console. OFF in the shipped build.
#
#   cmake -B build-pico2-console -S . ... -DANGBAND_CONSOLE=ON
#
# With it ON the game has USB CDC and UART0 stdio, waits up to 10 s for a USB terminal,
# prints the boot lines on the panel and on serial, prints the PICO[...] memory reports,
# writes the `init ... heap ... stack ...` note on row 0 of the splash, and echoes plog()
# to serial. With it OFF none of that is built: no TinyUSB device stack, no UART driver, no
# wait, and the panel shows nothing before the splash. The panel half of the two failure
# paths (`SD card mount failed`, `*** Angband quit ***`) is NOT gated -- a player with no
# serial lead must still be told why the game did not start.
#
# Every harness and bench option prints or reads on the console, so each one needs it; the
# guard below refuses the combination rather than building a harness binary that is deaf.
# The three diagnostics are not affected: they keep USB stdio unconditionally.
#
# THE SHIPPED BUILD CANNOT BE RESET BY picotool OVER USB. picotool's reset interface rides
# on USB stdio, so a release build is flashed through BOOTSEL or the card's loader menu.
option(ANGBAND_CONSOLE "Developer console: USB+UART stdio, boot lines, memory reports, the 10 s USB wait" OFF)

foreach(needs_console ANGBAND_SERIAL_KEYS ANGBAND_SERIAL_SCREEN ANGBAND_SYNC ANGBAND_TURN_LOG
                      ANGBAND_SAVE_RESTORE)
    if(${needs_console} AND NOT ANGBAND_CONSOLE)
        message(FATAL_ERROR
            "${needs_console}=ON needs ANGBAND_CONSOLE=ON: every harness and bench option "
            "prints or reads on the developer console, and the shipped build (ANGBAND_CONSOLE "
            "OFF) has no stdio at all. Configure with -DANGBAND_CONSOLE=ON.")
    endif()
endforeach()

# The SYNC line is only worth emitting to something that is driving the game, and the only
# thing that can drive it over serial is the stage 030 key path. Without it the option would
# print a line no one asked for on every command of a hand-played session -- and, worse, a
# lockstep run would be impossible while the records piled up looking correct. Refused
# rather than ignored, the same rule ANGBAND_SERIAL_SCREEN is held to below.
if(ANGBAND_SYNC AND NOT ANGBAND_SERIAL_KEYS)
    message(FATAL_ERROR
        "ANGBAND_SYNC=ON needs ANGBAND_SERIAL_KEYS=ON: the SYNC line is the answer to keys "
        "sent down the stage 030 serial key path, and nothing else can send them. "
        "Configure with -DANGBAND_SERIAL_KEYS=ON.")
endif()

# ANGBAND_SERIAL_SCREEN_UNSAFE -- the way through the guard below. Not a feature.
option(ANGBAND_SERIAL_SCREEN_UNSAFE "Harness stage 045: allow the screen mirror in a logged build" OFF)

# The mirror rides on the stage 030 poll: the 0x1C request arrives through
# push_serial_key(), which only exists under ANGBAND_SERIAL_KEYS. Without it the option
# would compile to nothing and silently answer nothing, so it is refused rather than
# ignored.
if(ANGBAND_SERIAL_SCREEN AND NOT ANGBAND_SERIAL_KEYS)
    message(FATAL_ERROR
        "ANGBAND_SERIAL_SCREEN=ON needs ANGBAND_SERIAL_KEYS=ON: the 0x1C request is read by "
        "the getchar_timeout_us(0) poll that ANGBAND_SERIAL_KEYS puts in check_events(), and "
        "is dispatched by push_serial_key(). Configure with -DANGBAND_SERIAL_KEYS=ON.")
endif()

# The guard. The screen mirror and the turn log together make sense for debugging and are
# wrong for measuring, so the combination is not forbidden, it is loud.
if(ANGBAND_SERIAL_SCREEN AND ANGBAND_TURN_LOG AND NOT ANGBAND_SERIAL_SCREEN_UNSAFE)
    message(FATAL_ERROR
        "ANGBAND_SERIAL_SCREEN=ON with ANGBAND_TURN_LOG=ON: this build would log turns it "
        "is also instrumenting. A dump is served inside check_events()'s idle bracket, so "
        "its ~200 ms of wire at 115200 is subtracted from `tot` -- but NOT from wall clock, "
        "and not from `idle`, which stage 030 recorded cannot hold inter-command waiting. A "
        "session that polls the screen therefore produces a turn log whose per-phase figures "
        "are honest and whose elapsed time is not, and nothing in the capture says which "
        "records were dumped over. Measure with the mirror OFF. To debug a logged build "
        "anyway, and to accept that its wall clock is not a measurement, configure with "
        "-DANGBAND_SERIAL_SCREEN_UNSAFE=ON.")
endif()

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

# PORT: picocalc-device-harness stage 055. THE CHAR SIGNEDNESS PIN.
#
# ARM GCC defaults `char` to UNSIGNED; x86-64 GCC defaults it to SIGNED. Neither this file
# nor tools/host-build.sh used to say which, so a `char` compared against zero or
# sign-extended took a DIFFERENT VALUE on the device and on the host build of the same
# source. That is invisible until two builds of one game are asked to agree, which is
# exactly what harness stage 055's lockstep does: the PC runs the borg inside a host build
# of this core, sends its keys to the device, and compares an RNG checksum per turn. An
# unpinned `char` would show up there as a divergence that looks like a game bug.
#
# -fsigned-char, not -funsigned-char, because the host side is the one that cannot be
# changed by a flag on this file (WSL gcc, upstream tests, the borg) and because upstream
# Angband is developed and tested on signed-char platforms.
#
# add_compile_options() and not a per-target option: it has to reach angband_core,
# angband_platform, angband_zterm, the three diagnostics, the game, the SDK and pico-vfs.
# One translation unit left out is one that can disagree. It is placed BEFORE
# pico_sdk_init() and before add_subdirectory(pico-vfs) so it is inherited by everything
# those create.
#
# tools/compile-sweep.sh and tools/host-build.sh carry the same flag; all three are edited
# together or none is. The .text movement this caused on the shipped build is recorded in
# specifications.md 10.
add_compile_options(-fsigned-char)

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

# PORT: picocalc-device-harness stage 055. The ANGBAND_SYNC_END() call site is in
# src/game/ui-game.c, which is an angband_core source, so the define has to reach the object
# library and not only the executable. PUBLIC so that anything linking the core sees the same
# ui-game.c it was compiled from. The diagnostics do not link angband_core at all.
if(ANGBAND_SYNC)
    target_compile_definitions(angband_core PUBLIC ANGBAND_SYNC=1)
endif()

if(ANGBAND_TURN_LOG)
    target_compile_definitions(angband_core PUBLIC ANGBAND_TURN_LOG=1)
endif()

# -w here; tools/compile-sweep.sh is the build that runs with -Wall and counts warnings.
target_compile_options(angband_core PRIVATE -w -ffunction-sections -fdata-sections)

# STAGE 070 ITEM 6: the core's optimisation level. MinSizeRel's -Os is everywhere else;
# the core alone gets -O2 (it comes after -Os on the command line, and the last -O wins).
# Flash is not scarce -- 16 MB, under 1 MB used -- and SRAM has room for the larger hot
# objects ANGBAND_SRAM_OBJECTS moves. Placement-only and optimisation-only changes cannot
# move the host pre-check by construction; the bench round is what decides this one.
target_compile_options(angband_core PRIVATE -O2)

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
    # PORT: picocalc-device-harness stage 030. angband_termdiag also compiles main-pico.c
    # and deliberately does not get this define.
    $<$<BOOL:${ANGBAND_SERIAL_KEYS}>:ANGBAND_SERIAL_KEYS=1>
    # PORT: picocalc-device-harness stage 045. Same rule: angband_termdiag compiles
    # main-pico.c too and deliberately does not get this define either.
    $<$<BOOL:${ANGBAND_SERIAL_SCREEN}>:ANGBAND_SERIAL_SCREEN=1>
    # PORT: picocalc-device-harness stage 055. ANGBAND_SYNC has to reach BOTH sync.c and
    # angband_core, because the call site is in src/game/ui-game.c: a define on this target
    # alone would compile sync.c and never call it. angband_core gets it just below, the way
    # ANGBAND_TURN_LOG does, and for the same reason.
    $<$<BOOL:${ANGBAND_SYNC}>:ANGBAND_SYNC=1>
    # Stage 065 item 1. This one reaches THIS TARGET ONLY: the restore lives in src/main.c,
    # which nothing else compiles, and no src/game/ source knows it exists. That is the
    # point of putting it there -- platform-cmp.sh and the PORT: hunk invariant have
    # nothing to check, because no game source was edited.
    $<$<BOOL:${ANGBAND_SAVE_RESTORE}>:ANGBAND_SAVE_RESTORE=1>
    # Stage 090. This target only: src/main.c and src/platform/*.c read it; no src/game/
    # source does, and angband_termdiag compiles main-pico.c without it.
    $<$<BOOL:${ANGBAND_CONSOLE}>:ANGBAND_CONSOLE=1>
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
#
# STAGE 070 ITEM 3: THE HOT CORE OBJECTS LIVE IN SRAM. Every name in ANGBAND_SRAM_OBJECTS is
# excluded from flash .text AND flash .rodata by the two generated overrides, and the SDK's
# section_default_data.incl then gathers both into .data, which is `> RAM AT> RAM_STORE`.
# Why: a loop running from flash while reading the PSRAM heap pays 6,297-7,949 ns an access
# on the shared QMI (specifications.md 6.4), and the seven per-step sweeps of the level are
# exactly that loop. Code in SRAM also has no placement sensitivity, which is what makes the
# rest of stage 070 measurable. Each pattern carries the directory separator so that
# `cave.c` does not also match `gen-cave.c` or `cmd-cave.c`. CHECK WITH nm, NOT BY READING
# THIS LIST: a placement that silently fails looks exactly like one that worked (stage 045).
set(ANGBAND_SRAM_OBJECTS
    cave-view cave-square cave cave-map game-world z-bitflag z-queue z-type z-virt
    mon-move mon-util mon-predicate ui-map
    z-util z-rand z-form z-textblock player-calcs player-util player-timed
    ui-display ui-term ui-output ui-event ui-input ui-keymap ui-game game-event game-input
    cmd-core obj-gear obj-util obj-knowledge trap mon-list obj-list ui-obj-list ui-mon-list
    cmd-cave obj-pile obj-desc obj-ignore message mon-msg player-path ui-target ui-menu
    project project-mon project-feat project-player mon-attack mon-blows mon-spell
    player-attack)
# Not game objects, and named verbatim. THIS TOOLCHAIN'S NEWLIB IS libg.a, NOT libc.a, so
# the SDK's own `*libc.a:` exclusion never matched and malloc/free ran from flash while
# walking free lists in the PSRAM heap -- the stage 045 shape exactly. The allocator's
# members, the SDK's malloc wrapper, and the string functions the game calls per command.
set(ANGBAND_SRAM_LIBRARY_MEMBERS
    *libg.a:libc_a-mallocr.o *libg.a:libc_a-freer.o *libg.a:libc_a-reallocr.o
    *libg.a:libc_a-callocr.o *libg.a:libc_a-malloc.o *libg.a:libc_a-mlock.o
    *libg.a:libc_a-strcmp.o *libg.a:libc_a-strchr.o *libg.a:libc_a-strcpy.o
    *libg.a:libc_a-strlen-stub.o *libg.a:libc_a-strlen.o *libg.a:libc_a-strstr.o *libg.a:libc_a-ctype_.o
    */pico_malloc/malloc.c.obj)
# PORT: stage 075 item 1. The instrument itself, when it is built. Stage 075's brackets sit
# inside notice_stuff/update_stuff/redraw_stuff, which run many times a step from SRAM code
# between PSRAM reads; with turnlog.c and time_us_64() in flash each bracket paid the QMI
# alternation, and round s075-r0 read +9.7 % on the depth-1 turn. Bench builds only: with
# ANGBAND_TURN_LOG OFF the list, the image and the shipped RAM line are unchanged.
if(ANGBAND_TURN_LOG)
    list(APPEND ANGBAND_SRAM_LIBRARY_MEMBERS
        */src/platform/turnlog.c.obj */hardware_timer/timer.c.obj)
endif()
set(ANGBAND_SRAM_PATTERNS "")
foreach(obj IN LISTS ANGBAND_SRAM_OBJECTS)
    string(APPEND ANGBAND_SRAM_PATTERNS " */src/game/${obj}.c.obj")
endforeach()
foreach(member IN LISTS ANGBAND_SRAM_LIBRARY_MEMBERS)
    string(APPEND ANGBAND_SRAM_PATTERNS " ${member}")
endforeach()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/link/default_text_excludes.incl.in
               ${CMAKE_CURRENT_BINARY_DIR}/link-gen/default_text_excludes.incl @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/link/default_rodata_excludes.incl.in
               ${CMAKE_CURRENT_BINARY_DIR}/link-gen/default_rodata_excludes.incl @ONLY)
pico_add_linker_script_override_path(angband ${CMAKE_CURRENT_BINARY_DIR}/link-gen
    FILES default_text_excludes.incl default_rodata_excludes.incl)
target_compile_definitions(angband PRIVATE PICO_STACK_SIZE=0x10000)

# pico_rand for the RNG seed: specifications.md 6.3 says time() here is boot time plus a
# constant, so z-rand.c's Rand_init() would roll the same dungeon on every boot.
target_link_libraries(angband PRIVATE angband_platform hardware_dma pico_rand)

# specifications.md 6.5. The game is not a diagnostic. In a console build every boot line,
# the init_angband() timing and the per-level heap figures go to serial, and a quit()
# message goes to both.
# PORT: picocalc-device-harness stage 020. The case USB-C is the Pico's UART0 console
# through the CH340 and it survives every reset; USB is on because picotool's reset
# interface rides on it.
# Stage 090: both transports only with ANGBAND_CONSOLE. The shipped build has neither.
pico_enable_stdio_usb(angband ${ANGBAND_CONSOLE})
pico_enable_stdio_uart(angband ${ANGBAND_CONSOLE})

pico_add_extra_outputs(angband)
