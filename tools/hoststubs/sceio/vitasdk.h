/* vitasdk.h -- host stand-ins for the sceIo calls modstack.c, lang.c and
 * k2res.c make, so they run on a Mac or Linux box against a folder laid out
 * like the memory card (see tools/test_modstack.c). "ux0:" is the folder named
 * by the HOST_UX0 environment variable. NOT used by the Vita build. */
#ifndef HOST_SCEIO_VITASDK_H
#define HOST_SCEIO_VITASDK_H

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The Vita's SceIoStat names its fields st_mtime and friends, which the host's
 * <sys/stat.h> defines as macros. */
#undef st_atime
#undef st_mtime
#undef st_ctime
#ifdef __APPLE__
#define HOST_MTIME(s) ((s)->st_mtimespec.tv_sec)
#else
#define HOST_MTIME(s) ((s)->st_mtim.tv_sec)
#endif

typedef int SceUID;
typedef unsigned int SceSize;
typedef int64_t SceOff;
typedef struct { unsigned short year, month, day, hour, minute, second; unsigned microsecond; } SceDateTime;
typedef struct { int st_mode; unsigned st_attr; SceOff st_size; SceDateTime st_ctime, st_atime, st_mtime; unsigned st_private[6]; } SceIoStat;
typedef struct { SceIoStat d_stat; char d_name[256]; void *d_private; int dummy; } SceIoDirent;

#define SCE_S_IFMT  0xF000
#define SCE_S_IFDIR 0x1000
#define SCE_S_IFREG 0x2000
#define SCE_S_ISDIR(m) (((m) & SCE_S_IFMT) == SCE_S_IFDIR)
#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDWR   0x0003
#define SCE_O_CREAT  0x0200
#define SCE_O_TRUNC  0x0400
#define SCE_SEEK_SET 0
#define SCE_SEEK_END 2

static inline const char *host_path(const char *p, char *out, size_t sz) {
  const char *root = getenv("HOST_UX0");
  if (!strncmp(p, "ux0:", 4)) {
    p += 4;
    while (*p == '/') p++;
    snprintf(out, sz, "%s/%s", root ? root : ".", p);
    return out;
  }
  return p;
}

static inline void host_stat_from(SceIoStat *o, const struct stat *s) {
  memset(o, 0, sizeof *o);
  o->st_mode = S_ISDIR(s->st_mode) ? SCE_S_IFDIR : SCE_S_IFREG;
  o->st_size = s->st_size;
  struct tm tm;
  time_t mt = HOST_MTIME(s);
  gmtime_r(&mt, &tm);
  o->st_mtime = (SceDateTime){(unsigned short)(tm.tm_year + 1900), (unsigned short)(tm.tm_mon + 1),
                              (unsigned short)tm.tm_mday, (unsigned short)tm.tm_hour, (unsigned short)tm.tm_min,
                              (unsigned short)tm.tm_sec, 0};
}

static inline int sceIoGetstat(const char *p, SceIoStat *st) {
  char b[1024];
  struct stat s;
  if (stat(host_path(p, b, sizeof b), &s)) return -1;
  host_stat_from(st, &s);
  return 0;
}

static inline SceUID sceIoOpen(const char *p, int flags, int mode) {
  char b[1024];
  int f = (flags & 3) == SCE_O_RDONLY ? O_RDONLY : (flags & 3) == SCE_O_WRONLY ? O_WRONLY : O_RDWR;
  if (flags & SCE_O_CREAT) f |= O_CREAT;
  if (flags & SCE_O_TRUNC) f |= O_TRUNC;
  int fd = open(host_path(p, b, sizeof b), f, mode ? mode : 0644);
  return fd < 0 ? -1 : fd;
}
static inline int sceIoClose(SceUID fd) { return close(fd); }
static inline int sceIoRead(SceUID fd, void *b, SceSize n) { return (int)read(fd, b, n); }
static inline int sceIoWrite(SceUID fd, const void *b, SceSize n) { return (int)write(fd, b, n); }
static inline SceOff sceIoLseek(SceUID fd, SceOff off, int whence) { return lseek(fd, off, whence); }
static inline int sceIoPread(SceUID fd, void *b, SceSize n, SceOff off) { return (int)pread(fd, b, n, off); }
static inline int sceIoRemove(const char *p) { char b[1024]; return unlink(host_path(p, b, sizeof b)); }
static inline int sceIoMkdir(const char *p, int mode) { char b[1024]; return mkdir(host_path(p, b, sizeof b), (mode_t)mode); }
static inline int sceIoRename(const char *a, const char *z) {
  char b1[1024], b2[1024];
  return rename(host_path(a, b1, sizeof b1), host_path(z, b2, sizeof b2));
}

/* A directory handle: the DIR* and its path, for the per-entry stat. */
#define HOST_DIRS 64
static struct { DIR *d; char path[1024]; } host_dirs[HOST_DIRS];
static inline SceUID sceIoDopen(const char *p) {
  for (int i = 0; i < HOST_DIRS; i++)
    if (!host_dirs[i].d) {
      host_path(p, host_dirs[i].path, sizeof host_dirs[i].path);
      host_dirs[i].d = opendir(host_dirs[i].path);
      return host_dirs[i].d ? i + 1 : -1;
    }
  return -1;
}
static inline int sceIoDread(SceUID fd, SceIoDirent *e) {
  struct dirent *de;
  while ((de = readdir(host_dirs[fd - 1].d)) && de->d_name[0] == '.' &&
         (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
    ;
  if (!de) return 0;
  char b[2048];
  struct stat s;
  snprintf(b, sizeof b, "%s/%s", host_dirs[fd - 1].path, de->d_name);
  memset(e, 0, sizeof *e);
  if (!stat(b, &s)) host_stat_from(&e->d_stat, &s);
  snprintf(e->d_name, sizeof e->d_name, "%s", de->d_name);
  return 1;
}
static inline int sceIoDclose(SceUID fd) {
  closedir(host_dirs[fd - 1].d);
  host_dirs[fd - 1].d = NULL;
  return 0;
}

static inline uint64_t sceKernelGetProcessTimeWide(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#endif
