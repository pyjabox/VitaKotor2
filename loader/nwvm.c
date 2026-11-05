/* nwvm.c -- the NWScript interpreter (CVirtualMachineInternal::ExecuteCode) in
 * C, with the stack's common operations done in place.
 *
 * In the Khoonda battle the AI used 50-80 ms of CPU a frame, and one creature's
 * AIUpdate about 6 ms, nearly all of it scripts (hardware, 2026-11-05). In the
 * AI's profile the interpreter loop itself was a third of the script time, and
 * every instruction also called the engine's stack helpers: StackPopInteger,
 * StackPushInteger -> AddToTopOfStack, SetStackPointer, AssignLocationToLocation
 * (about 10% of the AI more). This is the same loop -- libkotor2 0x588ce4,
 * transcribed handler by handler, the engine's address next to each -- doing
 * the helpers' int, float and object cases itself. Strings and engine
 * structures still go through the engine's own functions (they own memory),
 * and so do the stack's growth and anything unusual.
 *
 * What is kept exactly: the stack layout (CVirtualMachineStack at VM+0x1ac:
 * pointer, base, capacity, type bytes, values, owner), the instruction counter
 * (VM+0x24, at most 131070), the return-address stack (VM+0x1c4/+0x1c8, 128),
 * the stored state for DelayCommand (VM+0x3c8..0x3d0), *pIP written after every
 * instruction, each opcode's operand decoding (big-endian, the engine's signed
 * and unsigned reads), its quirks (SHRIGHTII shifts a negative number's
 * magnitude, USHRIGHTII is arithmetic, vector / float multiplies by 1/f, vector
 * +/- float pops and pushes nothing), and every error code.
 * NWVM_VERIFY runs generated programs through both at start (nwvm_verify.c).
 *
 * ux0:data/kotor2/nwvm_mode.txt: 0 leaves the engine's interpreter in place. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "dynlib.h"
#include "nwvm.h"
#include "script_cache.h"

#if NWVM

/* ---- the engine's structures --------------------------------------------------- */
typedef struct {
  int32_t sp, bp, cap;
  int8_t *types;
  int32_t *values;
  void *owner;
} vmstack_t;
#define STK(vm) ((vmstack_t *)((uint8_t *)(vm) + 0x1ac))
#define VMI(vm, off) (*(int32_t *)((uint8_t *)(vm) + (off)))
#define VM_COUNT 0x24
#define VM_DEPTH 0x28
#define VM_SELF_OK 0x16c          /* [depth] == 1: the script has an object */
#define VM_SELF 0x18c             /* [depth]: OBJECT_SELF */
#define VM_RET_N 0x1c4
#define VM_RET 0x1c8              /* int[128] */
#define VM_STATE_IP 0x3c8
#define VM_STATE_A 0x3cc
#define VM_STATE_B 0x3d0
#define VM_IMPL 0x3d4
#define LIMIT 131070
#define OBJ_INVALID 0x7f000000

enum { T_INT = 3, T_FLOAT = 4, T_STRING = 5, T_OBJECT = 6 };
enum {
  E_DONE = 0, E_POP = -100, E_TYPE = -97, E_DIV0 = -105, E_OVERFLOW = -99, E_NOIMPL = -108,
  E_EQTYPE = -109, E_OPCODE = -101, E_END = -107, E_LIMIT = -93
};

typedef struct { char *s; uint32_t len; } exostr_t;   /* CExoString */

/* The command implementer's virtual functions the loop calls. */
typedef int (*impl_run_t)(void *impl, int cmd, int argc);                   /* vtable +12 */
typedef int (*impl_cmp_t)(void *impl, int type, void *a, void *b);           /* vtable +32 */

/* Engine functions. */
static int (*o_exec)(void *vm, int *pip, char *code, int size);
static void (*f_add_top)(vmstack_t *s, int type);
static void (*f_set_sp)(vmstack_t *s, int n);
static void (*f_assign)(vmstack_t *s, int src, int dst);
static int (*f_push_string)(void *vm, const exostr_t *str);
static int (*f_pop_string)(void *vm, exostr_t *str);
static int (*f_push_struct)(void *vm, int type, void *p);
static void (*f_str_ctor)(exostr_t *s);
static void (*f_str_ctor_c)(exostr_t *s, const char *c);
static void (*f_str_dtor)(exostr_t *s);
static void (*f_str_plus)(exostr_t *ret, const exostr_t *a, const exostr_t *b);
static int (*f_str_cmpnc)(const exostr_t *a, const exostr_t *b);
static void (*f_str_format)(exostr_t *s, const char *fmt, ...);
static void *(*f_new_array)(size_t n);
static void (*f_delete_array)(void *p);

static int s_on = 1;
static uint32_t w_calls, w_insns;

/* ---- the stack, in place ----------------------------------------------------- */
static inline int is_owned(int8_t t) { return t == T_STRING || (uint8_t)(t - 16) <= 9; }

/* StackPush{Integer,Float,Object}: AddToTopOfStack(type), then the value into
 * the new top. In place while there is room; the engine grows the stack. */
static inline void push(void *vm, int8_t t, int32_t v) {
  vmstack_t *s = STK(vm);
  int32_t sp = s->sp;
  if (sp >= 0 && sp < s->cap) {
    s->types[sp] = t;
    s->values[sp] = v;
    s->sp = sp + 1;
    return;
  }
  f_add_top(s, t);
  s->values[s->sp - 1] = v;
}

/* SetStackPointer: only ever lowers it; strings and structures freed by the engine. */
static inline void set_sp(vmstack_t *s, int32_t n) {
  int32_t sp = s->sp;
  if (sp <= n) return;
  for (int32_t i = n < 0 ? 0 : n; i < sp; i++)
    if (is_owned(s->types[i])) { f_set_sp(s, n); return; }
  if (n < 0) { f_set_sp(s, n); return; }
  s->sp = n;
}

