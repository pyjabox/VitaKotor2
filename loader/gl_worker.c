/* gl_worker.c -- run vitaGL on a dedicated thread (GL worker).
 *
 * Every vitaGL function the loader references is linked with -Wl,--wrap
 * (cmake/glw_wraps.cmake): the loader's calls, including the addresses in the
 * game's import table, land in __wrap_<name> (generated glw_gen.c, or the
 * hand-written ones below). The wrappers record each call into a command
 * stream; the worker replays it into vitaGL. vitaGL is initialised by the
 * worker too, so it is the only thread that ever touches vitaGL or GXM.
 *
 * GL_WORKER_MODE: 0 off (wrappers call vitaGL directly), 1 inline (record,
 * then replay at once on the calling thread: validates the codec with no
 * threading), 2 worker thread.
 *
 * Producer-side state copy: buffer bindings, vertex attribute pointers and
 * enables, the current program and the unpack alignment, all as of the point
 * of recording (the worker replays in order, so it is exactly the state the
 * worker will have there). Draws use it to snapshot client-memory vertex data
 * and indices, which vitaGL only reads during the draw: the replay binds no
 * array buffer, points the attributes at the snapshot, draws, and restores
 * the original pointers and binding.
 *
 * Calls that return data run SYNC: the producer waits while the worker runs
 * them, so their pointer arguments refer to the producer's memory directly. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "gl_worker.h"
#include "gl_worker_internal.h"

struct glw_ro glw_ro;
/* Also set up once, then only read. */
static struct {
  uint32_t *ring;
  SceUID work_sema, sync_sema;
  volatile uint32_t producer_key;
} GLW_LINE R = {NULL, -1, -1, 0};

/* Custom ops (hand-written wrappers below). */
enum {
  COP_WRAP = GLW_OP_CUSTOM_BASE, COP_VAP, COP_FFP_PTR, COP_ATTR_ENABLE, COP_CLIENT_STATE, COP_BIND_BUFFER,
  COP_DELETE_BUFFERS, COP_DRAW_ARRAYS, COP_DRAW_ELEMENTS, COP_PIXEL_STORE, COP_USE_PROGRAM, COP_SWAP,
  COP_GETPROC, COP_DRAW_ARRAYS_RAW, COP_DRAW_ELEMENTS_RAW,
  COP_ENABLE, COP_BIND_FB, COP_SCISSOR, COP_VIEWPORT, COP_BEGIN_QUERY, COP_END_QUERY, COP_DELETE_PROGRAM,
  COP_CREATE_PROGRAM, COP_GEN_QUERIES, COP_IS_PROGRAM, COP_IS_ENABLED, COP_GET_INTEGERV, COP_GET_QUERY,
  COP_GET_ERROR,
};

extern void __real_glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
extern void __real_glVertexPointer(GLint, GLenum, GLsizei, const GLvoid *);
extern void __real_glTexCoordPointer(GLint, GLenum, GLsizei, const GLvoid *);
extern void __real_glEnableVertexAttribArray(GLuint);
extern void __real_glDisableVertexAttribArray(GLuint);
extern void __real_glEnableClientState(GLenum);
extern void __real_glDisableClientState(GLenum);
extern void __real_glBindBuffer(GLenum, GLuint);
extern void __real_glDeleteBuffers(GLsizei, const GLuint *);
extern void __real_glDrawArrays(GLenum, GLint, GLsizei);
extern void __real_glDrawElements(GLenum, GLsizei, GLenum, const GLvoid *);
extern void __real_glPixelStorei(GLenum, GLint);
extern void __real_glUseProgram(GLuint);
extern void __real_vglSwapBuffers(GLboolean);
extern void *__real_vglGetProcAddress(const char *);
extern GLboolean __real_glIsProgram(GLuint);
extern GLboolean __real_glIsEnabled(GLenum);
extern void __real_glGetIntegerv(GLenum, GLint *);
extern GLenum __real_glGetError(void);
extern void __real_glGetQueryObjectuiv(GLuint, GLenum, GLuint *);
extern void __real_glBeginQuery(GLenum, GLuint);
extern void __real_glEndQuery(GLenum);
extern void __real_glGenQueries(GLsizei, GLuint *);
extern GLuint __real_glCreateProgram(void);
extern void __real_glDeleteProgram(GLuint);
extern void __real_glEnable(GLenum);
extern void __real_glDisable(GLenum);
extern void __real_glBindFramebuffer(GLenum, GLuint);
extern void __real_glScissor(GLint, GLint, GLsizei, GLsizei);
extern void __real_glViewport(GLint, GLint, GLsizei, GLsizei);

/* ---- stats ------------------------------------------------------------------ */
static uint32_t st_cmds, st_syncs, st_sync_us, st_stall_us, st_pace_us, st_capture_bytes, st_raw_draws;
static uint32_t st_sync_by_op[GLW_OP_CUSTOM_BASE + 48];
static uint32_t st_shadow_hits, st_wakes, st_skipped;
/* Skip state calls that change nothing (alternates on/off per window when
 * GL_WORKER_DEDUP_AB is set). */
static int s_dedup = GL_WORKER_DEDUP;

/* ---- command stream ---------------------------------------------------------- */
#define RING_WORDS (1u << 18)                    /* 1 MB: stays warm in the shared L2 */
/* Written by the game thread only. */
static struct {
  volatile uint32_t pub_wr;                      /* published write index */
} GLW_LINE P;
/* Written by the worker only. */
static struct {
  volatile uint32_t rd;                          /* read index, stored every few commands */
  volatile uint32_t idle;                        /* about to sleep on the semaphore */
  volatile uint32_t sync_ret;
  volatile uint32_t swaps_done;
  volatile uint32_t busy_us;
} GLW_LINE W;
static uint32_t s_wr;                            /* producer's write index */
static uint32_t s_rd_seen;                       /* W.rd as the producer last read it */
static uint32_t *s_cur;                          /* command being built */
static uint32_t s_scratch[(GLW_INLINE_MAX + 64 * 1024) / 4 + 64];   /* inline mode */
static uint32_t s_foreign[(GLW_INLINE_MAX + 64 * 1024) / 4 + 64];   /* dropped calls */
static uint32_t s_swaps_issued;

static void execute(uint32_t *c, uint32_t *ret);

/* Commands are made visible to the worker in batches (one barrier and one
 * index store per batch instead of per command). s_batch alternates with 1 at
 * each report window for an A/B when GL_WORKER_BATCH_AB is set. */
