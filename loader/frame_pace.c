/* frame_pace.c -- a 30 FPS cap (default) and vsync.
 *
 * vitaGL's display callback sets the new framebuffer for the next vblank, then
 * waits vsync_interval vblanks (vgl.c, gxm.c): 1 by default, so frames are
 * paced to 60 Hz. The game's own options never reached it: V-Sync goes through
 * wglSwapIntervalEXT to SDL_GL_SetSwapInterval, which the loader stubs, and
 * LockFramerate only lets WinMain sleep up to limitFPS, a constant 60.0.
 *
 * By default (FRAME_CAP_30) the interval is 2: each frame is shown for two
 * vblanks, 30 FPS at most. Light scenes ran at 34-37 FPS, unevenly; heavy ones
 * are below 30 anyway. The display queue holds one buffer less than vitaGL
 * has, so the GL worker, and the game behind it, wait for the display at that
 * rate. ux0:data/kotor2/fps_cap.txt containing 0 removes the cap; then
 * [Graphics Options] in swkotor2.ini decides:
 *   LockFramerate=1  -> interval 2 again (the game's in-game toggle,
 *                       g_bFrameRateLocked, is followed while playing);
 *   V-Sync=0         -> interval 0: no wait for the vblank;
 *   otherwise        -> interval 1.
 * Vsync off measured no different (hardware A/B, PERFORMANCE.md 5.15): the
 * display queue already absorbs the vblank wait below 60 FPS.
 * SDL_GL_SetSwapInterval calls from the game are logged, not obeyed: the game
 * makes them from paths other than its options too.
 *
 * Test builds (VSYNC_AB): vsync on and off in alternate windows of
 * VSYNC_AB_WINDOW_S seconds, one "[vsync-ab]" line per window. */

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ini_defaults.h"
#include "frame_pace.h"

extern uint32_t vsync_interval;              /* vitaGL (vgl.c) */

static volatile const unsigned char *s_locked; /* the engine's g_bFrameRateLocked */
static int s_cap = FRAME_CAP_30, s_ini_lock, s_vsync = 1, s_applied = -1;

static void apply(int interval, const char *why) {
  if (interval == s_applied) return;
  s_applied = interval;
  vsync_interval = (uint32_t)interval;
  log_printf("[vsync] %s: %s", why,
             interval == 2 ? "vsync on, 30 FPS at most" : interval ? "vsync on, no cap" : "vsync off, no cap");
}

#if !VSYNC_AB
static int wanted(void) {
  int lock = s_cap || (s_locked ? *s_locked != 0 : s_ini_lock);
  return lock ? 2 : s_vsync ? 1 : 0;
}
#endif

#if VSYNC_AB
#define MAX_FRAMES 1024
static uint64_t s_t0, s_prev;
static int s_window = -1;
static unsigned s_n;
static uint32_t s_us[MAX_FRAMES];
static uint64_t s_sum;

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

static void window_report(void) {
  if (s_window < 0 || s_n < 20) return;
  unsigned n = s_n < MAX_FRAMES ? s_n : MAX_FRAMES, slow = 0;
  for (unsigned i = 0; i < n; i++) slow += s_us[i] >= 50000;
  qsort(s_us, n, sizeof s_us[0], cmp_u32);
  log_printf("[vsync-ab] window %d %s: %u frames, frame mean %.1f ms p50 %.1f p90 %.1f p99 %.1f, %u of 50 ms or more",
             s_window, (s_window & 1) ? "B off" : "A on", s_n, s_sum / 1000.0 / s_n, s_us[n / 2] / 1000.0,
             s_us[n * 9 / 10] / 1000.0, s_us[n * 99 / 100] / 1000.0, slow);
}
#endif

void frame_pace_frame(uint64_t swap_end_us) {
#if VSYNC_AB
  if (s_prev) {
    uint64_t d = swap_end_us - s_prev;
    if (!s_t0) { s_t0 = swap_end_us; s_window = 0; }
    if (s_n < MAX_FRAMES) s_us[s_n] = (uint32_t)d;
    s_n++;
    s_sum += d;
    int w = (int)((swap_end_us - s_t0) / (VSYNC_AB_WINDOW_S * 1000000ull));
    if (w != s_window) {
      window_report();
      s_window = w;
      s_n = 0;
      s_sum = 0;
      apply((w & 1) ? 0 : 1, "A/B window");
    }
  }
  s_prev = swap_end_us;
#else
  (void)swap_end_us;
  if (s_locked) apply(wanted(), "game option");
#endif
}

int frame_pace_swap_interval(int interval) {
  static unsigned s_logged;
  if (s_logged++ < 8) log_printf("[vsync] the game asked for swap interval %d (not applied; see frame_pace.c)", interval);
  return 0;
}

void frame_pace_init(void) {
  SceUID fd = sceIoOpen(DATA_PATH "/fps_cap.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char c = 0; if (sceIoRead(fd, &c, 1) == 1 && (c == '0' || c == '1')) s_cap = c - '0'; sceIoClose(fd); }
  s_ini_lock = ini_graphics_int("LockFramerate", 0) != 0;
  s_vsync = ini_graphics_int("V-Sync", 1) != 0;
  s_locked = (volatile const unsigned char *)so_symbol(&kotor_mod, "g_bFrameRateLocked");
#if VSYNC_AB
  log_printf("[vsync-ab] armed: vsync on and off in alternate %d s windows (ini LockFramerate=%d V-Sync=%d ignored)",
             VSYNC_AB_WINDOW_S, s_ini_lock, s_vsync);
  apply(1, "A/B start");
#else
  /* The engine sets g_bFrameRateLocked from the ini when it loads its options;
   * until then the ini value stands. */
  apply(s_cap || s_ini_lock ? 2 : s_vsync ? 1 : 0,
        s_cap ? "default (fps_cap.txt with 0 removes the cap)" : "fps_cap.txt 0, swkotor2.ini");
#endif
}
