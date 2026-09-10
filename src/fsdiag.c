// PORT: port-written for angband-pico, stage 030.
//
// angband_fsdiag -- the SD card file layer app.
//
// It links the platform layer plus exactly five units of game code: z-file.c, z-util.c,
// z-virt.c, z-form.c and z-rand.c (z-file.c calls Rand_simple from file_get_tempfile).
// Every filesystem call below goes through the z-file.c API, so what passes here is the
// same code path the game takes -- not a parallel re-implementation that might work when
// the real one does not.
//
// It also runs three raw-syscall probes first, in the "semantics" section, because two
// pico-vfs behaviours differ from POSIX in ways that would corrupt a savefile:
//
//   * rename() refuses an existing target (FatFs FR_EXIST), where POSIX replaces it;
//   * O_EXCL is ignored and a bare O_CREAT does not truncate.
//
// Both are worked around in z-file.c under PICOCALC (stage 030 PORT: edits). The raw probes
// record what pico-vfs actually does, and the z-file.c steps that follow prove the
// workarounds hold. If a future pico-vfs fixes either one, the probes say so and the
// workarounds can go.
//
// specifications.md 6.3, 6.5, 8.
//
// Everything drawn on the panel is also written to USB serial at 115200, in a buffer wider
// than the panel so the serial log -- the record the run logs quote -- is never clipped.
// The whole sequence waits up to 10 s for a USB terminal and re-runs on any PicoCalc key,
// both stage 020 lessons (specifications.md 6.5).
//
// STACK NOTE. The core-0 stack is the 2 KB the SDK puts in SCRATCH_Y. Every buffer here is
// static; nothing large is automatic.

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "platform/lcd.h"
#include "platform/font5x10.h"
#include "platform/southbridge.h"
#include "platform/psram_heap.h"
#include "platform/sd_fs.h"
#include "platform/syscalls.h"

#include "z-file.h"
#include "z-util.h"
#include "z-virt.h"

// ---------------------------------------------------------------------------------------
// Paths. specifications.md 8: the three DEFAULT_*_PATH roots are all /angband/lib/.

#define LIB_ROOT "/angband/lib"
#define GAMEDATA_DIR LIB_ROOT "/gamedata"
#define CONSTANTS_TXT GAMEDATA_DIR "/constants.txt"
#define MONSTER_TXT GAMEDATA_DIR "/monster.txt"
#define SAVE_DIR LIB_ROOT "/user/save"
#define PROBE_NEW SAVE_DIR "/probe.new"
#define PROBE_FINAL SAVE_DIR "/probe"
#define SEM_A SAVE_DIR "/sem_a.tmp"
#define SEM_B SAVE_DIR "/sem_b.tmp"

// The round trip. 70,000 bytes written 4 KB at a time, read back at offset 65,000.
#define PROBE_BYTES 70000u
#define PROBE_CHUNK 4096u
#define PROBE_SEEK 65000u
#define PROBE_VERIFY 1000u

// ---------------------------------------------------------------------------------------
// Output. Every line goes to USB serial in full and to the panel truncated to 64 columns.
// Copied in shape from psramdiag.c; see specifications.md 6.5 for why the two buffers
// differ in size.

#define DIAG_COLS 64
#define DIAG_ROWS 32
#define DIAG_LOG_ROWS (DIAG_ROWS - 2)
#define DIAG_HINT_ROW (DIAG_ROWS - 2)
#define DIAG_STATUS_ROW (DIAG_ROWS - 1)

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const uint16_t COL_BG = RGB565(20, 12, 28);
static const uint16_t COL_FG = RGB565(222, 238, 214);
static const uint16_t COL_OK = RGB565(109, 170, 44);
static const uint16_t COL_BAD = RGB565(208, 70, 72);
static const uint16_t COL_NOTE = RGB565(218, 212, 94);

static char diag_line[160];
static int diag_row;

// How many steps reported pass and how many fail, so the last line is a verdict rather
// than something the reader has to reconstruct by scrolling.
static unsigned n_pass;
static unsigned n_fail;

static void diag_draw(int row, const char *s, uint16_t fg)
{
    lcd_set_foreground(fg);
    lcd_set_background(COL_BG);

    int x = 0;
    for (; x < DIAG_COLS && s[x]; x++)
        lcd_putc((uint8_t)x, (uint8_t)row, (uint8_t)s[x]);
    for (; x < DIAG_COLS; x++)
        lcd_putc((uint8_t)x, (uint8_t)row, (uint8_t)' ');
}

