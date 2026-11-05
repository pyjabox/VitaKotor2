/* ai_thread.h -- run the server AI on core 1 while the scene is drawn (ai_thread.c). */
#ifndef AI_THREAD_H
#define AI_THREAD_H

/* Reads ux0:data/kotor2/ai_thread.txt and takes over the relocation slots.
 * Call after every other installer that patches engine slots. */
void ai_thread_install(void);

/* Once per frame, at the swap: decides whether the next frame's drawing may
 * run the AI, and logs a [aithread] line every 10 s. */
void ai_thread_on_swap(void);

/* For diagnostics: 1 on the AI worker; the worker's time spent waiting at
 * gates since boot, in microseconds (BATTLE_LEDGER builds; 0 otherwise). */
#include <stdint.h>
int ai_thread_on_worker(void);
uint64_t ai_thread_idle_us(void);
/* Game thread: the time spent drawing the scene with the AI beside it, plus
 * waiting for the AI after it, since boot (us). */
uint64_t ai_thread_render_us(void);
/* Game thread, inside the drawing, with no animation in progress (between two
 * Gob::Animate calls, after ManageSceneBSP): makes the call the AI worker is
 * waiting on, if any (AI_SERVE). */
void ai_thread_safe_point(void);

#endif
