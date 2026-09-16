// PORT: port-written for angband-pico, stage 140. See bootprof.h for what this is and why
// the declarations live in a header src/game/datafile.c can reach.
//
// THREE PIECES, IN THE ORDER THE STAGE PLAN ASKS FOR THEM.
//
//   1. THE SAMPLER. A SysTick handler at 1 kHz, the SDK's own clock source (item 2). The
//      handler is a two-instruction naked trampoline that picks MSP or PSP off EXC_RETURN
//      bit 2 and tail-branches into bootprof_isr_c(), which reads the stacked PC and LR out
//      of the exception frame and appends one 8-byte sample to a raw buffer malloc'd in
//      PSRAM before init_angband() runs. THE HANDLER AND EVERYTHING IT CALLS MUST BE IN
//      SRAM (specifications.md 13: "an instrument placed in flash pays the toll it
//      measures") -- so this whole file is excluded into SRAM by CMakeLists.txt the same
//      way turnlog.c is, and sram-check.py's extension for this stage checks every symbol
//      here, not a sample of them.
//   2. THE RUN-TIME GATE. bootprof_gate_poll() is called from the console build's existing
//      10 s USB wait in src/main.c and arms the sampler -- allocates the buffer, nothing
//      more -- on the byte 'P'. Item 3 of the stage plan: the harness can drive this over
//      the case UART (COM7), which is live from power-on and does not need the USB CDC host
//      the wait is otherwise waiting for.
//   3. THE DUMP. Printed once, from src/main.c, after every phase mark has been taken.
//      Phases and parser records are already-computed numbers, so printing them first costs
//      nothing timing-sensitive; the PC/(PC,LR) aggregation that follows is a sort over
//      already-captured samples and cannot perturb any of the figures printed before it.
//
// WITHOUT ANGBAND_BOOT_PROFILE THIS FILE IS NOT COMPILED AT ALL -- unlike turnlog.c, which
// is always a source of the `angband` target and is an empty translation unit when its
// option is off. CMakeLists.txt adds this file to the target's sources only when
// ANGBAND_BOOT_PROFILE is ON: one more object on the link, even one that compiles to a
// single typedef, still moves the linker's long-branch veneers and shifted build-pico2's
// .text by 24,497 of 500,088 bytes with text/data/bss unchanged (specifications.md 13's own
// ANGBAND_SYNC precedent -- "one more object ... moves the veneers even when it contributes
// nothing" -- found the hard way, by this stage's own criterion-1 cmp, before this comment
// was corrected to say so). The #else stub below is therefore dead in every build that
// exists, but is kept for the same "an ISO C translation unit needs a declaration" reason
// turnlog.c's has, in case a later stage ever changes how this file is wired in.

#include "bootprof.h"

#ifdef ANGBAND_BOOT_PROFILE

// Stage 090: the dump is printed on the developer console, which only a console build has.
// CMakeLists.txt refuses the combination at configure time; this is the second lock, and it
// lives here rather than in bootprof.h because that header is also included by
// src/game/datafile.c, which compiles as part of angband_core -- a target that never
// receives ANGBAND_CONSOLE even in a console build (specifications.md 10). turnlog.c's own
// #error is in exactly the same place for exactly the same reason.
#ifndef ANGBAND_CONSOLE
#error "ANGBAND_BOOT_PROFILE needs ANGBAND_CONSOLE: configure with -DANGBAND_CONSOLE=ON"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "hardware/clocks.h"
#include "hardware/exception.h"
#include "hardware/structs/systick.h"
#include "pico/stdlib.h"

#include "psram_heap.h"

// ---------------------------------------------------------------------------------------
// Sizing. 200 s at BOOTPROF_SAMPLE_HZ (500) is 800,000 B (0.763 MiB): stage 070/075 measured
// 5.9 MB of PSRAM free during init_angband(), so this comes nowhere near the heap the parser
// itself needs. The stage plan is explicit that an overrun COUNTS rather than wraps -- a
// wrapped buffer would silently overweight the end of the boot -- so bp_overflow is what a
// run that grew past 200 s would show, and specifications.md 13's "a check that cannot see
// the thing changed is not a check of that change" is why it is a counter and not a
// saturating no-op.
//
// THE RATE ITSELF WAS MEASURED DOWN FROM 1 kHz. Item 8.4 measured 5.7 % overhead on the
// device at 1 kHz -- init_angband() 113,798 ms sampled against 107,679 ms not, both the
// mean of two boots of build-pico2-prof, same binary, sampler gated at run time -- over the
// 3 % budget, which the stage plan's own open question 1 anticipated and named the fallback
// for: halve the rate. See the run log for the 500 Hz figure.
#define BOOTPROF_SAMPLE_HZ    500u
#define BOOTPROF_MAX_SECONDS  200u
#define BOOTPROF_CAPACITY     ((size_t)BOOTPROF_SAMPLE_HZ * BOOTPROF_MAX_SECONDS)