static uint32_t s_unpub, s_batch = GL_WORKER_BATCH;
static void flush(void) {
  if (!s_unpub) return;
  __sync_synchronize();
  P.pub_wr = s_wr;
  s_unpub = 0;
  if (W.idle) { sceKernelSignalSema(R.work_sema, 1); st_wakes++; }
}

#define DEDUP_GROUPS 24
static struct { uint32_t op, n, v[4]; uint8_t valid; } s_last[DEDUP_GROUPS];
int glw_dedup(uint32_t group, uint32_t op, const uint32_t *v, uint32_t n) {
  if (group >= DEDUP_GROUPS || n > 4) return 0;
  if (s_dedup && s_last[group].valid && s_last[group].op == op && s_last[group].n == n &&
      !memcmp(s_last[group].v, v, n * 4)) {
    st_skipped++;
    return 1;
  }
  s_last[group].op = op;
  s_last[group].n = n;
  memcpy(s_last[group].v, v, n * 4);
  s_last[group].valid = 1;
  return 0;
}

/* Only one thread may record (the game thread once attached; before that,
 * whoever calls). Others are logged and dropped: their command is built in a
 * throwaway buffer and never published. */
static int foreign_thread(void) {
  uint32_t k = glw_tls_key();
  if (!R.producer_key || k == R.producer_key) return 0;
  static int warned;
  if (!warned++) log_printf("[glw] GL call from a thread that is not the producer (key 0x%08x) -- dropped", (unsigned)k);
  return 1;
}

static uint32_t *ring_reserve(uint32_t n) {
  uint64_t t0 = 0;
  for (;;) {
    /* A stale copy of the read index only understates the free space, so
     * W.rd (the worker's line) is read only when the copy says full. */
    uint32_t rd = s_rd_seen;
    if (s_wr >= rd) {
      if (s_wr + n < RING_WORDS) break;
      if (n < rd) {                               /* wrap: marker, then start at 0 */
        R.ring[s_wr] = COP_WRAP | (1u << 16);
        __sync_synchronize();
        s_wr = 0;
        P.pub_wr = 0;                             /* publishes everything up to the marker */
        s_unpub = 0;
        continue;
      }
    } else if (s_wr + n < rd) {
      break;
    }
    uint32_t now = W.rd;
    if (now != s_rd_seen) { s_rd_seen = now; continue; }
    if (!t0) t0 = sceKernelGetProcessTimeWide();
    flush();
    if (W.idle) sceKernelSignalSema(R.work_sema, 1);
    sceKernelDelayThread(20);
  }
  if (t0) st_stall_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
  return R.ring + s_wr;
}

uint32_t *glw_begin(uint32_t op, uint32_t nargs, uint32_t data_bytes) {
  uint32_t n = 1 + nargs + (data_bytes + 3) / 4;
  if (foreign_thread()) return s_foreign + 1;
  uint32_t *c = glw_ro.mode == 2 ? ring_reserve(n) : s_scratch;
  c[0] = op | (n << 16);
  s_cur = c;
  return c + 1;
}

static void publish(void) {
  uint32_t h = s_cur[0];
  s_wr += h >> 16;
  st_cmds++;
  if (++s_unpub >= s_batch || (h & GLW_SYNC) || (h & GLW_OP_MASK) == COP_SWAP) flush();
}

void glw_end(uint32_t *a) {
  if (a == s_foreign + 1) return;
  if (glw_ro.mode == 2) { publish(); return; }
  uint32_t ret;
  glw_ro.in_replay = 1;
  execute(s_cur, &ret);
  glw_ro.in_replay = 0;
  st_cmds++;
}

uint32_t glw_sync(uint32_t *a) {
  if (a == s_foreign + 1) return 0;
  uint32_t op = s_cur[0] & GLW_OP_MASK;
  st_syncs++;
  if (op < sizeof st_sync_by_op / sizeof st_sync_by_op[0]) st_sync_by_op[op]++;
  if (glw_ro.mode != 2) {
    uint32_t ret = 0;
    glw_ro.in_replay = 1;
    execute(s_cur, &ret);
    glw_ro.in_replay = 0;
    st_cmds++;
    return ret;
  }
  uint64_t t0 = sceKernelGetProcessTimeWide();
  publish();                       /* a SYNC command always flushes */
  {
    GLW_PROF(3);
    sceKernelSignalSema(R.work_sema, 1);
    sceKernelWaitSema(R.sync_sema, 1, NULL);
  }
  __sync_synchronize();
  st_sync_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
  return W.sync_ret;
}

/* ---- producer-side state ------------------------------------------------------ */
#define ATTRS 16
typedef struct {
  uint8_t enabled, client, norm, set; int32_t size; uint32_t type; int32_t stride; const void *ptr; uint32_t buf;
} attr_t;
static attr_t sh_attr[ATTRS];
static uint32_t sh_array_buf, sh_elem_buf, sh_prog, sh_unpack = 4, sh_ffp_client;

static uint32_t type_size(uint32_t t) {
  switch (t) {
  case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
  case GL_SHORT: case GL_UNSIGNED_SHORT: case 0x140B /* HALF_FLOAT */: return 2;
  default: return 4;
  }
}

uint32_t glw_pixels_size(int w, int h, unsigned format, unsigned type) {
  if (w <= 0 || h <= 0) return 0;
  uint32_t bpp;
  if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_5_6_5) {
    bpp = 2;
  } else {
    uint32_t comp = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
    bpp = comp * (type == GL_FLOAT ? 4 : 1);
  }
  uint32_t row = (uint32_t)w * bpp, a = sh_unpack ? sh_unpack : 1;
  uint32_t pitch = (row + a - 1) & ~(a - 1);
  return pitch * (uint32_t)(h - 1) + row;
}

static int producer_ok(void) { return !foreign_thread(); }

/* ---- hand-written wrappers ------------------------------------------------------ */
void __wrap_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                  const void *pointer) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glVertexAttribPointer(index, size, type, normalized, stride, pointer); return; }
  if (!producer_ok()) return;
  if (index < ATTRS) {
    attr_t *at = &sh_attr[index];
    if (s_dedup && at->set && at->buf == sh_array_buf && at->size == size && at->type == type &&
        at->norm == normalized && at->stride == stride && at->ptr == pointer) {
      st_skipped++;
      return;
    }
    at->client = sh_array_buf == 0;
    at->size = size; at->type = type; at->norm = normalized; at->stride = stride; at->ptr = pointer;
    at->buf = sh_array_buf; at->set = 1;
  }
  uint32_t *a = glw_begin(COP_VAP, 6, 0);
  a[0] = index; a[1] = (uint32_t)size; a[2] = type; a[3] = normalized; a[4] = (uint32_t)stride;
  a[5] = (uint32_t)(uintptr_t)pointer;
  glw_end(a);
}

