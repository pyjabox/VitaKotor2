/* occlusion_cull.c -- skip Gobs the GPU says are completely hidden.
 *
 * A Gob is one of the engine's renderable objects: a creature, placeable, door
 * or item. Scene::DoGobBuckets asks Gob::VisibilityCheck about each one, then
 * pays for its light setup and Gob::Render. The engine's own culling is frustum
 * plus a size/line-of-sight test, so in KOTOR II's corridors most of what it
 * renders is behind walls: on the hardware test route 60-80 % of rendered Gobs
 * produced no visible sample, and their Gob::Render CPU was 7-19 ms per frame.
 *
 * Mechanism, entirely through two PLT slots (no code patching):
 *   Gob::Render          every outermost call is bracketed by a
 *                        GL_ANY_SAMPLES_PASSED query, and the draws it issues
 *                        are counted;
 *   Gob::VisibilityCheck the engine's check runs first and unchanged; a Gob our
 *                        queries say is hidden then gets 0, so DoGobBuckets
 *                        skips it exactly as it skips an engine cull.
 * Queries are double-buffered (two frames in flight) and read only once
 * vitaGL's fence says the GPU has finished them -- the CPU never waits. Results
 * are collected at the first VisibilityCheck of a frame, so a decision normally
 * applies the frame after the query.
 *
 * Per-Gob state, keyed by Gob pointer:
 *   VISIBLE  rendered and queried every frame; OCCLUSION_CULL_HIDE_AFTER
 *            consecutive results with no visible sample -> HIDDEN.
 *   HIDDEN   skipped, until its re-test frame -> PENDING. The first re-test is
 *            spread over 1..RETEST frames by address so Gobs hidden together do
 *            not all re-test together; later ones are every RETEST frames.
 *   PENDING  rendered and queried normally until the result arrives: any
 *            visible sample -> VISIBLE, none -> HIDDEN. It keeps rendering while
 *            the result is in flight, so a Gob that came back never blinks.
 * Never skipped: Gobs that issue no draws inside Gob::Render (their parts may be
 * drawn by a later pass, which the query cannot see) and Gobs the engine marks
 * IsAlwaysRendering (gob+0x12). The table is cleared after any frame longer
 * than a second (loads), and a fail-safe turns culling off for the session if
 * nearly every result reads hidden -- what non-functional queries look like.
 *
 * Gob pointers are table keys only. They are dereferenced solely inside the
 * VisibilityCheck hook, where the engine has just handed over a live Gob; a
 * result arriving a frame later may name a Gob that has since been freed. */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitaGL.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "gl_patch.h"
#include "log.h"
#include "occlusion_cull.h"

#if !OCCLUSION_CULL_ENABLE

void occlusion_cull_install(void) {}
void occlusion_cull_on_swap(uint64_t swap_end_us) { (void)swap_end_us; }

#else

#define QN OCCLUSION_CULL_QUERIES_PER_FRAME
#if QN < 1 || 2 * QN > 128
#error "OCCLUSION_CULL_QUERIES_PER_FRAME: two frames of queries must fit vitaGL's 128 slots"
#endif

/* ---- queries ----------------------------------------------------------------- */
typedef struct { GLuint id; const void *gob; uint32_t draws0, draws; } oc_slot_t;
typedef struct { oc_slot_t slot[QN]; unsigned used; int pending; } oc_bank_t;

static oc_bank_t s_bank[2];
static int s_installed;          /* both slots are ours */
static int s_state;              /* 0 queries not created yet, 1 running, -1 off */
static int s_cur = -1;           /* bank taking this frame's queries, -1 none */
static int s_depth;              /* Gob::Render nesting */
static oc_slot_t *s_active;      /* query open around the outermost Render */
static uint32_t s_frame, s_collected_frame = ~0u;
static uint64_t s_prev_swap_us;

/* ---- per-Gob state -------------------------------------------------------------- */
#define CULL_TABLE 1024
#define CULL_PROBE 16
#define CULL_STALE_FRAMES 300u
enum { CS_VISIBLE = 0, CS_HIDDEN = 1, CS_PENDING = 2 };
typedef struct {
  const void *gob;
  uint32_t last_seen, next_retest;
  uint8_t state, streak, empty;
} cull_entry_t;
static cull_entry_t s_cull[CULL_TABLE];
static int s_on = 1;             /* cleared by the fail-safe (and test toggles) */
#if !OCCLUSION_CULL_TEST
static unsigned s_fs_windows;
#endif
static uint32_t s_fs_total, s_fs_hidden;