static void diag_printf_colour(uint16_t fg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag_line, sizeof(diag_line), fmt, ap);
    va_end(ap);

    printf("%s\n", diag_line);

    if (diag_row >= DIAG_LOG_ROWS)
    {
        diag_row = 0;
        for (int r = 0; r < DIAG_LOG_ROWS; r++)
            diag_draw(r, "", COL_FG);
    }
    diag_draw(diag_row++, diag_line, fg);
}

#define diag_printf(...) diag_printf_colour(COL_FG, __VA_ARGS__)

// One graded step. `ok` decides the word and the colour, and keeps the tally.
static void diag_step(bool ok, const char *fmt, ...)
{
    static char body[140];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    if (ok)
        n_pass++;
    else
        n_fail++;

    diag_printf_colour(ok ? COL_OK : COL_BAD, "%s  %s", ok ? "pass" : "FAIL", body);
}

// A line that records something rather than grading it -- a measurement, or a pico-vfs
// behaviour that is a fact about the library and not a fault.
static void diag_note(const char *fmt, ...)
{
    static char body[140];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    diag_printf_colour(COL_NOTE, "note  %s", body);
}

// ---------------------------------------------------------------------------------------
// z-util.c wants these two hooks. Without them quit() calls exit(), which on a board means
// a silent halt with nothing on the panel -- the worst possible failure mode for a diag.

static void diag_plog_hook(const char *str)
{
    diag_printf_colour(COL_BAD, "plog  %s", str ? str : "(null)");
}

static void diag_quit_hook(const char *str)
{
    diag_printf_colour(COL_BAD, "quit  %s", str ? str : "(null)");
    diag_draw(DIAG_HINT_ROW, "core called quit() -- halted, press RESET", COL_BAD);
    for (;;)
        sleep_ms(1000);
}

// ---------------------------------------------------------------------------------------
// The 70,000-byte pattern. Deterministic in the offset alone, so any byte can be checked
// without holding the file -- which matters, because 70,000 bytes will not fit in SRAM
// alongside anything else and the point of the seek test is to check one slice.

static uint8_t probe_byte(uint32_t off)
{
    return (uint8_t)((off * 31u) ^ (off >> 7) ^ 0x5Au);
}

static uint8_t probe_buf[PROBE_CHUNK];

// ---------------------------------------------------------------------------------------
// Step 1. Mount.

static bool step_mount(void)
{
    bool detected = sd_fs_card_detected();
    diag_note("card detect GP22 = %s", detected ? "card present" : "no card (mounting anyway)");

    uint64_t t0 = time_us_64();
    bool ok = sd_fs_mount();
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000ull);

    diag_step(ok, "sd_fs_mount() %s in %lu ms  (%s, errno %d)",
              ok ? "mounted /" : "FAILED", (unsigned long)ms,
              sd_fs_last_error(), sd_fs_last_errno());

    if (!ok)
        return false;

    // The number specifications.md 6.3 records. Requested and effective differ because
    // spi_set_baudrate() can only divide clk_peri by an even prescale times a post-divide;
    // see the header comment in platform/sd_fs.c.
    diag_note("sd spi  requested %lu Hz  effective %lu Hz  clk_peri %lu Hz",
              (unsigned long)sd_fs_requested_hz(),
              (unsigned long)sd_fs_effective_hz(),
              (unsigned long)sd_fs_peri_hz());

    return true;
}

// ---------------------------------------------------------------------------------------
// Step 2. opendir / readdir over lib/gamedata, through my_dopen and my_dread.
//
// my_dread stat()s every entry to filter directories out, so this exercises stat() on both
// files and directories as a side effect -- one of the two things stage 030's plan listed
// as unverified.

