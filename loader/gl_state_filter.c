/* gl_state_filter.c -- skip GL state calls that change nothing, before Aspyr's
 * ASLgl translation layer sees them.
 *
 * A hardware profile (real-vita-fnprof-20261003) put the translation layer at
 * 14-21 ms per frame. Besides the draws themselves, thousands of state calls
 * per frame each run gles2-bc's full path, which has no redundancy check:
 *   ASLgl::glEnable/glDisable          4-5k calls per frame
 *   ASLgl::glActiveTextureARB          1.5-2k (plus glActiveTexture)
 *   ASLgl::glClientActiveTextureARB    0.9-1.3k (plus glClientActiveTexture)
 * Their PLT slots are replaced: a call that sets the value the filter last saw
 * passed is dropped. The filter's copy of the state only ever comes from calls
 * that went through it.
 *  - Nothing else changes these states. ASLgl's glPushAttrib/glPopAttrib (and
 *    the client variants) are empty, the engine's direct vitaGL calls are
 *    framebuffer binds and glBlendEquation, and ASL_GLBlitter saves/restores
 *    through these same ASLgl entry points.
 *  - ASLgl's special cases (fog, alpha test, lighting, fragment program) are
 *    idempotent, so dropping a repeat is the same as running it.
 *  - Texture caps (GL_TEXTURE_2D, texgen, ...) are per texture unit and keyed
 *    by the unit the filter last saw selected.
 *  - The copy is forgotten after a long frame (loads), whenever the loader
 *    draws by itself (loading screen, its font), and while a unit is unknown.
 *    GL_SCISSOR_TEST, which a loader GUI fix sets in vitaGL directly, is never
 *    filtered.
 *
 * ASLgl::glTexParameteri (440-600 calls per frame, ~4 us each) ends with a
 * glGetIntegerv(GL_TEXTURE_BINDING_2D) whose result it never reads, a
 * synchronous query into vitaGL. That one call is patched out (instruction
 * bytes checked first).
 *
 * GL_STATE_FILTER: 0 off, 1 on, 2 verify (removes the dead query but never
 * skips a call; each call it would skip is checked against ASLgl::glIsEnabled
 * or glGetIntegerv, and disagreements are counted and logged). Only states
 * whose answer is real are checked: those vitaGL keeps (blend, depth, cull,
 * ...). gles2-bc forwards glIsEnabled to vitaGL for everything but fog and
 * alpha test, so its fixed-function states (lighting, texture units, ...) read
 * as off. Fog and alpha test themselves are kept by ASLgl, not gles2-bc: alpha
 * test is the global s_useAlphaTestShader, written only by ASLgl::glEnable,
 * glDisable and ASLgl_InitKotor2. Those count as unverifiable. gles2-bc's
 * fixed-function states are set only by OpenGLES20Context::glEnable and
 * glDisable, which are reached only from the ASLgl entry points hooked here.
 * Hardware runs real-vita-glsf-20261003 and -glsf2-:
 *  - no disagreement on the active or client-active unit, or on any
 *    vitaGL-kept cap;
 *  - the only ones were GL_LIGHTING and GL_ALPHA_TEST, which are artifacts of
 *    the check as above. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "gl_state_filter.h"

#define GLSF_TEXPARAM_GETI_OFF 0x5db58cu  /* blx ASLgl::glGetIntegerv in glTexParameteri */

enum { G_EN, G_ACT, G_CACT, G_N };
static const char *const k_group[G_N] = {"enable", "active", "client"};

typedef struct { uint32_t calls, redundant, skipped, mismatch, unverifiable; } gstat_t;
static gstat_t s_st[G_N];

static int s_mode, s_installed;
static int (*s_is_enabled)(unsigned cap);
static void (*s_get_integerv)(unsigned pname, int *out);

/* ---- the filter's copy of the state ---------------------------------------- */
#define CAPS 256u
typedef struct { uint32_t key; uint8_t on; } cap_t;   /* key 0 = empty */
static cap_t s_cap[CAPS];
static unsigned s_unit, s_cunit;                      /* 0 = unknown */

static void forget(void) {
  memset(s_cap, 0, sizeof s_cap);
  s_unit = s_cunit = 0;
}
void gl_state_filter_forget(void) { forget(); }