/* StackPop{Integer,Float,Object}: the top must have that type. */
static inline int pop(void *vm, int8_t t, int32_t *out) {
  vmstack_t *s = STK(vm);
  int32_t sp = s->sp;
  if (sp < 1 || s->types[sp - 1] != t) return 0;
  *out = s->values[sp - 1];
  s->sp = sp - 1;                    /* SetStackPointer(sp - 1): an int, float or object frees nothing */
  return 1;
}

/* StackPopVector: z, y, x (three floats; a failure leaves what was popped). */
static inline int pop_vector(void *vm, int32_t v[3]) {
  return pop(vm, T_FLOAT, &v[2]) && pop(vm, T_FLOAT, &v[1]) && pop(vm, T_FLOAT, &v[0]);
}
static inline void push_vector(void *vm, const int32_t v[3]) {
  push(vm, T_FLOAT, v[0]);
  push(vm, T_FLOAT, v[1]);
  push(vm, T_FLOAT, v[2]);
}

/* AssignLocationToLocation(src, dst): a copy of a slot, pushing first when dst
 * is the top. Strings and structures: the engine's (it copies them). */
static inline void assign(vmstack_t *s, int32_t src, int32_t dst) {
  if ((src | dst) < 0 || !s->owner) return;
  int8_t t = s->types[src];
  if (is_owned(t)) { f_assign(s, src, dst); return; }
  if (dst == s->sp) {
    if (dst < s->cap && (t == T_INT || t == T_FLOAT || t == T_OBJECT)) {
      s->types[dst] = t;
      s->sp = dst + 1;
    } else {
      f_add_top(s, t);               /* grows, or does nothing for an unknown type */
    }
  }
  s->values[dst] = s->values[src];
  s->types[dst] = s->types[src];
}

static inline int32_t be32(const uint8_t *p) {
  return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}
static inline int32_t be16s(const uint8_t *p) { return (int16_t)((uint16_t)(p[0] << 8 | p[1])); }
/* base - ((-off) >> 2): the engine's slot arithmetic for byte offsets. */
static inline int32_t slot_of(int32_t base, int32_t off) { return base - ((int32_t)(0u - (uint32_t)off) >> 2); }

/* ARM register shifts: the amount's low byte; 32 and up shift everything out. */
static inline int32_t arm_lsl(int32_t v, int32_t n) { uint32_t s = (uint32_t)n & 0xff; return s >= 32 ? 0 : (int32_t)((uint32_t)v << s); }
static inline int32_t arm_lsr(int32_t v, int32_t n) { uint32_t s = (uint32_t)n & 0xff; return s >= 32 ? 0 : (int32_t)((uint32_t)v >> s); }
static inline int32_t arm_asr(int32_t v, int32_t n) { uint32_t s = (uint32_t)n & 0xff; return s >= 32 ? (v < 0 ? -1 : 0) : v >> s; }

static inline float f_of(int32_t b) { union { int32_t i; float f; } u; u.i = b; return u.f; }
static inline int32_t b_of(float f) { union { int32_t i; float f; } u; u.f = f; return u.i; }

/* ---- global set-up snapshots ---------------------------------------------------
 * A compiled script with globals starts JSR 8; RETN; then sets its globals up --
 * RSADD, CONST, NEG, CPDOWNSP -8, MOVSP -4 for each -- and ends that block
 * with SAVEBP before calling main. k_ai_master's block is 720 instructions
 * (186 ints and an object), the k_def_ and k_hen_ wrappers' 120: ~70% of the
 * instructions the AI interprets in a battle (static reading of the dumped
 * scripts, 2026-11-05), all to rebuild the same constants every run. A block
 * made only of those instructions on ints, floats and RSADD objects has a
 * fixed result: it is worked out once per script and copied onto the stack,
 * with the instruction counter advanced as if it had run. The first run of
 * each script still interprets the block, and the snapshot is used only if it
 * matches what that run left on the stack. */
#define SNAP_N 64
#define SNAP_MAX_SLOTS 1024
#define SNAP_MAX_BYTES 16384
typedef struct {
  uint32_t key;
  int32_t size;
  int state;                        /* 0 empty, 1 checked, 2 to check on this run, -1 not usable */
  uint32_t end, insns, nslots, nbytes;
  int8_t *types;
  int32_t *values;
  uint8_t *bytes;                   /* the block, [8, end), to recognise the script */
} snap_t;
static snap_t s_snap[SNAP_N];
static snap_t *s_checking;          /* the snapshot this run checks, its code, and where its block began */
static const uint8_t *s_check_code;
static int32_t s_check_sp;
static uint32_t s_check_n0;
static uint32_t w_snap_used, w_snap_insns, s_snap_bad;

