/* dynlib.h -- default dynamic-symbol resolution table for the KOTOR loader */

#ifndef __DYNLIB_H__
#define __DYNLIB_H__

#include "so_util.h"

// Main resolution table: category (a) libc/libm/C++/pthread wiring + category
// (b) stubs + the vitaGL gap stubs. Audio (FMOD/OpenSLES) lives in audio_patch.
extern so_default_dynlib default_dynlib[];
extern const int default_dynlib_size;   // in bytes, for so_resolve()

// Diagnostic: when nonzero, malloc/calloc/realloc/operator-new log each request.
// Armed by gl_patch.c at init()'s final GL cap-query to trace the hang window.
extern volatile int g_mem_trace;

// Diagnostic: when nonzero, fopen/fseek/ftell/fread log. Armed around OBB mount.
extern volatile int g_io_trace;

// Files currently open, as counted by fopen_diag/fclose_diag. Lets the sound
// probes report the handle count next to each stream event, so stream lifetime
// and handle growth can be correlated from a single log line.
int io_open_count(void);
void io_obb_mount_done(void);

typedef struct {
  unsigned reads, hits, seeks;
  uint64_t card_bytes, card_us;
} io_perf_t;

// Cumulative OBB I/O counters. A frame snapshot can distinguish cache-served
// asset churn from real memory-card reads without tracing every file call.
void io_perf_snapshot(io_perf_t *out);

// Every thread the game creates, recorded by our pthread_create wrapper. `entry`
// is the thread body's address -- feed it to addr2line against libkotor2.so or
// libObbVfs.so to name a blocked worker. The watchdog sweeps this list so a
// stall on a thread OTHER than the main one is visible (see main.c).
#define GAME_THREADS_MAX 24
typedef struct { SceUID thid; uintptr_t entry; } game_thread_t;
extern game_thread_t g_game_threads[GAME_THREADS_MAX];
extern volatile int  g_game_threads_n;

#endif
