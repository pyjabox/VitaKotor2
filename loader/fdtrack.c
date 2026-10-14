/* fdtrack.c -- count the files and folders the process holds open.
 *
 * Twice in Telos's apartments (203TEL) the log stopped mid-session while the
 * game went on running, and a save made afterwards left its folder empty. The
 * log reopens its file at every flush, so that is what running out of file
 * handles looks like: nothing more can be opened -- not the log, not the save's
 * files, not the dialogues and scripts the room's door, bed and crew need.
 *
 * sceIoOpen/sceIoClose and sceIoDopen/sceIoDclose are wrapped at link time
 * (CMakeLists.txt), which covers everything in the eboot -- newlib's fopen and
 * open, SDL's RWops, our own code -- and so everything libkotor2 and
 * libObbVfs open through them. Each live handle keeps the start of its path.
 * A failure other than "not there" is logged with its code and the live
 * count, and the first one also lists what is open, grouped by folder. */

#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "log.h"
#include "fdtrack.h"

#if FD_TRACK

#define FDT_MAX 512
#define FDT_PATH 72
#define ERR_NOENT 0x80010002

typedef struct { SceUID fd; int dir; char path[FDT_PATH]; } fdt_t;
static fdt_t g_fdt[FDT_MAX];
static volatile int g_fdt_lock;
static int g_live, g_peak, g_dlive, g_dpeak, g_untracked;
static unsigned g_opens, g_dopens, g_fails;
static int g_last_err;
static int g_dumped;

SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
int __real_sceIoClose(SceUID fd);
SceUID __real_sceIoDopen(const char *dirname);
int __real_sceIoDclose(SceUID fd);

static void lock(void)   { while (__atomic_exchange_n(&g_fdt_lock, 1, __ATOMIC_ACQUIRE)) sceKernelDelayThread(0); }
static void unlock(void) { __atomic_store_n(&g_fdt_lock, 0, __ATOMIC_RELEASE); }

static void note_open(SceUID fd, const char *path, int dir) {
  lock();
  if (dir) { if (++g_dlive > g_dpeak) g_dpeak = g_dlive; g_dopens++; }
  else     { if (++g_live > g_peak) g_peak = g_live; g_opens++; }
  int i;
  for (i = 0; i < FDT_MAX && g_fdt[i].fd; i++) {}
  if (i < FDT_MAX) {
    g_fdt[i].fd = fd;
    g_fdt[i].dir = dir;
    snprintf(g_fdt[i].path, FDT_PATH, "%s", path ? path : "?");
  } else {
    g_untracked++;
  }
  unlock();
}

static void note_close(SceUID fd, int dir) {
  lock();
  if (dir) g_dlive--; else g_live--;
  for (int i = 0; i < FDT_MAX; i++)
    if (g_fdt[i].fd == fd && g_fdt[i].dir == dir) { g_fdt[i].fd = 0; break; }
  unlock();
}

/* What is open, grouped by folder, largest first. Called without the lock
 * held by the caller; takes a snapshot. */
static void dump_open(void) {
  static fdt_t snap[FDT_MAX];
  lock();
  memcpy(snap, g_fdt, sizeof snap);
  unlock();
  struct { char dir[FDT_PATH]; int n; } grp[32];
  int ng = 0;
  for (int i = 0; i < FDT_MAX; i++) {
    if (!snap[i].fd) continue;
    char d[FDT_PATH];
    snprintf(d, sizeof d, "%s%s", snap[i].dir ? "[dir] " : "", snap[i].path);
    char *s = strrchr(d, '/');
    if (s && !snap[i].dir) *s = 0;
    int j;
    for (j = 0; j < ng && strcmp(grp[j].dir, d); j++) {}
    if (j == ng && ng < 32) { snprintf(grp[ng].dir, FDT_PATH, "%s", d); grp[ng].n = 0; ng++; }
    if (j < ng) grp[j].n++;
  }
  for (int k = 0; k < ng; k++) {
    int best = k;
    for (int j = k + 1; j < ng; j++) if (grp[j].n > grp[best].n) best = j;
    if (best != k) { typeof(grp[0]) t = grp[k]; grp[k] = grp[best]; grp[best] = t; }
    log_printf("[fd]   %4d open in %s", grp[k].n, grp[k].dir);
  }
}

static void note_fail(const char *path, int err, int dir) {
  if (err == ERR_NOENT) return;                  /* probing for files that are not there */
  unsigned n = __atomic_add_fetch(&g_fails, 1, __ATOMIC_RELAXED);
  g_last_err = err;
  if (n <= 16 || n % 256 == 0)
    log_printf("[fd] %s FAILED 0x%08X for %.120s (%d files and %d folders open, %u failures)",
               dir ? "sceIoDopen" : "sceIoOpen", (unsigned)err, path ? path : "?", g_live, g_dlive, n);
  if (!g_dumped) { g_dumped = 1; dump_open(); }
}

SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode) {
  SceUID fd = __real_sceIoOpen(file, flags, mode);
  if (fd >= 0) note_open(fd, file, 0); else note_fail(file, fd, 0);
  return fd;
}
int __wrap_sceIoClose(SceUID fd) {
  int r = __real_sceIoClose(fd);
  if (r >= 0) note_close(fd, 0);
  return r;
}
SceUID __wrap_sceIoDopen(const char *dirname) {
  SceUID fd = __real_sceIoDopen(dirname);
  if (fd >= 0) note_open(fd, dirname, 1); else note_fail(dirname, fd, 1);
  return fd;
}
int __wrap_sceIoDclose(SceUID fd) {
  int r = __real_sceIoDclose(fd);
  if (r >= 0) note_close(fd, 1);
  return r;
}

void fdtrack_report(void) {
  log_printf("[fd] %d files open (peak %d), %d folders (peak %d); %u files and %u folders opened, "
             "%u failures (last 0x%08X)%s",
             g_live, g_peak, g_dlive, g_dpeak, g_opens, g_dopens, g_fails, (unsigned)g_last_err,
             g_untracked ? " (table full)" : "");
}

#else
/* The link-time wraps stay in CMakeLists.txt: pass straight through. */
SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
int __real_sceIoClose(SceUID fd);
SceUID __real_sceIoDopen(const char *dirname);
int __real_sceIoDclose(SceUID fd);
SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode) { return __real_sceIoOpen(file, flags, mode); }
int __wrap_sceIoClose(SceUID fd) { return __real_sceIoClose(fd); }
SceUID __wrap_sceIoDopen(const char *dirname) { return __real_sceIoDopen(dirname); }
int __wrap_sceIoDclose(SceUID fd) { return __real_sceIoDclose(fd); }
void fdtrack_report(void) {}
#endif
