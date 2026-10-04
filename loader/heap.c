/* heap.c -- newlib heap telemetry and exhausted-heap handling
 * Historical measurements below are inherited from VitaKotor (KOTOR I), not
 * KOTOR II validation.
 *
 * log140 ended at t=926.9s with UNDEFINED_INSTRUCTION at _kill_r+0x42, which is
 * newlib's `udf #255` for a signal it cannot deliver -- r2=6, SIGABRT. r6 held
 * the "std::bad_alloc" what() string and r7 std::bad_alloc::~bad_alloc, and the
 * throw site in operator new builds the exception with exactly those two values.
 * So: an allocation failed, operator new threw bad_alloc, nothing in KOTOR
 * catches it, and terminate called abort. It happened 13s into loading
 * tar_m02aa, after the run had already loaded tar_m02af successfully -- so it is
 * cumulative pressure, not one oversized area.
 *
 * The game reaches our C++ runtime because dynlib.c binds _Znwj/_Znaj to it, and
 * GCC's operator new is a retry loop around malloc:
 *
 *     while ((p = malloc(sz)) == 0) {
 *       handler = std::get_new_handler();
 *       if (!handler) throw bad_alloc();      <-- what killed log140
 *       handler();
 *     }
 *
 * That loop is the supported place to intervene, and it covers new[] too since
 * _Znaj tail-calls _Znwj. We install a handler that hands back the decoded-audio
 * cache -- pure speed, worst case the next createSound decodes again -- and lets
 * the allocation retry. If there is nothing left to give it removes itself, so
 * the allocation throws exactly as it does today rather than spinning forever on
 * a handler that cannot make progress.
 *
 * log141 then answered the question that motivated the telemetry, and answered
 * it against the obvious suspect. The fatal allocation was 1 MB with 62.8 MB
 * free across 3994 blocks and the arena pinned at the 192 MB cap, while the
 * audio cache held 3.6 MB. So this is fragmentation against a ceiling, not
 * exhaustion, and not our cache: purging bought two earlier saves and could not
 * possibly have helped the third. See docs/specs/2026-08-02-heap-fragmentation.md.
 *
 * What is measured here now is aimed at choosing the fix. `headroom` is sbrk
 * room plus free-in-arena; `largest block` is what the heap can actually still
 * serve, which mallinfo will not tell you and which separates "barely short"
 * from "hopeless"; and the large-allocation census says whether megabyte
 * requests are constant or occasional -- constant makes their own churn the
 * shredder and a size-classed recycler the fix, occasional means the damage is
 * being done by something else entirely.
 */

#include <vitasdk.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "heap.h"
#include "audio_patch.h"
#include "bigalloc.h"
#include "config.h"
#include "log.h"

volatile unsigned g_heap_last_new = 0;

/* std::set_new_handler. Declared by mangled name for the same reason dynlib.c
 * takes _Znwj that way: this is a C translation unit and the symbol is C++. */
extern void *_ZSt15set_new_handlerPFvvE(void (*)(void));
#define set_new_handler _ZSt15set_new_handlerPFvvE

#define HEAP_LIMIT_BYTES ((unsigned)MEMORY_NEWLIB_MB * 1024u * 1024u)

/* Tenths of a MB, so the log can show one decimal without pulling in float
 * formatting. */
static unsigned tenths_mb(unsigned bytes) {
  return (unsigned)(((uint64_t)bytes * 10u) / (1024u * 1024u));
}

/* ---- large-allocation census ----------------------------------------------
 * log141 killed the run on a 1 MB request while 62.8 MB sat free in 3994
 * pieces. What it cannot say is whether requests that size are constant or
 * occasional, and that decides the fix: a few of them means something else is
 * shredding the heap and recycling them would achieve nothing, while hundreds
 * of them makes the churn itself the shredder and a size-classed recycler the
 * answer. Counting is nearly free -- a branch and an increment on allocations
 * we already funnel through dynlib.c -- so count rather than guess again.
 *
 * Classes are powers of two from 256 KB up; anything smaller is ignored, since
 * it is not what fails. */
#define BIG_MIN_SHIFT 18                       /* 256 KB */
#define BIG_CLASSES   8                        /* 256K, 512K, 1M, 2M, 4M, 8M, 16M, 32M+ */
static unsigned g_big_count[BIG_CLASSES];
static unsigned g_big_peak[BIG_CLASSES];       /* largest request seen in class */

void heap_note_alloc(unsigned n) {
  if (n < (1u << BIG_MIN_SHIFT)) return;
  int c = 0;
  unsigned v = n >> BIG_MIN_SHIFT;
  while (v > 1 && c < BIG_CLASSES - 1) { v >>= 1; c++; }
  g_big_count[c]++;
  if (n > g_big_peak[c]) g_big_peak[c] = n;
}

static void heap_log_big(void) {
  char buf[224];
  unsigned pos = 0;
  int any = 0;
  for (int c = 0; c < BIG_CLASSES && pos + 32 < sizeof buf; c++) {
    if (!g_big_count[c]) continue;
    unsigned kb = (1u << (BIG_MIN_SHIFT + c)) / 1024u;
    int w = snprintf(buf + pos, sizeof buf - pos, "%s%uK+:%u(max %uK)",
                     any ? "  " : "", kb, g_big_count[c], g_big_peak[c] / 1024u);
    if (w <= 0) break;
    pos += (unsigned)w;
    any = 1;
  }
  if (any) log_printf("[heap] large allocations so far: %s", buf);
}

