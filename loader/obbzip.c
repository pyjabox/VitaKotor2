/* obbzip.c -- see obbzip.h. */

#include "obbzip.h"
#include "log.h"

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>

#define SIG_EOCD  0x06054b50u
#define SIG_CDIR  0x02014b50u
#define SIG_LOCAL 0x04034b50u

/* An EOCD record is 22 bytes plus a comment of at most 65535, so it always
 * starts within the last 65557 bytes of the file. */
#define EOCD_SEARCH 65557u

/* Nothing the boot screen reads is anywhere near this large; the cap turns a
 * garbled size field into a NULL rather than a wild allocation. */
#define MAX_ENTRY (32u * 1024 * 1024)

struct ObbZip {
  SceUID fd;
  unsigned char *cd;
  unsigned cd_size;
};

/* Zip is little-endian and so is the Vita, but the central directory is a byte
 * stream with no alignment guarantees -- assemble from bytes rather than
 * casting a pointer into it. */
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

static int read_at(SceUID fd, unsigned long long off, void *dst, unsigned len) {
  if (sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) < 0) return 0;
  return sceIoRead(fd, dst, len) == (int)len;
}

ObbZip *obbzip_open(const char *path) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return NULL;

  SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
  if (size < 22) { sceIoClose(fd); return NULL; }

  unsigned tail = (unsigned)(size < (SceOff)EOCD_SEARCH ? size : (SceOff)EOCD_SEARCH);
  unsigned char *buf = malloc(tail);
  if (!buf) { sceIoClose(fd); return NULL; }
  if (!read_at(fd, (unsigned long long)size - tail, buf, tail)) {
    free(buf); sceIoClose(fd); return NULL;
  }

  /* Search backwards: with a trailing comment there can be more than one
   * EOCD-looking sequence, and the real one is the last. */
  long i;
  unsigned cd_size = 0, cd_off = 0;
  for (i = (long)tail - 22; i >= 0; i--) {
    if (rd32(buf + i) != SIG_EOCD) continue;
    cd_size = rd32(buf + i + 12);
    cd_off  = rd32(buf + i + 16);
    break;
  }
  free(buf);
  if (i < 0 || cd_size == 0 || (SceOff)cd_off + (SceOff)cd_size > size) {
    sceIoClose(fd); return NULL;
  }

  ObbZip *z = malloc(sizeof *z);
  if (!z) { sceIoClose(fd); return NULL; }
  z->cd = malloc(cd_size);
  if (!z->cd) { free(z); sceIoClose(fd); return NULL; }
  if (!read_at(fd, cd_off, z->cd, cd_size)) {
    free(z->cd); free(z); sceIoClose(fd); return NULL;
  }
  z->fd = fd;
  z->cd_size = cd_size;
  return z;
}

void obbzip_close(ObbZip *z) {
  if (!z) return;
  if (z->fd >= 0) sceIoClose(z->fd);
  free(z->cd);
  free(z);
}

/* Step to the entry after the one at `off`, or 0 when the directory ends. Every
 * caller walks the directory the same way, so the bounds checks live here. */
static unsigned cd_next(const ObbZip *z, unsigned off) {
  unsigned name = rd16(z->cd + off + 28);
  unsigned extra = rd16(z->cd + off + 30);
  unsigned comment = rd16(z->cd + off + 32);
  unsigned next = off + 46 + name + extra + comment;
  return next > z->cd_size ? 0 : next;
}

static int cd_valid(const ObbZip *z, unsigned off) {
  return off + 46 <= z->cd_size && rd32(z->cd + off) == SIG_CDIR;
}

int obbzip_locate(ObbZip *z, const char *name, unsigned long long *off,
                  unsigned *size) {
  if (!z || !name) return 0;
  unsigned want = (unsigned)strlen(name);

  unsigned pos = 0;
  while (cd_valid(z, pos)) {
    unsigned nlen = rd16(z->cd + pos + 28);
    if (pos + 46 + nlen > z->cd_size) break;

    if (nlen == want && memcmp(z->cd + pos + 46, name, want) == 0) {
      unsigned method = rd16(z->cd + pos + 10);
      unsigned usize  = rd32(z->cd + pos + 24);
      unsigned lho    = rd32(z->cd + pos + 42);
      if (method != 0) {
        log_printf("[obbzip] %s is method %u, not STORED -- skipped", name, method);
        return 0;
      }
      if (usize == 0) return 0;

      /* The local header repeats the name and carries its own extra field,
       * whose length routinely differs from the central directory's. The data
       * offset can only be computed from the local copy. */
      unsigned char lh[30];
      if (!read_at(z->fd, lho, lh, sizeof lh) || rd32(lh) != SIG_LOCAL) return 0;
      if (off) *off = (unsigned long long)lho + 30 + rd16(lh + 26) + rd16(lh + 28);
      if (size) *size = usize;
      return 1;
    }

    unsigned next = cd_next(z, pos);
    if (next <= pos) break;
    pos = next;
  }
  return 0;
}

int obbzip_pread(ObbZip *z, unsigned long long off, void *dst, unsigned len) {
  return z ? read_at(z->fd, off, dst, len) : 0;
}

void *obbzip_read(ObbZip *z, const char *name, unsigned *size_out) {
  unsigned long long off = 0;
  unsigned usize = 0;
  if (!obbzip_locate(z, name, &off, &usize)) return NULL;
  if (usize > MAX_ENTRY) {
    log_printf("[obbzip] %s is %u bytes, over the %u cap", name, usize, MAX_ENTRY);
    return NULL;
  }

  void *out = malloc(usize);
  if (!out) return NULL;
  if (!read_at(z->fd, off, out, usize)) { free(out); return NULL; }
  if (size_out) *size_out = usize;
  return out;
}

int obbzip_match(ObbZip *z, const char *prefix, const char *suffix,
                 int pick, char *out, unsigned out_size) {
  if (!z) return 0;
  unsigned plen = prefix ? (unsigned)strlen(prefix) : 0;
  unsigned slen = suffix ? (unsigned)strlen(suffix) : 0;
  int n = 0;

  unsigned off = 0;
  while (cd_valid(z, off)) {
    unsigned nlen = rd16(z->cd + off + 28);
    const unsigned char *nm = z->cd + off + 46;
    if (off + 46 + nlen > z->cd_size) break;

    int hit = nlen >= plen + slen &&
              (!plen || memcmp(nm, prefix, plen) == 0) &&
              (!slen || memcmp(nm + nlen - slen, suffix, slen) == 0);
    if (hit) {
      if (pick == n && out && out_size > nlen) {
        memcpy(out, nm, nlen);
        out[nlen] = '\0';
      }
      n++;
    }

    unsigned next = cd_next(z, off);
    if (next <= off) break;
    off = next;
  }
  return n;
}
