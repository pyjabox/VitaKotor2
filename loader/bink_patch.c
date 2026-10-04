/* bink_patch.c -- integrate Bink video (see bink_patch.h) */

#include <vitasdk.h>
#include <stdint.h>
#include <string.h>
#include "bink_patch.h"
#include "config.h"
#include "dynlib.h"
#include "opensl_patch.h"
#include "so_util.h"
#include "log.h"

// MacPlayBinkGL(const char *path, bool a, bool &finished, int c):
// report the movie as finished immediately so callers advance past it.
static void MacPlayBinkGL_stub(const char *path, int a, unsigned char *finished, int c) {
  log_printf("[BINK] skip movie: %s", path ? path : "(null)");
  if (finished)
    *finished = 1;
}

#ifdef KOTOR2_BUILD
static void MacPlayBinkGL_stub_k2(const char *path, unsigned char *finished, int arg) {
  (void)arg;
  log_printf("[BINK] skip KOTOR II movie: %s", path ? path : "(null)");
  if (finished) *finished = 1;
}
#endif

static int bink_stub(void) {
  return 0;
}

typedef struct {
  int active;
  unsigned swaps, frame_intervals, texture_uploads;
  uint64_t start_us, last_swap_us, frame_sum_us, frame_max_us, texture_bytes;
} bink_perf_t;

static bink_perf_t g_bink_perf;

#if defined(KOTOR2_BUILD) && \
    (BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY)
/* KOTOR II's native worker invokes +0x160 too early, before decoded input is
 * available, and never wakes it again. Keep the native provider and producer
 * intact: prime it from the movie swap path, then refill it when OpenSL reports
 * a completed buffer. No internal Bink code or callback table is patched. */
#define K2_BINK_PRODUCER_OFF    0x63f04cu
#define K2_BINK_ACTIVE_HEAD_OFF 0xe73accu

static uintptr_t g_k2_bink_base;
static void (*g_k2_bink_producer)(void);
static volatile int g_k2_feed_busy;
static volatile int g_k2_feed_active;
static unsigned g_k2_feed_calls, g_k2_feed_blocks;

static void k2_bink_feed(unsigned max_blocks) {
  if (!g_k2_feed_active || !g_k2_bink_base || !g_k2_bink_producer) return;
  if (__atomic_exchange_n(&g_k2_feed_busy, 1, __ATOMIC_ACQUIRE)) return;

  void *bink = *(void **)(g_k2_bink_base + K2_BINK_ACTIVE_HEAD_OFF);
  void *sound = bink ? *(void **)((char *)bink + 0x184) : NULL;
  if (sound) {
    g_k2_feed_calls++;
    for (unsigned i = 0; i < max_blocks; i++) {
      void *write = *(void **)sound;
      void *read = *(void **)((char *)sound + 0x18);
      if (write == read) break;
      g_k2_bink_producer();
      g_k2_feed_blocks++;
    }
  }
  __atomic_store_n(&g_k2_feed_busy, 0, __ATOMIC_RELEASE);
}

/* Called by the OpenSL shim after Bink's registered completion callback. */
void bink_patch_on_audio_buffer_complete(void) {
  k2_bink_feed(1);
}

static void k2_bink_monitor_on_swap(void) {
  /* A 3072-byte 48 kHz stereo block is 16 ms; a movie frame is ~33 ms.
   * Four attempts quickly prime the five-slot queue. Completion callbacks then
   * maintain it, while this path also recovers from a missed callback. */
  k2_bink_feed(4);
}

static void install_k2_bink_native_feed(so_module *mod) {
  g_k2_bink_base = mod->text_base;
  g_k2_bink_producer = (void *)((mod->text_base + K2_BINK_PRODUCER_OFF) | 1u);
  log_printf("[BINK:K2] native producer feed ready: producer=%p", g_k2_bink_producer);
}

static void k2_bink_native_log_stats(void) {
  log_printf("[BINK:K2] feed calls=%u producer blocks=%u",
             g_k2_feed_calls, g_k2_feed_blocks);
}
#else
/* OpenSL is built in every KOTOR II configuration. Non-audio diagnostic modes
 * still need this symbol but have no native producer to refill. */
void bink_patch_on_audio_buffer_complete(void) {}
#endif

void bink_patch_note_texture_upload(unsigned width, unsigned height) {
  if (!g_bink_perf.active) return;
  g_bink_perf.texture_uploads++;
  g_bink_perf.texture_bytes += (uint64_t)width * height;
}

void bink_patch_on_swap(uint64_t swap_end_us) {
  if (!g_bink_perf.active) return;
  if (g_bink_perf.last_swap_us) {
    uint64_t frame_us = swap_end_us - g_bink_perf.last_swap_us;
    g_bink_perf.frame_sum_us += frame_us;
    if (frame_us > g_bink_perf.frame_max_us) g_bink_perf.frame_max_us = frame_us;
    g_bink_perf.frame_intervals++;
  }
  g_bink_perf.last_swap_us = swap_end_us;
  g_bink_perf.swaps++;
#if defined(KOTOR2_BUILD) && \
    (BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY)
  k2_bink_monitor_on_swap();
#endif
}

