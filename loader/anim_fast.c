/* anim_fast.c -- three lookups the animation and the client repeat (ANIM_FAST),
 * each giving the engine's answer (read in libkotor2's disassembly, 2026-11-05).
 *
 * NewController::GetKeyIndexAndAlpha(t, node, &idx, &alpha) (0x4d5bec; through
 * the PLT from Get{Quaternion,Vector,Float}Value, for every animated node of
 * every Gob every frame): with times = node+0x44 data + ctrl+8 (u16) and
 * n = ctrl+6 (u16) keys, idx = the first i with !(times[i] < t) (or n), and
 * alpha = (t - times[i-1]) / (times[i] - times[i-1]) unless idx is 0 or n
 * (then 1). A linear scan from key 0. When the times are non-decreasing and
 * no NaN (checked once per key array and remembered), a binary search finds
 * the same first index; the alpha is the same expression.
 *
 * FindAnimation(model, name) (0x4d4cc4; Gob::PlayAnimation, GetAnimation,
 * Reset, PlayOutOfOrderAnimation): _stricmp over the model's animations
 * (+0x58, count +0x5c; name at +8), then its supermodel's (+0x64), and at the
 * root of that chain the "default" animation. Half of all _stricmp calls in a
 * battle. A found name is remembered with where it was found; a later lookup
 * walks the same chain from the model and checks that slot still holds that
 * animation with that name and the arrays are the same ones (pointer and
 * count), so a model freed and another loaded at its address is not
 * believed. Not-found and "default" answers are not remembered.
 *
 * C2DA::GetColumnIndex(name) (0x564790; projectiles, effects, AI): a linear
 * search of the column names (+0x48 text with u16 offsets at +0x50, or
 * CExoStrings at +0x34, by the flag at +0x3c; count +0x28). A found index is
 * remembered with the table's column storage and count, and checked with one
 * compare done the engine's way. -1 is not remembered.
 *
 * The two memos are shared by the game thread and the AI worker (a lock);
 * the key search's table only on the game thread (the worker calls the
 * engine's). ANIM_FAST_VERIFY (test builds): every answer against the
 * engine's own. anim_mode.txt 0: all three off. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_thread.h"
#include "anim_fast.h"

#if ANIM_FAST

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

static int s_on = 1;
static int (*f_stricmp)(const char *a, const char *b);       /* the engine's own */
static int (*f_cmpnc)(const void *a, const void *b);         /* CExoString::CompareNoCase */
static const char *(*f_cstr)(const void *s);                 /* CExoString::CStr */
static uint32_t w_key, w_key_bin, w_anim, w_anim_hit, w_col, w_col_hit;
#if ANIM_FAST_VERIFY
static uint32_t w_ver_n, w_ver_bad, s_ver_logged;
static void ver_note(int same, const char *what) {
  w_ver_n++;
  if (same) return;
  w_ver_bad++;
  if (s_ver_logged++ < 12) log_printf("[animfast] VERIFY mismatch in %s", what);
}
#endif

/* ---- GetKeyIndexAndAlpha ------------------------------------------------------------ */
typedef void (*gkia_t)(void *ctrl, uint32_t t, void *node, int *idx, float *alpha);   /* softfp: t as bits */
static gkia_t o_gkia;
#define SORT_N 2048
static struct { const float *p; uint16_t n; uint8_t ok; } s_sorted[SORT_N];

static inline float f_of(uint32_t b) { union { uint32_t i; float f; } u; u.i = b; return u.f; }

static int sorted_ok(const float *t, unsigned n) {
  uint32_t h = (((uint32_t)(uintptr_t)t >> 2) ^ (n * 2654435761u)) & (SORT_N - 1);
  if (s_sorted[h].p == t && s_sorted[h].n == n) return s_sorted[h].ok;
  int ok = 1;
  for (unsigned i = 0; i + 1 < n; i++)
    if (!(t[i] <= t[i + 1])) { ok = 0; break; }    /* NaN fails too */
  s_sorted[h].p = t;
  s_sorted[h].n = (uint16_t)n;
  s_sorted[h].ok = (uint8_t)ok;
  return ok;
}

