/* leaf_fast.h -- faster versions of small functions the engine calls a lot (leaf_fast.c). */
#ifndef LEAF_FAST_H
#define LEAF_FAST_H

#include <stdint.h>

/* Takes slerp's and _stricmp's slots; after so_resolve. */
void leaf_fast_install(void);
void leaf_fast_on_swap(void);

/* The engine's imports (dynlib.c); fminf/fmaxf softfp in and out. */
int leaf_tolower(int c);
int leaf_toupper(int c);
uint32_t leaf_fminf(uint32_t a, uint32_t b);
uint32_t leaf_fmaxf(uint32_t a, uint32_t b);

#endif
