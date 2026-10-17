/* path_cache.c -- failed path-point searches remembered (PATH_CACHE).
 *
 * Hardware, Telos, 2026-10-17: a ~140 ms frame every 3.5-5 s. The AI worker's
 * profile of those passes (PC_PROF_TARGET_AI): one creature's AIUpdate ->
 * AIActionMoveToPoint -> CSWSModule::PlotPath -> CSWSArea::PlotPathPointPath
 * -> CSWSArea::ClosestPathPoint (85%) -> TestDirectLine -> walkmesh line
 * tests (NoNonWalkPolysOnRoom, 71%).
 *
 * ClosestPathPoint looks for the nearest path point with a clear line from a
 * position: the area's points (sorted by x, area+0x260, count +0x25c) in
 * square rings 10 units wider each time, up to 50; it ends after the first
 * ring that has one. TestDirectLine checks, per room crossed: the static
 * walkmesh (ClippedLineSegmentWalkable, NoNonWalkPolysOnRoom), then the
 * area's objects (NoNonWalkPolysDetailed: doors, placeables), then creatures
 * -- but ClosestPathPoint takes "blocked by a creature" (-3) as a hit, so
 * creatures never change its answer. A search that found nothing, with no
 * point turned down by the object check, depends on the area's static data
 * only: the same position in the same area gives the same answer. Those are
 * kept here (until the area changes), and asked again they are answered at
 * once. Successes are never kept (a door may close).
 *
 * ux0:data/kotor2/path_mode.txt: 0 off (nothing hooked), 1 on, a on and off
 * alternately every 10 s. Without the file: PATH_CACHE_DEFAULT. Every 10 s a
 * [path] line; every search over 10 ms is logged with its position. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "path_cache.h"

#if PATH_CACHE

static inline uint32_t tls_key(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}
static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static int s_installed, s_ab, s_on;

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

/* ---- the object check, watched during a search --------------------------------------- */
typedef uint32_t (*nnwpd_t)(void *, const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t, void *,
                            uint32_t, void *, uint32_t, void *, uint32_t);
static nnwpd_t o_nnwpd;
static volatile uint32_t s_search_key;          /* the thread searching, 0: none */
static uint32_t s_object_rejects;
static uint32_t h_nnwpd(void *area, const void *a, const void *b, uint32_t f0, uint32_t f1, uint32_t f2, uint32_t id,
                        void *out_id, uint32_t i0, void *list, uint32_t i1, void *hit, uint32_t i2) {
  uint32_t r = o_nnwpd(area, a, b, f0, f1, f2, id, out_id, i0, list, i1, hit, i2);
  if (!r && s_search_key && tls_key() == s_search_key) s_object_rejects++;
  return r;
}

/* ---- ClosestPathPoint -------------------------------------------------------------------- */
typedef uint32_t (*cpp_t)(void *area, const float *pos, uint32_t *out);
static cpp_t o_cpp;

#define PC_N 64
typedef struct { void *area, *points; uint32_t npoints, x, y, ret, idx; } pc_t;
static pc_t s_pc[PC_N];
static uint32_t s_pc_next;

static uint32_t w_calls, w_hits, w_kept, w_long;
static uint64_t w_us, w_saved_us;
static uint32_t s_last_cost_us[PC_N];

static uint32_t h_cpp(void *area, const float *pos, uint32_t *out) {
  uint8_t *a = area;
  void *points = *(void **)(a + 0x260);
  uint32_t npoints = *(uint32_t *)(a + 0x25c), x, y;
  memcpy(&x, &pos[0], 4);
  memcpy(&y, &pos[1], 4);
  if (s_on) {
    for (int i = 0; i < PC_N; i++) {
      pc_t *e = &s_pc[i];
      if (e->area == area && e->x == x && e->y == y && e->points == points && e->npoints == npoints) {
        *out = e->idx;
        w_hits++;
        w_saved_us += s_last_cost_us[i];
        return e->ret;
      }
    }
  }
  uint32_t key = tls_key();
  int mine = !s_search_key;                     /* not nested, not racing another thread */
  if (mine) { s_search_key = key; s_object_rejects = 0; }
  uint64_t t0 = now_us();
  uint32_t r = o_cpp(area, pos, out);
  uint32_t us = (uint32_t)(now_us() - t0);
  uint32_t rejects = mine ? s_object_rejects : 1;
  if (mine) s_search_key = 0;
  w_calls++;
  w_us += us;
  if (us >= 10000 && w_long++ < 40)
    log_printf("[path] ClosestPathPoint (%.2f, %.2f): %s, index %d, %.1f ms, %u turned down by objects%s",
               (double)pos[0], (double)pos[1], r ? "found" : "nothing", (int)*out, us / 1000.0, (unsigned)rejects,
               !r && !rejects && s_on ? "; kept" : "");
  if (s_on && !r && !rejects && mine) {         /* static failure: kept until the area changes */
    uint32_t i = s_pc_next++ % PC_N;
    pc_t *e = &s_pc[i];
    e->area = area; e->points = points; e->npoints = npoints; e->x = x; e->y = y; e->ret = r; e->idx = *out;
    s_last_cost_us[i] = us;
    w_kept++;
  }
  return r;
}

void path_cache_on_swap(void) {
  if (!s_installed) return;
  static uint64_t t0, prev;
  static uint32_t frames;
  uint64_t now = now_us();
  if (prev && now - prev > 250000) memset(s_pc, 0, sizeof s_pc);   /* a load: areas may be new */
  prev = now;
  frames++;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  log_printf("[path] %s%u frames | ClosestPathPoint: %u searches, %.1f ms in all, %u answered from memory (%.1f ms "
             "saved), %u failures kept",
             s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)frames, (unsigned)w_calls, w_us / 1000.0,
             (unsigned)w_hits, w_saved_us / 1000.0, (unsigned)w_kept);
  w_calls = w_hits = w_kept = w_long = 0;
  w_us = w_saved_us = 0;
  frames = 0;
  t0 = now;
  if (s_ab) s_on = !s_on;
}

int path_cache_phase(void) { return s_installed ? s_on : -1; }

void path_cache_install(void) {
  char m = PATH_CACHE_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/path_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    char b = 0;
    if (sceIoRead(fd, &b, 1) == 1) m = b;
    sceIoClose(fd);
  }
  if (m != '1' && m != 'a') {
    log_printf("[path] off (path_mode '%c')", m);
    return;
  }
  s_ab = m == 'a';
  unsigned n_cpp = 0, n_nnwpd = 0;
  o_nnwpd = (nnwpd_t)take_slots("_ZN8CSWSArea22NoNonWalkPolysDetailedERK6VectorS2_fffmRmiP13CExoArrayListIP11CGameObjectEiP12CWalkHitInfoi",
                                (uintptr_t)h_nnwpd, &n_nnwpd);
  if (o_nnwpd) o_cpp = (cpp_t)take_slots("_ZN8CSWSArea16ClosestPathPointERK6VectorRm", (uintptr_t)h_cpp, &n_cpp);
  s_on = 1;
  s_installed = 1;
  log_printf("[path] armed: ClosestPathPoint %u slots, NoNonWalkPolysDetailed %u%s", n_cpp, n_nnwpd,
             s_ab ? "; A/B: on and off alternately every 10 s" : "");
}

#else
void path_cache_install(void) {}
void path_cache_on_swap(void) {}
int path_cache_phase(void) { return -1; }
#endif
