/* frame_pace.h -- vsync and the 30 FPS cap from swkotor2.ini (frame_pace.c). */

#ifndef __FRAME_PACE_H__
#define __FRAME_PACE_H__

#include <stdint.h>

/* After the engine is loaded, before the game thread starts. */
void frame_pace_init(void);
/* gl_patch_on_swap, every presented frame. */
void frame_pace_frame(uint64_t swap_end_us);
/* The game's SDL_GL_SetSwapInterval (sdl_patch.c): logged only. */
int frame_pace_swap_interval(int interval);

#endif
