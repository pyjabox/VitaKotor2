/* script_cache.c -- compiled scripts kept in memory (SCRIPT_CACHE).
 *
 * Hardware, Khoonda battle (TSLRCM), 2026-10-17, AI worker profile:
 * CServerAIMaster::UpdateState 95%, CVirtualMachineInternal::RunScript 73-79%,
 * ReadScriptFile 35-43%. Each script run loads its code again:
 *
 *   CVirtualMachineInternal::ReadScriptFile(CExoString *name), libkotor2 2.0.4:
 *     level = this+0x28; at 7 or more: fail (-94), nothing changed
 *     this+0x28 = ++level
 *     CResHelper<CResNCS,2010>::SetResRef(name); CRes::Demand   (key tables,
 *       CExoResMan::Demand -> ServiceFromResFile -> the OBB, every time)
 *     CExoString at this+0x44+level*40 = *name
 *     header "NCS V1.0B" checked
 *     InitializeScript(this, this+0x2c+level*40, data+13, size-13)
 *       -- new[size] + memcpy: the VM keeps its own copy
 *     CRes::Release; SetResRef("")
 *
 * So the bytes a name gives are all the VM takes from the resource. This keeps
 * them after the first successful load and, on the next run of the same name,
 * does the same three things to the VM -- level, name, the script's fields --
 * without the resource manager.
 *
 * No copy (hardware, 2026-10-17: a hit that still went through InitializeScript
 * cost 87-106 us, the new[] and memcpy of the whole script, 100-150 runs a frame
 * in the battle). The script's code pointer (+0x10) is our buffer, read-only to
 * the VM: ExecuteCode reads it, StackPopCommand (DelayCommand and the like) and
 * SaveScriptSituation copy from it. Only DeleteScript (delete[]) and
 * ~CVirtualMachineScript free +0x10; both are reached through PLT slots, which
 * clear our pointer first. Our buffers carry a magic word in front. RunScript
 * always calls DeleteScript after the run, so no slot keeps one past its
 * script; when the cache is emptied, the buffers wait for the next frame swap
 * (no script runs then) before they are freed.
 *
 * What a name gives changes only when the resource set does: modules, override
 * directories and archives all go through CExoResMan::AddKeyTable,
 * RemoveKeyTable or UpdateKeyTable (every Add/Remove/Update*File and
 * *Directory tail-calls one of the three through the PLT). Any call to them
 * empties the cache.
 *
 * ux0:data/kotor2/script_mode.txt: 0 off (nothing hooked), 1 on, a on and off
 * alternately every 10 s. Without the file: SCRIPT_CACHE_DEFAULT. A [script]
 * line every 10 s. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "script_cache.h"

#if SCRIPT_CACHE

static inline uint32_t tls_key(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}
static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

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

/* ---- engine pieces ------------------------------------------------------------------------ */
typedef int (*rsf_t)(void *vm, void *name);
typedef void (*init_t)(void *vm, void *script, const void *code, int size);
typedef void (*resref_t)(void *rr, const void *name);
typedef void *(*str_assign_t)(void *dst, const void *src);
typedef uint32_t (*kt_t)(void *rm, uint32_t a, uint32_t b, uint32_t c);
typedef uint32_t (*del_t)(void *vm, void *script);
typedef void *(*dtor_t)(void *script);

static rsf_t o_rsf;
static init_t o_init;
static resref_t f_resref;
static str_assign_t f_str_assign;
static kt_t o_kt_add, o_kt_remove, o_kt_update;
static del_t o_del;
static dtor_t o_dtor;

#define VM_LEVEL       0x28
#define VM_NAME(l)     (0x44 + (l) * 40)
#define VM_SCRIPT(l)   (0x2c + (l) * 40)
#define VM_MAX_LEVEL   7

/* ---- the cache ---------------------------------------------------------------------------- */
#define SLOTS 2048                       /* power of two; open addressing */
#define MAX_ONE (256 * 1024)
typedef struct { uint8_t key[16]; uint8_t *code; int size; } ent_t;
static ent_t s_ent[SLOTS];
static uint32_t s_count, s_bytes;
static volatile int s_lock;

static void lock(void) { while (__sync_lock_test_and_set(&s_lock, 1)) sceKernelDelayThread(10); }
static void unlock(void) { __sync_lock_release(&s_lock); }

