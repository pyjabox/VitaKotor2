/* proc_anim.c -- ProceduralAnimateHierarchy without a call per random number
 * (PROC_ANIM).
 *
 * ProceduralAnimateHierarchy(Part*) (libkotor2 0x4d67a8) animates the texture
 * coordinates of procedural meshes, then recurses into the part's children.
 * For a part whose procedural data (vtable +0x34) has a pool (+0x78) and whose
 * MdlNodeTriMesh has a UV channel (+0x138, +0x160):
 *   - A = mesh+0x144; half = A/2; n = (int)(A * 1000);
 *   - the output UVs: GLRender::LockPool(pool, 0, 0, 16);
 *   - per vertex (mesh+0x180, int16), base UV pd+0x90: base += (mesh+0x13c,
 *     +0x140); if n, out.u += K * (rand() % n * 0.001 - half) with K =
 *     mesh+0x148, held within base.u +- half; out.v the same with a second
 *     rand(); then base -= the integer part of vertex 0's base as it was
 *     before the loop -- or, for "LEH_hyper01" (the Ebon Hawk's hyperspace),
 *     (25, 200) once u passed 25, else nothing;
 *   - the output UVs are copied into the model's vertex pool (model+0xac, at
 *     mesh+0x19c, stride mesh+0x14c, attribute mesh+0x160).
 * Each random number was rand_wincompatible() through the PLT and a libgcc
 * division: in the Khoonda battle, 7.4% of the game thread (hardware profile,
 * 2026-11-04). Here the generator (stWinRandSeed = seed * 214013 + 2531011,
 * bits 16-30) is inline and the remainder a multiplication (fastmod.h), with
 * the same sequence of seeds, the same floats in the same order, and the same
 * calls to the engine around them.
 *
 * PROC_ANIM_VERIFY (test builds), two checks:
 *   - each mesh is also animated by a plain transcription that calls the
 *     engine's rand_wincompatible and %, from the same inputs: UVs and seed
 *     must match bit for bit;
 *   - each top-level call first runs the engine's own function, then ours
 *     from the same base UVs and seed: every mesh's base UVs and the final
 *     seed must match (the output UVs live in GL pools, not read here, and
 *     get the jitter twice in such a build).
 *
 * ux0:data/kotor2/proc_mode.txt: 0 off (nothing hooked), 1 on, a on and off
 * alternately every 10 s. Without the file: PROC_ANIM_DEFAULT. A [proc] line
 * every 10 s. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "fastmod.h"
#include "proc_anim.h"

#if PROC_ANIM

#define FLD(p, off, T) (*(T *)((uint8_t *)(p) + (off)))

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

typedef void (*pah_t)(void *part);
typedef void *(*get_t)(void *self);
typedef float *(*lock_t)(uint32_t pool, uint32_t off, uint32_t size, uint32_t flags);
typedef void (*unlock_t)(uint32_t pool);
typedef uint32_t (*rand_t)(void);

static pah_t o_pah;
static get_t f_as_trimesh, f_as_model;
static lock_t f_lock;
static unlock_t f_unlock;
static rand_t f_rand;
static volatile uint32_t *s_seed;
static int s_installed, s_ab, s_on;
static int s_depth;                              /* game thread only */
static uint32_t w_calls, w_meshes, w_verts, w_ver_ok, w_ver_bad, w_ver_skip;
static uint64_t w_us;

/* The per-vertex loop. Fast: inline generator, fastmod. Plain (VERIFY): the
 * engine's rand_wincompatible and %, as the original. */
