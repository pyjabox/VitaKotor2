/* ai_throttle.c -- spend the server AI's time on what is in combat or moving.
 *
 * CServerAIMaster::UpdateState walks its five AI levels (4 first) and, within
 * each, AIUpdates the objects of that level's CServerAIList round robin
 * (CServerAIList::GetNextObject), until its time budget (10 ms, 60% of what is
 * left to each level) runs out or the list comes back to the frame's first
 * object. Before every AIUpdate it runs a walk pass over the whole level
 * (WalkUpdateLocation_QuickWalk for moving creatures). AIUpdate itself moves
 * creatures to level 4 while they are in combat.
 *
 * On the Vita the AI filled its 10 ms in busy areas. A 3 ms budget (the former
 * ai_budget.c default) cut every level, level 4 included: creatures in combat
 * then went frames without an AIUpdate while the walk pass kept moving them,
 * and turned to face their target late -- party members "moonwalked" to it.
 *
 * This hook (GetNextObject's relocation slot, from UpdateState's call site
 * only) keeps the engine's budget for level 4 and limits the rest:
 *  - level 4 is never touched;
 *  - below it, an object that is not a creature with an action queued or in a
 *    walking state 4-6 (the walk pass's own test) is updated once every
 *    AI_THROTTLE_PERIOD frames, the others are skipped in the round robin;
 *  - below it, once the frame's AI has run AI_THROTTLE_CAP_US, GetNextObject
 *    answers NULL, which ends that level's loop for the frame without moving
 *    its cursor; each frame the lower levels still get AI_THROTTLE_MIN_LOWER_US
 *    after level 4.
 * Perception and heartbeats are on the engine's own timers.
 *
 * UpdateState ends a level's loop when GetNextObject returns NULL or comes back
 * to the frame's first object, so NULL is passed straight through, the first
 * object returned per level per frame is remembered and returned again when the
 * list comes back to it, and skipping never goes past the list's length.
 *
 * Hardware, Dxun fight, 10 s windows alternating with the original (2026-10-11):
 * AI 10.2 -> ~5.3 ms per frame, frame ~45 -> ~39 ms, the 2.9 creatures in
 * combat updated every frame in every window, no moonwalk. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_throttle.h"

#define GNO_CALL_OFF 0x4a8028u   /* blx CServerAIList::GetNextObject@plt in UpdateState */
#define GNO_RET_OFF  0x4a802cu
#define TYPE_CREATURE 5
#define LAST_N 2048u             /* last-update frame per object, pointer-hashed */
#define LEVELS_N 8

typedef struct { const void *obj; uint32_t frame; } last_t;
typedef struct { const void *list; uint32_t frame; const void *marker; } level_t;

static last_t s_last[LAST_N];
static level_t s_level[LEVELS_N];
static uintptr_t s_ret, s_top_list;      /* the level-4 list sits at the highest address */
static uint32_t s_frame = 1, s_t_frame;
static unsigned s_period = AI_THROTTLE_PERIOD;
static int s_installed, s_on = 1, s_ab;
static void *(*s_orig)(void *list);
static uint64_t s_t_first, s_t_last, s_lower_t0, s_window_t0;

static uint32_t w_frames, w_updates, w_top, w_skipped, w_capped;
static uint64_t w_us, w_top_us;

static last_t *last_slot(const void *obj) {
  return &s_last[((uint32_t)(uintptr_t)obj * 2654435761u) >> 21 & (LAST_N - 1)];
}

static level_t *level_slot(const void *list) {
  for (int i = 0; i < LEVELS_N; i++)
    if (s_level[i].list == list) return &s_level[i];
  for (int i = 0; i < LEVELS_N; i++)
    if (!s_level[i].list) { s_level[i].list = list; return &s_level[i]; }
  return &s_level[LEVELS_N - 1];
}

/* The walk pass's own test: an action at the head of the queue, or a walking
 * state. The creature comes from the object's AsSWSCreature (vtable +0x34). */
static int creature_active(void *obj) {
  void *(*as_creature)(void *) = (*(void *(***)(void *))obj)[0x34 / 4];
  uint8_t *c = as_creature(obj);
  if (!c) return 0;
  void **queue = *(void ***)(c + 0x100);
  if (queue && *queue && *(void **)((uint8_t *)*queue + 8)) return 1;
  int state = *(int *)(c + 0x11b4);
  return state >= 4 && state <= 6;
}

