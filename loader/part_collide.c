/* part_collide.c -- particles' collision tests (PART_COLLIDE).
 *
 * The blaster-impact effects of a battle (v_blaster_imp, v_spkb_imp,
 * v_spkr_imp, v_spksplat_imp: up to ~950 live particles in the Khoonda battle)
 * have the bounce flag. In PartEmitter::moveParticleStandard (0x525dc8-
 * 0x525e16), every particle not yet at rest calls the scene's HitCheck
 * (vtable +0x3c, Scene::HitCheck 0x536674) for its step, each frame, inside
 * Scene::DoEmitterBucket -- which grew from 4-12 to 18-31 ms a frame on
 * hardware once the enemies fought (2026-11-05). HitCheck tests the rooms'
 * boxes, then each part's and its AABB tree's, all with BoxIntersect
 * (0x5778e8, through the PLT from HitCheck, HitPartOnBSPNode,
 * HitCheckAABBnode and PartTriMesh::HitCheckGeom).
 *
 * BoxIntersect(start, end, min, max), read in the disassembly: SetOpCode gives
 * each point a 6-bit outcode (x < min 0x20, x > max 0x10, y 0x08/0x04, z
 * 0x02/0x01, strict ordered compares: a NaN coordinate sets no bit); the start
 * inside the box returns -1, two codes sharing a bit return 0, and only
 * otherwise does it clip the segment against the box's faces. The two quick
 * answers are done here inline -- the same compares on the same floats, so the
 * same result for every input -- and the rest goes to the engine's function.
 *
 * PART_COLLIDE_STATS (diagnostic builds): calls a frame, quick answers, and
 * the time and count of particles' HitCheck calls, in a [partcol] line every
 * 10 s. part_collide_mode.txt 0: the engine's BoxIntersect. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "part_collide.h"

#if PART_COLLIDE

static uintptr_t take_slots(const char *sym, uintptr_t repl) {
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
    if (pass == 0 && (!n || bad)) return 0;
  }
  return callable;
}

/* SetOpCode, inline: the engine's bits, its compares (vcmpe; bpl/ble). */
static inline unsigned opcode(const float *p, const float *mn, const float *mx) {
  unsigned c = 0;
  if (p[0] < mn[0]) c = 0x20;
  else if (p[0] > mx[0]) c = 0x10;
  if (p[1] < mn[1]) c |= 0x08;
  else if (p[1] > mx[1]) c |= 0x04;
  if (p[2] < mn[2]) c |= 0x02;
  else if (p[2] > mx[2]) c |= 0x01;
  return c;
}

typedef int (*boxint_t)(const float *a, const float *b, const float *mn, const float *mx);
static boxint_t o_boxint;
static int s_on = 1;
#if PART_COLLIDE_STATS
static uint32_t w_box, w_box_in, w_box_out;
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

#if PART_COLLIDE_VERIFY
/* PART_COLLIDE_VERIFY (test builds): every quick answer against the engine's. */
static uint32_t w_ver_same, w_ver_diff, s_ver_logged;
static int verify(int ours, const float *a, const float *b, const float *mn, const float *mx) {
  int theirs = o_boxint(a, b, mn, mx);
  if (theirs == ours) { w_ver_same++; return ours; }
  w_ver_diff++;
  if (s_ver_logged++ < 10)
    log_printf("[partcol] VERIFY mismatch: ours %d, engine %d for (%g %g %g)-(%g %g %g) in (%g %g %g)-(%g %g %g)", ours,
               theirs, (double)a[0], (double)a[1], (double)a[2], (double)b[0], (double)b[1], (double)b[2], (double)mn[0],
               (double)mn[1], (double)mn[2], (double)mx[0], (double)mx[1], (double)mx[2]);
  return theirs;
}
#define ANSWER(v) return verify((v), a, b, mn, mx)
#else
#define ANSWER(v) return (v)
#endif

static int h_boxint(const float *a, const float *b, const float *mn, const float *mx) {
  STAT(w_box++);
  if (!s_on) return o_boxint(a, b, mn, mx);
  unsigned c0 = opcode(a, mn, mx);
  if (!c0) { STAT(w_box_in++); ANSWER(-1); }          /* the start is inside */
  if (c0 & opcode(b, mn, mx)) { STAT(w_box_out++); ANSWER(0); }   /* both beyond one face */
  return o_boxint(a, b, mn, mx);
}

/* Scene::HitCheck(Vector a, Vector b, Vector *out, CHitInfo *hi), softfp: the
 * vectors' floats travel as words. From a particle's move (bouncing sparks):
 * not made at all unless spark_collision.txt says 1 -- the user's choice
 * (2026-11-05: 300-660 a frame at ~20 us on hardware, 6-13 ms of the battle's
 * frames). The engine then takes it as no hit (moveParticleStandard 0x525e18:
 * nothing is touched), so a spark flies on through the floor, where the depth
 * test hides it, and lives its normal life. Every other caller is untouched. */
#define MOVE_LO 0x5258a0u        /* PartEmitter::moveParticleStandard */
#define MOVE_HI 0x526204u
typedef int (*hitcheck_t)(void *scene, uint32_t ax, uint32_t ay, uint32_t az, uint32_t bx, uint32_t by, uint32_t bz,
                          void *out, void *hi);
