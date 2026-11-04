/* plt_direct.c -- the engine's calls to its own functions, made direct
 * (PLT_DIRECT).
 *
 * libkotor2 was built as a shared library with every function exported, so a
 * call from one engine function to another goes through the PLT: blx to a
 * three-instruction ARM stub that loads the GOT slot and jumps. A tail call
 * goes b.w -> a Thumb-to-ARM veneer -> the same stub. Most of the engine's
 * calls are like this (107,483 blx and 5,129 b.w to functions it defines,
 * against 24,302 direct bl), down to one-instruction getters, and each costs
 * a mode switch, an extra indirect jump, a GOT load and the I-cache the 224 KB
 * of stubs take.
 *
 * plt_direct_scan, right after libkotor2 is loaded and before any hook
 * touches its code, finds those calls and keeps the list. plt_direct_install,
 * once, before SDL_main, after the last hook (locstr_fallback, on the game
 * thread): every call on the list whose instruction is still the original
 * (an entry hook can overwrite one) and whose GOT slot still holds the
 * function's own address (no loader hook on it) is rewritten as bl/b.w (blx
 * for libgcc's ARM functions) straight to the function. Calls to hooked
 * functions keep going through their slot, so every hook still sees them.
 * Return addresses do not change: the new branch has the old one's size and
 * place, so hooks that check their caller still work.
 *
 * Which calls: plt_scan sweeps .text for them; tools/plt_oracle.py disassembled
 * the same library following control flow from every function entry and
 * listed the sites that sweep must not touch (C++ landing pads it cannot
 * prove are code, a few overlapping literals) in plt_exclude.h, with the
 * count and hash of the sweep. If the sweep here gives another count or hash
 * -- another libkotor2 -- nothing is patched.
 *
 * ux0:data/kotor2/plt_mode.txt: 0 off, 1 on, a on and off on alternate
 * launches (plt_ab.txt holds the last launch's state). Without the file:
 * PLT_DIRECT_DEFAULT. Every 10 s a check that no slot of a direct-called
 * function has changed since (a hook installed later would miss those calls). */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "plt_direct.h"

#if PLT_DIRECT

#include "plt_scan.h"
#include "plt_exclude.h"

/* This libkotor2 (sha256 in plt_exclude.h): .plt, and .text up to its first
 * ARM function (libgcc's __aeabi_idiv). Checked below, and by the hash. */
#define PLT_ADDR   0x258d80u
#define SWEEP_LO   0x28f910u
#define SWEEP_HI   0x692738u

static int s_on, s_scanned;
static uint32_t s_nplt;
static uint32_t *s_def;          /* per PLT entry: the function's address, 0 = not ours to call */
static uint8_t *s_direct;        /* per PLT entry: some call to it was made direct */
static uint32_t s_hash, s_count;
static uint32_t *s_site;         /* the list: site | kind << 31 */
static uint16_t *s_idx;          /*           its PLT entry */
static uint32_t s_nsite;
static uint32_t s_calls, s_tails, s_hooked, s_excluded, s_changed, s_failed;
static uint64_t s_scan_us;

static int excluded(uint32_t site) {
  int lo = 0, hi = (int)(sizeof plt_exclude / sizeof plt_exclude[0]) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (plt_exclude[mid] == site) return 1;
    if (plt_exclude[mid] < site) lo = mid + 1; else hi = mid - 1;
  }
  return 0;
}

/* Sites the sweep finds that plt_direct may rewrite: a defined function, and
 * for a tail call a Thumb one (b.w cannot switch to ARM). Same filter as
 * tools/test_plt_scan.c, so the hash is the one it printed. */
static int patchable(uint32_t idx, int kind, uint32_t *def) {
  *def = s_def[idx];
  if (!*def) return 0;
  return kind == PLT_SITE_CALL || (*def & 1);
}

/* One pass: the count and hash of every patchable site, and the list of
 * those not excluded. */
static void keep_site(void *u, uint32_t site, uint32_t idx, int kind) {
  (void)u;
  uint32_t def;
  if (!patchable(idx, kind, &def)) return;
  s_count++;
  s_hash = plt_hash_step(s_hash, site, idx, kind);
  if (excluded(site)) { s_excluded++; return; }
  if (s_nsite < PLT_EXPECT_SITES) {
    s_site[s_nsite] = site | (uint32_t)kind << 31;
    s_idx[s_nsite] = (uint16_t)idx;
    s_nsite++;
  }
}

/* Rewrites go through a page-sized window: one kernel copy per page touched. */
static uint8_t s_win[4096];
static uint32_t s_win_page = 0xffffffffu;

static void win_flush(void) {
  if (s_win_page == 0xffffffffu) return;
  kuKernelCpuUnrestrictedMemcpy((void *)(kotor_mod.text_base + s_win_page), s_win, sizeof s_win);
  s_win_page = 0xffffffffu;
}

