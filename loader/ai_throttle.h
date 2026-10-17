/* ai_throttle.h -- server AI time spent on what is in combat or moving
 * (see ai_throttle.c). */

#ifndef __AI_THROTTLE_H__
#define __AI_THROTTLE_H__

/* Main thread, after libkotor2 is relocated. `period`: idle objects below
 * level 4 are updated every `period` frames (0 keeps AI_THROTTLE_PERIOD).
 * `ab`: test builds, alternates with the engine's own behaviour every 10 s. */
void ai_throttle_install(unsigned period, int ab);
/* Once per frame, game thread. Also writes the [aithrottle] line every 10 s. */
void ai_throttle_on_swap(void);

/* The object UpdateState last got from GetNextObject (diagnostics). */
extern void *volatile ai_throttle_cur_obj;

#endif