/* Our code buffers: 8 bytes of magic, then the code. */
#define MAGIC0 0x53435243u   /* "CRCS" */
#define MAGIC1 0x9e3779b9u
static uint8_t *buf_new(int size) {
  uint32_t *h = malloc((size_t)size + 8);
  if (!h) return NULL;
  h[0] = MAGIC0; h[1] = MAGIC1;
  return (uint8_t *)(h + 2);
}
static int buf_ours(const void *code) {
  if (!code || ((uintptr_t)code & 3)) return 0;
  const uint32_t *h = (const uint32_t *)code - 2;  /* a malloc chunk header if not ours: mapped */
  return h[0] == MAGIC0 && h[1] == MAGIC1;
}
static void buf_free(uint8_t *code) {
  uint32_t *h = (uint32_t *)code - 2;
  h[0] = h[1] = 0;
  free(h);
}

/* Emptied buffers, freed at the next swap. */
#define RETIRED_MAX 4096
static uint8_t *s_retired[RETIRED_MAX];
static uint32_t s_nretired;

static uint32_t hash16(const uint8_t *k) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 16; i++) h = (h ^ k[i]) * 16777619u;
  return h;
}

static ent_t *find(const uint8_t *key) {           /* caller holds the lock */
  for (uint32_t i = hash16(key) & (SLOTS - 1), n = 0; n < SLOTS; i = (i + 1) & (SLOTS - 1), n++) {
    ent_t *e = &s_ent[i];
    if (!e->code) return NULL;
    if (!memcmp(e->key, key, 16)) return e;
  }
  return NULL;
}

static void clear_all(void) {                       /* caller holds the lock */
  for (int i = 0; i < SLOTS; i++)
    if (s_ent[i].code) {
      if (s_nretired < RETIRED_MAX) s_retired[s_nretired++] = s_ent[i].code;   /* else leaked: never freed early */
      s_ent[i].code = NULL;
    }
  s_count = s_bytes = 0;
}

static int insert(const uint8_t *key, uint8_t *code, int size) {   /* takes ownership on success */
  if (s_count >= SLOTS / 2 || s_bytes + (uint32_t)size > SCRIPT_CACHE_KB * 1024u) return 0;
  for (uint32_t i = hash16(key) & (SLOTS - 1);; i = (i + 1) & (SLOTS - 1)) {
    ent_t *e = &s_ent[i];
    if (e->code) { if (!memcmp(e->key, key, 16)) return 0; continue; }
    memcpy(e->key, key, 16);
    e->size = size;
    __sync_synchronize();
    e->code = code;
    s_count++;
    s_bytes += (uint32_t)size;
    return 1;
  }
}

/* ---- state ---------------------------------------------------------------------------------- */
static int s_installed, s_ab, s_on;
static uint32_t w_runs, w_hits, w_misses, w_kept, w_full, w_clears, w_lines;
static uint64_t w_miss_us, w_hit_us;
static uint32_t s_avg_miss_us = 300;      /* running average of a miss, for "saved" */
static uint32_t w_ver_same, w_ver_diff;    /* SCRIPT_CACHE_VERIFY */

/* A miss: the original runs; its InitializeScript call hands us the bytes. */
static volatile uint32_t s_cap_key;
static uint8_t *s_cap_buf;
static int s_cap_size;

static void h_init(void *vm, void *script, const void *code, int size) {
  if (s_cap_key && tls_key() == s_cap_key && !s_cap_buf && code && size > 0 && size <= MAX_ONE) {
    uint8_t *b = buf_new(size);
    if (b) { memcpy(b, code, (size_t)size); s_cap_buf = b; s_cap_size = size; }
  }
  o_init(vm, script, code, size);
}

