/* dynlib.c -- default_dynlib[] resolution table
 * Historical diagnostic narratives in this file are inherited from VitaKotor
 * (KOTOR I) unless explicitly marked as KOTOR II.
 *
 * Imports are either wired to VitaSDK libc/libm/libstdc++/pthread or handled by
 * narrow compatibility stubs. Audio, OpenSL, Bink, and OpenGL are supplied by
 * their dedicated loader modules.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <wchar.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <utime.h>
#include <wctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <fnmatch.h>
#include <libgen.h>
#include <locale.h>
#include <malloc.h>
#include <setjmp.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/times.h>
#include <zlib.h>

#include "dynlib.h"
#include "fs_patch.h"
#include "log.h"
#include "stall_parts.h"
#include "autotest.h"
#include "ramfile.h"
#include "obb_cache.h"
#include "heap.h"
#include "main.h"
#include "config.h"
#include "obb_index.h"
#include "bigalloc.h"
#include "loadscreen.h"
#include "audio_patch.h"
#include "sdl_patch.h"
#include "gl_patch.h"

// ---- generic stubs (category b) ------------------------------------------
int ret0(void) { return 0; }
int ret1(void) { return 1; }

// libminiz/libLzmaLib were compiled against bionic, where `stderr` is
// `&__sF[2]`. Provide a dummy array so that address computes to valid storage,
// and a safe fprintf that ignores the (bionic-layout, unusable) stream and
// routes the message to our log -- miniz only fprintf()s on error paths, but a
// real fprintf on a bogus FILE* would fault.
char __sF[0x400];
static int fprintf_safe(void *stream, const char *fmt, ...) {
  (void)stream;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_printf("[stdio] %s", buf);
  return n;
}

// ---- stack protector (category a) -----------------------------------------
// libKOTOR is built with -fstack-protector: every protected function loads the
// canary from the __stack_chk_guard OBJECT and calls __stack_chk_fail on a
// mismatch. __stack_chk_guard must therefore resolve to REAL storage holding a
// canary value -- if we leave its GOT slot 0 (unresolved), the first protected
// function to run (libKOTOR's very first static constructor) does
// `ldr rX,[__stack_chk_guard]` on a null pointer and data-aborts before main's
// game code ever runs. (Matches gtasa_vita's loader.)
static uintptr_t __stack_chk_guard_fake = 0x42424242;
static void __stack_chk_fail_fake(void) {
  fatal_error("stack smashing detected (__stack_chk_fail)");
}

static int __android_log_print_impl(int prio, const char *tag, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_printf("[android_log:%s] %s", tag ? tag : "?", buf);
  return 0;
}

static int __android_log_write_impl(int prio, const char *tag, const char *text) {
  log_printf("[android_log:%s] %s", tag ? tag : "?", text ? text : "");
  return 0;
}

// Bionic emits these fortified entry points even when the source operation is
// ordinary libc. Keep the bounds checks, then forward to newlib.
static void *__memcpy_chk_impl(void *dst, const void *src, size_t n, size_t cap) {
  if (n > cap) fatal_error("__memcpy_chk overflow: %u > %u", (unsigned)n, (unsigned)cap);
  return memcpy(dst, src, n);
}
static void *__memset_chk_impl(void *dst, int c, size_t n, size_t cap) {
  if (n > cap) fatal_error("__memset_chk overflow: %u > %u", (unsigned)n, (unsigned)cap);
  return memset(dst, c, n);
}
static char *__strcpy_chk_impl(char *dst, const char *src, size_t cap) {
  size_t n = strlen(src) + 1;
  if (n > cap) fatal_error("__strcpy_chk overflow: %u > %u", (unsigned)n, (unsigned)cap);
  return memcpy(dst, src, n);
}
static char *__strcat_chk_impl(char *dst, const char *src, size_t cap) {
  size_t a = strlen(dst), b = strlen(src) + 1;
  if (a + b > cap) fatal_error("__strcat_chk overflow: %u > %u", (unsigned)(a + b), (unsigned)cap);
  memcpy(dst + a, src, b);
  return dst;
}
static size_t __strlen_chk_impl(const char *s, size_t cap) {
  size_t n = strnlen(s, cap);
  if (n == cap) fatal_error("__strlen_chk unterminated string");
  return n;
}
static char *__strchr_chk_impl(const char *s, int c, size_t cap) {
  (void)cap;
  return strchr(s, c);
}
static char *__strrchr_chk_impl(const char *s, int c, size_t cap) {
  (void)cap;
  return strrchr(s, c);
}
static char *__strncpy_chk_impl(char *dst, const char *src, size_t n, size_t cap) {
  if (n > cap) fatal_error("__strncpy_chk overflow: %u > %u", (unsigned)n, (unsigned)cap);
  return strncpy(dst, src, n);
}
static char *__strncpy_chk2_impl(char *dst, const char *src, size_t n,
                                 size_t dst_cap, size_t src_cap) {
  if (n > dst_cap || strnlen(src, src_cap) == src_cap)
    fatal_error("__strncpy_chk2 overflow");
  return strncpy(dst, src, n);
}
static size_t __strlcpy_chk_impl(char *dst, const char *src, size_t n, size_t cap) {
  if (n > cap) fatal_error("__strlcpy_chk overflow: %u > %u", (unsigned)n, (unsigned)cap);
  size_t len = strlen(src);
  if (n) {
    size_t copy = len < n - 1 ? len : n - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
  }
  return len;
}
static int __vsnprintf_chk_impl(char *dst, size_t n, int flags, size_t cap,
                                const char *fmt, va_list ap) {
  (void)flags;
  if (n > cap) fatal_error("__vsnprintf_chk overflow: %u > %u", (unsigned)n, (unsigned)cap);
  return vsnprintf(dst, n, fmt, ap);
}
static int __vsprintf_chk_impl(char *dst, int flags, size_t cap,
                               const char *fmt, va_list ap) {
  (void)flags;
  return vsnprintf(dst, cap, fmt, ap);
}
static int __gnu_strerror_r_impl(int err, char *buf, size_t len) {
  const char *s = strerror(err);
  if (len) snprintf(buf, len, "%s", s ? s : "unknown error");
  return 0;
}
static int __cxa_thread_atexit_impl_stub(void (*dtor)(void *), void *obj,
                                         void *dso) {
  (void)dtor;
  (void)obj;
  (void)dso;
  return 0;
}
static void *__dlopen_stub(const char *name, int flags) {
  (void)flags;
  log_printf("[K2] dlopen unsupported: %s", name ? name : "(null)");
  return NULL;
}
static void *__dlsym_stub(void *handle, const char *name) {
  (void)handle;
  if (!name) return NULL;
  uintptr_t value = fs_lookup_symbol(name);
  if (!value) value = sdl_lookup_symbol(name);
  if (!value) value = gl_lookup_symbol(name);
  if (!value) value = audio_lookup_symbol(name);
  if (!value) {
    for (unsigned i = 0; i * sizeof(default_dynlib[0]) < (unsigned)default_dynlib_size; i++)
      if (!strcmp(default_dynlib[i].symbol, name)) {
        value = default_dynlib[i].func;
        break;
      }
  }
  if (value) {
    log_printf("[K2] dlsym resolved: %s -> %p", name, (void *)value);
    return (void *)value;
  }
  log_printf("[K2] dlsym unsupported: %s", name);
  return NULL;
}
static int fnmatch_basic(const char *pattern, const char *text, int flags) {
  (void)flags;
  while (*pattern) {
    if (*pattern == '*') {
      while (*pattern == '*') pattern++;
      if (!*pattern) return 0;
      do {
        if (!fnmatch_basic(pattern, text, flags)) return 0;
      } while (*text++);
      return 1;
    }
    if (*pattern != '?' && *pattern != *text) return 1;
    if (!*text) return 1;
    pattern++;
    text++;
  }
  return *text ? 1 : 0;
}

// ---- bionic static-initializer bridge (pthread ABI) -----------------------
// Both ABIs store a pointer-sized handle, but the STATIC initializer differs:
//   bionic  PTHREAD_MUTEX_INITIALIZER / COND_INITIALIZER == 0
//   vitasdk PTHREAD_*_INITIALIZER                        == (T)-1 (0xFFFFFFFF)
// vitasdk lazily inits when it sees its own -1 sentinel; a bionic-static object
// holds 0, which vitasdk rejects -> lock/wait fails. (This is exactly what made
// bionic's __cxa_guard_acquire abort with "failed to acquire mutex" the moment a
// C++ static-local guard ran.) Dynamically-init'd objects already hold a real
// heap pointer and pass through untouched. Translate 0 -> -1 on first use.
static inline void b2v(void *p) { if (p && *(intptr_t *)p == 0) *(intptr_t *)p = -1; }

static int pth_mutex_lock(pthread_mutex_t *m)    { b2v(m); return pthread_mutex_lock(m); }
static int pth_mutex_trylock(pthread_mutex_t *m) { b2v(m); return pthread_mutex_trylock(m); }
static int pth_mutex_unlock(pthread_mutex_t *m)  { b2v(m); return pthread_mutex_unlock(m); }
static int pth_mutex_destroy(pthread_mutex_t *m) {
  if (m && *(intptr_t *)m == 0) return 0;   // never-locked static mutex: nothing to free
  return pthread_mutex_destroy(m);
}
static int pth_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)      { b2v(c); b2v(m); return pthread_cond_wait(c, m); }
static int pth_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { b2v(c); b2v(m); return pthread_cond_timedwait(c, m, t); }
static int pth_cond_signal(pthread_cond_t *c)    { b2v(c); return pthread_cond_signal(c); }
static int pth_cond_broadcast(pthread_cond_t *c) { b2v(c); return pthread_cond_broadcast(c); }
static int pth_cond_destroy(pthread_cond_t *c)   {
  if (c && *(intptr_t *)c == 0) return 0;
  return pthread_cond_destroy(c);
}

// Android extension: forward to the standard monotonic-ish wait.
static int pthread_cond_timedwait_monotonic_np_impl(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) {
  if (!t) return EINVAL;
  b2v(c); b2v(m);

  /* Android's extension takes an absolute CLOCK_MONOTONIC deadline, while
   * VitaSDK's pthread_cond_timedwait takes CLOCK_REALTIME. KOTOR II's Bink
   * workers use this path for their asynchronous decode queues; passing the
   * monotonic timestamp through makes every deadline look like January 1970. */
  struct timespec mono_now, real_now, remaining, realtime_deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &mono_now) ||
      clock_gettime(CLOCK_REALTIME, &real_now))
    return EINVAL;

  remaining.tv_sec = t->tv_sec - mono_now.tv_sec;
  remaining.tv_nsec = t->tv_nsec - mono_now.tv_nsec;
  if (remaining.tv_nsec < 0) {
    remaining.tv_sec--;
    remaining.tv_nsec += 1000000000L;
  }
  if (remaining.tv_sec < 0) return ETIMEDOUT;

  realtime_deadline.tv_sec = real_now.tv_sec + remaining.tv_sec;
  realtime_deadline.tv_nsec = real_now.tv_nsec + remaining.tv_nsec;
  if (realtime_deadline.tv_nsec >= 1000000000L) {
    realtime_deadline.tv_sec++;
    realtime_deadline.tv_nsec -= 1000000000L;
  }
  return pthread_cond_timedwait(c, m, &realtime_deadline);
}

