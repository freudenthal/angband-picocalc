// PORT: port-written for angband-pico, stage 160.
//
// Arms the ARMv8-M stack limit (MSPLIM) so a core-0 stack overflow raises a fault at once,
// instead of silently writing into .bss below the stack (specifications.md 6.5, 7.2, 12).

#ifndef ANGBAND_PICO_PLATFORM_STACKGUARD_H
#define ANGBAND_PICO_PLATFORM_STACKGUARD_H

#ifdef __cplusplus
extern "C"
{
#endif

    // Sets MSPLIM to __StackBottom + STACKGUARD_RESERVE (src/platform/stackguard.c) and
    // installs the HardFault handler that reports an overflow and halts. Call once, right
    // after stack_paint() and before anything else runs (src/main.c).
    void stackguard_arm(void);

#ifdef __cplusplus
}
#endif

#endif
