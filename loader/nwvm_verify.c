/* nwvm_verify.c -- NWVM_VERIFY (test builds): generated NWScript programs run
 * through the engine's interpreter and ours from the same state, and every
 * result compared: return code, instruction pointer, the stack (types, values,
 * strings by content), base pointer, return-address stack, instruction count,
 * stored state. Runs once, at a swap where the VM exists and is idle.
 *
 * The generator keeps a model of the stack so that it never asks the engine
 * to copy a string or structure over another kind of slot (the engine frees
 * what the destination held as a string or structure of the source's type),
 * and it emits wrong types, underflows, division by zero and the instruction
 * limit on purpose: their error codes are compared too. No ACTION (engine
 * commands have side effects). */

#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "nwvm.h"

#if NWVM && NWVM_VERIFY

typedef struct { int32_t sp, bp, cap; int8_t *types; int32_t *values; void *owner; } vmstack_t;
typedef struct { char *s; uint32_t len; } exostr_t;
#define STK(vm) ((vmstack_t *)((uint8_t *)(vm) + 0x1ac))
#define VMI(vm, off) (*(int32_t *)((uint8_t *)(vm) + (off)))

static uint32_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

/* ---- program generator ------------------------------------------------------ */
#define MAXP 1400
#define MAXS 96
static uint8_t P[MAXP];
static int pn;
static int8_t st[MAXS];     /* the model: slot types; 0x7f unknown, 0x40 the saved base pointer (an int) */
#define T_SAVED 0x40
static int sp, bp, savebp_at = -1, bp_before;
static int jsr_at[64], njsr;

static void b1(int v) { if (pn < MAXP) P[pn++] = (uint8_t)v; }
static void b2(int v) { b1(v >> 8); b1(v); }
static void b4(int32_t v) { b1(v >> 24); b1(v >> 16); b1(v >> 8); b1(v); }

static int32_t rint_edge(void) {
  static const int32_t e[] = {0, 1, -1, 2, 7, 31, 32, 33, 40, 255, 256, 0x7fffffff, (int32_t)0x80000000, -2, 100, -100};
  return rn(3) ? e[rn(sizeof e / sizeof e[0])] : (int32_t)rnd();
}
static int32_t rfloat_bits(void) {
  static const uint32_t e[] = {0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000,
                               0x00000001, 0x00800000, 0x40490fdb, 0x4b000000, 0x3eaaaaab, 0x447a0000};
  return rn(3) ? (int32_t)e[rn(sizeof e / sizeof e[0])] : (int32_t)rnd();
}
static int owned(int8_t t) { return t == 5 || (t >= 16 && t <= 25); }
static void pushm(int8_t t) { if (sp < MAXS) st[sp++] = t; }
static int topis(int8_t t, int n) { if (sp < n) return 0; for (int i = 1; i <= n; i++) if (st[sp - i] != t) return 0; return 1; }

