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
 * Host pointers are 8 bytes and glibc's minimum chunk is 32; the RP2350 has 4-byte
 * pointers and newlib's minimum chunk is 16, so device figures are expected to be
 * 55-65 % of these. See specifications.md section 7.1.
 *
 * The accounting helper is hs_acct(), not acct(): acct() is declared by <unistd.h>.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static size_t live, peak, nallocs;

static void hs_acct(long d)
{
	live += d;
	if (live > peak) peak = live;
}

void *malloc(size_t n)
{
	void *p = __libc_malloc(n);
	if (p) { hs_acct(malloc_usable_size(p)); nallocs++; }
	return p;
}

void *calloc(size_t a, size_t b)
{
	void *p = __libc_calloc(a, b);
	if (p) { hs_acct(malloc_usable_size(p)); nallocs++; }
	return p;
}

void *realloc(void *q, size_t n)
{
	size_t o = q ? malloc_usable_size(q) : 0;
	void *p = __libc_realloc(q, n);
	if (p) hs_acct((long)malloc_usable_size(p) - (long)o);
	return p;
}

void free(void *p)
{
	if (p) hs_acct(-(long)malloc_usable_size(p));
	__libc_free(p);
}

void heapshim_report(const char *tag)
{
	fprintf(stderr, "HEAP[%s] live=%zu peak=%zu allocs=%zu\n", tag, live, peak, nallocs);
}

static void fin(void)
{
	heapshim_report("exit");
}

__attribute__((constructor)) static void ini(void)
{
	atexit(fin);
}
