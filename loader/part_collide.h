/* part_collide.h -- particles' collision tests (PART_COLLIDE): see part_collide.c. */
#ifndef PART_COLLIDE_H
#define PART_COLLIDE_H
/* Takes its relocation slots; call with the other installers in main(),
 * before the call rewrite (plt_direct) runs. */
void part_collide_install(void);
/* Every frame (PART_COLLIDE_STATS: a [partcol] line every 10 s). */
void part_collide_on_swap(void);
#endif