/* The block's result, worked out without running it; 0 if it does anything else. */
static int snap_build(snap_t *e, const uint8_t *code, int32_t size) {
  static int8_t ty[SNAP_MAX_SLOTS];
  static int32_t va[SNAP_MAX_SLOTS];
  int32_t sp = 0, ip = 8;
  uint32_t n = 0;
  for (;;) {
    if (ip + 2 > size || ip - 8 > SNAP_MAX_BYTES) return 0;
    uint8_t op = code[ip], t = code[ip + 1];
    if (op == 42) break;                                           /* SAVEBP: the block's end */
    n++;
    switch (op) {
    case 2:                                                        /* RSADD */
      if (sp >= SNAP_MAX_SLOTS) return 0;
      if (t == T_INT || t == T_FLOAT) { ty[sp] = (int8_t)t; va[sp] = 0; }
      else if (t == T_OBJECT) { ty[sp] = T_OBJECT; va[sp] = OBJ_INVALID; }
      else return 0;
      sp++;
      ip += 2;
      break;
    case 4:                                                        /* CONST: ints and floats only */
      if (ip + 6 > size || sp >= SNAP_MAX_SLOTS || (t != T_INT && t != T_FLOAT)) return 0;
      ty[sp] = (int8_t)t;
      va[sp] = be32(code + ip + 2);
      sp++;
      ip += 6;
      break;
    case 25:                                                       /* NEG */
      if (sp < 1 || ty[sp - 1] != t) return 0;
      if (t == T_INT) va[sp - 1] = (int32_t)(0u - (uint32_t)va[sp - 1]);
      else if (t == T_FLOAT) va[sp - 1] = (int32_t)((uint32_t)va[sp - 1] ^ 0x80000000u);
      else return 0;
      ip += 2;
      break;
    case 1: {                                                      /* CPDOWNSP, inside the block */
      if (ip + 8 > size) return 0;
      int32_t dst = slot_of(sp, be32(code + ip + 2)), cnt = be16s(code + ip + 6) >> 2;
      if (dst < 0 || cnt < 1 || dst + cnt > sp || sp - cnt < 0) return 0;
      for (int32_t i = 0; i < cnt; i++) {
        ty[dst + i] = ty[sp - cnt + i];
        va[dst + i] = va[sp - cnt + i];
      }
      ip += 8;
      break;
    }
    case 27: {                                                     /* MOVSP: down only */
      if (ip + 6 > size) return 0;
      int32_t to = slot_of(sp, be32(code + ip + 2));
      if (to < 0 || to > sp) return 0;
      sp = to;
      ip += 6;
      break;
    }
    default:
      return 0;
    }
  }
  if (!sp) return 0;
  e->end = (uint32_t)ip;
  e->insns = n;
  e->nslots = (uint32_t)sp;
  e->nbytes = (uint32_t)(ip - 8);
  e->types = (int8_t *)malloc((size_t)sp);
  e->values = (int32_t *)malloc((size_t)sp * 4);
  e->bytes = (uint8_t *)malloc(e->nbytes);
  if (!e->types || !e->values || !e->bytes) {
    free(e->types); free(e->values); free(e->bytes);
    e->types = NULL; e->values = NULL; e->bytes = NULL;
    return 0;
  }
  memcpy(e->types, ty, (size_t)sp);
  memcpy(e->values, va, (size_t)sp * 4);
  memcpy(e->bytes, code + 8, e->nbytes);
  return 1;
}

/* At ip 8, just after the script's opening JSR 8: copy the snapshot in and
 * go on at SAVEBP (1), or interpret the block (0). */
static int snap_enter(void *vm, int *pip, const uint8_t *code, int32_t size) {
  int32_t rn = VMI(vm, VM_RET_N);
  if (size < 16 || code[0] != 30 || be32(code + 2) != 8 || code[6] != 32 || rn < 1 || VMI(vm, VM_RET + 4 * (rn - 1)) != 6)
    return 0;
  uint32_t key = 2166136261u ^ (uint32_t)size;
  for (int32_t i = 8; i < 72 && i < size; i++) key = (key ^ code[i]) * 16777619u;
  snap_t *e = NULL;
  for (uint32_t k = 0; k < SNAP_N; k++) {
    snap_t *c = &s_snap[(key + k) & (SNAP_N - 1)];
    if (c->state && c->key == key && c->size == size) { e = c; break; }
    if (!c->state) {                                               /* first sight: work it out, check it on this run */
      c->key = key;
      c->size = size;
      c->state = snap_build(c, code, size) ? 2 : -1;
      e = c;
      break;
    }
  }
  if (!e || e->state < 0) return 0;
  if ((uint32_t)size < e->end + 2 || code[e->end] != 42 || memcmp(e->bytes, code + 8, e->nbytes)) return 0;
  if (e->state == 2) {                                             /* interpret it once, compare at SAVEBP */
    s_checking = e;
    s_check_code = code;
    s_check_sp = STK(vm)->sp;
    s_check_n0 = (uint32_t)VMI(vm, VM_COUNT);
    return 0;
  }
  int32_t n = VMI(vm, VM_COUNT);
  if ((int64_t)n + e->insns > LIMIT) return 0;                     /* let the loop reach the limit itself */
  for (uint32_t i = 0; i < e->nslots; i++) push(vm, e->types[i], e->values[i]);
  VMI(vm, VM_COUNT) = n + (int32_t)e->insns;
  *pip = (int)e->end;
  w_snap_used++;
  w_snap_insns += e->insns;
  return 1;
}

/* At the checked block's SAVEBP: the stack it built against the snapshot. */
static void snap_check(void *vm) {
  snap_t *e = s_checking;
  s_checking = NULL;
  vmstack_t *s = STK(vm);
  int ok = s->sp == s_check_sp + (int32_t)e->nslots &&
           (uint32_t)VMI(vm, VM_COUNT) - s_check_n0 == e->insns;
  for (uint32_t i = 0; ok && i < e->nslots; i++)
    ok = s->types[s_check_sp + (int32_t)i] == e->types[i] && s->values[s_check_sp + (int32_t)i] == e->values[i];
  e->state = ok ? 1 : -1;
  if (!ok && s_snap_bad++ < 8)
    log_printf("[nwvm] global set-up snapshot did not match a run (%u slots, %u instructions): not used for that script",
               (unsigned)e->nslots, (unsigned)e->insns);
}