// 16 B: specifications.md 6.4 puts the whole 638 KB .text inside one 8 KB XIP cache period,
// so a bucket much narrower than that buys resolution the placement error bar cannot use.
// Widen this (32, 64) if item 4's dump-duration budget is missed on the device; the format
// is unaffected, only how many BOOTPROF pc/pair lines the device has to print.
#define BOOTPROF_BUCKET_SHIFT 4u

// The top N (PC, LR) pairs. All non-empty PC buckets are printed -- there are at most a few
// tens of thousands of those -- but a full pair table can be one entry per sample, so only
// the busiest 64 go on the wire; tools/bootprof.py's Callers table reads exactly these.
#define BOOTPROF_TOP_PAIRS    64u

#define BOOTPROF_MAX_MARKS    16u
#define BOOTPROF_MAX_PARSERS  96u

typedef struct {
    uint32_t pc;
    uint32_t lr;
} bp_sample_t;

typedef struct {
    const char *name;
    uint32_t us_since_reset_lo; // time_us_64() truncated to 32 bits: safe under ~71 minutes
} bp_mark_t;

typedef struct {
    const char *name;
    uint32_t us;
    uint32_t brk_before_kb;
    uint32_t brk_after_kb;
} bp_parser_rec_t;

// ---------------------------------------------------------------------------------------
// State. All of it is file-scope statics in this SRAM-resident object; none of it is heap
// except the sample buffer itself, which lives in PSRAM by design (item 2's "why PSRAM and
// not SRAM": 45 KB free cannot hold 1.5 MiB of raw samples).

static bp_sample_t *bp_buf;
static bool bp_gate_armed;
static bool bp_handler_installed;
static volatile bool bp_running;
static volatile uint32_t bp_count;
static volatile uint32_t bp_overflow;
static uint32_t bp_achieved_hz;
static uint64_t bp_window_start_us;
static uint64_t bp_window_end_us;

static bp_mark_t bp_marks[BOOTPROF_MAX_MARKS];
static uint32_t bp_mark_n;

static bp_parser_rec_t bp_parsers[BOOTPROF_MAX_PARSERS];
static uint32_t bp_parser_n;
static uint32_t bp_parser_dropped;
static uint64_t bp_cur_parser_t0;
static uint32_t bp_cur_parser_brk_before_kb;

// ---------------------------------------------------------------------------------------
// Item 6's self-test toggle. Reading the wrong exception-frame slots is a deliberate,
// compile-time-only mutation -- specifications.md 13: "a check that cannot see the thing
// changed is not a check of that change; break it on purpose". It never reaches a build a
// stage plan would ask for otherwise: ANGBAND_BOOT_PROFILE_SELFTEST_BREAK is refused
// without ANGBAND_BOOT_PROFILE_SELFTEST at configure time (CMakeLists.txt).
#ifdef ANGBAND_BOOT_PROFILE_SELFTEST_BREAK
#define BP_FRAME_PC 7 // wrong on purpose: the stacked xPSR's slot, not the stacked PC's
#define BP_FRAME_LR 4 // wrong on purpose: the stacked r12's slot, not the stacked LR's
#else
#define BP_FRAME_PC 6 // Cortex-M exception frame: r0 r1 r2 r3 r12 lr pc xpsr
#define BP_FRAME_LR 5
#endif

// ---------------------------------------------------------------------------------------
// The handler. __attribute__((used)): its only reference is the "b" in the naked trampoline
// below, which the compiler's dead-code analysis does not see as a call -- without this a
// build with --gc-sections could drop it, and a placement that silently fails looks exactly
// like one that worked (specifications.md 6.4's stage 045 correction).
//
// frame[] is the eight words the Cortex-M33 exception entry stacks: r0 r1 r2 r3 r12 lr pc
// xpsr. The stacked LR (frame[5]) is the one level of caller that makes a leaf function
// such as strtol or memcpy attributable; the stacked PC (frame[6]) is where the sample
// landed.
static void __attribute__((used)) bootprof_isr_c(uint32_t *frame)
{
    uint32_t idx;

    if (!bp_running)
        return;

    idx = bp_count;
    if (idx >= BOOTPROF_CAPACITY) {
        bp_overflow++;
        return;
    }

    bp_buf[idx].pc = frame[BP_FRAME_PC];
    bp_buf[idx].lr = frame[BP_FRAME_LR];
    bp_count = idx + 1;
}

