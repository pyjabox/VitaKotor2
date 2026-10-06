/* pc_prof.c -- statistical PC profiler for the engine (PC_PROF; diagnostic
 * builds only).
 *
 * A user thread cannot read another thread's registers on the Vita, but
 * kubridge delivers a prefetch abort to a user handler with the full register
 * context and resumes the thread when the handler returns. So:
 *  - a sampler thread on core 2 removes EXEC from libkotor2's code every
 *    PC_PROF_PERIOD_US (kuKernelMemProtect);
 *  - the next engine instruction any thread fetches faults;
 *  - the handler records the PC and the r7 frame chain (libkotor2 keeps r7
 *    frames: [r7] = caller's r7, [r7+4] = return address), restores EXEC and
 *    returns.
 * Time the game thread spends outside the engine (vitaGL, libc, the loader,
 * kernel waits) lands on the engine instruction it returns to, i.e. it is
 * counted in the calling engine function.
 *
 * Before touching the engine, a self-test runs the same cycle on a one-page
 * code block from a throwaway thread; if that thread does not come back, the
 * profiler stays off.
 *
 * Every 10 s: samples per thread kind, cost of a sample, and the top engine
 * functions by self samples (PC) and inclusive samples (PC + frame chain),
 * as a share of the game thread's samples. Names are the mangled dynamic
 * symbols; a PC past a symbol's size is shown as "~name" (a non-exported
 * function after it). */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "pc_prof.h"
#include "crash.h"

#if PC_PROF

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)   /* as in so_util.c */
#endif

#define DEPTH 32

static uintptr_t s_lo, s_hi;                 /* protected range: .text only (see pc_prof_start) */
static uintptr_t s_eng_lo, s_eng_hi;         /* the engine's whole code segment, for symbols */
static SceUID s_game = -1;
static uintptr_t s_stk_lo, s_stk_hi;         /* game thread stack */
static KuKernelExceptionHandler s_old_prefetch, s_old_data;

/* Faults that are not ours go to the handler registered before (crash.c),
 * or straight to the crash report if kubridge did not return it. */
static void pass_on(KuKernelExceptionHandler old, KuKernelExceptionContext *c) {
  if (old) old(c); else crash_report(c);
}

/* One sample at a time. The sampler moves IDLE -> ARMING, protects, -> ARMED;
 * the first faulting thread moves ARMED -> TAKEN, records, restores, -> IDLE.
 * A protection change is not atomic: while the kernel rewrites the mapping,
 * the range can be briefly unmapped (hardware run real-vita-pcprof2: a data
 * read of .rodata took a translation fault). Faults in the range in any other
 * state are retried by returning. */
enum { ST_IDLE, ST_ARMING, ST_ARMED, ST_TAKEN };
static volatile uint32_t s_state;
static volatile uint32_t s_disabled;

/* Symbol table: engine functions sorted by offset. */
typedef struct { uint32_t lo, hi, name; } fn_t;
static fn_t *s_fn;
static int s_nfn;
/* Per function: self samples inside it, self samples past its end (~name),
 * inclusive samples. */
static uint32_t *s_self, *s_after, *s_incl;
static uint32_t s_game_samples, s_other_samples, s_nosym, s_chain_frames;
static uint32_t s_retry_fetch, s_retry_data;  /* faults retried during a protection change */
static uint32_t s_data_samples;              /* samples taken on a data read */
static uint32_t s_sample_us;                 /* handler time (restore included) */
static uint32_t s_arm_fail;
static int s_arm_rc;

/* Raw samples for offline analysis, written by the sampler thread to
 * ux0:data/kotor2/pcprof.bin. Header: "PCP2", text base, text size, protected
 * range lo/hi (5 words). Then one record per game-thread sample: process time
 * in ms (the log's clock), n, LR as an engine offset (0 if outside), then n
 * engine offsets: the PC, then the return addresses of the frame chain. */
#define RAW_WORDS (1u << 18)                  /* 1 MB ring */
static uint32_t *s_raw;
static volatile uint32_t s_raw_head, s_raw_tail;  /* free-running; head: sample owner, tail: sampler */
static uint32_t s_raw_drops, s_raw_written;
static SceUID s_raw_fd = -1;

static volatile uint32_t s_testing, s_test_faults;
static uintptr_t s_test_lo;

static int fn_find(uint32_t off) {
  int lo = 0, hi = s_nfn - 1, r = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (s_fn[mid].lo <= off) { r = mid; lo = mid + 1; } else hi = mid - 1;
  }
  return r;
}

