/* sdl_patch.h -- SDL2 resolution for the KOTOR Android build
 *
 * libKOTOR imports 39 SDL2 functions (window/GL context, input, threading,
 * timing, RWops) plus a couple of Android-only globals. We satisfy them with
 * Vita-native SDL2 (linked, vitaGL-backed) -- "SDL2 owns context creation" --
 * with a few hooks (force Vita geometry, translate RWops paths).
 */

#ifndef __SDL_PATCH_H__
#define __SDL_PATCH_H__

#include <stdint.h>

#include "so_util.h"

// Resolver entries this layer contributes (SDL_* funcs + g_SDL_BufferGeometry_*).
const so_default_dynlib *sdl_get_dynlib(void);
extern const int sdl_dynlib_size;
uintptr_t sdl_lookup_symbol(const char *name);

// Read an entire SDL_RWops into a fresh NUL-terminated buffer and close it.
// Returns a malloc'd buffer (caller owns it; see AurResGet_hook -- the game must
// never free it) or NULL on failure, in which case the RWops is still closed.
// *out_len receives the byte count. Lives here so SDL headers stay in this layer.
void *sdl_slurp_rwops_close(void *rwops, unsigned int *out_len);

// Open `name` and slurp it whole, going through SDL_RWFromFile so both the path
// translation and the OBB fallback below apply. Returns a malloc'd buffer the
// caller owns (free it), or NULL. *out_len receives the byte count.
void *sdl_load_file(const char *name, unsigned int *out_len);

// Arm the OBB fallback for SDL_RWFromFile: any read-mode open that is not on the
// card is retried against the mounted archives via the companion's
// ObbFile::RWFromFile. Call once, after mountObb/mountPatchObb have run. Pass the
// resolved libandroid_port addresses of ObbFile::RWFromFile(const char*),
// g_mainObb and g_patchObb; a NULL disables the fallback rather than faulting.
void sdl_obb_fallback_init(uintptr_t rwfromfile, uintptr_t mainobb, uintptr_t patchobb);

// Point the gamepad probe at libKOTOR's own button state so the JOYBUTTON log
// line can report what the press actually did. `pressed` and `this_frame` are
// pressedGamepadButtons / pressedGamepadButtonsThisFrame; `map` is
// gamepadButtonById (a libc++ __tree: begin@0, root@4, size@8). Any of them may
// be 0, in which case that figure is simply omitted. Call once after libKOTOR
// is resolved.
void sdl_gamepad_probe_init(uintptr_t pressed, uintptr_t this_frame, uintptr_t map);

// The game's current held-button mask, for periodic sampling. 0 if unavailable.
unsigned sdl_gamepad_mask(void);

// Per-type counts of the events the game actually consumed since the previous
// call. Drive it from a clock, not from event volume: the failure we are chasing
// is events NOT arriving, and a volume-triggered summary goes quiet exactly then.
void sdl_input_census(void);

typedef struct {
  unsigned delay_calls;
  uint64_t delay_requested_us, delay_actual_us;
  unsigned delay_max_us;
} sdl_perf_t;

// Cumulative SDL_Delay timing used by the frame-hitch trace.
void sdl_perf_snapshot(sdl_perf_t *out);

#endif
