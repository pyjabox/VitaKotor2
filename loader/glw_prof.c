/* glw_prof.c -- where the game thread's frame goes, with the GL worker on or
 * off (GL_WORKER_PROFILE; diagnostic builds only).
 *
 * Markers, two stores each, say what the game thread is doing:
 *  - glw_prof_inner: set by every vitaGL wrapper (gl_worker.c, glw_gen.c).
 *    1 a GL call, 2 a draw, 3 waiting for a blocking call, 4 the swap.
 *    With the worker off (mode 0) that is vitaGL itself; with it on (mode 2)
 *    it is the recording.
 *  - s_outer: inside ASLgl's draw entry points (glDrawElements, glDrawArrays,
 *    glDrawRangeElements, glEnd), whose PLT slots are taken here. Time there
 *    outside any wrapper is gles2-bc's per-draw preparation.
 *  - glw_prof_worker: the worker sleeping (0), polling (1) or executing (2).
 *
 * A sampler thread on core 2 (priority above the worker's) reads them every
 * ~200 us together with the game thread's scheduler status (running, ready =
 * preempted, waiting). Each 10 s report gives ms per frame per bin, plus the
 * game thread's and the worker's run clocks from sceKernelGetThreadInfo. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "gl_worker.h"
#include "gl_worker_internal.h"

#if GL_WORKER_PROFILE

/* Separate lines: the game thread writes one, the worker the other. */
glw_mark_t glw_prof_inner, glw_prof_worker;
static glw_mark_t s_outer;           /* the game thread writes it per draw */
static SceUID s_game = -1, s_worker = -1;

/* ---- ASLgl draw entry points ---------------------------------------------------- */
static void (*o_draw_elements)(unsigned, int, unsigned, const void *);
static void (*o_draw_arrays)(unsigned, int, int);
static void (*o_draw_range)(unsigned, unsigned, unsigned, int, unsigned, const void *);
static void (*o_end)(void);

static void h_draw_elements(unsigned m, int n, unsigned t, const void *i) {
  uint8_t o = s_outer.v; s_outer.v = 1; o_draw_elements(m, n, t, i); s_outer.v = o;
}
static void h_draw_arrays(unsigned m, int f, int n) {
  uint8_t o = s_outer.v; s_outer.v = 1; o_draw_arrays(m, f, n); s_outer.v = o;
}
static void h_draw_range(unsigned m, unsigned a, unsigned b, int n, unsigned t, const void *i) {
  uint8_t o = s_outer.v; s_outer.v = 1; o_draw_range(m, a, b, n, t, i); s_outer.v = o;
}
static void h_end(void) {
  uint8_t o = s_outer.v; s_outer.v = 1; o_end(); s_outer.v = o;
}

/* Same as gl_state_filter.c: point every relocation slot of sym that holds the
 * engine's own function at repl; returns the original, 0 if nothing changed. */
static uintptr_t take_slots(const char *sym, uintptr_t repl, unsigned *n_out) {
  uintptr_t original = so_symbol(&kotor_mod, sym), callable = 0;
  unsigned n = 0, bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *s = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + s->st_name, sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0 && (!n || bad)) { if (n_out) *n_out = 0; return 0; }
  }
  if (n_out) *n_out = n;
  return callable;
}

void glw_prof_install(void) {
  struct { const char *sym; void *hook; void **orig; } k[] = {
    {"_ZN5ASLgl14glDrawElementsEjijPKv", (void *)h_draw_elements, (void **)&o_draw_elements},
    {"_ZN5ASLgl12glDrawArraysEjii", (void *)h_draw_arrays, (void **)&o_draw_arrays},
    {"_ZN5ASLgl19glDrawRangeElementsEjjjijPKv", (void *)h_draw_range, (void **)&o_draw_range},
    {"_ZN5ASLgl5glEndEv", (void *)h_end, (void **)&o_end},
  };
  char buf[96];
  int o = 0;
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
    uintptr_t f = so_symbol(&kotor_mod, k[i].sym);
    *k[i].orig = (void *)f;
    unsigned n = 0;
    if (f) {
      uintptr_t orig = take_slots(k[i].sym, (uintptr_t)k[i].hook, &n);
      if (orig) *k[i].orig = (void *)orig;
    }
    o += snprintf(buf + o, sizeof buf - o, " %u", n);
  }
  log_printf("[glw:prof] ASLgl draw slots%s (drawElements drawArrays drawRange end)", buf);
}

/* ---- sampler -------------------------------------------------------------------- */
enum { ST_RUN, ST_READY, ST_WAIT, ST_N };
#define INNER_N 5
static uint32_t s_hist[2][INNER_N][ST_N];   /* [outer][inner][game status] */
static uint32_t s_whist[3];                 /* worker sleeping, polling, executing */
static uint32_t s_samples;
static volatile uint32_t s_reset;           /* report -> sampler: start a new window */