/* Returns a counter index: 2*i (inside fn i) or 2*i+1 (past its end), -1. */
static int fn_slot(uintptr_t pc) {
  if (pc < s_eng_lo || pc >= s_eng_hi) return -1;
  uint32_t off = (uint32_t)(pc - kotor_mod.text_base);
  int i = fn_find(off);
  if (i < 0) return -1;
  return 2 * i + (off < s_fn[i].hi ? 0 : 1);
}

static void record(KuKernelExceptionContext *c, uintptr_t pc) {
  if (sceKernelGetThreadId() != s_game) { s_other_samples++; return; }
  s_game_samples++;
  int seen[DEPTH + 1], ns = 0;
  uint32_t addr[DEPTH + 1], na = 0;
  addr[na++] = (uint32_t)(pc - kotor_mod.text_base);
  /* At a function's first instructions r7 is still the caller's frame, so
   * the chain misses the caller: LR has it. Recorded raw (0 if outside). */
  uintptr_t lr = c->lr & ~(uintptr_t)1;
  uint32_t lr_off = lr >= s_eng_lo && lr < s_eng_hi ? (uint32_t)(lr - kotor_mod.text_base) : 0;
  int k = fn_slot(pc);
  if (k < 0) s_nosym++;
  else {
    if (k & 1) s_after[k >> 1]++; else s_self[k >> 1]++;
    seen[ns++] = k >> 1;
  }
  /* Frame chain; a frame outside the stack or outside the engine ends it. */
  uintptr_t fp = c->r7;
  for (int d = 0; d < DEPTH; d++) {
    if (fp < s_stk_lo || fp + 8 > s_stk_hi || (fp & 3)) break;
    uintptr_t ret = ((const uint32_t *)fp)[1], next = ((const uint32_t *)fp)[0];
    int f = fn_slot(ret & ~(uintptr_t)1);
    if (f < 0) break;
    addr[na++] = (uint32_t)((ret & ~(uintptr_t)1) - kotor_mod.text_base);
    f >>= 1;
    int dup = 0;
    for (int j = 0; j < ns; j++) if (seen[j] == f) { dup = 1; break; }
    if (!dup && ns <= DEPTH) seen[ns++] = f;
    s_chain_frames++;
    if (next <= fp) break;
    fp = next;
  }
  for (int j = 0; j < ns; j++) s_incl[seen[j]]++;
  if (s_raw) {
    uint32_t h = s_raw_head;
    if (h - s_raw_tail + 3 + na > RAW_WORDS) { s_raw_drops++; return; }
    s_raw[h++ & (RAW_WORDS - 1)] = (uint32_t)(sceKernelGetProcessTimeWide() / 1000u);
    s_raw[h++ & (RAW_WORDS - 1)] = na;
    s_raw[h++ & (RAW_WORDS - 1)] = lr_off;
    for (uint32_t j = 0; j < na; j++) s_raw[h++ & (RAW_WORDS - 1)] = addr[j];
    __sync_synchronize();
    s_raw_head = h;
  }
}

