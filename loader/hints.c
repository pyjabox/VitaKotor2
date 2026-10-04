/* hints.c -- see hints.h. */

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>

#include "hints.h"
#include "config.h"
#include "log.h"
#include "obbzip.h"

#define MAX_HINTS  256
#define ARENA_SIZE (48 * 1024)

#define RESTYPE_2DA 2017

static char  *g_arena;
static unsigned g_used;
static const char *g_hint[MAX_HINTS];
static int    g_n;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

/* ---- chitin.key ------------------------------------------------------------ */

/* Find a resource by name and type. Writes the archive's own filename (as the
 * key spells it, e.g. "data\\2da.bif") and the resource's index within it. */
static int key_find(const unsigned char *key, unsigned len, const char *name,
                    unsigned type, char *bif, unsigned bif_size, unsigned *index) {
  if (len < 24 || memcmp(key, "KEY V1", 6) != 0) return 0;
  unsigned nbif = rd32(key + 8), nkey = rd32(key + 12);
  unsigned offfile = rd32(key + 16), offkey = rd32(key + 20);
  if ((unsigned long long)offkey + (unsigned long long)nkey * 22 > len) return 0;

  for (unsigned i = 0; i < nkey; i++) {
    const unsigned char *e = key + offkey + i * 22;
    if (rd16(e + 16) != type) continue;
    /* ResRefs are 16 bytes, NUL-padded rather than NUL-terminated. */
    char ref[17];
    memcpy(ref, e, 16);
    ref[16] = '\0';
    if (strcmp(ref, name) != 0) continue;

    unsigned resid = rd32(e + 18);
    unsigned bifidx = resid >> 20;
    *index = resid & 0xFFFFF;
    if (bifidx >= nbif) return 0;

    const unsigned char *f = key + offfile + bifidx * 12;
    if (offfile + (bifidx + 1) * 12 > len) return 0;
    unsigned fnoff = rd32(f + 4), fnlen = rd16(f + 8);
    if (fnoff + fnlen > len || fnlen >= bif_size) return 0;
    memcpy(bif, key + fnoff, fnlen);
    bif[fnlen] = '\0';
    return 1;
  }
  return 0;
}

/* ---- .bzf ------------------------------------------------------------------ */

/* Pull one resource out of a compressed BIF.
 *
 * The entry's third field is the size once decompressed; the packed bytes run
 * from its offset to whichever entry starts next, which is not necessarily the
 * next one in the table. */
static unsigned char *bzf_extract(const unsigned char *bzf, unsigned len,
                                  unsigned index, LzmaUncompressFn lzma,
                                  unsigned *out_len) {
  if (len < 20 || memcmp(bzf, "BIFF", 4) != 0) return NULL;
  unsigned nvar = rd32(bzf + 8), offvar = rd32(bzf + 16);
  if (index >= nvar) return NULL;
  if ((unsigned long long)offvar + (unsigned long long)nvar * 16 > len) return NULL;

  const unsigned char *e = bzf + offvar + index * 16;
  unsigned off = rd32(e + 4), usize = rd32(e + 8);
  if (off >= len || usize == 0 || usize > ARENA_SIZE * 4) return NULL;

  unsigned end = len;
  for (unsigned i = 0; i < nvar; i++) {
    unsigned o = rd32(bzf + offvar + i * 16 + 4);
    if (o > off && o < end) end = o;
  }
  if (end <= off + 5) return NULL;

  unsigned char *out = malloc(usize);
  if (!out) return NULL;
  size_t dst_len = usize, src_len = end - off - 5;
  /* Five bytes of LZMA properties, then the stream. The eight-byte uncompressed
   * length that a standalone .lzma file carries is not present, which is why
   * the size has to come from the entry. */
  int rc = lzma(out, &dst_len, bzf + off + 5, &src_len, bzf + off, 5);
  if (rc != 0 || dst_len == 0) {
    log_printf("[hints] LzmaUncompress rc=%d (%u packed -> %u of %u)", rc,
               (unsigned)src_len, (unsigned)dst_len, usize);
    free(out);
    return NULL;
  }
  *out_len = (unsigned)dst_len;
  return out;
}

/* ---- binary 2DA ------------------------------------------------------------ */

/* Collect every cell of a "2DA V2.b" that parses as a number -- for
 * loadscreenhints both columns are StrRefs, so that is all of them. */
static int twoda_strrefs(const unsigned char *d, unsigned len, unsigned *out,
                         int max) {
  if (len < 9 || memcmp(d, "2DA V2.b", 8) != 0) return 0;
  unsigned p = 9;                                  /* past the header newline */

  unsigned cols = 0;
  while (p < len && d[p] != '\0') {                /* tab-separated labels */
    if (d[p] == '\t') cols++;
    p++;
  }
  if (p >= len || cols == 0) return 0;
  p++;

  if (p + 4 > len) return 0;
  unsigned rows = rd32(d + p);
  p += 4;
  if (!rows || (unsigned long long)rows * cols > 65535) return 0;

  for (unsigned seen = 0; seen < rows && p < len; p++)  /* row labels */
    if (d[p] == '\t') seen++;

  unsigned cells = rows * cols;
  if (p + cells * 2 + 2 > len) return 0;
  const unsigned char *idx = d + p;
  p += cells * 2;
  unsigned pool_len = rd16(d + p);
  p += 2;
  if (p + pool_len > len) return 0;
  const unsigned char *pool = d + p;

  int n = 0;
  for (unsigned i = 0; i < cells && n < max; i++) {
    unsigned o = rd16(idx + i * 2);
    if (o >= pool_len) continue;
    const char *s = (const char *)pool + o;
    unsigned v = 0;
    int digits = 0;
    for (const char *q = s; *q && (unsigned)(q - s) < pool_len - o; q++) {
      if (*q < '0' || *q > '9') { digits = 0; break; }
      v = v * 10 + (unsigned)(*q - '0');
      digits++;
    }
    if (digits > 0) out[n++] = v;
  }
  return n;
}