/* ---- statistics, per log interval ------------------------------------------------ */
static uint64_t s_log_t0;
static uint32_t s_st_frames, s_st_queried, s_st_hidden, s_st_skipped, s_st_retests;
static uint32_t s_st_protected, s_st_over_budget, s_st_behind;

static int (*s_vis_orig)(void *self);
static void (*s_render_orig)(void *self, int recurse);

static inline unsigned cull_hash(const void *p) {
  uint32_t x = (uint32_t)(uintptr_t)p;
  x ^= x >> 5;
  x *= 0x9E3779B1u;
  return (x >> 16) & (CULL_TABLE - 1);
}

static void cull_clear(const char *why) {
  static unsigned logged;
  memset(s_cull, 0, sizeof s_cull);
  if (logged < 4) { logged++; log_printf("[cull] table cleared (%s)", why); }
}

static cull_entry_t *cull_find(const void *gob, int create) {
  unsigned h = cull_hash(gob);
  cull_entry_t *spare = NULL;
  for (unsigned i = 0; i < CULL_PROBE; i++) {
    cull_entry_t *e = &s_cull[(h + i) & (CULL_TABLE - 1)];
    int stale = e->gob && s_frame - e->last_seen > CULL_STALE_FRAMES;
    if (e->gob == gob) {
      if (stale) { e->state = CS_VISIBLE; e->streak = 0; e->empty = 0; }
      return e;
    }
    if (!spare && (!e->gob || stale)) spare = e;
  }
  if (!create || !spare) return NULL;
  memset(spare, 0, sizeof *spare);
  spare->gob = gob;
  spare->last_seen = s_frame;
  return spare;
}

static void cull_note_result(const void *gob, int visible, uint32_t draws) {
#if OCCLUSION_CULL_TEST
  /* Vita3K has no visibility queries: a moving pattern (a quarter of Gobs
   * "hidden", reshuffled every 60 frames) drives the skip and re-test paths. */
  visible = ((cull_hash(gob) + s_frame / 60u) & 3u) != 0;
#endif
  if (draws) {
    s_fs_total++;
    s_st_queried++;
    if (!visible) { s_fs_hidden++; s_st_hidden++; }
  }
  cull_entry_t *e = cull_find(gob, 1);
  if (!e) return;
  e->last_seen = s_frame;
  if (!draws) { e->empty = 1; e->state = CS_VISIBLE; e->streak = 0; return; }
  e->empty = 0;
  if (visible) {
    e->state = CS_VISIBLE;
    e->streak = 0;
  } else if (e->state == CS_PENDING) {
    e->state = CS_HIDDEN;
    e->next_retest = s_frame + OCCLUSION_CULL_RETEST;
  } else if (e->state == CS_VISIBLE && ++e->streak >= OCCLUSION_CULL_HIDE_AFTER) {
    e->state = CS_HIDDEN;
    e->streak = 0;
    e->next_retest = s_frame + 1u + cull_hash(gob) % OCCLUSION_CULL_RETEST;
  }
}

/* Collect a pending bank if every query in it has a result. Returns 1 if the
 * bank is free afterwards. Never waits. */
static int bank_collect(oc_bank_t *b) {
  if (!b->pending) return 1;
  for (unsigned i = 0; i < b->used; i++) {
    GLuint avail = GL_FALSE;
    glGetQueryObjectuiv(b->slot[i].id, GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) return 0;
  }
  for (unsigned i = 0; i < b->used; i++) {
    GLuint any = GL_TRUE;          /* unknown counts as visible */
    glGetQueryObjectuiv(b->slot[i].id, GL_QUERY_RESULT_NO_WAIT, &any);
    if (s_on) cull_note_result(b->slot[i].gob, any ? 1 : 0, b->slot[i].draws);
  }
  b->used = 0;
  b->pending = 0;
  return 1;
}

