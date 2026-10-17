/* asl_probe.c -- measure the engine's calls into ASLgl (test builds, ASL_PROBE).
 *
 * Phase 1 of moving ASLgl, gles2-bc and vitaGL onto the GL worker: the game
 * thread would only record the engine's calls into ASLgl, and the worker would
 * run them. Reverse engineering (libkotor2 2.0.4) settled the boundary --
 * ASLgl's per-draw shader setup (GlesKotorSetupShader, KotorProgram) reads
 * gles2-bc's matrix stacks, so the two cannot be split -- but not its cost.
 * What a recorder must carry, per 10 s window ([aslprobe] lines):
 *  - every call into an ASLgl function from outside the GL layers (ASLgl,
 *    gles2-bc, KotorProgram, ProgramType), per function, per frame: the volume
 *    to record; calls from inside the layers are counted apart;
 *  - the queries among them (glGetIntegerv, glGetFloatv, glGetBooleanv,
 *    glIsEnabled) with their pname: each would make the game thread wait for
 *    the worker unless answered from a copy;
 *  - draws by where their data lives: vertex arrays and indices in client
 *    memory have to be copied when the draw is recorded;
 *  - the time ASLgl::glDrawElements takes per draw, and GlesKotorSetupShader
 *    within it: the work that would leave the game thread;
 *  - a lean recorder's cost per call on this CPU (a micro-benchmark).
 * Every ASLgl function slot is taken over (the GL worker's ai_thread-style
 * stubs: one per slot, 8 bytes of ARM code, then asl_probe_enter); the draw
 * entry points and GlesKotorSetupShader get C wrappers that time them. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "asl_probe.h"

#if ASL_PROBE

#define NSTUBS 512
#define NFN 320
#define NIN 2048
#define NQ 48
#define WINDOW_US 10000000u

enum { K_NONE, K_GETIV, K_GETFV, K_GETBV, K_ISEN, K_BIND, K_VPTR, K_CPTR, K_TPTR, K_NPTR, K_SPTR,
       K_APTR, K_ECS, K_DCS, K_EVA, K_DVA, K_CAT };

extern char asl_probe_stubs[];
uintptr_t asl_probe_enter(unsigned stub, uintptr_t ret, uint32_t *args);

static uintptr_t s_next[NSTUBS];
static uint16_t s_fn[NSTUBS];
static char s_name[NFN][40];
static uint8_t s_kind[NFN];
static int s_nfn, s_installed;
static struct { uintptr_t lo, hi; } s_in[NIN];   /* code of ASLgl, gles2-bc, KotorProgram, ProgramType */
static int s_nin;

static uint32_t w_calls[NFN], w_inner[NFN];
static struct { int fn; uint32_t pname, n; } w_q[NQ];
static uint32_t w_frames, w_draw_e, w_draw_a, w_cv_draws, w_ci_draws, w_cv_vbo_idx, w_ci_vbo_vtx;
static uint64_t w_cv_bytes, w_ci_bytes, w_de_us, w_da_us, w_setup_us, w_t0;
static uint32_t w_de_n, w_da_n, w_setup_n;

/* What the engine has bound and enabled, as seen at the boundary. */
static uint32_t sh_array_buf, sh_elem_buf, sh_client_tex;
typedef struct { uint8_t on; uint32_t buf, size, type, stride; } arr_t;
static arr_t sh_arr[24];                    /* 0 vertex, 1 color, 2 normal, 3 secondary, 4-11 texcoord, 12-23 attribs */

static void (*o_draw_e)(uint32_t, int, uint32_t, const void *);
static void (*o_draw_a)(uint32_t, int, int);
static void (*o_setup)(void);

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static int inside(uintptr_t a) {
  int lo = 0, hi = s_nin - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (a < s_in[m].lo) hi = m - 1;
    else if (a >= s_in[m].hi) lo = m + 1;
    else return 1;
  }
  return 0;
}