static hitcheck_t o_hit;
static int s_spark_skip = 1;
static uint32_t w_hit_skipped;
#if PART_COLLIDE_STATS
static uint32_t w_hit_part, w_hit_other, w_hit_yes, w_box_part;
static uint64_t w_hit_part_us;
static int s_in_part;
#endif
static int h_hit(void *scene, uint32_t ax, uint32_t ay, uint32_t az, uint32_t bx, uint32_t by, uint32_t bz, void *out,
                 void *hi) {
  uint32_t off = (uint32_t)(((uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1) - kotor_mod.text_base);
#if PART_COLLIDE_STATS
  if (off < MOVE_LO || off >= MOVE_HI || s_in_part) {
    w_hit_other++;
    return o_hit(scene, ax, ay, az, bx, by, bz, out, hi);
  }
#else
  if (off < MOVE_LO || off >= MOVE_HI) return o_hit(scene, ax, ay, az, bx, by, bz, out, hi);
#endif
  if (s_spark_skip) { w_hit_skipped++; return 0; }
#if !PART_COLLIDE_STATS
  return o_hit(scene, ax, ay, az, bx, by, bz, out, hi);
#else
  uint32_t b0 = w_box;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  s_in_part = 1;
  int r = o_hit(scene, ax, ay, az, bx, by, bz, out, hi);
  s_in_part = 0;
  w_hit_part_us += sceKernelGetProcessTimeWide() - t0;
  w_hit_part++;
  w_box_part += w_box - b0;
  if (r) w_hit_yes++;
  return r;
#endif
}
static uint64_t s_t0;
static uint32_t s_frames;

void part_collide_install(void) {
  char m = '1';
  SceUID fd = sceIoOpen(DATA_PATH "/part_collide_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char ch = 0; if (sceIoRead(fd, &ch, 1) == 1) m = ch; sceIoClose(fd); }
  s_on = m != '0';
  /* The function the quick answers stand for must be the one read. */
  uintptr_t fn = so_symbol(&kotor_mod, "_Z12BoxIntersectRK6VectorS1_S1_S1_");
  if (fn != kotor_mod.text_base + 0x5778e8u + 1) {
    log_printf("[partcol] NOT armed: BoxIntersect is not at the expected place");
    return;
  }
  o_boxint = (boxint_t)take_slots("_Z12BoxIntersectRK6VectorS1_S1_S1_", (uintptr_t)h_boxint);
  {
    char sc = '0';
    SceUID f2 = sceIoOpen(DATA_PATH "/spark_collision.txt", SCE_O_RDONLY, 0);
    if (f2 >= 0) { char ch = 0; if (sceIoRead(f2, &ch, 1) == 1) sc = ch; sceIoClose(f2); }
    s_spark_skip = sc != '1';
  }
  /* The call site the skip keys on must be the one read. */
  if (so_symbol(&kotor_mod, "_ZN11PartEmitter20moveParticleStandardEf") == kotor_mod.text_base + MOVE_LO + 1)
    o_hit = (hitcheck_t)take_slots("_ZN5Scene8HitCheckE6VectorS0_PS0_P8CHitInfo", (uintptr_t)h_hit);
  log_printf("[partcol] BoxIntersect quick answers %s; sparks' collision %s%s",
             o_boxint ? (s_on ? "on" : "off (mode 0)") : "NOT armed",
             !o_hit ? "as the engine's (HitCheck not hooked)" : s_spark_skip ? "skipped (spark_collision.txt 1: on)" : "on",
             PART_COLLIDE_STATS && o_hit ? "; timed" : "");
}

void part_collide_on_swap(void) {
  if (!o_boxint && !o_hit) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  s_frames++;
  if (!s_t0) { s_t0 = now; return; }
  if (now - s_t0 < 10000000u) return;
  double f = s_frames ? s_frames : 1;
  if (s_spark_skip && o_hit)
    log_printf("[partcol] %u frames | sparks' collision checks skipped: %.1f a frame", (unsigned)s_frames,
               w_hit_skipped / f);
  w_hit_skipped = 0;
#if PART_COLLIDE_STATS
  log_printf("[partcol] %u frames | BoxIntersect %.0f a frame (%.0f%% start inside, %.0f%% beyond one face) | "
             "particles' HitCheck %.1f a frame, %.2f ms a frame (%.1f us each, %.0f box tests each, %.1f hit a frame); "
             "other HitCheck %.1f a frame",
             (unsigned)s_frames, w_box / f, w_box ? 100.0 * w_box_in / w_box : 0.0,
             w_box ? 100.0 * w_box_out / w_box : 0.0, w_hit_part / f, w_hit_part_us / f / 1000.0,
             w_hit_part ? (double)w_hit_part_us / w_hit_part : 0.0, w_hit_part ? (double)w_box_part / w_hit_part : 0.0,
             w_hit_yes / f, w_hit_other / f);
#if PART_COLLIDE_VERIFY
  log_printf("[partcol] VERIFY: %u quick answers the same as the engine's, %u different", (unsigned)w_ver_same,
             (unsigned)w_ver_diff);
  w_ver_same = w_ver_diff = 0;
#endif
  w_box = w_box_in = w_box_out = w_hit_part = w_hit_other = w_hit_yes = w_box_part = 0;
  w_hit_part_us = 0;
#endif
  s_frames = 0;
  s_t0 = now;
}

#else
void part_collide_install(void) {}
void part_collide_on_swap(void) {}
#endif
