/* test_pixel_ops.c -- host check of loader/pixel_ops.c.
 *   cc -O2 -Iloader tools/test_pixel_ops.c loader/pixel_ops.c -lm && ./a.out
 * On an ARM host the NEON paths run. tex16_pack must equal the scalar loop
 * byte for byte; image_rescale must stay within 2 of a float bilinear (8-bit
 * horizontal and 7-bit vertical weights). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pixel_ops.h"

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int ref_rescale_diff(const unsigned char *src, int sw, int sh, int ss, const unsigned char *dst, int dw, int dh,
                            int ds, int bpp) {
  int worst = 0;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      double fx = (x + 0.5) * sw / dw - 0.5, fy = (y + 0.5) * sh / dh - 0.5;
      if (fx < 0) fx = 0;
      if (fy < 0) fy = 0;
      int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
      if (x0 > sw - 1) x0 = sw - 1;
      if (y0 > sh - 1) y0 = sh - 1;
      double ax = fx - x0, ay = fy - y0;
      for (int k = 0; k < bpp; k++) {
        double v = (1 - ay) * ((1 - ax) * src[y0 * ss + x0 * bpp + k] + ax * src[y0 * ss + x1 * bpp + k]) +
                   ay * ((1 - ax) * src[y1 * ss + x0 * bpp + k] + ax * src[y1 * ss + x1 * bpp + k]);
        int d = abs((int)lround(v) - dst[y * ds + x * bpp + k]);
        if (d > worst) worst = d;
      }
    }
  return worst;
}

int main(void) {
  int fails = 0;
  static const int widths[] = { 1, 7, 8, 9, 15, 16, 17, 64, 255, 1023, 1024 };
  for (int rgba = 0; rgba < 2; rgba++)
    for (unsigned i = 0; i < sizeof widths / sizeof widths[0]; i++) {
      int w = widths[i], h = 37, bpp = rgba ? 4 : 3;
      unsigned char *src = malloc((size_t)w * h * bpp);
      for (int k = 0; k < w * h * bpp; k++) src[k] = (unsigned char)(k % 7 == 0 ? 255 - (rnd() & 15) : rnd());
      uint16_t *a = malloc((size_t)w * h * 2), *b = malloc((size_t)w * h * 2);
      tex16_pack(a, src, w, h, rgba);
      tex16_pack_scalar(b, src, w, h, rgba);
      if (memcmp(a, b, (size_t)w * h * 2)) { printf("FAIL tex16 %s w=%d\n", rgba ? "4444" : "565", w); fails++; }
      free(src); free(a); free(b);
    }
  printf("tex16_pack: %s\n", fails ? "MISMATCH" : "identical to the scalar loop on every case");

  static const int cases[][5] = { {1920, 1200, 2048, 1024, 4}, {1100, 655, 1024, 512, 4}, {1023, 1024, 1024, 1024, 3},
                                  {600, 597, 512, 512, 4}, {1024, 767, 1024, 512, 4}, {33, 17, 32, 16, 2},
                                  {5, 3, 4, 4, 1} };
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    int sw = cases[c][0], sh = cases[c][1], dw = cases[c][2], dh = cases[c][3], bpp = cases[c][4];
    int ss = (sw * bpp + 3) / 4 * 4, ds = (dw * bpp + 3) / 4 * 4;
    unsigned char *src = malloc((size_t)ss * sh), *dst = malloc((size_t)ds * dh);
    for (int y = 0; y < sh; y++)
      for (int x = 0; x < sw * bpp; x++) src[y * ss + x] = (unsigned char)((x * 3 + y * 5 + (rnd() & 31)) & 255);
    double t0 = now();
    image_rescale(src, sw, sh, ss, dst, dw, dh, ds, bpp);
    double t1 = now();
    int worst = ref_rescale_diff(src, sw, sh, ss, dst, dw, dh, ds, bpp);
    printf("rescale %dx%d -> %dx%d bpp %d: worst diff %d, %.1f ms on this host\n", sw, sh, dw, dh, bpp, worst,
           (t1 - t0) * 1000);
    if (worst > 2) fails++;
    free(src); free(dst);
  }
  printf("%s\n", fails ? "FAILED" : "all OK");
  return fails != 0;
}