static int h_rsf(void *vm, void *name) {
  uint8_t *v = vm;
  int level = *(int *)(v + VM_LEVEL);
  if (!s_on || !name || level >= VM_MAX_LEVEL) return o_rsf(vm, name);
  uint8_t rr[64];
  memset(rr, 0, sizeof rr);
  f_resref(rr, name);
  w_runs++;
  uint64_t t0 = now_us();
  lock();
  ent_t *e = find(rr);
#if SCRIPT_CACHE_VERIFY
  /* Test builds: the engine loads it as usual; its bytes are compared with ours. */
  if (e) {
    uint8_t *mine = malloc((size_t)e->size);
    int mine_size = e->size;
    if (mine) memcpy(mine, e->code, (size_t)mine_size);
    unlock();
    s_cap_buf = NULL;
    s_cap_key = tls_key();
    int r = o_rsf(vm, name);
    s_cap_key = 0;
    uint8_t *b = s_cap_buf;
    s_cap_buf = NULL;
    if (mine && b) {
      int same = r == 0 && s_cap_size == mine_size && !memcmp(b, mine, (size_t)mine_size);
      if (same) w_ver_same++;
      else if (w_ver_diff++ < 20)
        log_printf("[script] VERIFY MISMATCH %.16s: engine %d bytes (r %d), cache %d bytes", (const char *)rr,
                   s_cap_size, r, mine_size);
    }
    free(mine);
    if (b) buf_free(b);
    w_hits++;
    return r;
  }
#endif
  if (e) {
    /* What the original does to the VM on success, in its order. Under the
     * lock: AddKeyTable may run on the engine's async loader thread and empty
     * the cache (InitializeScript copies the bytes before we let go). */
    *(int *)(v + VM_LEVEL) = level + 1;
    f_str_assign(v + VM_NAME(level + 1), name);
    /* InitializeScript's stores, with our buffer for the code. */
    uint32_t *sc = (uint32_t *)(v + VM_SCRIPT(level + 1));
    sc[0x14 / 4] = (uint32_t)e->size;
    sc[0] = 0; sc[1] = 0;
    sc[2] = 13; sc[3] = 0;
    sc[0x20 / 4] = 0;
    sc[0x10 / 4] = (uint32_t)(uintptr_t)e->code;
    sc[0x24 / 4] = 0;
    unlock();
    w_hits++;
    w_hit_us += now_us() - t0;
    return 0;
  }
  unlock();
  s_cap_buf = NULL;
  s_cap_key = tls_key();
  int r = o_rsf(vm, name);
  s_cap_key = 0;
  uint32_t us = (uint32_t)(now_us() - t0);
  w_misses++;
  w_miss_us += us;
  s_avg_miss_us = (s_avg_miss_us * 7 + us) / 8;
  uint8_t *b = s_cap_buf;
  s_cap_buf = NULL;
  if (b) {
    int ok = 0;
    if (r == 0) { lock(); ok = insert(rr, b, s_cap_size); unlock(); }
    if (ok) w_kept++;
    else { if (r == 0) w_full++; buf_free(b); }
  }
  return r;
}

static uint32_t w_unshared;
static uint32_t h_del(void *vm, void *script) {
  uint32_t *sc = script;
  if (sc && buf_ours((const void *)(uintptr_t)sc[0x10 / 4])) { sc[0x10 / 4] = 0; w_unshared++; }
  return o_del(vm, script);
}
static void *h_dtor(void *script) {
  uint32_t *sc = script;
  if (sc && buf_ours((const void *)(uintptr_t)sc[0x10 / 4])) { sc[0x10 / 4] = 0; w_unshared++; }
  return o_dtor(script);
}

static void invalidate(const char *why) {
  lock();
  uint32_t n = s_count, kb = s_bytes / 1024;
  clear_all();
  unlock();
  w_clears++;
  if (n && w_lines++ < 50) log_printf("[script] cache emptied (%s): %u scripts, %u KB", why, (unsigned)n, (unsigned)kb);
}
static uint32_t h_kt_add(void *rm, uint32_t a, uint32_t b, uint32_t c) {
  uint32_t r = o_kt_add(rm, a, b, c);
  invalidate("key table added");
  return r;
}
static uint32_t h_kt_remove(void *rm, uint32_t a, uint32_t b, uint32_t c) {
  invalidate("key table removed");
  return o_kt_remove(rm, a, b, c);
}
static uint32_t h_kt_update(void *rm, uint32_t a, uint32_t b, uint32_t c) {
  uint32_t r = o_kt_update(rm, a, b, c);
  invalidate("key table updated");
  return r;
}

void script_cache_on_swap(void) {
  if (!s_installed) return;
  if (s_nretired) {                      /* no script runs at a swap */
    lock();
    for (uint32_t i = 0; i < s_nretired; i++) buf_free(s_retired[i]);
    s_nretired = 0;
    unlock();
  }
  static uint64_t t0;
  static uint32_t frames;
  uint64_t now = now_us();
  frames++;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  uint32_t hit_avg = w_hits ? (uint32_t)(w_hit_us / w_hits) : 0;
  uint32_t miss_avg = w_misses ? (uint32_t)(w_miss_us / w_misses) : 0;
  uint64_t saved = (uint64_t)w_hits * (s_avg_miss_us > hit_avg ? s_avg_miss_us - hit_avg : 0);
#if SCRIPT_CACHE_VERIFY
  log_printf("[script] verify: %u same, %u different", (unsigned)w_ver_same, (unsigned)w_ver_diff);
#endif
  log_printf("[script] %s%u frames | %u reads: %u from memory (%u us each), %u loaded (%u us each, %.1f ms a frame) | "
             "~%.1f ms a frame saved | %u handed back | %u kept, %u not kept (full), %u emptied | %u scripts, %u KB | heap %u MB used",
             s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)frames, (unsigned)(w_hits + w_misses), (unsigned)w_hits,
             (unsigned)hit_avg, (unsigned)w_misses, (unsigned)miss_avg,
             frames ? w_miss_us / 1000.0 / frames : 0.0, frames ? saved / 1000.0 / frames : 0.0, (unsigned)w_unshared, (unsigned)w_kept,
             (unsigned)w_full, (unsigned)w_clears, (unsigned)s_count, (unsigned)(s_bytes / 1024),
             (unsigned)(mallinfo().uordblks / (1024 * 1024)));
  w_runs = w_hits = w_misses = w_kept = w_full = w_clears = w_unshared = 0;
  w_miss_us = w_hit_us = 0;
  frames = 0;
  t0 = now;
  if (s_ab) s_on = !s_on;
}