/* One instruction; returns 0 when the model says execution stops here. */
static int gen_one(void) {
  int k = rn(100);
  if (sp > MAXS - 8) k = 60 + rn(10);                    /* make room */
  if (k < 14) {                                          /* constants */
    int t = 3 + rn(4);
    b1(4); b1(t);
    if (t == 3) b4(rint_edge());
    else if (t == 4) b4(rfloat_bits());
    else if (t == 6) b4(rn(2) ? 0 : (int32_t)rnd());
    else { int len = rn(12); b2(len); for (int i = 0; i < len; i++) b1('a' + rn(26)); }
    pushm(t);
  } else if (k < 18) {                                   /* RSADD */
    int t = rn(8) ? 3 + rn(4) : 16 + rn(4);
    b1(2); b1(t); pushm(t);
  } else if (k < 34) {                                   /* int binary */
    static const uint8_t ops[] = {6, 7, 8, 9, 10, 17, 18, 19, 20, 21, 22, 23, 24, 13, 14, 15, 16, 11, 12};
    int op = ops[rn(sizeof ops)];
    int ok = topis(3, 2);
    if (!ok && rn(20)) { b1(4); b1(3); b4(rint_edge()); pushm(3); return 1; }
    if ((op == 23 || op == 24) && ok && rn(4)) { b1(4); b1(3); b4(rn(2) ? 3 : -7); pushm(3); }  /* mostly not 0 */
    if (rn(60) == 0) { b1(op); b1(0x27 + rn(3)); return 0; }   /* a type byte no handler takes */
    b1(op); b1(0x20);
    if (!ok) return 0;
    sp -= 2; pushm(3);
  } else if (k < 44) {                                   /* float, mixed */
    static const uint8_t ops[] = {20, 21, 22, 23, 13, 14, 15, 16};
    int op = ops[rn(sizeof ops)], ty = 0x21 + (rn(3) == 0 ? 4 : 0) + (rn(3) == 0 ? 5 : 0);
    if (ty == 0x2a) ty = 0x21;
    int ok;
    if (op >= 13 && op <= 16) ty = 0x21;
    if (ty == 0x21) ok = topis(4, 2);
    else if (ty == 0x25) ok = sp >= 2 && st[sp - 1] == 4 && st[sp - 2] == 3;   /* IF: int below, float top */
    else ok = sp >= 2 && st[sp - 1] == 3 && st[sp - 2] == 4;                   /* FI */
    if (!ok && rn(15)) { b1(4); b1(4); b4(rfloat_bits()); pushm(4); return 1; }
    b1(op); b1(ty);
    if (!ok) return 0;
    sp -= 2; pushm(op >= 13 && op <= 16 ? 3 : 4);
  } else if (k < 50) {                                   /* vectors */
    int op = 20 + rn(4), ty = 0x3a + rn(3);
    int ok;
    if (ty == 0x3a) ok = topis(4, 6);
    else ok = topis(4, 4);
    if (!ok && rn(10)) { for (int i = 0; i < 3; i++) { b1(4); b1(4); b4(rfloat_bits()); pushm(4); } return 1; }
    b1(op); b1(ty);
    if (!ok) return 0;
    if (ty == 0x3a) { sp -= 6; if (op == 20 || op == 21) for (int i = 0; i < 3; i++) pushm(4); }
    else { sp -= 4; if (op == 22 || op == 23) for (int i = 0; i < 3; i++) pushm(4); }
  } else if (k < 54) {                                   /* strings */
    int ok = topis(5, 2), op = rn(3) ? 20 : 11 + rn(2);
    if (!ok && rn(10)) { b1(4); b1(5); int len = rn(6); b2(len); for (int i = 0; i < len; i++) b1('A' + rn(4)); pushm(5); return 1; }
    b1(op); b1(0x23);
    if (!ok) return 0;
    sp -= 2; if (op == 20) pushm(5); else pushm(3);
  } else if (k < 58) {                                   /* EQUAL/NEQUAL of a pair, or TT */
    int op = 11 + rn(2);
    int8_t pt = sp >= 2 ? st[sp - 1] : 0;
    if (sp >= 2 && st[sp - 1] == st[sp - 2] && ((pt >= 3 && pt <= 6) || (pt >= 16 && pt <= 25)) && rn(2)) {
      int8_t t = st[sp - 1];
      int ty = t == 3 ? 0x20 : t == 4 ? 0x21 : t == 5 ? 0x23 : t == 6 ? 0x22 : 0x30 + (t - 16);
      b1(op); b1(ty); sp -= 2; pushm(3);
    } else {
      int cnt = 1 + rn(3);
      int ok = sp >= 2 * cnt;
      for (int i = 0; ok && i < cnt; i++) if (st[sp - 1 - i] != st[sp - 1 - i - cnt] || st[sp - 1 - i] == 0x7f) ok = 0;
      if (!ok) { b1(4); b1(3); b4(1); pushm(3); return 1; }
      b1(op); b1(0x24); b2(cnt * 4); sp -= 2 * cnt; pushm(3);
    }
  } else if (k < 62) {                                   /* unary */
    int op = 25 + rn(2), ty = 3;
    if (rn(3) == 0) op = 34;
    if (op == 25 && rn(2)) ty = 4;
    int ok = sp >= 1 && st[sp - 1] == ty;
    if (!ok && rn(15)) { b1(4); b1(ty); b4(ty == 3 ? rint_edge() : rfloat_bits()); pushm(ty); return 1; }
    b1(op); b1(ty);
    if (!ok) return 0;
  } else if (k < 70) {                                   /* MOVSP */
    if (!sp) return 1;
    int n = 1 + rn(sp < 4 ? sp : 4);
    b1(27); b1(0); b4(-4 * n); sp -= n;
  } else if (k < 78) {                                   /* CPTOPSP / CPTOPBP */
    if (!sp) return 1;
    int useb = rn(4) == 0;
    int cnt = 1 + rn(sp < 3 ? sp : 3);
    int loc = rn(sp - cnt + 1);
    int base = useb ? bp : sp;
    if (useb && loc < bp && rn(2)) loc = bp < sp - cnt + 1 ? bp : loc;
    b1(useb ? 39 : 3); b1(1); b4((loc - base) * 4); b2(cnt * 4);
    for (int i = 0; i < cnt; i++) pushm(st[loc + i]);
  } else if (k < 84) {                                   /* CPDOWNSP / CPDOWNBP: same types only */
    if (sp < 2) return 1;
    int cnt = 1 + rn(sp / 2 < 3 ? sp / 2 : 3);
    int dst = rn(sp - cnt + 1);
    int useb = rn(4) == 0;
    int ok = 1;
    for (int i = 0; i < cnt; i++) if (st[dst + i] != st[sp - cnt + i] || st[dst + i] == 0x7f) ok = 0;
    /* A string or structure copied onto itself: the engine frees it, then
     * copies the freed one -- garbage in both, so not a comparison. */
    for (int i = 0; ok && i < cnt; i++) if (dst + i == sp - cnt + i && owned(st[dst + i])) ok = 0;
    if (!ok) return 1;
    int base = useb ? bp : sp;
    b1(useb ? 38 : 1); b1(1); b4((dst - base) * 4); b2(cnt * 4);
  } else if (k < 88) {                                   /* INCISP etc. */
    if (!sp) return 1;
    int op = rn(2) ? 35 + rn(2) : 40 + rn(2);
    int base = op <= 36 ? sp : bp;
    int loc = rn(sp);
    if (st[loc] == T_SAVED) return 1;
    b1(op); b1(rn(8) ? 3 : 4); b4((loc - base) * 4);
  } else if (k < 91) {                                   /* DESTRUCT */
    if (sp < 2) return 1;
    int total = 2 + rn(sp - 1 < 5 ? sp - 1 : 5);
    if (total > sp) total = sp;
    int keepsz = 1 + rn(total - 1), keepoff = 1 + rn(total - keepsz);
    if (keepoff + keepsz > total) return 1;
    int base = sp - total, ok = 1;
    for (int i = 0; i < keepsz; i++) {
      int8_t s_t = st[base + keepoff + i], d_t = st[base + i];
      if ((owned(s_t) || owned(d_t)) && s_t != d_t) ok = 0;
    }
    if (!ok) return 1;
    b1(33); b1(1); b2(total * 4); b2(keepoff * 4); b2(keepsz * 4);
    for (int i = 0; i < keepsz; i++) st[base + i] = st[base + keepoff + i];
    sp = base + keepsz;
  } else if (k < 93) {                                   /* SAVEBP / RESTOREBP */
    if (savebp_at >= 0 && sp == savebp_at + 1 && st[sp - 1] == T_SAVED) {
      b1(43); b1(0); sp--; bp = bp_before; savebp_at = -1;
    } else if (savebp_at < 0) {
      b1(42); b1(0); bp_before = bp; bp = sp; pushm(T_SAVED); savebp_at = sp - 1;
    }
  } else if (k < 96) {                                   /* jumps over a neutral instruction */
    int op = rn(3) == 0 ? 29 : 31 + (rn(2) ? 6 : 0);
    if (op != 29) { if (!(sp >= 1 && st[sp - 1] == 3)) { b1(4); b1(3); b4(rn(2)); pushm(3); return 1; } sp--; }
    b1(op); b1(0); b4(rn(2) ? 6 : 12);
    if (sp && st[sp - 1] != T_SAVED) { b1(36); b1(3); b4(-4); } else { b1(28); b1(4); b1(28); b1(4); b1(28); b1(4); }
  } else if (k < 98) {                                   /* stored state */
    if (rn(2)) { b1(28); b1(rn(16)); }
    else { b1(44); b1(rn(16)); b4((int32_t)rnd()); b4((int32_t)rnd()); }
  } else {                                               /* JSR to the subroutine (patched later) */
    if (njsr < 64) jsr_at[njsr++] = pn;
    b1(30); b1(0); b4(0);
  }
  return 1;
}