static void step_readdir(void)
{
    static char name[256];
    static char firstfive[140];

    ang_dir *d = my_dopen(GAMEDATA_DIR);
    if (!d)
    {
        diag_step(false, "my_dopen(\"%s\") returned NULL (errno %d)", GAMEDATA_DIR, errno);
        return;
    }

    unsigned count = 0;
    firstfive[0] = '\0';
    uint64_t t0 = time_us_64();

    while (my_dread(d, name, sizeof(name)))
    {
        if (count < 5)
        {
            if (count)
                strncat(firstfive, " ", sizeof(firstfive) - strlen(firstfive) - 1);
            strncat(firstfive, name, sizeof(firstfive) - strlen(firstfive) - 1);
        }
        count++;
    }

    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000ull);
    my_dclose(d);

    // Non-zero is the machine-checkable part. The exact number is checked by the operator
    // against `ls angband-pico/lib/gamedata | wc -l`, which is why it is printed alone.
    diag_step(count > 0, "my_dread listed %u entries in %s  (%lu ms)",
              count, GAMEDATA_DIR, (unsigned long)ms);
    diag_note("first five: %s", firstfive);
}

// ---------------------------------------------------------------------------------------
// Step 3. file_open MODE_READ + file_getl over a gamedata text file.

static void step_read_text(void)
{
    static char line[128];

    ang_file *f = file_open(CONSTANTS_TXT, MODE_READ, FTYPE_TEXT);
    if (!f)
    {
        diag_step(false, "file_open(\"%s\", MODE_READ) failed (errno %d)", CONSTANTS_TXT, errno);
        return;
    }

    unsigned got = 0;
    for (; got < 10; got++)
    {
        if (!file_getl(f, line, sizeof(line)))
            break;
        // Indented, so the ten lines are obviously file content and not diag output.
        diag_printf("  %02u| %s", got + 1, line);
    }

    file_close(f);
    diag_step(got == 10, "file_getl read %u of the first 10 lines of constants.txt", got);
}

// Optional fix from the stage plan: monster.txt is 309 KB and every boot parses it, so its
// file_getl rate is the number stage 070 will want first.
static void step_getl_throughput(void)
{
    static char line[512];

    if (!file_exists(MONSTER_TXT))
    {
        diag_note("monster.txt absent -- file_getl throughput not measured");
        return;
    }

    ang_file *f = file_open(MONSTER_TXT, MODE_READ, FTYPE_TEXT);
    if (!f)
    {
        diag_step(false, "file_open(\"%s\", MODE_READ) failed (errno %d)", MONSTER_TXT, errno);
        return;
    }

    unsigned long lines = 0;
    unsigned long bytes = 0;
    uint64_t t0 = time_us_64();
    while (file_getl(f, line, sizeof(line)))
    {
        lines++;
        bytes += (unsigned long)strlen(line) + 1u; // +1 for the newline file_getl ate
    }
    uint64_t us = time_us_64() - t0;
    file_close(f);

    unsigned long kbs = us ? (unsigned long)((uint64_t)bytes * 1000ull / us) : 0ul;
    diag_step(lines > 0, "file_getl monster.txt: %lu lines, %lu B, %lu ms, %lu KB/s",
              lines, bytes, (unsigned long)(us / 1000ull), kbs);
}

// ---------------------------------------------------------------------------------------
// Step 4. dir_create, twice. The second call must also succeed: dir_create() returns true
// early when the directory already exists, and the game calls it on every start.

static void step_dir_create(void)
{
    bool first = dir_create(SAVE_DIR);
    bool second = dir_create(SAVE_DIR);
    bool exists = dir_exists(SAVE_DIR);

    diag_step(first, "dir_create(\"%s\") = %s", SAVE_DIR, first ? "true" : "false");
    diag_step(second, "dir_create() again on the existing directory = %s",
              second ? "true" : "false");
    diag_step(exists, "dir_exists() -> %s  (stat() on a directory)",
              exists ? "true" : "false");
}

// ---------------------------------------------------------------------------------------
// The semantics probes. Raw syscalls, so what is recorded is pico-vfs's own behaviour and
// not z-file.c's view of it. See the header comment.