static void uv_loop(float *base, float *out, int count, float half, int n, float sx, float sy, float k,
                    float wx, float wy, int plain) {
  float s22 = 0.001f, nhalf = -half;
  uint32_t seed = *s_seed;
  fastmod_t fm = fastmod_init(n < 0 ? (uint32_t)(-(int64_t)n) : (uint32_t)n);
  for (int i = 0; i < count; i++) {
    float *v = base + 2 * i, *o = out + 2 * i;
    v[0] = sx + v[0];
    v[1] = sy + v[1];
    if (n) {
      int r;
      if (plain) r = (int)f_rand() % n;
      else { seed = seed * 214013u + 2531011u; r = (int)fastmod(fm, (seed >> 16) & 0x7fffu); }
      float s2 = (float)r * s22 - half;
      float ou = o[0] + k * s2;
      float d = ou - v[0];
      if (d > half) ou = half + v[0];
      else if (d < nhalf) ou = v[0] - half;
      o[0] = ou;
      if (plain) r = (int)f_rand() % n;
      else { seed = seed * 214013u + 2531011u; r = (int)fastmod(fm, (seed >> 16) & 0x7fffu); }
      s2 = (float)r * s22 - half;
      float ov = o[1] + k * s2;
      d = ov - v[1];
      if (d > half) ov = half + v[1];
      else if (d < nhalf) ov = v[1] - half;
      o[1] = ov;
    }
    v[0] = v[0] - wx;
    v[1] = v[1] - wy;
  }
  if (!plain) *s_seed = seed;
}

#if PROC_ANIM_VERIFY
#define VMAX 8192
static float v_base[2 * VMAX], v_out[2 * VMAX], r_base[2 * VMAX], r_out[2 * VMAX];
#endif

static void animate_mesh(void *pd, void *mesh) {
  float a = FLD(mesh, 0x144, float);
  uint32_t pool = FLD(pd, 0x78, uint32_t);
  float *out = f_lock(pool, 0, 0, 16);
  float half = a * 0.5f;
  int n = (int)(a * 1000.0f);
  float *base = FLD(pd, 0x90, float *);
  float wx = (float)(int)base[0], wy = (float)(int)base[1];
  const char *name = (const char *)mesh + 0xa8;
  if (name[0] == 'L' && strstr(name, "LEH_hyper01")) {
    int big = base[0] > 25.0f;
    wx = big ? 25.0f : 0.0f;
    wy = big ? 200.0f : 0.0f;
  }
  int count = FLD(mesh, 0x180, int16_t);
  float sx = FLD(mesh, 0x13c, float), sy = FLD(mesh, 0x140, float), k = FLD(mesh, 0x148, float);
#if PROC_ANIM_VERIFY
  if (count > 0 && count <= VMAX) {
    uint32_t seed0 = *s_seed;
    memcpy(v_base, base, (size_t)count * 8);
    memcpy(v_out, out, (size_t)count * 8);
    uv_loop(base, out, count, half, n, sx, sy, k, wx, wy, 1);   /* the engine's way */
    memcpy(r_base, base, (size_t)count * 8);
    memcpy(r_out, out, (size_t)count * 8);
    uint32_t r_seed = *s_seed;
    memcpy(base, v_base, (size_t)count * 8);
    memcpy(out, v_out, (size_t)count * 8);
    *s_seed = seed0;
    uv_loop(base, out, count, half, n, sx, sy, k, wx, wy, 0);
    if (!memcmp(base, r_base, (size_t)count * 8) && !memcmp(out, r_out, (size_t)count * 8) && *s_seed == r_seed) w_ver_ok++;
    else if (w_ver_bad++ < 8) log_printf("[proc] VERIFY MISMATCH: %d vertices, n %d, seed %08x vs %08x", count, n,
                                         (unsigned)*s_seed, (unsigned)r_seed);
  } else {
    w_ver_skip++;
    uv_loop(base, out, count, half, n, sx, sy, k, wx, wy, 0);
  }
#else
  uv_loop(base, out, count, half, n, sx, sy, k, wx, wy, 0);
#endif
  w_meshes++;
  w_verts += (uint32_t)(count > 0 ? count : 0);
  count = FLD(mesh, 0x180, int16_t);
  uint32_t stride = FLD(mesh, 0x14c, uint32_t);
  void *model = f_as_model(FLD(mesh, 8, void *));
  uint8_t *dst = (uint8_t *)f_lock(FLD(model, 0xac, uint32_t), FLD(mesh, 0x19c, uint32_t), stride * (uint32_t)count, 1);
  uint32_t attr = FLD(mesh, 0x160, uint32_t);
  const uint32_t *src = (const uint32_t *)out;
  for (int i = 0; i < count; i++) {
    uint32_t *d = (uint32_t *)(dst + stride * (uint32_t)i + attr);
    d[0] = src[2 * i];
    d[1] = src[2 * i + 1];
  }
  model = f_as_model(FLD(mesh, 8, void *));
  f_unlock(FLD(model, 0xac, uint32_t));
  f_unlock(FLD(pd, 0x78, uint32_t));
}

