/* scene_cpu.c -- the game thread's scene work made cheaper (SCENE_CPU).
 *
 * From the 2026-10-17 hardware profile and the study of what Scene::Render
 * does per frame (docs/specs/CPU_OFFLOAD.md):
 *
 * Light lists. LightManager::SetupLightsForThisPositionAndRadius (once per
 * drawn gob, and from ManageSceneBSP) empties the render list at lm+0x2c, then
 * for each light of the visible rooms runs a 2-D box test and calls
 * AddRenderLight (vtable +0x14), which scans the render list
 * (List<PartLight*>::Contains) before adding: O(lights x render lights). The
 * copy here runs the same tests in the same order and remembers what it added
 * in a small hash set, so duplicates are dropped without the scan. The result
 * is the same list. SCENE_CPU_VERIFY runs the engine's own after it and
 * compares.
 *
 * Animation of gobs not drawn this frame. The engine animates every gob of the
 * scene once a frame (Gob::Animate stamps gob+0x1b8 with scene+0x50 and
 * returns at once when called again in that frame). Scene::DoGobBuckets
 * animates the gobs it is about to draw, draws them, then animates all the
 * others (vtable +0x18, return address DoGobBuckets+0x311). Those, here, are
 * animated one frame in ANIM_LOD_N, with the time of the frames skipped added
 * to the next step; in a skipped frame the gob is processed (Gob::ProcessGob,
 * as Animate does first) and stamped, so nothing else animates it that frame.
 * Gobs drawn later in the frame (the hologram, force-push and lightsaber
 * buckets), gobs forced to animate or to render, cutscene gobs and procedural
 * ones are left alone. Their animation events fire up to ANIM_LOD_N - 1
 * frames late.
 *
 * ux0:data/kotor2/scene_mode.txt: 0 off (nothing hooked), 1 on, a on and off
 * alternately every 10 s (both at once). Without the file: SCENE_CPU_DEFAULT.
 * SCENE_CPU_STATS (test builds): every 10 s, the time in ManageSceneBSP and
 * DoGobBuckets, Gob::Animate per call site (calls, the ones that did work,
 * time), the gobs skipped, the light lists, and the models costing most in
 * the after-draw loop ([scene] lines). */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "scene_cpu.h"

#if SCENE_CPU

static inline uint32_t tls_key(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}
static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static int s_installed, s_ab, s_on;
static uint32_t s_game_key, s_frame;
static inline int on_game_thread(void) { return s_game_key && tls_key() == s_game_key; }

/* Point every relocation slot of sym that holds the engine's own function at
 * repl; returns the original, 0 if nothing changed (as in gl_state_filter.c). */
