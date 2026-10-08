/* k2res.c -- see k2res.h. All formats are little-endian byte streams with no
 * alignment guarantees, so fields are assembled from bytes. */

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "k2res.h"

#define TABLE_MAX   (16u << 20)    /* OBB table, inflated: 1.6 MB in the patch OBB */
#define MEMBER_MAX  (24u << 20)    /* anything read whole */

static uint32_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

void *k2_read(SceUID fd, uint64_t off, uint32_t len) {
  if (fd < 0 || len == 0 || len > MEMBER_MAX) return NULL;
  uint8_t *b = malloc(len);
  if (!b) return NULL;
  if (sceIoPread(fd, b, len, (SceOff)off) != (int)len) { free(b); return NULL; }
  return b;
}

int k2obb_scan(SceUID fd, k2obb_fn fn, void *ctx) {
  SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
  uint8_t tail[16];
  if (size < 32 || sceIoPread(fd, tail, 16, size - 16) != 16) return -1;
  uint64_t off = u64(tail), raw = u64(tail + 8);
  if (off >= (uint64_t)size - 16 || raw < 8 || raw > TABLE_MAX) return -1;
  uint64_t zlen = (uint64_t)size - 16 - off;
  if (zlen > TABLE_MAX) return -1;
  uint8_t *z = k2_read(fd, off, (uint32_t)zlen), *t = malloc((size_t)raw);
  uLongf got = (uLongf)raw;
  int ok = z && t && uncompress(t, &got, z, (uLong)zlen) == Z_OK && got == raw;
  free(z);
  if (!ok) { free(t); return -1; }
  uint64_t n = u64(t), p = 8;
  int visited = 0;
  for (uint64_t i = 0; i < n; i++) {
    if (p + 8 > raw) break;
    uint64_t nl = u64(t + p);
    if (nl > 1024 || p + 8 + nl + 24 > raw) break;
    const char *name = (const char *)t + p + 8;
    uint64_t moff = u64(t + p + 8 + nl), msize = u64(t + p + 16 + nl);
    p += 8 + nl + 24;
    visited++;
    if (fn(name, (unsigned)nl, moff, msize, ctx)) break;
  }
  free(t);
  return visited;
}

