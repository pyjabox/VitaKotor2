/* leaf_fast.c -- faster versions of small functions the engine calls a lot
 * (LEAF_FAST).
 *
 * slerp(Quaternion, Quaternion const&, float) (libkotor2 0x5766c8): the
 * animation's key interpolation. The engine's calls acosf once and sinf three
 * times -- newlib's, through the softfp wrappers in dynlib.c -- and divides
 * twice; 1.4-1.6% of the game thread in the Telos and Khoonda profiles. Ours
 * does the same arithmetic in the same order with polynomials: acos from
 * Abramowitz & Stegun 4.4.46 (|error| <= 2e-8 on [0, 1]), sin from its Taylor
 * series to x^11 (|error| < 6e-8 on [0, pi/2], all the angles slerp sees:
 * the dot product is made non-negative, so the angle is at most pi/2, and t
 * outside [0, 1] goes to the engine's own), one division.
 *
 * _stricmp (0x5b5d78): calls tolower through the PLT for each character of
 * both strings. Ours folds ASCII letters inline, compares as the engine does
 * (signed chars, -1/0/1), and leaves NULL arguments and non-ASCII characters
 * to the engine's own and newlib's tolower.
 *
 * tolower, toupper, fminf, fmaxf: the engine's imports, answered here instead
 * of by newlib (dynlib.c): ASCII inline, the rest to newlib; fminf/fmaxf as
 * newlib's (NaN rules included), without the call.
 *
 * ux0:data/kotor2/leaf_mode.txt: 0 off (engine and newlib versions), 1 on,
 * a on and off alternately every 10 s. Without the file: LEAF_FAST_DEFAULT.
 * LEAF_FAST_BENCH: at install, both versions timed on the same inputs, and
 * the largest difference. A [leaf] line every 10 s. */

#include <vitasdk.h>
#include <kubridge.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "leaf_fast.h"

#if LEAF_FAST

static int s_on, s_ab, s_installed;
static uint32_t w_slerp, w_stricmp;
static uint32_t s_ns_slerp[2], s_ns_stricmp[2];   /* bench: engine, ours */

/* ---- imports (dynlib.c points the engine's slots here) ---- */

int leaf_tolower(int c) {
  if (s_on && (unsigned)c < 128u) return (unsigned)(c - 'A') < 26u ? c + 32 : c;
  return tolower(c);
}
int leaf_toupper(int c) {
  if (s_on && (unsigned)c < 128u) return (unsigned)(c - 'a') < 26u ? c - 32 : c;
  return toupper(c);
}
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
/* softfp in and out, as dynlib.c's SFP_ wrappers. */
uint32_t leaf_fminf(uint32_t a, uint32_t b) {
  float x = u2f(a), y = u2f(b);
  if (!s_on) return f2u(fminf(x, y));
  if (x != x) return b;
  if (y != y) return a;
  return x < y ? a : b;
}
uint32_t leaf_fmaxf(uint32_t a, uint32_t b) {
  float x = u2f(a), y = u2f(b);
  if (!s_on) return f2u(fmaxf(x, y));
  if (x != x) return b;
  if (y != y) return a;
  return x > y ? a : b;
}

/* ---- slots ---- */

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

/* ---- slerp ---- */

typedef struct { float x, y, z, w; } quat_t;
typedef quat_t (__attribute__((pcs("aapcs"))) *slerp_t)(quat_t a, const quat_t *b, float t);
static slerp_t o_slerp;

static inline float poly_acos(float c) {          /* 0 <= c < 1 */
  float p = -0.0012624911f;
  p = p * c + 0.0066700901f;
  p = p * c - 0.0170881256f;
  p = p * c + 0.0308918810f;
  p = p * c - 0.0501743046f;
  p = p * c + 0.0889789874f;
  p = p * c - 0.2145988016f;
  p = p * c + 1.5707963050f;
  return __builtin_sqrtf(1.0f - c) * p;
}
static inline float poly_sin(float x) {           /* 0 <= x <= pi/2 */
  float x2 = x * x;
  float p = -2.5052108e-8f;                       /* -1/11! */
  p = p * x2 + 2.7557319e-6f;                     /*  1/9!  */
  p = p * x2 - 1.9841270e-4f;                     /* -1/7!  */
  p = p * x2 + 8.3333333e-3f;                     /*  1/5!  */
  p = p * x2 - 1.6666667e-1f;                     /* -1/3!  */
  return x + x * x2 * p;
}