// ---- memory helpers (wrap sceClib for speed, like gtasa) ------------------
static void *sceClibMemclr(void *dst, SceSize len) { return sceClibMemset(dst, 0, len); }
static void *sceClibMemset2(void *dst, SceSize len, int ch) { return sceClibMemset(dst, ch, len); }

// ---- softfp libm bridge ---------------------------------------------------
// The Android .so is armeabi-v7a **softfp**: floats travel in core registers
// (r0..), doubles in even-aligned core register PAIRS (r0:r1, r2:r3), and both
// are returned the same way. This SDK is hardfp -- s0/d0. Binding libm straight
// through therefore hands every math call a garbage argument and reads a garbage
// result back out of the wrong register.
//
// This was not theoretical. CSWGuiObject::ScaleExtentForResolution(float) scales
// a widget's WIDTH and HEIGHT via `vmov r0,s0 ; blx ceilf ; mov r6,r0` while
// scaling LEFT/TOP inline with NEON and no call at all. So x/y survived and w/h
// came back garbage -> every widget got extent.w == 0 -> CSWGuiImage::Draw and
// CSWGuiBorder::Draw both early-out on `w==0 || h==0` -> no GUI widget ever drew
// on any screen.
//
// Reinterpreting through uint32_t/uint64_t is exactly right: AAPCS places a
// 32-bit integer in r0 and a 64-bit integer in an even-aligned core pair, which
// is precisely where softfp expects a float and a double.
//
// Not affected, deliberately left alone: variadic functions (printf/scanf
// family). AAPCS passes variadic arguments in core registers under BOTH ABIs.
static inline float    u2f(uint32_t u) { float f;    memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f)    { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double   u2d(uint64_t u) { double d;   memcpy(&d, &u, 8); return d; }
static inline uint64_t d2u(double d)   { uint64_t u; memcpy(&u, &d, 8); return u; }

#define SFP_F1(n) static uint32_t sfp_##n(uint32_t a)             { return f2u(n(u2f(a))); }
#define SFP_F2(n) static uint32_t sfp_##n(uint32_t a, uint32_t b) { return f2u(n(u2f(a), u2f(b))); }
#define SFP_D1(n) static uint64_t sfp_##n(uint64_t a)             { return d2u(n(u2d(a))); }
#define SFP_D2(n) static uint64_t sfp_##n(uint64_t a, uint64_t b) { return d2u(n(u2d(a), u2d(b))); }

SFP_D1(sin)   SFP_D1(cos)   SFP_D1(tan)
SFP_D1(asin)  SFP_D1(acos)  SFP_D1(atan) SFP_D2(atan2)
SFP_D1(exp)   SFP_D1(log)   SFP_D1(log10)
SFP_D2(pow)   SFP_D1(sqrt)  SFP_D1(ceil) SFP_D1(floor)
SFP_D1(fabs)  SFP_D2(fmod)
SFP_D2(fmin)

SFP_F1(sinf)  SFP_F1(cosf)  SFP_F1(tanf)
SFP_F1(asinf) SFP_F1(acosf) SFP_F1(atanf) SFP_F2(atan2f)
SFP_F1(expf)  SFP_F1(logf)  SFP_F2(powf)  SFP_F1(sqrtf)
SFP_F1(fabsf) SFP_F1(floorf) SFP_F1(ceilf) SFP_F2(fmodf)
SFP_F2(fmaxf) SFP_F2(fminf)

// Mixed signatures: the non-float arguments already sit in the right registers.
static uint64_t sfp_ldexp(uint64_t a, int e)       { return d2u(ldexp(u2d(a), e)); }
static uint32_t sfp_ldexpf(uint32_t a, int e)       { return f2u(ldexpf(u2f(a), e)); }
static uint64_t sfp_frexp(uint64_t a, int *e)      { return d2u(frexp(u2d(a), e)); }
static uint64_t sfp_modf(uint64_t a, double *ip)   { return d2u(modf(u2d(a), ip)); }
// Double-returning conversions are broken the same way (result in d0, not r0:r1).
static uint64_t sfp_atof(const char *s)                    { return d2u(atof(s)); }
static uint64_t sfp_strtod(const char *s, char **end)      { return d2u(strtod(s, end)); }

// ---- compiler / C++ runtime symbols (resolved from libgcc / libstdc++) ----
extern void *__aeabi_d2ulz, *__aeabi_dsub, *__aeabi_idiv, *__aeabi_idivmod, *__aeabi_l2d, *__aeabi_l2f;
extern void *__aeabi_ldivmod, *__aeabi_llsl, *__aeabi_llsr, *__aeabi_ui2d;
extern void *__aeabi_uidiv, *__aeabi_uidivmod, *__aeabi_ul2d, *__aeabi_ul2f, *__aeabi_uldivmod;
extern void *_Znwj, *_Znaj, *_ZdlPv, *_ZdaPv;
extern void *__cxa_atexit, *__cxa_finalize, *__cxa_guard_acquire, *__cxa_guard_release;
extern void *__cxa_begin_catch, *__cxa_end_catch, *__cxa_throw, *__cxa_rethrow;
extern void *__cxa_allocate_exception, *__cxa_free_exception, *__cxa_pure_virtual;
extern void *__gxx_personality_v0, *__dynamic_cast;
extern void *_ZSt9terminatev, *_ZSt17__throw_bad_allocv;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
// std::exception/bad_cast dtors + typeinfos (present in this libstdc++)
extern void *_ZNSt8bad_castD1Ev, *_ZNSt9exceptionD2Ev;
extern void *_ZTIi, *_ZTISt8bad_cast, *_ZTISt9exception;

// ---- allocation tracing (diagnostic) --------------------------------------
// Armed (by gl_patch.c) only once init()'s final GL cap-query runs, so we don't
// drown in the thousands of CRT/SDL/vitaGL allocations that precede it. Each
// heap request in the hang window is logged with size BEFORE the call, so a
// garbage size that makes the allocator spin still leaves a breadcrumb. The
// companion's OpenGLESState::init is allocation-heavy; the last logged size
// localises a hang there.
volatile int g_mem_trace = 0;
static volatile int g_mem_seq = 0;
static void *(*real_Znwj)(size_t) = (void *(*)(size_t)) & _Znwj;
static void *(*real_Znaj)(size_t) = (void *(*)(size_t)) & _Znaj;
static void (*real_ZdlPv)(void *) = (void (*)(void *)) & _ZdlPv;
static void (*real_ZdaPv)(void *) = (void (*)(void *)) & _ZdaPv;

// Logging every allocation is ~20ms each and turns instant init into a crawl.
// Throttle to a heartbeat (every 1024th) plus any unusually large request, so
// the game runs near full speed: a FINITE init reaches the next phase quickly
// (glCreateShader or a real hang) with a low mem# total, while an UNBOUNDED
// alloc loop shows mem# climbing without limit.
#define MEM_TRACE(tag, size) do {                                        \
    if (g_mem_trace) {                                                   \
      int s = g_mem_seq++;                                               \
      if ((s & 1023) == 0 || (size_t)(size) > (1u << 20))                    \
        log_printf("[mem#%d] " tag "(%u)", s, (unsigned)(size));         \
    }                                                                    \
  } while (0)

// log72 stalls with the game thread HEALTHY: it keeps rendering the module
// loading screen at ~40fps (frame 15480, draw counts climbing) while all resource
// I/O stops dead at t=665s and never resumes. So the thread that is stuck is not
// the one the watchdog samples -- KOTOR loads a module on a worker while the main
// thread animates the loading screen. sceKernelGetThreadmgrUidList is not exposed
// in the userland SDK, so instead we record every thread the game creates here,
// keeping its ENTRY POINT: that is an address addr2line can resolve, which turns
// "some thread is blocked" into "this named function is blocked on this sync
// object".
game_thread_t g_game_threads[GAME_THREADS_MAX];
volatile int g_game_threads_n = 0;

typedef struct { void *(*fn)(void *); void *arg; int slot; } thread_shim_t;

