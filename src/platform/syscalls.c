// PORT: port-written for angband-pico, stage 030.
//
// _gettimeofday, _getpid and _kill. specifications.md 6.3.
//
// WHY OVERRIDE _gettimeofday AT ALL. The SDK's version (pico_clib_interface/
// newlib_interface.c) is already monotonic from time_us_64() -- but its epoch offset starts
// at zero, so time() returns "1970-01-01 plus a few seconds since boot" and every date the
// game writes (lib/user/scores/scores.raw, the character dump header) is 1970. The SDK
// offers settimeofday() to move the epoch, but that would leave the epoch in the SDK's
// static and out of this port's sight. One self-contained definition is simpler to reason
// about, and it is what specifications.md 6.3 says this file does.
//
// WHAT time() IS AND IS NOT HERE. There is no RTC and no network. This is boot time plus a
// fixed constant: monotonic, never going backwards, always plausible-looking, and always
// wrong by however long the board has been off. The game's uses survive that -- z-rand.c
// seeds from time(NULL) (which only needs to differ between boots... and does not, on this
// port; see the note at the bottom), scores are ordered by score not date, and the date in
// a character dump is cosmetic.
//
// THE LINKER TRAP. All three of these are __weak in the SDK. A strong definition in a
// STATIC library is not enough on its own -- see syscalls_init() in syscalls.h, and the
// stage 020 correction about _sbrk. main() must call syscalls_init().

#include "platform/syscalls.h"

#include <errno.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>

#include "pico/stdlib.h"
#include "pico/time.h"

void syscalls_init(void)
{
    // Deliberately empty. Its only job is to be a name the executable references, so the
    // linker opens this object file and the strong symbols below win over the SDK's weak
    // ones. Read the header comment before deleting it.
}

uint32_t syscalls_uptime_s(void)
{
    return (uint32_t)(time_us_64() / 1000000ull);
}

int _gettimeofday(struct timeval *__restrict tv, void *__restrict tz)
{
    (void)tz;
    if (tv)
    {
        uint64_t us = time_us_64();
        tv->tv_sec = (time_t)(SYSCALLS_EPOCH_2026 + (uint32_t)(us / 1000000ull));
        tv->tv_usec = (suseconds_t)(us % 1000000ull);
    }
    return 0;
}

// Angband never forks. getpid() reaches this only through library paths that want a
// nonzero, stable id; 1 is the conventional answer for a single-process system. The SDK's
// weak version returns 0, which some libc code reads as "no process".
pid_t _getpid(void)
{
    return 1;
}

// There is nothing to signal. h-basic.h still pulls in <signal.h> unconditionally
// (specifications.md 6.1), so raise()/abort() can still reach _kill; failing with EINVAL is
// the honest answer and leaves abort() to fall through to its own halt.
int _kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

// NOTE FOR STAGE 050. Because the epoch is fixed and the board has no RTC, time(NULL) at
// the moment the RNG is seeded is very nearly the same number on every boot -- it is just
// however many seconds bring-up took. z-rand.c under a non-UNIX platform seeds from time()
// alone (stage 010 correction), so without help every new game would roll the same dungeon.
// Stage 050 has to mix in something that actually varies: the ROSC, or the time between
// power-on and the player's first keypress.