// The two-line naked entry item 2 asks for. EXC_RETURN (in lr at exception entry) has bit 2
// clear when the interrupted code was using the main stack and set when it was using the
// process stack; this program never switches to PSP, so in practice this always takes the
// MSP arm, but reading it rather than assuming it is what makes the handler correct on a
// build that someday does. "b", not "bl": LR is left exactly as the CPU set it at exception
// entry, so when bootprof_isr_c() (an ordinary AAPCS function) saves and restores it as
// part of its own prologue/epilogue, its final "pop {..., pc}" performs the actual exception
// return.
static void __attribute__((naked)) bootprof_systick_isr(void)
{
    __asm volatile (
        "tst lr, #4         \n"
        "ite eq             \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "b bootprof_isr_c   \n"
        :
        :
        : "r0"
    );
}

// ---------------------------------------------------------------------------------------
// Marks and parser records. Neither is on the ISR's path; both are called from ordinary
// main-line code a handful of times over the whole boot, so unlike the ISR they cost
// nothing to leave in an SRAM-resident object either way.

void bootprof_mark(const char *name)
{
    if (bp_mark_n >= BOOTPROF_MAX_MARKS)
        return;

    bp_marks[bp_mark_n].name = name;
    bp_marks[bp_mark_n].us_since_reset_lo = (uint32_t)time_us_64();
    bp_mark_n++;
}

void bootprof_parser_begin(void)
{
    bp_cur_parser_t0 = time_us_64();
    bp_cur_parser_brk_before_kb = (uint32_t)(psram_heap_used() / 1024u);
}

void bootprof_parser_end(const char *name)
{
    uint32_t elapsed_us = (uint32_t)(time_us_64() - bp_cur_parser_t0);
    uint32_t brk_after_kb = (uint32_t)(psram_heap_used() / 1024u);

    if (bp_parser_n >= BOOTPROF_MAX_PARSERS) {
        bp_parser_dropped++;
        return;
    }

    bp_parsers[bp_parser_n].name = name;
    bp_parsers[bp_parser_n].us = elapsed_us;
    bp_parsers[bp_parser_n].brk_before_kb = bp_cur_parser_brk_before_kb;
    bp_parsers[bp_parser_n].brk_after_kb = brk_after_kb;
    bp_parser_n++;
}

// ---------------------------------------------------------------------------------------
// The run-time gate (item 3). Called from src/main.c's existing 10 s USB-wait loop, once
// per 100 ms poll, the same cadence ANGBAND_SERIAL_KEYS's check_events() poll uses.
// getchar_timeout_us(0) reads whichever stdio transport has a byte waiting and returns
// PICO_ERROR_TIMEOUT at once otherwise -- it is why the harness can arm this over the case
// UART (COM7), which is live long before the USB CDC host this wait is otherwise waiting
// for, without this file needing to touch UART0 registers directly.
//
// The buffer is malloc'd here, ONCE, the first time 'P' is seen -- "allocated once with
// malloc before init_angband()" (item 2) -- and never freed: this is a one-shot boot
// profiler, not a long-running instrument.
void bootprof_gate_poll(void)
{
    int c;

    if (bp_gate_armed)
        return;

    c = getchar_timeout_us(0);
    if (c != (int)'P')
        return;

    bp_buf = malloc(BOOTPROF_CAPACITY * sizeof(bp_sample_t));
    if (!bp_buf) {
        // Leave the gate unarmed rather than sample into a NULL buffer. bootprof_dump()
        // then reports zero samples honestly instead of crashing.
        printf("BOOTPROF gate_failed no memory for %lu samples\n",
               (unsigned long)BOOTPROF_CAPACITY);
        return;
    }

    bp_gate_armed = true;
    printf("BOOTPROF gate armed\n");
}

