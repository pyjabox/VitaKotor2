/* threads.h -- which CPU core each thread runs on, and how busy each one is
 *
 * Ported from the KOTOR I loader (VitaKotor v0.4.1, threads.c). The Vita gives
 * an app three cores (USER_0..2). The game's loop lives on one thread inside
 * libkotor2.so and cannot be split, so that core sets the frame rate; our GL
 * worker owns core 2. With pinning on:
 *  - the game thread has core 0 to itself;
 *  - the audio mixer sits on core 1, a step above default priority (it sleeps
 *    in sceAudioOutOutput nearly all the time, so it costs its neighbours
 *    little and a busy worker cannot make it late);
 *  - the game's own workers (pthread_create and SDL_CreateThread, both through
 *    our shims) alternate between cores 2 and 1;
 *  - the GL worker, the parked main thread and vitaGL's garbage collector stay
 *    on core 2.
 *
 * The census records every thread we start or see started, so the log can say
 * which core is busy and with what ([cpu] lines). THREAD_PINNING_AB test builds
 * switch every recorded thread between that layout and the one before it
 * (v0.5.5: game on cores 0-1, everything else where the kernel put it) every
 * 10 s, so one session compares them.
 */

#ifndef THREADS_H
#define THREADS_H

#include <psp2/kernel/cpu.h>
#include <psp2/types.h>

#define CPU_GAME   SCE_KERNEL_CPU_MASK_USER_0   /* SDL_main, nothing else */
#define CPU_AUX_A  SCE_KERNEL_CPU_MASK_USER_1   /* audio mixer, half the workers */
#define CPU_AUX_B  SCE_KERNEL_CPU_MASK_USER_2   /* GL worker, parked main, GC, the rest */

/* The core mask to create a helper thread with: `mask` when pinning is on,
 * 0 (the kernel default) when THREAD_PINNING is off. */
int thread_mask(int mask);

/* The next core for a game worker, alternating CPU_AUX_B and CPU_AUX_A. */
int thread_next_worker_mask(void);

/* Move the CALLING thread to `mask` (a no-op with pinning off) and record it
 * under `tag`, remembering the mask it had before for the A/B. */
void thread_pin_self(int mask, const char *tag);

/* Record a thread we created. `pin_mask`/`pin_prio` are its pinned layout,
 * `old_mask`/`old_prio` the one before (0 = leave as is). */
void thread_census_add_ex(SceUID thid, const char *tag, int pin_mask, int pin_prio,
                          int old_mask, int old_prio);
static inline void thread_census_add(SceUID thid, const char *tag) {
  thread_census_add_ex(thid, tag, 0, 0, 0, 0);
}

/* THREAD_PINNING_AB: apply the pinned layout (1) or the old one (0) to every
 * recorded thread. */
void thread_layout_apply(int pinned);
int thread_layout_pinned(void);

/* Log every recorded thread with a share of the last window: core, priority,
 * load, plus a per-core total. The first call only takes the baseline. */
void thread_census_log(void);

#endif
