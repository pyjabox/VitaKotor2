/* ai_events.c -- AI_EVENTS_DIAG (diagnostic): what goes into the server's AI
 * event queue, by event type and by caller, and how long the queue is.
 *
 * Saving a module writes the whole queue (CServerAIMaster::SaveEventQueue,
 * each event with its script's stack). In Vita3K the module's save data was
 * 179 KB after 20 s of fighting and 6.5-7.5 MB after 20 s on the "enemy
 * sighted" pause, with the engine's own AI loop as with ours; the save that
 * crashed on hardware needed 16 MB. This says which events pile up while the
 * game is paused, and whether the pile keeps growing.
 *
 * Queue: CServerAIMaster+0x54, a CExoLinkedList whose internal object keeps
 * the count at +8 (CExoLinkedListInternal::RemoveHead). */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_events.h"

#if AI_EVENTS_DIAG

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

typedef uint32_t (*add_t)(void *self, uint32_t a, uint32_t b, uint32_t caller, uint32_t target, uint32_t event,
                          void *data);
static add_t o_delta, o_abs;
static void *volatile s_master;
static volatile int s_lock;

#define NTYPES 64
#define NCALLERS 32
static uint32_t c_type[NTYPES], c_type_data[NTYPES], c_adds, c_adds_data;
static struct { uint32_t ret, n; } c_caller[NCALLERS];

static void note(void *self, uint32_t event, void *data, uint32_t ret) {
  while (__sync_lock_test_and_set(&s_lock, 1)) {}
  s_master = self;
  c_adds++;
  if (data) c_adds_data++;
  uint32_t t = event < NTYPES ? event : NTYPES - 1;
  c_type[t]++;
  if (data) c_type_data[t]++;
  int i;
  for (i = 0; i < NCALLERS && c_caller[i].n && c_caller[i].ret != ret; i++) {}
  if (i < NCALLERS) { c_caller[i].ret = ret; c_caller[i].n++; }
  __sync_lock_release(&s_lock);
}

static uint32_t h_delta(void *self, uint32_t days, uint32_t time, uint32_t caller, uint32_t target, uint32_t event,
                        void *data) {
  note(self, event, data, (uint32_t)(uintptr_t)__builtin_return_address(0));
  return o_delta(self, days, time, caller, target, event, data);
}
static uint32_t h_abs(void *self, uint32_t days, uint32_t time, uint32_t caller, uint32_t target, uint32_t event,
                      void *data) {
  note(self, event, data, (uint32_t)(uintptr_t)__builtin_return_address(0));
  return o_abs(self, days, time, caller, target, event, data);
}

void ai_events_install(void) {
  o_delta = (add_t)take_slots("_ZN15CServerAIMaster17AddEventDeltaTimeEmmmmmPv", (uintptr_t)h_delta);
  o_abs = (add_t)take_slots("_ZN15CServerAIMaster20AddEventAbsoluteTimeEmmmmmPv", (uintptr_t)h_abs);
  log_printf("[aievents] armed: delta %s, absolute %s", o_delta ? "yes" : "NO", o_abs ? "yes" : "NO");
}

void ai_events_on_swap(void) {
  static uint64_t t0;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  t0 = now;
  uint32_t queue = 0;
  void *m = s_master;
  if (m) {
    uint8_t *internal = *(uint8_t **)((uint8_t *)m + 0x54);
    if (internal) queue = *(uint32_t *)(internal + 8);
  }
  char types[300], callers[400];
  int o = 0, oc = 0;
  types[0] = callers[0] = 0;
  while (__sync_lock_test_and_set(&s_lock, 1)) {}
  for (int k = 0; k < 8; k++) {
    uint32_t best = 0, bi = 0;
    for (uint32_t i = 0; i < NTYPES; i++) if (c_type[i] > best) { best = c_type[i]; bi = i; }
    if (!best) break;
    o += snprintf(types + o, sizeof types - (size_t)o, " %u:%u(%u with data)", (unsigned)bi, (unsigned)best,
                  (unsigned)c_type_data[bi]);
    c_type[bi] = 0;
  }
  for (int k = 0; k < 8; k++) {
    uint32_t best = 0; int bi = -1;
    for (int i = 0; i < NCALLERS; i++) if (c_caller[i].n > best) { best = c_caller[i].n; bi = i; }
    if (bi < 0) break;
    uint32_t r = c_caller[bi].ret;
    if (r - 1 - kotor_mod.text_base < kotor_mod.text_size)
      oc += snprintf(callers + oc, sizeof callers - (size_t)oc, " k+%x:%u", (unsigned)(r - 1 - kotor_mod.text_base),
                     (unsigned)best);
    else
      oc += snprintf(callers + oc, sizeof callers - (size_t)oc, " %08x:%u", (unsigned)r, (unsigned)best);
    c_caller[bi].n = 0;
  }
  uint32_t adds = c_adds, with = c_adds_data;
  memset(c_type, 0, sizeof c_type);
  memset(c_type_data, 0, sizeof c_type_data);
  memset(c_caller, 0, sizeof c_caller);
  c_adds = c_adds_data = 0;
  __sync_lock_release(&s_lock);
  log_printf("[aievents] queue %u events | 10 s: %u added (%u with data) | by type%s | by caller%s", (unsigned)queue,
             (unsigned)adds, (unsigned)with, types, callers);
}

#else
void ai_events_install(void) {}
void ai_events_on_swap(void) {}
#endif
