/* log.c -- file logger to LOG_PATH (see log.h) */

#include <vitasdk.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "log.h"

// Originally every line did its own open/append/close so a hard crash could not
// lose output. That is three memory-card syscalls per line; keeping one fd open
// and closing periodically cut it to one write per line. That was still too
// much: logs 125-128 measured a line at 8-11ms, which made the logger ~60% of
// startup and ~19% of gameplay frame time -- the log was pacing the game.
//
// So lines now accumulate in a buffer and reach the card when it fills or when
// LOG_FLUSH_MS has elapsed (see config.h). A burst costs one write instead of
// hundreds. The clock is read for the timestamp anyway, so the elapsed check is
// free.
//
// Two invariants matter more than the speed:
//   * A CPU fault must never deadlock on the log. crash.c calls log_panic()
//     before it writes anything; after that every line bypasses both the buffer
//     and the lock and goes straight to the card, as it did originally.
//   * A hard hang must not swallow the last of the log. The time-based flush
//     bounds what is unwritten to LOG_FLUSH_MS.
#define LOG_BUF_BYTES (LOG_BUFFER_KB * 1024)

static SceUID g_log_fd = -1;

#if LOG_BUF_BYTES > 0
static char     g_buf[LOG_BUF_BYTES];
static int      g_buf_n;
static uint64_t g_last_flush;
static SceUID   g_log_mtx = -1;
static volatile int g_panic;

// Lock only when there is a mutex and we are not handling a fault. Before
// log_init (or if mutex creation failed) there is a single thread anyway.
static inline int log_lock(void) {
  if (g_panic || g_log_mtx < 0) return 0;
  return sceKernelLockMutex(g_log_mtx, 1, NULL) >= 0;
}
static inline void log_unlock(int held) {
  if (held) sceKernelUnlockMutex(g_log_mtx, 1);
}
#endif

static void log_open_if_needed(void) {
  if (g_log_fd < 0)
    g_log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
}

static void log_write_direct(const char *buf, int len) {
  log_open_if_needed();
  if (g_log_fd >= 0)
    sceIoWrite(g_log_fd, buf, len);
}

#if LOG_BUF_BYTES > 0
// Push whatever is buffered to the card. Caller holds the lock, or is panicking.
static void log_drain(void) {
  if (g_buf_n <= 0) return;
  log_write_direct(g_buf, g_buf_n);
  g_buf_n = 0;
}
#endif

void log_flush(void) {
#if LOG_BUF_BYTES > 0
  int held = log_lock();
  log_drain();
  // Close inside the lock: another thread reopening/writing g_log_fd while we
  // close it would write to a stale descriptor and lose the line.
  if (g_log_fd >= 0) {
    sceIoClose(g_log_fd);
    g_log_fd = -1;
  }
  log_unlock(held);
#else
  if (g_log_fd >= 0) {
    sceIoClose(g_log_fd);
    g_log_fd = -1;
  }
#endif
}

void log_panic(void) {
#if LOG_BUF_BYTES > 0
  // Set before draining: from here on nothing takes the lock, so a fault that
  // happened while another thread held it cannot deadlock us.
  g_panic = 1;
  log_drain();          // keep the crash dump ordered after earlier lines
#endif
}

/* First LOG_JNI_BUDGET lines only; see LOG_JNI in log.h. */
#define LOG_JNI_BUDGET 3000
int log_jni_enabled(void) {
  static int budget = LOG_JNI_BUDGET;
  if (budget <= 0) return 0;
  if (--budget == 0) {
    log_printf("[JNI] trace silenced after %d lines (steady state)", LOG_JNI_BUDGET);
    return 0;
  }
  return 1;
}

#if !LOG_DIAGNOSTICS
/* Release quiet. Tags are literal prefixes of the format string, so this runs
 * before any formatting: a suppressed line costs a handful of byte compares and
 * nothing else. See LOG_DIAGNOSTICS in config.h.
 *
 * The keep-rule comes first on purpose. A line that reports a failure is worth
 * more than the noise it sits in, and several of these tags carry both -- [GL]
 * emits per-window statistics and shader compile failures alike. */
static const char *const k_log_noisy[] = {
  "[GL]", "[res]", "[JNI]", "[gui]", "[FS]", "[SDL]", "[snd?]", "[heap]",
  "[big]", "[wd]", "[wd:t0]", "[wd:t1]", "[vgl]", "[input]", "[touch]",
  "[load]", "[model]",
};
static const char *const k_log_keep[] = {
  "fail", "FAIL", "error", "ERROR", "Error", "WARNING", "warning",
  "MISSING", "missing", "abort", "ABORT", "CRASH", "unresolved",
  "[DRAW_FRAME]", "[cull]", "[ailist]", "[cansee]", "[bloom]", "[ramfile]", "[glsf]", "[effects-low]", "[aibudget]", "[autotest]", "[glw]", "[glw:prof]",
};