__attribute__((pcs("aapcs"))) static quat_t fast_slerp(quat_t a, const quat_t *b, float t) {
  float c = b->y * a.y;                           /* the engine's order */
  c += b->x * a.x;
  c += b->z * a.z;
  c += b->w * a.w;
  if (c < 0.0f) { a.x = -a.x; a.y = -a.y; a.z = -a.z; a.w = -a.w; c = -c; }
  if (!(c < 1.0f)) return a;
  float om = poly_acos(c);
  if (om == 0.0f) return a;
  float ot = om * t;
  float r = 1.0f / poly_sin(om);
  float s0 = poly_sin(om - ot) * r, s1 = poly_sin(ot) * r;
  quat_t q = { s0 * a.x + s1 * b->x, s0 * a.y + s1 * b->y, s0 * a.z + s1 * b->z, s0 * a.w + s1 * b->w };
  return q;
}

__attribute__((pcs("aapcs"))) static quat_t h_slerp(quat_t a, const quat_t *b, float t) {
  if (!s_on || !(t >= 0.0f && t <= 1.0f)) return o_slerp(a, b, t);
  w_slerp++;
  return fast_slerp(a, b, t);
}

/* ---- _stricmp ---- */

typedef int (*stricmp_t)(const char *a, const char *b);
static stricmp_t o_stricmp;

static int fast_stricmp(const char *a, const char *b) {
  const signed char *p = (const signed char *)a, *q = (const signed char *)b;
  for (;;) {
    int x = *p++, y = *q++;
    if ((unsigned)(x - 'A') < 26u) x += 32; else if (x < 0) x = (signed char)tolower(x);
    if ((unsigned)(y - 'A') < 26u) y += 32; else if (y < 0) y = (signed char)tolower(y);
    if (x < y) return -1;
    if (x > y) return 1;
    if (!x) return 0;
  }
}

#if LEAF_FAST_TRACE
/* Test builds: who calls _stricmp, by return address; the top ones every 10 s. */
#define TR_SLOTS 512
static uint32_t tr_addr[TR_SLOTS], tr_n[TR_SLOTS], tr_lost;
static void trace_caller(uint32_t ra) {
  uint32_t h = (ra * 2654435761u) >> 23;
  for (int i = 0; i < 16; i++, h = (h + 1) & (TR_SLOTS - 1)) {
    if (tr_addr[h] == ra) { tr_n[h]++; return; }
    if (!tr_addr[h]) { tr_addr[h] = ra; tr_n[h] = 1; return; }
  }
  tr_lost++;
}
static void trace_log(void) {
  char buf[400];
  int pos = 0;
  uint32_t total = tr_lost;
  for (int i = 0; i < TR_SLOTS; i++) total += tr_n[i];
  for (int k = 0; k < 10 && pos < (int)sizeof buf - 40; k++) {
    int best = -1;
    for (int i = 0; i < TR_SLOTS; i++) if (tr_n[i] && (best < 0 || tr_n[i] > tr_n[best])) best = i;
    if (best < 0) break;
    uint32_t off = tr_addr[best] - kotor_mod.text_base;
    pos += snprintf(buf + pos, sizeof buf - pos, " %x:%u", (unsigned)(off & ~1u), (unsigned)tr_n[best]);
    tr_n[best] = 0;
  }
  log_printf("[leaf] _stricmp callers (libkotor2 return addresses: calls), %u calls:%s", (unsigned)total, buf);
  memset(tr_addr, 0, sizeof tr_addr);
  memset(tr_n, 0, sizeof tr_n);
  tr_lost = 0;
}
#endif

static int h_stricmp(const char *a, const char *b) {
#if LEAF_FAST_TRACE
  trace_caller((uint32_t)__builtin_return_address(0));
#endif
  if (!s_on || !a || !b) return o_stricmp(a, b);
  w_stricmp++;
  return fast_stricmp(a, b);
}

/* ---- bench ---- */

#if LEAF_FAST_BENCH
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static float frand(uint32_t *s) { return (float)(lcg(s) & 0xffff) / 65535.0f; }