static uintptr_t take_slots(const char *sym, uintptr_t repl, unsigned *n_out) {
  uintptr_t original = so_symbol(&kotor_mod, sym), callable = 0;
  unsigned n = 0, bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
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

/* ---- window counters ------------------------------------------------------------------ */
static uint32_t w_frames;
static uint64_t w_frame_us, w_bsp_us, w_buckets_us, w_lights_us;
static uint32_t w_lights_calls, w_lights_src, w_lights_out, w_lights_dups, w_verify_bad;
static uint32_t w_lod_skipped, w_lod_stepped;

/* ---- light lists ---------------------------------------------------------------------- */
typedef struct { void **data; int32_t count, cap; } list_t;
typedef void (*setup_t)(void *lm, const float *pos, uint32_t radius, list_t *src);
static setup_t o_setup;
static void (*o_list_add)(list_t *, void *);
static void *s_add_render_light;               /* LightManager::AddRenderLight */

#define LSET_N 256u
static void *s_lset[LSET_N];
static uint32_t s_lset_gen[LSET_N], s_gen = 1;
/* 1 if newly added. */
static int lset_add(void *p) {
  uint32_t h = ((uint32_t)(uintptr_t)p >> 3) * 2654435761u;
  for (uint32_t i = 0; i < LSET_N; i++) {
    uint32_t k = (h + i) & (LSET_N - 1);
    if (s_lset_gen[k] != s_gen) { s_lset_gen[k] = s_gen; s_lset[k] = p; return 1; }
    if (s_lset[k] == p) return 0;
  }
  return -1;                                    /* full: let the engine decide */
}

static void setup_lights(void *lm, const float *pos, float radius, list_t *src) {
  uint8_t *m = lm;
  list_t *out = (list_t *)(m + 0x2c);
  out->count = 0;                               /* str r5, [r0, #0x30] */
  float twice = radius + radius;
  int direct = (*(void ***)lm)[0x14 / 4] == s_add_render_light && o_list_add;
  if (++s_gen == 0) { memset(s_lset_gen, 0, sizeof s_lset_gen); s_gen = 1; }
  for (int i = 0; i < src->count; i++) {
    uint8_t *l = src->data[i];
    float lr = *(float *)(l + 88);
    if (lr <= 0.0f) continue;                   /* bls: NaN goes on, as there */
    if (!*(void **)(l + 0x6c) && !*(void **)(l + 0x70)) continue;
    if (*(float *)(l + 76) == 0.0f && *(float *)(l + 80) == 0.0f && *(float *)(l + 84) == 0.0f) continue;
    float dx = *(float *)(l + 148) - pos[0], dy = *(float *)(l + 152) - pos[1];
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (!(twice > dx - lr) || !(twice > dy - lr)) continue;
    if (!direct) {
      ((void (*)(void *, void *))(*(void ***)lm)[0x14 / 4])(lm, l);
      continue;
    }
    int added = lset_add(l);
    if (added > 0) o_list_add(out, l);
    else if (added == 0) w_lights_dups++;
    else ((void (*)(void *, void *))s_add_render_light)(lm, l);
  }
}

static void h_setup(void *lm, const float *pos, uint32_t radius_bits, list_t *src) {
  if (!on_game_thread() || !src) { o_setup(lm, pos, radius_bits, src); return; }
  if (!s_on) {
    uint64_t t = SCENE_CPU_STATS ? now_us() : 0;
    o_setup(lm, pos, radius_bits, src);
    if (SCENE_CPU_STATS) {
      w_lights_us += now_us() - t;
      w_lights_calls++;
      w_lights_src += (uint32_t)src->count;
      w_lights_out += (uint32_t)((list_t *)((uint8_t *)lm + 0x2c))->count;
    }
    return;
  }
  uint64_t t0 = SCENE_CPU_STATS ? now_us() : 0;
  float radius;
  memcpy(&radius, &radius_bits, 4);
  setup_lights(lm, pos, radius, src);
#if SCENE_CPU_VERIFY
  {
    list_t *out = (list_t *)((uint8_t *)lm + 0x2c);
    static void *mine[512];
    int n = out->count < 512 ? out->count : 512;
    memcpy(mine, out->data, (size_t)n * sizeof(void *));
    o_setup(lm, pos, radius_bits, src);
    if (out->count != n || memcmp(mine, out->data, (size_t)n * sizeof(void *))) {
      if (w_verify_bad++ < 8)
        log_printf("[scene] VERIFY lights: mine %d, engine %d", n, (int)out->count);
    }
  }
#endif
  if (SCENE_CPU_STATS) {
    w_lights_us += now_us() - t0;
    w_lights_calls++;
    w_lights_src += (uint32_t)src->count;
    w_lights_out += (uint32_t)((list_t *)((uint8_t *)lm + 0x2c))->count;
  }
}

/* ---- animation ------------------------------------------------------------------------ */
typedef void (*animate_t)(void *gob, uint32_t dt);
static animate_t o_animate;
static void (*o_process_gob)(void *gob);
static const char *(*o_model_name)(void *gob);
static list_t *s_holo, *s_push, *s_saber;     /* buckets drawn after DoGobBuckets */
#define SITE_BEFORE 0x537d25u                  /* DoGobBuckets: the gobs about to be drawn */
#define SITE_AFTER 0x537f51u                   /* DoGobBuckets: every other gob */

static int in_list(list_t *l, void *p) {
  if (!l || !l->data) return 0;
  for (int i = 0; i < l->count; i++) if (l->data[i] == p) return 1;
  return 0;
}
/* 0 if it may be animated less often, else why not (1-based, w_excl). */
enum { X_OK, X_FORCED, X_ALWAYS, X_CUTSCENE, X_ONCE, X_PROC, X_LATER, NX };
static uint32_t w_excl[NX];
static int lod_exclusion(uint8_t *g) {
  if (g[0x10]) return X_FORCED;
  if (g[0x12]) return X_ALWAYS;
  if (g[0x14]) return X_CUTSCENE;
  if (g[0x17]) return X_ONCE;
  if (g[0x1b0]) return X_PROC;                  /* a byte: Gob::Animate's ldrb before ProceduralAnimateHierarchy */
  if (in_list(s_holo, g) || in_list(s_push, g) || in_list(s_saber, g)) return X_LATER;
  return X_OK;
}

/* Time a gob has not been animated for, kept per gob. */
#define LOD_N 1024u
typedef struct { void *gob; float acc; uint32_t frame; } lod_t;
static lod_t s_lod[LOD_N];
static lod_t *lod_slot(void *gob) {
  uint32_t h = ((uint32_t)(uintptr_t)gob >> 3) * 2654435761u;
  lod_t *free_e = NULL;
  for (uint32_t i = 0; i < 16; i++) {
    lod_t *e = &s_lod[(h + i) & (LOD_N - 1)];
    if (e->gob == gob) return e;
    if (!free_e && (!e->gob || s_frame - e->frame > 120)) free_e = e;   /* empty, or not seen for 2 s */
  }
  if (!free_e) return NULL;
  free_e->gob = gob; free_e->acc = 0; free_e->frame = s_frame;
  return free_e;
}

static lod_t *lod_find(void *gob) {
  uint32_t h = ((uint32_t)(uintptr_t)gob >> 3) * 2654435761u;
  for (uint32_t i = 0; i < 16; i++) {
    lod_t *e = &s_lod[(h + i) & (LOD_N - 1)];
    if (e->gob == gob) return e;
  }
  return NULL;
}

/* Statistics per call site (by return address). */
#define NSITE 12
static struct { uint32_t lr, calls, real; uint64_t us; } w_site[NSITE];
static int s_anim_depth;
#define NMODEL 48
static struct { const char *name; uint32_t calls; uint64_t us; } w_model[NMODEL];

static void note_model(void *gob, uint32_t us) {
  const char *n = o_model_name ? o_model_name(gob) : NULL;
  int free_i = -1;
  for (int i = 0; i < NMODEL; i++) {
    if (w_model[i].name == n && w_model[i].calls) { w_model[i].calls++; w_model[i].us += us; return; }
    if (!w_model[i].calls && free_i < 0) free_i = i;
  }
  if (free_i >= 0) { w_model[free_i].name = n; w_model[free_i].calls = 1; w_model[free_i].us = us; }
}

static void h_animate(void *gob, uint32_t dt) {
  if (!on_game_thread()) { o_animate(gob, dt); return; }
  uint32_t lr = (uint32_t)((uintptr_t)__builtin_return_address(0) - kotor_mod.text_base);
  uint8_t *g = gob;
  uint8_t *scene = *(uint8_t **)(g + 0x94);
  int real = !scene || *(uint32_t *)(g + 0x1b8) != *(uint32_t *)(scene + 0x50);
  int excl = lr == SITE_AFTER && real && scene ? lod_exclusion(g) : -1;
  if (excl >= 0) w_excl[excl]++;
  if (s_on && excl == X_OK) {
    lod_t *e = lod_slot(gob);
    if (e) {
      float d;
      memcpy(&d, &dt, 4);
      uint32_t phase = ((uint32_t)(uintptr_t)gob >> 4) + s_frame;
      e->frame = s_frame;
      if (phase % ANIM_LOD_N) {                 /* not its frame: processed, stamped, time kept */
        e->acc += d;
        o_process_gob(gob);
        *(uint32_t *)(g + 0x1b8) = *(uint32_t *)(scene + 0x50);
        w_lod_skipped++;
        return;
      }
      d += e->acc;
      e->acc = 0;
      memcpy(&dt, &d, 4);
      w_lod_stepped++;
    }
  } else if (s_on && real && scene) {
    /* Animated from elsewhere (back in view, pulled by an attachment): the
     * time owed goes with it. */
    lod_t *e = lod_find(gob);
    if (e && e->acc > 0) {
      float d;
      memcpy(&d, &dt, 4);
      d += e->acc;
      e->acc = 0;
      memcpy(&dt, &d, 4);
    }
  }
  if (!SCENE_CPU_STATS || s_anim_depth) { s_anim_depth++; o_animate(gob, dt); s_anim_depth--; return; }
  int k = -1;
  for (int i = 0; i < NSITE; i++) {
    if (w_site[i].lr == lr && w_site[i].calls) { k = i; break; }
    if (!w_site[i].calls) { k = i; w_site[i].lr = lr; break; }
  }
  uint64_t t0 = now_us();
  s_anim_depth++;
  o_animate(gob, dt);
  s_anim_depth--;
  uint32_t us = (uint32_t)(now_us() - t0);
  if (k >= 0) { w_site[k].calls++; w_site[k].real += (uint32_t)real; w_site[k].us += us; }
  if (lr == SITE_AFTER && real) note_model(gob, us);
}

/* ---- the two passes, timed ------------------------------------------------------------- */
static void (*o_bsp)(void *scene);
static void h_bsp(void *scene) {
  if (!on_game_thread()) { o_bsp(scene); return; }
  uint64_t t0 = now_us();
  o_bsp(scene);
  w_bsp_us += now_us() - t0;
}
static void (*o_buckets)(void *scene);
static void h_buckets(void *scene) {
  if (!on_game_thread()) { o_buckets(scene); return; }
  uint64_t t0 = now_us();
  o_buckets(scene);
  w_buckets_us += now_us() - t0;
}

/* ---- report, on and off ------------------------------------------------------------------ */
int scene_cpu_phase(void) { return s_installed ? s_on : -1; }

void scene_cpu_on_swap(void) {
  if (!s_installed) return;
  if (!s_game_key) s_game_key = tls_key();
  s_frame++;
  static uint64_t t0, prev;
  uint64_t now = now_us();
  if (prev) { w_frame_us += now - prev; w_frames++; }
  prev = now;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  double f = w_frames ? (double)w_frames : 1.0;
  log_printf("[scene] %s%u frames, %.1f ms | ManageSceneBSP %.2f ms, DoGobBuckets %.2f ms | lights: %.1f setups, "
             "%.3f ms, %.1f lights in, %.1f out, %.1f duplicates dropped%s | not drawn: %.1f gobs skipped, %.1f "
             "stepped",
             s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)w_frames, w_frames ? (double)w_frame_us / f / 1000.0 : 0.0,
             (double)w_bsp_us / f / 1000.0, (double)w_buckets_us / f / 1000.0, w_lights_calls / f,
             (double)w_lights_us / f / 1000.0, w_lights_calls ? (double)w_lights_src / w_lights_calls : 0.0,
             w_lights_calls ? (double)w_lights_out / w_lights_calls : 0.0, w_lights_dups / f,
             SCENE_CPU_VERIFY ? (w_verify_bad ? " VERIFY FAILED" : " (verified)") : "", w_lod_skipped / f,
             w_lod_stepped / f);
#if SCENE_CPU_STATS
  {
    char b[400];
    int o = 0;
    for (int i = 0; i < NSITE && w_site[i].calls && o < (int)sizeof b - 64; i++) {
      const char *nm = w_site[i].lr == SITE_BEFORE ? "before-draw" : w_site[i].lr == SITE_AFTER ? "after-draw" : NULL;
      o += nm ? snprintf(b + o, sizeof b - (size_t)o, " %s", nm) : snprintf(b + o, sizeof b - (size_t)o, " +%06x", (unsigned)w_site[i].lr);
      o += snprintf(b + o, sizeof b - (size_t)o, " %.0f/%.1f/%.0fus;", w_site[i].calls / f, w_site[i].real / f,
                    (double)w_site[i].us / f);
    }
    log_printf("[scene] Gob::Animate by caller (calls/working/time a frame):%s", o ? b : " none");
    log_printf("[scene] after-draw working calls a frame: %.1f may skip; kept: %.1f forced, %.1f always rendered, %.1f "
               "cutscene, %.1f forced once, %.1f procedural, %.1f drawn later",
               w_excl[X_OK] / f, w_excl[X_FORCED] / f, w_excl[X_ALWAYS] / f, w_excl[X_CUTSCENE] / f, w_excl[X_ONCE] / f,
               w_excl[X_PROC] / f, w_excl[X_LATER] / f);
    memset(w_excl, 0, sizeof w_excl);
    o = 0;
    for (int r = 0; r < 6; r++) {
      int best = -1;
      for (int i = 0; i < NMODEL; i++)
        if (w_model[i].calls && (best < 0 || w_model[i].us > w_model[best].us)) best = i;
      if (best < 0 || o > (int)sizeof b - 64) break;
      o += snprintf(b + o, sizeof b - (size_t)o, " %.24s %.0fus(%.1f);", w_model[best].name ? w_model[best].name : "?",
                    (double)w_model[best].us / f, w_model[best].calls / f);
      w_model[best].calls = 0;
    }
    if (o) log_printf("[scene] after-draw, most time:%s", b);
    memset(w_site, 0, sizeof w_site);
    memset(w_model, 0, sizeof w_model);
  }
#endif
  w_frames = 0; w_frame_us = 0; w_bsp_us = w_buckets_us = w_lights_us = 0;
  w_lights_calls = w_lights_src = w_lights_out = w_lights_dups = 0;
  w_lod_skipped = w_lod_stepped = 0;
  t0 = now;
  if (s_ab) {
    s_on = !s_on;
    if (!s_on) memset(s_lod, 0, sizeof s_lod);  /* every gob animates again: no time owed */
  }
}