static void ffp_ptr(uint32_t which, GLint size, GLenum type, GLsizei stride, const void *pointer) {
  uint32_t *a = glw_begin(COP_FFP_PTR, 5, 0);
  a[0] = which; a[1] = (uint32_t)size; a[2] = type; a[3] = (uint32_t)stride; a[4] = (uint32_t)(uintptr_t)pointer;
  glw_end(a);
}
void __wrap_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glVertexPointer(size, type, stride, pointer); return; }
  if (producer_ok()) ffp_ptr(0, size, type, stride, pointer);
}
void __wrap_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glTexCoordPointer(size, type, stride, pointer); return; }
  if (producer_ok()) ffp_ptr(1, size, type, stride, pointer);
}

static void attr_enable(GLuint index, uint32_t on) {
  if (index < ATTRS) {
    if (s_dedup && sh_attr[index].enabled == on) { st_skipped++; return; }
    sh_attr[index].enabled = (uint8_t)on;
  }
  uint32_t *a = glw_begin(COP_ATTR_ENABLE, 2, 0);
  a[0] = index; a[1] = on;
  glw_end(a);
}
void __wrap_glEnableVertexAttribArray(GLuint index) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glEnableVertexAttribArray(index); return; }
  if (producer_ok()) attr_enable(index, 1);
}
void __wrap_glDisableVertexAttribArray(GLuint index) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glDisableVertexAttribArray(index); return; }
  if (producer_ok()) attr_enable(index, 0);
}

static void client_state(GLenum array, uint32_t on) {
  uint32_t bit = array == GL_VERTEX_ARRAY ? 1 : array == GL_TEXTURE_COORD_ARRAY ? 2 : array == GL_COLOR_ARRAY ? 4 : 8;
  if (on) sh_ffp_client |= bit; else sh_ffp_client &= ~bit;
  uint32_t *a = glw_begin(COP_CLIENT_STATE, 2, 0);
  a[0] = array; a[1] = on;
  glw_end(a);
}
void __wrap_glEnableClientState(GLenum array) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glEnableClientState(array); return; }
  if (producer_ok()) client_state(array, 1);
}
void __wrap_glDisableClientState(GLenum array) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glDisableClientState(array); return; }
  if (producer_ok()) client_state(array, 0);
}

void __wrap_glBindBuffer(GLenum target, GLuint buffer) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glBindBuffer(target, buffer); return; }
  if (!producer_ok()) return;
  if (target == GL_ARRAY_BUFFER) {
    if (s_dedup && sh_array_buf == buffer) { st_skipped++; return; }
    sh_array_buf = buffer;
  } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
    if (s_dedup && sh_elem_buf == buffer) { st_skipped++; return; }
    sh_elem_buf = buffer;
  }
  uint32_t *a = glw_begin(COP_BIND_BUFFER, 2, 0);
  a[0] = target; a[1] = buffer;
  glw_end(a);
}

void __wrap_glDeleteBuffers(GLsizei n, const GLuint *buffers) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glDeleteBuffers(n, buffers); return; }
  if (!producer_ok() || n <= 0 || !buffers) return;
  for (GLsizei i = 0; i < n; i++) {
    if (buffers[i] == sh_array_buf) sh_array_buf = 0;
    if (buffers[i] == sh_elem_buf) sh_elem_buf = 0;
    /* An attribute pointer into a deleted buffer must be re-sent even if the
     * same name is created and bound again. */
    for (int k = 0; k < ATTRS; k++) if (sh_attr[k].buf == buffers[i]) sh_attr[k].set = 0;
  }
  uint32_t bytes = (uint32_t)n * 4u;
  if (bytes > GLW_INLINE_MAX) {          /* never in practice */
    uint32_t *a = glw_begin(COP_DELETE_BUFFERS | GLW_SYNC, 2, 0);
    a[0] = (uint32_t)n; a[1] = (uint32_t)(uintptr_t)buffers;
    (void)glw_sync(a);
    return;
  }
  uint32_t *a = glw_begin(COP_DELETE_BUFFERS, 2, bytes);
  a[0] = (uint32_t)n; a[1] = 1;
  memcpy(a + 2, buffers, bytes);
  glw_end(a);
}

void __wrap_glPixelStorei(GLenum pname, GLint param) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glPixelStorei(pname, param); return; }
  if (!producer_ok()) return;
  if (pname == GL_UNPACK_ALIGNMENT) sh_unpack = (uint32_t)param;
  uint32_t *a = glw_begin(COP_PIXEL_STORE, 2, 0);
  a[0] = pname; a[1] = (uint32_t)param;
  glw_end(a);
}

void __wrap_glUseProgram(GLuint program) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glUseProgram(program); return; }
  if (!producer_ok()) return;
  if (s_dedup && sh_prog == program && program) { st_skipped++; return; }
  sh_prog = program;
  uint32_t *a = glw_begin(COP_USE_PROGRAM, 1, 0);
  a[0] = program;
  glw_end(a);
}

/* Highest index in a client-side index array. */
static uint32_t max_index(const void *idx, GLsizei count, GLenum type) {
  uint32_t m = 0;
  if (type == GL_UNSIGNED_SHORT) {
    const uint16_t *p = idx;
    for (GLsizei i = 0; i < count; i++) if (p[i] > m) m = p[i];
  } else if (type == GL_UNSIGNED_INT) {
    const uint32_t *p = idx;
    for (GLsizei i = 0; i < count; i++) if (p[i] > m) m = p[i];
  } else {
    const uint8_t *p = idx;
    for (GLsizei i = 0; i < count; i++) if (p[i] > m) m = p[i];
  }
  return m;
}

/* Client attributes a draw reads, and the bytes for vertices [first, first+n). */
typedef struct { uint32_t idx, eff_stride, bytes; } cap_t;
static int collect_client(uint32_t first, uint32_t n, cap_t *cap, uint32_t *total) {
  int k = 0;
  *total = 0;
  for (uint32_t i = 0; i < ATTRS; i++) {
    attr_t *at = &sh_attr[i];
    if (!at->enabled || !at->client || !at->ptr || n == 0) continue;
    uint32_t elem = (uint32_t)at->size * type_size(at->type);
    uint32_t eff = at->stride ? (uint32_t)at->stride : elem;
    cap[k].idx = i;
    cap[k].eff_stride = eff;
    cap[k].bytes = (n - 1) * eff + elem;
    *total += (cap[k].bytes + 3) & ~3u;
    k++;
  }
  (void)first;
  return k;
}

