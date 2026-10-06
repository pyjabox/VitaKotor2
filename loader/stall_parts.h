/* stall_parts.h -- game-thread time outside the engine, per slow frame
 * (stall_parts.c). */

#ifndef __STALL_PARTS_H__
#define __STALL_PARTS_H__

#include <stdint.h>

enum { SP_OPEN, SP_READ, SP_SEEK, SP_CLOSE, SP_GLSYNC, SP_TEXUP, SP_TEX16, SP_MIPGEN, SP_SND, SP_N };

/* Any thread; only the game thread's calls count. */
void stall_part(int part, uint32_t us, uint32_t bytes);
/* Game thread, once per presented frame: logs the frame if slow, then resets. */
void stall_parts_frame(uint64_t frame_us, uint64_t end_us);

#endif
