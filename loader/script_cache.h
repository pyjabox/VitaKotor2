#ifndef SCRIPT_CACHE_H
#define SCRIPT_CACHE_H

/* Compiled scripts kept in memory between runs (SCRIPT_CACHE). See script_cache.c. */
void script_cache_install(void);
void script_cache_on_swap(void);
/* 1 on, 0 off (A/B window), -1 not installed. */
int script_cache_phase(void);
/* Changes whenever the cache empties (the resource set changed). */
#include <stdint.h>
uint32_t script_cache_generation(void);

#endif