/* Largest block the heap can still hand out, which mallinfo does not report and
 * which is the number that separates "barely short" from "hopeless". Descending
 * powers of two: at most a dozen malloc/free pairs, each returned immediately,
 * so the probe reuses one block rather than leaving a mark. Deliberately NOT
 * run every sample -- see heap_log's caller.
 *
 * Two things this got wrong the first time, both visible in logs 143-144:
 *
 * 1. The whole function was compiled away. GCC's allocation DCE (on at -O2)
 *    deletes a malloc/free pair whose result is never otherwise used, and then
 *    folds the `if (p)` to taken -- so this reduced to `return 32u << 20` with
 *    no call left in it. Every "largest block: 32.0 MB" line in both logs is a
 *    compile-time constant, printed even at startup with a 0.0 MB arena, and
 *    printed unchanged 0.5 ms after a 4 MB operator new had failed. The `asm`
 *    barrier below makes `p` opaque, which is what stops the pairing.
 *
 * 2. Starting the ladder at 32 MB measures the wrong thing when the arena is
 *    small. A request the free list cannot serve goes to sbrk, so a low arena
 *    answers "32 MB" via fresh memory and says nothing about contiguity, while
 *    briefly growing the arena for real -- and the arena ratchet is the thing
 *    we are chasing. So cap the ladder at what is already free inside the
 *    arena: above that, only sbrk could answer, and sbrk room is `headroom`,
 *    which heap_log already reports. */
static unsigned heap_largest_block(unsigned ceiling) {
  unsigned start = 4096;
  while (start < (32u << 20) && (start << 1) <= ceiling) start <<= 1;

  for (unsigned sz = start; sz >= 4096; sz >>= 1) {
    void *p = malloc(sz);
    /* Load-bearing: without it the compiler deletes the malloc/free pair and
     * this function becomes a constant. See (1) above. */
    __asm__ volatile("" : "+r"(p) : : "memory");
    if (p) { free(p); return sz; }
  }
  return 0;
}

/* heap_log plus the two figures that cost something to obtain. Kept off the 3s
 * path because the probe allocates: cheap, but not free, and not something to
 * do to a struggling heap two hundred times a run. */
void heap_log_full(const char *why) {
  heap_log(why);
  struct mallinfo mi = mallinfo();
  unsigned freed = (unsigned)mi.fordblks;
  unsigned big = heap_largest_block(freed);
  log_printf("[heap] %slargest block the heap can still serve: %u.%u MB (%u KB) "
             "-- of %u.%u MB free, probe ceiling %u KB",
             why ? why : "", tenths_mb(big) / 10, tenths_mb(big) % 10, big / 1024u,
             tenths_mb(freed) / 10, tenths_mb(freed) % 10, freed / 1024u);
  heap_log_big();
}

void heap_log(const char *why) {
  struct mallinfo mi = mallinfo();
  unsigned arena = (unsigned)mi.arena;
  unsigned used  = (unsigned)mi.uordblks;
  unsigned freed = (unsigned)mi.fordblks;
  /* Room the heap could still take from sbrk, plus what is already free inside
   * it. This is the only figure that says whether we are actually close. */
  unsigned headroom = (arena < HEAP_LIMIT_BYTES ? HEAP_LIMIT_BYTES - arena : 0) + freed;
  unsigned pcm = audio_cache_bytes();
  /* keepcost is the top chunk: the free run at the end of the arena, and the
   * only one sbrk can extend. It is the one contiguity figure mallinfo gives
   * away for nothing, so it costs a field here rather than a probe. */
  unsigned top = (unsigned)mi.keepcost;

  log_printf("[heap] %s%u.%u MB used, %u.%u free in arena (%d blocks, top %u KB), "
             "arena %u.%u of %u MB -> headroom %u.%u MB; audio cache %u.%u MB",
             why ? why : "", tenths_mb(used) / 10, tenths_mb(used) % 10,
             tenths_mb(freed) / 10, tenths_mb(freed) % 10, mi.ordblks, top / 1024u,
             tenths_mb(arena) / 10, tenths_mb(arena) % 10,
             (unsigned)MEMORY_NEWLIB_MB,
             tenths_mb(headroom) / 10, tenths_mb(headroom) % 10,
             tenths_mb(pcm) / 10, tenths_mb(pcm) % 10);
  /* The pool's own occupancy belongs next to this, not on its own schedule:
   * the two numbers only mean anything read together. */
  bigalloc_log(why);
}

static void heap_new_handler(void) {
  log_printf("[heap] ALLOCATION FAILED (last new through dynlib was %u bytes)",
             g_heap_last_new);
  heap_log_full("at failure: ");
  // An uncaught bad_alloc reaches newlib's UDF-based abort, which emulators may not
  // dispatch through our CPU-fault handler. Flush here so the request size and
  // allocator state survive even when the process exits before the normal
  // time-based log drain.
  log_flush();

  unsigned got = audio_cache_purge();
  if (got) {
    log_printf("[heap] released %u.%u MB of decoded audio -- retrying",
               tenths_mb(got) / 10, tenths_mb(got) % 10);
    heap_log_full("after purge: ");
    log_flush();
    return;                       /* operator new loops and tries malloc again */
  }

  /* Returning without freeing anything would spin operator new forever, which
   * is worse than the crash. Stand down and let it throw, as it did before. */
  log_printf("[heap] nothing left to release -- operator new will throw "
             "bad_alloc and the game does not catch it");
  log_flush();
  set_new_handler(NULL);
}

void heap_init(void) {
  set_new_handler(heap_new_handler);
  heap_log_full("startup: ");
}