static int sampler(SceSize args, void *argp) {
  (void)args; (void)argp;
  SceKernelThreadInfo ti;
  for (;;) {
    sceKernelDelayThread(200);
    if (s_reset) {
      memset(s_hist, 0, sizeof s_hist);
      memset(s_whist, 0, sizeof s_whist);
      s_samples = 0;
      __sync_synchronize();
      s_reset = 0;
    }
    unsigned outer = s_outer.v ? 1u : 0u, inner = glw_prof_inner.v, w = glw_prof_worker.v;
    if (inner >= INNER_N) inner = 0;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    if (sceKernelGetThreadInfo(s_game, &ti) < 0) continue;
    unsigned st = ti.status == SCE_THREAD_RUNNING ? ST_RUN : ti.status == SCE_THREAD_READY ? ST_READY : ST_WAIT;
    s_hist[outer][inner][st]++;
    if (w < 3) s_whist[w]++;
    s_samples++;
  }
  return 0;
}

static uint64_t run_clocks(SceUID th) {
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  return th >= 0 && sceKernelGetThreadInfo(th, &ti) >= 0 ? ti.runClocks : 0;
}

void glw_prof_start(void) {
  s_game = sceKernelGetThreadId();
  SceUID th = sceKernelCreateThread("glw_prof", sampler, 100, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
  int rc = th >= 0 ? sceKernelStartThread(th, 0, NULL) : th;
  log_printf("[glw:prof] sampler started (rc 0x%x), game thid 0x%08x, worker thid 0x%08x", rc,
             (unsigned)s_game, (unsigned)s_worker);
}

void glw_prof_set_worker(int thid) { s_worker = thid; }

#define MS(x) (unsigned)((x) / 1000u), (unsigned)((x) / 100u % 10u)

void glw_prof_report(unsigned frames, unsigned long long window_us) {
  static uint64_t last_game, last_worker;
  uint64_t g = run_clocks(s_game), wk = run_clocks(s_worker);
  uint64_t dg = last_game ? g - last_game : 0, dw = last_worker ? wk - last_worker : 0;
  last_game = g;
  last_worker = wk;
  uint32_t n = s_samples, f = frames ? frames : 1;
  if (!n) { s_reset = 1; log_printf("[glw:prof] no samples this window"); return; }
  /* us per frame for a bin of c samples */
#define BIN(c) ((uint64_t)(c) * window_us / n / f)
  uint32_t run_engine = s_hist[0][0][ST_RUN], run_xdraw = s_hist[1][0][ST_RUN];
  uint32_t run_call = s_hist[0][1][ST_RUN] + s_hist[1][1][ST_RUN];
  uint32_t run_call_in_draw = s_hist[1][1][ST_RUN];
  uint32_t run_draw = s_hist[0][2][ST_RUN] + s_hist[1][2][ST_RUN];
  uint32_t run_sync = s_hist[0][3][ST_RUN] + s_hist[1][3][ST_RUN];
  uint32_t run_swap = s_hist[0][4][ST_RUN] + s_hist[1][4][ST_RUN];
  uint32_t ready = 0, wait_sync = 0, wait_swap = 0, wait_other = 0;
  for (int o = 0; o < 2; o++) {
    for (int i = 0; i < INNER_N; i++) {
      ready += s_hist[o][i][ST_READY];
      if (i == 3) wait_sync += s_hist[o][i][ST_WAIT];
      else if (i == 4) wait_swap += s_hist[o][i][ST_WAIT];
      else wait_other += s_hist[o][i][ST_WAIT];
    }
  }
  uint32_t ws = s_whist[0] + s_whist[1] + s_whist[2];
  log_printf("[glw:prof] mode %d, %u frames, %u.%u ms/frame, %u samples | game running: engine %u.%u, "
             "gles2-bc draw prep %u.%u, GL calls %u.%u (%u.%u under draws), GL draws %u.%u, sync %u.%u, swap %u.%u "
             "| ready %u.%u | waiting: sync %u.%u, swap %u.%u, other %u.%u | game cpu %u.%u ms/frame",
             glw_ro.mode, f, MS(window_us / f), n, MS(BIN(run_engine)), MS(BIN(run_xdraw)), MS(BIN(run_call)),
             MS(BIN(run_call_in_draw)), MS(BIN(run_draw)), MS(BIN(run_sync)), MS(BIN(run_swap)), MS(BIN(ready)),
             MS(BIN(wait_sync)), MS(BIN(wait_swap)), MS(BIN(wait_other)), MS(dg / f));
  if (glw_ro.mode == 2 && ws)
    log_printf("[glw:prof] worker: executing %u.%u, polling %u.%u, sleeping %u.%u ms/frame | worker cpu %u.%u ms/frame",
               MS((uint64_t)s_whist[2] * window_us / ws / f), MS((uint64_t)s_whist[1] * window_us / ws / f),
               MS((uint64_t)s_whist[0] * window_us / ws / f), MS(dw / f));
#undef BIN
  s_reset = 1;
}

#endif