void bink_patch_stop_audio_pump(void) {
#if defined(KOTOR2_BUILD) && \
    (BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY)
  __atomic_store_n(&g_k2_feed_active, 0, __ATOMIC_RELEASE);
#endif
}

#if BINK_MODE == BINK_MODE_LEGAL_VIDEO || BINK_MODE == BINK_MODE_OPENSL_TEST || \
    BINK_MODE == BINK_MODE_PLAY
#if BINK_MODE == BINK_MODE_OPENSL_TEST
#define BINK_TEST_MOVIE ".\\movies\\01c.bik"
#define BINK_TEST_NAME  "OpenSL"
#elif BINK_MODE == BINK_MODE_LEGAL_VIDEO
#define BINK_TEST_MOVIE ".\\movies\\legal.bik"
#define BINK_TEST_NAME  "legal"
#endif

#ifdef KOTOR2_BUILD
/* KOTOR II ABI: MacPlayBinkGL(char const*, bool&, int). There is no can-skip
 * argument; r1 is the address of the finished byte and r2 is the integer arg. */
static void (*MacPlayBinkGL_orig)(const char *path, unsigned char *finished,
                                  int arg) = NULL;
#else
static void (*MacPlayBinkGL_orig)(const char *path, int can_skip,
                                  unsigned char *finished, int arg) = NULL;
#endif
#if BINK_MODE != BINK_MODE_PLAY
static int g_bink_test_played = 0;
#endif

#ifdef KOTOR2_BUILD
static void MacPlayBinkGL_real(const char *path, unsigned char *finished, int arg) {
  int can_skip = 1; /* logging only; KOTOR II owns skip policy internally */
#else
static void MacPlayBinkGL_real(const char *path, int can_skip,
                               unsigned char *finished, int arg) {
#endif
#if BINK_MODE != BINK_MODE_PLAY
  if (g_bink_test_played || !MacPlayBinkGL_orig) {
    MacPlayBinkGL_stub(path, can_skip, finished, arg);
    return;
  }
  g_bink_test_played = 1;
  const char *play_path = BINK_TEST_MOVIE;
#else
  if (!MacPlayBinkGL_orig) {
    MacPlayBinkGL_stub(path, can_skip, finished, arg);
    return;
  }
  const char *play_path = path;
#endif

  memset(&g_bink_perf, 0, sizeof g_bink_perf);
  g_bink_perf.active = 1;
  g_bink_perf.start_us = sceKernelGetProcessTimeWide();
#if defined(KOTOR2_BUILD) && \
    (BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY)
  g_k2_feed_calls = 0;
  g_k2_feed_blocks = 0;
#endif
  int open_before = io_open_count();
#if BINK_MODE == BINK_MODE_PLAY
  log_printf("[BINK] playback begin: path=\"%s\" canSkip=%d arg=%d finished=%d",
             play_path ? play_path : "(null)", can_skip, arg,
             finished ? *finished : -1);
#else
  log_printf("[BINK] %s test begin: requested=\"%s\" substitute=\"%s\" "
             "canSkip=%d arg=%d finished=%d",
             BINK_TEST_NAME, path ? path : "(null)", play_path, can_skip, arg,
             finished ? *finished : -1);
#endif
#ifdef KOTOR2_BUILD
#if BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY
  g_k2_feed_calls = 0;
  g_k2_feed_blocks = 0;
  __atomic_store_n(&g_k2_feed_active, 1, __ATOMIC_RELEASE);
#endif
  MacPlayBinkGL_orig(play_path, finished, arg);
#else
  MacPlayBinkGL_orig(play_path, can_skip, finished, arg);
#endif
#ifdef KOTOR2_BUILD
#if BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY
  bink_patch_stop_audio_pump();
#endif
#endif
  g_bink_perf.active = 0;
#if BINK_MODE != BINK_MODE_PLAY
  if (finished) *finished = 1;
#endif
  log_printf("[BINK] playback end: path=\"%s\" elapsed=%u ms swaps=%u "
             "frame avg/max=%u/%u ms lumaUploads=%u/%u KB files=%d->%d finished=%d",
             play_path ? play_path : "(null)",
             (unsigned)((sceKernelGetProcessTimeWide() - g_bink_perf.start_us) / 1000u),
             g_bink_perf.swaps,
             g_bink_perf.frame_intervals ?
               (unsigned)((g_bink_perf.frame_sum_us / g_bink_perf.frame_intervals) / 1000u) : 0,
             (unsigned)(g_bink_perf.frame_max_us / 1000u),
             g_bink_perf.texture_uploads,
             (unsigned)(g_bink_perf.texture_bytes / 1024u),
             open_before, io_open_count(), finished ? *finished : -1);
#if BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY
  bink_opensl_log_stats();
#ifdef KOTOR2_BUILD
  k2_bink_native_log_stats();
#else
  bink_pump_log();
#endif
#endif
  log_flush();
}
#endif

void bink_patch(so_module *port_mod) {
  if (!port_mod)
    return;

#ifdef KOTOR2_BUILD
  uintptr_t play = so_symbol(port_mod, "_Z13MacPlayBinkGLPKcRbi");
#else
  uintptr_t play = so_symbol(port_mod, "_Z13MacPlayBinkGLPKcbRbi");
#endif
  uintptr_t shaders = so_symbol(port_mod, "_Z20MacCreateBinkShadersv");
  uintptr_t shaders_hook = shaders;
#ifdef KOTOR2_BUILD
  // KOTOR II's four-byte MacCreateBinkShaders veneer is immediately followed
  // by BinkEventFilter. The generic eight-byte hook would corrupt that function.
  uintptr_t shaders_impl = so_symbol(port_mod, "Create_Bink_shaders");
  if (shaders_impl) shaders_hook = shaders_impl;
#endif
  const char *mode = BINK_MODE == BINK_MODE_PLAY ? "PLAY" :
                     BINK_MODE == BINK_MODE_OPENSL_TEST ? "OPENSL_TEST" :
                     BINK_MODE == BINK_MODE_LEGAL_VIDEO ? "LEGAL_VIDEO" :
                     BINK_MODE == BINK_MODE_SHADER_TEST ? "SHADER_TEST" : "SKIP";
  log_printf("[BINK] mode=%s play=0x%08x shaders=0x%08x",
             mode, (unsigned)play, (unsigned)shaders);

  if (!play || !shaders_hook) {
    log_printf("[BINK] missing companion export -- forcing available hooks to skip");
#ifdef KOTOR2_BUILD
    hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub_k2);
#else
    hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
#endif
    hook_addr(shaders_hook, (uintptr_t)&bink_stub);
    return;
  }

  // Shader-test mode leaves the real shader initializer enabled while every
  // movie is skipped. The isolated video modes permit exactly one substituted
  // call; production forwards every requested path to the real player.
#if BINK_MODE == BINK_MODE_SKIP
#ifdef KOTOR2_BUILD
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub_k2);
#else
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
#endif
  hook_addr(shaders_hook, (uintptr_t)&bink_stub);