static void pah(void *part) {
  void *pd = ((get_t)(*(void ***)part)[0x34 / 4])(part);
  if (pd && FLD(pd, 0x78, int32_t) != -1) {
    void *mesh = f_as_trimesh(FLD(pd, 4, void *));
    if (FLD(mesh, 0x138, uint32_t) && FLD(mesh, 0x160, int32_t) != -1 && FLD(pd, 0x78, int32_t) != -1)
      animate_mesh(pd, mesh);
  }
  for (int i = 0; i < FLD(part, 0x34, int32_t); i++) pah(FLD(part, 0x30, void **)[i]);
}

#if PROC_ANIM_VERIFY
/* The subtree's base UV arrays, as pah would visit them. */
#define OMAX 64
static float *o_ptr[OMAX];
static int o_cnt[OMAX], o_n;
static float o_save[2 * VMAX], o_after[2 * VMAX];
static uint32_t w_orig_ok, w_orig_bad, w_orig_skip, w_orig_seed_only;
static void collect(void *part) {
  void *pd = ((get_t)(*(void ***)part)[0x34 / 4])(part);
  if (pd && FLD(pd, 0x78, int32_t) != -1) {
    void *mesh = f_as_trimesh(FLD(pd, 4, void *));
    if (FLD(mesh, 0x138, uint32_t) && FLD(mesh, 0x160, int32_t) != -1 && o_n < OMAX) {
      o_ptr[o_n] = FLD(pd, 0x90, float *);
      o_cnt[o_n] = FLD(mesh, 0x180, int16_t);
      o_n++;
    }
  }
  for (int i = 0; i < FLD(part, 0x34, int32_t); i++) collect(FLD(part, 0x30, void **)[i]);
}
static int copy_bases(float *buf, int out) {   /* out: buf <- arrays, else arrays <- buf */
  size_t at = 0;
  for (int i = 0; i < o_n; i++) {
    size_t n = (size_t)(o_cnt[i] > 0 ? o_cnt[i] : 0) * 2;
    if (at + n > 2 * VMAX) return 0;
    if (out) memcpy(buf + at, o_ptr[i], n * 4); else memcpy(o_ptr[i], buf + at, n * 4);
    at += n;
  }
  return 1;
}
static void verify_against_engine(void *part) {
  o_n = 0;
  collect(part);
  if (o_n >= OMAX || !copy_bases(o_save, 1)) { w_orig_skip++; pah(part); return; }
  uint32_t seed0 = *s_seed;
  o_pah(part);                       /* its recursion comes back here with s_depth set: the engine's */
  copy_bases(o_after, 1);
  uint32_t seed_e = *s_seed;
  copy_bases(o_save, 0);
  *s_seed = seed0;
  pah(part);
  copy_bases(o_save, 1);             /* ours, into o_save */
  size_t total = 0;
  for (int i = 0; i < o_n; i++) total += (size_t)(o_cnt[i] > 0 ? o_cnt[i] : 0) * 2;
  /* The AI worker calls rand_wincompatible too, beside the drawing: a seed
   * that differs alone can be its doing; bases that differ cannot. */
  int bases_same = !memcmp(o_save, o_after, total * 4);
  if (bases_same && *s_seed == seed_e) w_orig_ok++;
  else if (bases_same) w_orig_seed_only++;
  else if (w_orig_bad++ < 8)
    log_printf("[proc] VERIFY against the engine MISMATCH: %d meshes, %u floats, seed %08x vs %08x", o_n,
               (unsigned)total, (unsigned)*s_seed, (unsigned)seed_e);
}
#endif