/* ---- OnDialogue runs that do nothing (NWVM_DIALOGUE_SKIP) -------------------------
 * A creature hears every shout in range: CSWSCreature::EventHandler stores the
 * shout's listen pattern at creature+0x1bc (0x476de2) and runs its OnDialogue
 * script (RunScript, returning to 0x476f16; the result is not used). For
 * ordinary creatures that is k_def_dialogue01, which only ExecuteScripts
 * k_ai_master with 1004. Read in their bytecode (2026-11-05), k_ai_master's
 * 1004 handler (0x19d1-0x1e7c) and GN_RespondToShout (0x1cf8c) change nothing
 * unless one of these holds:
 *  - the pattern is -1 (a real conversation) or 1 (the only one
 *    GN_RespondToShout acts on: the branch for 15 sits inside its `== 1`);
 *  - local boolean 97 (OnDialogue user event at the start), 26 (at the end)
 *    or 86 (ClearAllActions when the shout is refused) is set.
 * Everything else on those paths is getters, and debug prints that ShipBuild
 * (always 1) turns off. In the battle the round-end shouts (15) made ~30
 * listeners a frame run all of it (7-17 ms a frame on hardware). Those runs
 * are skipped -- only for these two scripts byte for byte (sizes and FNV-1a
 * of the dumped code, checked against the code the VM runs, again whenever
 * the script cache empties), and only while the script cache is on (so the
 * check is once per buffer). A creature's CSWVarTable is at +0x114, as the
 * engine's own callers of GetLocalBoolean use it. dialogue_mode.txt 0: off. */
#define KAM_SIZE 121624
#define KAM_FNV 0x6a2cbe60u
#define KDD_SIZE 722
#define KDD_FNV 0x75b3875eu
#define ONDIALOGUE_RET 0x476f16u
static int s_dlg_on;
static uintptr_t s_app_mgr, s_vt_creature;
static void *(*f_get_obj)(void *app, uint32_t oid);
static int (*f_get_bool)(void *vartable, int n);
typedef struct { const uint8_t *buf; uint32_t gen; int good; } fp_t;   /* gen: cache generation + 1 when checked */
static fp_t s_fp_kam, s_fp_kdd;
static uint32_t w_dlg_runs, w_dlg_skipped, s_fp_logged;

static uint32_t fnv1a(const uint8_t *p, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}
static void fp_check(fp_t *f, const char *nm, const uint8_t *code, int size, int want_size, uint32_t want_fnv) {
  uint32_t gen = script_cache_generation() + 1;
  if (f->buf == code && f->gen == gen) return;
  f->buf = code;
  f->gen = gen;
  f->good = size == want_size && fnv1a(code, size) == want_fnv;
  if (s_fp_logged++ < 6)
    log_printf("[nwvm] %s is %s the script the OnDialogue skip was checked against (%d bytes)", nm,
               f->good ? "" : "NOT", size);
}
/* At a script's start: its fingerprint, if it is one of the two. */
static void fp_seen(void *vm, const uint8_t *code, int size) {
  if (!s_dlg_on || script_cache_phase() != 1) return;
  int32_t d = VMI(vm, VM_DEPTH);
  if (d < 0 || d >= 8) return;
  const char *nm = *(const char **)((uint8_t *)vm + 0x2c + d * 40 + 24);
  if (!nm) return;
  if (!strcasecmp(nm, "k_ai_master")) fp_check(&s_fp_kam, "k_ai_master", code, size, KAM_SIZE, KAM_FNV);
  else if (!strcasecmp(nm, "k_def_dialogue01")) fp_check(&s_fp_kdd, "k_def_dialogue01", code, size, KDD_SIZE, KDD_FNV);
}

int nwvm_dialogue_skip(const void *name, uint32_t oid, uintptr_t ret) {
  if (!s_dlg_on || !s_on || ((ret & ~(uintptr_t)1) - kotor_mod.text_base) != ONDIALOGUE_RET) return 0;
  const char *n = name ? *(const char *const *)name : NULL;
  if (!n || strcasecmp(n, "k_def_dialogue01")) return 0;
  w_dlg_runs++;
  uint32_t gen = script_cache_generation() + 1;
  if (script_cache_phase() != 1 || !s_fp_kam.good || s_fp_kam.gen != gen || !s_fp_kdd.good || s_fp_kdd.gen != gen)
    return 0;
  void *am = *(void **)s_app_mgr;
  void *app = am ? *(void **)((uint8_t *)am + 8) : NULL;
  uint8_t *o = app ? (uint8_t *)f_get_obj(app, oid) : NULL;
  if (!o || *(uintptr_t *)o != s_vt_creature) return 0;
  int32_t pattern = *(int32_t *)(o + 0x1bc);
  if (pattern == -1 || pattern == 1) return 0;
  void *vars = o + 0x114;
  if (f_get_bool(vars, 97) || f_get_bool(vars, 26) || f_get_bool(vars, 86)) return 0;
  w_dlg_skipped++;
  return 1;
}

#if !BATTLE_LEDGER                    /* BATTLE_LEDGER builds take RunScript's slot and ask nwvm_dialogue_skip */
typedef int (*runscript_t)(void *vm, void *name, uint32_t oid, int a);
static runscript_t o_runscript;
static int h_runscript(void *vm, void *name, uint32_t oid, int a) {
  if (nwvm_dialogue_skip(name, oid, (uintptr_t)__builtin_return_address(0))) return 1;
  return o_runscript(vm, name, oid, a);
}
#endif