static int gen_program(uint32_t seed) {
  rng = seed * 2654435761u + 0x9e3779b9u;
  if (!rng) rng = 1;
  pn = 0; sp = 0; bp = 0; savebp_at = -1; njsr = 0;
  int n = 8 + rn(50);
  for (int i = 0; i < n && pn < MAXP - 40; i++) {
    if (!gen_one()) break;
    if (savebp_at >= 0 && (savebp_at >= sp || st[savebp_at] != T_SAVED)) savebp_at = -1;
  }
  b1(32); b1(0);                                         /* RETN: pops the -1 */
  int sub = pn;                                          /* the subroutine: push, drop, return */
  b1(4); b1(3); b4(42); b1(27); b1(0); b4(-4); b1(32); b1(0);
  for (int j = 0; j < njsr; j++) {                       /* JSRs -> the subroutine */
    int i = jsr_at[j];
    int32_t off = sub - i;
    P[i + 2] = off >> 24; P[i + 3] = off >> 16; P[i + 4] = off >> 8; P[i + 5] = off;
  }
  return pn;
}

/* ---- running and comparing ---------------------------------------------------- */
typedef struct {
  int r, ip, sp, bp, retn, count, sip, sa, sb, nslots;
  int32_t ret[8];
  int8_t t[MAXS + 32];
  int32_t v[MAXS + 32];
  char str[MAXS + 32][24];
  uint32_t slen[MAXS + 32];
} snap_t;
static snap_t SA, SB;
static void (*e_set_sp)(vmstack_t *, int);