/* ---- dialog.tlk ------------------------------------------------------------ */

static int arena_put(const char *s, unsigned n) {
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
  if (!n || g_used + n + 1 > ARENA_SIZE) return 0;
  char *dst = g_arena + g_used;
  memcpy(dst, s, n);
  dst[n] = '\0';
  g_used += n + 1;
  g_hint[g_n++] = dst;
  return 1;
}

/* ---- public ---------------------------------------------------------------- */

int hints_count(void) { return g_n; }

const char *hints_get(int i) {
  return (i >= 0 && i < g_n) ? g_hint[i] : NULL;
}

void hints_free(void) {
  free(g_arena);
  g_arena = NULL;
  g_used = 0;
  g_n = 0;
}

int hints_load(LzmaUncompressFn lzma) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  unsigned char *key = NULL, *bzf = NULL, *tda = NULL;
  unsigned *refs = NULL;
  if (!lzma) { log_printf("[hints] no LZMA decompressor"); return 0; }
  ObbZip *z = obbzip_open(OBB_MAIN_PATH);
  if (!z) { log_printf("[hints] cannot read %s as a zip", OBB_MAIN_PATH); return 0; }

  unsigned key_len = 0;
  key = obbzip_read(z, "chitin.key", &key_len);
  if (!key) { log_printf("[hints] no chitin.key"); goto done; }

  char bifname[64];
  unsigned index = 0;
  if (!key_find(key, key_len, "loadscreenhints", RESTYPE_2DA, bifname,
                sizeof bifname, &index)) {
    log_printf("[hints] loadscreenhints not in chitin.key");
    goto done;
  }

  /* The key names the uncompressed BIF ("data\\2da.bif"); the OBB carries the
   * compressed sibling under a forward-slash path. */
  char path[80];
  unsigned j = 0;
  for (unsigned i = 0; bifname[i] && j + 1 < sizeof path; i++)
    path[j++] = (bifname[i] == '\\') ? '/' : bifname[i];
  path[j] = '\0';
  if (j > 4 && strcmp(path + j - 4, ".bif") == 0) memcpy(path + j - 4, ".bzf", 4);

  unsigned bzf_len = 0;
  bzf = obbzip_read(z, path, &bzf_len);
  if (!bzf) { log_printf("[hints] no %s in the archive", path); goto done; }

  unsigned tda_len = 0;
  tda = bzf_extract(bzf, bzf_len, index, lzma, &tda_len);
  if (!tda) goto done;

  refs = malloc(sizeof(unsigned) * MAX_HINTS);
  if (!refs) goto done;
  int nref = twoda_strrefs(tda, tda_len, refs, MAX_HINTS);
  if (nref <= 0) { log_printf("[hints] loadscreenhints held no StrRefs"); goto done; }

  /* dialog.tlk is 5.4 MB; read only the records we need. */
  unsigned long long tlk = 0;
  unsigned tlk_len = 0;
  if (!obbzip_locate(z, "dialog.tlk", &tlk, &tlk_len)) {
    log_printf("[hints] no dialog.tlk");
    goto done;
  }
  unsigned char head[20];
  if (!obbzip_pread(z, tlk, head, sizeof head) || memcmp(head, "TLK V3.0", 8) != 0) {
    log_printf("[hints] dialog.tlk header is not TLK V3.0");
    goto done;
  }
  unsigned nstr = rd32(head + 12), stroff = rd32(head + 16);

  g_arena = malloc(ARENA_SIZE);
  if (!g_arena) goto done;

  char buf[512];
  for (int i = 0; i < nref && g_n < MAX_HINTS; i++) {
    if (refs[i] >= nstr) continue;
    unsigned char ent[40];
    if (!obbzip_pread(z, tlk + 20 + (unsigned long long)refs[i] * 40, ent, sizeof ent))
      continue;
    unsigned so = rd32(ent + 28), sz = rd32(ent + 32);
    if (!sz || sz >= sizeof buf) continue;
    if (!obbzip_pread(z, tlk + stroff + so, buf, sz)) continue;
    arena_put(buf, sz);
  }

  log_printf("[hints] %d of %d StrRefs resolved from %s in %ums (%u of %u bytes)",
             g_n, nref, path,
             (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000),
             g_used, (unsigned)ARENA_SIZE);

done:
  free(refs);
  free(tda);
  free(bzf);
  free(key);
  obbzip_close(z);
  return g_n;
}
