/* bigalloc.h -- a separate arena for the allocations that shred the heap.
 *
 * See bigalloc.c for why this exists. The short version: everything at or above
 * BIGALLOC_MIN_BYTES is served from memblocks of our own instead of newlib's
 * arena, and `free`/`operator delete` tell our pointers from newlib's by an
 * address-range check.
 */
#ifndef BIGALLOC_H
#define BIGALLOC_H

#include <stddef.h>

/* Reserve nothing yet -- segments are taken on first demand. Safe to call twice. */
void bigalloc_init(void);

/* Pool allocation. Returns NULL if the request is small, the pool is full, or
 * no segment could be taken: every caller must fall back to malloc on NULL. */
void *bigalloc(size_t n);

/* True if p came from bigalloc. Cheap enough for the free path: a bounds test
 * per live segment, of which there are at most BIGALLOC_MAX_SEGS. */
int bigalloc_owns(const void *p);

/* Only ever call on a pointer bigalloc_owns(). */
void bigfree(void *p);

/* Payload size of a pool block, for realloc's copy. 0 if not ours. */
size_t bigalloc_size(const void *p);

/* Threshold-aware helpers: pool when it can, newlib when it cannot. These are
 * what callers should use; they are safe for any size and any pointer. */
void *big_malloc(size_t n);
void  big_free(void *p);
void *big_realloc(void *p, size_t n);

/* One line of pool stats, emitted alongside the heap sample. */
void bigalloc_log(const char *why);

#endif
