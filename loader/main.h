/* main.h -- shared loader declarations */

#ifndef __MAIN_H__
#define __MAIN_H__

#include "so_util.h"

extern so_module kotor_mod;   // libkotor2.so
extern so_module port_mod;    // libObbVfs.so
extern so_module lzma_mod;    // libLzmaLib.so (LzmaUncompress, used by hints.c)
extern so_module cxx_mod;     // Android libc++_shared.so

int debugPrintf(const char *text, ...);
void fatal_error(const char *fmt, ...);

int ret0(void);
int ret1(void);

typedef struct {
  unsigned game_calls, screen_calls;
  uint64_t game_us, screen_us;
  unsigned policy_seq, selected_skip;
  float selector_ai_ms, next_ai_ms, display_fps;
  int movie_fps;
} engine_perf_t;

void engine_perf_snapshot(engine_perf_t *out, uint64_t now_us);
void engine_perf_presented(void);

/* main.c: re-zero the effect switches the game turned back on (each swap). */
void effects_low_rearm(void);

#endif