/* ---- the loop ------------------------------------------------------------------ */
static int nw_exec(void *vm, int *pip, char *code_c, int size) {
  const uint8_t *code = (const uint8_t *)code_c;
  vmstack_t *s = STK(vm);
  w_calls++;
  if (*pip == 0) fp_seen(vm, code, size);
  for (;;) {
    int32_t ip = *pip;
    if (ip == 8) {
      s_checking = NULL;                                           /* a check left by a run that failed */
      if (snap_enter(vm, pip, code, size)) continue;
    }
    if (s_checking && code == s_check_code && (uint32_t)ip == s_checking->end && code[ip] == 42) snap_check(vm);
    if (ip == -1) return E_DONE;                                   /* 588d1c */
    if (ip + 1 >= size) return E_END;                              /* 588d22 */
    int32_t n = VMI(vm, VM_COUNT);
    VMI(vm, VM_COUNT) = n + 1;
    if (n > LIMIT) return E_LIMIT;                                 /* 588d30 */
    uint8_t op = code[ip];
    if ((uint8_t)(op - 1) > 43) return E_OPCODE;                    /* 588d3e */
    const uint8_t *c = code + ip;
    uint8_t ty = c[1];
    w_insns++;
    int32_t a, b;
    switch (op) {
    case 1: case 38: {                                             /* CPDOWNSP, CPDOWNBP: 588e90 */
      int32_t dst = slot_of(op == 1 ? s->sp : s->bp, be32(c + 2));
      if (dst < 0) return E_POP;
      int32_t cnt = be16s(c + 6) >> 2;
      for (int32_t i = 0; i < cnt; i++) assign(s, s->sp - cnt + i, dst + i);
      *pip = ip + 8;
      break;
    }
    case 3: case 39: {                                             /* CPTOPSP, CPTOPBP: 588ef2 */
      int32_t src = slot_of(op == 3 ? s->sp : s->bp, be32(c + 2));
      if (src < 0) return E_POP;
      int32_t cnt = be16s(c + 6) >> 2, top = s->sp;
      for (int32_t i = 0; i < cnt; i++) assign(s, src + i, top + i);
      *pip = ip + 8;
      break;
    }
    case 2:                                                        /* RSADD: 589006 */
      if (ty == T_INT) push(vm, T_INT, 0);
      else if (ty == T_FLOAT) push(vm, T_FLOAT, 0);
      else if (ty == T_STRING) {                                   /* 58989e */
        exostr_t e;
        f_str_ctor_c(&e, "");
        f_push_string(vm, &e);
        f_str_dtor(&e);
      } else if (ty == T_OBJECT) push(vm, T_OBJECT, OBJ_INVALID);
      else if ((uint8_t)(ty - 16) <= 9) {                          /* 589874 */
        if (!f_push_struct(vm, ty - 16, NULL)) return E_OVERFLOW;
      } else return E_TYPE;
      *pip = ip + 2;
      break;
    case 4:                                                        /* CONST: 589022 */
      if (ty == T_INT) { push(vm, T_INT, be32(c + 2)); *pip = ip + 6; }
      else if (ty == T_FLOAT) { push(vm, T_FLOAT, be32(c + 2)); *pip = ip + 6; }
      else if (ty == T_STRING) {                                   /* 5897e0 */
        int32_t len = be16s(c + 2);
        if (len < 0) return E_OPCODE;                              /* the engine writes outside its buffer here */
        exostr_t e;
        f_str_ctor(&e);
        char *buf = (char *)f_new_array((size_t)len + 1);
        memcpy(buf, c + 4, (size_t)len);
        buf[len] = 0;
        f_str_format(&e, "%s", buf);
        f_delete_array(buf);
        f_push_string(vm, &e);
        *pip = *pip + len + 4;
        f_str_dtor(&e);
      } else if (ty == T_OBJECT) {                                 /* 58981a */
        int32_t v = be32(c + 2), o = OBJ_INVALID;
        if (v == 0) {                                              /* OBJECT_SELF: 589976 */
          int32_t d = VMI(vm, VM_DEPTH);
          o = VMI(vm, VM_SELF_OK + 4 * d) == 1 ? VMI(vm, VM_SELF + 4 * d) : OBJ_INVALID;
        }
        push(vm, T_OBJECT, o);
        *pip = ip + 6;
      } else return E_TYPE;
      break;
    case 5: {                                                      /* ACTION: 589064 */
      void *impl = *(void **)((uint8_t *)vm + VM_IMPL);
      if (!impl) return E_NOIMPL;
      impl_run_t run = (impl_run_t)(*(void ***)impl)[3];
      int r = run(impl, be16s(c + 2), c[4]);
      if (r < 0) return r;
      *pip = *pip + 5;
      break;
    }
    case 6: case 7: case 8: case 9: case 10:                      /* LOGANDII .. BOOLANDII */
      if (ty != 0x20) return E_TYPE;
      if (!pop(vm, T_INT, &a) || !pop(vm, T_INT, &b)) return E_POP;
      if (op == 6) push(vm, T_INT, (a != 0) & (b != 0));
      else if (op == 7) push(vm, T_INT, (a | b) != 0);
      else if (op == 8) push(vm, T_INT, a | b);
      else if (op == 9) push(vm, T_INT, a ^ b);
      else push(vm, T_INT, a & b);
      *pip = *pip + 2;
      break;
    case 11: case 12: {                                            /* EQUAL, NEQUAL: 588f50 */
      int32_t bytes, ilen;
      if ((uint8_t)(ty - 0x20) < 5) {
        if (ty == 0x24) { bytes = (int32_t)(c[2] << 8 | c[3]); ilen = 4; }
        else { bytes = 4; ilen = 2; }
      } else if ((uint8_t)(ty - 0x30) < 10) { bytes = 4; ilen = 2; }
      else return E_TYPE;
      int32_t sh = (int32_t)((uint32_t)bytes << 16);
      if (s->sp < (sh >> 17)) return E_POP;                        /* 5894ba */
      int32_t cnt = sh >> 18, eq = 1, idx = s->sp - 1;
      for (int32_t i = 0; i < cnt; i++, idx--) {
        int8_t t = s->types[idx];
        int32_t x = s->values[idx], y = s->values[idx - cnt];
        if ((uint8_t)(t - 3) < 2 || t == T_OBJECT) { if (x != y) eq = 0; }
        else if (t == T_STRING) { if (!f_str_cmpnc((const exostr_t *)x, (const exostr_t *)y)) eq = 0; }
        else if ((uint8_t)(t - 16) <= 9) {
          void *impl = *(void **)((uint8_t *)vm + VM_IMPL);
          if (!impl) return E_NOIMPL;
          impl_cmp_t cmp = (impl_cmp_t)(*(void ***)impl)[8];
          if (!cmp(impl, t - 16, (void *)x, (void *)y)) eq = 0;
        } else return E_EQTYPE;
      }
      set_sp(s, s->sp - 2 * cnt);
      push(vm, T_INT, op == 11 ? eq : eq == 0);
      *pip = *pip + ilen;
      break;
    }
    case 13: case 14: case 15: case 16: {                          /* GEQ, GT, LT, LEQ: 588da4 */
      int r;
      if (ty == 0x21) {
        if (!pop(vm, T_FLOAT, &a) || !pop(vm, T_FLOAT, &b)) return E_POP;
        float x = f_of(a), y = f_of(b);
        r = op == 13 ? y >= x : op == 14 ? y > x : op == 15 ? y < x : y <= x;
      } else if (ty == 0x20) {
        if (!pop(vm, T_INT, &a) || !pop(vm, T_INT, &b)) return E_POP;
        r = op == 13 ? b >= a : op == 14 ? b > a : op == 15 ? b < a : b <= a;
      } else return E_TYPE;
      push(vm, T_INT, r);
      *pip = *pip + 2;
      break;
    }
    case 17: case 18: case 19:                                     /* SHLEFTII, SHRIGHTII, USHRIGHTII */
      if (ty != 0x20) return E_TYPE;
      if (!pop(vm, T_INT, &a) || !pop(vm, T_INT, &b)) return E_POP;
      if (op == 17) push(vm, T_INT, arm_lsl(b, a));
      else if (op == 18) {                                         /* 589134 */
        if (b <= -1) push(vm, T_INT, (int32_t)(0u - (uint32_t)arm_asr((int32_t)(0u - (uint32_t)b), a)));
        else push(vm, T_INT, arm_lsr(b, a));
      } else push(vm, T_INT, arm_asr(b, a));
      *pip = *pip + 2;
      break;
    case 20: case 21: case 22: case 23:                            /* ADD, SUB, MUL, DIV: 588e4c */
      if (ty == 0x20) {
        if (!pop(vm, T_INT, &a) || !pop(vm, T_INT, &b)) return E_POP;
        int32_t r;
        if (op == 22) r = (int32_t)((uint32_t)a * (uint32_t)b);
        else if (op == 21) r = (int32_t)((uint32_t)b - (uint32_t)a);
        else if (op == 20) r = (int32_t)((uint32_t)b + (uint32_t)a);
        else {
          if (a == 0) return E_DIV0;
          r = (b == INT32_MIN && a == -1) ? INT32_MIN : b / a;
        }
        push(vm, T_INT, r);
        *pip = *pip + 2;
      } else if (ty == 0x21 || ty == 0x25 || ty == 0x26) {         /* FF, IF, FI: 588f96 */
        float x, y;
        if (ty == 0x26) { if (!pop(vm, T_INT, &a)) return E_POP; x = (float)a; }
        else { if (!pop(vm, T_FLOAT, &a)) return E_POP; x = f_of(a); }
        if (ty == 0x25) { if (!pop(vm, T_INT, &b)) return E_POP; y = (float)b; }
        else { if (!pop(vm, T_FLOAT, &b)) return E_POP; y = f_of(b); }
        float r;
        if (op == 22) r = y * x;
        else if (op == 21) r = y - x;
        else if (op == 20) r = y + x;
        else { if (x == 0.0f) return E_DIV0; r = y / x; }
        push(vm, T_FLOAT, b_of(r));
        *pip = *pip + 2;
      } else if (ty == 0x3b || ty == 0x3c) {                       /* VF, FV: 5895b6 */
        int32_t v[3] = {0, 0, 0}, f;
        if (ty == 0x3c) { if (!pop_vector(vm, v) || !pop(vm, T_FLOAT, &f)) return E_POP; }
        else { if (!pop(vm, T_FLOAT, &f) || !pop_vector(vm, v)) return E_POP; }
        if (op == 23 || op == 22) {
          float k = f_of(f);
          if (op == 23) { if (k == 0.0f) return E_DIV0; k = 1.0f / k; }
          int32_t r[3] = {b_of(k * f_of(v[0])), b_of(k * f_of(v[1])), b_of(k * f_of(v[2]))};
          push_vector(vm, r);
        }
        *pip = *pip + 2;
      } else if (ty == 0x3a) {                                     /* VV: 589706 */
        int32_t va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
        if (!pop_vector(vm, va) || !pop_vector(vm, vb)) return E_POP;
        if (op == 20 || op == 21) {
          int32_t r[3];
          for (int k = 0; k < 3; k++)
            r[k] = b_of(op == 20 ? f_of(vb[k]) + f_of(va[k]) : f_of(vb[k]) - f_of(va[k]));
          push_vector(vm, r);
        }
        *pip = *pip + 2;
      } else if (ty == 0x23) {                                     /* SS: 589766 */
        exostr_t sa, sb;
        f_str_ctor(&sa);
        if (!f_pop_string(vm, &sa)) { f_str_dtor(&sa); return E_POP; }
        f_str_ctor(&sb);
        if (!f_pop_string(vm, &sb)) { f_str_dtor(&sb); f_str_dtor(&sa); return E_POP; }
        if (op == 20) {
          exostr_t sc;
          f_str_plus(&sc, &sb, &sa);
          f_push_string(vm, &sc);
          f_str_dtor(&sc);
        }
        *pip = *pip + 2;
        f_str_dtor(&sb);
        f_str_dtor(&sa);
      } else return E_TYPE;
      break;
    case 24:                                                       /* MOD: 589366 */
      if (ty != 0x20) return E_TYPE;
      if (!pop(vm, T_INT, &a) || !pop(vm, T_INT, &b)) return E_POP;
      if (a == 0) return E_DIV0;
      push(vm, T_INT, (b == INT32_MIN && a == -1) ? 0 : b % a);
      *pip = *pip + 2;
      break;
    case 25:                                                       /* NEG: 589172 */
      if (ty == T_FLOAT) {
        if (!pop(vm, T_FLOAT, &a)) return E_POP;
        push(vm, T_FLOAT, (int32_t)((uint32_t)a ^ 0x80000000u));
      } else if (ty == T_INT) {
        if (!pop(vm, T_INT, &a)) return E_POP;
        push(vm, T_INT, (int32_t)(0u - (uint32_t)a));
      } else return E_TYPE;
      *pip = *pip + 2;
      break;
    case 26:                                                       /* COMP: 589194 */
      if (ty != T_INT) return E_TYPE;
      if (!pop(vm, T_INT, &a)) return E_POP;
      push(vm, T_INT, ~a);
      *pip = *pip + 2;
      break;
    case 27:                                                       /* MOVSP: 5891b0 */
      set_sp(s, slot_of(s->sp, be32(c + 2)));
      *pip = *pip + 6;
      break;
    case 28:                                                       /* STORE_STATEALL: 5891dc */
      VMI(vm, VM_STATE_IP) = ip + ty;
      VMI(vm, VM_STATE_A) = 0;
      VMI(vm, VM_STATE_B) = 0;
      *pip = *pip + 2;
      break;
    case 29:                                                       /* JMP: 5893a2 */
      *pip = ip + be32(c + 2);
      break;
    case 30: {                                                     /* JSR: 5893be */
      int32_t back = ip + 6, cnt = VMI(vm, VM_RET_N);
      *pip = back;
      VMI(vm, VM_RET + 4 * cnt) = back;
      VMI(vm, VM_RET_N) = cnt + 1;
      if (cnt > 126) return E_OVERFLOW;
      *pip = *pip + be32(c + 2) - 6;
      break;
    }
    case 31: case 37:                                              /* JZ, JNZ: 58940c, 58943e */
      if (!pop(vm, T_INT, &a)) return E_POP;
      if ((op == 31) == (a == 0)) *pip = *pip + be32(c + 2);
      else *pip = *pip + 6;
      break;
    case 32: {                                                     /* RETN: PopInstructionPtr */
      int32_t cnt = VMI(vm, VM_RET_N);
      VMI(vm, VM_RET_N) = cnt - 1;
      if (cnt <= 0) { VMI(vm, VM_RET_N) = 0; return E_POP; }
      *pip = VMI(vm, VM_RET + 4 * (cnt - 1));
      break;
    }
    case 33: {                                                     /* DESTRUCT: 5891ec */
      int32_t total = be16s(c + 2) >> 2, keepoff = be16s(c + 4) >> 2, keepsz = be16s(c + 6) >> 2;
      int32_t keepend = keepsz + keepoff, top = s->sp;
      if (keepend < total) {
        top = top - total + keepend;
        set_sp(s, top);
      }
      for (int32_t i = top - keepend; i < top - keepoff; i++) assign(s, keepoff + i, i);
      if (be16s(c + 4) > 3) set_sp(s, s->sp - keepoff);
      *pip = *pip + 8;
      break;
    }
    case 34:                                                       /* NOTI: 58924c */
      if (ty != T_INT) return E_TYPE;
      if (!pop(vm, T_INT, &a)) return E_POP;
      push(vm, T_INT, a == 0);
      *pip = *pip + 2;
      break;
    case 35: case 36: case 40: case 41: {                         /* DECISP, INCISP, DECIBP, INCIBP: 588df6 */
      if (ty == T_INT) {
        int32_t at = slot_of(op <= 36 ? s->sp : s->bp, be32(c + 2));
        if (at < 0) return E_POP;
        if (s->owner && s->types[at] == T_INT)                     /* ModifyIntegerAtLocation */
          s->values[at] += (op == 36 || op == 41) ? 1 : -1;
      }
      *pip = *pip + 6;
      break;
    }
    case 42: {                                                     /* SAVEBP: 58947a */
      int32_t was_sp = s->sp;
      push(vm, T_INT, s->bp);
      s->bp = was_sp;
      *pip = *pip + 2;
      break;
    }
    case 43:                                                       /* RESTOREBP: 58948c */
      if (!pop(vm, T_INT, &a)) return E_POP;
      s->bp = a;
      *pip = *pip + 2;
      break;
    case 44:                                                       /* STORE_STATE: 58926c */
      VMI(vm, VM_STATE_IP) = ip + ty;
      VMI(vm, VM_STATE_A) = be32(c + 6);
      VMI(vm, VM_STATE_B) = be32(c + 2);
      *pip = *pip + 10;
      break;
    default:
      return E_OPCODE;
    }
  }
}

