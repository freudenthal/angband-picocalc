// PORT: port-written for angband-pico, stage 030.
//
// The SD card file layer. One call brings up a pico-vfs FAT filesystem on the PicoCalc's
// microSD slot and mounts it at "/", after which newlib's fopen/fread/fwrite/fseek,
// open/read/write/lseek, rename, remove, mkdir, stat and opendir/readdir all reach the
// card -- which is everything src/game/z-file.c uses.
//
// specifications.md 6.3. The pin map is ../pico2w-port/specifications.md 6.3: SPI0, MISO
// 16, CS 17, SCK 18, MOSI 19. Card detect is GP22, active low.

#ifndef ANGBAND_PICO_SD_FS_H
#define ANGBAND_PICO_SD_FS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount the card at "/". Idempotent: a second call on an already-mounted card returns
// true without touching the hardware. Returns false and leaves a message in
// sd_fs_last_error() on any failure.
bool sd_fs_mount(void);

// True once sd_fs_mount() has succeeded.
bool sd_fs_is_mounted(void);

// GP22, active low. Reported for the log; sd_fs_mount() attempts the mount either way, so
// a board that does not wire the detect pin still works.
bool sd_fs_card_detected(void);

// What sd_fs_mount() asked blockdevice_sd_create() for. See SD_FS_REQUESTED_HZ in sd_fs.c.
uint32_t sd_fs_requested_hz(void);

// What the SPI block actually runs at, read back from the hardware after the mount.
// This is the number specifications.md 6.3 records, and it is NOT the requested one:
// spi_set_baudrate() can only divide clk_peri by an even prescale times a post-divide.
// Zero before a successful mount.
uint32_t sd_fs_effective_hz(void);

// clk_peri, the clock spi_set_baudrate() divides. 150 MHz on the RP2350, not the 125 MHz
// the Mothpad expression this was copied from assumes.
uint32_t sd_fs_peri_hz(void);

// A short description of the last failure, or "ok". Never NULL.
const char *sd_fs_last_error(void);

// The errno-style code fs_mount() returned, or 0.
int sd_fs_last_errno(void);

#ifdef __cplusplus
}
#endif

#endif // ANGBAND_PICO_SD_FS_H