/* Record: a[] = header args, then per client attribute 7 words
 * {index, size, type, norm, stride, original pointer, data offset in words}. */
static uint32_t *put_captures(uint32_t *a, uint32_t nhead, const cap_t *cap, int k, uint32_t first,
                              uint32_t data_word0) {
  uint32_t *rec = a + nhead, off = data_word0;
  uint8_t *data = (uint8_t *)a;
  for (int j = 0; j < k; j++) {
    attr_t *at = &sh_attr[cap[j].idx];
    rec[0] = cap[j].idx; rec[1] = (uint32_t)at->size; rec[2] = at->type; rec[3] = at->norm;
    rec[4] = (uint32_t)at->stride; rec[5] = (uint32_t)(uintptr_t)at->ptr; rec[6] = off;
    memcpy(data + off * 4, (const uint8_t *)at->ptr + (size_t)first * cap[j].eff_stride, cap[j].bytes);
    off += (cap[j].bytes + 3) / 4;
    rec += 7;
  }
  st_capture_bytes += (off - data_word0) * 4;
  return rec;
}

static int ffp_client_draw(void) { return sh_prog == 0 && sh_ffp_client; }

void __wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  GLW_PROF(2);
  if (glw_direct()) { __real_glDrawArrays(mode, first, count); return; }
  if (!producer_ok()) return;
  cap_t cap[ATTRS];
  uint32_t total = 0;
  int k = count > 0 ? collect_client((uint32_t)first, (uint32_t)count, cap, &total) : 0;
  if (ffp_client_draw() || total > GLW_INLINE_MAX) {
    /* Fixed-function client arrays, or too much data: the worker draws from
     * the producer's memory while the producer waits. */
    st_raw_draws++;
    uint32_t *a = glw_begin(COP_DRAW_ARRAYS_RAW | GLW_SYNC, 3, 0);
    a[0] = mode; a[1] = (uint32_t)first; a[2] = (uint32_t)count;
    (void)glw_sync(a);
    return;
  }
  uint32_t nhead = 5, nwords = nhead + 7u * (uint32_t)k;
  uint32_t *a = glw_begin(COP_DRAW_ARRAYS, nwords, total);
  a[0] = mode; a[1] = (uint32_t)first; a[2] = (uint32_t)count; a[3] = (uint32_t)k; a[4] = sh_array_buf;
  put_captures(a, nhead, cap, k, (uint32_t)first, nwords);
  glw_end(a);
}

void __wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
  GLW_PROF(2);
  if (glw_direct()) { __real_glDrawElements(mode, count, type, indices); return; }
  if (!producer_ok()) return;
  uint32_t isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
  int client_idx = sh_elem_buf == 0 && indices && count > 0;
  uint32_t ibytes = client_idx ? (uint32_t)count * isz : 0;
  cap_t cap[ATTRS];
  uint32_t total = 0;
  int any_client = 0;
  for (int i = 0; i < ATTRS; i++) if (sh_attr[i].enabled && sh_attr[i].client && sh_attr[i].ptr) any_client = 1;
  int k = 0;
  if (any_client && client_idx) k = collect_client(0, max_index(indices, count, type) + 1, cap, &total);
  if (ffp_client_draw() || (any_client && !client_idx) || total + ibytes > GLW_INLINE_MAX) {
    st_raw_draws++;
    uint32_t *a = glw_begin(COP_DRAW_ELEMENTS_RAW | GLW_SYNC, 4, 0);
    a[0] = mode; a[1] = (uint32_t)count; a[2] = type; a[3] = (uint32_t)(uintptr_t)indices;
    (void)glw_sync(a);
    return;
  }
  uint32_t nhead = 7, nwords = nhead + 7u * (uint32_t)k;
  uint32_t ibytes4 = (ibytes + 3) & ~3u;
  uint32_t *a = glw_begin(COP_DRAW_ELEMENTS, nwords, ibytes4 + total);
  a[0] = mode; a[1] = (uint32_t)count; a[2] = type; a[3] = (uint32_t)k; a[4] = sh_array_buf;
  a[5] = client_idx;
  if (client_idx) {
    a[6] = nwords;                                     /* index data offset (words) */
    memcpy(a + nwords, indices, ibytes);
    st_capture_bytes += ibytes4;
  } else {
    a[6] = (uint32_t)(uintptr_t)indices;               /* offset into the element buffer */
  }
  put_captures(a, nhead, cap, k, 0, nwords + ibytes4 / 4);
  glw_end(a);
}


/* ---- answers from the producer-side state copy (no round trip) --------------------- */
/* Live program names: vitaGL hands out slot + 1 (custom_shaders.c), all through
 * glCreateProgram/glDeleteProgram below. */
#define PROGS 1024
static uint8_t sh_prog_live[PROGS + 1];

GLuint __wrap_glCreateProgram(void) {
  GLW_PROF(1);
  if (glw_direct()) return __real_glCreateProgram();
  uint32_t *a = glw_begin(COP_CREATE_PROGRAM | GLW_SYNC, 0, 0);
  GLuint p = (GLuint)glw_sync(a);
  if (p && p <= PROGS) sh_prog_live[p] = 1;
  return p;
}
void __wrap_glDeleteProgram(GLuint prog) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glDeleteProgram(prog); return; }
  if (!producer_ok()) return;
  if (prog && prog <= PROGS) sh_prog_live[prog] = 0;
  if (prog == sh_prog) sh_prog = 0;
  uint32_t *a = glw_begin(COP_DELETE_PROGRAM, 1, 0);
  a[0] = prog;
  glw_end(a);
}
GLboolean __wrap_glIsProgram(GLuint prog) {
  GLW_PROF(1);
  if (glw_direct()) return __real_glIsProgram(prog);
  if (prog <= PROGS) { st_shadow_hits++; return prog ? sh_prog_live[prog] : GL_FALSE; }
  uint32_t *a = glw_begin(COP_IS_PROGRAM | GLW_SYNC, 1, 0);
  a[0] = prog;
  return (GLboolean)glw_sync(a);
}

/* Enables: each cap as last set through glEnable/glDisable, or as first read
 * from vitaGL (then kept, since every change comes through here). */
