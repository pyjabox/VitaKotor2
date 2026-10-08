/* gxm_patcher.c -- see gxm_patcher.h. Linked in with -Wl,--wrap (CMakeLists.txt),
 * so vitaGL's own calls come through here. Ported from the KOTOR I port
 * (VitaKotor 32ffdb8 and 9b3ac28); what differs is the thread (the GL
 * worker's), the fixed-function exclusion and the report. */

#include <vitasdk.h>
#include <psp2/gxm.h>
#include <string.h>

#include "config.h"
#include "gxm_patcher.h"
#include "log.h"

int __real_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    const SceGxmVertexAttribute *attr, unsigned int na, const SceGxmVertexStream *st,
    unsigned int ns, SceGxmVertexProgram **out);
int __real_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    SceGxmOutputRegisterFormat fmt, SceGxmMultisampleMode msaa, const SceGxmBlendInfo *blend,
    const SceGxmProgram *vp, SceGxmFragmentProgram **out);
int __real_sceGxmShaderPatcherForceUnregisterProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id);
void __real_sceGxmSetVertexProgram(SceGxmContext *ctx, const SceGxmVertexProgram *vp);

/* vitaGL (ffp.c): the shader of the fixed-function variant being created. The
 * fixed-function path patches only when its state changes and binds the stored
 * ffp_vertex_program_patched on every other draw, so none of its variants may
 * ever be freed; they are left out altogether. Only the boot loading screen
 * draws that way. */
extern SceGxmShaderPatcherId ffp_vertex_program_id;

/* A variant not bound for this many frames is fair game. The GPU runs at most
 * a few frames behind the GL thread, so nothing this old can still be in
 * flight. */
#define GXMP_IDLE_FRAMES 30
#define GXMP_SLOTS 8192              /* open-addressed, power of two */
#define GXMP_DETAIL_MAX 12           /* failure lines, then one per report */

typedef struct {
  SceGxmVertexProgram *vp;           /* NULL = empty, GXMP_TOMB = deleted */
  SceGxmShaderPatcherId id;
  uint32_t last_frame;
  uint32_t creates;
} gxmp_slot;

#define GXMP_TOMB ((SceGxmVertexProgram *)1)

static gxmp_slot g_slots[GXMP_SLOTS];
static unsigned g_live, g_tombs;
static volatile int g_armed;         /* set by the game thread, read by vitaGL's */
static uint32_t g_frame;
static SceGxmShaderPatcher *g_patcher;
static uint32_t g_last_gc_frame = ~0u;
static const SceGxmVertexProgram *g_last_set;
static gxmp_slot *g_last_set_slot;

/* report-window / lifetime counters */
static unsigned w_vp_calls, w_vp_fail, w_fp_calls, w_fp_fail, w_evicted, w_gc;
static unsigned t_vp_fail, t_fp_fail, t_evicted, t_gc, t_recovered, d_fail;
static int g_vp_lasterr, g_fp_lasterr;
static uint32_t g_usse_peak;

uint32_t gxmp_vertex_usse_bytes(void) {
  return GXMP_VERTEX_USSE_KB * 1024u;
}

static unsigned slot_hash(const SceGxmVertexProgram *vp) {
  return ((uintptr_t)vp >> 4) * 2654435761u & (GXMP_SLOTS - 1);
}

static gxmp_slot *slot_find(const SceGxmVertexProgram *vp) {
  unsigned h = slot_hash(vp);
  for (unsigned n = 0; n < GXMP_SLOTS; n++, h = (h + 1) & (GXMP_SLOTS - 1)) {
    gxmp_slot *s = &g_slots[h];
    if (s->vp == vp) return s;
    if (!s->vp) return NULL;
  }
  return NULL;
}

static gxmp_slot *slot_insert(SceGxmVertexProgram *vp) {
  unsigned h = slot_hash(vp);
  for (unsigned n = 0; n < GXMP_SLOTS; n++, h = (h + 1) & (GXMP_SLOTS - 1)) {
    gxmp_slot *s = &g_slots[h];
    if (!s->vp || s->vp == GXMP_TOMB) {
      if (s->vp == GXMP_TOMB) g_tombs--;
      s->vp = vp;
      g_live++;
      return s;
    }
  }
  return NULL;
}

/* Repeat requests, answered without the patcher. Keyed on everything the create
 * takes: the shader id and the exact attribute and stream arrays (both structs
 * are fully packed, so they compare as bytes). An entry points at its tracking
 * slot, so anything that kills or moves slots marks the whole memo stale and
 * the next lookup wipes it. */
