/* obb_cache.h -- repeated .obb reads served from RAM (obb_cache.c). Every call
 * must be serialised by the caller. */

#ifndef __OBB_CACHE_H__
#define __OBB_CACHE_H__

#include <stdint.h>

typedef struct {
  uint32_t hits, hit_bytes;     /* reads served from RAM */
  uint32_t misses;              /* reads that went to the card (cacheable size) */
  uint32_t admits, evictions;
  uint32_t items, bytes;        /* held now */
} obb_cache_stats_t;

/* 1 and dst filled if (file, pos, len) is cached. */
int obb_cache_get(const void *file, long pos, void *dst, long len);
/* After a card read that missed: admitted on its second sight. */
void obb_cache_put(const void *file, long pos, const void *src, long len);
/* Forget everything read from file (the archive was closed). */
void obb_cache_drop(const void *file);
/* Runtime switch (A/B tests); on by default. */
void obb_cache_set(int on);
void obb_cache_stats(obb_cache_stats_t *out);

#endif