static void probe_raw_semantics(void)
{
    // Start from a known state.
    (void)remove(SEM_A);
    (void)remove(SEM_B);

    // A. O_EXCL. POSIX: the second open fails with EEXIST.
    int fd = open(SEM_A, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        diag_step(false, "raw open(O_CREAT|O_EXCL) on a fresh name failed (errno %d)", errno);
        return;
    }
    (void)write(fd, "0123456789", 10);
    close(fd);

    errno = 0;
    int fd2 = open(SEM_A, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    bool excl_honoured = (fd2 < 0);
    if (fd2 >= 0)
        close(fd2);
    diag_note("pico-vfs O_EXCL over an existing name: %s",
              excl_honoured ? "honoured (fails, POSIX)"
                            : "IGNORED (succeeds) -- z-file.c emulates it under PICOCALC");

    // B. O_CREAT without O_TRUNC. POSIX leaves the old length; FatFs FA_OPEN_ALWAYS does
    // too, which is the half that would leave a stale tail on a short savefile.
    fd = open(SEM_A, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd >= 0)
    {
        (void)write(fd, "xy", 2);
        close(fd);
    }
    struct stat st;
    long len_no_trunc = (stat(SEM_A, &st) == 0) ? (long)st.st_size : -1L;

    fd = open(SEM_A, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd >= 0)
    {
        (void)write(fd, "xy", 2);
        close(fd);
    }
    long len_trunc = (stat(SEM_A, &st) == 0) ? (long)st.st_size : -1L;

    diag_note("pico-vfs O_CREAT no-TRUNC leaves %ld B, with O_TRUNC leaves %ld B (want 2)",
              len_no_trunc, len_trunc);

    // C. rename over an existing target. POSIX replaces; FatFs returns FR_EXIST.
    fd = open(SEM_B, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd >= 0)
    {
        (void)write(fd, "target", 6);
        close(fd);
    }
    errno = 0;
    int rr = rename(SEM_A, SEM_B);
    diag_note("pico-vfs rename() onto an existing name: %s (rc %d, errno %d)",
              rr == 0 ? "replaces it (POSIX)"
                      : "REFUSES -- z-file.c file_move removes the target first under PICOCALC",
              rr, errno);

    (void)remove(SEM_A);
    (void)remove(SEM_B);
}

// ---------------------------------------------------------------------------------------
// Step 5. The 70,000-byte round trip, through file_open / file_write / file_skip /
// file_read, exactly as savefile.c writes a save.

static bool step_roundtrip(void)
{
    // savefile.c always writes to a name that does not exist yet, because the previous
    // save renamed it away. Match that.
    if (file_exists(PROBE_NEW))
        (void)file_delete(PROBE_NEW);

    ang_file *f = file_open(PROBE_NEW, MODE_WRITE, FTYPE_SAVE);
    if (!f)
    {
        diag_step(false, "file_open(\"%s\", MODE_WRITE, FTYPE_SAVE) failed (errno %d)",
                  PROBE_NEW, errno);
        return false;
    }

    uint32_t written = 0;
    bool wrote_ok = true;
    uint64_t t0 = time_us_64();

    while (written < PROBE_BYTES && wrote_ok)
    {
        uint32_t n = PROBE_BYTES - written;
        if (n > PROBE_CHUNK)
            n = PROBE_CHUNK;
        for (uint32_t i = 0; i < n; i++)
            probe_buf[i] = probe_byte(written + i);
        wrote_ok = file_write(f, (const char *)probe_buf, n);
        written += n;
    }

    bool closed = file_close(f);
    uint64_t write_us = time_us_64() - t0;

    diag_step(wrote_ok && closed,
              "file_write %u B in %u B pieces, file_close: %s  (%lu ms, %lu KB/s)",
              (unsigned)PROBE_BYTES, (unsigned)PROBE_CHUNK,
              (wrote_ok && closed) ? "ok" : "FAILED",
              (unsigned long)(write_us / 1000ull),
              write_us ? (unsigned long)((uint64_t)PROBE_BYTES * 1000ull / write_us) : 0ul);
    if (!wrote_ok || !closed)
        return false;

    // The size the card reports, through the same stat() the game uses.
    struct stat st;
    long size = (stat(PROBE_NEW, &st) == 0) ? (long)st.st_size : -1L;
    diag_step(size == (long)PROBE_BYTES, "stat() size = %ld B (want %u)",
              size, (unsigned)PROBE_BYTES);

    // Read back one slice from the middle-end of the file. file_skip is z-file.c's only
    // seek and it is SEEK_CUR, so from a freshly opened file it is an absolute seek.
    f = file_open(PROBE_NEW, MODE_READ, FTYPE_SAVE);
    if (!f)
    {
        diag_step(false, "file_open for read after write failed (errno %d)", errno);
        return false;
    }

    bool seeked = file_skip(f, (int)PROBE_SEEK);
    t0 = time_us_64();
    int got = file_read(f, (char *)probe_buf, PROBE_VERIFY);
    uint64_t read_us = time_us_64() - t0;
    file_close(f);

    if (!seeked || got != (int)PROBE_VERIFY)
    {
        diag_step(false, "file_skip(%u)=%s, file_read(%u) returned %d",
                  (unsigned)PROBE_SEEK, seeked ? "ok" : "FAILED",
                  (unsigned)PROBE_VERIFY, got);
        return false;
    }

    uint32_t bad = 0;
    uint32_t first_bad = 0;
    for (uint32_t i = 0; i < PROBE_VERIFY; i++)
    {
        if (probe_buf[i] != probe_byte(PROBE_SEEK + i))
        {
            if (!bad)
                first_bad = PROBE_SEEK + i;
            bad++;
        }
    }

    diag_step(bad == 0, "file_skip+file_read %u B at offset %u: %s (%lu us)",
              (unsigned)PROBE_VERIFY, (unsigned)PROBE_SEEK,
              bad ? "MISMATCH" : "byte-exact", (unsigned long)read_us);
    if (bad)
        diag_printf_colour(COL_BAD, "      %lu bad bytes, first at offset %lu",
                           (unsigned long)bad, (unsigned long)first_bad);

    return bad == 0;
}

// ---------------------------------------------------------------------------------------
// Step 6. file_move, both ways round: onto a free name and onto an occupied one. The
// second is savefile.c's normal case and the one FatFs refuses without the PICOCALC edit
// in z-file.c.

static void step_move_and_delete(void)
{
    // Pass 1: the target does not exist.
    if (file_exists(PROBE_FINAL))
        (void)file_delete(PROBE_FINAL);

    bool moved1 = file_move(PROBE_NEW, PROBE_FINAL);
    diag_step(moved1 && file_exists(PROBE_FINAL) && !file_exists(PROBE_NEW),
              "file_move probe.new -> probe (target absent): %s",
              moved1 ? "ok" : "FAILED");

    // Pass 2: write a second probe.new and move it over the probe that now exists. This is
    // the case the acceptance criterion means by "run the sequence twice without deleting
    // in between", done inside one run so it cannot be skipped by accident.
    ang_file *f = file_open(PROBE_NEW, MODE_WRITE, FTYPE_SAVE);
    if (f)
    {
        for (uint32_t i = 0; i < PROBE_CHUNK; i++)
            probe_buf[i] = probe_byte(i);
        (void)file_write(f, (const char *)probe_buf, PROBE_CHUNK);
        file_close(f);
    }

    bool moved2 = file_move(PROBE_NEW, PROBE_FINAL);
    struct stat st;
    long size2 = (stat(PROBE_FINAL, &st) == 0) ? (long)st.st_size : -1L;

    diag_step(moved2 && size2 == (long)PROBE_CHUNK,
              "file_move probe.new -> probe (target PRESENT): %s, probe is now %ld B (want %u)",
              moved2 ? "ok" : "FAILED", size2, (unsigned)PROBE_CHUNK);

    // The exclusive-create guard: probe.new is gone, probe is not, so opening probe for
    // FTYPE_SAVE writing must be refused. Without the PICOCALC edit pico-vfs would grant it
    // and leave the old tail in place.
    errno = 0;
    ang_file *taken = file_open(PROBE_FINAL, MODE_WRITE, FTYPE_SAVE);
    if (taken)
        file_close(taken);
    diag_step(taken == NULL,
              "file_open(existing name, MODE_WRITE, FTYPE_SAVE) refused: %s",
              taken == NULL ? "yes (upstream semantics kept)" : "NO -- it overwrote");

    bool exists_before = file_exists(PROBE_FINAL);
    bool deleted = file_delete(PROBE_FINAL);
    bool exists_after = file_exists(PROBE_FINAL);

    diag_step(exists_before && deleted && !exists_after,
              "file_exists before=%d, file_delete=%d, file_exists after=%d",
              (int)exists_before, (int)deleted, (int)exists_after);

    // Leave nothing behind, so a re-run starts from the same state as run 1.
    (void)file_delete(PROBE_NEW);
}

// ---------------------------------------------------------------------------------------
// Step 7. time(). The scores file and the character dump both date themselves from it, and
// the SDK's weak _gettimeofday would have them all in 1970 -- see platform/syscalls.c.

static void step_time(void)
{
    static char stamp[64];

    time_t t1 = time(NULL);
    struct tm *tmv = gmtime(&t1);
    if (tmv)
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tmv);
    else
        my_strcpy(stamp, "(gmtime failed)", sizeof(stamp));

    diag_note("time() = %lu  -> %s UTC", (unsigned long)t1, stamp);
    diag_printf("      sleeping 5 s...");
    sleep_ms(5000);

    time_t t2 = time(NULL);
    long delta = (long)(t2 - t1);

    diag_step(delta >= 4 && delta <= 6,
              "time() advanced %ld s over a 5 s sleep (want 5 +/- 1): %lu -> %lu",
              delta, (unsigned long)t1, (unsigned long)t2);
}