static unsigned type_size(uint32_t t) {
  switch (t) {
    case 0x1400: case 0x1401: return 1;     /* BYTE, UNSIGNED_BYTE */
    case 0x1402: case 0x1403: return 2;     /* SHORT, UNSIGNED_SHORT */
    case 0x140B: return 2;                  /* HALF_FLOAT */
    default: return 4;
  }
}

static arr_t *cap_array(uint32_t cap) {
  switch (cap) {
    case 0x8074: return &sh_arr[0];         /* VERTEX_ARRAY */
    case 0x8076: return &sh_arr[1];         /* COLOR_ARRAY */
    case 0x8075: return &sh_arr[2];         /* NORMAL_ARRAY */
    case 0x845E: return &sh_arr[3];         /* SECONDARY_COLOR_ARRAY */
    case 0x8078: return &sh_arr[4 + (sh_client_tex & 7)];   /* TEXTURE_COORD_ARRAY */
    default: return NULL;
  }
}

static void set_ptr(arr_t *a, uint32_t size, uint32_t type, uint32_t stride) {
  a->buf = sh_array_buf;
  a->size = size;
  a->type = type;
  a->stride = stride;
}

static void note_query(int fn, uint32_t pname) {
  for (int i = 0; i < NQ; i++) {
    if (w_q[i].n && w_q[i].fn == fn && w_q[i].pname == pname) { w_q[i].n++; return; }
    if (!w_q[i].n) { w_q[i].fn = fn; w_q[i].pname = pname; w_q[i].n = 1; return; }
  }
}

uintptr_t asl_probe_enter(unsigned stub, uintptr_t ret, uint32_t *args) {
  int fn = s_fn[stub];
  if (inside(ret & ~(uintptr_t)1)) {
    w_inner[fn]++;
    return s_next[stub];
  }
  w_calls[fn]++;
  switch (s_kind[fn]) {
    case K_GETIV: case K_GETFV: case K_GETBV: case K_ISEN: note_query(fn, args[0]); break;
    case K_BIND:
      if (args[0] == 0x8892) sh_array_buf = args[1];
      else if (args[0] == 0x8893) sh_elem_buf = args[1];
      break;
    case K_VPTR: set_ptr(&sh_arr[0], args[0], args[1], args[2]); break;
    case K_CPTR: set_ptr(&sh_arr[1], args[0], args[1], args[2]); break;
    case K_NPTR: set_ptr(&sh_arr[2], 3, args[0], args[1]); break;
    case K_SPTR: set_ptr(&sh_arr[3], args[0], args[1], args[2]); break;
    case K_TPTR: set_ptr(&sh_arr[4 + (sh_client_tex & 7)], args[0], args[1], args[2]); break;
    case K_APTR: if (args[0] < 12) set_ptr(&sh_arr[12 + args[0]], args[1], args[2], args[6]); break;
    case K_ECS: case K_DCS: { arr_t *a = cap_array(args[0]); if (a) a->on = s_kind[fn] == K_ECS; } break;
    case K_EVA: case K_DVA: if (args[0] < 12) sh_arr[12 + args[0]].on = s_kind[fn] == K_EVA; break;
    case K_CAT: sh_client_tex = args[0] - 0x84C0; break;   /* GL_TEXTURE0 */
    default: break;
  }
  return s_next[stub];
}

/* Bytes of client-memory vertex data a draw of vertices [0, n) reads. */
static uint64_t client_vertex_bytes(uint32_t n, int *any) {
  uint64_t b = 0;
  *any = 0;
  for (int i = 0; i < 24; i++) {
    const arr_t *a = &sh_arr[i];
    if (!a->on || a->buf) continue;
    *any = 1;
    b += (uint64_t)n * (a->stride ? a->stride : a->size * type_size(a->type));
  }
  return b;
}