#if GXMP_MEMO
#define MEMO_SLOTS 2048              /* power of two */
#define MEMO_MAX 16                  /* attributes/streams per program */

typedef struct {
  gxmp_slot *slot;                   /* NULL = empty */
  SceGxmShaderPatcherId id;
  uint32_t hash;
  uint8_t na, ns;
  SceGxmVertexAttribute attr[MEMO_MAX];
  SceGxmVertexStream st[MEMO_MAX];
} memo_ent;

static memo_ent g_memo[MEMO_SLOTS];
static unsigned g_memo_used;
static int g_memo_stale;
static unsigned w_memo_hits, w_memo_clears;

static uint32_t memo_hash(SceGxmShaderPatcherId id, const SceGxmVertexAttribute *attr,
                          unsigned na, const SceGxmVertexStream *st, unsigned ns) {
  uint32_t h = 2166136261u;
  h = (h ^ (uint32_t)(uintptr_t)id) * 16777619u;
  h = (h ^ (na | ns << 8)) * 16777619u;
  /* Halfwords: both structs are only 2-byte aligned, and a word load the
   * compiler fuses into LDRD/LDM faults on an unaligned address. */
  const uint16_t *w = (const uint16_t *)attr;
  for (unsigned i = 0; i < na * 4; i++) h = (h ^ w[i]) * 16777619u;
  w = (const uint16_t *)st;
  for (unsigned i = 0; i < ns * 2; i++) h = (h ^ w[i]) * 16777619u;
  return h;
}

static void memo_wipe(void) {
  memset(g_memo, 0, sizeof(g_memo));
  g_memo_used = 0;
  g_memo_stale = 0;
  w_memo_clears++;
}

static memo_ent *memo_find(SceGxmShaderPatcherId id, const SceGxmVertexAttribute *attr,
                           unsigned na, const SceGxmVertexStream *st, unsigned ns,
                           uint32_t h, int *free_idx) {
  *free_idx = -1;
  if (g_memo_stale) memo_wipe();
  unsigned i = h & (MEMO_SLOTS - 1);
  for (unsigned n = 0; n < MEMO_SLOTS; n++, i = (i + 1) & (MEMO_SLOTS - 1)) {
    memo_ent *m = &g_memo[i];
    if (!m->slot) { *free_idx = (int)i; return NULL; }
    if (m->hash == h && m->id == id && m->na == na && m->ns == ns &&
        !memcmp(m->attr, attr, na * sizeof(*attr)) && !memcmp(m->st, st, ns * sizeof(*st)))
      return m;
  }
  return NULL;
}
#define MEMO_STALE() (g_memo_stale = 1)
#else
#define MEMO_STALE() ((void)0)
#endif

static void slot_kill(gxmp_slot *s) {
  MEMO_STALE();
  s->vp = GXMP_TOMB;
  g_live--;
  g_tombs++;
  if (s == g_last_set_slot) { g_last_set = NULL; g_last_set_slot = NULL; }
}

/* Tombstones make misses walk further; rebuild once they pile up. */
static void slot_compact(void) {
  static gxmp_slot tmp[GXMP_SLOTS];
  unsigned n = 0;
  MEMO_STALE();                      /* every slot is about to move */
  for (unsigned i = 0; i < GXMP_SLOTS; i++)
    if (g_slots[i].vp && g_slots[i].vp != GXMP_TOMB) tmp[n++] = g_slots[i];
  memset(g_slots, 0, sizeof(g_slots));
  g_live = g_tombs = 0;
  g_last_set = NULL;
  g_last_set_slot = NULL;
  for (unsigned i = 0; i < n; i++) {
    gxmp_slot *s = slot_insert(tmp[i].vp);
    s->id = tmp[i].id;
    s->last_frame = tmp[i].last_frame;
    s->creates = tmp[i].creates;
  }
}

/* Give back every variant no draw has bound lately. Only variants vitaGL has
 * asked for more than once are released: the per-draw path re-creates its
 * program on every draw, so anything it uses gets there immediately, while a
 * program made once and re-bound from a stored pointer never does, and must
 * not be freed out from under that pointer. */
static unsigned evict_idle(void) {
  unsigned freed = 0;
  for (unsigned i = 0; i < GXMP_SLOTS; i++) {
    gxmp_slot *s = &g_slots[i];
    if (!s->vp || s->vp == GXMP_TOMB) continue;
    if (s->creates < 2 || g_frame - s->last_frame < GXMP_IDLE_FRAMES) continue;
    sceGxmShaderPatcherReleaseVertexProgram(g_patcher, s->vp);
    slot_kill(s);
    freed++;
  }
  if (g_tombs > GXMP_SLOTS / 4) slot_compact();
  return freed;
}