/* ---- the hook --------------------------------------------------------------------- */
static int h_exec(void *vm, int *pip, char *code, int size) {
  if (!s_on) return o_exec(vm, pip, code, size);
  return nw_exec(vm, pip, code, size);
}

int nwvm_run(void *vm, int *pip, char *code, int size) { return nw_exec(vm, pip, code, size); }
int nwvm_run_engine(void *vm, int *pip, char *code, int size) { return o_exec(vm, pip, code, size); }

static uintptr_t take_slots(const char *sym, uintptr_t repl) {
  uintptr_t original = so_symbol(&kotor_mod, sym), callable = 0;
  unsigned n = 0, bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *sy = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + sy->st_name, sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0 && (!n || bad)) return 0;
  }
  return callable;
}

#define SYM(var, name) (*(void **)&(var) = (void *)so_symbol(&kotor_mod, name))

int nwvm_resolve(void) {
  SYM(f_add_top, "_ZN20CVirtualMachineStack15AddToTopOfStackEi");
  SYM(f_set_sp, "_ZN20CVirtualMachineStack15SetStackPointerEi");
  SYM(f_assign, "_ZN20CVirtualMachineStack24AssignLocationToLocationEii");
  SYM(f_push_string, "_ZN23CVirtualMachineInternal15StackPushStringERK10CExoString");
  SYM(f_pop_string, "_ZN23CVirtualMachineInternal14StackPopStringEP10CExoString");
  SYM(f_push_struct, "_ZN23CVirtualMachineInternal24StackPushEngineStructureEiPv");
  SYM(f_str_ctor, "_ZN10CExoStringC1Ev");
  SYM(f_str_ctor_c, "_ZN10CExoStringC1EPKc");
  SYM(f_str_dtor, "_ZN10CExoStringD1Ev");
  SYM(f_str_plus, "_ZNK10CExoStringplERKS_");
  SYM(f_str_cmpnc, "_ZNK10CExoString13CompareNoCaseERKS_");
  SYM(f_str_format, "_ZN10CExoString6FormatEPcz");
  f_new_array = k2_new_array;
  f_delete_array = k2_delete_array;
  return f_add_top && f_set_sp && f_assign && f_push_string && f_pop_string && f_push_struct && f_str_ctor &&
         f_str_ctor_c && f_str_dtor && f_str_plus && f_str_cmpnc && f_str_format;
}

