/* ai_census.c -- AI_CENSUS (diagnostic): how often each creature gets an
 * AIUpdate, how many attack, and how many change combat state, every 10 s.
 *
 * In the Khoonda battle on hardware (8 FPS) most enemies stood idle until many
 * had died, and the AI master ran ~3 objects a frame at level 4 (the level
 * AIUpdate moves creatures in combat to). This tells starvation (creatures
 * that get few AIUpdates) from creatures that are updated but never fight,
 * and compares our AI path with the engine's own (ai_thread.txt / ai_mode.txt
 * 0) on the same save. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_census.h"
#include "ai_thread.h"

#if AI_CENSUS

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

#define NC 256
static struct { const void *c; uint32_t upd, atk, combat; uint8_t lvls; } s_c[NC];
static uint32_t s_overflow;
static volatile int s_lock;

static int slot_of(const void *c) {
  uint32_t h = ((uint32_t)(uintptr_t)c >> 4) * 2654435761u;
  for (uint32_t k = 0; k < NC; k++) {
    uint32_t i = (h + k) & (NC - 1);
    if (s_c[i].c == c) return (int)i;
    if (!s_c[i].c) { s_c[i].c = c; return (int)i; }
  }
  s_overflow++;
  return -1;
}
static void bump(const void *c, int what) {
  while (__sync_lock_test_and_set(&s_lock, 1)) {}
  int i = slot_of(c);
  if (i >= 0) { if (what == 0) s_c[i].upd++; else if (what == 1) s_c[i].atk++; else s_c[i].combat++; }
  __sync_lock_release(&s_lock);
}

typedef int (*upd_t)(void *self);
typedef int (*atk_t)(void *self, void *node);
typedef void (*comb_t)(void *self, int a, uint8_t b, int c);
static upd_t o_upd;
static atk_t o_atk;
static comb_t o_comb;
#if BATTLE_LEDGER
/* AIUpdates on the AI worker by the AI level the creature was updated from
 * (CSWSObject+0x78, as CServerAIMaster::SetAILevel reads it): how many, how
 * long without waits at gates, and how many creatures. */
static uint32_t w_lvl_n[5];
static uint64_t w_lvl_us[5], w_lvl_max[5];
static int h_upd(void *self) {
  bump(self, 0);
  if (!ai_thread_on_worker()) return o_upd(self);
  int lvl = *(const int *)((const uint8_t *)self + 0x78);
  uint64_t t0 = sceKernelGetProcessTimeWide(), i0 = ai_thread_idle_us();
  int r = o_upd(self);
  uint64_t d = sceKernelGetProcessTimeWide() - t0, i = ai_thread_idle_us() - i0;
  d = d > i ? d - i : 0;
  if (lvl >= 0 && lvl < 5) {
    w_lvl_n[lvl]++;
    w_lvl_us[lvl] += d;
    if (d > w_lvl_max[lvl]) w_lvl_max[lvl] = d;
    while (__sync_lock_test_and_set(&s_lock, 1)) {}
    int k = slot_of(self);
    if (k >= 0) s_c[k].lvls |= (uint8_t)(1u << lvl);
    __sync_lock_release(&s_lock);
  }
  return r;
}
#else
static int h_upd(void *self) { bump(self, 0); return o_upd(self); }
#endif
static int h_atk(void *self, void *node) { bump(self, 1); return o_atk(self, node); }
static void h_comb(void *self, int a, uint8_t b, int c) { bump(self, 2); o_comb(self, a, b, c); }

void ai_census_install(void) {
  o_upd = (upd_t)take_slots("_ZN12CSWSCreature8AIUpdateEv", (uintptr_t)h_upd);
  o_atk = (atk_t)take_slots("_ZN12CSWSCreature23AIActionPhysicalAttacksEP20CSWSObjectActionNode", (uintptr_t)h_atk);
  o_comb = (comb_t)take_slots("_ZN12CSWSCreature14SetCombatStateEihi", (uintptr_t)h_comb);
  log_printf("[aicensus] armed: AIUpdate %s, attacks %s, combat state %s", o_upd ? "yes" : "NO", o_atk ? "yes" : "NO",
             o_comb ? "yes" : "NO");
}