static void *pthread_thread_shim(void *p) {
  thread_shim_t *s = (thread_shim_t *)p;
  void *(*fn)(void *) = s->fn;
  void *arg = s->arg;
  int slot = s->slot;
  free(s);
  // The UID is only knowable from inside the new thread; publish it before the
  // body runs so a thread that blocks immediately is still identifiable.
  if (slot >= 0 && slot < GAME_THREADS_MAX)
    g_game_threads[slot].thid = sceKernelGetThreadId();
  log_printf("[thread] started thid=0x%08x entry=%p", (unsigned)sceKernelGetThreadId(),
             (void *)g_game_threads[slot >= 0 ? slot : 0].entry);
  return fn(arg);
}

static int pthread_create_diag(pthread_t *t, const pthread_attr_t *a,
                               void *(*fn)(void *), void *arg) {
  int slot = -1;
  if (g_game_threads_n < GAME_THREADS_MAX) {
    slot = g_game_threads_n++;
    g_game_threads[slot].thid  = -1;
    g_game_threads[slot].entry = (uintptr_t)fn;
  }
  thread_shim_t *s = (thread_shim_t *)malloc(sizeof(*s));
  if (!s)                       // never fail the game's thread on our account
    return pthread_create(t, a, fn, arg);
  s->fn = fn; s->arg = arg; s->slot = slot;
  int r = pthread_create(t, a, &pthread_thread_shim, s);
  log_printf("[thread] pthread_create entry=%p -> rc=%d slot=%d", (void *)fn, r, slot);
  if (r != 0)
    free(s);
  return r;
}

// usleep is in the game's imports; log the caller's return address so a sleep-
// poll hang is pinned by addr2line. Throttled like SDL_Delay_hook.
static int usleep_diag(useconds_t us) {
  static volatile int n = 0;
  int c = n++;
  if (c < 4 || (c & 1023) == 0)
    log_printf("[sleep] usleep(%u) #%d LR=%p", (unsigned)us, c, __builtin_return_address(0));
  return usleep(us);
}

// File-I/O tracing for the OBB mount: miniz (mz_zip_reader_init_file) opens the
// .obb with fopen and reads its central directory. Armed (g_io_trace) only
// around the mount so we see whether fopen succeeds and where the zip read fails.
volatile int g_io_trace = 0;
/* Open/close accounting. The run freezes once fopen starts returning NULL, and
 * the failures land in whatever opens a file next (model load, shaders) rather
 * than at the culprit. Counting opens against closes says definitively whether
 * something leaks or the game just legitimately holds a lot open at once. */
static volatile int g_files_open = 0, g_files_peak = 0, g_opens = 0;

/* log112 settled that nothing LEAKS -- 1088 opened, ~50 concurrent, balanced.
 * The process simply hits a ceiling around 53 open files, and then every later
 * fopen returns NULL: the game limps on with a half-built object until it
 * passes a NULL to a "%s" and dies in strlen. To fix that we need to know WHO
 * holds the handles, so keep the paths and dump them the moment one fails. */
#define IO_TRACK 64
static struct { FILE *f; char path[72]; } g_openfiles[IO_TRACK];

static void io_track_add(FILE *f, const char *path) {
  for (int i = 0; i < IO_TRACK; i++)
    if (!g_openfiles[i].f) {
      g_openfiles[i].f = f;
      snprintf(g_openfiles[i].path, sizeof g_openfiles[i].path, "%s", path ? path : "?");
      return;
    }
}
static void io_track_del(FILE *f) {
  for (int i = 0; i < IO_TRACK; i++)
    if (g_openfiles[i].f == f) { g_openfiles[i].f = NULL; return; }
}
static void io_dump_open(void) {
  log_printf("[io] ---- %d files currently open ----", g_files_open);
  for (int i = 0; i < IO_TRACK; i++) {
    if (!g_openfiles[i].f) continue;
    int n = 1;                                  /* collapse duplicates of a path */
    for (int j = i + 1; j < IO_TRACK; j++)
      if (g_openfiles[j].f && !strcmp(g_openfiles[j].path, g_openfiles[i].path)) n++;
    int first = 1;
    for (int j = 0; j < i; j++)
      if (g_openfiles[j].f && !strcmp(g_openfiles[j].path, g_openfiles[i].path)) first = 0;
    if (first) log_printf("[io]   x%-3d %s", n, g_openfiles[i].path);
  }
}

/* ---- shared .obb descriptors ----------------------------------------------
 * Inherited KOTOR I finding: nearly every open file was the same main OBB. The
 * companion opens the archive afresh for every OBB-backed RWops and holds them
 * all, so the process hits its ~56-handle ceiling and every later fopen returns
 * NULL -- which is the freeze, the strlen(NULL) crash, AND the missing sounds
 * (streams whose handle died mid-read).
 *
 * Fix: hand out VIRTUAL handles for .obb opens. All of them share ONE real FILE*
 * and each keeps its own file position; reads seek-then-read under a mutex so
 * interleaved readers cannot disturb each other. 54 handles collapse to 1.
 *
 * Safe because we bind EVERY stdio entry point the .so can reach, so nothing
 * dereferences a virtual handle without passing through the guards below. Each
 * one checks vf_of() first. */
#define VF_MAX 160
#define SF_MAX 4

/* rpos = where the real FILE* is believed to sit. Seeking a newlib FILE*
 * DISCARDS its read buffer, so seeking before every read turned miniz's
 * buffered sequential scan of ~27k zip entries into unbuffered card reads --
 * that is what made attempt #1 crawl (main thread had 2.7M runClk at t=660s
 * versus 135M by t=420 in a healthy run). Only seek when actually needed. */
/* fd replaces the old FILE*: sceIoPread is one syscall against an absolute
 * offset, where fseek+fread is two plus a newlib buffer that the seek discards
 * anyway. rpos is kept only for the seek-ratio diagnostic now. */
typedef struct {
  int refs; SceUID fd; long size; long rpos; char path[72]; ObbIndex *idx;
} SharedFile;
typedef struct { int used; SharedFile *sf; long pos; int eof, err; } VFile;

static SharedFile g_sf[SF_MAX];
static VFile      g_vf[VF_MAX];
static SceUID     g_io_mutex = -1;
static unsigned   g_vreads = 0, g_vseeks = 0, g_vhits = 0;
static uint64_t   g_vread_bytes = 0, g_vread_us = 0;
static int        g_vlive = 0;

static void io_lock(void) {
  if (g_io_mutex < 0) g_io_mutex = sceKernelCreateMutex("kotor_io", 0, 0, NULL);
  if (g_io_mutex >= 0) sceKernelLockMutex(g_io_mutex, 1, NULL);
}
static void io_unlock(void) {
  if (g_io_mutex >= 0) sceKernelUnlockMutex(g_io_mutex, 1);
}

/* Non-NULL only for pointers that are really one of our virtual handles. */
static VFile *vf_of(FILE *f) {
  if ((void *)f >= (void *)g_vf && (void *)f < (void *)(g_vf + VF_MAX)) {
    VFile *v = (VFile *)f;
    return v->used ? v : NULL;
  }
  return NULL;
}

/* Escape hatch. Three attempts at this have now broken boot in different ways,
 * so make it disable-able WITHOUT reinstalling: create an empty file
 *     ux0:data/kotor2/no_obb_share
 * and every .obb open goes back to a plain, private FILE* (the old behaviour).
 * Checked once, lazily, then cached. */
static int share_disabled(void) {
  static int cached = -1;
  if (cached < 0) {
    SceUID fd = sceIoOpen(DATA_PATH "/no_obb_share", SCE_O_RDONLY, 0);
    cached = (fd >= 0);
    if (fd >= 0) sceIoClose(fd);
    log_printf("[io] .obb handle sharing %s%s", cached ? "DISABLED" : "enabled",
               cached ? " (no_obb_share present)" : "");
  }
  return cached;
}

static int share_this(const char *path, const char *mode) {
  if (share_disabled()) return 0;
  if (!path || !mode) return 0;
  if (mode[0] != 'r' || strchr(mode, '+')) return 0;    /* read-only opens only */
  size_t n = strlen(path);
  return n > 4 && !strcasecmp(path + n - 4, ".obb");
}

static FILE *fopen_shared(const char *path, const char *mode) {
  io_lock();
  SharedFile *sf = NULL;
  for (int i = 0; i < SF_MAX; i++)
    if (g_sf[i].fd && !strcmp(g_sf[i].path, path)) { sf = &g_sf[i]; break; }

  if (!sf) {
    for (int i = 0; i < SF_MAX; i++)
      if (!g_sf[i].fd) { sf = &g_sf[i]; break; }
    if (!sf) { io_unlock(); return NULL; }
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { io_unlock(); return NULL; }
    sf->fd   = fd;
    sf->size = (long)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    sf->rpos = 0;
    sf->refs = 0;
    snprintf(sf->path, sizeof sf->path, "%s", path);
    sf->idx  = obbidx_open(path, sf->size);
    g_files_open++;
    log_printf("[io] shared archive opened: %s (%ld bytes) -- further opens "
               "share this one handle", path, sf->size);
  }

  VFile *v = NULL;
  for (int i = 0; i < VF_MAX; i++)
    if (!g_vf[i].used) { v = &g_vf[i]; break; }
  if (!v) { io_unlock(); log_printf("[io] out of virtual handles for %s", path); return NULL; }

  memset(v, 0, sizeof *v);
  v->used = 1; v->sf = sf; v->pos = 0;
  sf->refs++;
  g_vlive++;
  io_unlock();
  return (FILE *)v;
}

int io_open_count(void) { return g_files_open; }

