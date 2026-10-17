/* path_cache.h -- failed path-point searches remembered (path_cache.c). */
#ifndef PATH_CACHE_H
#define PATH_CACHE_H

/* Takes CSWSArea::ClosestPathPoint and NoNonWalkPolysDetailed (path_mode.txt). */
void path_cache_install(void);

/* At the swap: forgets everything after a load; the A/B and a [path] line every 10 s. */
void path_cache_on_swap(void);

/* -1 not installed, 1 on, 0 off (the A/B's other half). */
int path_cache_phase(void);

#endif
