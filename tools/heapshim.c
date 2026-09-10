/*
 * heapshim.c -- an LD_PRELOAD malloc counter for the host harness.
 *
 * Built by tools/host-build.sh into build-host/heapshim.so and preloaded around
 * build-host/angband-test. It wraps malloc/calloc/realloc/free over glibc's __libc_*
 * entry points and keeps a running total of live bytes, peak bytes and allocation count,
 * measured with malloc_usable_size so the figures include glibc's per-chunk overhead.
 *
 * src/host/main-test.c's "heap?" command calls heapshim_report(), which is declared weak
 * there so the binary also runs without the preload. An atexit hook reports once more
 * under the tag "exit".
 *
 * STAGE 020 ADDED THE DEVICE MODEL. The old note here said device figures were "expected to
 * be 55-65 %" of the host's. That was a guess, and specifications.md 7.1 carried it as an
 * ASSUMPTION. It does not need to be a guess: the shim knows every requested size, and
 * stage 020 measured what newlib charges for one on the device. So the shim now carries a
 * second set of counters computed with newlib's chunk rule, and reports them beside the
 * glibc ones. dev_live / dev_peak are the numbers the RP2350 heap would show for the same
 * run, and they need no device to obtain.
 *
 * The rule is dev_chunk(n) = max(16, align8(n + 4)), from dlmalloc's structure with
 * SIZE_SZ 4 and 8-byte alignment. Two points measured on the device by angband_psramdiag on
 * 2026-09-09 pin it: malloc(3) costs 16 B and malloc(24) costs 32 B, each confirmed twice
 * over -- once as an _sbrk break delta and once as the modal address stride between
 * consecutive allocations. malloc(16) returning heap_base+8 on the first allocation of a
 * fresh heap is what fixes SIZE_SZ at 4: dlmalloc hands back chunk + 2*SIZE_SZ.
 *
 * Tracking dev_live needs the requested size back at free() time, which glibc will not give
 * (malloc_usable_size reports the rounded size, not the request), so the shim keeps its own
 * pointer -> request table. It is a fixed open-addressed array with tombstones, allocated
 * once through __libc_malloc to avoid recursing into the wrappers.
 *
 * The accounting helper is hs_acct(), not acct(): acct() is declared by <unistd.h>.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static size_t live, peak, nallocs;
static size_t dev_live, dev_peak;

/* ------------------------------------------------------------------ the device model */

/* What newlib's malloc takes from _sbrk for a request of n bytes. See the header comment. */
static size_t dev_chunk(size_t n)
{
	size_t c = (n + 4 + 7) & ~(size_t)7;
	return c < 16 ? 16 : c;
}

/* ------------------------------------------------------ pointer -> requested size table */

/*
 * Open addressing, linear probing, tombstones. Sized well above the ~310k allocations the
 * whole probe makes so that occupied-plus-tombstone load stays under about a third: a run
 * that outgrows it would silently lose sizes, so hs_put says so on stderr and gives up
 * rather than reporting a wrong dev_live.
 */
#define TAB_BITS 21
#define TAB_SLOTS ((size_t)1 << TAB_BITS)
#define TAB_MASK (TAB_SLOTS - 1)

#define SLOT_FREE ((void *)0)
#define SLOT_TOMB ((void *)(uintptr_t)1)

struct ent {
	void *p;
	size_t n;
};

static struct ent *tab;
static int tab_dead; /* set once the table has overflowed; counters stop being trusted */

static size_t hs_hash(void *p)
{
	uint64_t h = (uint64_t)(uintptr_t)p >> 4;
	h *= 0x9E3779B97F4A7C15ull;
	return (size_t)(h >> 40) & TAB_MASK;
}

static int hs_table_ready(void)
{
	if (tab)
		return 1;
	if (tab_dead)
		return 0;
	tab = __libc_calloc(TAB_SLOTS, sizeof *tab);
	if (!tab) {
		fprintf(stderr, "HEAP[warn] heapshim: no room for the size table;"
				" dev_live is not reported\n");
		tab_dead = 1;
		return 0;
	}
	return 1;
}

static void hs_put(void *p, size_t n)
{
	if (!hs_table_ready())
		return;

	size_t i = hs_hash(p);
	for (size_t probes = 0; probes < TAB_SLOTS; probes++) {
		struct ent *e = &tab[(i + probes) & TAB_MASK];
		if (e->p == SLOT_FREE || e->p == SLOT_TOMB || e->p == p) {
			e->p = p;
			e->n = n;
			return;
		}
	}

	fprintf(stderr, "HEAP[warn] heapshim: size table full;"
			" dev_live is no longer trustworthy\n");
	tab_dead = 1;
}

/* Returns the recorded request and clears the slot, or SIZE_MAX if the pointer is unknown. */
static size_t hs_take(void *p)
{
	if (!tab || tab_dead)
		return (size_t)-1;

	size_t i = hs_hash(p);
	for (size_t probes = 0; probes < TAB_SLOTS; probes++) {
		struct ent *e = &tab[(i + probes) & TAB_MASK];
		if (e->p == SLOT_FREE)
			return (size_t)-1;
		if (e->p == p) {
			size_t n = e->n;
			e->p = SLOT_TOMB;
			return n;
		}
	}
	return (size_t)-1;
}

static size_t hs_peek(void *p)
{
	if (!tab || tab_dead)
		return (size_t)-1;

	size_t i = hs_hash(p);
	for (size_t probes = 0; probes < TAB_SLOTS; probes++) {
		struct ent *e = &tab[(i + probes) & TAB_MASK];
		if (e->p == SLOT_FREE)
			return (size_t)-1;
		if (e->p == p)
			return e->n;
	}
	return (size_t)-1;
}