/* The engine's interpreter as this build has it: the start of its loop and the
 * instruction limit it checks. */
static int engine_matches(uintptr_t fn) {
  static const uint16_t expect[] = {0xb5f0, 0xaf03, 0xe92d, 0x0f00, 0xb097, 0x4605};
  return fn == kotor_mod.text_base + 0x588ce4u + 1 && !memcmp((const void *)(fn - 1), expect, sizeof expect) &&
         *(const uint32_t *)(kotor_mod.text_base + 0x589040u) == (uint32_t)LIMIT;
}

void nwvm_install(void) {
  char m = NWVM_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/nwvm_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char ch = 0; if (sceIoRead(fd, &ch, 1) == 1) m = ch; sceIoClose(fd); }
  if (m == '0') { log_printf("[nwvm] off (nwvm_mode '0')"); return; }
  uintptr_t fn = so_symbol(&kotor_mod, "_ZN23CVirtualMachineInternal11ExecuteCodeEPiPci");
  if (!engine_matches(fn) || !nwvm_resolve()) {
    log_printf("[nwvm] NOT armed: the script interpreter is not the expected code");
    return;
  }
  o_exec = (int (*)(void *, int *, char *, int))take_slots("_ZN23CVirtualMachineInternal11ExecuteCodeEPiPci",
                                                           (uintptr_t)h_exec);
  if (!o_exec) { log_printf("[nwvm] NOT armed: ExecuteCode's slot is taken"); return; }
  s_on = 1;
  log_printf("[nwvm] armed: scripts run in our interpreter%s", m == 'a' ? " (A/B every 10 s)" : "");