static int log_suppressed(const char *fmt) {
  if (!fmt) return 1;
  for (unsigned i = 0; i < sizeof k_log_keep / sizeof k_log_keep[0]; i++)
    if (strstr(fmt, k_log_keep[i])) return 0;
#if LOG_BENCHMARK_MINIMAL
  /* Maximum-FPS benchmark: routine logs are not formatted, locked, buffered,
   * or written. Panic mode bypasses this function, while the keep list above
   * preserves actionable failure/error/warning lines during normal execution. */
  return 1;
#else
  if (fmt[0] != '[') return 0;
  for (unsigned i = 0; i < sizeof k_log_noisy / sizeof k_log_noisy[0]; i++) {
    const char *t = k_log_noisy[i];
    size_t n = strlen(t);
    if (!strncmp(fmt, t, n)) return 1;
  }
  return 0;
#endif
}
#endif

void log_printf(const char *fmt, ...) {
  char line[1024];

#if !LOG_DIAGNOSTICS
  if (!g_panic && log_suppressed(fmt)) return;
#endif

  // microsecond uptime stamp so ordering is unambiguous across threads
  uint64_t t = sceKernelGetProcessTimeWide();
  int n = snprintf(line, sizeof(line), "[%llu.%06llu] ",
                   (unsigned long long)(t / 1000000ULL),
                   (unsigned long long)(t % 1000000ULL));

  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);

  if (n > (int)sizeof(line) - 2)
    n = sizeof(line) - 2;
  line[n++] = '\n';
  line[n] = '\0';

#if LOG_BUF_BYTES > 0
  // Straight to the card when buffering cannot be done safely: a fault is in
  // progress, or there is no mutex yet (before log_init) / at all (creation
  // failed). Buffering without the lock would let threads interleave inside the
  // shared buffer, which is worse than the syscall it saves.
  if (g_panic || g_log_mtx < 0) {
    log_write_direct(line, n);
    return;
  }
  int held = log_lock();
  if (g_buf_n + n > LOG_BUF_BYTES)
    log_drain();
  if (n > LOG_BUF_BYTES) {       // cannot happen at 1024, but never overrun
    log_write_direct(line, n);
    log_unlock(held);
    return;
  }
  memcpy(g_buf + g_buf_n, line, (size_t)n);
  g_buf_n += n;
  // Durability floor: drain and close so at most LOG_FLUSH_MS is ever at risk.
  if (t - g_last_flush >= (uint64_t)LOG_FLUSH_MS * 1000) {
    log_drain();
    if (g_log_fd >= 0) { sceIoClose(g_log_fd); g_log_fd = -1; }
    g_last_flush = t;
  }
  log_unlock(held);
#else
  log_write_direct(line, n);
#endif
}

// ---- crash handling -------------------------------------------------------
// Hardware CPU-fault capture (PC/LR/registers) lives in crash.c via kubridge's
// kuKernelRegisterExceptionHandler. This log_fatal covers the controlled fatal
// path the loader itself takes (unresolved symbol, missing file, etc.).
void log_fatal(const char *reason) {
  log_printf("[CRASH] %s", reason ? reason : "(unknown)");
}

void log_init(void) {
  // Ensure DATA_PATH exists (ignore EEXIST).
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA_PATH, 0777);

  // Truncate the log at startup.
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0)
    sceIoClose(fd);

#if LOG_BUF_BYTES > 0
  // Several threads log (game, watchdog, workers, audio). Without a lock they
  // would interleave inside the shared buffer; the kernel used to serialise
  // whole lines for us only because each line was its own write.
  g_log_mtx = sceKernelCreateMutex("kotor_log", 0, 0, NULL);
  g_last_flush = sceKernelGetProcessTimeWide();
#endif

  log_printf("=== KOTOR Vita loader log ===");
  // Build stamp so a log is unambiguously tied to a specific .vpk (ends the
  // "did they flash the latest build?" guesswork).
  // CAVEAT: __DATE__/__TIME__ are baked when THIS file compiles. Cross-check an
  // incremental build against the installed eboot hash and a feature line too.
  log_printf("=== BUILD " __DATE__ " " __TIME__ " ===");
#if LOG_BUF_BYTES > 0
  log_printf("=== log buffered: %d KB, flush every %d ms (mutex=0x%x) ===",
             LOG_BUFFER_KB, LOG_FLUSH_MS, (unsigned)g_log_mtx);
#endif
}