void io_perf_snapshot(io_perf_t *out) {
  if (!out) return;
  io_lock();
  out->reads = g_vreads;
  out->hits = g_vhits;
  out->seeks = g_vseeks;
  out->card_bytes = g_vread_bytes;
  out->card_us = g_vread_us;
  io_unlock();
}

/* Called once the archives are mounted. Stops recording and writes each cache
 * out; recording every read for the whole session would grow without bound. */
void io_obb_mount_done(void) {
  io_lock();
  for (int i = 0; i < SF_MAX; i++)
    if (g_sf[i].fd && g_sf[i].idx) obbidx_finish(g_sf[i].idx);
  io_unlock();
  log_printf("[obbidx] mount complete: %u card reads, %u served from the replay index, %d handles live",
             g_vreads, g_vhits, g_vlive);
}

/* sysinfo for libkotor2's GlobalMemoryStatus. It used to be ret0, which left the
 * caller's struct as stack garbage; CExoResMan's constructor sizes the resource
 * cache from it (budget = dwTotalPhys / 2 above 32 MB, else 16 MB), so the cache
 * size was whatever the stack held -- and combat scripts were re-read from the
 * card on every run. Report a deliberate total instead: 2 x RESMAN_BUDGET_MB.
 * Layout is bionic's 32-bit struct sysinfo, as GlobalMemoryStatus reads it
 * (totalram +16, freeram +20, totalswap +32, freeswap +36, mem_unit +52). */
struct sysinfo_bionic {
  long uptime;
  unsigned long loads[3];
  unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  unsigned short procs, pad;
  unsigned long totalhigh, freehigh;
  unsigned int mem_unit;
  char reserved[8];
};
_Static_assert(sizeof(struct sysinfo_bionic) == 64, "bionic struct sysinfo is 64 bytes");
static int sysinfo_vita(struct sysinfo_bionic *si) {
  if (!si) return -1;
  memset(si, 0, sizeof *si);
#if RESMAN_BUDGET_MB
  si->totalram = (unsigned long)RESMAN_BUDGET_MB * 2u * 1024u * 1024u + 2u;
  si->freeram = si->totalram / 2u;
#endif
  si->mem_unit = 1;
  si->uptime = (long)(sceKernelGetProcessTimeWide() / 1000000u);
  return 0;
}