static void h_pah(void *part) {
  if (s_depth) {                     /* the engine's own recursion, while it runs (off) */
    o_pah(part);
    return;
  }
  s_depth = 1;
  uint64_t t0 = sceKernelGetProcessTimeWide();
#if PROC_ANIM_VERIFY
  if (s_on) verify_against_engine(part); else o_pah(part);
#else
  if (s_on) pah(part); else o_pah(part);
#endif
  w_us += sceKernelGetProcessTimeWide() - t0;
  w_calls++;
  s_depth = 0;
}

void proc_anim_on_swap(void) {
  if (!s_installed) return;
  static uint64_t t0;
  static uint32_t frames;
  uint64_t now = sceKernelGetProcessTimeWide();
  frames++;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  log_printf("[proc] %s%u frames | %u calls, %.2f ms a frame | %u meshes, %u vertices a frame (ours only)"
#if PROC_ANIM_VERIFY
             " | verify %u same, %u different, %u not checked | against the engine %u same, %u UVs different, %u seed only, %u not checked"
#endif
             , s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)frames, (unsigned)w_calls,
             frames ? w_us / 1000.0 / frames : 0.0, frames ? (unsigned)(w_meshes / frames) : 0,
             frames ? (unsigned)(w_verts / frames) : 0
#if PROC_ANIM_VERIFY
             , (unsigned)w_ver_ok, (unsigned)w_ver_bad, (unsigned)w_ver_skip, (unsigned)w_orig_ok,
             (unsigned)w_orig_bad, (unsigned)w_orig_seed_only, (unsigned)w_orig_skip
#endif
  );
#if PROC_ANIM_VERIFY
  w_orig_ok = w_orig_bad = w_orig_skip = w_orig_seed_only = 0;
#endif
  w_calls = w_meshes = w_verts = w_ver_ok = w_ver_bad = w_ver_skip = 0;
  w_us = 0;
  frames = 0;
  t0 = now;
  if (s_ab) s_on = !s_on;
}

void proc_anim_install(void) {
  char m = PROC_ANIM_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/proc_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char b = 0; if (sceIoRead(fd, &b, 1) == 1) m = b; sceIoClose(fd); }
  if (m != '1' && m != 'a') { log_printf("[proc] off (proc_mode '%c')", m); return; }
  /* rand_wincompatible as this build has it: the multiplier and increment
   * the inline generator copies. */
  uintptr_t rf = so_symbol(&kotor_mod, "rand_wincompatible");
  const uint32_t *lit = (const uint32_t *)(kotor_mod.text_base + 0x5b6b64u);
  s_seed = (volatile uint32_t *)so_symbol(&kotor_mod, "stWinRandSeed");
  f_rand = (rand_t)rf;
  f_as_trimesh = (get_t)so_symbol(&kotor_mod, "_ZN7MdlNode16AsMdlNodeTriMeshEv");
  f_as_model = (get_t)so_symbol(&kotor_mod, "_ZN7MaxTree7AsModelEv");
  f_lock = (lock_t)so_symbol(&kotor_mod, "_ZN8GLRender8LockPoolEmmmm");
  f_unlock = (unlock_t)so_symbol(&kotor_mod, "_ZN8GLRender10UnlockPoolEm");
  if (rf != kotor_mod.text_base + 0x5b6b48u + 1 || lit[0] != 2531011u || lit[1] != 214013u || !s_seed ||
      !f_as_trimesh || !f_as_model || !f_lock || !f_unlock) {
    log_printf("[proc] NOT armed: rand_wincompatible or the functions it needs are not as expected");
    return;
  }
  unsigned n = 0;
  o_pah = (pah_t)take_slots("_Z26ProceduralAnimateHierarchyP4Part", (uintptr_t)h_pah, &n);
  if (!o_pah) { log_printf("[proc] NOT armed: ProceduralAnimateHierarchy slots not found or taken"); return; }
  s_ab = m == 'a';
  s_on = 1;
  s_installed = 1;
  log_printf("[proc] armed: ProceduralAnimateHierarchy %u slots%s%s", n,
             PROC_ANIM_VERIFY ? "; VERIFY: every mesh also animated the engine's way and compared" : "",
             s_ab ? "; A/B: on and off alternately every 10 s" : "");
}

#else
void proc_anim_install(void) {}
void proc_anim_on_swap(void) {}
#endif
