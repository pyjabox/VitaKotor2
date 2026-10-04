/* log.h -- tiny file logger writing to LOG_PATH
 * Historical performance comments are inherited from VitaKotor (KOTOR I).
 *
 * Used to trace JNI calls and crashes during bring-up. Cheap and
 * fully self-contained (no libc buffering surprises): every line is
 * opened/appended/closed so nothing is lost on a hard crash.
 */

#ifndef __LOG_H__
#define __LOG_H__

// Initialise the log (creates DATA_PATH, truncates log.txt, installs the
// crash handler). Safe to call once from main() before anything else.
void log_init(void);

// Drain the buffer and close the log fd so everything written so far is durable.
// Called automatically every LOG_FLUSH_MS and explicitly from the crash handler.
void log_flush(void);

// Enter panic mode: drain what is buffered, then make every later log_printf
// write straight to the card without taking the log lock. crash.c MUST call this
// before it logs anything, so a fault that happened while another thread held
// the lock cannot deadlock the dump. Irreversible by design.
void log_panic(void);

// Append a printf-formatted line (timestamp + newline added automatically).
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Convenience macro used by the JNI layer to trace each call site.
/* JNI tracing is invaluable during bring-up and pure overhead afterwards: the
 * game polls getCurrentLanguage ~16x a second, and each call emitted five lines
 * straight to the memory card (log118: 82 [JNI] lines in a single second, during
 * exactly the loading bursts that stutter). Keep the startup trace, then stop.
 * log_jni_enabled() lives in log.c so the budget is shared across all callers. */
int log_jni_enabled(void);
#define LOG_JNI(...)  do { if (log_jni_enabled()) log_printf("[JNI] " __VA_ARGS__); } while (0)

#endif
