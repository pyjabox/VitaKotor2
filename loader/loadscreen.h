/* loadscreen.h -- the game's own loading screen, shown while the game boots.
 *
 * KOTOR II draws nothing until its first frame, about 25 s into a boot on
 * hardware (the legal screen), so the console otherwise sits on a black
 * screen. From the moment vitaGL is up (about 4.6 s), the loader draws the
 * screen the game shows between areas: one of its load_* pictures, the
 * KOTOR II logo, LOADING in its box, the progress bar and a gameplay hint,
 * all read from the game's own data (k2res.c) and laid out as
 * loadscreen_p.gui does.
 *
 * Every GL call runs on the GL thread through glw_call, in order with
 * everything else, and leaves the default state behind: nothing the GL worker
 * keeps a copy of changes underneath it.
 *
 * The screen only draws while the loader owns GL outright. The first GL call
 * of the game's own (loadscreen_note_gl, from GLLOG; about 19 s in) freezes
 * it: the last frame stays up until the game's first frame. The bar estimates
 * from how long that took on the previous boot (DATA_PATH/startup.tim), for
 * a warm and a cold archive index separately, and never claims 100% before
 * the freeze. Each piece that fails to load is left out; with no picture the
 * screen is black with the rest on it. */

#ifndef __LOADSCREEN_H__
#define __LOADSCREEN_H__

/* Take over the screen, on the game thread once vitaGL is up. `warm`: the
 * archive replay index exists, which sets the duration estimate. */
void loadscreen_begin(int warm);

/* Draw a frame if due. Cheap, throttled, safe from any hot path; a no-op off
 * the owning thread, before begin, after the freeze and after end. */
void loadscreen_tick(void);

/* The game's first frame: record this boot's timing. Issues no GL. */
void loadscreen_end(void);

/* 1 between begin and end. */
int loadscreen_active(void);

/* The game has issued a GL call of its own (GLLOG): the first one freezes the
 * screen and frees everything it loaded. */
void loadscreen_note_gl(void);

#endif
