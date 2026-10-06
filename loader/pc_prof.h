/* pc_prof.h -- statistical PC profiler for the engine (pc_prof.c). */

#ifndef __PC_PROF_H__
#define __PC_PROF_H__

/* The game thread, at its start (after crash_init): self-test, then sample. */
void pc_prof_start(void);

#endif
