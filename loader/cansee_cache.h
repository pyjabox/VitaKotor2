/* cansee_cache.h -- short-lived CSWCCreature::CanSee result cache (cansee_cache.c). */

#ifndef __CANSEE_CACHE_H__
#define __CANSEE_CACHE_H__

#include <stdint.h>

void cansee_cache_install(void);
void cansee_cache_set(int on);
void cansee_cache_on_swap(void);
void cansee_cache_stats(uint32_t *hits, uint32_t *misses);

#endif
