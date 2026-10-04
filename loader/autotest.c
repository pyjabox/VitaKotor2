/* autotest.c -- scripted controller input for unattended Vita3K smoke tests.
 *
 * macOS will not deliver synthetic keystrokes to Vita3K, so a test build drives
 * the game itself: the link wraps sceCtrlPeekBufferPositive2 (Vita SDL's pad
 * read) and ORs scripted buttons and stick positions into what the hardware
 * reports. Nothing else in the input path changes.
 *
 * Script, reconstructed from a user-driven run that loaded 101PER
 * (diagnostics/vita3k-manual-loaded-autosave-then-stuck-20260919):
 *   main menu GUI opened  -> wait, D-pad down, Cross        (Load Game)
 *   saveload GUI opened   -> wait, D-pad down x N, Cross     (load entry N)
 *   3D area rendering     -> >=100 draws/frame for 180 frames
 *   play                  -> walk, turn, tap Cross, for AUTOTEST_PLAY_S
 *   done                  -> "[autotest] DONE", stop injecting
 * Every transition is logged with the [autotest] tag. */

#include <vitasdk.h>
#include <string.h>

#include "config.h"
#include "autotest.h"
#include "gl_patch.h"
#include "log.h"

#if AUTOTEST_ENABLE

#ifndef AUTOTEST_SAVE_DOWNS
#define AUTOTEST_SAVE_DOWNS 2
#endif
#ifndef AUTOTEST_PLAY_S
#define AUTOTEST_PLAY_S 120
#endif

enum {
  S_WAIT_MENU, S_MENU_INPUT, S_WAIT_SAVELIST, S_SAVE_INPUT, S_LOADING, S_PLAY,
  S_DONE, S_FAIL
};
static const char *const k_state[] = {
  "wait-menu", "menu-input", "wait-savelist", "save-input", "loading", "play",
  "done", "fail"
};

static int s_state = S_WAIT_MENU;
static uint64_t s_t0_us, s_state_us;
static unsigned s_frames_in_state, s_retries;
static uint32_t s_draws_at_play;
/* Gameplay detector: the loading screen and menus draw a few dozen calls per
 * frame, a 3D area well over a hundred. Count consecutive frames above the
 * threshold; module file opens are not a reliable signal (save loads read the
 * module through the resource manager, not stdio). */
static uint32_t s_prev_draws;
static unsigned s_busy_frames;
#define GAMEPLAY_DRAWS_PER_FRAME 100
#define GAMEPLAY_FRAMES 180
static volatile uint64_t s_menu_us, s_saveload_us;

/* One step: hold `buttons` and the sticks for hold_ms, then release for gap_ms. */
typedef struct { uint32_t buttons; uint8_t lx, ly, rx, ry; uint16_t hold_ms, gap_ms; } step_t;
#define QMAX 32
static step_t s_q[QMAX];
static unsigned s_q_head, s_q_tail;
static uint64_t s_step_us;
static int s_step_phase;          /* 0 idle, 1 holding, 2 gap */

/* What the pad wrapper injects right now. Written and read on the game thread
 * (SDL pumps the joystick from SDL_PumpEvents, which the game calls). */
static volatile uint32_t s_btn;
static volatile uint8_t s_lx = 128, s_ly = 128, s_rx = 128, s_ry = 128;

static void enqueue(uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry,
                    uint16_t hold_ms, uint16_t gap_ms) {
  unsigned next = (s_q_tail + 1) % QMAX;
  if (next == s_q_head) return;
  s_q[s_q_tail] = (step_t){ buttons, lx, ly, rx, ry, hold_ms, gap_ms };
  s_q_tail = next;
}
static void tap(uint32_t buttons) { enqueue(buttons, 128, 128, 128, 128, 160, 600); }
static int queue_idle(void) { return s_q_head == s_q_tail && s_step_phase == 0; }

static void set_inject(uint32_t b, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
  s_btn = b; s_lx = lx; s_ly = ly; s_rx = rx; s_ry = ry;
}

static void run_queue(uint64_t now) {
  if (s_step_phase == 0 && s_q_head != s_q_tail) {
    step_t *st = &s_q[s_q_head];
    set_inject(st->buttons, st->lx, st->ly, st->rx, st->ry);
    s_step_phase = 1;
    s_step_us = now;
  }
  if (s_step_phase == 1 && now - s_step_us >= (uint64_t)s_q[s_q_head].hold_ms * 1000u) {
    set_inject(0, 128, 128, 128, 128);
    s_step_phase = 2;
    s_step_us = now;
  }
  if (s_step_phase == 2 && now - s_step_us >= (uint64_t)s_q[s_q_head].gap_ms * 1000u) {
    s_q_head = (s_q_head + 1) % QMAX;
    s_step_phase = 0;
  }
}

static void go(int state, uint64_t now, const char *why) {
  log_printf("[autotest] %s -> %s at %llu.%03llu s (%s)", k_state[s_state], k_state[state],
             (unsigned long long)((now - s_t0_us) / 1000000u),
             (unsigned long long)(((now - s_t0_us) / 1000u) % 1000u), why);
  s_state = state;
  s_state_us = now;
  s_frames_in_state = 0;
}

/* Continuous play pattern, 10 s period: walk forward, turn right, tap Cross
 * (advances dialogue, opens doors), walk back, turn left. */
