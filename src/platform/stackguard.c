// PORT: port-written for angband-pico, stage 160.
//
// specifications.md 6.5 (the guard and the fault message), 7.2 (the invariant), 12 (closed).
//
// WHY. crt0.S writes 0 to MSPLIM only on the debug-entry path (never taken on a normal UF2
// boot, which goes through the bootrom instead -- pico-sdk 2.3.0
// src/rp2_common/pico_crt0/crt0.S:424-428, guarded by !PICO_CRT0_DEBUG_ENTRY_RESETS_VIA_BOOTROM),
// and the SDK's own arming code, runtime_init_per_core_install_stack_guard()
// (src/rp2_common/pico_runtime_init/runtime_init_stack_guard.c:70-73, "msr msplim, %0" on
// Armv8-M), is compiled out by default: PICO_RUNTIME_NO_INIT_PER_CORE_INSTALL_STACK_GUARD
// defaults to 1 unless PICO_USE_STACK_GUARDS is 1, and PICO_USE_STACK_GUARDS itself defaults
// to 0 (src/rp2350/pico_platform/include/pico/platform.h:64-66). This project sets neither,
// so on this board MSPLIM is left at its Armv8-M reset value (0, i.e. no limit) for the
// whole life of the program until this file arms it. Without that, an overflow keeps
// writing downward into whatever sits below __StackBottom -- .bss, possibly while the
// savefile is being written -- with no fault at all.
//
// WHAT AN OVERFLOW LOOKS LIKE ONCE ARMED. A push that lowers SP below MSPLIM raises a
// UsageFault with CFSR.STKOF set (core_cm33.h's SCB_CFSR_STKOF_Msk, bit 20 of CFSR --
// Armv8-M Architecture Reference Manual / Cortex-M33 Technical Reference Manual, "UsageFault
// Status Register", DDI0553). The SDK never sets SHCSR.USGFAULTENA, so UsageFault stays
// disabled and the fault escalates to HardFault with HFSR.FORCED set instead (DDI0553,
// "HardFault Status Register") -- but CFSR is a fault STATUS register, latched by the fault
// itself before the CPU decides which handler to take, so STKOF is still readable from the
// HardFault handler below.
//
// THE RESERVE. Exception entry on the Cortex-M33 pushes an 8-word frame onto the faulting
// stack before the handler's first instruction runs. If that push itself crosses MSPLIM, the
// entry is still taken (the architecture does not fault its own frame push a second time),
// but SP is left sitting at or below the limit -- so the handler's own prologue, and
// anything it calls, would immediately re-trip the same condition if MSPLIM were still armed
// at the true bottom. STACKGUARD_RESERVE gives the frame push, the naked trampoline and the
// C handler's own frame somewhere to land. Item 7.1 of the stage plan proves 1,024 B is
// enough; the naked entry below also clears MSPLIM to 0 as its very first instruction, so
// even a handler frame that eats the whole reserve cannot re-fault the report itself.
//
// THE SHAPE. The naked-trampoline-into-a-C-handler pattern is bootprof.c's SysTick handler
// (stage 140) and codediag.c's own HardFault handler (stage 150), adapted to
// HARDFAULT_EXCEPTION here. EXC_RETURN bit 2 (LR on exception entry) says whether the
// faulting code was on MSP or PSP; this project is single-core and single-stack (core 1 is
// never launched, specifications.md 7.2), so it is always MSP in practice, but the test costs
// one instruction and costs nothing to get right.

#include <stdbool.h>
#include <stdint.h>

#include "hardware/exception.h"
#include "hardware/structs/scb.h"
#include "hardware/uart.h"

#include "platform/lcd.h"
#include "platform/stackguard.h"

// Recommendation from the stage plan's open question 2: room for the handler's own frame
// and the polled print, proved sufficient by item 7.1's device run (transcript in the stage
// run log).
#ifndef STACKGUARD_RESERVE
#define STACKGUARD_RESERVE 1024u
#endif

extern char __StackBottom;

#ifdef ANGBAND_CONSOLE

