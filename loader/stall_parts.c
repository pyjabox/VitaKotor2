/* stall_parts.c -- where a slow frame's game-thread time went outside the
 * engine (test builds, STALL_LOG_MS).
 *
 * The PC profiler (pc_prof.c) samples only libkotor2's code. The minimap's
 * first draw and the start of a conversation spent most of their slow frames
 * outside it (e.g. 23 samples in a 428 ms frame): in the loader, libObbVfs,
 * miniz or the kernel. Loader entry points add their game-thread time here;
 * each frame of STALL_LOG_MS or more appends the totals to its [stall] line. */

#include <vitasdk.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "stall_parts.h"
#include "fs_patch.h"

#if STALL_LOG_MS

static struct { uint32_t n, us, kb; } s_part[SP_N];
static uint32_t s_game_tp;                 /* the game thread's TPIDRURO */
static unsigned s_answered0;               /* fs_miss_answered at the frame start */

static inline uint32_t thread_pointer(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}

void stall_part(int part, uint32_t us, uint32_t bytes) {
  if (part < 0 || part >= SP_N || !s_game_tp || thread_pointer() != s_game_tp) return;
  s_part[part].n++;
  s_part[part].us += us;
  s_part[part].kb += bytes / 1024u;
}

void stall_parts_frame(uint64_t frame_us, uint64_t end_us) {
  if (!s_game_tp) s_game_tp = thread_pointer();   /* called on the game thread */
  if (frame_us >= STALL_LOG_MS * 1000u) {
#define P(i) s_part[i].n, s_part[i].us / 1000u
    unsigned answered = fs_miss_answered();
    log_printf("[stall] frame %u ms, end %llu ms | fopen %u/%u ms, fread %u/%u ms %u KB, fseek %u/%u ms, "
               "fclose %u/%u ms, misses answered %u | gl sync %u/%u ms | tex upload %u/%u ms, "
               "16-bit %u/%u ms | mipmaps %u/%u ms | sound create %u/%u ms",
               (unsigned)(frame_us / 1000u), (unsigned long long)(end_us / 1000u), P(SP_OPEN), P(SP_READ),
               s_part[SP_READ].kb, P(SP_SEEK), P(SP_CLOSE), answered - s_answered0, P(SP_GLSYNC),
               P(SP_TEXUP), P(SP_TEX16), P(SP_MIPGEN), P(SP_SND));
#undef P
  }
  memset(s_part, 0, sizeof s_part);
  s_answered0 = fs_miss_answered();
}

#else
void stall_part(int part, uint32_t us, uint32_t bytes) { (void)part; (void)us; (void)bytes; }
void stall_parts_frame(uint64_t frame_us, uint64_t end_us) { (void)frame_us; (void)end_us; }
#endif
