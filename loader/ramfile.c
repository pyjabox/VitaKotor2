/* ramfile.c -- serve the talk table (dialog.tlk) from RAM.
 *
 * Every CTlkTable::FetchInternal (one per string the game looks up) calls
 * CExoFile::GetSize twice -- fflush, ftell, fseek to the end, ftell, fseek
 * back -- then seeks and reads the entry and the text. On the memory card
 * that cost ~38 ms per fetch (hardware run real-vita-ccmsg-20261002), and a
 * combat feedback message fetches 10-20 strings: the 0.2-0.6 s freezes when
 * attacking. The freads themselves took 0.2 ms; the time was in the
 * flush/seek system calls.
 *
 * The file is read once into a buffer kept for the session (later opens reuse
 * it), and each open gets an fmemopen stream over it, so seeks, tells, flushes
 * and reads never reach the card. The stream is a
 * real newlib FILE, so every stdio call the game makes on it behaves as
 * before. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "bigalloc.h"
#include "ramfile.h"

#define RF_FILES 2              /* distinct files kept */
#define RF_STREAMS 8            /* streams open at once */

typedef struct { char name[16]; void *buf; size_t size; } rf_file_t;
static rf_file_t s_file[RF_FILES];
static FILE *s_stream[RF_STREAMS];
static SceKernelLwMutexWork s_mtx __attribute__((aligned(8)));
static volatile int s_mtx_state;     /* 0 none, 1 creating, 2 ready */

static void lock(void) {
  if (s_mtx_state != 2) {
    if (__sync_bool_compare_and_swap(&s_mtx_state, 0, 1)) {
      sceKernelCreateLwMutex(&s_mtx, "ramfile", 0, 0, NULL);
      __sync_synchronize();
      s_mtx_state = 2;
    } else {
      while (s_mtx_state != 2) sceKernelDelayThread(100);
    }
  }
  sceKernelLockLwMutex(&s_mtx, 1, NULL);
}
static void unlock(void) { sceKernelUnlockLwMutex(&s_mtx, 1); }

static const char *wanted(const char *path, const char *mode) {
  if (!RAMFILE_TLK || !path || !mode || mode[0] != 'r' || strchr(mode, '+')) return NULL;
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  return (!strcasecmp(b, "dialog.tlk") || !strcasecmp(b, "dialogf.tlk")) ? b : NULL;
}

/* Whole file into a new buffer, through newlib so relative paths resolve
 * exactly as fopen would resolve them. */