// Polled, not printf(): a fault is not guaranteed to leave stdio's buffers or its USB/UART
// driver state in any usable condition (codediag.c, bootprof.c: the same reasoning).
static void stackguard_uart_puts_polled(const char *s)
{
    while (*s)
    {
        while (!uart_is_writable(uart0))
        {
        }
        uart_putc_raw(uart0, *s++);
    }
}

static void stackguard_uart_puthex32_polled(uint32_t v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[9];
    for (int i = 7; i >= 0; i--)
    {
        buf[i] = hexd[v & 0xFu];
        v >>= 4;
    }
    buf[8] = '\0';
    stackguard_uart_puts_polled(buf);
}

#endif /* ANGBAND_CONSOLE */

// One red row across the full width, direct to the panel. lcd_set_window() and
// lcd_write16_data() never touch lcd_sem -- only lcd_blit() and lcd_solid_rectangle() do
// (src/platform/lcd.c) -- which matters here because the code the overflow interrupted may
// itself have been mid-blit, holding that semaphore: calling anything that tries to acquire
// it from this handler could deadlock the one path meant to report the fault. This is the
// "fault-safe path" the stage plan asks to look for in src/platform/ before deciding whether
// a panel message is possible at all; it is, through these two primitives, in every build
// (the shipped one has no console to print to, so this is its only report).
static void stackguard_paint_panel(void)
{
    const uint16_t red = RGB(255, 0, 0);

    lcd_set_window(0, 0, WIDTH - 1, GLYPH_HEIGHT - 1);
    for (uint32_t i = 0; i < (uint32_t)WIDTH * GLYPH_HEIGHT; i++)
        lcd_write16_data(1, red);
}

// frame[] is the eight words Cortex-M33 exception entry stacks: r0 r1 r2 r3 r12 lr pc xpsr.
// __attribute__((used)): its only reference is the "b" in the naked trampoline below, which
// is invisible to dead-code analysis (bootprof.c, codediag.c: the same note).
static void __attribute__((used)) stackguard_hardfault_isr_c(uint32_t *frame)
{
#ifdef ANGBAND_CONSOLE
    uint32_t cfsr = scb_hw->cfsr;
    uint32_t hfsr = scb_hw->hfsr;
    bool stkof = (cfsr & (1u << 20)) != 0; // SCB_CFSR_STKOF_Msk (core_cm33.h)

    stackguard_uart_puts_polled("\r\n*** stack overflow ***\r\n");
    stackguard_uart_puts_polled(stkof ? "STKOF " : "(no STKOF bit) ");
    stackguard_uart_puts_polled("pc=0x");
    stackguard_uart_puthex32_polled(frame[6]);
    stackguard_uart_puts_polled(" lr=0x");
    stackguard_uart_puthex32_polled(frame[5]);
    stackguard_uart_puts_polled(" cfsr=0x");
    stackguard_uart_puthex32_polled(cfsr);
    stackguard_uart_puts_polled(" hfsr=0x");
    stackguard_uart_puthex32_polled(hfsr);
    stackguard_uart_puts_polled("\r\nhalted -- power cycle the PicoCalc\r\n");
#else
    (void)frame;
#endif

    stackguard_paint_panel();

    for (;;)
        __asm volatile("wfi");
}

static void __attribute__((naked)) stackguard_hardfault_isr(void)
{
    __asm volatile(
        "movs r1, #0                    \n" // Clear MSPLIM FIRST: see "THE RESERVE" above --
        "msr msplim, r1                 \n" // if the frame push itself crossed the limit, SP
                                             // is already at or below it, and the prologue
                                             // below must not re-fault on its own frame.
        "tst lr, #4                     \n"
        "ite eq                         \n"
        "mrseq r0, msp                  \n"
        "mrsne r0, psp                  \n"
        "b stackguard_hardfault_isr_c   \n"
        :
        :
        : "r0", "r1");
}

void stackguard_arm(void)
{
    uintptr_t limit;

    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, stackguard_hardfault_isr);

    limit = (uintptr_t)&__StackBottom + STACKGUARD_RESERVE;
    __asm volatile("msr msplim, %0" : : "r"(limit));
}
