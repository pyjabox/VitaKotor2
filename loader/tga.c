/* tga.c -- see tga.h. */

#include "tga.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define HDR 18

/* Same reasoning as obbzip's cap: a bad header should yield NULL, not a
 * multi-hundred-megabyte allocation during boot. */
#define MAX_DIM 4096

/* Write one source pixel to its place in the output.
 *
 * `idx` counts pixels in the order the file stores them, which lets the plain
 * and run-length paths share this and stay indifferent to row order -- an RLE
 * run is allowed to straddle a scanline boundary, so decoding row by row is
 * wrong for exactly the files that need it most. */
static void put_px(unsigned char *out, unsigned w, unsigned h, int top_down,
                   unsigned idx, const unsigned char *s, unsigned bpp) {
  unsigned y = idx / w, x = idx % w;
  if (y >= h) return;
  unsigned char *d = out + ((size_t)(top_down ? y : h - 1 - y) * w + x) * 4;
  d[0] = s[2];                          /* TGA stores BGR(A) */
  d[1] = s[1];
  d[2] = s[0];
  d[3] = (bpp == 32) ? s[3] : 0xff;
}

unsigned char *tga_decode(const void *data, unsigned len, int *w, int *h) {
  const unsigned char *p = data;
  if (!p || len < HDR) return NULL;

  unsigned id_len = p[0];
  unsigned cmap   = p[1];
  unsigned type   = p[2];
  unsigned width  = p[12] | (p[13] << 8);
  unsigned height = p[14] | (p[15] << 8);
  unsigned bpp    = p[16];
  unsigned desc   = p[17];

  if ((type != 2 && type != 10) || cmap != 0) {
    log_printf("[tga] type %u cmap %u not supported", type, cmap);
    return NULL;
  }
  if (bpp != 24 && bpp != 32) {
    log_printf("[tga] %u bpp not supported", bpp);
    return NULL;
  }
  if (!width || !height || width > MAX_DIM || height > MAX_DIM) {
    log_printf("[tga] implausible size %ux%u", width, height);
    return NULL;
  }

  unsigned pxb = bpp / 8;
  unsigned total = width * height;
  if (len < HDR + id_len) return NULL;
  const unsigned char *src = p + HDR + id_len;
  const unsigned char *end = p + len;

  /* Bit 5 set means the first row stored is the top one. TGA's default is
   * bottom-up, which is what every KOTOR asset uses. */
  int top_down = (desc & 0x20) != 0;

  unsigned char *out = malloc((size_t)total * 4);
  if (!out) return NULL;

  if (type == 2) {
    if ((unsigned)(end - src) < total * pxb) {
      log_printf("[tga] truncated: %u bytes, need %u", len, HDR + id_len + total * pxb);
      free(out);
      return NULL;
    }
    for (unsigned i = 0; i < total; i++, src += pxb)
      put_px(out, width, height, top_down, i, src, bpp);
  } else {
    unsigned i = 0;
    while (i < total) {
      if (src >= end) goto truncated;
      unsigned hdr = *src++;
      unsigned count = (hdr & 0x7f) + 1;
      if (count > total - i) count = total - i;   /* trust the header, not the file */

      if (hdr & 0x80) {                           /* run: one pixel, repeated */
        if ((unsigned)(end - src) < pxb) goto truncated;
        for (unsigned n = 0; n < count; n++)
          put_px(out, width, height, top_down, i + n, src, bpp);
        src += pxb;
      } else {                                    /* literal run */
        if ((unsigned)(end - src) < count * pxb) goto truncated;
        for (unsigned n = 0; n < count; n++, src += pxb)
          put_px(out, width, height, top_down, i + n, src, bpp);
      }
      i += count;
    }
  }

  *w = (int)width;
  *h = (int)height;
  return out;

truncated:
  log_printf("[tga] RLE data ran out %u pixels short of %ux%u", total, width, height);
  free(out);
  return NULL;
}
