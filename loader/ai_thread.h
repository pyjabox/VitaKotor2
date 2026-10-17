/* ai_thread.h -- run the server AI on core 1 while the scene is drawn (ai_thread.c). */
#ifndef AI_THREAD_H
#define AI_THREAD_H

/* Reads ux0:data/kotor2/ai_thread.txt and takes over the relocation slots.
 * Call after every other installer that patches engine slots. */
void ai_thread_install(void);

/* Once per frame, at the swap: decides whether the next frame's drawing may
 * run the AI, and logs a [aithread] line every 10 s. */
void ai_thread_on_swap(void);

#endif
