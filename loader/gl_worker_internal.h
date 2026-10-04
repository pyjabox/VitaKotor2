/* gl_worker_internal.h -- shared between gl_worker.c and the generated
 * glw_gen.c (tools/gen_glw.py). */

#ifndef __GL_WORKER_INTERNAL_H__
#define __GL_WORKER_INTERNAL_H__

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vitaGL.h>

#include "config.h"

/* Command header: op (bits 0-13), GLW_SYNC (bit 14), length in words
 * including the header (bits 16-31). Arguments follow, one word each; a
 * DATA call's buffer follows the arguments. */
#define GLW_SYNC 0x4000u
#define GLW_OP_MASK 0x3fffu
#define GLW_OP_CUSTOM_BASE 0x0400u
/* Data copied into the stream per call at most; larger calls run as SYNC
 * (the worker reads the caller's memory while the caller waits). */
#define GLW_INLINE_MAX (128u * 1024u)

/* Cross-thread variables live in cache lines written by one thread only
 * (Cortex-A9 lines are 32 bytes; 64 stays clear of the neighbour). A line that
 * the other core wrote moves across on the next access: on hardware, the worker
 * rewrote the line holding the worker key (read by every wrapper) after each
 * command, so every recorded call missed (real-vita-glw-prof-20261003). */
#define GLW_LINE __attribute__((aligned(64)))
/* Set before the game thread starts, then only read by both threads. */
struct glw_ro {
  int mode;                           /* 0 off, 1 inline codec, 2 worker thread */
  volatile uint32_t worker_key;       /* the worker's TLS key */
  volatile uint32_t in_replay;        /* inline mode: replaying (game thread only) */
} GLW_LINE;
extern struct glw_ro glw_ro;

static inline uint32_t glw_tls_key(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}
/* Call vitaGL directly: worker off, or we are the worker (replay, and vitaGL's
 * own calls between its objects, which --wrap also routes here). */
static inline int glw_direct(void) {
  return glw_ro.mode == 0 || glw_ro.in_replay || glw_tls_key() == glw_ro.worker_key;
}
/* GL_WORKER_PROFILE (glw_prof.c): what the game thread is doing, read by a
 * sampler thread. GLW_PROF(k) marks the rest of the enclosing block as k:
 * 1 a GL call, 2 a draw, 3 waiting for a blocking call, 4 the swap (pacing).
 * The worker's own calls leave the marker alone. */
#if GL_WORKER_PROFILE
typedef struct { volatile uint8_t v; } GLW_LINE glw_mark_t;   /* one line each */
extern glw_mark_t glw_prof_inner, glw_prof_worker;
static inline uint8_t glw_prof_enter(uint8_t k) {
  if (glw_tls_key() == glw_ro.worker_key) return 0xff;
  uint8_t saved = glw_prof_inner.v;
  glw_prof_inner.v = k;
  return saved;
}
static inline void glw_prof_leave(uint8_t *saved) { if (*saved != 0xff) glw_prof_inner.v = *saved; }
#define GLW_PROF(k) uint8_t glw_prof_saved_ __attribute__((cleanup(glw_prof_leave), unused)) = glw_prof_enter(k)
#define GLW_PROF_WORKER(x) (glw_prof_worker.v = (x))
#else
#define GLW_PROF(k) ((void)0)
#define GLW_PROF_WORKER(v) ((void)0)
#endif

static inline float glw_f(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }
static inline uint32_t glw_u(float f) { union { uint32_t u; float f; } x; x.f = f; return x.u; }

/* Producer side: begin a command with nargs argument words and data_bytes of
 * inline data; returns the argument area (data starts at a + nargs). */
uint32_t *glw_begin(uint32_t op, uint32_t nargs, uint32_t data_bytes);
void glw_end(uint32_t *a);           /* publish (or run, inline mode) */
uint32_t glw_sync(uint32_t *a);      /* publish, wait, return the result */
uint32_t glw_pixels_size(int w, int h, unsigned format, unsigned type);
/* 1 if (op, v[0..n)) repeats the last call of its state group: skip it. */
int glw_dedup(uint32_t group, uint32_t op, const uint32_t *v, uint32_t n);

/* Generated: replay one generated op; 0 if op is not a generated one. */
int glw_replay_gen(uint32_t op, int sync, const uint32_t *a, uint32_t *ret);
typedef struct { const char *name; void *fn; } glw_proc_t;
extern const glw_proc_t glw_procs[];
extern const char *const glw_op_names[];

#endif