static void draw_e_hook(uint32_t mode, int count, uint32_t type, const void *idx) {
  int boundary = !inside((uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1);
  if (boundary) {
    w_draw_e++;
    int any;
    if (!sh_elem_buf && idx && count > 0) {                 /* client indices: scan for the range */
      uint32_t mx = 0;
      for (int i = 0; i < count; i++) {
        uint32_t v = type == 0x1403 ? ((const uint16_t *)idx)[i] :
                     type == 0x1405 ? ((const uint32_t *)idx)[i] : ((const uint8_t *)idx)[i];
        if (v > mx) mx = v;
      }
      w_ci_draws++;
      w_ci_bytes += (uint64_t)count * type_size(type);
      uint64_t vb = client_vertex_bytes(mx + 1, &any);
      if (any) { w_cv_draws++; w_cv_bytes += vb; }
      else w_ci_vbo_vtx++;
    } else {
      (void)client_vertex_bytes(0, &any);
      if (any) w_cv_vbo_idx++;
    }
  }
  uint64_t t0 = now_us();
  o_draw_e(mode, count, type, idx);
  if (boundary) { w_de_us += now_us() - t0; w_de_n++; }
}

static void draw_a_hook(uint32_t mode, int first, int count) {
  int boundary = !inside((uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1);
  if (boundary) {
    w_draw_a++;
    int any;
    uint64_t vb = client_vertex_bytes((uint32_t)(first + count), &any);
    if (any) { w_cv_draws++; w_cv_bytes += vb; }
  }
  uint64_t t0 = now_us();
  o_draw_a(mode, first, count);
  if (boundary) { w_da_us += now_us() - t0; w_da_n++; }
}

static void setup_hook(void) {
  uint64_t t0 = now_us();
  o_setup();
  w_setup_us += now_us() - t0;
  w_setup_n++;
}

/* A lean recorder's cost: a 6-word command through a function pointer into a
 * ring, published every 32 commands -- what each recorded call would add. */
static uint32_t s_bench_ring[64 * 1024];
static uint32_t *volatile s_bench_w;
static volatile uint32_t s_bench_pub;
static void __attribute__((noinline)) bench_record(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  uint32_t *w = s_bench_w;
  if (w > s_bench_ring + 64 * 1024 - 8) w = s_bench_ring;
  w[0] = op; w[1] = a; w[2] = b; w[3] = c; w[4] = d; w[5] = 0;
  w += 6;
  s_bench_w = w;
  if (!(op & 31)) { __sync_synchronize(); s_bench_pub = (uint32_t)(w - s_bench_ring); }
}
static void (*volatile s_bench_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = bench_record;
static unsigned bench_ns(void) {
  s_bench_w = s_bench_ring;
  uint64_t t0 = now_us();
  for (uint32_t i = 0; i < 20000; i++) s_bench_fn(i, i * 3, i * 5, i * 7, 0x3f800000);
  uint64_t dt = now_us() - t0;
  return (unsigned)(dt * 1000 / 20000);
}

void asl_probe_on_swap(void) {
  if (!s_installed) return;
  uint64_t now = now_us();
  w_frames++;
  if (!w_t0) { w_t0 = now; return; }
  if (now - w_t0 < WINDOW_US) return;
  unsigned f = w_frames ? w_frames : 1;
  uint64_t total = 0, inner = 0;
  for (int i = 0; i < s_nfn; i++) { total += w_calls[i]; inner += w_inner[i]; }
  log_printf("[aslprobe] %u frames: %.0f boundary calls a frame (%.0f more from inside the GL layers)",
             f, (double)total / f, (double)inner / f);
  for (int line = 0; line < 4; line++) {
    char b[440];
    int n = 0;
    b[0] = 0;
    for (int k = 0; k < 12; k++) {
      int best = -1;
      for (int i = 0; i < s_nfn; i++)
        if (w_calls[i] && (best < 0 || w_calls[i] > w_calls[best])) best = i;
      if (best < 0) break;
      n += snprintf(b + n, sizeof b - (size_t)n, " %s:%.1f", s_name[best], (double)w_calls[best] / f);
      w_calls[best] = 0;
    }
    if (!n) break;
    log_printf("[aslprobe] calls a frame:%s", b);
  }
  {
    char b[440];
    int n = 0;
    b[0] = 0;
    for (int i = 0; i < NQ && n < (int)sizeof b - 48; i++)
      if (w_q[i].n)
        n += snprintf(b + n, sizeof b - (size_t)n, " %s(0x%x):%.2f", s_name[w_q[i].fn],
                      (unsigned)w_q[i].pname, (double)w_q[i].n / f);
    log_printf("[aslprobe] queries a frame:%s", n ? b : " none");
  }
  log_printf("[aslprobe] draws a frame: elements %.1f, arrays %.1f | client vertex arrays %.1f draws, %.0f bytes; "
             "client indices %.1f draws, %.0f bytes (%.1f with buffer vertices); client vertices with buffer "
             "indices %.1f | glDrawElements %.1f us a draw, glDrawArrays %.1f us | GlesKotorSetupShader %.1f "
             "a frame, %.1f us each | recorder bench %u ns a call",
             (double)w_draw_e / f, (double)w_draw_a / f, (double)w_cv_draws / f, (double)w_cv_bytes / f,
             (double)w_ci_draws / f, (double)w_ci_bytes / f, (double)w_ci_vbo_vtx / f, (double)w_cv_vbo_idx / f,
             w_de_n ? (double)w_de_us / w_de_n : 0.0, w_da_n ? (double)w_da_us / w_da_n : 0.0,
             (double)w_setup_n / f, w_setup_n ? (double)w_setup_us / w_setup_n : 0.0, bench_ns());
  memset(w_calls, 0, sizeof w_calls);
  memset(w_inner, 0, sizeof w_inner);
  memset(w_q, 0, sizeof w_q);
  w_frames = w_draw_e = w_draw_a = w_cv_draws = w_ci_draws = w_cv_vbo_idx = w_ci_vbo_vtx = 0;
  w_cv_bytes = w_ci_bytes = w_de_us = w_da_us = w_setup_us = 0;
  w_de_n = w_da_n = w_setup_n = 0;
  w_t0 = now_us();
}

/* "_ZN5ASLgl13glGetIntegervEjPi" -> "glGetIntegerv" */
static int aslgl_name(const char *m, char *out, size_t cap) {
  if (strncmp(m, "_ZN5ASLgl", 9)) return 0;
  const char *p = m + 9;
  int len = 0;
  while (*p >= '0' && *p <= '9') len = len * 10 + (*p++ - '0');
  if (len <= 0 || (size_t)len >= cap) return 0;
  memcpy(out, p, (size_t)len);
  out[len] = 0;
  return 1;
}

static uint8_t kind_of(const char *n) {
  static const struct { const char *n; uint8_t k; } t[] = {
    {"glGetIntegerv", K_GETIV}, {"glGetFloatv", K_GETFV}, {"glGetBooleanv", K_GETBV}, {"glIsEnabled", K_ISEN},
    {"glBindBuffer", K_BIND}, {"glBindBufferARB", K_BIND}, {"glVertexPointer", K_VPTR},
    {"glColorPointer", K_CPTR}, {"glTexCoordPointer", K_TPTR}, {"glNormalPointer", K_NPTR},
    {"glSecondaryColorPointer", K_SPTR}, {"glVertexAttribPointer", K_APTR},
    {"glVertexAttribPointerARB", K_APTR}, {"glEnableClientState", K_ECS}, {"glDisableClientState", K_DCS},
    {"glEnableVertexAttribArray", K_EVA}, {"glEnableVertexAttribArrayARB", K_EVA},
    {"glDisableVertexAttribArray", K_DVA}, {"glDisableVertexAttribArrayARB", K_DVA},
    {"glClientActiveTexture", K_CAT}, {"glClientActiveTextureARB", K_CAT},
  };
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].k;
  return K_NONE;
}

static int cmp_range(const void *a, const void *b) {
  uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
  return x < y ? -1 : x > y;
}

static void patch(uintptr_t *slot, uintptr_t v) { kuKernelCpuUnrestrictedMemcpy(slot, &v, sizeof v); }

void asl_probe_install(void) {
  /* The GL layers' code, to tell their own calls from the engine's. */
  for (int i = 0; i < kotor_mod.num_dynsym && s_nin < NIN; i++) {
    const Elf32_Sym *sym = &kotor_mod.dynsym[i];
    if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC || !sym->st_value || !sym->st_size) continue;
    const char *n = kotor_mod.dynstr + sym->st_name;
    if (strncmp(n, "_ZN5ASLgl", 9) && strncmp(n, "_ZN8OpenGLES", 12) && strncmp(n, "_ZN12KotorProgram", 17) &&
        strncmp(n, "_ZN11ProgramType", 16) && strncmp(n, "ASLgl_", 6)) continue;
    s_in[s_nin].lo = kotor_mod.text_base + (sym->st_value & ~1u);
    s_in[s_nin].hi = s_in[s_nin].lo + sym->st_size;
    s_nin++;
  }
  qsort(s_in, (size_t)s_nin, sizeof s_in[0], cmp_range);

  unsigned nstub = 0, over = 0, wrapped = 0;
  for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
    Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
    unsigned type = ELF32_R_TYPE(rel->r_info);
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
    const Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
    if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC) continue;
    const char *m = kotor_mod.dynstr + sym->st_name;
    char name[40];
    if (!aslgl_name(m, name, sizeof name)) continue;
    uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
    if (!strcmp(name, "glDrawElements") && !strcmp(m, "_ZN5ASLgl14glDrawElementsEjijPKv")) {
      o_draw_e = (void *)*slot; patch(slot, (uintptr_t)&draw_e_hook); wrapped++; continue;
    }
    if (!strcmp(m, "_ZN5ASLgl12glDrawArraysEjii")) {
      o_draw_a = (void *)*slot; patch(slot, (uintptr_t)&draw_a_hook); wrapped++; continue;
    }
    if (!strcmp(m, "_ZN5ASLgl20GlesKotorSetupShaderEv")) {
      o_setup = (void *)*slot; patch(slot, (uintptr_t)&setup_hook); wrapped++; continue;
    }
    int fn = -1;
    for (int k = 0; k < s_nfn; k++) if (!strcmp(s_name[k], name)) { fn = k; break; }
    if (fn < 0) {
      if (s_nfn >= NFN) { over++; continue; }
      fn = s_nfn++;
      snprintf(s_name[fn], sizeof s_name[fn], "%s", name);
      s_kind[fn] = kind_of(name);
    }
    if (nstub >= NSTUBS) { over++; continue; }
    s_next[nstub] = *slot;
    s_fn[nstub] = (uint16_t)fn;
    __sync_synchronize();
    patch(slot, (uintptr_t)asl_probe_stubs + 8u * nstub);
    nstub++;
  }
  s_installed = 1;
  log_printf("[aslprobe] armed: %u slots for %d ASLgl functions, %u timed (draws, shader setup), %u over; "
             "%d GL-layer functions tell inner calls apart", nstub, s_nfn, wrapped, over, s_nin);
}

#define AP_STR2(x) #x
#define AP_STR(x) AP_STR2(x)
__asm__(
    ".pushsection .text.asl_probe_stubs, \"ax\", %progbits\n"
    ".syntax unified\n"
    ".arm\n"
    ".balign 4\n"
    ".global asl_probe_stubs\n"
    "asl_probe_stubs:\n"
    ".set asl_probe_i, 0\n"
    ".rept " AP_STR(NSTUBS) "\n"
    "  movw ip, #asl_probe_i\n"
    "  b asl_probe_common\n"
    "  .set asl_probe_i, asl_probe_i + 1\n"
    ".endr\n"
    "asl_probe_common:\n"
    "  push {r0-r3, ip, lr}\n"
    "  mov r0, ip\n"
    "  mov r1, lr\n"
    "  mov r2, sp\n"
    "  bl asl_probe_enter\n"
    "  mov ip, r0\n"
    "  pop {r0-r3}\n"
    "  add sp, sp, #4\n"
    "  pop {lr}\n"
    "  bx ip\n"
    ".popsection\n"
#if defined(__thumb__)
    ".thumb\n"
#endif
);

#else
void asl_probe_install(void) {}
void asl_probe_on_swap(void) {}
#endif