/* ---- hooks ------------------------------------------------------------------------ */
static void gob_render_hook(void *self, int recurse) {
  if (s_depth++ == 0 && s_state == 1 && s_on) {
    if (s_cur >= 0 && s_bank[s_cur].used < QN) {
      oc_slot_t *slot = &s_bank[s_cur].slot[s_bank[s_cur].used++];
      slot->gob = self;
      slot->draws0 = gl_patch_total_draws();
      slot->draws = 0;
      glBeginQuery(GL_ANY_SAMPLES_PASSED, slot->id);
      s_active = slot;
    } else {
      s_st_over_budget++;
    }
  }
  s_render_orig(self, recurse);
  if (--s_depth == 0 && s_active) {
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    s_active->draws = gl_patch_total_draws() - s_active->draws0;
    s_active = NULL;
  }
}

/* The one place a Gob is culled. The engine's check runs first and unchanged (it
 * keeps its own LOS timer and flag at gob+0x18/0x1c); a query decision can only
 * turn its 1 into a 0. */
static int gob_visibility_hook(void *self) {
  int v = s_vis_orig(self);
  if (s_state != 1 || !s_on) return v;
  if (s_collected_frame != s_frame) {
    /* First check of the frame, i.e. the start of the Gob pass: take last
     * frame's results now if the GPU is done with them. */
    s_collected_frame = s_frame;
    for (int i = 0; i < 2; i++)
      if (i != s_cur) bank_collect(&s_bank[i]);
  }
  if (!v) return v;
  cull_entry_t *e = cull_find(self, 0);
  if (!e) return v;
  e->last_seen = s_frame;
  if (e->state != CS_HIDDEN || e->empty) return v;
  if (((const uint8_t *)self)[0x12]) {         /* Gob::IsAlwaysRendering */
    s_st_protected++;
    return v;
  }
  if ((int32_t)(s_frame - e->next_retest) < 0) {
    s_st_skipped++;
    return 0;
  }
  e->state = CS_PENDING;
  s_st_retests++;
  return v;
}

/* ---- per frame ------------------------------------------------------------------------ */
static void queries_init(void) {
  GLuint ids[2 * QN];
  memset(ids, 0, sizeof ids);
  glGenQueries(2 * QN, ids);
  for (unsigned i = 0; i < 2 * QN; i++) {
    if (!ids[i]) {
      log_printf("[cull] DISABLED: glGenQueries returned only %u of %u ids", i, 2 * QN);
      s_state = -1;
      return;
    }
    s_bank[i / QN].slot[i % QN].id = ids[i];
  }
  s_cur = 0;
  s_state = 1;
}

static void failsafe_check(void) {
#if !OCCLUSION_CULL_TEST
  /* Three consecutive groups of >= 240 results, each at least 99.5 % hidden,
   * mean the queries are not working: a real scene always shows some Gob (the
   * player character, the party). */
  if (s_fs_total < 240) return;
  if ((uint64_t)s_fs_hidden * 1000u >= (uint64_t)s_fs_total * 995u) {
    if (++s_fs_windows >= 3) {
      s_on = 0;
      cull_clear("fail-safe");
      log_printf("[cull] FAIL-SAFE: %u of %u results hidden -- queries look "
                 "non-functional, culling off for this session", s_fs_hidden, s_fs_total);
    }
  } else {
    s_fs_windows = 0;
  }
  s_fs_total = s_fs_hidden = 0;
#endif
}