static void *load(const char *path, size_t *size_out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  off_t size = lseek(fd, 0, SEEK_END);
  if (size <= 0 || size > (off_t)RAMFILE_MAX_BYTES || lseek(fd, 0, SEEK_SET) != 0) {
    log_printf("[ramfile] %s not loaded: size %ld", path, (long)size);
    close(fd);
    return NULL;
  }
  char *buf = malloc((size_t)size);
  if (!buf) {
    log_printf("[ramfile] %s not loaded: no memory for %ld bytes", path, (long)size);
    close(fd);
    return NULL;
  }
  size_t got = 0;
  while (got < (size_t)size) {
    size_t want = (size_t)size - got;
    if (want > 1024 * 1024) want = 1024 * 1024;
    ssize_t r = read(fd, buf + got, want);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  if (got != (size_t)size) {
    log_printf("[ramfile] %s not loaded: read %u of %ld bytes", path, (unsigned)got, (long)size);
    free(buf);
    return NULL;
  }
  *size_out = (size_t)size;
  return buf;
}

FILE *ramfile_open(const char *path, const char *mode) {
  const char *name = wanted(path, mode);
  if (!name) return NULL;
  lock();
  rf_file_t *rf = NULL;
  for (int i = 0; i < RF_FILES && !rf; i++)
    if (s_file[i].buf && !strcasecmp(s_file[i].name, name)) rf = &s_file[i];
  if (!rf) {
    for (int i = 0; i < RF_FILES && !rf; i++)
      if (!s_file[i].buf) rf = &s_file[i];
    if (!rf) { unlock(); return NULL; }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    size_t size = 0;
    void *buf = load(path, &size);
    if (!buf) { unlock(); return NULL; }
    snprintf(rf->name, sizeof rf->name, "%s", name);
    rf->buf = buf;
    rf->size = size;
    log_printf("[ramfile] %s in RAM: %u KB, loaded in %u ms", path, (unsigned)(size / 1024u),
               (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000u));
  }
  int slot = -1;
  for (int i = 0; i < RF_STREAMS && slot < 0; i++)
    if (!s_stream[i]) slot = i;
  FILE *f = slot >= 0 ? fmemopen(rf->buf, rf->size, "rb") : NULL;
  if (f) s_stream[slot] = f;
  unlock();
  if (!f) log_printf("[ramfile] %s: no stream (slot %d), using the card", path, slot);
  return f;
}

/* ---- voice lines -----------------------------------------------------------
 * CExoStreamingSoundSourceInternal opens each voice line with fopen and keeps
 * the FILE until the same source plays its next line or is destroyed: Stop
 * only closes the FMOD stream (libkotor2 0x5736c4; InitializeSource 0x573738
 * and the destructor are the only fcloses, of source+0xc4). Every speaker and
 * terminal therefore holds its last line open. On Android that costs nothing.
 * On the Vita a process can hold 64 files, and a voice pack's lines are loose
 * files on the card: in Telos's apartments 58 of them were held at once, then
 * every open failed (0x80010018) -- dialogues, scripts, the save -- and the
 * room stopped answering. Archive voice lines (English) share one handle and
 * never reach this path.
 *
 * So a loose voice line or streamed sound is read whole into RAM at open, its
 * card handle closed at once, and the game gets an fmemopen stream over the
 * copy, freed at its fclose. */
#define RF_VOICES 256
#define RF_VOICE_MAX_BYTES (1024u * 1024u)
typedef struct { FILE *f; void *buf; size_t size; } rf_voice_t;
static rf_voice_t s_voice[RF_VOICES];
static unsigned s_voice_live, s_voice_peak, s_voice_total;
static size_t s_voice_bytes;

static int has_dir(const char *path, const char *dir) {   /* "dir/" at the start or after a '/' */
  size_t n = strlen(dir);
  for (const char *p = path; *p; p++)
    if ((p == path || p[-1] == '/') && !strncasecmp(p, dir, n) && p[n] == '/') return 1;
  return 0;
}

static int voice_wanted(const char *path, const char *mode) {
  if (!RAMFILE_VOICE || !path || !mode || mode[0] != 'r' || strchr(mode, '+')) return 0;
  return has_dir(path, "streamvoice") || has_dir(path, "streamsounds");
}

FILE *ramfile_open_voice(const char *path, const char *mode) {
  if (!voice_wanted(path, mode)) return NULL;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;                       /* not there: fopen says so as usual */
  off_t size = lseek(fd, 0, SEEK_END);
  if (size <= 0 || size > (off_t)RF_VOICE_MAX_BYTES || lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return NULL;                                 /* the card as before */
  }
  char *buf = big_malloc((size_t)size);
  if (!buf) { close(fd); return NULL; }
  size_t got = 0;
  while (got < (size_t)size) {
    ssize_t r = read(fd, buf + got, (size_t)size - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  if (got != (size_t)size) { big_free(buf); return NULL; }
  FILE *f = fmemopen(buf, (size_t)size, "rb");
  if (!f) { big_free(buf); return NULL; }
  lock();
  int slot = -1;
  for (int i = 0; i < RF_VOICES && slot < 0; i++)
    if (!s_voice[i].f) slot = i;
  if (slot >= 0) {
    s_voice[slot].f = f;
    s_voice[slot].buf = buf;
    s_voice[slot].size = (size_t)size;
    s_voice_bytes += (size_t)size;
    if (++s_voice_live > s_voice_peak) s_voice_peak = s_voice_live;
    s_voice_total++;
  }
  unsigned live = s_voice_live, total = s_voice_total;
  size_t bytes = s_voice_bytes;
  unlock();
  if (slot < 0) { fclose(f); big_free(buf); return NULL; }   /* table full: the card */
  if (total <= 3 || total % 64 == 0)
    log_printf("[ramfile] voice line in RAM: %s (%u KB); %u held now (%u KB), %u served",
               path, (unsigned)(size / 1024), live, (unsigned)(bytes / 1024), total);
  return f;
}

int ramfile_close(FILE *f) {
  int ours = 0;
  lock();
  for (int i = 0; i < RF_VOICES; i++)
    if (f && s_voice[i].f == f) {
      void *buf = s_voice[i].buf;
      s_voice_bytes -= s_voice[i].size;
      s_voice[i].f = NULL;
      s_voice[i].buf = NULL;
      s_voice[i].size = 0;
      s_voice_live--;
      unlock();
      fclose(f);
      big_free(buf);
      return 1;
    }
  for (int i = 0; i < RF_STREAMS; i++)
    if (f && s_stream[i] == f) { s_stream[i] = NULL; ours = 1; break; }
  unlock();
  if (ours) fclose(f);        /* the buffer stays for the next open */
  return ours;
}