static int must_update(void *obj) {
  last_t *e = last_slot(obj);
  if (e->obj != obj || s_frame - e->frame >= s_period) return 1;
  return ((const uint8_t *)obj)[8] == TYPE_CREATURE && creature_active(obj);
}

static void *gno_hook(void *list) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  if (ra != s_ret) return s_orig(list);

  uint64_t now = sceKernelGetProcessTimeWide();
  if (s_t_frame != s_frame) { s_t_frame = s_frame; s_t_first = now; s_lower_t0 = 0; }
  s_t_last = now;
  if ((uintptr_t)list > s_top_list) s_top_list = (uintptr_t)list;
  int top = (uintptr_t)list == s_top_list;

  if (!top) {
    if (!s_lower_t0) { s_lower_t0 = now; w_top_us += now - s_t_first; }
    if (s_on) {
      uint64_t deadline = s_t_first + AI_THROTTLE_CAP_US;
      if (deadline < s_lower_t0 + AI_THROTTLE_MIN_LOWER_US)
        deadline = s_lower_t0 + AI_THROTTLE_MIN_LOWER_US;
      if (now > deadline) { w_capped++; return NULL; }
    }
  }

  level_t *L = level_slot(list);
  int first = L->frame != s_frame;
  if (first) { L->frame = s_frame; L->marker = NULL; }

  void *obj = s_orig(list);
  if (s_on && obj && !top) {
    int n = ((const int *)list)[1];
    for (int k = 0; k < n && obj && obj != L->marker && !must_update(obj); k++) {
      w_skipped++;
      obj = s_orig(list);
    }
  }
  if (!obj) { if (first) L->marker = NULL; return NULL; }
  if (first) L->marker = obj;
  else if (obj == L->marker) return obj;        /* back at the start: the level is done */

  last_t *e = last_slot(obj);
  e->obj = obj;
  e->frame = s_frame;
  w_updates++;
  if (top) w_top++;
  return obj;
}

void ai_throttle_install(unsigned period, int ab) {
  static const char *const k_sym = "_ZN13CServerAIList13GetNextObjectEv";
  if (period >= 2) s_period = period;
  s_ab = ab;
  const uint16_t *insn = (const uint16_t *)(kotor_mod.text_base + GNO_CALL_OFF);
  if (insn[0] != 0xf5c9 || insn[1] != 0xea44) {
    log_printf("[aithrottle] DISABLED: unexpected code at UpdateState's GetNextObject call (%04x %04x)",
               insn[0], insn[1]);
    return;
  }
  s_ret = kotor_mod.text_base + GNO_RET_OFF;
  uintptr_t original = so_symbol(&kotor_mod, k_sym);
  unsigned n = 0, bad = 0;
  uintptr_t callable = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + sym->st_name, k_sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      uintptr_t repl = (uintptr_t)&gno_hook;
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0) {
      if (!n || bad) {
        log_printf("[aithrottle] DISABLED: GetNextObject slots %u, already hooked %u", n, bad);
        return;
      }
      s_orig = (void *(*)(void *))callable;
      __sync_synchronize();
    }
  }
  s_installed = 1;
  log_printf("[aithrottle] armed: below level 4, idle objects every %u frames, AI capped at %u us "
             "(at least %u us after level 4)%s", s_period, (unsigned)AI_THROTTLE_CAP_US,
             (unsigned)AI_THROTTLE_MIN_LOWER_US,
             s_ab ? "; A/B: alternates with the engine's own every 10 s" : "");
}

void ai_throttle_on_swap(void) {
  if (!s_installed) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (s_t_frame == s_frame) w_us += s_t_last - s_t_first;
  w_frames++;
  if (++s_frame == 0) s_frame = 1;
  if (!s_window_t0) s_window_t0 = now;
  if (now - s_window_t0 < 10000000u) return;
  log_printf("[aithrottle] %s%u frames: updates %.1f/frame (level 4: %.1f), skipped %.1f, "
             "cut %.1f, level 4 %.2f ms, AI %.2f ms/frame (first to last request)",
             s_ab ? (s_on ? "THROTTLE " : "FULL ") : "", w_frames,
             (double)w_updates / w_frames, (double)w_top / w_frames,
             (double)w_skipped / w_frames, (double)w_capped / w_frames,
             (double)w_top_us / w_frames / 1000.0, (double)w_us / w_frames / 1000.0);
  w_frames = w_updates = w_top = w_skipped = w_capped = 0;
  w_us = w_top_us = 0;
  s_window_t0 = now;
  if (s_ab) s_on = !s_on;
}