void occlusion_cull_on_swap(uint64_t swap_end_us) {
  if (!s_installed) return;
  if (s_state == 0) queries_init();
  if (s_state != 1) return;
  uint64_t frame_us = s_prev_swap_us ? swap_end_us - s_prev_swap_us : 0;
  s_prev_swap_us = swap_end_us;

  /* The engine never presents from inside Gob::Render, but a query left open
   * across the swap would bleed into the next frame. */
  if (s_active) {
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    s_active->draws = gl_patch_total_draws() - s_active->draws0;
    s_active = NULL;
  }
  if (s_cur >= 0 && s_bank[s_cur].used) s_bank[s_cur].pending = 1;
  int free0 = bank_collect(&s_bank[0]);
  int free1 = bank_collect(&s_bank[1]);
  int next = s_cur == 0 ? (free1 ? 1 : free0 ? 0 : -1) : (free0 ? 0 : free1 ? 1 : -1);
  if (next < 0) s_st_behind++;   /* GPU two frames behind: no queries this frame */
  s_cur = next;
  s_frame++;

  if (frame_us > 1000000u) cull_clear("long frame / load");
  if (s_on) failsafe_check();
#if OCCLUSION_CULL_TEST
  if (s_frame % 1500u == 0) {
    s_on = !s_on;
    cull_clear(s_on ? "test toggle ON" : "test toggle OFF");
  }
#endif

#if OCCLUSION_CULL_LOG_INTERVAL_US
  s_st_frames++;
  if (!s_log_t0) s_log_t0 = swap_end_us;
  if (swap_end_us - s_log_t0 >= OCCLUSION_CULL_LOG_INTERVAL_US) {
    unsigned f = s_st_frames ? s_st_frames : 1;
    unsigned q10 = s_st_queried * 10u / f, s10 = s_st_skipped * 10u / f;
    unsigned h10 = s_st_queried ? (unsigned)((uint64_t)s_st_hidden * 1000u / s_st_queried) : 0;
    log_printf("[cull] %s frames=%u | queried=%u.%u Gobs/frame, %u.%u%% hidden | skipped=%u.%u "
               "Gobs/frame retests=%u protected=%u over-budget=%u gpu-behind=%u",
               s_on ? "on" : "off", s_st_frames, q10 / 10, q10 % 10, h10 / 10, h10 % 10,
               s10 / 10, s10 % 10, s_st_retests, s_st_protected, s_st_over_budget,
               s_st_behind);
    s_log_t0 = swap_end_us;
    s_st_frames = s_st_queried = s_st_hidden = s_st_skipped = s_st_retests = 0;
    s_st_protected = s_st_over_budget = s_st_behind = 0;
  }
#endif
}

/* ---- install ------------------------------------------------------------------------ */
/* Walk the relocation slots bound to `symbol`. With replacement == 0 only count
 * them: *bad counts slots that no longer hold the engine's own function (some
 * other hook got there first). Otherwise replace every slot and return the
 * original callable. Same mechanism as main.c's probe installers. */
static uintptr_t plt_slots(const char *symbol, uintptr_t replacement,
                           unsigned *n_out, unsigned *bad_out) {
  uintptr_t original = so_symbol(&kotor_mod, symbol);
  uintptr_t callable = 0;
  unsigned n = 0, bad = 0;
  for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
    Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
        &kotor_mod.relplt[i - kotor_mod.num_reldyn];
    unsigned type = ELF32_R_TYPE(rel->r_info);
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
      continue;
    Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
    if (strcmp(kotor_mod.dynstr + sym->st_name, symbol) != 0) continue;
    uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
    if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { bad++; continue; }
    n++;
    if (!replacement) continue;
    if (!callable) callable = *slot;
    kuKernelCpuUnrestrictedMemcpy(slot, &replacement, sizeof replacement);
  }
  if (n_out) *n_out = n;
  if (bad_out) *bad_out = bad;
  return callable;
}

void occlusion_cull_install(void) {
  static const char *const k_vis = "_ZN3Gob15VisibilityCheckEv";
  static const char *const k_render = "_ZN3Gob6RenderEb";
  unsigned vn = 0, vbad = 0, rn = 0, rbad = 0;
  plt_slots(k_vis, 0, &vn, &vbad);
  plt_slots(k_render, 0, &rn, &rbad);
  if (!vn || vbad || !rn || rbad) {
    log_printf("[cull] DISABLED: Gob slots not free (VisibilityCheck %u/%u hooked, "
               "Render %u/%u hooked)", vbad, vn + vbad, rbad, rn + rbad);
    return;
  }
  s_vis_orig = (int (*)(void *))plt_slots(k_vis, (uintptr_t)&gob_visibility_hook, NULL, NULL);
  s_render_orig = (void (*)(void *, int))plt_slots(k_render, (uintptr_t)&gob_render_hook,
                                                   NULL, NULL);
  s_installed = s_vis_orig && s_render_orig;
  log_printf("[cull] armed: VisibilityCheck x%u, Render x%u | %u queries/frame, hide after %u, "
             "re-test every %u frames%s", vn, rn, (unsigned)QN,
             (unsigned)OCCLUSION_CULL_HIDE_AFTER, (unsigned)OCCLUSION_CULL_RETEST,
             OCCLUSION_CULL_TEST ? " | TEST PATTERN (not real query results)" : "");
}

#endif /* OCCLUSION_CULL_ENABLE */
