/* ai_list_cache.c -- stop CServerAIMaster::UpdateState re-looking-up objects it
 * cannot act on.
 *
 * For each AI level, UpdateState runs a "walk" pass over every entry of that
 * level's CServerAIList (an array of object ids) and, for creatures in certain
 * movement states, calls CSWSCreature::WalkUpdateLocation_QuickWalk. It runs
 * that whole pass again after every object it AIUpdates (the loop at
 * UpdateState+0x31c branches back to +0xce), so a frame makes about 130 passes
 * over ~255 entries -- ~33k CServerAIList::GetObjectAtPosition calls, each a
 * CServerExoApp::GetGameObject table lookup. Only the ~19 creatures are ever
 * acted on: for any other object the pass body is a no-op (AsSWSCreature()
 * returns NULL).
 *
 * This replaces GetObjectAtPosition's PLT slot. Calls from that one call site
 * (return address UpdateState+0xe8) for an id already seen this frame as a
 * non-creature return NULL at once, which is exactly what the pass does with
 * such an object anyway. Everything else -- other call sites, creatures, ids
 * not yet seen this frame -- goes to the original. The cache is invalidated
 * every frame, so a destroyed or new object is at most one frame stale, and
 * only a non-creature id reused by a new creature within the same frame could
 * ever differ (object ids are not reused like that). */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_list_cache.h"

#define WALK_CALL_OFF 0x4a7ea8u   /* blx GetObjectAtPosition@plt in UpdateState */
#define WALK_RET_OFF  0x4a7eacu
#define CACHE_N 1024u
#define TYPE_CREATURE 5

typedef struct { uint32_t id, gen; } entry_t;
static entry_t s_cache[CACHE_N];
static uint32_t s_gen = 1;
static int s_on, s_installed;
static uintptr_t s_walk_ret;
static void *(*s_orig)(void *list, int pos);
static uint32_t s_hits, s_misses, s_other;

static void *at_hook(void *list, int pos) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  if (!s_on || ra != s_walk_ret) { s_other++; return s_orig(list, pos); }
  const uint32_t *ids = *(uint32_t *const *)list;
  int n = ((const int *)list)[1];
  if (!ids || pos < 0 || pos >= n) return s_orig(list, pos);
  uint32_t id = ids[pos];
  entry_t *e = &s_cache[(id * 2654435761u) >> 22];   /* 10-bit hash */
  if (e->id == id && e->gen == s_gen) { s_hits++; return NULL; }
  s_misses++;
  void *obj = s_orig(list, pos);
  if (!obj || ((const uint8_t *)obj)[8] != TYPE_CREATURE) { e->id = id; e->gen = s_gen; }
  return obj;
}

void ai_list_cache_install(void) {
  static const char *const k_sym = "_ZN13CServerAIList19GetObjectAtPositionEi";
  /* Binary check: the walk-pass call site must be the expected Thumb-2 BLX. */
  const uint16_t *insn = (const uint16_t *)(kotor_mod.text_base + WALK_CALL_OFF);
  if (insn[0] != 0xf5c9 || insn[1] != 0xeae6) {
    log_printf("[ailist] DISABLED: unexpected code at UpdateState walk call (%04x %04x)",
               insn[0], insn[1]);
    return;
  }
  s_walk_ret = kotor_mod.text_base + WALK_RET_OFF;
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
      uintptr_t repl = (uintptr_t)&at_hook;
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0) {
      if (!n || bad) {
        log_printf("[ailist] DISABLED: GetObjectAtPosition slots %u, already hooked %u", n, bad);
        return;
      }
      s_orig = (void *(*)(void *, int))callable;
      __sync_synchronize();
    }
  }
  s_installed = 1;
  log_printf("[ailist] armed: GetObjectAtPosition x%u, walk-pass return %p", n, (void *)s_walk_ret);
}

void ai_list_cache_set(int on) { s_on = s_installed && on; }

void ai_list_cache_on_swap(void) {
  if (++s_gen == 0) s_gen = 1;
}

void ai_list_cache_stats(uint32_t *hits, uint32_t *misses, uint32_t *other) {
  *hits = s_hits; *misses = s_misses; *other = s_other;
  s_hits = s_misses = s_other = 0;
}