/* Record a successful create. The patcher returns the same program for the
 * same request with its refcount bumped; drop that extra reference so every
 * variant sits at exactly one, and one release frees it. */
static gxmp_slot *track(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id, SceGxmVertexProgram *vp) {
  gxmp_slot *s = slot_find(vp);
  if (s) {
    sceGxmShaderPatcherReleaseVertexProgram(sp, vp);
  } else {
    if (g_live + g_tombs >= GXMP_SLOTS * 3 / 4) {
      slot_compact();
      if (g_live >= GXMP_SLOTS * 3 / 4) return NULL;   /* table full: leave it unmanaged */
    }
    s = slot_insert(vp);
    s->id = id;
    s->creates = 0;
  }
  s->creates++;
  s->last_frame = g_frame;
  return s;
}

int __wrap_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    const SceGxmVertexAttribute *attr, unsigned int na, const SceGxmVertexStream *st,
    unsigned int ns, SceGxmVertexProgram **out) {
  int managed = g_armed && id != ffp_vertex_program_id;
#if GXMP_MEMO
  uint32_t mh = 0;
  int mfree = -1;
  int memo_ok = managed && out && na <= MEMO_MAX && ns <= MEMO_MAX && (na == 0 || attr) && (ns == 0 || st);
  if (memo_ok) {
    mh = memo_hash(id, attr, na, st, ns);
    memo_ent *m = memo_find(id, attr, na, st, ns, mh, &mfree);
    if (m) {
      /* What track() would have recorded for the create this replaces. */
      m->slot->creates++;
      m->slot->last_frame = g_frame;
      *out = m->slot->vp;
      w_vp_calls++;
      w_memo_hits++;
      return 0;
    }
  }
#endif
  int r = __real_sceGxmShaderPatcherCreateVertexProgram(sp, id, attr, na, st, ns, out);
  g_patcher = sp;
  w_vp_calls++;
  /* one sweep a frame: a full pool fails every draw, and a sweep that found
   * nothing idle will not find anything more until frames pass */
  if (r < 0 && g_armed && g_last_gc_frame != g_frame) {
    g_last_gc_frame = g_frame;
    unsigned freed = evict_idle();
    w_gc++; t_gc++;
    w_evicted += freed; t_evicted += freed;
    if (freed) {
      int r2 = __real_sceGxmShaderPatcherCreateVertexProgram(sp, id, attr, na, st, ns, out);
      if (r2 >= 0) t_recovered++;
      if (d_fail < GXMP_DETAIL_MAX) {
        d_fail++;
        log_printf("[gxmp] vertex program create failed 0x%08x (%u attrs, %u streams): "
                   "released %u idle variants, retry %s (usse now %u KB)",
                   (unsigned)r, na, ns, freed, r2 >= 0 ? "OK" : "FAILED",
                   sceGxmShaderPatcherGetVertexUsseMemAllocated(sp) >> 10);
      }
      r = r2;
    }
  }
  if (r < 0) {
    w_vp_fail++; t_vp_fail++; g_vp_lasterr = r;
    if (d_fail < GXMP_DETAIL_MAX) {
      d_fail++;
      log_printf("[gxmp] VERTEX PROGRAM CREATE FAILED 0x%08x: %u attrs, %u streams, stride0=%u "
                 "-- vitaGL draws with its previous program (lifetime fails %u)",
                 (unsigned)r, na, ns, ns ? (unsigned)st[0].stride : 0u, t_vp_fail);
    }
  } else if (managed && out && *out) {
    gxmp_slot *s = track(sp, id, *out);
#if GXMP_MEMO
    /* track() can compact, which stales the memo and the free index with it. */
    if (s && memo_ok && !g_memo_stale && mfree >= 0 && g_memo_used < MEMO_SLOTS * 3 / 4) {
      memo_ent *m = &g_memo[mfree];
      m->slot = s;
      m->id = id;
      m->hash = mh;
      m->na = (uint8_t)na;
      m->ns = (uint8_t)ns;
      memcpy(m->attr, attr, na * sizeof(*attr));
      memcpy(m->st, st, ns * sizeof(*st));
      g_memo_used++;
    } else if (s && memo_ok && g_memo_used >= MEMO_SLOTS * 3 / 4) {
      MEMO_STALE();                  /* full: start over rather than probe long */
    }
#else
    (void)s;
#endif
  }
  return r;
}

