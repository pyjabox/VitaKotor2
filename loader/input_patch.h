/* input_patch.h -- front touchscreen and physical-controller setup, and
 * diagnostics (see input_patch.c). */
#ifndef INPUT_PATCH_H
#define INPUT_PATCH_H

// At process start, before the game initialises SDL: read touch_mode.txt, tell
// SDL's touch backend to leave the rear panel alone, set panel sampling.
void input_early_init(void);

// After SDL video initialisation (which starts both panels): rear panel off
// again, front on unless touch_mode.txt says 0.
void input_init(void);

// Sample raw pad health once per rendered frame; this does not feed the game.
void input_probe_pump(void);

// Sample raw pad and touch health. Call on a fixed clock alongside
// sdl_input_census.
void input_probe_census(void);

#endif