static void run_one(void *vm, int ours, int start_count, snap_t *o) {
  vmstack_t *s = STK(vm);
  s->bp = 0;
  int n = VMI(vm, 0x1c4);
  VMI(vm, 0x1c8 + 4 * n) = -1;                           /* as RunScriptFile */
  VMI(vm, 0x1c4) = n + 1;
  VMI(vm, 0x24) = start_count;
  int ip = 0;
  o->r = ours ? nwvm_run(vm, &ip, (char *)P, pn) : nwvm_run_engine(vm, &ip, (char *)P, pn);
  o->ip = ip; o->sp = s->sp; o->bp = s->bp; o->retn = VMI(vm, 0x1c4); o->count = VMI(vm, 0x24);
  o->sip = VMI(vm, 0x3c8); o->sa = VMI(vm, 0x3cc); o->sb = VMI(vm, 0x3d0);
  for (int i = 0; i < 8; i++) o->ret[i] = i < o->retn ? VMI(vm, 0x1c8 + 4 * i) : 0;
  o->nslots = s->sp < MAXS + 32 ? s->sp : MAXS + 32;
  for (int i = 0; i < o->nslots; i++) {
    int8_t t = s->types[i];
    o->t[i] = t;
    o->v[i] = (t == 5 || (t >= 16 && t <= 25)) ? 0 : s->values[i];
    o->str[i][0] = 0; o->slen[i] = 0;
    if (t == 5 && s->values[i]) {
      exostr_t *e = (exostr_t *)s->values[i];
      o->slen[i] = e->len;
      if (e->s) { strncpy(o->str[i], e->s, 23); o->str[i][23] = 0; }
    }
  }
  if (s->sp > 0) e_set_sp(s, 0);                         /* frees strings and structures */
  s->sp = 0;
  s->bp = 0;
  VMI(vm, 0x1c4) = 0;
}