// ---------------------------------------------------------------------------------------

static void run_all(unsigned run)
{
    diag_row = 0;
    n_pass = 0;
    n_fail = 0;
    for (int r = 0; r < DIAG_LOG_ROWS; r++)
        diag_draw(r, "", COL_FG);

    printf("\n");
    diag_printf_colour(COL_NOTE,
                       "angband_fsdiag -- angband-pico stage 030 -- run %u%s",
                       run, (run == 1) ? "" : " (re-run)");

    if (!step_mount())
    {
        diag_printf_colour(COL_BAD, "no filesystem -- nothing further can be tested");
        return;
    }

    step_readdir();
    step_read_text();
    step_dir_create();
    probe_raw_semantics();

    if (step_roundtrip())
        step_move_and_delete();
    else
        diag_printf_colour(COL_BAD, "round trip failed -- skipping the move/delete steps");

    step_getl_throughput();
    step_time();

    diag_printf_colour(n_fail ? COL_BAD : COL_OK,
                       "done   %u pass, %u FAIL   heap brk %u KB", n_pass, n_fail,
                       (unsigned)(psram_heap_used() / 1024u));
}

int main(void)
{
    stdio_init_all();

    // Two named references the linker needs, both for the same reason (stage 020's _sbrk
    // correction): psram_heap.c holds the strong _sbrk and syscalls.c the strong
    // _gettimeofday, and a strong symbol in a STATIC library that nothing names is never
    // pulled in over the SDK's __weak one. Neither call does anything interesting here.
    syscalls_init();
    (void)psram_heap_used();

    lcd_init();
    lcd_set_font(&font_5x10);
    lcd_enable_cursor(false);
    lcd_set_background(COL_BG);
    lcd_set_foreground(COL_FG);
    lcd_clear_screen();

    sb_init(); // the keyboard, for the re-run key

    // z-util.c's hooks. Set before any core call, so a quit() lands on the panel instead
    // of halting the board silently.
    plog_aux = diag_plog_hook;
    quit_aux = diag_quit_hook;

    diag_draw(DIAG_HINT_ROW, "waiting up to 10 s for a USB terminal...", COL_NOTE);

    // specifications.md 6.5. Never block: the diag must still run with no terminal.
    for (unsigned i = 0; i < 100 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    sleep_ms(300);

    unsigned run = 1;
    run_all(run);

    diag_draw(DIAG_HINT_ROW, "press any key to re-run the whole sequence", COL_NOTE);
    printf("press any key on the PicoCalc to re-run the whole sequence\n");

    unsigned long frames = 0;
    while (true)
    {
        uint16_t key = sb_read_keyboard();
        if (((key >> 8) & 0xFF) == 1)
        {
            run++;
            run_all(run);
            diag_draw(DIAG_HINT_ROW, "press any key to re-run the whole sequence", COL_NOTE);
            printf("press any key on the PicoCalc to re-run the whole sequence\n");
            frames = 0;
        }

        snprintf(diag_line, sizeof(diag_line), "run %u   frame %lu   up %lu s",
                 run, frames, (unsigned long)syscalls_uptime_s());
        diag_draw(DIAG_STATUS_ROW, diag_line, COL_NOTE);

        if ((frames % 40u) == 0u)
            printf("%s\n", diag_line);

        frames++;
        sleep_ms(50);
    }
}
