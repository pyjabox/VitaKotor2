/* gl_state_filter.h -- redundant GL state calls skipped before ASLgl
 * (gl_state_filter.c). */

#ifndef __GL_STATE_FILTER_H__
#define __GL_STATE_FILTER_H__

#include <stdint.h>

/* Main thread, after libkotor2 is relocated. */
void gl_state_filter_install(void);
/* 0 off, 1 on, 2 verify (counts, never skips). */
void gl_state_filter_set_mode(int mode);
int gl_state_filter_mode(void);
/* The loader is about to draw with vitaGL by itself: drop the filter's copy. */
void gl_state_filter_forget(void);
/* Game thread, every presented frame. */
void gl_state_filter_on_swap(uint64_t frame_us);
/* Per-frame call/redundancy/skip counts since the last call (then reset). */
void gl_state_filter_stats(char *buf, int cap, unsigned frames);

#endif