static void h_gkia(void *ctrl, uint32_t tb, void *node, int *idx, float *alpha) {
  if (!s_on || ai_thread_on_worker()) { o_gkia(ctrl, tb, node, idx, alpha); return; }
  w_key++;
  float t = f_of(tb);
  unsigned n = *(const uint16_t *)((const uint8_t *)ctrl + 6);
  const float *times = *(const float *const *)((const uint8_t *)node + 0x44) + *(const uint16_t *)((const uint8_t *)ctrl + 8);
  unsigned i;
  if (n >= 8 && sorted_ok(times, n)) {
    unsigned lo = 0, hi = n;                       /* first i with !(times[i] < t) */
    while (lo < hi) {
      unsigned mid = (lo + hi) >> 1;
      if (times[mid] < t) lo = mid + 1;
      else hi = mid;
    }
    i = lo;
    w_key_bin++;
  } else {
    i = 0;
    while (i < n && times[i] < t) i++;
  }
  float a = 1.0f;
  if (i != 0 && i != n) a = (t - times[i - 1]) / (times[i] - times[i - 1]);
#if ANIM_FAST_VERIFY
  int ei = 0;
  float ea = 0;
  o_gkia(ctrl, tb, node, &ei, &ea);
  ver_note(ei == (int)i && !memcmp(&ea, &a, 4), "GetKeyIndexAndAlpha");
#endif
  *idx = (int)i;
  *alpha = a;
}

/* ---- the two memos ----------------------------------------------------------------- */
static volatile int s_lock;
static inline void lock(void) { while (__sync_lock_test_and_set(&s_lock, 1)) {} }
static inline void unlock(void) { __sync_lock_release(&s_lock); }
#define NAME_MAX_LEN 31
static inline uint32_t name_hash(const char *s, unsigned *len) {
  uint32_t h = 2166136261u;
  unsigned n = 0;
  while (s[n] && n <= NAME_MAX_LEN) { h = (h ^ (uint8_t)s[n]) * 16777619u; n++; }
  *len = n;
  return h;
}

/* FindAnimation */
typedef void *(*findanim_t)(void *model, const char *name);
static findanim_t o_findanim;
#define ANIM_N 1024
typedef struct {
  void *model, *mlist, *owner, *olist, *anim;
  int mcount, ocount, depth, idx;
  uint8_t len;
  char name[NAME_MAX_LEN + 1];
} anim_ent_t;
static anim_ent_t s_anim[ANIM_N];
#define M_LIST(m) (*(void ***)((uint8_t *)(m) + 0x58))
#define M_COUNT(m) (*(int *)((uint8_t *)(m) + 0x5c))
#define M_SUPER(m) (*(void **)((uint8_t *)(m) + 0x64))

/* Where the engine's search finds name (depth in the supermodel chain, index), or 0. */
static int locate(void *model, const char *name, void *found, void **owner, int *depth, int *idx) {
  void *m = model;
  for (int d = 0; m && d < 8; d++, m = M_SUPER(m)) {
    int c = M_COUNT(m);
    void **l = M_LIST(m);
    for (int i = 0; i < c; i++)
      if (l[i] == found) { *owner = m; *depth = d; *idx = i; return 1; }
  }
  return 0;
}

