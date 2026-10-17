/* asl_worker.h -- ASLgl, gles2-bc and vitaGL on the GL worker (asl_worker.c). */
#ifndef ASL_WORKER_H
#define ASL_WORKER_H

#include <stdint.h>

/* Takes over every ASLgl function slot (aslw_mode.txt). After every other
 * installer that hooks ASLgl, so their hooks stay in the chain. */
void aslw_install(void);

/* Once per frame, at the swap: the A/B, an [aslw] report every 10 s. */
void aslw_on_swap(void);

/* The loader drew by itself: the state copies no longer hold. */
void aslw_forget(void);

/* fn(arg) where ASLgl runs: recorded and run on the worker in order with the
 * engine's GL calls when ASLgl is there, else now. For loader code that
 * reads or sets GL state ASLgl's calls also reach. */
void aslw_run(void (*fn)(uint32_t), uint32_t arg);

/* What engine code reaches when it calls the ASLgl function `mangled`: its
 * recorder when ASLW is installed, else fallback (ASLgl itself). For loader
 * code that calls ASLgl by address. */
void *aslw_entry(const char *mangled, void *fallback);

/* -1 not installed, 1 ASLgl on the GL worker, 0 on the game thread. */
int aslw_phase(void);
/* Time into the current 10 s report (and A/B) window; -1 if none. */
int64_t aslw_window_age_us(void);

#endif