void __wrap_sceGxmSetVertexProgram(SceGxmContext *ctx, const SceGxmVertexProgram *vp) {
  if (g_armed) {
    if (vp == g_last_set && g_last_set_slot) {
      g_last_set_slot->last_frame = g_frame;
    } else {
      gxmp_slot *s = slot_find(vp);
      if (s) s->last_frame = g_frame;
      g_last_set = vp;
      g_last_set_slot = s;
    }
  }
  __real_sceGxmSetVertexProgram(ctx, vp);
}

/* Deleting a shader frees all its variants inside the patcher; forget them so
 * a later eviction does not release a dangling pointer. */
int __wrap_sceGxmShaderPatcherForceUnregisterProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id) {
  if (g_armed) {
    for (unsigned i = 0; i < GXMP_SLOTS; i++) {
      gxmp_slot *s = &g_slots[i];
      if (s->vp && s->vp != GXMP_TOMB && s->id == id) slot_kill(s);
    }
  }
  return __real_sceGxmShaderPatcherForceUnregisterProgram(sp, id);
}

int __wrap_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    SceGxmOutputRegisterFormat fmt, SceGxmMultisampleMode msaa, const SceGxmBlendInfo *blend,
    const SceGxmProgram *vp, SceGxmFragmentProgram **out) {
  int r = __real_sceGxmShaderPatcherCreateFragmentProgram(sp, id, fmt, msaa, blend, vp, out);
  g_patcher = sp;
  w_fp_calls++;
  if (r < 0) {
    w_fp_fail++; t_fp_fail++; g_fp_lasterr = r;
    if (d_fail < GXMP_DETAIL_MAX) {
      d_fail++;
      log_printf("[gxmp] FRAGMENT PROGRAM CREATE FAILED 0x%08x (lifetime fails %u)",
                 (unsigned)r, t_fp_fail);
    }
  }
  return r;
}

void gxmp_arm(void) {
  char c = 0;
  SceUID fd = sceIoOpen(DATA_PATH "/patcher_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    if (sceIoRead(fd, &c, 1) != 1) c = 0;
    sceIoClose(fd);
  }
  if (c == '0') {
    log_printf("[gxmp] off (patcher_mode.txt): vertex USSE pool %u KB, variants never freed",
               (unsigned)GXMP_VERTEX_USSE_KB);
    return;
  }
  g_armed = 1;
  log_printf("[gxmp] armed: vertex USSE pool %u KB, memo %s", (unsigned)GXMP_VERTEX_USSE_KB,
             GXMP_MEMO ? "on" : "off");
}

static void report(void) {
  uint32_t usse = sceGxmShaderPatcherGetVertexUsseMemAllocated(g_patcher);
  if (usse > g_usse_peak) g_usse_peak = usse;
#if GXMP_MEMO
  log_printf("[gxmp] memo: %u of %u vp calls answered from cache, %u entries, %u wipes",
             w_memo_hits, w_vp_calls, g_memo_used, w_memo_clears);
  w_memo_hits = w_memo_clears = 0;
#endif
  log_printf("[gxmp] vp calls=%u fail=%u (lifetime %u, last 0x%x) variants=%u | gc=%u evicted=%u "
             "(lifetime gc=%u evicted=%u recovered=%u) | fp calls=%u fail=%u (lifetime %u, last 0x%x) | "
             "mem host=%u KB buffer=%u KB vertexUsse=%u/%u KB (peak %u) fragmentUsse=%u/%u KB",
             w_vp_calls, w_vp_fail, t_vp_fail, (unsigned)g_vp_lasterr, g_live,
             w_gc, w_evicted, t_gc, t_evicted, t_recovered,
             w_fp_calls, w_fp_fail, t_fp_fail, (unsigned)g_fp_lasterr,
             sceGxmShaderPatcherGetHostMemAllocated(g_patcher) >> 10,
             sceGxmShaderPatcherGetBufferMemAllocated(g_patcher) >> 10,
             usse >> 10, (unsigned)GXMP_VERTEX_USSE_KB, g_usse_peak >> 10,
             sceGxmShaderPatcherGetFragmentUsseMemAllocated(g_patcher) >> 10,
             GXMP_FRAGMENT_USSE_MEM >> 10);
  w_vp_calls = w_vp_fail = w_fp_calls = w_fp_fail = w_gc = w_evicted = 0;
  if (d_fail >= GXMP_DETAIL_MAX) d_fail = GXMP_DETAIL_MAX - 1;
}

void gxmp_on_swap(void) {
  static uint64_t t0;
  g_frame++;
  if (!g_patcher) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!t0) t0 = now;
  if (now - t0 < (uint64_t)GXMP_REPORT_S * 1000000u) return;
  t0 = now;
  report();
}