static void *h_findanim(void *model, const char *name) {
  if (!s_on || !model || !name) return o_findanim(model, name);
  w_anim++;
  unsigned len;
  uint32_t h = name_hash(name, &len);
  if (len > NAME_MAX_LEN) return o_findanim(model, name);
  anim_ent_t *e = &s_anim[(h ^ ((uint32_t)(uintptr_t)model >> 3) * 2654435761u) & (ANIM_N - 1)];
  lock();
  if (e->model == model && e->len == len && !memcmp(e->name, name, len) && M_LIST(model) == e->mlist &&
      M_COUNT(model) == e->mcount) {
    void *m = model;
    for (int d = 0; m && d < e->depth; d++) m = M_SUPER(m);
    void *anim = e->anim;
    if (m == e->owner && M_LIST(m) == e->olist && M_COUNT(m) == e->ocount && e->idx < e->ocount &&
        M_LIST(m)[e->idx] == anim) {
      unlock();
      if (!f_stricmp((const char *)anim + 8, name)) {
        w_anim_hit++;
#if ANIM_FAST_VERIFY
        ver_note(o_findanim(model, name) == anim, "FindAnimation");
#endif
        return anim;
      }
      lock();
    }
  }
  unlock();
  void *r = o_findanim(model, name);
  void *owner;
  int depth, idx;
  /* Only a direct hit: the slot found must carry this very name. */
  if (r && !f_stricmp((const char *)r + 8, name) && locate(model, name, r, &owner, &depth, &idx)) {
    lock();
    e->model = model;
    e->mlist = M_LIST(model);
    e->mcount = M_COUNT(model);
    e->owner = owner;
    e->olist = M_LIST(owner);
    e->ocount = M_COUNT(owner);
    e->depth = depth;
    e->idx = idx;
    e->anim = r;
    e->len = (uint8_t)len;
    memcpy(e->name, name, len);
    unlock();
  }
  return r;
}

/* C2DA::GetColumnIndex */
typedef int (*colidx_t)(void *tda, const void *name);
static colidx_t o_colidx;
#define COL_N 512
typedef struct {
  void *tda, *store;
  int count, packed, idx;
  uint8_t len;
  char name[NAME_MAX_LEN + 1];
} col_ent_t;
static col_ent_t s_col[COL_N];
#define T_COUNT(t) (*(int *)((uint8_t *)(t) + 0x28))
#define T_PACKED(t) (*(int *)((uint8_t *)(t) + 0x3c))
static void *t_store(void *t) {
  return T_PACKED(t) ? *(void **)((uint8_t *)t + 0x48) : *(void **)((uint8_t *)t + 0x34);
}
/* Column idx's name equal to name, as the engine compares. */
static int col_is(void *t, int idx, const void *name, const char *cname) {
  if (T_PACKED(t)) {
    const char *base = *(const char **)((uint8_t *)t + 0x48);
    const uint16_t *off = *(const uint16_t **)((uint8_t *)t + 0x50);
    return !f_stricmp(base + off[idx], cname);
  }
  const uint8_t *cols = *(const uint8_t **)((uint8_t *)t + 0x34);
  return f_cmpnc(cols + 8 * idx, name) != 0;
}

static int h_colidx(void *tda, const void *name) {
  if (!s_on || !tda || !name) return o_colidx(tda, name);
  w_col++;
  const char *cname = f_cstr(name);
  if (!cname) return o_colidx(tda, name);
  unsigned len;
  uint32_t h = name_hash(cname, &len);
  if (len > NAME_MAX_LEN) return o_colidx(tda, name);
  col_ent_t *e = &s_col[(h ^ ((uint32_t)(uintptr_t)tda >> 3) * 2654435761u) & (COL_N - 1)];
  lock();
  int hit = e->tda == tda && e->len == len && !memcmp(e->name, cname, len) && e->store == t_store(tda) &&
            e->count == T_COUNT(tda) && e->packed == (T_PACKED(tda) != 0) && e->idx < e->count;
  int idx = e->idx;
  unlock();
  if (hit && col_is(tda, idx, name, cname)) {
    w_col_hit++;
#if ANIM_FAST_VERIFY
    ver_note(o_colidx(tda, name) == idx, "GetColumnIndex");
#endif
    return idx;
  }
  int r = o_colidx(tda, name);
  if (r >= 0) {
    lock();
    e->tda = tda;
    e->store = t_store(tda);
    e->count = T_COUNT(tda);
    e->packed = T_PACKED(tda) != 0;
    e->idx = r;
    e->len = (uint8_t)len;
    memcpy(e->name, cname, len);
    unlock();
  }
  return r;
}