static void put_insn(uint32_t site, const uint16_t ins[2]) {
  uint32_t page = site & ~4095u;
  if ((site & 4095u) > 4092u) {                    /* straddles two pages: alone */
    win_flush();
    kuKernelCpuUnrestrictedMemcpy((void *)(kotor_mod.text_base + site), ins, 4);
    return;
  }
  if (page != s_win_page) {
    win_flush();
    memcpy(s_win, (const void *)(kotor_mod.text_base + page), sizeof s_win);
    s_win_page = page;
  }
  memcpy(s_win + (site - page), ins, 4);
}

static void patch_site(const plt_scan_ctx *c, uint32_t site, uint32_t idx, int kind) {
  uint32_t def = s_def[idx];
  uint32_t got = kotor_mod.text_base + kotor_mod.relplt[idx].r_offset;
  if (*(const uint32_t *)got != kotor_mod.text_base + def) { s_hooked++; return; }
  uint32_t idx2;
  int kind2;
  if (!plt_site_at(c, site, &idx2, &kind2) || idx2 != idx || kind2 != kind) { s_changed++; return; }
  uint16_t ins[2];
  int ok = kind == PLT_SITE_TAIL ? thumb_enc_bw(site, def, ins)
           : (def & 1) ? thumb_enc_bl(site, def, ins) : thumb_enc_blx(site, def, ins);
  if (!ok) { s_failed++; return; }
  put_insn(site, ins);
  s_direct[idx] = 1;
  if (kind == PLT_SITE_TAIL) s_tails++; else s_calls++;
}