#define CAPS 64
static struct { uint32_t cap; uint8_t known, on; } sh_cap[CAPS];
static int cap_slot(uint32_t cap) {
  for (int i = 0; i < CAPS; i++) {
    if (sh_cap[i].known && sh_cap[i].cap == cap) return i;
    if (!sh_cap[i].known) return i;
  }
  return -1;
}
static void set_cap(GLenum cap, uint32_t on) {
  int i = cap_slot(cap);
  if (i >= 0 && s_dedup && sh_cap[i].known && sh_cap[i].cap == cap && sh_cap[i].on == on) { st_skipped++; return; }
  if (i >= 0) { sh_cap[i].cap = cap; sh_cap[i].known = 1; sh_cap[i].on = (uint8_t)on; }
  uint32_t *a = glw_begin(COP_ENABLE, 2, 0);
  a[0] = cap; a[1] = on;
  glw_end(a);
}
void __wrap_glEnable(GLenum cap) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glEnable(cap); return; }
  if (producer_ok()) set_cap(cap, 1);
}
void __wrap_glDisable(GLenum cap) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glDisable(cap); return; }
  if (producer_ok()) set_cap(cap, 0);
}
GLboolean __wrap_glIsEnabled(GLenum cap) {
  GLW_PROF(1);
  if (glw_direct()) return __real_glIsEnabled(cap);
  int i = cap_slot(cap);
  if (i >= 0 && sh_cap[i].known && sh_cap[i].cap == cap) { st_shadow_hits++; return sh_cap[i].on; }
  static int logged;
  if (logged++ < 16) log_printf("[glw] isEnabled(0x%04x) not in the state copy: sync", (unsigned)cap);
  uint32_t *a = glw_begin(COP_IS_ENABLED | GLW_SYNC, 1, 0);
  a[0] = cap;
  GLboolean r = (GLboolean)glw_sync(a);
  if (i >= 0 && !sh_cap[i].known) { sh_cap[i].cap = cap; sh_cap[i].known = 1; sh_cap[i].on = r ? 1 : 0; }
  return r;
}

/* Integer state the game and the loader read every frame. */
static uint32_t sh_fb, sh_fb_known;
static int32_t sh_scissor[4], sh_viewport[4];
static uint8_t sh_scissor_known, sh_viewport_known;

void __wrap_glBindFramebuffer(GLenum target, GLuint fb) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glBindFramebuffer(target, fb); return; }
  if (!producer_ok()) return;
  if (target == GL_FRAMEBUFFER) { sh_fb = fb; sh_fb_known = 1; }
  else sh_fb_known = 0;
  uint32_t *a = glw_begin(COP_BIND_FB, 2, 0);
  a[0] = target; a[1] = fb;
  glw_end(a);
}
static void rect(uint32_t op, int32_t *sh, uint8_t *known, GLint x, GLint y, GLsizei w, GLsizei h) {
  if (s_dedup && *known && sh[0] == x && sh[1] == y && sh[2] == w && sh[3] == h) { st_skipped++; return; }
  sh[0] = x; sh[1] = y; sh[2] = w; sh[3] = h; *known = 1;
  uint32_t *a = glw_begin(op, 4, 0);
  a[0] = (uint32_t)x; a[1] = (uint32_t)y; a[2] = (uint32_t)w; a[3] = (uint32_t)h;
  glw_end(a);
}
void __wrap_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glScissor(x, y, w, h); return; }
  if (producer_ok()) rect(COP_SCISSOR, sh_scissor, &sh_scissor_known, x, y, w, h);
}
void __wrap_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glViewport(x, y, w, h); return; }
  if (producer_ok()) rect(COP_VIEWPORT, sh_viewport, &sh_viewport_known, x, y, w, h);
}
void __wrap_glGetIntegerv(GLenum pname, GLint *data) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glGetIntegerv(pname, data); return; }
  if (data) {
    switch (pname) {
    case GL_FRAMEBUFFER_BINDING: if (sh_fb_known) { data[0] = (GLint)sh_fb; st_shadow_hits++; return; } break;
    case GL_SCISSOR_BOX: if (sh_scissor_known) { memcpy(data, sh_scissor, 16); st_shadow_hits++; return; } break;
    case GL_VIEWPORT: if (sh_viewport_known) { memcpy(data, sh_viewport, 16); st_shadow_hits++; return; } break;
    case GL_CURRENT_PROGRAM: data[0] = (GLint)sh_prog; st_shadow_hits++; return;
    case GL_ARRAY_BUFFER_BINDING: data[0] = (GLint)sh_array_buf; st_shadow_hits++; return;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: data[0] = (GLint)sh_elem_buf; st_shadow_hits++; return;
    case GL_UNPACK_ALIGNMENT: data[0] = (GLint)sh_unpack; st_shadow_hits++; return;
    }
  }
  static int logged;
  if (logged++ < 24) log_printf("[glw] getIntegerv(0x%04x) not in the state copy: sync", (unsigned)pname);
  uint32_t *a = glw_begin(COP_GET_INTEGERV | GLW_SYNC, 2, 0);
  a[0] = pname; a[1] = (uint32_t)(uintptr_t)data;
  (void)glw_sync(a);
  if (data && pname == GL_FRAMEBUFFER_BINDING) { sh_fb = (uint32_t)data[0]; sh_fb_known = 1; }
  if (data && pname == GL_SCISSOR_BOX) { memcpy(sh_scissor, data, 16); sh_scissor_known = 1; }
  if (data && pname == GL_VIEWPORT) { memcpy(sh_viewport, data, 16); sh_viewport_known = 1; }
}

/* Errors stay on the worker: the game only logs them. */
GLenum __wrap_glGetError(void) {
  GLW_PROF(1);
  if (glw_direct()) return __real_glGetError();
  st_shadow_hits++;
  return GL_NO_ERROR;
}

/* Occlusion queries. The worker polls ended queries after each swap and
 * publishes {result, generation}; the producer answers AVAILABLE / NO_WAIT
 * from that. Results arrive a frame or two later than polling vitaGL directly,
 * which the culler already tolerates ("not available yet" is valid GL). */
#define QUERIES 256
typedef struct {
  volatile uint32_t id, gen, ended_gen, done_gen, result;
} qslot_t;
static qslot_t s_q[QUERIES];
static uint32_t s_nq, s_open_q[2] = {~0u, ~0u};   /* open query slot per target kind */

