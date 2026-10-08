/* input_patch.c -- front touchscreen alongside the physical controls.
 *
 * Gameplay input comes from Vita SDL's joystick backend (sdl_patch.c maps its
 * button indices to the Android ordering KOTOR expects) and, on top of that,
 * from the FRONT touchscreen through Vita SDL's own touch backend
 * (VITA_PollTouch). That backend is what the game expects from Android: finger
 * events, which the game's event filter tracks, plus the mouse events SDL
 * synthesises from them, which is how the game's GUI is clicked
 * (MSG_Mac::ProcessEvent) -- so taps work on menus, dialogue and the HUD while
 * every button keeps working. The game is still told it has no touchscreen
 * (jni_patch.c, ASPYR.HasTouchScreen), so it keeps its controller layout.
 *
 * The REAR panel never reaches the game: its sampling is stopped, SDL is told
 * not to poll it (VITA_DISABLE_TOUCH_BACK), and SDL's reads of it come back
 * empty (__wrap_sceTouchPeek). While the on-screen keyboard is up, the front
 * panel reads empty too: the taps are aimed at the keyboard, and passing them
 * on would also press whatever GUI control sits underneath. SDL sees every
 * finger lift, so nothing is left held down.
 *
 * ux0:data/kotor2/touch_mode.txt containing 0 turns the front panel off as
 * well (controls only, as in v0.4.2).
 */

#include <vitasdk.h>
#include <SDL2/SDL.h>
#include <string.h>

#include "config.h"
#include "ime_patch.h"
#include "input_patch.h"
#include "log.h"

int __real_sceTouchPeek(SceUInt32 port, SceTouchData *data, SceUInt32 n);

static int s_front_on = 1;

/* Raw-hardware witnesses, reported alongside the SDL event census. */
static unsigned s_samples = 0;
static unsigned s_rstick_samples = 0;
static unsigned s_rstick_max = 0;
static unsigned s_touch_samples = 0;     /* front panel reads with a finger on it */
static unsigned s_blanked = 0;           /* ... withheld because the keyboard is up */

static void set_sampling(void) {
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                           s_front_on ? SCE_TOUCH_SAMPLING_STATE_START : SCE_TOUCH_SAMPLING_STATE_STOP);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
}

void input_early_init(void) {
  char c = 0;
  SceUID fd = sceIoOpen(DATA_PATH "/touch_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    if (sceIoRead(fd, &c, 1) != 1) c = 0;
    sceIoClose(fd);
  }
  s_front_on = c != '0';
  /* Read by VITA_InitTouch when the game initialises SDL video. */
  SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
  if (!s_front_on) SDL_setenv("VITA_DISABLE_TOUCH_FRONT", "1", 1);
  /* The GUI is clicked with the mouse events SDL synthesises from touches,
   * and SDL 2.32 makes them only for the panel this hint names. Unset, it
   * names none: SDL_VitaTouchMouseDeviceChanged ignores a NULL value and
   * leaves the device at 0, which matches neither panel (front is touch id 1,
   * the hint's "0"). Fingers then reach the game -- they steer the character
   * -- but no tap ever clicks a menu or a target. */
  SDL_SetHint(SDL_HINT_VITA_TOUCH_MOUSE_DEVICE, "0");
  set_sampling();
}

void input_init(void) {
  /* SDL video initialisation starts sampling on both panels; stop the rear
   * one again now that it has run. */
  set_sampling();
  log_printf("[touch] front touch %s, rear touch off%s", s_front_on ? "on" : "OFF",
             s_front_on ? "" : " (touch_mode.txt)");
}

/* Vita SDL's touch backend is the only caller (libSDL2.a, linked with
 * -Wl,--wrap=sceTouchPeek). */
int __wrap_sceTouchPeek(SceUInt32 port, SceTouchData *data, SceUInt32 n) {
  int r = __real_sceTouchPeek(port, data, n);
  if (r <= 0 || !data) return r;
  int keep = port == SCE_TOUCH_PORT_FRONT && s_front_on;
  for (int i = 0; i < r; i++) {
    if (!data[i].reportNum) continue;
    if (keep && ime_dialog_active()) {
      s_blanked++;
      data[i].reportNum = 0;
    } else if (keep) {
      s_touch_samples++;
    } else {
      data[i].reportNum = 0;
    }
  }
  return r;
}

void input_probe_pump(void) {
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) > 0) {
    int dx = (int)pad.rx - 128, dy = (int)pad.ry - 128;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    unsigned d = (unsigned)(dx > dy ? dx : dy);
    s_samples++;
    if (d > 32) {
      s_rstick_samples++;
      if (d > s_rstick_max) s_rstick_max = d;
    }
  }
}

void input_probe_census(void) {
  log_printf("[input] raw pad: %u samples, rstick off-centre %u (peak %u/127)",
             s_samples, s_rstick_samples, s_rstick_max);
  /* Kept in release logs, so only for windows the panel was used in. */
  if (s_touch_samples || s_blanked)
    log_printf("[touch] front panel: %u reads touching, %u withheld for the keyboard",
               s_touch_samples, s_blanked);
  s_samples = s_rstick_samples = s_rstick_max = s_touch_samples = s_blanked = 0;
}