/* ------------------------------------------------------------------------- accounting */

static void hs_acct(long d)
{
	live += d;
	if (live > peak) peak = live;
}

static void hs_dev_acct(long d)
{
	dev_live += d;
	if (dev_live > dev_peak) dev_peak = dev_live;
}

static void hs_record(void *p, size_t n)
{
	hs_acct(malloc_usable_size(p));
	hs_dev_acct((long)dev_chunk(n));
	hs_put(p, n);
	nallocs++;
}

void *malloc(size_t n)
{
	void *p = __libc_malloc(n);
	if (p) hs_record(p, n);
	return p;
}

void *calloc(size_t a, size_t b)
{
	void *p = __libc_calloc(a, b);
	if (p) hs_record(p, a * b);
	return p;
}

void *realloc(void *q, size_t n)
{
	size_t o = q ? malloc_usable_size(q) : 0;
	size_t o_req = q ? hs_take(q) : (size_t)-1;

	void *p = __libc_realloc(q, n);
	if (!p)
		return p;

	hs_acct((long)malloc_usable_size(p) - (long)o);
	if (o_req != (size_t)-1)
		hs_dev_acct(-(long)dev_chunk(o_req));
	hs_dev_acct((long)dev_chunk(n));
	hs_put(p, n);
	return p;
}

void free(void *p)
{
	if (p) {
		hs_acct(-(long)malloc_usable_size(p));
		size_t req = hs_take(p);
		if (req != (size_t)-1)
			hs_dev_acct(-(long)dev_chunk(req));
	}
	__libc_free(p);
}

/* ---------------------------------------------------------------------------- reporting */

/*
 * The live size histogram, by EXACT request size. This is what turns dev_peak from a number
 * into an explanation: it says which request sizes the heap is actually made of, so a later
 * stage can see that the 3-byte square info bitflags cost 16 B each and dominate the count
 * while a handful of six-figure arrays dominate the bytes.
 *
 * Exact sizes, not power-of-two buckets. Buckets were tried first and were actively
 * misleading: the four largest live allocations on dungeon level 98 are different sizes that
 * all land in the 256K-512K bucket, and a bucket can only be labelled with one of them.
 * Exact sizes also make the host-to-device correction possible at all -- a device request is
 * SMALLER than the host's wherever the size came from a sizeof() over a struct holding
 * pointers (struct square is 40 B here and 24 B on the Cortex-M33), and that ratio can only
 * be applied per allocation site, which means per exact size.
 *
 * Distinct live sizes number in the low hundreds, so a 4096-slot open-addressed map is
 * ample. Lines start with "HEAP[" so that host-build.sh's grep keeps them.
 */
#define HIST_SLOTS 4096
#define HIST_TOP 12

static void hs_histogram(const char *tag)
{
	if (!tab || tab_dead)
		return;

	static size_t h_size[HIST_SLOTS];
	static size_t h_cnt[HIST_SLOTS];
	static size_t h_dev[HIST_SLOTS];
	memset(h_size, 0, sizeof h_size);
	memset(h_cnt, 0, sizeof h_cnt);
	memset(h_dev, 0, sizeof h_dev);

	size_t total_dev = 0, total_cnt = 0, overflow = 0;

	for (size_t i = 0; i < TAB_SLOTS; i++) {
		void *p = tab[i].p;
		if (p == SLOT_FREE || p == SLOT_TOMB)
			continue;

		size_t n = tab[i].n;
		total_dev += dev_chunk(n);
		total_cnt++;

		/* Key on n + 1 so that slot 0 stays available as "empty". */
		size_t k = ((n + 1) * 0x9E3779B1u) & (HIST_SLOTS - 1);
		size_t probes = 0;
		for (; probes < HIST_SLOTS; probes++) {
			size_t j = (k + probes) & (HIST_SLOTS - 1);
			if (h_cnt[j] == 0) {
				h_size[j] = n;
				h_cnt[j] = 1;
				h_dev[j] = dev_chunk(n);
				break;
			}
			if (h_size[j] == n) {
				h_cnt[j]++;
				h_dev[j] += dev_chunk(n);
				break;
			}
		}
		if (probes == HIST_SLOTS)
			overflow++;
	}

	fprintf(stderr, "HEAP[%s.sizes] %zu live allocations, %zu device bytes"
			" in the top %d classes below%s\n",
		tag, total_cnt, total_dev, HIST_TOP,
		overflow ? " (SOME SIZES LOST: histogram full)" : "");

	for (int k = 0; k < HIST_TOP; k++) {
		size_t best = 0, bi = 0;
		for (size_t j = 0; j < HIST_SLOTS; j++)
			if (h_dev[j] > best) { best = h_dev[j]; bi = j; }
		if (!best)
			break;

		size_t n = h_size[bi];
		fprintf(stderr, "HEAP[%s.size] %8zu B x %6zu = %9zu dev B"
				"  (%zu B/alloc, %zu%% of dev_live)\n",
			tag, n, h_cnt[bi], h_dev[bi], dev_chunk(n),
			total_dev ? h_dev[bi] * 100 / total_dev : 0);
		h_dev[bi] = 0;
	}
}

void heapshim_report(const char *tag)
{
	fprintf(stderr, "HEAP[%s] live=%zu peak=%zu allocs=%zu dev_live=%zu dev_peak=%zu\n",
		tag, live, peak, nallocs, dev_live, dev_peak);
	hs_histogram(tag);
}

static void fin(void)
{
	heapshim_report("exit");
}

__attribute__((constructor)) static void ini(void)
{
	atexit(fin);
}

/* Silence -Wunused-function if a future edit drops the only caller. */
void hs_unused(void *p);
void hs_unused(void *p) { (void)hs_peek(p); }