int script_cache_phase(void) { return s_installed ? s_on : -1; }

void script_cache_install(void) {
  char m = SCRIPT_CACHE_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/script_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    char b = 0;
    if (sceIoRead(fd, &b, 1) == 1) m = b;
    sceIoClose(fd);
  }
  if (m != '1' && m != 'a') {
    log_printf("[script] off (script_mode '%c')", m);
    return;
  }
  f_resref = (resref_t)so_symbol(&kotor_mod, "_ZN7CResRefC1ERK10CExoString");
  if (!f_resref) f_resref = (resref_t)so_symbol(&kotor_mod, "_ZN7CResRefC2ERK10CExoString");
  f_str_assign = (str_assign_t)so_symbol(&kotor_mod, "_ZN10CExoStringaSERKS_");
  if (!f_resref || !f_str_assign) {
    log_printf("[script] NOT armed: CResRef(CExoString) %d, CExoString::operator= %d", !!f_resref, !!f_str_assign);
    return;
  }
  unsigned n_add = 0, n_rem = 0, n_upd = 0, n_init = 0, n_rsf = 0, n_del = 0, n_dtor = 0;
  /* Invalidation first: a cache that cannot be emptied must not exist. */
  o_kt_add = (kt_t)take_slots("_ZN10CExoResMan11AddKeyTableERK10CExoStringmm", (uintptr_t)h_kt_add, &n_add);
  o_kt_remove = (kt_t)take_slots("_ZN10CExoResMan14RemoveKeyTableERK10CExoStringm", (uintptr_t)h_kt_remove, &n_rem);
  o_kt_update = (kt_t)take_slots("_ZN10CExoResMan14UpdateKeyTableERK10CExoStringm", (uintptr_t)h_kt_update, &n_upd);
  if (o_kt_add && o_kt_remove && o_kt_update) {
    o_init = (init_t)take_slots("_ZN23CVirtualMachineInternal16InitializeScriptEP21CVirtualMachineScriptPci",
                                (uintptr_t)h_init, &n_init);
    o_del = (del_t)take_slots("_ZN23CVirtualMachineInternal12DeleteScriptEP21CVirtualMachineScript", (uintptr_t)h_del,
                              &n_del);
    o_dtor = (dtor_t)take_slots("_ZN21CVirtualMachineScriptD2Ev", (uintptr_t)h_dtor, &n_dtor);
    if (o_init && o_del && o_dtor)
      o_rsf = (rsf_t)take_slots("_ZN23CVirtualMachineInternal14ReadScriptFileEP10CExoString", (uintptr_t)h_rsf, &n_rsf);
  }
  if (!o_rsf) {
    /* Whatever was taken only passes through (s_on stays 0). */
    log_printf("[script] NOT armed: key tables %u %u %u, InitializeScript %u, DeleteScript %u, ~Script %u, "
               "ReadScriptFile %u", n_add, n_rem, n_upd, n_init, n_del, n_dtor, n_rsf);
    return;
  }
  s_ab = m == 'a';
  s_on = 1;
  s_installed = 1;
  log_printf("[script] armed: ReadScriptFile %u slots, InitializeScript %u, DeleteScript %u, ~Script %u, key tables "
             "%u/%u/%u; shared code, up to %u KB%s", n_rsf, n_init, n_del, n_dtor, n_add, n_rem, n_upd, (unsigned)SCRIPT_CACHE_KB, s_ab ? "; A/B: on and off alternately every 10 s" : "");
}

#else
void script_cache_install(void) {}
void script_cache_on_swap(void) {}
int script_cache_phase(void) { return -1; }
#endif