void scene_cpu_install(void) {
  char m = SCENE_CPU_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/scene_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    char b = 0;
    if (sceIoRead(fd, &b, 1) == 1) m = b;
    sceIoClose(fd);
  }
  if (m != '1' && m != 'a') {
    log_printf("[scene] off (scene_mode '%c')", m);
    return;
  }
  s_ab = m == 'a';
  o_list_add = (void (*)(list_t *, void *))so_symbol(&kotor_mod, "_ZN4ListIP9PartLightE3AddES1_");
  s_add_render_light = (void *)so_symbol(&kotor_mod, "_ZN12LightManager14AddRenderLightEP9PartLight");
  o_process_gob = (void (*)(void *))so_symbol(&kotor_mod, "_ZN3Gob10ProcessGobEv");
  o_model_name = (const char *(*)(void *))so_symbol(&kotor_mod, "_ZN3Gob12GetModelNameEv");
  s_holo = (list_t *)so_symbol(&kotor_mod, "g_listHologramBucket");
  s_push = (list_t *)so_symbol(&kotor_mod, "g_listForcePushBucket");
  s_saber = (list_t *)so_symbol(&kotor_mod, "g_listLightsaberBucket");

  unsigned n_setup = 0, n_anim = 0, n_bsp = 0, n_buckets = 0;
  if (o_list_add && s_add_render_light)
    o_setup = (setup_t)take_slots("_ZN12LightManager35SetupLightsForThisPositionAndRadiusERK6VectorfR4ListIP9PartLightE",
                                  (uintptr_t)h_setup, &n_setup);
  if (o_process_gob && s_holo && s_push && s_saber)
    o_animate = (animate_t)take_slots("_ZN3Gob7AnimateEf", (uintptr_t)h_animate, &n_anim);
  if (SCENE_CPU_STATS) {
    o_bsp = (void (*)(void *))take_slots("_Z14ManageSceneBSPP5Scene", (uintptr_t)h_bsp, &n_bsp);
    o_buckets = (void (*)(void *))take_slots("_ZN5Scene12DoGobBucketsEv", (uintptr_t)h_buckets, &n_buckets);
  }
  s_on = 1;
  s_installed = 1;
  log_printf("[scene] armed: light setup %u slots, Gob::Animate %u (not-drawn gobs every %d frames)%s%s%s", n_setup,
             n_anim, ANIM_LOD_N, SCENE_CPU_STATS ? "; timing ManageSceneBSP and DoGobBuckets" : "",
             s_ab ? "; A/B: on and off alternately every 10 s" : "",
             SCENE_CPU_VERIFY ? "; VERIFY: light lists checked against the engine's" : "");
  (void)n_bsp; (void)n_buckets;
}

#else
void scene_cpu_install(void) {}
void scene_cpu_on_swap(void) {}
int scene_cpu_phase(void) { return -1; }
#endif
