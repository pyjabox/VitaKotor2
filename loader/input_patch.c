/* input_patch.c -- raw Vita controller diagnostics.
 *
 * Gameplay input itself comes from Vita SDL's joystick backend. sdl_patch.c
 * normalizes those button indices to the Android SDL ordering KOTOR expects.
 * Both touch panels stay disabled so all game input is physical controls only.
 */

#include <vitasdk.h>
#include <string.h>

#include "input_patch.h"
#include "log.h"

void input_init(void) {
  /* SDL video initialization can enable touch sampling, so assert this after
   * the game creates its window as well as during process startup. */
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
  log_printf("[input] physical-controls-only mode; front and rear touch disabled");
}

/* Raw-hardware witness, reported alongside the SDL event census. */
static unsigned s_samples = 0;
static unsigned s_rstick_samples = 0;
static unsigned s_rstick_max = 0;

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
  s_samples = s_rstick_samples = s_rstick_max = 0;
}