static int qslot(GLuint id) {
  for (uint32_t i = 0; i < s_nq; i++) if (s_q[i].id == id) return (int)i;
  return -1;
}
void __wrap_glGenQueries(GLsizei n, GLuint *ids) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glGenQueries(n, ids); return; }
  uint32_t *a = glw_begin(COP_GEN_QUERIES | GLW_SYNC, 2, 0);
  a[0] = (uint32_t)n; a[1] = (uint32_t)(uintptr_t)ids;
  (void)glw_sync(a);
  for (GLsizei i = 0; ids && i < n && s_nq < QUERIES; i++) {
    if (!ids[i] || qslot(ids[i]) >= 0) continue;
    qslot_t *q = &s_q[s_nq];
    q->gen = q->ended_gen = q->done_gen = 0;
    q->result = 0;
    __sync_synchronize();
    q->id = ids[i];
    __sync_synchronize();
    s_nq++;
  }
}
void __wrap_glBeginQuery(GLenum target, GLuint id) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glBeginQuery(target, id); return; }
  if (!producer_ok()) return;
  int i = qslot(id);
  uint32_t gen = 0;
  if (i >= 0) { gen = ++s_q[i].gen; s_open_q[target == 0x88BF /* GL_TIME_ELAPSED */ ? 1 : 0] = (uint32_t)i; }
  uint32_t *a = glw_begin(COP_BEGIN_QUERY, 2, 0);
  a[0] = target; a[1] = id;
  glw_end(a);
  (void)gen;
}
void __wrap_glEndQuery(GLenum target) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glEndQuery(target); return; }
  if (!producer_ok()) return;
  uint32_t k = target == 0x88BF /* GL_TIME_ELAPSED */ ? 1 : 0, slot = s_open_q[k];
  s_open_q[k] = ~0u;
  uint32_t *a = glw_begin(COP_END_QUERY, 3, 0);
  a[0] = target; a[1] = slot; a[2] = slot < QUERIES ? s_q[slot].gen : 0;
  glw_end(a);
}
void __wrap_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params) {
  GLW_PROF(1);
  if (glw_direct()) { __real_glGetQueryObjectuiv(id, pname, params); return; }
  int i = qslot(id);
  if (i >= 0 && params && (pname == GL_QUERY_RESULT_AVAILABLE || pname == GL_QUERY_RESULT_NO_WAIT)) {
    qslot_t *q = &s_q[i];
    uint32_t done = q->done_gen;
    __sync_synchronize();
    int ready = done == q->gen && done != 0;
    st_shadow_hits++;
    if (pname == GL_QUERY_RESULT_AVAILABLE) params[0] = ready ? GL_TRUE : GL_FALSE;
    else if (ready) params[0] = q->result;
    return;
  }
  static int logged;
  if (logged++ < 8) log_printf("[glw] getQueryObjectuiv(id 0x%x, 0x%04x) not answerable locally: sync", (unsigned)id, (unsigned)pname);
  uint32_t *a = glw_begin(COP_GET_QUERY | GLW_SYNC, 3, 0);
  a[0] = id; a[1] = pname; a[2] = (uint32_t)(uintptr_t)params;
  (void)glw_sync(a);
}

/* Worker: after each swap, collect results of queries that have ended. */
static void poll_queries(void) {
  uint32_t n = s_nq;
  for (uint32_t i = 0; i < n; i++) {
    qslot_t *q = &s_q[i];
    uint32_t ended = q->ended_gen;
    if (!ended || ended == q->done_gen) continue;
    GLuint avail = 0, res = 0;
    __real_glGetQueryObjectuiv(q->id, GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) continue;
    __real_glGetQueryObjectuiv(q->id, GL_QUERY_RESULT_NO_WAIT, &res);
    q->result = res;
    __sync_synchronize();
    q->done_gen = ended;
  }
}

/* ---- frame boundary --------------------------------------------------------------- */
static void glw_report(void);

void __wrap_vglSwapBuffers(GLboolean has_commondialog) {
  GLW_PROF(4);
  if (glw_direct()) {
    __real_vglSwapBuffers(has_commondialog);
    if (glw_ro.mode == 0) glw_report();
    return;
  }
  if (!producer_ok()) return;
  uint32_t *a = glw_begin(COP_SWAP, 1, 0);
  a[0] = has_commondialog;
  glw_end(a);
  memset(s_last, 0, sizeof s_last);   /* generic dedup: start each frame afresh */
  s_swaps_issued++;
  if (glw_ro.mode == 2) {
    uint64_t t0 = 0;
    while (s_swaps_issued - W.swaps_done > GL_WORKER_FRAMES_AHEAD) {
      if (!t0) t0 = sceKernelGetProcessTimeWide();
      if (W.idle) sceKernelSignalSema(R.work_sema, 1);
      sceKernelDelayThread(50);
    }
    if (t0) st_pace_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
  } else {
    W.swaps_done = s_swaps_issued;
  }
  glw_report();
}

void *__wrap_vglGetProcAddress(const char *name) {
  GLW_PROF(1);
  if (glw_direct()) return __real_vglGetProcAddress(name);
  if (name)
    for (const glw_proc_t *p = glw_procs; p->name; p++)
      if (!strcmp(p->name, name)) return p->fn;
  static int warned;
  if (warned++ < 8) log_printf("[glw] vglGetProcAddress(%s): not wrapped, raw vitaGL pointer", name ? name : "?");
  if (!producer_ok()) return NULL;
  uint32_t *a = glw_begin(COP_GETPROC | GLW_SYNC, 1, 0);
  a[0] = (uint32_t)(uintptr_t)name;
  return (void *)(uintptr_t)glw_sync(a);
}

/* ---- replay (the worker, or inline) ------------------------------------------------- */
static void replay_draw_captures(const uint32_t *a, uint32_t nhead, uint32_t k, uint32_t array_buf,
                                 uint32_t first, int restore) {
  const uint32_t *rec = a + nhead;
  const uint8_t *data = (const uint8_t *)a;
  if (!k) return;
  __real_glBindBuffer(GL_ARRAY_BUFFER, 0);
  for (uint32_t j = 0; j < k; j++, rec += 7) {
    uint32_t elem = rec[1] * type_size(rec[2]);
    uint32_t eff = rec[4] ? rec[4] : elem;
    const uint8_t *p = restore ? (const uint8_t *)(uintptr_t)rec[5]
                               : data + rec[6] * 4 - (size_t)first * eff;
    __real_glVertexAttribPointer(rec[0], (GLint)rec[1], rec[2], (GLboolean)rec[3], (GLsizei)rec[4], p);
  }
  __real_glBindBuffer(GL_ARRAY_BUFFER, array_buf);
}

