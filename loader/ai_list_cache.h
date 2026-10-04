/* ai_list_cache.h -- skip repeated lookups of non-creatures in the AI master's
 * walk pass (see ai_list_cache.c). */

#ifndef __AI_LIST_CACHE_H__
#define __AI_LIST_CACHE_H__

#include <stdint.h>

/* Main thread, after libkotor2 is relocated. */
void ai_list_cache_install(void);
/* Enable or disable the fast path at runtime (installed hook stays). */
void ai_list_cache_set(int on);
/* Once per frame, game thread: invalidates the cache. */
void ai_list_cache_on_swap(void);
/* Calls since the last read: fast-path hits, walk-pass misses, other callers. */
void ai_list_cache_stats(uint32_t *hits, uint32_t *misses, uint32_t *other);

#endif