static uint64_t s_t0;
static uint32_t s_frames;

/* One report: every 10 s from the swap, or, with AI_BUDGET_SCALE_AB, at the
 * end of each of the AI thread's windows with its phase as the label. */
void ai_census_report(const char *label) {
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!s_t0) { s_t0 = now; return; }
  uint32_t frames = s_frames;
  uint64_t t0 = s_t0;
  float secs = (float)(now - t0) / 1e6f;
  uint32_t n = 0, never = 0, lt1 = 0, lt3 = 0, lt10 = 0, ge10 = 0, attackers = 0, attacks = 0, combat = 0, upd = 0;
  uint32_t idle_rare = 0;   /* updated under 1/s and never attacked */
  while (__sync_lock_test_and_set(&s_lock, 1)) {}
  for (int i = 0; i < NC; i++) {
    if (!s_c[i].c) continue;
    n++;
    float r = s_c[i].upd / secs;
    upd += s_c[i].upd;
    if (!s_c[i].upd) never++;
    else if (r < 1.0f) lt1++;
    else if (r < 3.0f) lt3++;
    else if (r < 10.0f) lt10++;
    else ge10++;
    if (s_c[i].atk) { attackers++; attacks += s_c[i].atk; }
    if (s_c[i].combat) combat++;
    if (r < 1.0f && !s_c[i].atk) idle_rare++;
  }
  /* Creatures stay in the table: one that gets no AIUpdate in a later window
   * is counted under "none" there. */
  for (int i = 0; i < NC; i++) s_c[i].upd = s_c[i].atk = s_c[i].combat = 0;
  uint32_t ov = s_overflow;
  s_overflow = 0;
  __sync_lock_release(&s_lock);
#if BATTLE_LEDGER
  {
    char b[400];
    int o = 0;
    unsigned seen[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < NC; i++)
      for (int l = 0; l < 5; l++) if (s_c[i].lvls & (1u << l)) seen[l]++;
    for (int i = 0; i < NC; i++) s_c[i].lvls = 0;
    b[0] = 0;
    for (int l = 4; l >= 0; l--) {
      if (!w_lvl_n[l]) continue;
      o += snprintf(b + o, sizeof b - (size_t)o, " | L%d: %u creatures, %.1f updates a frame, %.2f ms each (worst %.1f)",
                    l, seen[l], frames ? (double)w_lvl_n[l] / frames : 0.0, w_lvl_us[l] / (w_lvl_n[l] * 1000.0),
                    w_lvl_max[l] / 1000.0);
      w_lvl_n[l] = 0;
      w_lvl_us[l] = w_lvl_max[l] = 0;
    }
    log_printf("[aicensus] creature AIUpdates on the AI worker by AI level, without waits at gates%s", o ? b : ": none");
  }
#endif
  log_printf("[aicensus] %s%u frames, %.1f s: %u creatures seen, %u AIUpdates (%.1f a frame) | updates a second per "
             "creature: none %u, <1 %u, 1-3 %u, 3-10 %u, 10+ %u | %u attacked (%u attacks), %u changed combat state "
             "| %u updated under 1/s and never attacked%s",
             label ? label : "", (unsigned)frames, secs, (unsigned)n, (unsigned)upd, frames ? (float)upd / frames : 0.0f, (unsigned)never,
             (unsigned)lt1, (unsigned)lt3, (unsigned)lt10, (unsigned)ge10, (unsigned)attackers, (unsigned)attacks,
             (unsigned)combat, (unsigned)idle_rare, ov ? " (table full)" : "");
  s_frames = 0;
  s_t0 = now;
}

void ai_census_on_swap(void) {
  s_frames++;
#if !AI_BUDGET_SCALE_AB && !NWVM_AB && !BATTLE_LEDGER
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!s_t0) { s_t0 = now; return; }
  if (now - s_t0 >= 10000000u) ai_census_report(NULL);
#endif
}

#else
void ai_census_install(void) {}
void ai_census_on_swap(void) {}
void ai_census_report(const char *label) { (void)label; }
#endif
