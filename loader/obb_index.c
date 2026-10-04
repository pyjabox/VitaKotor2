/* obb_index.c -- see obb_index.h. */

#include "obb_index.h"
#include "config.h"
#include "log.h"

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDX_MAGIC   0x3149424Fu   /* "OBI1" */
#define IDX_MAX_REC 65536         /* ranges; ~16k entries need far fewer */

typedef struct { long off; int len; int blob; } Rec;

struct ObbIndex {
  int   serving;          /* 1 = replaying a loaded cache, 0 = recording */
  int   full;             /* recording hit its budget and stopped */
  Rec  *rec;
  int   nrec, caprec;
  unsigned char *blob;
  int   nblob, capblob;
  long  archive_size;
  char  path[160];        /* the cache file, not the archive */
};

typedef struct {
  unsigned magic;
  unsigned version;
  long     archive_size;
  int      nrec;
  int      nblob;
} IdxHeader;

/* ---- small helpers -------------------------------------------------------- */

static int rec_cmp(const void *a, const void *b) {
  long x = ((const Rec *)a)->off, y = ((const Rec *)b)->off;
  return (x > y) - (x < y);
}

/* Greatest record with off <= target, or -1. */
static int rec_find(const Rec *r, int n, long target) {
  int lo = 0, hi = n - 1, best = -1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (r[mid].off <= target) { best = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  return best;
}

/* ---- load ----------------------------------------------------------------- */

static int idx_load(ObbIndex *ix) {
  SceUID fd = sceIoOpen(ix->path, SCE_O_RDONLY, 0);
  if (fd < 0) return 0;

  IdxHeader h;
  int ok = 0;
  if (sceIoRead(fd, &h, sizeof h) == (int)sizeof h &&
      h.magic == IDX_MAGIC && h.version == 1 &&
      h.archive_size == ix->archive_size &&
      h.nrec > 0 && h.nrec <= IDX_MAX_REC && h.nblob > 0) {

    Rec *r = (Rec *)malloc((size_t)h.nrec * sizeof(Rec));
    unsigned char *b = (unsigned char *)malloc((size_t)h.nblob);
    if (r && b &&
        sceIoRead(fd, r, h.nrec * (int)sizeof(Rec)) == h.nrec * (int)sizeof(Rec) &&
        sceIoRead(fd, b, h.nblob) == h.nblob) {
      ix->rec = r; ix->nrec = h.nrec;
      ix->blob = b; ix->nblob = h.nblob;
      ix->serving = 1;
      ok = 1;
      log_printf("[obbidx] serving %d ranges (%d KB) from %s", h.nrec, h.nblob / 1024, ix->path);
    } else {
      free(r); free(b);
    }
  }
  sceIoClose(fd);
  if (!ok) log_printf("[obbidx] %s unusable/absent -- recording a fresh one", ix->path);
  return ok;
}

/* ---- public --------------------------------------------------------------- */

ObbIndex *obbidx_open(const char *archive_path, long archive_size) {
#if OBB_INDEX_CACHE
  if (!archive_path || archive_size <= 0) return NULL;
  ObbIndex *ix = (ObbIndex *)calloc(1, sizeof *ix);
  if (!ix) return NULL;
  ix->archive_size = archive_size;
  snprintf(ix->path, sizeof ix->path, "%s.idx", archive_path);
  idx_load(ix);
  return ix;
#else
  (void)archive_path; (void)archive_size;
  return NULL;
#endif
}

int obbidx_is_serving(const ObbIndex *ix) { return ix && ix->serving; }

int obbidx_serve(ObbIndex *ix, long off, void *dst, long len) {
  if (!ix || !ix->serving || !dst || len <= 0) return 0;
  int i = rec_find(ix->rec, ix->nrec, off);
  if (i < 0) return 0;
  const Rec *r = &ix->rec[i];
  if (off + len > r->off + r->len) return 0;          /* not fully covered */
  memcpy(dst, ix->blob + r->blob + (off - r->off), (size_t)len);
  return 1;
}

void obbidx_record(ObbIndex *ix, long off, const void *src, long len) {
  if (!ix || ix->serving || ix->full || !src || len <= 0) return;
  if (len > OBB_INDEX_MAX_RANGE) return;               /* bulk asset read, not a header */

  if (ix->nblob + len > OBB_INDEX_BUDGET_KB * 1024 || ix->nrec >= IDX_MAX_REC) {
    ix->full = 1;
    log_printf("[obbidx] recording budget reached (%d ranges, %d KB) -- cache will be partial",
               ix->nrec, ix->nblob / 1024);
    return;
  }
  if (ix->nrec == ix->caprec) {
    int cap = ix->caprec ? ix->caprec * 2 : 1024;
    Rec *r = (Rec *)realloc(ix->rec, (size_t)cap * sizeof(Rec));
    if (!r) { ix->full = 1; return; }
    ix->rec = r; ix->caprec = cap;
  }
  if (ix->nblob + len > ix->capblob) {
    int cap = ix->capblob ? ix->capblob * 2 : 64 * 1024;
    while (cap < ix->nblob + len) cap *= 2;
    unsigned char *b = (unsigned char *)realloc(ix->blob, (size_t)cap);
    if (!b) { ix->full = 1; return; }
    ix->blob = b; ix->capblob = cap;
  }
  ix->rec[ix->nrec].off  = off;
  ix->rec[ix->nrec].len  = (int)len;
  ix->rec[ix->nrec].blob = ix->nblob;
  ix->nrec++;
  memcpy(ix->blob + ix->nblob, src, (size_t)len);
  ix->nblob += (int)len;
}

void obbidx_finish(ObbIndex *ix) {
  if (!ix || ix->serving || ix->nrec <= 0) return;

  /* Sort so serving can binary-search. Duplicate offsets are harmless: the
   * first match wins and holds identical bytes. */
  qsort(ix->rec, (size_t)ix->nrec, sizeof(Rec), rec_cmp);

  SceUID fd = sceIoOpen(ix->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) { log_printf("[obbidx] cannot write %s -- next boot stays slow", ix->path); return; }

  IdxHeader h = { IDX_MAGIC, 1, ix->archive_size, ix->nrec, ix->nblob };
  int ok = sceIoWrite(fd, &h, sizeof h) == (int)sizeof h &&
           sceIoWrite(fd, ix->rec, ix->nrec * (int)sizeof(Rec)) == ix->nrec * (int)sizeof(Rec) &&
           sceIoWrite(fd, ix->blob, ix->nblob) == ix->nblob;
  sceIoClose(fd);

  if (ok) log_printf("[obbidx] wrote %s: %d ranges, %d KB -- next boot should skip the scattered reads",
                     ix->path, ix->nrec, ix->nblob / 1024);
  else  { log_printf("[obbidx] write of %s failed", ix->path); sceIoRemove(ix->path); }

  /* Stop recording either way; the mount is over. */
  ix->full = 1;
}

void obbidx_close(ObbIndex *ix) {
  if (!ix) return;
  free(ix->rec);
  free(ix->blob);
  free(ix);
}