#ifdef ANGBAND_BOOT_PROFILE_SELFTEST
// Forces the gate on without waiting for 'P' -- the self-test tree has nothing driving the
// UART, and its whole point is to run unattended. See src/main.c.
void bootprof_force_arm(void)
{
    if (bp_gate_armed)
        return;

    bp_buf = malloc(BOOTPROF_CAPACITY * sizeof(bp_sample_t));
    if (!bp_buf) {
        printf("BOOTPROF gate_failed no memory for %lu samples\n",
               (unsigned long)BOOTPROF_CAPACITY);
        return;
    }
    bp_gate_armed = true;
    printf("BOOTPROF gate armed (forced, self-test)\n");
}
#endif

// ---------------------------------------------------------------------------------------
// Start/stop. A no-op either way if the gate was never armed, so src/main.c can bracket
// init_angband() with these unconditionally (item 3: "the phase marks and parser brackets
// run in both modes").
void bootprof_start(void)
{
    uint32_t clk_hz, reload;

    if (!bp_gate_armed)
        return;

    if (!bp_handler_installed) {
        exception_set_exclusive_handler(SYSTICK_EXCEPTION, bootprof_systick_isr);
        bp_handler_installed = true;
    }

    // "the core's own clock source; reload from clk_sys, read the achieved frequency back
    // rather than assuming 150 MHz" (item 2).
    clk_hz = clock_get_hz(clk_sys);
    reload = clk_hz / BOOTPROF_SAMPLE_HZ;
    if (reload == 0)
        reload = 1;
    reload -= 1;
    if (reload > 0x00FFFFFFu)
        reload = 0x00FFFFFFu;
    bp_achieved_hz = clk_hz / (reload + 1);

    bp_count = 0;
    bp_overflow = 0;

    systick_hw->rvr = reload;
    systick_hw->cvr = 0;
    // ENABLE | TICKINT | CLKSOURCE=1 (processor clock, not the external reference).
    systick_hw->csr = 0x7u;

    bp_window_start_us = time_us_64();
    bp_running = true;
}

void bootprof_stop(void)
{
    if (!bp_running)
        return;

    systick_hw->csr = 0;
    bp_running = false;
    bp_window_end_us = time_us_64();
}

// ---------------------------------------------------------------------------------------
// The dump. Order matters: phases and parser records are numbers already computed by the
// time this runs, so printing them first costs nothing the boot time reflects; the
// aggregation below is a sort over samples already captured with the sampler stopped, so it
// cannot perturb anything printed before it (item 4).

static int bp_cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int bp_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    uint64_t key; // (pc_bucket << 32) | lr_bucket
    uint32_t count;
} bp_pair_count_t;

static int bp_cmp_pair_count_desc(const void *a, const void *b)
{
    const bp_pair_count_t *x = a, *y = b;

    if (x->count != y->count)
        return (y->count > x->count) - (y->count < x->count);
    return (x->key > y->key) - (x->key < y->key);
}

// All non-empty PC buckets, in address order. There are at most a few tens of thousands of
// these -- bounded by how much code the boot actually executes, not by the address space --
// which is what item 4's 60 s dump budget is measured against.
static void bp_dump_pc_buckets(uint32_t n)
{
    uint32_t *keys = malloc((size_t)n * sizeof(uint32_t));
    uint32_t i, run_start;

    if (!keys) {
        printf("BOOTPROF aggregate_failed no memory for %lu pc keys\n", (unsigned long)n);
        return;
    }

    for (i = 0; i < n; i++)
        keys[i] = bp_buf[i].pc >> BOOTPROF_BUCKET_SHIFT;

    qsort(keys, n, sizeof(uint32_t), bp_cmp_u32);

    run_start = 0;
    for (i = 1; i <= n; i++) {
        if (i == n || keys[i] != keys[run_start]) {
            printf("BOOTPROF pc 0x%08lx %lu\n",
                   (unsigned long)(keys[run_start] << BOOTPROF_BUCKET_SHIFT),
                   (unsigned long)(i - run_start));
            run_start = i;
        }
    }

    free(keys);
}

