/* bigalloc.c -- a separate arena for large allocations
 * Historical measurements below are inherited from VitaKotor (KOTOR I), not
 * KOTOR II validation.
 *
 * log145 measured the thing three earlier logs could only guess at. The probe
 * in heap.c reports the largest request the heap can still serve; across a
 * 34-minute session it went
 *
 *     32 MB (t=727) -> 16 -> 8 -> 4 -> 2 -> 1 -> 0.5 MB (t=2052)
 *
 * while the free total sat at 45-50 MB the whole time and `used` never moved
 * off ~140 MB. Not a leak, not exhaustion: the free space is intact and the
 * pieces just keep getting smaller, roughly halving every two and a half
 * minutes, with each area load taking a visible step out of it.
 *
 * Both of that session's symptoms are that one curve crossing a request size.
 * At t=1989-2019, with the largest block at 1.0 MB, three dialogue lines wanted
 * 1.1-1.3 MB of PCM and played silent. At t=2062, with it down to 0.5 MB, a
 * 1,326,383-byte `operator new` for the cantina threw bad_alloc and the game
 * does not catch it. 46 MB free at the moment of death.
 *
 * Raising MEMORY_NEWLIB_MB was the other candidate and log145 retires it: at
 * t=1690 the largest block was already down to 4 MB while the arena was 184.6
 * of 192, with 7 MB of sbrk room still unspent. Contiguity decays whether or
 * not there is headroom. What more ceiling buys is time, and the log prices it:
 * live `used` held at ~140 MB while the arena climbed 137 -> 191 MB, so about
 * 1.6 MB/min goes to fragmentation loss -- 37 seconds of play per extra MB.
 *
 * So: keep the big blocks out of the general heap. They are what interleaves
 * with the game's thousands of small long-lived objects, and they are few --
 * 1458 requests at or above 256 KB in the whole session, under one a second.
 *
 * WHY A SEPARATE MEMBLOCK RATHER THAN A FREE LIST INSIDE THE HEAP
 * The hard part of recycling is `free`: it must recognise our pointers without
 * reading in front of a pointer some other allocator produced, which is how
 * this turns into a worse bug than the one it fixes. Owning whole segments
 * makes that a bounds test -- no header read until the range has already said
 * the block is ours, and no table to keep in step. If the pool cannot serve a
 * request we return NULL, the caller falls back to malloc, and the same bounds
 * test then correctly says "not ours" when that block comes back to be freed.
 * Degrading to today's behaviour is always available and always safe.
 *
 * The budget comes out of newlib rather than being added on top: vitaGL takes
 * everything except MEMORY_VITAGL_THRESHOLD_MB, so there is nothing spare to
 * claim. MEMORY_NEWLIB_MB drops by exactly BIGALLOC_MAX_BYTES, leaving the
 * process footprint where it was. That is affordable only because this is the
 * same memory, differently placed: the 46 MB that was free-but-useless at the
 * crash is the budget being moved.
 */

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "bigalloc.h"
#include "config.h"
#include "log.h"

#ifdef BIGALLOC_HOST_TEST
/* The host build swaps the segment source for malloc so the allocator logic --
 * split, coalesce, best fit, the bounds test -- can be exercised off-device. */
#include <stdio.h>
static void *seg_acquire(size_t bytes) { return malloc(bytes); }
static void  lock(void) {}
static void  unlock(void) {}
#else
#include <vitasdk.h>
static SceUID s_mutex = -1;
static void *seg_acquire(size_t bytes) {
  SceUID uid = sceKernelAllocMemBlock("bigalloc", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                      (SceSize)bytes, NULL);
  if (uid < 0) return NULL;
  void *base = NULL;
  if (sceKernelGetMemBlockBase(uid, &base) < 0) { sceKernelFreeMemBlock(uid); return NULL; }
  return base;
}
static void lock(void)   { if (s_mutex >= 0) sceKernelLockMutex(s_mutex, 1, NULL); }
static void unlock(void) { if (s_mutex >= 0) sceKernelUnlockMutex(s_mutex, 1); }
#endif

/* Header on every block, in use or free. 16 bytes keeps payloads 16-aligned,
 * which is what the segment base is and what NEON loads want. `prev` carries
 * the physically preceding block's size so free() can coalesce backwards
 * without a footer. */
typedef struct {
  uint32_t size;    /* total block size including this header */
  uint32_t prev;    /* size of the physically previous block, 0 if first */
  uint32_t inuse;
  uint32_t magic;
} Hdr;

#define HDR_SZ      ((uint32_t)sizeof(Hdr))
#define MAGIC       0xB16A110Cu
#define ALIGN_UP(n) (((n) + 15u) & ~15u)
/* Do not split off a remainder smaller than this: a scrap too small to serve
 * any request this allocator sees is just a hole with bookkeeping on it. */
#define MIN_SPLIT   (HDR_SZ + 64u * 1024u)

