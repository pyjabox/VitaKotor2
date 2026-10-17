/* pc_prof.h -- statistical PC profiler for the engine (pc_prof.c). */

#ifndef __PC_PROF_H__
#define __PC_PROF_H__

/* The game thread, at its start (after crash_init): self-test, then sample. */
void pc_prof_start(void);

/* Sample this thread (and walk its stack) instead of the game thread. */
#include <stdint.h>
void pc_prof_retarget(int thid, uintptr_t stk_lo, uintptr_t stk_hi);

#endif