/* States whose glIsEnabled answer is real: the ones vitaGL keeps. */
static int verifiable(unsigned cap) {
  switch (cap) {
  case 0x0BE2: case 0x0B71: case 0x0B44: case 0x0B90: case 0x8037: case 0x0BD0:
  case 0x809E: case 0x80A0:
    return 1;
  }
  return 0;
}

static int per_unit(unsigned cap) {
  return cap == 0x0DE0 || cap == 0x0DE1 || cap == 0x806F || cap == 0x8513 ||
         (cap >= 0x0C60 && cap <= 0x0C63);
}

/* Slot for cap under the current unit, or NULL when the unit is unknown. */
static cap_t *cap_slot(unsigned cap) {
  if (cap == 0x0C11) return NULL;                     /* GL_SCISSOR_TEST: see above */
  uint32_t key = cap & 0xfffffu;
  if (per_unit(cap)) {
    if (!s_unit) return NULL;
    key |= (s_unit & 0xfffu) << 20;
  }
  key |= 0x80000000u;                                 /* never 0 */
  uint32_t h = (key * 2654435761u) >> 24;
  for (unsigned i = 0; i < CAPS; i++) {
    cap_t *c = &s_cap[(h + i) & (CAPS - 1)];
    if (c->key == key) return c;
    if (!c->key) { c->key = key; c->on = 0xff; return c; }
  }
  return NULL;                                        /* full: do not filter */
}

/* ---- hooks ------------------------------------------------------------------- */
static void (*o_enable)(unsigned), (*o_disable)(unsigned);
static void (*o_active)(unsigned), (*o_active_arb)(unsigned);
static void (*o_cactive)(unsigned), (*o_cactive_arb)(unsigned);

static void set_cap(unsigned cap, int on, void (*orig)(unsigned)) {
  gstat_t *g = &s_st[G_EN];
  g->calls++;
  cap_t *c = cap_slot(cap);
  if (c && c->on == on) {
    g->redundant++;
    if (s_mode == 1) { g->skipped++; return; }
    if (s_mode == 2 && s_is_enabled) {
      if (!verifiable(cap)) g->unverifiable++;
      else if (((s_is_enabled(cap) & 0xff) ? 1 : 0) != on && g->mismatch++ < 8)
        log_printf("[glsf] MISMATCH enable cap=0x%04x unit=0x%x filter=%d asl=%d", cap, s_unit, on, !on);
    }
  }
  orig(cap);
  if (c) c->on = (uint8_t)on;
}
static void h_enable(unsigned cap) { set_cap(cap, 1, o_enable); }
static void h_disable(unsigned cap) { set_cap(cap, 0, o_disable); }

static void set_unit(unsigned *shadow, int group, unsigned pname, unsigned unit,
                     void (*orig)(unsigned)) {
  gstat_t *g = &s_st[group];
  g->calls++;
  if (*shadow == unit) {
    g->redundant++;
    if (s_mode == 1) { g->skipped++; return; }
    if (s_mode == 2 && s_get_integerv) {
      int v = -1;
      s_get_integerv(pname, &v);
      if ((unsigned)v != unit && g->mismatch++ < 8)
        log_printf("[glsf] MISMATCH %s filter=0x%x asl=0x%x", k_group[group], unit, (unsigned)v);
    }
  }
  orig(unit);
  *shadow = unit;
}
static void h_active(unsigned u) { set_unit(&s_unit, G_ACT, 0x84E0, u, o_active); }
static void h_active_arb(unsigned u) { set_unit(&s_unit, G_ACT, 0x84E0, u, o_active_arb); }
static void h_cactive(unsigned u) { set_unit(&s_cunit, G_CACT, 0x84E1, u, o_cactive); }
static void h_cactive_arb(unsigned u) { set_unit(&s_cunit, G_CACT, 0x84E1, u, o_cactive_arb); }

/* ---- install ----------------------------------------------------------------- */
/* Point every relocation slot of sym that holds the engine's own function at
 * repl; returns the original (0 and nothing changed if absent or any slot is
 * already taken). */
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

/* The dead glGetIntegerv in ASLgl::glTexParameteri: verified, then NOP'd or
 * restored. */
