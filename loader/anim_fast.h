/* anim_fast.h -- the engine's answers to three repeated lookups, faster
 * (ANIM_FAST): see anim_fast.c. */
#ifndef ANIM_FAST_H
#define ANIM_FAST_H
/* Takes its relocation slots; call with the other installers in main(),
 * before the call rewrite (plt_direct) runs. */
void anim_fast_install(void);
/* Every frame: an [animfast] line every 10 s. */
void anim_fast_on_swap(void);
#endif
