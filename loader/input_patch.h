/* input_patch.h -- Vita physical-controller setup and diagnostics. */
#ifndef INPUT_PATCH_H
#define INPUT_PATCH_H

// Disable both touch panels after SDL video initialization.
void input_init(void);

// Sample raw pad health once per rendered frame; this does not feed the game.
void input_probe_pump(void);

// Sample raw pad health. Call on a fixed clock alongside sdl_input_census.
void input_probe_census(void);

#endif