static void execute(uint32_t *c, uint32_t *ret) {
  uint32_t op = c[0] & GLW_OP_MASK, sync = (c[0] & GLW_SYNC) != 0;
  const uint32_t *a = c + 1;
  *ret = 0;
  if (op < GLW_OP_CUSTOM_BASE) {
    if (!glw_replay_gen(op, sync, a, ret)) log_printf("[glw] FATAL: unknown op %u", (unsigned)op);
    return;
  }
  switch (op) {
  case COP_VAP:
    __real_glVertexAttribPointer(a[0], (GLint)a[1], a[2], (GLboolean)a[3], (GLsizei)a[4], (const void *)(uintptr_t)a[5]);
    break;
  case COP_FFP_PTR:
    if (a[0] == 0) __real_glVertexPointer((GLint)a[1], a[2], (GLsizei)a[3], (const void *)(uintptr_t)a[4]);
    else __real_glTexCoordPointer((GLint)a[1], a[2], (GLsizei)a[3], (const void *)(uintptr_t)a[4]);
    break;
  case COP_ATTR_ENABLE:
    if (a[1]) __real_glEnableVertexAttribArray(a[0]); else __real_glDisableVertexAttribArray(a[0]);
    break;
  case COP_CLIENT_STATE:
    if (a[1]) __real_glEnableClientState(a[0]); else __real_glDisableClientState(a[0]);
    break;
  case COP_BIND_BUFFER: __real_glBindBuffer(a[0], a[1]); break;
  case COP_DELETE_BUFFERS:
    __real_glDeleteBuffers((GLsizei)a[0], sync ? (const GLuint *)(uintptr_t)a[1] : (const GLuint *)(a + 2));
    break;
  case COP_PIXEL_STORE: __real_glPixelStorei(a[0], (GLint)a[1]); break;
  case COP_USE_PROGRAM: __real_glUseProgram(a[0]); break;
  case COP_DRAW_ARRAYS:
    replay_draw_captures(a, 5, a[3], a[4], a[1], 0);
    __real_glDrawArrays(a[0], (GLint)a[1], (GLsizei)a[2]);
    replay_draw_captures(a, 5, a[3], a[4], a[1], 1);
    break;
  case COP_DRAW_ELEMENTS:
    replay_draw_captures(a, 7, a[3], a[4], 0, 0);
    __real_glDrawElements(a[0], (GLsizei)a[1], a[2], a[5] ? (const void *)(a + a[6]) : (const void *)(uintptr_t)a[6]);
    replay_draw_captures(a, 7, a[3], a[4], 0, 1);
    break;
  case COP_DRAW_ARRAYS_RAW: __real_glDrawArrays(a[0], (GLint)a[1], (GLsizei)a[2]); break;
  case COP_DRAW_ELEMENTS_RAW: __real_glDrawElements(a[0], (GLsizei)a[1], a[2], (const void *)(uintptr_t)a[3]); break;
  case COP_SWAP: __real_vglSwapBuffers((GLboolean)a[0]); poll_queries(); break;
  case COP_ENABLE: if (a[1]) __real_glEnable(a[0]); else __real_glDisable(a[0]); break;
  case COP_BIND_FB: __real_glBindFramebuffer(a[0], a[1]); break;
  case COP_SCISSOR: __real_glScissor((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
  case COP_VIEWPORT: __real_glViewport((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
  case COP_BEGIN_QUERY: __real_glBeginQuery(a[0], a[1]); break;
  case COP_END_QUERY:
    __real_glEndQuery(a[0]);
    if (a[1] < QUERIES) s_q[a[1]].ended_gen = a[2];
    break;
  case COP_DELETE_PROGRAM: __real_glDeleteProgram(a[0]); break;
  case COP_CREATE_PROGRAM: *ret = __real_glCreateProgram(); break;
  case COP_GEN_QUERIES: __real_glGenQueries((GLsizei)a[0], (GLuint *)(uintptr_t)a[1]); break;
  case COP_IS_PROGRAM: *ret = __real_glIsProgram(a[0]); break;
  case COP_IS_ENABLED: *ret = __real_glIsEnabled(a[0]); break;
  case COP_GET_INTEGERV: __real_glGetIntegerv(a[0], (GLint *)(uintptr_t)a[1]); break;
  case COP_GET_QUERY: __real_glGetQueryObjectuiv(a[0], a[1], (GLuint *)(uintptr_t)a[2]); break;
  case COP_GET_ERROR: *ret = __real_glGetError(); break;
  case COP_GETPROC: *ret = (uint32_t)(uintptr_t)__real_vglGetProcAddress((const char *)(uintptr_t)a[0]); break;
  default: log_printf("[glw] FATAL: unknown custom op 0x%x", (unsigned)op); break;
  }
}

/* ---- worker thread ---------------------------------------------------------------------- */
static int worker_main(SceSize args, void *argp) {
  (void)args; (void)argp;
  glw_ro.worker_key = glw_tls_key();
  log_printf("[glw] worker running (key 0x%08x)", (unsigned)glw_ro.worker_key);
  uint32_t rd = 0, unstored = 0;
  uint64_t run_t0 = 0;                /* start of the current run of commands */
  for (;;) {
    if (rd == P.pub_wr) {
      /* Caught up: store the read index and close the busy run (timing per
       * run, not per command: two timer calls per command cost the worker
       * several ms per frame). */
      if (unstored) { __sync_synchronize(); W.rd = rd; unstored = 0; }
      if (run_t0) { W.busy_us += (uint32_t)(sceKernelGetProcessTimeWide() - run_t0); run_t0 = 0; }
      /* Keep polling for a while before sleeping: while the game thread is
       * rendering, commands arrive every few microseconds, and waking a
       * sleeping worker for each one costs a system call per command on the
       * game thread. This core has nothing else to do. */
      GLW_PROF_WORKER(1);
      for (uint32_t spin = 0; spin < GL_WORKER_SPIN && rd == P.pub_wr; spin++)
        __asm__ volatile("" ::: "memory");
      if (rd == P.pub_wr) {
        W.idle = 1;
        __sync_synchronize();
        GLW_PROF_WORKER(0);
        if (rd == P.pub_wr) sceKernelWaitSema(R.work_sema, 1, NULL);
        W.idle = 0;
      }
      continue;
    }
    __sync_synchronize();
    uint32_t *c = R.ring + rd;
    uint32_t h = c[0];
    if ((h & GLW_OP_MASK) == COP_WRAP) {
      rd = 0;
      W.rd = 0;
      continue;
    }
    GLW_PROF_WORKER(2);
    if (!run_t0) run_t0 = sceKernelGetProcessTimeWide();
    uint32_t ret = 0;
    execute(c, &ret);
    if ((h & GLW_OP_MASK) == COP_SWAP) {
      W.swaps_done++;
      W.busy_us += (uint32_t)(sceKernelGetProcessTimeWide() - run_t0);   /* runs end at a frame, too */
      run_t0 = 0;
    }
    rd += h >> 16;
    /* The read index matters to the producer only when the ring is full:
     * store it every 64 commands, at blocking calls and when caught up. */
    if (++unstored >= 64 || (h & GLW_SYNC)) {
      __sync_synchronize();
      W.rd = rd;
      unstored = 0;
    }
    if (h & GLW_SYNC) {
      W.sync_ret = ret;
      __sync_synchronize();
      sceKernelSignalSema(R.sync_sema, 1);
    }
  }
  return 0;
}

/* ux0:data/kotor2/glw_mode.txt, if present, overrides GL_WORKER_MODE (first
 * character 0, 1 or 2), so one build can run both ways. */
static int mode_override(void) {
  char c = 0;
  SceUID fd = sceIoOpen("ux0:data/kotor2/glw_mode.txt", SCE_O_RDONLY, 0);
  if (fd < 0) return -1;
  int n = sceIoRead(fd, &c, 1);
  sceIoClose(fd);
  return n == 1 && c >= '0' && c <= '2' ? c - '0' : -1;
}

int gl_worker_mode(void) { return glw_ro.mode; }

void gl_worker_init(void) {
  int ov = mode_override();
  glw_ro.mode = ov >= 0 ? ov : GL_WORKER_MODE;
  if (ov >= 0) log_printf("[glw] mode %d from ux0:data/kotor2/glw_mode.txt", ov);
  if (glw_ro.mode == 2) {
    R.ring = malloc(RING_WORDS * 4);
    R.work_sema = sceKernelCreateSema("glw_work", 0, 0, 1 << 20, NULL);
    R.sync_sema = sceKernelCreateSema("glw_sync", 0, 0, 1, NULL);
    SceUID th = R.ring && R.work_sema >= 0 && R.sync_sema >= 0
                    ? sceKernelCreateThread("gl_worker", worker_main, GL_WORKER_PRIORITY, GL_WORKER_STACK, 0,
                                            GL_WORKER_CPU_MASK, NULL)
                    : -1;
    int rc = th >= 0 ? sceKernelStartThread(th, 0, NULL) : th;
#if GL_WORKER_PROFILE
    if (rc >= 0) glw_prof_set_worker(th);
#endif
    if (rc < 0) {
      log_printf("[glw] worker start FAILED (0x%x): running inline", rc);
      glw_ro.mode = 1;
    } else {
      while (!glw_ro.worker_key) sceKernelDelayThread(1000);
    }
  }
  log_printf("[glw] mode %d (%s)", glw_ro.mode, glw_ro.mode == 2 ? "worker thread" : glw_ro.mode == 1 ? "inline codec" : "off");
}

void gl_worker_attach_producer(void) {
  R.producer_key = glw_tls_key();
  log_printf("[glw] producer attached (key 0x%08x)", (unsigned)R.producer_key);
#if GL_WORKER_PROFILE
  glw_prof_start();
#endif
}

/* Every DRAW_FRAME window: stream, sync, capture and pacing figures. */
static void glw_report(void) {
  static uint64_t t0;
  static uint32_t frames;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (!t0) t0 = now;
  frames++;
  if (now - t0 < (uint64_t)DRAW_FRAME_BENCHMARK_INTERVAL_US) return;
  uint32_t f = frames ? frames : 1;
  static uint32_t s_busy_reported;      /* W.busy_us only grows; report the delta */
  uint32_t busy = W.busy_us, busy_d = busy - s_busy_reported;
  char top[200];
  int o = 0;
  top[0] = 0;
  for (int k = 0; k < 4; k++) {
    uint32_t best = 0, bi = 0;
    for (uint32_t i = 0; i < sizeof st_sync_by_op / sizeof st_sync_by_op[0]; i++)
      if (st_sync_by_op[i] > best) { best = st_sync_by_op[i]; bi = i; }
    if (!best) break;
    static const char *const cop[] = {
      "wrap", "vap", "ffpPtr", "attrEnable", "clientState", "bindBuffer", "deleteBuffers", "drawArrays",
      "drawElements", "pixelStore", "useProgram", "swap", "getProc", "drawArraysRaw", "drawElementsRaw",
      "enable", "bindFb", "scissor", "viewport", "beginQuery", "endQuery", "deleteProgram", "createProgram",
      "genQueries", "isProgram(sync)", "isEnabled(sync)", "getIntegerv(sync)", "getQuery(sync)", "getError",
    };
    const char *nm = bi < GLW_OP_CUSTOM_BASE ? glw_op_names[bi]
                     : bi - GLW_OP_CUSTOM_BASE < sizeof cop / sizeof cop[0] ? cop[bi - GLW_OP_CUSTOM_BASE] : "custom";
    o += snprintf(top + o, sizeof top - o, " %s=%u", nm, best / f);
    st_sync_by_op[bi] = 0;
  }
  memset(st_sync_by_op, 0, sizeof st_sync_by_op);
#if GL_WORKER_PROFILE
  glw_prof_report(f, now - t0);
#endif
  if (glw_ro.mode != 0) {
  log_printf("[glw] batch %u dedup %d: wakes %u/frame, skipped %u/frame", (unsigned)s_batch, s_dedup,
             st_wakes / f, st_skipped / f);
  st_wakes = 0;
  st_skipped = 0;
#if GL_WORKER_DEDUP_AB
  s_dedup = !s_dedup;
#endif
#if GL_WORKER_BATCH_AB
  s_batch = s_batch == 1 ? GL_WORKER_BATCH : 1;
#endif
  log_printf("[glw] mode %d, %u frames: %u cmds/frame, %u syncs/frame (%u.%u ms wait), %u answered locally, "
             "capture %u KB/frame, raw draws %u/frame | worker busy %u.%u ms/frame, ring stalls %u.%u ms, pacing "
             "%u.%u ms | sync top:%s",
             glw_ro.mode, f, st_cmds / f, st_syncs / f, st_sync_us / f / 1000u, st_sync_us / f / 100u % 10u,
             st_shadow_hits / f,
             st_capture_bytes / f / 1024u, st_raw_draws / f, busy_d / f / 1000u,
             busy_d / f / 100u % 10u, st_stall_us / f / 1000u, st_stall_us / f / 100u % 10u,
             st_pace_us / f / 1000u, st_pace_us / f / 100u % 10u, top);
  }
  st_cmds = st_syncs = st_sync_us = st_stall_us = st_pace_us = st_capture_bytes = st_raw_draws = 0;
  st_shadow_hits = 0;
  s_busy_reported = busy;
  frames = 0;
  t0 = now;
}