static FILE *fopen_diag(const char *path, const char *mode) {
  autotest_note_open(path);
  if (share_this(path, mode)) {
    FILE *v = fopen_shared(path, mode);
    if (v) return v;                     /* fall through to a real open on failure */
  }
  /* The talk table: a stream over a RAM copy (ramfile.c). */
  FILE *rf = ramfile_open(path, mode);
  if (rf) {
    if (++g_files_open > g_files_peak) g_files_peak = g_files_open;
    io_track_add(rf, path);
    return rf;
  }

  /* CSWGuiSaveLoad opens its layout through two different APIs. The first menu
   * uses SDL_RWFromFile("override/saveload_p.gui") and gets the mobile layout,
   * while the in-game reopen uses stdio fopen("saveload_p.gui"). The latter
   * bypasses SDL_RWFromFile_hook and therefore used the desktop layout, whose
   * list box has no proto item. PopulateGameList then passed NULL as the source
   * button, producing the low BorderParams pointers and damaged selection.
   *
   * Materialize the known-good OBB member once and redirect only this read-only
   * bare-name open. sdl_load_file reaches the already-mounted patch/main OBBs;
   * sceIo is used for the cache write to avoid recursing through this wrapper. */
  const char *open_path = path;
  if (path && mode && mode[0] == 'r' && !strchr(mode, '+') &&
      (!strcmp(path, "saveload_p.gui") || !strcmp(path, "./saveload_p.gui"))) {
    static const char cache[] = DATA_PATH "/saveload_mobile.gui";
    SceIoStat st;
    if (sceIoGetstat(cache, &st) < 0 || st.st_size <= 0) {
      unsigned len = 0;
      void *data = sdl_load_file("override/saveload_p.gui", &len);
      if (data && len) {
        SceUID fd = sceIoOpen(cache, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        int wrote = fd >= 0 ? sceIoWrite(fd, data, len) : -1;
        if (fd >= 0) sceIoClose(fd);
        log_printf("[gui:saveload] cached mobile layout: %u bytes, write=%d",
                   len, wrote);
      } else {
        log_printf("[gui:saveload] mobile layout extraction FAILED");
      }
      free(data);
    }
    if (sceIoGetstat(cache, &st) >= 0 && st.st_size > 0) {
      open_path = cache;
      log_printf("[gui:saveload] stdio alias: %s -> %s", path, cache);
    }
  }

  /* FS_MISS_CACHE: a read-only open of a loose file that is not there. The
   * engine opens texturepacks/... and the like by relative path, which newlib
   * resolves against its working directory: so does this check. */
  if (open_path) {
    char abs[512];
    const char *ap = open_path;
    if (!strchr(open_path, ':') && open_path[0] != '/') {
      char cwd[256];
      if (getcwd(cwd, sizeof cwd)) {
        size_t cl = strlen(cwd);
        snprintf(abs, sizeof abs, "%s%s%s", cwd, (cl && cwd[cl - 1] == '/') ? "" : "/", open_path);
        ap = abs;
      } else {
        ap = NULL;
      }
    }
    if (ap && !strncmp(ap, "ux0:/", 5)) {            /* "ux0:/data" names "ux0:data" */
      char norm[512];
      snprintf(norm, sizeof norm, "ux0:%s", ap + 5);
      snprintf(abs, sizeof abs, "%s", norm);
      ap = abs;
    }
    if (ap && !strncmp(ap, "ux0:", 4)) {
      if (mode && mode[0] == 'r' && !strchr(mode, '+')) {
        if (fs_known_missing(ap)) { errno = ENOENT; return NULL; }
      } else {
        fs_miss_forget(ap);
      }
    }
  }
  FILE *f = fopen(open_path, mode);
  if (f) {
    if (++g_files_open > g_files_peak) g_files_peak = g_files_open;
    io_track_add(f, path);
    if ((++g_opens & 31) == 0)
      log_printf("[io] files: %d open now, peak %d, %d opened total",
                 g_files_open, g_files_peak, g_opens);
  }
  if (g_io_trace || !f) {
    log_printf("[io] fopen(\"%s\",\"%s\") -> %p  [%d open, peak %d]",
               path ? path : "(null)", mode ? mode : "?", (void *)f,
               g_files_open, g_files_peak);
    if (!f) {
      static int dumped = 0;
      if (dumped++ < 3) io_dump_open();
    }
  }
  return f;
}

static int fclose_diag(FILE *f) {
  if (!f) { log_printf("[io] fclose(NULL) REFUSED"); return -1; }
  VFile *v = vf_of(f);
  if (v) {
    io_lock();
    SharedFile *sf = v->sf;
    v->used = 0;
    g_vlive--;
    if (sf && --sf->refs <= 0) {
      sceIoClose(sf->fd);
      obbidx_close(sf->idx);
      obb_cache_drop(sf);
      log_printf("[io] shared archive closed: %s", sf->path);
      memset(sf, 0, sizeof *sf);
      g_files_open--;
    }
    io_unlock();
    return 0;
  }
  g_files_open--;
  io_track_del(f);
  if (ramfile_close(f)) return 0;
  return fclose(f);
}
/* NULL-FILE guards: miniz does not check fopen's result, so a failed
 * a failed main-OBB open turned straight into fseek(NULL) and a DATA_ABORT at
 * FAR=0xc (log107). newlib faults there; returning an error instead keeps a
 * failed open recoverable rather than fatal. Cheap, and it covers every caller. */
static int fseek_diag(FILE *f, long off, int whence) {
  if (!f) { log_printf("[io] fseek(NULL, %ld, %d) REFUSED", off, whence); return -1; }
  VFile *v = vf_of(f);
  if (v) {
    long base = (whence == SEEK_CUR) ? v->pos : (whence == SEEK_END) ? v->sf->size : 0;
    long np = base + off;
    if (np < 0) return -1;
    v->pos = np;
    v->eof = 0;
    return 0;
  }
  int r = fseek(f, off, whence);
  if (g_io_trace) log_printf("[io] fseek(%p, %ld, %d) -> %d", (void *)f, off, whence, r);
  return r;
}
/* miniz calls fseeko/ftello, NOT fseek/ftell -- verified from its UND list. These
 * being bound straight to newlib is why both earlier attempts at handle sharing
 * failed: miniz seeked our virtual FILE* through real newlib, which read the
 * fake struct as a real FILE, so the archive was unreadable from the moment it
 * was virtualised (every resource MISS, then a null-deref crash). Anything that
 * takes a FILE* MUST check vf_of() first. */
static int fseeko_diag(FILE *f, off_t off, int whence) {
  if (!f) { log_printf("[io] fseeko(NULL, %ld, %d) REFUSED", (long)off, whence); return -1; }
  VFile *v = vf_of(f);
  if (v) {
    long base = (whence == SEEK_CUR) ? v->pos : (whence == SEEK_END) ? v->sf->size : 0;
    long np = base + (long)off;
    if (np < 0) return -1;
    v->pos = np;
    v->eof = 0;
    return 0;
  }
  return fseeko(f, off, whence);
}
static off_t ftello_diag(FILE *f) {
  if (!f) { log_printf("[io] ftello(NULL) REFUSED"); return (off_t)-1; }
  VFile *v = vf_of(f);
  if (v) return (off_t)v->pos;
  return ftello(f);
}

static long ftell_diag(FILE *f) {
  if (!f) { log_printf("[io] ftell(NULL) REFUSED"); return -1L; }
  VFile *v = vf_of(f);
  if (v) return v->pos;
  long r = ftell(f);
  if (g_io_trace) log_printf("[io] ftell(%p) -> %ld", (void *)f, r);
  return r;
}
static size_t fread_diag(void *p, size_t sz, size_t n, FILE *f) {
  if (!f || !p) { log_printf("[io] fread(NULL) REFUSED"); return 0; }
  VFile *v = vf_of(f);
  if (v) {
    if (!sz || !n) return 0;
    io_lock();
    SharedFile *sf = v->sf;
    long want = (long)(sz * n);
    long got  = 0;

    /* 1. Replay cache. On any boot after the first this is nearly every read of
     *    the mount, and costs a memcpy instead of a card round trip. */
    if (obbidx_serve(sf->idx, v->pos, p, want)) {
      got = want;
      g_vhits++;
    } else if (obb_cache_get(sf, v->pos, p, want)) {
      /* 2. A read the game repeats (scripts, every run): RAM, see obb_cache.c. */
      got = want;
    } else {
      /* 3. One positional read; no seek, no newlib buffer to invalidate. */
      if (sf->rpos != v->pos) g_vseeks++;      /* diagnostic only now */
      uint64_t read_start = sceKernelGetProcessTimeWide();
      int rr = sceIoPread(sf->fd, p, (SceSize)want, (SceOff)v->pos);
      g_vread_us += sceKernelGetProcessTimeWide() - read_start;
      if (rr < 0) { v->err = 1; rr = 0; }
      got = rr;
      g_vread_bytes += (unsigned)got;
      sf->rpos = v->pos + got;
      if (got > 0) obbidx_record(sf->idx, v->pos, p, got);
      if (got == want) obb_cache_put(sf, v->pos, p, got);
      g_vreads++;
    }

    v->pos += got;
    if (got < want) v->eof = 1;

    if (((g_vreads + g_vhits) & 8191) == 0)
      log_printf("[io] shared reads=%u (cache hits=%u, %u%%) seeks=%u, %d handles live",
                 g_vreads, g_vhits,
                 (unsigned)((uint64_t)g_vhits * 100 / (g_vreads + g_vhits)),
                 g_vseeks, g_vlive);
    io_unlock();
    loadscreen_tick();                          /* outside the lock; throttled */
    return (size_t)got / sz;
  }
  size_t r = fread(p, sz, n, f);
  // Sparse heartbeat: OBB mount reads ~27k tiny local headers off slow storage.
  // Log every 4096th so we can see progress (and the final count) without the
  // per-read log I/O dominating the run.
  static int fc = 0;
  if (g_io_trace && (fc++ & 4095) == 0)
    log_printf("[io] fread #%d (sz=%u n=%u) -> %u", fc, (unsigned)sz, (unsigned)n, (unsigned)r);
  return r;
}

/* Remaining stdio entry points the .so can reach. Each must recognise a virtual
 * .obb handle before touching it as a real FILE*. The archives are opened
 * read-only, so the write-side calls simply refuse. */
static int feof_diag(FILE *f)   { VFile *v = vf_of(f); if (v) return v->eof; return f ? feof(f) : 1; }
static int ferror_diag(FILE *f) { VFile *v = vf_of(f); if (v) return v->err; return f ? ferror(f) : 1; }
static void rewind_diag(FILE *f) {
  VFile *v = vf_of(f);
  if (v) { v->pos = 0; v->eof = v->err = 0; return; }
  if (f) rewind(f);
}
static int fflush_diag(FILE *f) { if (vf_of(f)) return 0; return f ? fflush(f) : 0; }
/* Virtual archive handles are backed by a SceUID, not a newlib FILE*; hand
 * that back. Nothing in the .so does more than test it for validity. */
static int fileno_diag(FILE *f) { VFile *v = vf_of(f); if (v) return (int)v->sf->fd; return f ? fileno(f) : -1; }
static int setvbuf_diag(FILE *f, char *b, int m, size_t n) {
  if (vf_of(f)) return 0;
  return f ? setvbuf(f, b, m, n) : -1;
}
static size_t fwrite_diag(const void *p, size_t sz, size_t n, FILE *f) {
  if (vf_of(f)) { log_printf("[io] fwrite to a read-only archive REFUSED"); return 0; }
  return (f && p) ? fwrite(p, sz, n, f) : 0;
}
static int fputs_diag(const char *str, FILE *f) {
  if (vf_of(f)) return -1;
  return (f && str) ? fputs(str, f) : -1;
}
static int fputc_diag(int c, FILE *f) { if (vf_of(f)) return -1; return f ? fputc(c, f) : -1; }
static int fgetc_diag(FILE *f) {
  VFile *v = vf_of(f);
  if (v) { unsigned char c; return fread_diag(&c, 1, 1, f) == 1 ? (int)c : -1; }
  return f ? fgetc(f) : -1;
}
static char *fgets_diag(char *buf, int n, FILE *f) {
  VFile *v = vf_of(f);
  if (v) {
    if (!buf || n <= 1) return NULL;
    int i = 0;
    while (i < n - 1) {
      unsigned char c;
      if (fread_diag(&c, 1, 1, f) != 1) break;
      buf[i++] = (char)c;
      if (c == '\n') break;
    }
    if (!i) return NULL;
    buf[i] = '\0';
    return buf;
  }
  return (f && buf) ? fgets(buf, n, f) : NULL;
}


/* Every allocation the game makes comes through here, which is the one place
 * that can route the big ones away from newlib's arena. big_malloc/big_free
 * fall back to malloc/free below the threshold or when the pool is full, and
 * big_free's bounds test is what keeps the two populations apart -- see
 * bigalloc.c. The delete operators have to be wrapped for the same reason:
 * libstdc++'s own _ZdlPv calls free(), which would hand a pool pointer to
 * newlib. */
static void *malloc_diag(size_t n) { MEM_TRACE("malloc", n); heap_note_alloc(n); return big_malloc(n); }
static void *calloc_diag(size_t a, size_t b) {
  MEM_TRACE("calloc", a * b); heap_note_alloc(a * b);
  size_t n = a * b;
  if (a && n / a != b) return NULL;                 /* overflow */
  void *p = big_malloc(n);
  if (p) memset(p, 0, n);
  return p;
}
static void *realloc_diag(void *p, size_t n) { MEM_TRACE("realloc", n); heap_note_alloc(n); return big_realloc(p, n); }
static void free_diag(void *p) { big_free(p); }
/* Record the size so the new-handler can report what the failing request was;
 * a single huge allocation and a heap that is simply full need different fixes. */
static void *Znwj_diag(size_t n) {
  MEM_TRACE("new", n); g_heap_last_new = n; heap_note_alloc(n);
  void *p = bigalloc(n);
  /* On NULL fall through to the real operator new, which still runs the
   * new-handler and still throws bad_alloc -- the pool must not change what
   * failure looks like to the game. */
  return p ? p : real_Znwj(n);
}
static void *Znaj_diag(size_t n) {
  MEM_TRACE("new[]", n); g_heap_last_new = n; heap_note_alloc(n);
  void *p = bigalloc(n);
  return p ? p : real_Znaj(n);
}
static void ZdlPv_diag(void *p) { if (bigalloc_owns(p)) bigfree(p); else real_ZdlPv(p); }
static void ZdaPv_diag(void *p) { if (bigalloc_owns(p)) bigfree(p); else real_ZdaPv(p); }

// ---- libm gaps (absent from this newlib) ----------------------------------
// Derive from available primitives; accuracy is ample for game math.
// softfp in AND out, same as the SFP_* set -- these predate that pass and were
// missed because they are hand-written gap fills rather than direct libm binds.
// They are reached only through default_dynlib (no internal callers), so the
// uint32_t signature is safe to impose here.
static uint32_t exp2f_shim(uint32_t xb)  { return f2u(powf(2.0f, u2f(xb))); }
static uint32_t log10f_shim(uint32_t xb) { return f2u(logf(u2f(xb)) * 0.43429448190325176f); }  // 1/ln(10)
// These take the value BY VALUE and so are softfp-affected exactly like the SFP_*
// set above -- they were missed in that pass because their out-pointers made them
// look like pointer-only calls. They are not: written hardfp, `x` arrives in s0
// while the softfp caller put its bits in r0, so `s` was read out of r0 (the float
// pattern) and dereferenced. log70 died precisely here, DATA_ABORT with
// FAR == r0 == 0xbef71a94 == the bit pattern of the angle (~-0.483f).
// Taking the value as uint32_t/uint64_t puts it back in the core registers AAPCS
// uses for softfp, and shifts the pointers to r1/r2 (float) and r2/r3 (double --
// a uint64_t occupies the even-aligned r0:r1 pair). The *outputs* need no
// conversion: float/double memory representation is identical under both ABIs.
static void  sincos_shim(uint64_t xb, double *s, double *c) {
  double x = u2d(xb); *s = sin(x);  *c = cos(x);
}
static void  sincosf_shim(uint32_t xb, float *s, float *c) {
  float x = u2f(xb);  *s = sinf(x); *c = cosf(x);
}

// ---- Android/bionic libc extras -------------------------------------------
extern int *__errno(void);                         // newlib per-thread errno
static int  gettid_shim(void) { return sceKernelGetThreadId(); }
static void __assert2_shim(const char *file, int line, const char *fn, const char *expr) {
  fatal_error("assert %s:%d %s: %s", file ? file : "?", line, fn ? fn : "?", expr ? expr : "?");
}
// libKOTOR was built against bionic (stderr = &__sF[2], our dummy array); a real
// vfprintf on that bogus FILE* would fault. Route to the log like fprintf_safe.
static int vfprintf_safe(void *stream, const char *fmt, va_list ap) {
  (void)stream;
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  log_printf("[stdio] %s", buf);
  return n;
}

// ---- C++ exception_ptr / bad_cast internals (absent from this libsupc++) ----
// Bring-up stubs: KOTOR does not exercise std::exception_ptr on any hot path.
// If a real throw ever reaches these, swap in a proper libcxxabi impl.
static void *__cxa_current_primary_exception_stub(void)   { return 0; }
static void  __cxa_decrement_exception_refcount_stub(void *p) { (void)p; }
static void  __cxa_increment_exception_refcount_stub(void *p) { (void)p; }
static void  __cxa_rethrow_primary_exception_stub(void *p) { (void)p; }
static int   __cxa_uncaught_exceptions_stub(void)         { return 0; }
static void  bad_cast_ctor_stub(void *self)               { (void)self; }  // _ZNSt8bad_castC1Ev

// ---- game-owned data global -----------------------------------------------
// SDL's Android native-window handle: imported by libKOTOR, defined by NEITHER
// .so (Vita SDL2 has no Android video). We own the window, so own this slot.
void *Android_Window = 0;

// ---- FreeType (KOTOR's font/text engine) ----------------------------------
// The 16 FT_* imports pass straight through to VitaSDK's libfreetype (linked in
// CMakeLists). Declared as opaque externs -- we only need their addresses for
// the resolution table; the game calls them with the real FT_* argument types.
// Opaque externs (address-only) for the pass-through FT_* imports.
extern int FT_Done_FreeType(), FT_Done_Face(), FT_Attach_File(), FT_Attach_Stream(),
  FT_Get_Char_Index(), FT_Render_Glyph(), FT_Get_Kerning(),
  FT_Select_Charmap(), FT_Set_Charmap(), FT_Outline_Get_CBox();
// Real prototypes for the font-loading calls we trace (avoids cast warnings).
extern int FT_Init_FreeType(void *lib);
extern int FT_New_Face(void *lib, const char *path, long idx, void *face);
extern int FT_New_Memory_Face(void *lib, const void *buf, long sz, long idx, void *face);
extern int FT_Set_Char_Size(void *face, long w, long h, unsigned hres, unsigned vres);
extern int FT_Load_Glyph(void *face, unsigned gi, int flags);

// Diagnostic: is GUI text rendered via FreeType? The loadscreen crash is a null
// font (CAurTexture::GetFontInfo -> null). Trace the font-loading calls to see
// whether FreeType is on that path and, if so, whether it fails.
static int FT_Init_FreeType_t(void *lib) {
  int r = FT_Init_FreeType(lib);
  log_printf("[FT] Init_FreeType -> %d", r);
  return r;
}
// Inherited KOTOR I diagnostic: the first in-game dialogue triggered this fault:
//
//   [FT] New_Face("iosdialog.otf", idx=0) -> 1
//   [CRASH] DATA_ABORT  FAR=0x00000000  PC=companion+0x7e3d4
//
// FT_Error 1 is a FAILURE (0 is success), and the companion does not check it --
// it uses the unset face and null-derefs. The cause is the same defect class as
// the untranslated `stat`: this binding handed FreeType the game's bare relative
// path, which resolves against nothing on the Vita.
//
// Resolution order mirrors SDL_RWFromFile_hook: the VPK copy first so the font
// always travels with the loader (hand-copying assets to the card has been
// unreliable), then the card via fs_translate.
static int FT_New_Face_t(void *lib, const char *path, long idx, void *face) {
  char cand[512];
  const char *slash = path ? strrchr(path, '/') : NULL;
  const char *base  = slash ? slash + 1 : path;

  if (base) {
    snprintf(cand, sizeof cand, "app0:fonts/%s", base);
    int r = FT_New_Face(lib, cand, idx, face);
    if (r == 0) {
      log_printf("[FT] New_Face(\"%s\" -> %s) -> 0 (VPK)", path, cand);
      return 0;
    }
  }

  fs_translate(path, cand, sizeof cand);
  int r = FT_New_Face(lib, cand, idx, face);
  log_printf("[FT] New_Face(\"%s\" -> %s, idx=%ld) -> %d%s",
             path ? path : "(null)", cand, idx, r,
             r ? "  *** FAILED -- caller does not check, expect a null deref" : "");
  return r;
}
static int FT_New_Memory_Face_t(void *lib, const void *buf, long sz, long idx, void *face) {
  int r = FT_New_Memory_Face(lib, buf, sz, idx, face);
  log_printf("[FT] New_Memory_Face(buf=%p sz=%ld idx=%ld) -> %d", buf, sz, idx, r);
  return r;
}
static int FT_Set_Char_Size_t(void *face, long w, long h, unsigned hres, unsigned vres) {
  int r = FT_Set_Char_Size(face, w, h, hres, vres);
  log_printf("[FT] Set_Char_Size(w=%ld h=%ld hres=%u vres=%u) -> %d", w, h, hres, vres, r);
  return r;
}
static int FT_Load_Glyph_t(void *face, unsigned gi, int flags) {
  int r = FT_Load_Glyph(face, gi, flags);
  static int once = 0;
  if (!once) { once = 1; log_printf("[FT] Load_Glyph(gi=%u flags=0x%x) -> %d (first)", gi, flags, r); }
  return r;
}

#if STALL_LOG_MS
/* STALL_LOG_MS: the game thread's file time, for the [stall] line. */
static inline uint32_t us_since(uint64_t t0) { return (uint32_t)(sceKernelGetProcessTimeWide() - t0); }
static FILE *fopen_timed(const char *path, const char *mode) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  FILE *f = fopen_diag(path, mode);
  stall_part(SP_OPEN, us_since(t0), 0);
  return f;
}
static size_t fread_timed(void *p, size_t sz, size_t n, FILE *f) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  size_t r = fread_diag(p, sz, n, f);
  stall_part(SP_READ, us_since(t0), (uint32_t)(r * sz));
  return r;
}
static int fseek_timed(FILE *f, long off, int whence) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int r = fseek_diag(f, off, whence);
  stall_part(SP_SEEK, us_since(t0), 0);
  return r;
}
static int fseeko_timed(FILE *f, off_t off, int whence) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int r = fseeko_diag(f, off, whence);
  stall_part(SP_SEEK, us_since(t0), 0);
  return r;
}
static int fclose_timed(FILE *f) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int r = fclose_diag(f);
  stall_part(SP_CLOSE, us_since(t0), 0);
  return r;
}
#define FILE_FN(name) name##_timed
#else
#define FILE_FN(name) name##_diag
#endif