int k2erf_scan(SceUID fd, uint64_t base, k2erf_fn fn, void *ctx) {
  uint8_t h[32];
  if (sceIoPread(fd, h, 32, (SceOff)base) != 32) return -1;
  if (memcmp(h + 4, "V1.0", 4)) return -1;
  uint32_t n = u32(h + 16), okey = u32(h + 24), ores = u32(h + 28);
  if (n == 0 || n > 65536) return -1;
  uint8_t *keys = k2_read(fd, base + okey, n * 24), *res = k2_read(fd, base + ores, n * 8);
  int visited = -1;
  if (keys && res) {
    visited = 0;
    for (uint32_t i = 0; i < n; i++) {
      char rr[17];
      for (int k = 0; k < 16; k++) {
        char c = (char)keys[i * 24 + k];
        rr[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
      }
      rr[16] = 0;
      visited++;
      if (fn(rr, u16(keys + i * 24 + 20), base + u32(res + i * 8), u32(res + i * 8 + 4), ctx)) break;
    }
  }
  free(keys);
  free(res);
  return visited;
}

/* ---- textures ---------------------------------------------------------------- */

static void rgb565(uint32_t c, uint8_t *o) {
  o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
  o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
  o[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* One 4x4 colour block (DXT1 layout, also the second half of a DXT5 block). */
static void color_block(const uint8_t *b, uint8_t pal[4][4], int dxt1) {
  uint32_t c0 = u16(b), c1 = u16(b + 2);
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  for (int k = 0; k < 3; k++) {
    if (!dxt1 || c0 > c1) {
      pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
    } else {
      pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = (!dxt1 || c0 > c1) ? 255 : 0;
}

/* `dxt` 1 or 5. Output rows are flipped: TPC stores them bottom-up. */
static void dxt_decode(const uint8_t *src, int w, int h, int dxt, uint8_t *dst) {
  int bs = dxt == 1 ? 8 : 16;
  for (int by = 0; by < h; by += 4)
    for (int bx = 0; bx < w; bx += 4, src += bs) {
      uint8_t pal[4][4], al[8];
      uint64_t abits = 0;
      const uint8_t *cb = src;
      if (dxt == 5) {
        al[0] = src[0];
        al[1] = src[1];
        for (int i = 2; i < 8; i++)
          al[i] = al[0] > al[1] ? (uint8_t)(((8 - i) * al[0] + (i - 1) * al[1]) / 7)
                : i < 6         ? (uint8_t)(((6 - i) * al[0] + (i - 1) * al[1]) / 5)
                                : (uint8_t)(i == 6 ? 0 : 255);
        for (int i = 0; i < 6; i++) abits |= (uint64_t)src[2 + i] << (8 * i);
        cb = src + 8;
      }
      color_block(cb, pal, dxt == 1);
      uint32_t cbits = u32(cb + 4);
      for (int j = 0; j < 16; j++) {
        int x = bx + (j & 3), y = by + (j >> 2);
        if (x >= w || y >= h) continue;
        uint8_t *o = dst + ((size_t)(h - 1 - y) * w + x) * 4;
        memcpy(o, pal[(cbits >> (2 * j)) & 3], 4);
        if (dxt == 5) o[3] = al[(abits >> (3 * j)) & 7];
      }
    }
}

uint8_t *k2tpc_rgba(const uint8_t *t, uint32_t len, int *pw, int *ph) {
  if (!t || len < 128) return NULL;
  uint32_t dsz = u32(t), w = u16(t + 8), h = u16(t + 10), enc = t[12];
  if (w == 0 || h == 0 || w > 2048 || h > 2048) return NULL;
  uint32_t need = dsz ? dsz : w * h * (enc == 4 ? 4 : enc == 2 ? 3 : 1);
  if (128 + (uint64_t)need > len) return NULL;
  uint8_t *o = malloc((size_t)w * h * 4);
  if (!o) return NULL;
  const uint8_t *s = t + 128;
  if (dsz && enc == 4 && dsz >= w * h) dxt_decode(s, (int)w, (int)h, 5, o);
  else if (dsz && enc == 2 && dsz >= w * h / 2) dxt_decode(s, (int)w, (int)h, 1, o);
  else if (!dsz) {
    int bpp = enc == 4 ? 4 : enc == 2 ? 3 : 1;
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t *p = s + ((size_t)y * w + x) * bpp;
        uint8_t *q = o + ((size_t)(h - 1 - y) * w + x) * 4;
        q[0] = p[0];
        q[1] = bpp >= 3 ? p[1] : p[0];
        q[2] = bpp >= 3 ? p[2] : p[0];
        q[3] = bpp == 4 ? p[3] : 255;
      }
  } else {
    free(o);
    return NULL;
  }
  *pw = (int)w;
  *ph = (int)h;
  return o;
}

/* ---- KEY / BIF / 2DA / TLK ------------------------------------------------------ */

int k2key_find(const uint8_t *k, uint32_t len, const char *resref, unsigned type, char *bif, int bif_sz,
               uint32_t *index) {
  if (len < 64 || memcmp(k, "KEY V1  ", 8)) return 0;
  uint32_t nbif = u32(k + 8), nkey = u32(k + 12), ofile = u32(k + 16), okey = u32(k + 20);
  if ((uint64_t)okey + (uint64_t)nkey * 22 > len || (uint64_t)ofile + (uint64_t)nbif * 12 > len) return 0;
  size_t rl = strlen(resref);
  if (rl > 16) return 0;
  for (uint32_t i = 0; i < nkey; i++) {
    const uint8_t *e = k + okey + i * 22;
    if (u16(e + 16) != type || strncasecmp((const char *)e, resref, rl) || (rl < 16 && e[rl])) continue;
    uint32_t id = u32(e + 18), b = id >> 20;
    if (b >= nbif) return 0;
    uint32_t noff = u32(k + ofile + b * 12 + 4), nlen = u16(k + ofile + b * 12 + 8);
    if ((uint64_t)noff + nlen > len || (int)nlen >= bif_sz) return 0;
    for (uint32_t c = 0; c < nlen; c++) bif[c] = k[noff + c] == '\\' ? '/' : (char)k[noff + c];
    bif[nlen] = 0;
    *index = id & 0xfffff;
    return 1;
  }
  return 0;
}

uint8_t *k2bif_read(SceUID fd, uint64_t base, uint32_t index, uint32_t *len) {
  uint8_t h[20];
  if (sceIoPread(fd, h, 20, (SceOff)base) != 20 || memcmp(h, "BIFFV1  ", 8)) return NULL;
  uint32_t nvar = u32(h + 8), ovar = u32(h + 16);
  if (nvar == 0 || nvar > 1u << 20) return NULL;
  uint8_t e[16];
  /* The table is normally in index order; scan if not. */
  int found = index < nvar && sceIoPread(fd, e, 16, (SceOff)(base + ovar + (uint64_t)index * 16)) == 16 &&
              (u32(e) & 0xfffff) == index;
  for (uint32_t i = 0; !found && i < nvar; i++)
    found = sceIoPread(fd, e, 16, (SceOff)(base + ovar + (uint64_t)i * 16)) == 16 && (u32(e) & 0xfffff) == index;
  if (!found) return NULL;
  *len = u32(e + 8);
  return k2_read(fd, base + u32(e + 4), *len);
}

int k2_2da_ints(const uint8_t *d, uint32_t len, const char *column, int *out, int max) {
  if (len < 16 || memcmp(d, "2DA V2.b\n", 9)) return 0;
  uint32_t p = 9, ncol = 0, want = (uint32_t)-1;
  while (p < len && d[p]) {                              /* tab-terminated labels, then \0 */
    uint32_t s = p;
    while (p < len && d[p] != '\t' && d[p]) p++;
    if (p - s == strlen(column) && !strncasecmp((const char *)d + s, column, p - s)) want = ncol;
    ncol++;
    if (p < len && d[p] == '\t') p++;
  }
  p++;
  if (want == (uint32_t)-1 || p + 4 > len) return 0;
  uint32_t nrow = u32(d + p);
  p += 4;
  for (uint32_t r = 0; r < nrow; r++) {                  /* row labels */
    while (p < len && d[p] != '\t') p++;
    p++;
  }
  uint64_t cells = (uint64_t)nrow * ncol;
  if (p + cells * 2 + 2 > len) return 0;
  uint32_t offs = p, data = p + (uint32_t)cells * 2 + 2;
  int n = 0;
  for (uint32_t r = 0; r < nrow && n < max; r++) {
    uint32_t o = data + u16(d + offs + (r * ncol + want) * 2);
    if (o >= len || d[o] < '0' || d[o] > '9') continue;
    out[n++] = atoi((const char *)d + o);
  }
  return n;
}

/* cp1252 0x80-0x9F; the rest of the upper half is Latin-1. */
static const uint16_t k1252[32] = {
  0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D,
  0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
  0x0153, 0x009D, 0x017E, 0x0178};

int k2tlk_get(SceUID fd, uint64_t base, int strref, char *out, int out_sz) {
  uint8_t h[20], e[40];
  out[0] = 0;
  if (strref < 0 || sceIoPread(fd, h, 20, (SceOff)base) != 20 || memcmp(h, "TLK V3.0", 8)) return 0;
  if ((uint32_t)strref >= u32(h + 12)) return 0;
  if (sceIoPread(fd, e, 40, (SceOff)(base + 20 + (uint64_t)strref * 40)) != 40 || !(u32(e) & 1)) return 0;
  uint32_t off = u32(e + 28), size = u32(e + 32);
  if (size == 0 || size > 4096) return 0;
  uint8_t *s = k2_read(fd, base + u32(h + 16) + off, size);
  if (!s) return 0;
  int n = 0;
  for (uint32_t i = 0; i < size && n < out_sz - 4; i++) {
    uint32_t c = s[i];
    if (c >= 0x80 && c < 0xA0) c = k1252[c - 0x80];
    if (c < 0x80) out[n++] = (char)c;
    else if (c < 0x800) { out[n++] = (char)(0xC0 | c >> 6); out[n++] = (char)(0x80 | (c & 63)); }
    else { out[n++] = (char)(0xE0 | c >> 12); out[n++] = (char)(0x80 | ((c >> 6) & 63)); out[n++] = (char)(0x80 | (c & 63)); }
  }
  out[n] = 0;
  free(s);
  return n;
}
