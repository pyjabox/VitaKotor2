/* loadscreen.h -- progress bar covering startup.
 * Historical timings below are inherited from VitaKotor (KOTOR I), not KOTOR II
 * validation.
 *
 * The game draws nothing at all until its first draw call, which log124 put at
 * 69.3s -- after even the "Main Menu" analytics string at 56.1s. So the console
 * shows a black screen for over a minute and the loader is the only thing that
 * can say otherwise.
 *
 * An earlier version tracked only the archive mount. Once the mount got fast
 * (95s -> 2s via the replay cache) that bar filled at 10s and then sat at 100%
 * for another 59 seconds, which is worse than showing nothing: it claims to be
 * finished when it is not. This version spans begin -> first game draw.
 *
 * Progress is elapsed time against how long the LAST boot took, persisted to
 * DATA_PATH/startup.tim, kept separately for a warm and a cold archive
 * cache because those differ by about a minute. It is an estimate and is
 * clamped below 100% until the handoff actually happens -- a bar that stalls
 * near the end is honest; one that sits full is not.
 *
 * Drawn from whichever thread called loadscreen_begin(), never a helper thread:
 * issuing GL from a second thread caused an earlier hang in this port.
 *
 * With LOADSCREEN_ART the bar is dressed in the game's own loading-screen art:
 * a load_*.tga read straight out of the KOTOR II patch OBB (a stored ZIP entry,
 * to wait for the mount) with the bar drawn into the groove the art already
 * has. Every step of that can fail back to the plain bar.
 *
 * The art draws ONLY while the loader owns GL outright. loadscreen_tick() is
 * reached from the GLLOG macro, which sits at the top of every hooked GL
 * function -- so it runs INSIDE the game's own GL calls, between its
 * glBindBuffer and its glDrawElements. Scissor+clear survives that; a textured
 * quad does not. vitaGL's glVertexPointer stores whatever array buffer is
 * currently bound alongside the pointer it is given, so with a game VBO bound
 * our stack address becomes an offset into that VBO and the GPU fetches
 * geometry from a wrong address. On hardware that wedged GXM at 25.1s and
 * needed a hard power cycle; the screen had already gone black at ~22.5s, when
 * the game created its first framebuffer objects.
 *
 * So loadscreen_note_gl() freezes everything the moment the game issues any GL
 * of its own. The last swapped frame stays on screen -- a still loading screen,
 * which is the right picture with a stopped bar -- and from then on the tick
 * only records what the game had bound, for whoever tries to keep drawing for
 * the whole boot later. */

#ifndef __LOADSCREEN_H__
#define __LOADSCREEN_H__

/* Take over the screen. `warm` says whether the archive replay cache was
 * available, which selects the duration estimate. */
void loadscreen_begin(int warm);

/* Draw a frame if due. Cheap, throttled, safe to call from any hot path; it
 * no-ops off-thread, before begin, and after end. */
void loadscreen_tick(void);

/* Hand the screen to the game. Called from the first draw call. Records how
 * long this boot actually took, so the next one estimates better. */
void loadscreen_end(void);

/* 1 between begin and end. Lets the GL layer avoid work while we own the screen. */
int loadscreen_active(void);

/* The game has issued a GL call of its own. Called from GLLOG in place of
 * loadscreen_tick(): it freezes the screen the first time, then samples the
 * game's GL bindings a bounded number of times. Never draws. */
void loadscreen_note_gl(void);

#endif