so_default_dynlib default_dynlib[] = {
  // ============ (a) stack protector ============
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },

  // ============ (a) C++ new/delete + runtime ============
  { "_Znwj", (uintptr_t)&Znwj_diag }, { "_Znaj", (uintptr_t)&Znaj_diag },
  { "_ZdlPv", (uintptr_t)&ZdlPv_diag }, { "_ZdaPv", (uintptr_t)&ZdaPv_diag },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit }, { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_stub },
  { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire }, { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
  { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch }, { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
  { "__cxa_throw", (uintptr_t)&__cxa_throw }, { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
  { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception }, { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
  { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
  { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 }, { "__dynamic_cast", (uintptr_t)&__dynamic_cast },
  { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev }, { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
  { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
  { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
  { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },

  // ============ (a) __aeabi helpers ============
  { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
  { "__aeabi_dsub", (uintptr_t)&__aeabi_dsub },
  { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv }, { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
  { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d }, { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
  { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
  { "__aeabi_llsl", (uintptr_t)&__aeabi_llsl }, { "__aeabi_llsr", (uintptr_t)&__aeabi_llsr },
  { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
  { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv }, { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
  { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d }, { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
  { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
  { "__aeabi_memclr", (uintptr_t)&sceClibMemclr }, { "__aeabi_memclr4", (uintptr_t)&sceClibMemclr }, { "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
  { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy }, { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy }, { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
  { "__aeabi_memmove", (uintptr_t)&sceClibMemmove }, { "__aeabi_memmove4", (uintptr_t)&sceClibMemmove }, { "__aeabi_memmove8", (uintptr_t)&sceClibMemmove },
  { "__aeabi_memset", (uintptr_t)&sceClibMemset2 }, { "__aeabi_memset4", (uintptr_t)&sceClibMemset2 }, { "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },

  // ============ (a) libc: string.h ============
  { "memcpy", (uintptr_t)&sceClibMemcpy }, { "memmove", (uintptr_t)&sceClibMemmove },
  { "memset", (uintptr_t)&memset }, { "memcmp", (uintptr_t)&memcmp }, { "memchr", (uintptr_t)&memchr },
  { "strcpy", (uintptr_t)&strcpy }, { "strncpy", (uintptr_t)&strncpy }, { "strcat", (uintptr_t)&strcat }, { "strncat", (uintptr_t)&strncat },
  { "strcmp", (uintptr_t)&strcmp }, { "strncmp", (uintptr_t)&strncmp }, { "strlen", (uintptr_t)&strlen },
  { "strchr", (uintptr_t)&strchr }, { "strrchr", (uintptr_t)&strrchr }, { "strstr", (uintptr_t)&strstr },
  { "strtok", (uintptr_t)&strtok }, { "strdup", (uintptr_t)&strdup },
  { "strcasecmp", (uintptr_t)&strcasecmp }, { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strerror", (uintptr_t)&strerror }, { "strcoll", (uintptr_t)&strcoll },
  { "strnlen", (uintptr_t)&strnlen }, { "strcasestr", (uintptr_t)&strcasestr },
  { "strcspn", (uintptr_t)&strcspn }, { "strerror_r", (uintptr_t)&strerror_r },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strpbrk", (uintptr_t)&strpbrk }, { "strsep", (uintptr_t)&strsep }, { "strtok_r", (uintptr_t)&strtok_r },
  { "wcscmp", (uintptr_t)&wcscmp }, { "wcscoll", (uintptr_t)&wcscoll }, { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wcscat", (uintptr_t)&wcscat }, { "wcscpy", (uintptr_t)&wcscpy }, { "wcsncat", (uintptr_t)&wcsncat }, { "wcsncpy", (uintptr_t)&wcsncpy },
  { "wcsrchr", (uintptr_t)&wcsrchr }, { "wcstok", (uintptr_t)&wcstok }, { "wcstombs", (uintptr_t)&wcstombs },
  { "mbrtowc", (uintptr_t)&mbrtowc }, { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wmemchr", (uintptr_t)&wmemchr }, { "wmemcmp", (uintptr_t)&wmemcmp }, { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove }, { "wmemset", (uintptr_t)&wmemset },

  // ============ (a) libc: stdio.h ============
  { "printf", (uintptr_t)&printf }, { "snprintf", (uintptr_t)&snprintf }, { "sprintf", (uintptr_t)&sprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf }, { "sscanf", (uintptr_t)&sscanf }, { "fprintf", (uintptr_t)&fprintf_safe },
  { "fopen", (uintptr_t)&FILE_FN(fopen) }, { "fclose", (uintptr_t)&FILE_FN(fclose) }, { "fread", (uintptr_t)&FILE_FN(fread) }, { "fwrite", (uintptr_t)&fwrite_diag },
  { "fseek", (uintptr_t)&FILE_FN(fseek) }, { "ftell", (uintptr_t)&ftell_diag }, { "fflush", (uintptr_t)&fflush_diag },
  { "fputs", (uintptr_t)&fputs_diag }, { "fputc", (uintptr_t)&fputc_diag }, { "fgets", (uintptr_t)&fgets_diag }, { "fgetc", (uintptr_t)&fgetc_diag },
  { "getc", (uintptr_t)&getc }, { "ungetc", (uintptr_t)&ungetc },
  { "puts", (uintptr_t)&puts }, { "putchar", (uintptr_t)&putchar }, { "rewind", (uintptr_t)&rewind_diag },
  { "feof", (uintptr_t)&feof_diag }, { "ferror", (uintptr_t)&ferror_diag }, { "setvbuf", (uintptr_t)&setvbuf_diag },

  // ============ (a) libc: stdlib.h ============
  { "malloc", (uintptr_t)&malloc_diag }, { "free", (uintptr_t)&free_diag }, { "calloc", (uintptr_t)&calloc_diag }, { "realloc", (uintptr_t)&realloc_diag },
  { "usleep", (uintptr_t)&usleep_diag },
  { "atoi", (uintptr_t)&atoi }, { "atol", (uintptr_t)&atol }, { "atof", (uintptr_t)&sfp_atof },
  { "strtol", (uintptr_t)&strtol }, { "strtoul", (uintptr_t)&strtoul }, { "strtod", (uintptr_t)&sfp_strtod },
  { "strtoll", (uintptr_t)&strtoll }, { "strtoull", (uintptr_t)&strtoull },
  { "strtoimax", (uintptr_t)&strtoimax }, { "strtoumax", (uintptr_t)&strtoumax },
  { "qsort", (uintptr_t)&qsort }, { "bsearch", (uintptr_t)&bsearch },
  { "rand", (uintptr_t)&rand }, { "srand", (uintptr_t)&srand },
  { "abs", (uintptr_t)&abs }, { "labs", (uintptr_t)&labs }, { "div", (uintptr_t)&div }, { "ldiv", (uintptr_t)&ldiv },
  { "lldiv", (uintptr_t)&lldiv },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv }, { "unsetenv", (uintptr_t)&unsetenv },
  { "basename", (uintptr_t)&basename }, { "mkdtemp", (uintptr_t)&mkdtemp }, { "memalign", (uintptr_t)&memalign },

  // ============ (a) libc: ctype.h / wchar.h ============
  { "isalpha", (uintptr_t)&isalpha }, { "isdigit", (uintptr_t)&isdigit }, { "isalnum", (uintptr_t)&isalnum },
  { "isspace", (uintptr_t)&isspace }, { "isupper", (uintptr_t)&isupper }, { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint }, { "ispunct", (uintptr_t)&ispunct }, { "iscntrl", (uintptr_t)&iscntrl },
  { "isgraph", (uintptr_t)&isgraph }, { "isxdigit", (uintptr_t)&isxdigit }, { "isblank", (uintptr_t)&isblank },
  { "toupper", (uintptr_t)&toupper }, { "tolower", (uintptr_t)&tolower },
  { "wctomb", (uintptr_t)&wctomb }, { "mbrlen", (uintptr_t)&mbrlen }, { "mbtowc", (uintptr_t)&mbtowc },

  // ============ (a) libm ============
  { "sin", (uintptr_t)&sfp_sin }, { "cos", (uintptr_t)&sfp_cos }, { "tan", (uintptr_t)&sfp_tan },
  { "asin", (uintptr_t)&sfp_asin }, { "acos", (uintptr_t)&sfp_acos }, { "atan", (uintptr_t)&sfp_atan }, { "atan2", (uintptr_t)&sfp_atan2 },
  { "exp", (uintptr_t)&sfp_exp }, { "log", (uintptr_t)&sfp_log }, { "log10", (uintptr_t)&sfp_log10 },
  { "pow", (uintptr_t)&sfp_pow }, { "sqrt", (uintptr_t)&sfp_sqrt }, { "ceil", (uintptr_t)&sfp_ceil }, { "floor", (uintptr_t)&sfp_floor },
  { "fabs", (uintptr_t)&sfp_fabs }, { "fmod", (uintptr_t)&sfp_fmod }, { "ldexp", (uintptr_t)&sfp_ldexp }, { "frexp", (uintptr_t)&sfp_frexp }, { "modf", (uintptr_t)&sfp_modf },
  { "fmin", (uintptr_t)&sfp_fmin },
  { "sinf", (uintptr_t)&sfp_sinf }, { "cosf", (uintptr_t)&sfp_cosf }, { "tanf", (uintptr_t)&sfp_tanf },
  { "asinf", (uintptr_t)&sfp_asinf }, { "acosf", (uintptr_t)&sfp_acosf }, { "atanf", (uintptr_t)&sfp_atanf }, { "atan2f", (uintptr_t)&sfp_atan2f },
  { "expf", (uintptr_t)&sfp_expf }, { "logf", (uintptr_t)&sfp_logf }, { "powf", (uintptr_t)&sfp_powf }, { "sqrtf", (uintptr_t)&sfp_sqrtf },
  { "fabsf", (uintptr_t)&sfp_fabsf }, { "floorf", (uintptr_t)&sfp_floorf }, { "ceilf", (uintptr_t)&sfp_ceilf }, { "fmodf", (uintptr_t)&sfp_fmodf },
  { "fmaxf", (uintptr_t)&sfp_fmaxf }, { "fminf", (uintptr_t)&sfp_fminf },
  { "ldexpf", (uintptr_t)&sfp_ldexpf },

  // ============ (a) time.h + pthread ============
  { "time", (uintptr_t)&time }, { "clock", (uintptr_t)&clock }, { "gettimeofday", (uintptr_t)&gettimeofday },
  { "localtime", (uintptr_t)&localtime }, { "gmtime", (uintptr_t)&gmtime }, { "mktime", (uintptr_t)&mktime }, { "strftime", (uintptr_t)&strftime },
  { "localtime_r", (uintptr_t)&localtime_r }, { "gmtime_r", (uintptr_t)&gmtime_r }, { "times", (uintptr_t)&times },
  { "pthread_create", (uintptr_t)&pthread_create_diag }, { "pthread_join", (uintptr_t)&pthread_join },
  // mutex/cond lock+wait go through bionic static-initializer bridges (b2v).
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init }, { "pthread_mutex_destroy", (uintptr_t)&pth_mutex_destroy },
  { "pthread_mutex_lock", (uintptr_t)&pth_mutex_lock }, { "pthread_mutex_unlock", (uintptr_t)&pth_mutex_unlock }, { "pthread_mutex_trylock", (uintptr_t)&pth_mutex_trylock },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init }, { "pthread_cond_destroy", (uintptr_t)&pth_cond_destroy },
  { "pthread_cond_wait", (uintptr_t)&pth_cond_wait }, { "pthread_cond_timedwait", (uintptr_t)&pth_cond_timedwait },
  { "pthread_cond_signal", (uintptr_t)&pth_cond_signal }, { "pthread_cond_broadcast", (uintptr_t)&pth_cond_broadcast },
  { "pthread_key_create", (uintptr_t)&pthread_key_create }, { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific }, { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_self", (uintptr_t)&pthread_self }, { "pthread_once", (uintptr_t)&pthread_once },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "pthread_cond_timedwait_monotonic_np", (uintptr_t)&pthread_cond_timedwait_monotonic_np_impl },

  // ============ (b) stubs: logging + process control ============
  { "__android_log_print", (uintptr_t)&__android_log_print_impl },
  { "__android_log_write", (uintptr_t)&__android_log_write_impl },
  { "getpriority", (uintptr_t)&ret0 }, { "setpriority", (uintptr_t)&ret0 },
  { "syscall", (uintptr_t)&ret0 }, { "system", (uintptr_t)&ret0 }, { "dladdr", (uintptr_t)&ret0 },
  { "dlopen", (uintptr_t)&__dlopen_stub }, { "dlsym", (uintptr_t)&__dlsym_stub },
  { "inotify_init", (uintptr_t)&ret0 }, { "inotify_add_watch", (uintptr_t)&ret0 }, { "inotify_rm_watch", (uintptr_t)&ret0 },
  { "mmap", (uintptr_t)&ret0 }, { "mprotect", (uintptr_t)&ret0 }, { "mremap", (uintptr_t)&ret0 }, { "munmap", (uintptr_t)&ret0 },
  { "sysinfo", (uintptr_t)&sysinfo_vita },
  { "SDL_IsChromebook", (uintptr_t)&ret0 }, { "SDL_IsScreenKeyboardShown", (uintptr_t)&ret0 },

  // ============ (b) stubs: 14 gl* calls missing from our vitaGL build ============
  { "glBlendColor", (uintptr_t)&ret0 }, { "glSampleCoverage", (uintptr_t)&ret0 },
  { "glDetachShader", (uintptr_t)&ret0 }, { "glIsBuffer", (uintptr_t)&ret0 }, { "glIsShader", (uintptr_t)&ret0 },
  { "glValidateProgram", (uintptr_t)&ret0 }, { "glCompressedTexSubImage2D", (uintptr_t)&ret0 },
  { "glGetShaderPrecisionFormat", (uintptr_t)&ret0 }, { "glGetRenderbufferParameteriv", (uintptr_t)&ret0 },
  { "glGetTexParameterfv", (uintptr_t)&ret0 }, { "glGetTexParameteriv", (uintptr_t)&ret0 },
  { "glGetUniformfv", (uintptr_t)&ret0 }, { "glGetUniformiv", (uintptr_t)&ret0 }, { "glTexParameterfv", (uintptr_t)&ret0 },

  // ============ (a) zlib compression ============
  { "inflate", (uintptr_t)&inflate }, { "inflateReset", (uintptr_t)&inflateReset },
  { "inflateInit_", (uintptr_t)&inflateInit_ }, { "inflateEnd", (uintptr_t)&inflateEnd },
  { "deflate", (uintptr_t)&deflate }, { "deflateReset", (uintptr_t)&deflateReset },
  { "deflateInit_", (uintptr_t)&deflateInit_ }, { "deflateEnd", (uintptr_t)&deflateEnd },

  // mz_zip_reader_* (miniz) and LzmaUncompress resolve CROSS-MODULE to the
  // so-loaded libminiz.so / libLzmaLib.so -- NOT stubbed here (a ret0 stub in
  // this table would override the cross-module link; see so_resolve).

  // ============ (b) libminiz/libLzmaLib's own libc imports ============
  { "fseeko", (uintptr_t)&FILE_FN(fseeko) }, { "ftello", (uintptr_t)&ftello_diag },
  { "abort", (uintptr_t)&abort }, { "utime", (uintptr_t)&utime },
  { "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
  { "__sF", (uintptr_t)&__sF },

  // ============ (a) libc: POSIX file / process (newlib) ============
  { "open", (uintptr_t)&open }, { "close", (uintptr_t)&close }, { "read", (uintptr_t)&read },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "lseek", (uintptr_t)&lseek }, { "fstat", (uintptr_t)&fstat }, { "stat", (uintptr_t)&stat },
  { "chmod", (uintptr_t)&chmod }, { "getcwd", (uintptr_t)&getcwd }, { "getpid", (uintptr_t)&getpid },
  { "gethostname", (uintptr_t)&gethostname }, { "getrlimit", (uintptr_t)&getrlimit }, { "setrlimit", (uintptr_t)&setrlimit },
  { "select", (uintptr_t)&select }, { "tcgetattr", (uintptr_t)&ret0 }, { "tcsetattr", (uintptr_t)&ret0 },
  { "fnmatch", (uintptr_t)&fnmatch_basic }, { "utimes", (uintptr_t)&utimes },
  { "fileno", (uintptr_t)&fileno_diag }, { "sysconf", (uintptr_t)&sysconf },
  { "clock_gettime", (uintptr_t)&clock_gettime }, { "nanosleep", (uintptr_t)&nanosleep },
  { "ctime", (uintptr_t)&ctime }, { "raise", (uintptr_t)&raise }, { "exit", (uintptr_t)&exit },
  { "__errno", (uintptr_t)&__errno }, { "gettid", (uintptr_t)&gettid_shim },
  { "__assert2", (uintptr_t)&__assert2_shim },
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_impl }, { "__memset_chk", (uintptr_t)&__memset_chk_impl },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_impl }, { "__strcat_chk", (uintptr_t)&__strcat_chk_impl },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_impl }, { "__strchr_chk", (uintptr_t)&__strchr_chk_impl },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_impl }, { "__strncpy_chk", (uintptr_t)&__strncpy_chk_impl },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_impl }, { "__strlcpy_chk", (uintptr_t)&__strlcpy_chk_impl },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_impl }, { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_impl },
  { "__gnu_strerror_r", (uintptr_t)&__gnu_strerror_r_impl },
  { "__ctype_get_mb_cur_max", (uintptr_t)&ret1 }, { "__register_atfork", (uintptr_t)&ret0 },
  { "__FD_SET_chk", (uintptr_t)&ret0 }, { "_exit", (uintptr_t)&exit },
  { "stderr", (uintptr_t)&__sF[2] }, { "stdin", (uintptr_t)&__sF[0] },
  { "getchar", (uintptr_t)&getchar }, { "getc_unlocked", (uintptr_t)&getc }, { "perror", (uintptr_t)&perror },
  { "setlocale", (uintptr_t)&setlocale }, { "swprintf", (uintptr_t)&swprintf }, { "vswprintf", (uintptr_t)&vswprintf },
  { "longjmp", (uintptr_t)&longjmp }, { "setjmp", (uintptr_t)&setjmp },

  // ============ (a) libc: RNG + varargs stdio (newlib) ============
  { "srand48", (uintptr_t)&srand48 }, { "lrand48", (uintptr_t)&lrand48 },
  { "vasprintf", (uintptr_t)&vasprintf }, { "vprintf", (uintptr_t)&vprintf },
  { "vsprintf", (uintptr_t)&vsprintf }, { "vsscanf", (uintptr_t)&vsscanf },
  { "vfprintf", (uintptr_t)&vfprintf_safe },   // bogus bionic FILE* -> route to log

  // ============ (a) libc: wide-char / wctype (newlib) ============
  { "btowc", (uintptr_t)&btowc }, { "wctob", (uintptr_t)&wctob },
  { "towlower", (uintptr_t)&towlower }, { "towupper", (uintptr_t)&towupper },
  { "iswalpha", (uintptr_t)&iswalpha }, { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit }, { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint }, { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace }, { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit }, { "iswblank", (uintptr_t)&iswblank },

  // ============ (a) libm gaps (shimmed from primitives) ============
  { "exp2f", (uintptr_t)&exp2f_shim }, { "log10f", (uintptr_t)&log10f_shim },
  { "sincos", (uintptr_t)&sincos_shim }, { "sincosf", (uintptr_t)&sincosf_shim },

  // ============ (a) pthread attr / mutexattr (vitasdk pthread) ============
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init }, { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate },
  { "pthread_exit", (uintptr_t)&pthread_exit }, { "pthread_setname_np", (uintptr_t)&ret0 },
  { "pthread_detach", (uintptr_t)&pthread_detach }, { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype },

  // ============ (a) C++ RTTI / exception internals ============
  { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev }, { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
  { "_ZTIi", (uintptr_t)&_ZTIi }, { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast }, { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
  // exception_ptr internals absent from this libsupc++ -> bring-up stubs
  { "_ZNSt8bad_castC1Ev", (uintptr_t)&bad_cast_ctor_stub },
  { "__cxa_current_primary_exception", (uintptr_t)&__cxa_current_primary_exception_stub },
  { "__cxa_decrement_exception_refcount", (uintptr_t)&__cxa_decrement_exception_refcount_stub },
  { "__cxa_increment_exception_refcount", (uintptr_t)&__cxa_increment_exception_refcount_stub },
  { "__cxa_rethrow_primary_exception", (uintptr_t)&__cxa_rethrow_primary_exception_stub },
  { "__cxa_uncaught_exceptions", (uintptr_t)&__cxa_uncaught_exceptions_stub },

  // ============ (a) game-owned data global ============
  { "Android_Window", (uintptr_t)&Android_Window },

  // ============ (a) FreeType (font/text engine) -> libfreetype ============
  // Font-loading calls go through trace wrappers (diagnostic); the rest direct.
  { "FT_Init_FreeType", (uintptr_t)&FT_Init_FreeType_t }, { "FT_Done_FreeType", (uintptr_t)&FT_Done_FreeType },
  { "FT_New_Face", (uintptr_t)&FT_New_Face_t }, { "FT_New_Memory_Face", (uintptr_t)&FT_New_Memory_Face_t },
  { "FT_Done_Face", (uintptr_t)&FT_Done_Face }, { "FT_Attach_File", (uintptr_t)&FT_Attach_File },
  { "FT_Attach_Stream", (uintptr_t)&FT_Attach_Stream }, { "FT_Set_Char_Size", (uintptr_t)&FT_Set_Char_Size_t },
  { "FT_Get_Char_Index", (uintptr_t)&FT_Get_Char_Index }, { "FT_Load_Glyph", (uintptr_t)&FT_Load_Glyph_t },
  { "FT_Render_Glyph", (uintptr_t)&FT_Render_Glyph }, { "FT_Get_Kerning", (uintptr_t)&FT_Get_Kerning },
  { "FT_Select_Charmap", (uintptr_t)&FT_Select_Charmap }, { "FT_Set_Charmap", (uintptr_t)&FT_Set_Charmap },
  { "FT_Outline_Get_CBox", (uintptr_t)&FT_Outline_Get_CBox },
};

const int default_dynlib_size = sizeof(default_dynlib);