static void bench(void) {
  enum { N = 2048 };
  static quat_t qa[N], qb[N];
  static float tt[N];
  uint32_t seed = 12345;
  for (int i = 0; i < N; i++) {
    quat_t a = { frand(&seed) - 0.5f, frand(&seed) - 0.5f, frand(&seed) - 0.5f, frand(&seed) - 0.5f };
    float n = 1.0f / sqrtf(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    a.x *= n; a.y *= n; a.z *= n; a.w *= n;
    /* b near a for most (animation keys), far for some */
    float k = (i & 7) ? 0.05f * frand(&seed) : 1.0f;
    quat_t b = { a.x + k * (frand(&seed) - 0.5f), a.y + k * (frand(&seed) - 0.5f), a.z + k * (frand(&seed) - 0.5f),
                 a.w + k * (frand(&seed) - 0.5f) };
    n = 1.0f / sqrtf(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
    b.x *= n; b.y *= n; b.z *= n; b.w *= n;
    qa[i] = a; qb[i] = b; tt[i] = frand(&seed);
  }
  float worst = 0;
  for (int i = 0; i < N; i++) {
    quat_t e = o_slerp(qa[i], &qb[i], tt[i]), f = fast_slerp(qa[i], &qb[i], tt[i]);
    float d[4] = { fabsf(e.x - f.x), fabsf(e.y - f.y), fabsf(e.z - f.z), fabsf(e.w - f.w) };
    for (int j = 0; j < 4; j++) if (d[j] > worst) worst = d[j];
  }
  volatile float sink = 0;
  for (int k = 0; k < 2; k++) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (int r = 0; r < 8; r++)
      for (int i = 0; i < N; i++) { quat_t q = k ? fast_slerp(qa[i], &qb[i], tt[i]) : o_slerp(qa[i], &qb[i], tt[i]); sink += q.w; }
    s_ns_slerp[k] = (uint32_t)((sceKernelGetProcessTimeWide() - t0) * 1000u / (8u * N));
  }
  log_printf("[leaf] slerp: engine %u ns a call, ours %u ns; largest difference %.2e over %u cases",
             (unsigned)s_ns_slerp[0], (unsigned)s_ns_slerp[1], (double)worst, (unsigned)N);

  static const char *const names[] = { "PLC_Chair01", "plc_chair01", "N_CommonerM", "n_commonerm01", "c_drdassassin",
                                       "C_DrdAssassin", "w_blstrpstl001", "W_BlstrPstl002", "", "a", "dan14_militia",
                                       "DAN14_MILITIA", "mod_hostile", "MOD_FRIENDLY", "g_i_frarmbnds01", "Speeder" };
  enum { M = sizeof names / sizeof names[0] };
  int mism = 0;
  int was = s_on;
  s_on = 0;                       /* the engine's _stricmp with newlib's tolower */
  for (int i = 0; i < M; i++)
    for (int j = 0; j < M; j++) if (o_stricmp(names[i], names[j]) != fast_stricmp(names[i], names[j])) mism++;
  volatile int isink = 0;
  for (int k = 0; k < 2; k++) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (int r = 0; r < 64; r++)
      for (int i = 0; i < M; i++)
        for (int j = 0; j < M; j++) isink += k ? fast_stricmp(names[i], names[j]) : o_stricmp(names[i], names[j]);
    s_ns_stricmp[k] = (uint32_t)((sceKernelGetProcessTimeWide() - t0) * 1000u / (64u * M * M));
  }
  s_on = was;
  (void)sink; (void)isink;
  log_printf("[leaf] _stricmp: engine %u ns a call, ours %u ns; %d of %u answers differ", (unsigned)s_ns_stricmp[0],
             (unsigned)s_ns_stricmp[1], mism, (unsigned)(M * M));
}
#endif

void leaf_fast_on_swap(void) {
  if (!s_installed) return;
  static uint64_t t0;
  static uint32_t frames;
  uint64_t now = sceKernelGetProcessTimeWide();
  frames++;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  double saved = 0;
  if (frames && s_ns_slerp[0] > s_ns_slerp[1])
    saved += (double)w_slerp / frames * (s_ns_slerp[0] - s_ns_slerp[1]) / 1e6;
  if (frames && s_ns_stricmp[0] > s_ns_stricmp[1])
    saved += (double)w_stricmp / frames * (s_ns_stricmp[0] - s_ns_stricmp[1]) / 1e6;
  log_printf("[leaf] %s%u frames | a frame: slerp %u, _stricmp %u (fast path) | ~%.2f ms a frame saved at the bench's rates",
             s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)frames, frames ? (unsigned)(w_slerp / frames) : 0,
             frames ? (unsigned)(w_stricmp / frames) : 0, saved);
#if LEAF_FAST_TRACE
  trace_log();
#endif
  w_slerp = w_stricmp = 0;
  frames = 0;
  t0 = now;
  if (s_ab) s_on = !s_on;
}

void leaf_fast_install(void) {
  char m = LEAF_FAST_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/leaf_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char b = 0; if (sceIoRead(fd, &b, 1) == 1) m = b; sceIoClose(fd); }
  if (m != '1' && m != 'a') { log_printf("[leaf] off (leaf_mode '%c')", m); return; }
  unsigned n_slerp = 0, n_stricmp = 0;
  o_slerp = (slerp_t)take_slots("_Z5slerp10QuaternionRKS_f", (uintptr_t)h_slerp, &n_slerp);
  o_stricmp = (stricmp_t)take_slots("_stricmp", (uintptr_t)h_stricmp, &n_stricmp);
  s_ab = m == 'a';
  s_on = 1;
  s_installed = 1;
#if LEAF_FAST_BENCH
  if (o_slerp && o_stricmp) bench();
#endif
  log_printf("[leaf] armed: slerp %u slots, _stricmp %u slots, tolower/toupper/fminf/fmaxf imports%s", n_slerp,
             n_stricmp, s_ab ? "; A/B: on and off alternately every 10 s" : "");
}

#else
void leaf_fast_install(void) {}
void leaf_fast_on_swap(void) {}
#endif