static void restore(void) {
  kuKernelMemProtect((void *)s_lo, s_hi - s_lo, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
}

/* A fault in the range that is not a sample: retry. After a few retries at
 * the same PC, yield so the thread changing the protection (the sampler, or
 * the sample's owner) can finish even if it has a lower priority on this core.
 * A fault that keeps coming back is genuine: make the code executable, stop
 * sampling and hand it to the crash handler. */
static int retry(volatile uint32_t *counter, uintptr_t pc) {
  static uintptr_t last_pc;
  static uint32_t run;
  (*counter)++;
  if (pc != last_pc) { last_pc = pc; run = 0; }
  if (++run > 4) sceKernelDelayThread(50);
  if (run == 1000 && s_state == ST_IDLE) restore();
  if (run > 20000) { s_disabled = 1; restore(); return 0; }
  return 1;
}

/* The first fault after arming is the sample. */
static int take(KuKernelExceptionContext *c, uintptr_t pc) {
  if (!__sync_bool_compare_and_swap(&s_state, ST_ARMED, ST_TAKEN)) return 0;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  record(c, pc);
  restore();
  s_sample_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
  __sync_synchronize();
  s_state = ST_IDLE;
  return 1;
}

__attribute__((used)) static void prefetch_handler(KuKernelExceptionContext *c) {
  uintptr_t pc = c->pc & ~(uintptr_t)1;
  if (s_testing && pc >= s_test_lo && pc < s_test_lo + 0x1000) {
    s_test_faults++;
    kuKernelMemProtect((void *)s_test_lo, 0x1000, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
    return;
  }
  if (pc < s_lo || pc >= s_hi) {               /* not ours */
    pass_on(s_old_prefetch, c);
    return;
  }
  if (take(c, pc)) return;
  if (!retry(&s_retry_fetch, pc)) pass_on(s_old_prefetch, c);
}

/* Data reads of the range (literal pools, jump tables) should be allowed
 * while it is armed, and fault only while the mapping is being rewritten. If
 * the armed state turns out to deny reads too, the read is the sample: its PC
 * is the engine instruction that made it. */
__attribute__((used)) static void data_handler(KuKernelExceptionContext *c) {
  if (c->FAR >= s_lo && c->FAR < s_hi && !(c->FSR & 0x800) /* a read */) {
    uintptr_t pc = c->pc & ~(uintptr_t)1;
    if (take(c, pc)) { s_data_samples++; return; }
    if (retry(&s_retry_data, pc)) return;
  }
  pass_on(s_old_data, c);
}

/* kubridge runs a handler on the faulting thread's stack as it was, which is
 * only 4-byte aligned after an odd-sized push; GCC's vector stores assume the
 * AAPCS 8-byte alignment (hardware run real-vita-fxab: vst1.64 [sp :64] in
 * record() took an alignment fault). These entry stubs align the stack. */
__attribute__((naked)) static void prefetch_entry(KuKernelExceptionContext *c) {
  (void)c;
  __asm__ volatile("push {r4, lr}\n\t"
                   "mov r4, sp\n\t"
                   "bic r1, r4, #7\n\t"
                   "mov sp, r1\n\t"
                   "bl prefetch_handler\n\t"
                   "mov sp, r4\n\t"
                   "pop {r4, pc}\n\t");
}
__attribute__((naked)) static void data_entry(KuKernelExceptionContext *c) {
  (void)c;
  __asm__ volatile("push {r4, lr}\n\t"
                   "mov r4, sp\n\t"
                   "bic r1, r4, #7\n\t"
                   "mov sp, r1\n\t"
                   "bl data_handler\n\t"
                   "mov sp, r4\n\t"
                   "pop {r4, pc}\n\t");
}

/* ---- self-test ------------------------------------------------------------------ */
static volatile int s_test_ret = -1;
static int test_thread(SceSize args, void *argp) {
  (void)args;
  /* sceKernelStartThread passes a pointer to a copy of the argument bytes. */
  int (*f)(void) = (int (*)(void))(*(const uintptr_t *)argp | 1u);
  s_test_ret = f();
  return 0;
}

static int self_test(void) {
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  SceUID blk = kuKernelAllocMemBlock("pcprof_test", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt);
  void *base = NULL;
  if (blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 || !base) return -1;
  static const uint16_t code[] = {0x202a /* movs r0, #42 */, 0x4770 /* bx lr */};
  kuKernelCpuUnrestrictedMemcpy(base, code, sizeof code);
  kuKernelFlushCaches(base, sizeof code);
  s_test_lo = (uintptr_t)base;
  s_testing = 1;
  int rc = kuKernelMemProtect(base, 0x1000, KU_KERNEL_PROT_READ);
  if (rc < 0) { s_testing = 0; return rc; }
  SceUID th = sceKernelCreateThread("pcprof_test", test_thread, 100, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
  if (th < 0 || sceKernelStartThread(th, sizeof base, &base) < 0) { s_testing = 0; return -2; }
  for (int i = 0; i < 100 && s_test_ret != 42; i++) sceKernelDelayThread(5000);
  s_testing = 0;
  return s_test_ret == 42 && s_test_faults >= 1 ? 0 : -3;
}

/* ---- symbols -------------------------------------------------------------------- */
static int fn_cmp(const void *a, const void *b) {
  const fn_t *x = a, *y = b;
  return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static int load_symbols(void) {
  s_fn = malloc(sizeof(fn_t) * kotor_mod.num_dynsym);
  if (!s_fn) return -1;
  for (int i = 0; i < kotor_mod.num_dynsym; i++) {
    const Elf32_Sym *sy = &kotor_mod.dynsym[i];
    if (ELF32_ST_TYPE(sy->st_info) != STT_FUNC || sy->st_shndx == SHN_UNDEF || !sy->st_value) continue;
    uint32_t lo = (uint32_t)sy->st_value & ~1u;
    s_fn[s_nfn].lo = lo;
    s_fn[s_nfn].hi = lo + (sy->st_size ? sy->st_size : 2);
    s_fn[s_nfn].name = sy->st_name;
    s_nfn++;
  }
  qsort(s_fn, s_nfn, sizeof(fn_t), fn_cmp);
  s_self = calloc(s_nfn, 4);
  s_after = calloc(s_nfn, 4);
  s_incl = calloc(s_nfn, 4);
  return s_self && s_after && s_incl ? 0 : -1;
}

/* ---- sampler and report ------------------------------------------------------------ */
static void raw_flush(uint32_t min_words) {
  if (s_raw_fd < 0) return;
  uint32_t h = s_raw_head, t = s_raw_tail;
  __sync_synchronize();
  if (h - t < min_words) return;
  while (t != h) {
    uint32_t at = t & (RAW_WORDS - 1), n = h - t;
    if (at + n > RAW_WORDS) n = RAW_WORDS - at;      /* up to the end of the ring */
    int w = sceIoWrite(s_raw_fd, s_raw + at, n * 4);
    if (w < 0) { sceIoClose(s_raw_fd); s_raw_fd = -1; break; }
    t += n;
    s_raw_written += n * 4;
  }
  s_raw_tail = t;
}

static void report(uint32_t window_us, uint32_t arms, uint32_t arm_us) {
  uint32_t n = s_game_samples;
  log_printf("[pcprof] %u.%u s: game %u samples, other threads %u, via data read %u, retried fetch %u data %u, no symbol %u, "
             "chain %u.%u frames/sample | arm %u us, sample %u us (avg), arm failures %u (0x%x)%s",
             window_us / 1000000u, window_us / 100000u % 10u, n, s_other_samples, s_data_samples, s_retry_fetch, s_retry_data, s_nosym,
             n ? s_chain_frames / n : 0, n ? s_chain_frames * 10u / n % 10u : 0, arms ? arm_us / arms : 0,
             s_game_samples + s_other_samples ? s_sample_us / (s_game_samples + s_other_samples) : 0,
             s_arm_fail, (unsigned)s_arm_rc, s_disabled ? " | DISABLED (a fault kept repeating)" : "");
  log_printf("[pcprof] raw file: %u KB written, %u samples dropped%s", s_raw_written / 1024u, s_raw_drops,
             s_raw_fd < 0 ? " (file closed or not open)" : "");
  s_arm_fail = 0;
  if (!n) return;
  for (int pass = 0; pass < 2; pass++) {
    /* pass 0: self (inside or past a symbol), pass 1: inclusive */
    char line[1024];
    /* log.c keeps a line by its format string, so the tag goes there. */
    int o = snprintf(line, sizeof line, "top %s (permille of game samples):", pass ? "inclusive" : "self");
    for (int t = 0; t < 24; t++) {
      uint32_t best = 0;
      int bi = -1, bafter = 0;
      for (int i = 0; i < s_nfn; i++) {
        if (pass == 0) {
          if (s_self[i] > best) { best = s_self[i]; bi = i; bafter = 0; }
          if (s_after[i] > best) { best = s_after[i]; bi = i; bafter = 1; }
        } else if (s_incl[i] > best) {
          best = s_incl[i]; bi = i;
        }
      }
      if (bi < 0 || best * 1000u / n < 3) break;
      const char *nm = kotor_mod.dynstr + s_fn[bi].name;
      int room = (int)sizeof line - o;
      if (room < 120) break;
      o += snprintf(line + o, room, " %s%.80s+%x=%u", bafter ? "~" : "", nm,
                    (unsigned)s_fn[bi].lo, best * 1000u / n);
      if (pass == 0) { if (bafter) s_after[bi] = 0; else s_self[bi] = 0; }
      else s_incl[bi] = 0;
      if (o > (int)sizeof line - 140) {
        log_printf("[pcprof] %s", line);
        o = snprintf(line, sizeof line, "  ...");
      }
    }
    log_printf("[pcprof] %s", line);
  }
  memset(s_self, 0, s_nfn * 4);
  memset(s_after, 0, s_nfn * 4);
  memset(s_incl, 0, s_nfn * 4);
  s_game_samples = s_other_samples = s_data_samples = s_retry_fetch = s_retry_data = s_nosym = s_chain_frames = 0;
  s_sample_us = 0;
}

static int sampler(SceSize args, void *argp) {
  (void)args; (void)argp;
  uint64_t t_win = sceKernelGetProcessTimeWide();
  uint32_t arms = 0, arm_us = 0, cost = 0;
  for (;;) {
    /* Keep the cost a sample puts on the sampled thread under ~10%. */
    sceKernelDelayThread(PC_PROF_PERIOD_US + 9u * cost);
    if (s_disabled) continue;
    if (s_state == ST_IDLE) {
      uint64_t t0 = sceKernelGetProcessTimeWide();
      s_state = ST_ARMING;
      __sync_synchronize();
      int rc = kuKernelMemProtect((void *)s_lo, s_hi - s_lo, KU_KERNEL_PROT_READ);
      __sync_synchronize();
      if (rc < 0) { s_arm_fail++; s_arm_rc = rc; restore(); s_state = ST_IDLE; }
      else s_state = ST_ARMED;
      arm_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
      arms++;
    }
    uint32_t taken = s_game_samples + s_other_samples;
    if (taken) cost = s_sample_us / taken;
    raw_flush(16384);
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - t_win >= 10000000u) {
      /* Report with the engine executable, so no sample lands mid-report. */
      while (s_state != ST_IDLE && !s_disabled) sceKernelDelayThread(100);
      raw_flush(0);
      report((uint32_t)(now - t_win), arms, arm_us);
      arms = arm_us = 0;
      t_win = sceKernelGetProcessTimeWide();
    }
  }
  return 0;
}

void pc_prof_start(void) {
  s_game = sceKernelGetThreadId();
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  if (sceKernelGetThreadInfo(s_game, &ti) >= 0) {
    s_stk_lo = (uintptr_t)ti.stack;
    s_stk_hi = s_stk_lo + (uintptr_t)ti.stackSize;
  }
  KuKernelExceptionHandlerOpt opt;
  opt.size = sizeof opt;
  int rc = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, prefetch_entry,
                                            &s_old_prefetch, &opt);
  if (rc < 0) { log_printf("[pcprof] OFF: exception handler not registered (0x%x)", rc); return; }
  rc = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, data_entry, &s_old_data, &opt);
  if (rc < 0) { log_printf("[pcprof] OFF: data abort handler not registered (0x%x)", rc); return; }
  rc = self_test();
  if (rc < 0) {
    log_printf("[pcprof] OFF: self-test failed (%d, faults %u, ret %d)", rc, (unsigned)s_test_faults, s_test_ret);
    return;
  }
  if (load_symbols() < 0) { log_printf("[pcprof] OFF: no memory for symbols"); return; }
  /* Protect only the pages that hold functions (.text). The rest of the code
   * segment is read at run time: .dynsym/.dynstr (dlsym), the unwind tables,
   * .rodata (strings the loader reads). Partial pages at either end stay
   * executable. */
  s_eng_lo = kotor_mod.text_base;
  s_eng_hi = kotor_mod.text_base + kotor_mod.text_size;
  s_lo = (kotor_mod.text_base + s_fn[0].lo + 0xfff) & ~(uintptr_t)0xfff;
  uint32_t top = 0;
  for (int i = 0; i < s_nfn; i++) if (s_fn[i].hi > top) top = s_fn[i].hi;
  s_hi = (kotor_mod.text_base + top) & ~(uintptr_t)0xfff;
  s_raw = malloc(RAW_WORDS * 4);
  s_raw_fd = s_raw ? sceIoOpen("ux0:data/kotor2/pcprof.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666) : -1;
  if (s_raw_fd >= 0) {
    uint32_t hdr[5] = {0x32504350 /* "PCP2" */, (uint32_t)kotor_mod.text_base, (uint32_t)kotor_mod.text_size,
                       (uint32_t)s_lo, (uint32_t)s_hi};
    sceIoWrite(s_raw_fd, hdr, sizeof hdr);
  }
  SceUID th = sceKernelCreateThread("pcprof", sampler, 100, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
  rc = th >= 0 ? sceKernelStartThread(th, 0, NULL) : th;
  log_printf("[pcprof] ON (rc 0x%x): self-test ok, %d functions, code 0x%08x-0x%08x, game stack 0x%08x-0x%08x, "
             "every %u us, raw file fd 0x%x", rc, s_nfn, (unsigned)s_lo, (unsigned)s_hi, (unsigned)s_stk_lo,
             (unsigned)s_stk_hi, (unsigned)PC_PROF_PERIOD_US, (unsigned)s_raw_fd);
}

#endif
