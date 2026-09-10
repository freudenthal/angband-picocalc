// PORT: port-written for angband-pico, stage 030.
//
// The newlib syscalls the SDK leaves as weak stubs and the game actually needs.
// specifications.md 6.3.

#ifndef ANGBAND_PICO_SYSCALLS_H
#define ANGBAND_PICO_SYSCALLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The wall-clock epoch this port pretends to boot at, in seconds since 1970-01-01 UTC.
// 2026-01-01 00:00:00 UTC. There is no RTC and no network, so every date the game writes
// is boot time plus this.
#define SYSCALLS_EPOCH_2026 1767225600u

// CALL THIS FROM main() BEFORE ANYTHING READS THE CLOCK, and call it by name.
//
// It has nothing to do at run time -- _gettimeofday() below needs no setup -- but it is the
// reason the linker opens this archive member at all. The SDK's _gettimeofday, _getpid and
// _kill are __weak, and a strong definition sitting unreferenced in a STATIC library never
// gets pulled in: the linker satisfies libc's reference from the SDK object it has already
// loaded and never looks. This is the same trap stage 020 hit with _sbrk, and the same fix
// (see specifications.md 6.2 and the stage 020 corrections).
void syscalls_init(void);

// Seconds since SYSCALLS_EPOCH_2026, i.e. seconds since boot. Handy for a diagnostic that
// wants to talk about elapsed time without going through time_t.
uint32_t syscalls_uptime_s(void);

#ifdef __cplusplus
}
#endif

#endif // ANGBAND_PICO_SYSCALLS_H
