/* big_trace.c -- BIG_TRACE (diagnostic): every allocation of BIG_TRACE_MIN_KB
 * or more that the game makes through dynlib, with who asked for it (the
 * immediate caller and the return addresses found on the stack), which
 * allocator served it (the big-block pool or newlib), and when it is freed.
 *
 * Made for the save crash of 2026-11-05: a 16 MB operator new failed while
 * saving. This says which engine code asks for the whole save at once, how
 * big those requests get, and how long they live. */

#include <vitasdk.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "big_trace.h"

#if BIG_TRACE

#define LIVE_MAX 128
static struct { void *p; uint32_t kb; uint64_t t; } s_live[LIVE_MAX];
static unsigned s_nlive;
static volatile int s_lock;

static void lock(void) { while (__sync_lock_test_and_set(&s_lock, 1)) sceKernelDelayThread(10); }
static void unlock(void) { __sync_lock_release(&s_lock); }

/* A Thumb return address: odd, inside the module's text, right after a
 * 32-bit BL/BLX or a 16-bit BLX register. */
static int is_ret(const so_module *m, uint32_t w) {
  if (!(w & 1)) return 0;
  uint32_t a = w - 1;
  if (a < m->text_base + 4 || a >= m->text_base + m->text_size) return 0;
  uint16_t h1 = *(const uint16_t *)(a - 4), h2 = *(const uint16_t *)(a - 2);
  if ((h1 & 0xf800u) == 0xf000u && ((h2 & 0xd000u) == 0xd000u || (h2 & 0xd001u) == 0xc000u)) return 1;
  if ((h2 & 0xff87u) == 0x4780u) return 1;   /* blx rN */
  return 0;
}

static int name_ret(char *out, size_t cap, uint32_t w) {
  if (is_ret(&kotor_mod, w)) return snprintf(out, cap, " k+%x", (unsigned)(w - 1 - kotor_mod.text_base));
  if (is_ret(&cxx_mod, w)) return snprintf(out, cap, " c+%x", (unsigned)(w - 1 - cxx_mod.text_base));
  return 0;
}

void big_trace_alloc(const char *kind, size_t n, void *p, int pool, uint32_t caller) {
  char who[160];
  int o = snprintf(who, sizeof who, " caller %08x", (unsigned)caller);
  if (caller - 1 - kotor_mod.text_base < kotor_mod.text_size)
    o = snprintf(who, sizeof who, " caller k+%x", (unsigned)(caller - 1 - kotor_mod.text_base));
  else if (caller - 1 - cxx_mod.text_base < cxx_mod.text_size)
    o = snprintf(who, sizeof who, " caller c+%x", (unsigned)(caller - 1 - cxx_mod.text_base));

  /* Return addresses on this thread's stack, nearest first, inside its bounds. */
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t top = 0;
  if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0)
    top = (uintptr_t)ti.stack + (uintptr_t)ti.stackSize;
  o += snprintf(who + o, sizeof who - (size_t)o, " | stack");
  int found = 0;
  for (uintptr_t a = sp & ~3u; top && a + 4 <= top && a < sp + 4096 && found < 10; a += 4) {
    int w = name_ret(who + o, sizeof who - (size_t)o, *(const uint32_t *)a);
    if (w > 0 && o + w < (int)sizeof who) { o += w; found++; }
  }

  uint64_t now = sceKernelGetProcessTimeWide();
  if (p) {
    lock();
    if (s_nlive < LIVE_MAX) { s_live[s_nlive].p = p; s_live[s_nlive].kb = (uint32_t)(n >> 10); s_live[s_nlive].t = now; s_nlive++; }
    unlock();
  }
  log_printf("[bigtrace] +%u KB %s %p %s thread %s%s", (unsigned)(n >> 10), kind, p,
             !p ? "FAILED" : pool ? "pool" : "newlib", ti.name[0] ? ti.name : "?", who);
}

void big_trace_free(void *p) {
  if (!p || !s_nlive) return;
  lock();
  for (unsigned i = 0; i < s_nlive; i++) {
    if (s_live[i].p != p) continue;
    uint32_t kb = s_live[i].kb;
    uint64_t t = s_live[i].t;
    s_live[i] = s_live[--s_nlive];
    unlock();
    log_printf("[bigtrace] -%u KB %p after %u ms", (unsigned)kb, p,
               (unsigned)((sceKernelGetProcessTimeWide() - t) / 1000u));
    return;
  }
  unlock();
}

#endif