// The top BOOTPROF_TOP_PAIRS (PC, LR) pairs by sample count. tools/bootprof.py's Callers
// table -- who calls the busiest leaf functions -- is built from exactly these lines.
static void bp_dump_pairs(uint32_t n)
{
    uint64_t *pkeys = malloc((size_t)n * sizeof(uint64_t));
    bp_pair_count_t *counts;
    uint32_t i, run_start, n_distinct, top, j;

    if (!pkeys) {
        printf("BOOTPROF aggregate_failed no memory for %lu pair keys\n", (unsigned long)n);
        return;
    }

    for (i = 0; i < n; i++) {
        uint64_t pcb = bp_buf[i].pc >> BOOTPROF_BUCKET_SHIFT;
        uint64_t lrb = bp_buf[i].lr >> BOOTPROF_BUCKET_SHIFT;

        pkeys[i] = (pcb << 32) | (lrb & 0xFFFFFFFFu);
    }

    qsort(pkeys, n, sizeof(uint64_t), bp_cmp_u64);

    counts = malloc((size_t)n * sizeof(bp_pair_count_t));
    if (!counts) {
        printf("BOOTPROF aggregate_failed no memory for %lu pair counts\n", (unsigned long)n);
        free(pkeys);
        return;
    }

    n_distinct = 0;
    run_start = 0;
    for (i = 1; i <= n; i++) {
        if (i == n || pkeys[i] != pkeys[run_start]) {
            counts[n_distinct].key = pkeys[run_start];
            counts[n_distinct].count = i - run_start;
            n_distinct++;
            run_start = i;
        }
    }

    qsort(counts, n_distinct, sizeof(bp_pair_count_t), bp_cmp_pair_count_desc);

    top = (n_distinct < BOOTPROF_TOP_PAIRS) ? n_distinct : BOOTPROF_TOP_PAIRS;
    for (j = 0; j < top; j++) {
        uint32_t pcb = (uint32_t)(counts[j].key >> 32);
        uint32_t lrb = (uint32_t)(counts[j].key & 0xFFFFFFFFu);

        printf("BOOTPROF pair 0x%08lx 0x%08lx %lu\n",
               (unsigned long)(pcb << BOOTPROF_BUCKET_SHIFT),
               (unsigned long)(lrb << BOOTPROF_BUCKET_SHIFT),
               (unsigned long)counts[j].count);
    }

    free(counts);
    free(pkeys);
}

void bootprof_dump(void)
{
    uint32_t i;
    uint32_t n = bp_count;
    uint64_t parser_total_us = 0;

    for (i = 0; i < bp_mark_n; i++)
        printf("BOOTPROF phase %s %lu\n", bp_marks[i].name,
               (unsigned long)bp_marks[i].us_since_reset_lo);

    for (i = 0; i < bp_parser_n; i++) {
        printf("BOOTPROF parser %s %lu %lu %lu\n", bp_parsers[i].name,
               (unsigned long)bp_parsers[i].us, (unsigned long)bp_parsers[i].brk_before_kb,
               (unsigned long)bp_parsers[i].brk_after_kb);
        parser_total_us += bp_parsers[i].us;
    }
    if (bp_parser_dropped)
        printf("BOOTPROF parser_dropped %lu\n", (unsigned long)bp_parser_dropped);

    // The unbracketed remainder of the init_angband() window: NOT assumed to be zero
    // (specifications.md 13, and the stage plan's own acceptance criterion). Only printed
    // when bootprof_start()/bootprof_stop() actually bracketed something -- the self-test
    // build's window is the two burn loops, not init_angband(), and both cases use the same
    // window variables, so this line means "the window bootprof_start()/stop() bracketed"
    // whichever one it was.
    if (bp_window_end_us > bp_window_start_us) {
        uint64_t window_us = bp_window_end_us - bp_window_start_us;
        uint64_t remainder = (window_us > parser_total_us) ? window_us - parser_total_us : 0;

        printf("BOOTPROF window %lu\n", (unsigned long)window_us);
        printf("BOOTPROF parser_total %lu\n", (unsigned long)parser_total_us);
        printf("BOOTPROF parser_remainder %lu\n", (unsigned long)remainder);
    }

    if (!bp_gate_armed || n == 0) {
        printf("BOOTPROF samples 0 overflow %lu hz 0\n", (unsigned long)bp_overflow);
        printf("BOOTPROF end\n");
        stdio_flush();
        return;
    }

    bp_dump_pc_buckets(n);
    bp_dump_pairs(n);

    printf("BOOTPROF samples %lu overflow %lu hz %lu\n", (unsigned long)n,
           (unsigned long)bp_overflow, (unsigned long)bp_achieved_hz);
    printf("BOOTPROF end\n");
    stdio_flush();
}

#else /* !ANGBAND_BOOT_PROFILE */

// An ISO C translation unit needs at least one declaration. Costs nothing and keeps this
// file in tools/port-warnings.sh's list whichever way the option is set (turnlog.c's rule).
typedef int bootprof_not_compiled_in;

#endif /* ANGBAND_BOOT_PROFILE */