#if NWVM_DIALOGUE_SKIP
  {
    char dm = '1';
    SceUID f2 = sceIoOpen(DATA_PATH "/dialogue_mode.txt", SCE_O_RDONLY, 0);
    if (f2 >= 0) { char ch = 0; if (sceIoRead(f2, &ch, 1) == 1) dm = ch; sceIoClose(f2); }
    s_app_mgr = so_symbol(&kotor_mod, "g_pAppManager");
    f_get_obj = (void *(*)(void *, uint32_t))so_symbol(&kotor_mod, "_ZN13CServerExoApp13GetGameObjectEm");
    f_get_bool = (int (*)(void *, int))so_symbol(&kotor_mod, "_ZN11CSWVarTable15GetLocalBooleanEi");
    uintptr_t vt = so_symbol(&kotor_mod, "_ZTV12CSWSCreature");
    s_vt_creature = vt ? vt + 8 : 0;
    int ok = dm != '0' && s_app_mgr && f_get_obj && f_get_bool && s_vt_creature;
#if !BATTLE_LEDGER
    if (ok) {
      o_runscript = (runscript_t)take_slots("_ZN15CVirtualMachine9RunScriptEP10CExoStringmi", (uintptr_t)h_runscript);
      ok = o_runscript != NULL;
    }
#endif
    s_dlg_on = ok;
    log_printf("[nwvm] OnDialogue runs that do nothing: %s", ok ? "skipped" : dm == '0' ? "run (dialogue_mode 0)" : "run (not armed)");
  }
#endif
  if (m == 'a') s_on = 2;
}

static uint32_t s_frames;
#if !NWVM_AB
static uint64_t s_t0;
#endif

static void report(void) {
  uint32_t f = s_frames ? s_frames : 1;
  log_printf("[nwvm] %s%u frames | %u script runs, %u instructions a frame (ours) | global set-up from snapshots: "
             "%u runs, %u instructions a frame not interpreted",
             s_on == 0 ? "OFF " : s_on == 2 ? "ON " : "", (unsigned)s_frames, (unsigned)(w_calls / f),
             (unsigned)(w_insns / f), (unsigned)(w_snap_used / f), (unsigned)(w_snap_insns / f));
  w_calls = w_insns = 0;
  w_snap_used = w_snap_insns = 0;
  if (s_dlg_on)
    log_printf("[nwvm] OnDialogue (k_def_dialogue01): %.1f runs a frame, %.1f skipped as doing nothing",
               (double)w_dlg_runs / f, (double)w_dlg_skipped / f);
  w_dlg_runs = w_dlg_skipped = 0;
  s_frames = 0;
}

/* NWVM_AB: the AI thread ends its 10 s windows here, so each of its lines
 * belongs to one interpreter. */
int nwvm_phase_ours(void) { return o_exec && s_on != 0; }
void nwvm_ab_window_end(void) {
  if (!o_exec) return;
  report();
  s_on = s_on ? 0 : 2;
}

void nwvm_on_swap(void) {
  if (!o_exec) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  s_frames++;
#if !NWVM_AB
  if (!s_t0) { s_t0 = now; return; }
  if (now - s_t0 < 10000000u) return;
  report();
  s_t0 = now;
#else
  (void)now;
#endif
}

#else
void nwvm_install(void) {}
void nwvm_on_swap(void) {}
int nwvm_dialogue_skip(const void *name, uint32_t oid, uintptr_t ret) { (void)name; (void)oid; (void)ret; return 0; }
int nwvm_phase_ours(void) { return 0; }
void nwvm_ab_window_end(void) {}
#endif
