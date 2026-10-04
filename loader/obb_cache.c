/* obb_cache.c -- keep repeated archive reads in RAM.
 *
 * In steady gameplay the game thread blocked on about one 70-100 KB read of
 * main.obb per frame, 10-39 ms on the memory card (hardware run
 * real-vita-layer2-20261003). Most came from CVirtualMachineInternal::RunScript:
 * every run of a script (k_ai_master runs ~8 times a second) reads the compiled
 * script from the archive again, although the engine's resource cache had room.
 *
 * This caches reads of the shared, read-only .obb handles by exact (archive,
 * offset, length). A read is admitted the second time it is seen, so one-off
 * reads (textures and models while loading, which the engine keeps itself) do
 * not churn the cache. Items up to OBB_CACHE_MAX_ITEM_KB, OBB_CACHE_MB in all,
 * least recently used out first. The archives never change while mounted, so
 * nothing needs invalidating; obb_cache_drop forgets a closed archive.
 *
 * Callers serialise every call (dynlib.c holds io_lock). */

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "obb_cache.h"

#if OBB_CACHE_MB

#define ENTRIES 2048u           /* cached items at most */
#define BUCKETS 2048u
#define GHOSTS 4096u            /* reads seen once, waiting for a second sight */
#define NIL 0xffffu

typedef struct {
  const void *file;
  long pos, len;
  uint8_t *data;
  uint16_t prev, next;          /* LRU list, most recent at s_head */
  uint16_t hnext;               /* hash chain */
} entry_t;

static entry_t s_e[ENTRIES];
static uint16_t s_bucket[BUCKETS];
static uint16_t s_free = NIL, s_head = NIL, s_tail = NIL;
static uint32_t s_ghost[GHOSTS];
static size_t s_bytes;
static int s_ready, s_on = 1;
static obb_cache_stats_t s_st;

static uint32_t key_hash(const void *file, long pos, long len) {
  uint32_t h = (uint32_t)(uintptr_t)file * 2654435761u;
  h ^= (uint32_t)pos * 2246822519u;
  h ^= (uint32_t)len * 3266489917u;
  return h ^ (h >> 15);
}

static void init(void) {
  for (unsigned i = 0; i < BUCKETS; i++) s_bucket[i] = NIL;
  for (unsigned i = 0; i < ENTRIES; i++) s_e[i].next = (uint16_t)(i + 1 < ENTRIES ? i + 1 : NIL);
  s_free = 0;
  s_ready = 1;
}

static void lru_unlink(uint16_t i) {
  entry_t *e = &s_e[i];
  if (e->prev != NIL) s_e[e->prev].next = e->next; else s_head = e->next;
  if (e->next != NIL) s_e[e->next].prev = e->prev; else s_tail = e->prev;
}

static void lru_push_front(uint16_t i) {
  entry_t *e = &s_e[i];
  e->prev = NIL;
  e->next = s_head;
  if (s_head != NIL) s_e[s_head].prev = i;
  s_head = i;
  if (s_tail == NIL) s_tail = i;
}

static void remove_entry(uint16_t i) {
  entry_t *e = &s_e[i];
  uint16_t *pp = &s_bucket[key_hash(e->file, e->pos, e->len) % BUCKETS];
  while (*pp != NIL && *pp != i) pp = &s_e[*pp].hnext;
  if (*pp == i) *pp = e->hnext;
  lru_unlink(i);
  s_bytes -= (size_t)e->len;
  free(e->data);
  memset(e, 0, sizeof *e);
  e->next = s_free;
  s_free = i;
  s_st.items--;
}

static uint16_t find(const void *file, long pos, long len) {
  for (uint16_t i = s_bucket[key_hash(file, pos, len) % BUCKETS]; i != NIL; i = s_e[i].hnext)
    if (s_e[i].file == file && s_e[i].pos == pos && s_e[i].len == len) return i;
  return NIL;
}

int obb_cache_get(const void *file, long pos, void *dst, long len) {
  if (!s_on || len <= 0) return 0;
  if (!s_ready) init();
  uint16_t i = find(file, pos, len);
  if (i == NIL) return 0;
  memcpy(dst, s_e[i].data, (size_t)len);
  lru_unlink(i);
  lru_push_front(i);
  s_st.hits++;
  s_st.hit_bytes += (uint32_t)len;
  return 1;
}

void obb_cache_put(const void *file, long pos, const void *src, long len) {
  if (!s_on || len <= 0 || len > (long)OBB_CACHE_MAX_ITEM_KB * 1024) return;
  if (!s_ready) init();
  s_st.misses++;
  uint32_t h = key_hash(file, pos, len);
  uint32_t *g = &s_ghost[h % GHOSTS];
  if (*g != (h | 1u)) { *g = h | 1u; return; }      /* first sight: remember only */
  *g = 0;
  if (find(file, pos, len) != NIL) return;
  while (s_tail != NIL && (s_free == NIL || s_bytes + (size_t)len > (size_t)OBB_CACHE_MB << 20)) {
    remove_entry(s_tail);
    s_st.evictions++;
  }
  if (s_free == NIL) return;
  uint8_t *data = malloc((size_t)len);
  if (!data) return;
  memcpy(data, src, (size_t)len);
  uint16_t i = s_free;
  entry_t *e = &s_e[i];
  s_free = e->next;
  e->file = file;
  e->pos = pos;
  e->len = len;
  e->data = data;
  e->hnext = s_bucket[h % BUCKETS];
  s_bucket[h % BUCKETS] = i;
  lru_push_front(i);
  s_bytes += (size_t)len;
  s_st.items++;
  s_st.admits++;
}

void obb_cache_drop(const void *file) {
  if (!s_ready) return;
  for (uint16_t i = 0; i < ENTRIES; i++)
    if (s_e[i].data && s_e[i].file == file) remove_entry(i);
}

void obb_cache_set(int on) { s_on = on; }

void obb_cache_stats(obb_cache_stats_t *out) {
  s_st.bytes = (uint32_t)s_bytes;
  *out = s_st;
}

#else

int obb_cache_get(const void *file, long pos, void *dst, long len) {
  (void)file; (void)pos; (void)dst; (void)len;
  return 0;
}
void obb_cache_put(const void *file, long pos, const void *src, long len) {
  (void)file; (void)pos; (void)src; (void)len;
}
void obb_cache_drop(const void *file) { (void)file; }
void obb_cache_set(int on) { (void)on; }
void obb_cache_stats(obb_cache_stats_t *out) { memset(out, 0, sizeof *out); }

#endif