static int same(const snap_t *a, const snap_t *b, char *why, size_t cap) {
#define CHK(f) if (a->f != b->f) { snprintf(why, cap, #f " %d/%d", a->f, b->f); return 0; }
  CHK(r) CHK(ip) CHK(sp) CHK(bp) CHK(retn) CHK(count) CHK(sip) CHK(sa) CHK(sb)
  for (int i = 0; i < a->retn && i < 8; i++) if (a->ret[i] != b->ret[i]) { snprintf(why, cap, "ret[%d]", i); return 0; }
  for (int i = 0; i < a->nslots; i++) {
    if (a->t[i] != b->t[i]) { snprintf(why, cap, "type[%d] %d/%d", i, a->t[i], b->t[i]); return 0; }
    if (a->v[i] != b->v[i]) { snprintf(why, cap, "value[%d] %08x/%08x (type %d)", i, (unsigned)a->v[i], (unsigned)b->v[i], a->t[i]); return 0; }
    if (a->slen[i] != b->slen[i] || strcmp(a->str[i], b->str[i])) { snprintf(why, cap, "string[%d] \"%s\"/\"%s\"", i, a->str[i], b->str[i]); return 0; }
  }
  return 1;
#undef CHK
}

void nwvm_verify_tick(void) {
  static int done, frames;
  if (done || ++frames < 240) return;
  uintptr_t g = so_symbol(&kotor_mod, "g_pVirtualMachine");
  void *cvm = g ? *(void **)g : NULL;
  void *vm = cvm ? *(void **)((uint8_t *)cvm + 28) : NULL;
  if (!vm || !*(void **)((uint8_t *)vm + 0x3d4)) return;
  vmstack_t *s = STK(vm);
  if (s->sp != 0 || VMI(vm, 0x1c4) != 0 || !s->owner) return;
  done = 1;
  if (!nwvm_resolve()) { log_printf("[nwvm] verify: engine functions not found"); return; }
  *(void **)&e_set_sp = (void *)so_symbol(&kotor_mod, "_ZN20CVirtualMachineStack15SetStackPointerEi");
  int keep_bp = s->bp, keep_count = VMI(vm, 0x24), keep_sip = VMI(vm, 0x3c8), keep_sa = VMI(vm, 0x3cc),
      keep_sb = VMI(vm, 0x3d0);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  unsigned progs = 0, bad = 0, errors = 0, insns = 0;
  unsigned by_r[8] = {0};
  for (uint32_t seed = 1; seed <= NWVM_VERIFY_PROGRAMS; seed++) {
    gen_program(seed);
    int start = (seed % 97 == 0) ? 131060 : (int)(seed % 5);
    run_one(vm, 0, start, &SA);
    run_one(vm, 1, start, &SB);
    progs++;
    insns += (unsigned)(SA.count - start);
    if (SA.r) errors++;
    by_r[SA.r == 0 ? 0 : SA.r == -100 ? 1 : SA.r == -97 ? 2 : SA.r == -105 ? 3 : SA.r == -93 ? 4 : 5]++;
    char why[96];
    if (!same(&SA, &SB, why, sizeof why)) {
      if (bad++ < 12) {
        char hex[200];
        int o = 0;
        for (int i = 0; i < pn && o < 190; i++) o += snprintf(hex + o, sizeof hex - (size_t)o, "%02x", P[i]);
        log_printf("[nwvm] verify MISMATCH seed %u: %s | engine r=%d ip=%d sp=%d, ours r=%d ip=%d sp=%d | %s",
                   (unsigned)seed, why, SA.r, SA.ip, SA.sp, SB.r, SB.ip, SB.sp, hex);
      }
    }
  }
  s->bp = keep_bp;
  VMI(vm, 0x24) = keep_count; VMI(vm, 0x3c8) = keep_sip; VMI(vm, 0x3cc) = keep_sa; VMI(vm, 0x3d0) = keep_sb;
  log_printf("[nwvm] verify: %u programs, %u instructions, %u ended in an error (ok %u, pop %u, type %u, div0 %u, "
             "limit %u, other %u); %u differ; %.1f s",
             progs, insns, errors, by_r[0], by_r[1], by_r[2], by_r[3], by_r[4], by_r[5], bad,
             (double)(sceKernelGetProcessTimeWide() - t0) / 1e6);
}

#else
void nwvm_verify_tick(void) {}
#endif