/* Launch-to-launch A/B: plt_ab.txt holds what the last launch did. */
static int ab_this_launch(void) {
  char last = '0';
  SceUID fd = sceIoOpen(DATA_PATH "/plt_ab.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { sceIoRead(fd, &last, 1); sceIoClose(fd); }
  char now = last == '1' ? '0' : '1';
  fd = sceIoOpen(DATA_PATH "/plt_ab.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) { sceIoWrite(fd, &now, 1); sceIoClose(fd); }
  return now == '1';
}

#if PLT_DIRECT_BENCH
/* What one call through a PLT stub costs here, warm: dot(Vector, Vector)
 * through its stub and straight, 200,000 times each. */
typedef uint32_t (*dot_t)(const float *, const float *);
static void bench(void) {
  uint32_t fn = so_symbol(&kotor_mod, "_Z3dotRK6VectorS1_");
  uint32_t stub = 0;
  for (uint32_t i = 0; i < s_nplt && fn; i++)
    if (s_def[i] && kotor_mod.text_base + s_def[i] == fn) { stub = kotor_mod.text_base + PLT_ADDR + 20 + 12 * i; break; }
  if (!fn || !stub) { log_printf("[plt] bench: dot() or its stub not found"); return; }
  static volatile float va[3] = { 1, 2, 3 }, vb[3] = { 4, 5, 6 };
  volatile uint32_t sink = 0;
  uint64_t t[2] = { 0, 0 };
  for (int rep = 0; rep < 2; rep++)
    for (int k = 0; k < 2; k++) {
      dot_t f = (dot_t)(k ? fn : stub);
      uint64_t t0 = sceKernelGetProcessTimeWide();
      for (int i = 0; i < 200000; i++) sink += f((const float *)va, (const float *)vb);
      if (rep) t[k] = sceKernelGetProcessTimeWide() - t0;
    }
  (void)sink;
  log_printf("[plt] bench: dot() through its stub %u ns a call, straight %u ns (warm caches)",
             (unsigned)(t[0] * 1000 / 200000), (unsigned)(t[1] * 1000 / 200000));
}
#endif

static void drop_list(void) {
  free(s_site); free(s_idx);
  s_site = NULL; s_idx = NULL; s_nsite = 0;
}

void plt_direct_scan(void) {
  char m = PLT_DIRECT_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/plt_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char b = 0; if (sceIoRead(fd, &b, 1) == 1) m = b; sceIoClose(fd); }
  int on = m == '1' || (m == 'a' && ab_this_launch());
  if (m == 'a') log_printf("[plt] plt_mode a: this launch %s (alternates every launch)", on ? "ON" : "OFF");

  const uint8_t *img = (const uint8_t *)kotor_mod.text_base;
  s_nplt = (uint32_t)kotor_mod.num_relplt;
  static const uint32_t plt0[4] = { 0xe52de004u, 0xe59fe004u, 0xe08fe00eu, 0xe5bef008u };
  if (memcmp(img + PLT_ADDR, plt0, sizeof plt0) ||
      plt_entry_got(img, PLT_ADDR + 20) != kotor_mod.relplt[0].r_offset ||
      plt_entry_got(img, PLT_ADDR + 20 + 12 * (s_nplt - 1)) != kotor_mod.relplt[s_nplt - 1].r_offset) {
    log_printf("[plt] NOT armed: the PLT is not where this build expects it");
    return;
  }
  s_def = calloc(s_nplt, sizeof *s_def);
  s_direct = calloc(s_nplt, 1);
  if (!s_def || !s_direct) { log_printf("[plt] NOT armed: out of memory"); return; }
  for (uint32_t i = 0; i < s_nplt; i++) {
    const Elf32_Sym *s = &kotor_mod.dynsym[ELF32_R_SYM(kotor_mod.relplt[i].r_info)];
    if (s->st_shndx != SHN_UNDEF && ELF32_ST_TYPE(s->st_info) == STT_FUNC && s->st_value) s_def[i] = s->st_value;
  }
  if (!on) {
    log_printf("[plt] off: calls through the PLT left as they are");
    return;
  }
  s_site = malloc(PLT_EXPECT_SITES * sizeof *s_site);
  s_idx = malloc(PLT_EXPECT_SITES * sizeof *s_idx);
  if (!s_site || !s_idx) { drop_list(); log_printf("[plt] NOT armed: out of memory for the list"); return; }
  plt_scan_ctx c = { img, PLT_ADDR + 20, s_nplt, SWEEP_LO, SWEEP_HI };
  uint64_t t0 = sceKernelGetProcessTimeWide();
  s_hash = PLT_HASH_INIT;
  plt_scan(&c, keep_site, NULL);
  s_scan_us = sceKernelGetProcessTimeWide() - t0;
  if (s_count != PLT_EXPECT_SITES || s_hash != PLT_EXPECT_HASH) {
    drop_list();
    log_printf("[plt] NOT armed: %u sites, hash 0x%08x; this build was checked with %u, 0x%08x (another libkotor2?)",
               (unsigned)s_count, (unsigned)s_hash, (unsigned)PLT_EXPECT_SITES, (unsigned)PLT_EXPECT_HASH);
    return;
  }
  s_scanned = 1;
}

void plt_direct_install(void) {
  if (!s_scanned) return;
  s_scanned = 0;
#if PLT_DIRECT_BENCH
  bench();
#endif
  plt_scan_ctx c = { (const uint8_t *)kotor_mod.text_base, PLT_ADDR + 20, s_nplt, SWEEP_LO, SWEEP_HI };
  uint64_t t0 = sceKernelGetProcessTimeWide();
  for (uint32_t i = 0; i < s_nsite; i++)
    patch_site(&c, s_site[i] & 0x7fffffffu, s_idx[i], (int)(s_site[i] >> 31));
  win_flush();
  kuKernelFlushCaches((void *)(kotor_mod.text_base + SWEEP_LO), SWEEP_HI - SWEEP_LO);
  uint64_t t1 = sceKernelGetProcessTimeWide();
  drop_list();
  s_on = 1;
  log_printf("[plt] direct: %u calls and %u tail calls now branch straight to the function; left on the PLT: %u to "
             "hooked functions, %u rewritten by a hook, %u excluded, %u not encodable | scan %u ms, patch %u ms",
             (unsigned)s_calls, (unsigned)s_tails, (unsigned)s_hooked, (unsigned)s_changed, (unsigned)s_excluded,
             (unsigned)s_failed, (unsigned)(s_scan_us / 1000), (unsigned)((t1 - t0) / 1000));
}

void plt_direct_on_swap(void) {
  if (!s_on) return;
  static uint64_t t0;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  t0 = now;
  /* A hook installed after the calls were made direct would miss them. */
  static unsigned s_warned;
  unsigned changed = 0;
  for (uint32_t i = 0; i < s_nplt; i++) {
    if (!s_direct[i]) continue;
    uint32_t got = kotor_mod.text_base + kotor_mod.relplt[i].r_offset;
    if (*(const uint32_t *)got == kotor_mod.text_base + s_def[i]) continue;
    changed++;
    if (s_warned < 8) {
      s_warned++;
      log_printf("[plt] WARNING: %s was hooked after its calls were made direct; those calls skip the hook",
                 kotor_mod.dynstr + kotor_mod.dynsym[ELF32_R_SYM(kotor_mod.relplt[i].r_info)].st_name);
    }
  }
  if (changed && s_warned >= 8) log_printf("[plt] WARNING: %u hooked after the calls were made direct", changed);
}

int plt_direct_active(void) { return s_on; }

#else
void plt_direct_scan(void) {}
void plt_direct_install(void) {}
void plt_direct_on_swap(void) {}
int plt_direct_active(void) { return 0; }
#endif