static void play_pattern(uint64_t now) {
  unsigned ms = (unsigned)(((now - s_state_us) / 1000u) % 10000u);
  if (ms < 3000)       set_inject(0, 128, 0, 128, 128);
  else if (ms < 4500)  set_inject(0, 128, 128, 255, 128);
  else if (ms < 5000)  set_inject(0, 128, 128, 128, 128);
  else if (ms < 5160)  set_inject(SCE_CTRL_CROSS, 128, 128, 128, 128);
  else if (ms < 5500)  set_inject(0, 128, 128, 128, 128);
  else if (ms < 8000)  set_inject(0, 128, 255, 128, 128);
  else if (ms < 9500)  set_inject(0, 128, 128, 0, 128);
  else                 set_inject(0, 128, 128, 128, 128);
}

static void tick(uint64_t now) {
  if (!s_t0_us) {
    s_t0_us = s_state_us = now;
    log_printf("[autotest] armed: save entry %d, play %d s", AUTOTEST_SAVE_DOWNS,
               AUTOTEST_PLAY_S);
  }
  if (s_state != S_PLAY) run_queue(now);

  switch (s_state) {
  case S_WAIT_MENU:
    if (s_menu_us && now - s_menu_us > 6000000u) {
      tap(SCE_CTRL_DOWN);
      tap(SCE_CTRL_CROSS);
      go(S_MENU_INPUT, now, "main menu up");
    } else if (now - s_t0_us > 300000000u) {
      go(S_FAIL, now, "main menu never opened");
    }
    break;
  case S_MENU_INPUT:
    if (queue_idle()) go(S_WAIT_SAVELIST, now, "Load Game pressed");
    break;
  case S_WAIT_SAVELIST:
    if (s_saveload_us && s_saveload_us >= s_state_us - 2000000u &&
        now - s_saveload_us > 4000000u) {
      for (int i = 0; i < AUTOTEST_SAVE_DOWNS; i++)
        enqueue(SCE_CTRL_DOWN, 128, 128, 128, 128, 160, 450);
      tap(SCE_CTRL_CROSS);
      go(S_SAVE_INPUT, now, "save list up");
    } else if (now - s_state_us > 20000000u) {
      if (++s_retries > 2) { go(S_FAIL, now, "save list never opened"); break; }
      tap(SCE_CTRL_CIRCLE);     /* back out of whatever opened instead */
      tap(SCE_CTRL_CIRCLE);
      s_menu_us = now;          /* wait again, then retry Down + Cross */
      go(S_WAIT_MENU, now, "retry");
    }
    break;
  case S_SAVE_INPUT:
    if (queue_idle()) go(S_LOADING, now, "save chosen");
    break;
  case S_LOADING:
    if (now - s_state_us > 10000000u && s_busy_frames >= GAMEPLAY_FRAMES) {
      s_draws_at_play = gl_patch_total_draws();
      go(S_PLAY, now, "3D area rendering");
    } else if (now - s_state_us > 150000000u) {
      go(S_FAIL, now, "no 3D area after choosing the save");
    }
    break;
  case S_PLAY:
    play_pattern(now);
    if (now - s_state_us > (uint64_t)AUTOTEST_PLAY_S * 1000000u) {
      set_inject(0, 128, 128, 128, 128);
      log_printf("[autotest] DONE played=%d s frames=%u draws=%u",
                 AUTOTEST_PLAY_S, s_frames_in_state,
                 gl_patch_total_draws() - s_draws_at_play);
      go(S_DONE, now, "play finished");
      log_flush(); /* let short-run monitors stop promptly, not on a later flush */
    }
    break;
  default:
    set_inject(0, 128, 128, 128, 128);
    break;
  }
}

void autotest_on_swap(uint64_t now_us) {
  uint32_t draws = gl_patch_total_draws();
  if (draws - s_prev_draws >= GAMEPLAY_DRAWS_PER_FRAME) s_busy_frames++;
  else s_busy_frames = 0;
  s_prev_draws = draws;
  s_frames_in_state++;
  tick(now_us);
}

void autotest_note_open(const char *path) {
  if (!path) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!s_menu_us && strstr(path, "mainmenu")) s_menu_us = now;
  if (strstr(path, "saveload")) s_saveload_us = now;
}

int __real_sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count);
int __wrap_sceCtrlPeekBufferPositive2(int port, SceCtrlData *pad_data, int count) {
  int r = __real_sceCtrlPeekBufferPositive2(port, pad_data, count);
  static unsigned logged = 0;
  if (logged < 4) {
    logged++;
    log_printf("[autotest] pad read port=%d count=%d -> %d", port, count, r);
  }
  if (r > 0 && pad_data) {
    if (s_t0_us) tick(sceKernelGetProcessTimeWide());
    for (int i = 0; i < r && i < count; i++) {
      pad_data[i].buttons |= s_btn;
      if (s_lx != 128 || s_ly != 128) { pad_data[i].lx = s_lx; pad_data[i].ly = s_ly; }
      if (s_rx != 128 || s_ry != 128) { pad_data[i].rx = s_rx; pad_data[i].ry = s_ry; }
    }
  }
  return r;
}

#endif /* AUTOTEST_ENABLE */