#elif BINK_MODE == BINK_MODE_SHADER_TEST
#ifdef KOTOR2_BUILD
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub_k2);
#else
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
#endif
  log_printf("[BINK] real shader initialization enabled; movie playback still skipped");
#elif BINK_MODE == BINK_MODE_LEGAL_VIDEO || BINK_MODE == BINK_MODE_OPENSL_TEST || \
      BINK_MODE == BINK_MODE_PLAY
  size_t patch_len = thumb_patch_len(play);
  MacPlayBinkGL_orig = (void *)build_thumb_trampoline(play, patch_len);
  if (!MacPlayBinkGL_orig) {
    log_printf("[BINK] player trampoline failed -- all movies will be skipped");
#ifdef KOTOR2_BUILD
    hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub_k2);
#else
    hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
#endif
    return;
  }
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_real);
#if BINK_MODE == BINK_MODE_PLAY
  log_printf("[BINK] normal movie playback enabled: trampoline=%p patchLen=%u",
             (void *)MacPlayBinkGL_orig, (unsigned)patch_len);
#else
  log_printf("[BINK] one-movie test enabled: trampoline=%p patchLen=%u; "
             "first request becomes %s, later requests skipped",
             (void *)MacPlayBinkGL_orig, (unsigned)patch_len, BINK_TEST_MOVIE);
#endif
#if defined(KOTOR2_BUILD) && \
    (BINK_MODE == BINK_MODE_OPENSL_TEST || BINK_MODE == BINK_MODE_PLAY)
  install_k2_bink_native_feed(port_mod);
#endif
#endif

  // DO NOT stub MacDecompress. The `Mac` prefix makes it look like a sibling of
  // MacPlayBinkGL/MacCreateBinkShaders, and bring-up stubbed it on that basis --
  // it is actually the OBB's *resource* decompressor, and stubbing it silently
  // broke every LZMA-compressed asset in the game:
  //   MacDecompress(dest, destLen, src, srcLen)  @ libandroid_port+0x57244
  //     -> LzmaUncompress(dest, &destLen, src+5, &srcLen-5, props=src, 5)
  //     -> returns 1 on success, 0 on failure
  // `data/*.bzf` in the OBB are LZMA-compressed BIFs (5-byte props + stream per
  // entry), so every .bzf resource routes
  // through here. bink_stub returns 0 (= failure) and never touches `dest`, so
  // the resource manager handed the game a correctly-sized, correctly-tagged
  // block still holding newlib free-list bytes (`10 40 40 81 ...`). That is why
  // chargen's models were NULL: IODispatcher::ReadSync rejects the .mdl because
  // byte 0 of that garbage is not the required 0x00. Only assets served from
  // uncompressed RIM/module containers (gui3D_room, mainmenu) survived, which is
 // why the main menu looked fine.
}
