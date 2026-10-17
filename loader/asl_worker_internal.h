/* asl_worker_internal.h -- shared by asl_worker.c and the generated aslw_gen.c
 * (tools/gen_aslw.py): ASLgl recorded on the game thread, run on the GL worker. */

#ifndef ASL_WORKER_INTERNAL_H
#define ASL_WORKER_INTERNAL_H

#include "gl_worker_internal.h"

/* Ops in the GL worker's stream: generated ASLgl calls at ASLW_OP_BASE + index
 * in aslw_fns[] (ASLW_OP_BASE: gl_worker_internal.h), asl_worker.c's own at
 * ASLW_OP_CUSTOM and up. */
#define ASLW_OP_CUSTOM 0x1800u
#define ASLW_INLINE_MAX GLW_INLINE_MAX

typedef struct {
  const char *mangled, *name;
  void *gen;          /* the default recorder (aslw_gen_<name>) */
  void **orig;        /* where the original (what the slot held) is kept */
  void *custom;       /* aslw_custom_<name>, if asl_worker.c has one */
} aslw_fn_t;
extern const aslw_fn_t aslw_fns[];
extern const unsigned aslw_nfns;

void aslw_replay_gen(uint32_t idx, int sync, const uint32_t *a, uint32_t *ret);

/* Call ASLgl straight through: not on the game thread with the worker on
 * (the worker replaying, or no worker), or ASLgl back on the game thread
 * (aslw_direct_flag, the A/B's other half). */
extern volatile uint32_t aslw_direct_flag;
#define ASLW_DIRECT() (aslw_direct_flag || glw_direct())

/* ASLgl calls in progress on the game thread's side while ASLgl runs there
 * (the A/B's other half). ASLgl calls itself through its slots -- its
 * immediate mode sets client arrays and draws -- and those inner calls must
 * not reach the copies of the engine's state kept in asl_worker.c. The worker
 * never touches it. */
extern uint32_t aslw_depth;
#define ASLW_CALL(call)                                                     \
  do {                                                                      \
    if (aslw_direct_flag && !glw_direct()) { aslw_depth++; call; aslw_depth--; } \
    else { call; }                                                          \
  } while (0)

/* A pointer argument's size the recorder cannot work out: the call runs on
 * the worker while the game thread waits (reading the memory in place). */
#define ASLW_SIZE_UNKNOWN 0xFFFFFFFFu
uint32_t aslw_pixels_size(uint32_t w, uint32_t h, uint32_t format, uint32_t type);

/* Values a pname takes (the size rules of the generated COPY recorders). */
uint32_t aslw_n_light(uint32_t pname);
uint32_t aslw_n_material(uint32_t pname);
uint32_t aslw_n_lightmodel(uint32_t pname);
uint32_t aslw_n_fog(uint32_t pname);
uint32_t aslw_n_texenv(uint32_t pname);
uint32_t aslw_n_texparam(uint32_t pname);
uint32_t aslw_n_texgen(uint32_t pname);
uint32_t aslw_n_pointparam(uint32_t pname);
uint32_t aslw_type_size(uint32_t type);

#endif