static const uint8_t k_texparam_orig[] = {0x69, 0x46, 0x48, 0xf2, 0x69, 0x00, 0x7e, 0xf4, 0xc2, 0xea};
static const uint8_t k_nops[] = {0x00, 0xbf, 0x00, 0xbf};
static uint8_t *s_texparam_at;     /* the 4-byte blx, NULL if the code did not match */
static int s_texparam_nopped;

static void texparam_patch(int nop) {
  if (!s_texparam_at || nop == s_texparam_nopped) return;
  const uint8_t *src = nop ? k_nops : k_texparam_orig + 6;
  kuKernelCpuUnrestrictedMemcpy(s_texparam_at, src, 4);
  kuKernelFlushCaches(s_texparam_at, 4);
  s_texparam_nopped = nop;
}

void gl_state_filter_install(void) {
  struct { const char *sym; void *hook; void **orig; } k[] = {
    {"_ZN5ASLgl8glEnableEj", (void *)h_enable, (void **)&o_enable},
    {"_ZN5ASLgl9glDisableEj", (void *)h_disable, (void **)&o_disable},
    {"_ZN5ASLgl15glActiveTextureEj", (void *)h_active, (void **)&o_active},
    {"_ZN5ASLgl18glActiveTextureARBEj", (void *)h_active_arb, (void **)&o_active_arb},
    {"_ZN5ASLgl21glClientActiveTextureEj", (void *)h_cactive, (void **)&o_cactive},
    {"_ZN5ASLgl24glClientActiveTextureARBEj", (void *)h_cactive_arb, (void **)&o_cactive_arb},
  };
  /* All originals first: a hook must never run with a NULL original. */
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
    uintptr_t f = so_symbol(&kotor_mod, k[i].sym);
    if (!f) { log_printf("[glsf] DISABLED: %s not found", k[i].sym); return; }
    *k[i].orig = (void *)f;
  }
  s_is_enabled = (int (*)(unsigned))so_symbol(&kotor_mod, "_ZN5ASLgl11glIsEnabledEj");
  s_get_integerv = (void (*)(unsigned, int *))so_symbol(&kotor_mod, "_ZN5ASLgl13glGetIntegervEjPi");
  s_mode = GL_STATE_FILTER;
  __sync_synchronize();
  char buf[160];
  int o = 0;
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
    unsigned n = 0;
    uintptr_t orig = take_slots(k[i].sym, (uintptr_t)k[i].hook, &n);
    if (orig) *k[i].orig = (void *)orig;
    o += snprintf(buf + o, sizeof buf - o, " %u", n);
  }
  uint8_t *at = (uint8_t *)(kotor_mod.text_base + GLSF_TEXPARAM_GETI_OFF - 6);
  if (!memcmp(at, k_texparam_orig, sizeof k_texparam_orig)) s_texparam_at = at + 6;
  texparam_patch(s_mode != 0);
  s_installed = 1;
  log_printf("[glsf] armed: mode %d, slots%s (enable disable active activeARB client clientARB); "
             "texparam dead query %s", s_mode, buf,
             s_texparam_at ? (s_texparam_nopped ? "removed" : "kept") : "NOT FOUND (left alone)");
}

void gl_state_filter_set_mode(int mode) {
  if (!s_installed) return;
  s_mode = mode;
  texparam_patch(mode != 0);
}

int gl_state_filter_mode(void) { return s_mode; }

void gl_state_filter_on_swap(uint64_t frame_us) {
  if (frame_us >= 300000) forget();     /* loads: start from nothing */
}

void gl_state_filter_stats(char *buf, int cap, unsigned frames) {
  int o = snprintf(buf, cap, "mode=%d texparam-query=%s", s_mode, s_texparam_nopped ? "off" : "on");
  if (!frames) frames = 1;
  for (int g = 0; g < G_N && o < cap - 80; g++) {
    gstat_t *s = &s_st[g];
    o += snprintf(buf + o, cap - o, " | %s %u/frame, redundant %u%%, skipped %u/frame, mismatches %u",
                  k_group[g], s->calls / frames, s->calls ? (unsigned)(s->redundant * 100ull / s->calls) : 0,
                  s->skipped / frames, s->mismatch);
    if (s->unverifiable) o += snprintf(buf + o, cap - o, " (unverifiable %u)", s->unverifiable);
  }
  memset(s_st, 0, sizeof s_st);
}