/* ---- install, report ------------------------------------------------------------------ */
void anim_fast_install(void) {
  char m = '1';
  SceUID fd = sceIoOpen(DATA_PATH "/anim_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char ch = 0; if (sceIoRead(fd, &ch, 1) == 1) m = ch; sceIoClose(fd); }
  if (m == '0') { log_printf("[animfast] off (anim_mode 0)"); s_on = 0; return; }
  f_stricmp = (int (*)(const char *, const char *))so_symbol(&kotor_mod, "_stricmp");
  f_cmpnc = (int (*)(const void *, const void *))so_symbol(&kotor_mod, "_ZNK10CExoString13CompareNoCaseERKS_");
  f_cstr = (const char *(*)(const void *))so_symbol(&kotor_mod, "_ZNK10CExoString4CStrEv");
  /* The functions the answers stand for must be the ones read. */
  int ok = f_stricmp && f_cmpnc && f_cstr &&
           so_symbol(&kotor_mod, "_ZN13NewController19GetKeyIndexAndAlphaEfP7MdlNodeRiRf") == kotor_mod.text_base + 0x4d5bec + 1 &&
           so_symbol(&kotor_mod, "_Z13FindAnimationP5ModelPc") == kotor_mod.text_base + 0x4d4cc4 + 1 &&
           so_symbol(&kotor_mod, "_ZN4C2DA14GetColumnIndexERK10CExoString") == kotor_mod.text_base + 0x564790 + 1;
  if (!ok) { log_printf("[animfast] NOT armed: the functions are not the expected code"); s_on = 0; return; }
  o_gkia = (gkia_t)take_slots("_ZN13NewController19GetKeyIndexAndAlphaEfP7MdlNodeRiRf", (uintptr_t)h_gkia);
  o_findanim = (findanim_t)take_slots("_Z13FindAnimationP5ModelPc", (uintptr_t)h_findanim);
  o_colidx = (colidx_t)take_slots("_ZN4C2DA14GetColumnIndexERK10CExoString", (uintptr_t)h_colidx);
  log_printf("[animfast] key search %s, FindAnimation memo %s, GetColumnIndex memo %s%s", o_gkia ? "on" : "NOT armed",
             o_findanim ? "on" : "NOT armed", o_colidx ? "on" : "NOT armed", ANIM_FAST_VERIFY ? " (verifying)" : "");
}

static uint64_t s_t0;
static uint32_t s_frames;
void anim_fast_on_swap(void) {
  if (!s_on) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  s_frames++;
  if (!s_t0) { s_t0 = now; return; }
  if (now - s_t0 < 10000000u) return;
  double f = s_frames;
  log_printf("[animfast] %u frames | key searches %.0f a frame (%.0f%% binary) | FindAnimation %.0f a frame (%.0f%% "
             "remembered) | GetColumnIndex %.0f a frame (%.0f%% remembered)",
             (unsigned)s_frames, w_key / f, w_key ? 100.0 * w_key_bin / w_key : 0.0, w_anim / f,
             w_anim ? 100.0 * w_anim_hit / w_anim : 0.0, w_col / f, w_col ? 100.0 * w_col_hit / w_col : 0.0);
#if ANIM_FAST_VERIFY
  log_printf("[animfast] VERIFY: %u answers checked, %u different", (unsigned)w_ver_n, (unsigned)w_ver_bad);
  w_ver_n = w_ver_bad = 0;
#endif
  w_key = w_key_bin = w_anim = w_anim_hit = w_col = w_col_hit = 0;
  s_frames = 0;
  s_t0 = now;
}

#else
void anim_fast_install(void) {}
void anim_fast_on_swap(void) {}
#endif