typedef struct {
  unsigned char *base;
  unsigned char *end;
} Seg;

static Seg      s_seg[BIGALLOC_MAX_SEGS];
static int      s_nseg;
static int      s_inited;

static size_t   s_live, s_peak;           /* payload bytes handed out */
static unsigned s_nlive, s_nalloc, s_nfallback, s_nsegfail;
/* What the pool turned away, not just how often.
 *
 * "%u fell back to malloc" was a count with no size on it, and sizing the pool
 * from a count is guesswork: 622 refusals in log171 could be 622 x 300 KB or
 * 622 x 4 MB, and those want different budgets. Every refusal here goes to
 * newlib instead, which is the mixing this pool exists to prevent -- log171
 * died with 46.8 MB free and a largest servable block of 512 KB. Record the
 * bytes and the worst single request so the next log states the shortfall
 * rather than implying one. */
static uint32_t s_fb_max;
static uint64_t s_fb_bytes;

static Hdr *hdr_of(void *p)          { return (Hdr *)((unsigned char *)p - HDR_SZ); }
static void *payload_of(Hdr *h)      { return (unsigned char *)h + HDR_SZ; }
static Hdr *next_hdr(const Seg *s, Hdr *h) {
  unsigned char *n = (unsigned char *)h + h->size;
  return n < s->end ? (Hdr *)n : NULL;
}

/* Take one more segment. Called with the lock held. */
static int seg_add(void) {
  if (s_nseg >= BIGALLOC_MAX_SEGS) return 0;
  size_t bytes = (size_t)BIGALLOC_SEG_MB * 1024u * 1024u;
  unsigned char *base = (unsigned char *)seg_acquire(bytes);
  if (!base) { s_nsegfail++; return 0; }

  Seg *s = &s_seg[s_nseg];
  s->base = base;
  s->end  = base + bytes;

  Hdr *h = (Hdr *)base;                    /* one free block spanning it all */
  h->size  = (uint32_t)bytes;
  h->prev  = 0;
  h->inuse = 0;
  h->magic = MAGIC;

  s_nseg++;
  log_printf("[big] segment %d: %u MB at %p", s_nseg, (unsigned)BIGALLOC_SEG_MB, (void *)base);
  return 1;
}

void bigalloc_init(void) {
  if (s_inited) return;
  s_inited = 1;
#ifndef BIGALLOC_HOST_TEST
  s_mutex = sceKernelCreateMutex("bigalloc", 0, 0, NULL);
#endif
  log_printf("[big] pool armed: >=%u KB served from up to %d x %u MB",
             (unsigned)(BIGALLOC_MIN_BYTES / 1024), BIGALLOC_MAX_SEGS,
             (unsigned)BIGALLOC_SEG_MB);
}

/* Best fit across every segment. The population is small by construction --
 * these are all >=256 KB blocks, so a 12 MB segment holds a few dozen -- and
 * best fit is worth its walk here precisely because leaving the wrong-sized
 * remainder behind is the failure mode this whole file exists to avoid. */
static Hdr *find_fit(uint32_t need, Seg **which) {
  Hdr *best = NULL;
  Seg *best_seg = NULL;
  for (int i = 0; i < s_nseg; i++) {
    Seg *s = &s_seg[i];
    for (Hdr *h = (Hdr *)s->base; h; h = next_hdr(s, h)) {
      if (h->inuse || h->size < need) continue;
      if (!best || h->size < best->size) { best = h; best_seg = s; }
      if (best->size == need) break;
    }
  }
  if (best) *which = best_seg;
  return best;
}

static void split(Seg *s, Hdr *h, uint32_t need) {
  if (h->size - need < MIN_SPLIT) return;
  Hdr *rest = (Hdr *)((unsigned char *)h + need);
  rest->size  = h->size - need;
  rest->prev  = need;
  rest->inuse = 0;
  rest->magic = MAGIC;
  h->size = need;

  Hdr *after = next_hdr(s, rest);
  if (after) after->prev = rest->size;
}

void *bigalloc(size_t n) {
  if (!s_inited || n < BIGALLOC_MIN_BYTES) return NULL;
  uint32_t need = (uint32_t)ALIGN_UP(n) + HDR_SZ;
  if (need < n) return NULL;                       /* overflow */

  lock();
  Seg *s = NULL;
  Hdr *h = find_fit(need, &s);
  if (!h && seg_add()) h = find_fit(need, &s);
  if (!h) {
    s_nfallback++;
    s_fb_bytes += need;
    if (need > s_fb_max) s_fb_max = need;
    unlock();
    return NULL;
  }

  split(s, h, need);
  h->inuse = 1;
  /* Account by block, not by request: free() only knows the block, and mixing
   * the two makes `live` drift down over a session until it says nothing. */
  s_live += h->size - HDR_SZ;
  s_nlive++;
  s_nalloc++;
  if (s_live > s_peak) s_peak = s_live;
  unlock();
  return payload_of(h);
}

