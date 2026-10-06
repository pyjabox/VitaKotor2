/* gl_worker.h -- vitaGL on a dedicated thread (gl_worker.c). */

#ifndef __GL_WORKER_H__
#define __GL_WORKER_H__

#include <stdint.h>

/* main(), before the game thread starts: mode and (mode 2) the worker thread. */
void gl_worker_init(void);
/* The game thread, before its first GL call (vitaGL init). */
void gl_worker_attach_producer(void);
/* The mode in use: 0 off, 1 inline, 2 worker thread. */
int gl_worker_mode(void);
/* Run fn(args, data) on the thread that runs GL, in GL call order: nargs
 * words of args, and bytes of data copied with the call (over the stream's
 * inline limit the caller waits while fn reads data in place). */
typedef void (*glw_fn_t)(const uint32_t *args, const void *data);
void glw_call(glw_fn_t fn, const uint32_t *args, uint32_t nargs, const void *data, uint32_t bytes);

/* GL_WORKER_PROFILE (glw_prof.c): ASLgl draw hooks (after the engine is
 * loaded), the sampler (game thread), the worker's thread id, the report. */
void glw_prof_install(void);
void glw_prof_start(void);
void glw_prof_set_worker(int thid);
void glw_prof_report(unsigned frames, unsigned long long window_us);

#endif
