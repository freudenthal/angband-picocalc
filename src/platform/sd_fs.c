// PORT: port-written for angband-pico, stage 030.
//
// SD card mount, after mothpad/c/pico/mothpad_pico_main.c pico_init_sd() (lines 2108-2141),
// which is the version proven on this hardware. specifications.md 6.3.
//
// THE CLOCK ARGUMENT IS NOT WHAT IT LOOKS LIKE. Mothpad passes `125000000 / 2 / 4`, an
// expression written for the RP2040's 125 MHz clk_peri. This board runs clk_sys -- and so
// clk_peri -- at 150 MHz, and pico-vfs hands the number straight to spi_set_baudrate(),
// which can only produce clk_peri / (prescale * postdiv) with prescale even. So the
// requested 15,625,000 Hz is not achievable and the SPI block is programmed to the next
// value at or below it. The number that matters is the one read back from the hardware
// after the mount; sd_fs_effective_hz() is that, and the diag prints it.
//
// The literal is kept rather than recomputed from clk_peri, deliberately: it reproduces the
// clock Mothpad already ran this card and this wiring at. pico-vfs clamps anything above
// 25 MHz down to 25 MHz (blockdevice/sd.c _freq()) and returns -EINVAL while doing it, so
// raising it is not a free change.

#include "platform/sd_fs.h"

#include <errno.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/vfs.h"

// ../pico2w-port/specifications.md 6.3. SCK is not in that table; 18 is what Mothpad uses
// and what the ClockworkPi schematic wires.
#define SD_MOSI_PIN 19
#define SD_MISO_PIN 16
#define SD_SCLK_PIN 18
#define SD_CS_PIN 17
#define SD_DET_PIN 22

// See the header comment. 125000000 / 2 / 4, written out.
#define SD_FS_REQUESTED_HZ 15625000u

// CRC on read and write. Mothpad enables it; the card is the savefile's only home and a
// silent bad sector is worse here than a few percent of throughput.
#define SD_FS_ENABLE_CRC true

static bool s_mounted;
static uint32_t s_effective_hz;
static int s_errno;
static const char *s_error = "not attempted";

bool sd_fs_is_mounted(void) { return s_mounted; }
uint32_t sd_fs_requested_hz(void) { return SD_FS_REQUESTED_HZ; }
uint32_t sd_fs_effective_hz(void) { return s_effective_hz; }
uint32_t sd_fs_peri_hz(void) { return (uint32_t)clock_get_hz(clk_peri); }
const char *sd_fs_last_error(void) { return s_error; }
int sd_fs_last_errno(void) { return s_errno; }

bool sd_fs_card_detected(void)
{
    static bool inited;
    if (!inited)
    {
        gpio_init(SD_DET_PIN);
        gpio_set_dir(SD_DET_PIN, GPIO_IN);
        gpio_pull_up(SD_DET_PIN);
        inited = true;
        // The pull-up needs a moment to settle before the first read.
        sleep_us(50);
    }
    return !gpio_get(SD_DET_PIN); // active low
}

bool sd_fs_mount(void)
{
    if (s_mounted)
    {
        s_error = "ok";
        return true;
    }

    // Read the detect pin for the log, but do not gate on it. A board whose detect line is
    // not wired the way this assumes would otherwise be unmountable for no reason, and the
    // mount itself is the real test.
    (void)sd_fs_card_detected();

    blockdevice_t *sd = blockdevice_sd_create(spi0,
                                              SD_MOSI_PIN,
                                              SD_MISO_PIN,
                                              SD_SCLK_PIN,
                                              SD_CS_PIN,
                                              SD_FS_REQUESTED_HZ,
                                              SD_FS_ENABLE_CRC);
    if (!sd)
    {
        s_error = "blockdevice_sd_create failed (out of memory)";
        s_errno = ENOMEM;
        return false;
    }

    filesystem_t *fat = filesystem_fat_create();
    if (!fat)
    {
        blockdevice_sd_free(sd);
        s_error = "filesystem_fat_create failed (out of memory)";
        s_errno = ENOMEM;
        return false;
    }

    // fs_mount() initialises the block device on the way in, which is where the card is
    // actually talked to: CMD0/CMD8/ACMD41, the CSD read, and then _freq() programming the
    // transfer clock. A card that is absent, unformatted or not FAT fails here.
    int err = fs_mount("/", fat, sd);
    if (err != 0)
    {
        s_errno = errno;
        s_error = "fs_mount(\"/\") failed";
        filesystem_fat_free(fat);
        blockdevice_sd_free(sd);
        return false;
    }

    // Read the clock back rather than trusting the request. See the header comment.
    s_effective_hz = (uint32_t)spi_get_baudrate(spi0);

    s_mounted = true;
    s_errno = 0;
    s_error = "ok";
    return true;
}