/* Deliberately unlocked, and safe without one: s_nseg only ever grows, and it
 * is written after the segment it counts is fully initialised. A pointer can
 * only live in a segment that already existed when it was handed out, so a
 * reader racing seg_add either sees that segment or a larger count -- never a
 * half-built one. This runs on every free in the game, so the lock it does not
 * take is the point. */
int bigalloc_owns(const void *p) {
  const unsigned char *q = (const unsigned char *)p;
  for (int i = 0; i < s_nseg; i++)
    if (q >= s_seg[i].base && q < s_seg[i].end) return 1;
  return 0;
}

static Seg *seg_of(const void *p) {
  const unsigned char *q = (const unsigned char *)p;
  for (int i = 0; i < s_nseg; i++)
    if (q >= s_seg[i].base && q < s_seg[i].end) return &s_seg[i];
  return NULL;
}

size_t bigalloc_size(const void *p) {
  if (!bigalloc_owns(p)) return 0;
  const Hdr *h = (const Hdr *)((const unsigned char *)p - HDR_SZ);
  return h->magic == MAGIC ? h->size - HDR_SZ : 0;
}

void bigfree(void *p) {
  if (!p) return;
  Seg *s = seg_of(p);
  if (!s) return;

  lock();
  Hdr *h = hdr_of(p);
  if (h->magic != MAGIC || !h->inuse) {   /* double free or a stray pointer */
    log_printf("[big] BAD FREE %p (magic=%08x inuse=%u)", p, h ? h->magic : 0,
               h ? h->inuse : 0);
    unlock();
    return;
  }
  h->inuse = 0;
  s_nlive--;
  /* s_live is payload as requested, which we no longer have; account by block
   * so the figure stays honest rather than drifting negative. */
  size_t back = h->size - HDR_SZ;
  s_live = s_live > back ? s_live - back : 0;

  Hdr *nx = next_hdr(s, h);
  if (nx && !nx->inuse) {                 /* coalesce forward */
    h->size += nx->size;
    Hdr *after = next_hdr(s, h);
    if (after) after->prev = h->size;
  }
  if (h->prev) {                          /* coalesce backward */
    Hdr *pv = (Hdr *)((unsigned char *)h - h->prev);
    if (!pv->inuse) {
      pv->size += h->size;
      Hdr *after = next_hdr(s, pv);
      if (after) after->prev = pv->size;
    }
  }
  unlock();
}

void *big_malloc(size_t n) {
  void *p = bigalloc(n);
  return p ? p : malloc(n);
}

void big_free(void *p) {
  if (bigalloc_owns(p)) bigfree(p);
  else free(p);
}

void *big_realloc(void *p, size_t n) {
  if (!p) return big_malloc(n);
  size_t old = bigalloc_size(p);
  if (!old) {
    /* newlib's block. Growing past the threshold is the one case worth moving
     * into the pool, since that is exactly the request that would otherwise go
     * looking for a big contiguous run in the general heap. */
    if (n < BIGALLOC_MIN_BYTES) return realloc(p, n);
    /* How much of it may be read is newlib's to say. The first cut copied `n`
     * bytes on the theory that the old block was below the threshold and so
     * shorter -- which reads off the end of it, as ASan pointed out on the very
     * first host run. */
    size_t have = malloc_usable_size(p);
    void *q = bigalloc(n);
    if (!q) return realloc(p, n);
    memcpy(q, p, have < n ? have : n);
    free(p);
    return q;
  }
  if (n <= old) return p;         /* shrink in place; the slack stays in the block */
  void *q = big_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, old);
  bigfree(p);
  return q;
}

void bigalloc_log(const char *why) {
  if (!s_nseg) return;
  size_t cap = (size_t)s_nseg * BIGALLOC_SEG_MB * 1024u * 1024u;
  /* Largest free block, the same figure heap.c reports for newlib and for the
   * same reason: it is what says whether the next big request will land. */
  uint32_t largest = 0;
  unsigned nfree = 0;
  lock();
  for (int i = 0; i < s_nseg; i++)
    for (Hdr *h = (Hdr *)s_seg[i].base; h; h = next_hdr(&s_seg[i], h))
      if (!h->inuse) { nfree++; if (h->size > largest) largest = h->size; }
  unlock();

  log_printf("[big] %s%u/%u MB live in %u blocks (peak %u MB), %u free blocks, "
             "largest %u KB; %u served, %u fell back to malloc "
             "(%u MB total, worst %u KB), %u segment fails",
             why ? why : "", (unsigned)(s_live >> 20), (unsigned)(cap >> 20), s_nlive,
             (unsigned)(s_peak >> 20), nfree, largest / 1024u,
             s_nalloc, s_nfallback,
             (unsigned)(s_fb_bytes >> 20), s_fb_max / 1024u, s_nsegfail);
}
