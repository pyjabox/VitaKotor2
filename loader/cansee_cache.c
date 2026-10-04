/* cansee_cache.c -- reuse CSWCCreature::CanSee results for a few frames.
 *
 * CanSee(target) is a line-of-sight raycast, ~0.6 ms each on the Vita; the
 * client asks ~5 per frame for object highlighting (DoPassiveSelection) and
 * input targeting (ProcessInput), ~3 ms per frame. A result is reused for the
 * same (viewer, target) pair for CANSEE_TTL_FRAMES frames. The callers are UI
 * code: a highlight that updates 2 frames late is invisible. Pointers are only
 * compared, never dereferenced. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "cansee_cache.h"

#define CANSEE_TTL_FRAMES 3u
#define TAB_N 64u

typedef struct { const void *self, *target; int result; uint32_t frame; } entry_t;
static entry_t s_tab[TAB_N];
static uint32_t s_frame = 1;
static int s_installed, s_on;
static int (*s_orig)(void *self, void *target);
static uint32_t s_hits, s_misses;

static int cansee_hook(void *self, void *target) {
  if (!s_on) return s_orig(self, target);
  uint32_t h = ((uint32_t)(uintptr_t)self * 2654435761u) ^ ((uint32_t)(uintptr_t)target * 40503u);
  entry_t *e = &s_tab[(h >> 16) & (TAB_N - 1)];
  if (e->self == self && e->target == target && s_frame - e->frame < CANSEE_TTL_FRAMES) {
    s_hits++;
    return e->result;
  }
  int r = s_orig(self, target);
  e->self = self;
  e->target = target;
  e->result = r;
  e->frame = s_frame;
  s_misses++;
  return r;
}

void cansee_cache_install(void) {
  static const char *const k_sym = "_ZN12CSWCCreature6CanSeeEP10CSWCObject";
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
      uintptr_t repl = (uintptr_t)&cansee_hook;
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0) {
      if (!n || bad) {
        log_printf("[cansee] DISABLED: CanSee slots %u, already hooked %u", n, bad);
        return;
      }
      s_orig = (int (*)(void *, void *))callable;
      __sync_synchronize();
    }
  }
  s_installed = 1;
  log_printf("[cansee] armed: CanSee x%u slots, ttl %u frames", n, CANSEE_TTL_FRAMES);
}

void cansee_cache_set(int on) {
  if (on && !s_on) memset(s_tab, 0, sizeof s_tab);
  s_on = s_installed && on;
}

void cansee_cache_on_swap(void) { s_frame++; }

void cansee_cache_stats(uint32_t *hits, uint32_t *misses) {
  *hits = s_hits; *misses = s_misses;
  s_hits = s_misses = 0;
}
