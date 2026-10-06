/* pixel_ops.c -- the CPU pixel loops of image uploads, vectorised.
 *
 * Loading screens and menu art are uncompressed images of up to 2048x2048.
 * Two loops ran over every pixel of them on the game thread: the 16-bit
 * conversion of each upload (gl_patch.c, GL_TEX16_CONVERT) and, for an image
 * whose sides are not powers of two, the rescale to the size GLU picks
 * (mip_gpu.c). On hardware a 1920x1200 image still took 837 ms through both
 * (real-vita-boot-20261006). Both are written here with NEON. tex16_pack gives
 * exactly the bytes of the scalar loop (tools/test_pixel_ops.c checks it). */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define PIXEL_NEON 1
#endif

#include "pixel_ops.h"

/* Ordered 4x4 Bayer dither (see gl_patch.c, GL_TEX16_CONVERT). */
static const uint8_t k_bayer4[16] = { 0, 8, 2,10, 12, 4,14, 6,  3,11, 1, 9, 15, 7,13, 5 };

void tex16_pack_scalar(uint16_t *dst, const unsigned char *src, int w, int h, int rgba) {
  for (int y = 0; y < h; y++) {
    const unsigned char *sp = src + (size_t)y * w * (rgba ? 4 : 3);
    uint16_t *d = dst + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      unsigned dth = k_bayer4[((y & 3) << 2) | (x & 3)];
      if (rgba) {
        unsigned r4 = (sp[0] + dth) >> 4; if (r4 > 15) r4 = 15;
        unsigned g4 = (sp[1] + dth) >> 4; if (g4 > 15) g4 = 15;
        unsigned b4 = (sp[2] + dth) >> 4; if (b4 > 15) b4 = 15;
        unsigned a4 = (sp[3] + dth) >> 4; if (a4 > 15) a4 = 15;
        d[x] = (uint16_t)((r4 << 12) | (g4 << 8) | (b4 << 4) | a4);
        sp += 4;
      } else {
        unsigned r5 = (sp[0] + (dth >> 1)) >> 3; if (r5 > 31) r5 = 31;
        unsigned g6 = (sp[1] + (dth >> 2)) >> 2; if (g6 > 63) g6 = 63;
        unsigned b5 = (sp[2] + (dth >> 1)) >> 3; if (b5 > 31) b5 = 31;
        d[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        sp += 3;
      }
    }
  }
}

void tex16_pack(uint16_t *dst, const unsigned char *src, int w, int h, int rgba) {
#if PIXEL_NEON
  /* A saturating add then a shift equals the scalar add, shift and clamp:
   * e.g. 250 + 15 saturates to 255, and 255 >> 4 is the clamp's 15. */
  for (int y = 0; y < h; y++) {
    const uint8_t *row = src + (size_t)y * w * (rgba ? 4 : 3);
    uint16_t *d = dst + (size_t)y * w;
    const uint8_t *bay = &k_bayer4[(y & 3) << 2];
    const uint8_t pat[8] = { bay[0], bay[1], bay[2], bay[3], bay[0], bay[1], bay[2], bay[3] };
    uint8x8_t dth = vld1_u8(pat);
    int x = 0;
    if (rgba) {
      for (; x + 8 <= w; x += 8) {
        uint8x8x4_t p = vld4_u8(row + (size_t)x * 4);
        uint8x8_t r = vshr_n_u8(vqadd_u8(p.val[0], dth), 4);
        uint8x8_t g = vshr_n_u8(vqadd_u8(p.val[1], dth), 4);
        uint8x8_t b = vshr_n_u8(vqadd_u8(p.val[2], dth), 4);
        uint8x8_t a = vshr_n_u8(vqadd_u8(p.val[3], dth), 4);
        uint8x8_t hi = vsli_n_u8(g, r, 4), lo = vsli_n_u8(a, b, 4);
        vst1q_u16(d + x, vorrq_u16(vshll_n_u8(hi, 8), vmovl_u8(lo)));
      }
    } else {
      uint8x8_t d1 = vshr_n_u8(dth, 1), d2 = vshr_n_u8(dth, 2);
      for (; x + 8 <= w; x += 8) {
        uint8x8x3_t p = vld3_u8(row + (size_t)x * 3);
        uint8x8_t r = vshr_n_u8(vqadd_u8(p.val[0], d1), 3);
        uint8x8_t g = vshr_n_u8(vqadd_u8(p.val[1], d2), 2);
        uint8x8_t b = vshr_n_u8(vqadd_u8(p.val[2], d1), 3);
        uint16x8_t v = vshlq_n_u16(vshll_n_u8(r, 8), 3);
        v = vorrq_u16(v, vshll_n_u8(g, 5));
        vst1q_u16(d + x, vorrq_u16(v, vmovl_u8(b)));
      }
    }
    if (x < w) {                                       /* the row's last pixels */
      for (; x < w; x++) {
        const uint8_t *sp = row + (size_t)x * (rgba ? 4 : 3);
        unsigned dt = bay[x & 3];
        if (rgba) {
          unsigned r4 = (sp[0] + dt) >> 4; if (r4 > 15) r4 = 15;
          unsigned g4 = (sp[1] + dt) >> 4; if (g4 > 15) g4 = 15;
          unsigned b4 = (sp[2] + dt) >> 4; if (b4 > 15) b4 = 15;
          unsigned a4 = (sp[3] + dt) >> 4; if (a4 > 15) a4 = 15;
          d[x] = (uint16_t)((r4 << 12) | (g4 << 8) | (b4 << 4) | a4);
        } else {
          unsigned r5 = (sp[0] + (dt >> 1)) >> 3; if (r5 > 31) r5 = 31;
          unsigned g6 = (sp[1] + (dt >> 2)) >> 2; if (g6 > 63) g6 = 63;
          unsigned b5 = (sp[2] + (dt >> 1)) >> 3; if (b5 > 31) b5 = 31;
          d[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
      }
    }
  }
#else
  tex16_pack_scalar(dst, src, w, h, rgba);
#endif
}

/* ---- bilinear rescale -------------------------------------------------------
 * Separable: each needed source row is scaled across once (8-bit weights; an
 * RGBA pixel as two 16-bit lanes per 32-bit word), then each destination row
 * blends two such rows (7-bit weights, NEON). Pixel centres map as
 * (x + 0.5) * src / dst - 0.5, clamped to the edges. */

static void scale_row(const uint8_t *src, int sw, uint8_t *dst, int dw, int bpp, const int *x0, const uint8_t *wx) {
  (void)sw;
  if (bpp == 4) {
    for (int x = 0; x < dw; x++) {
      uint32_t a, b;
      memcpy(&a, src + (size_t)x0[x] * 4, 4);
      memcpy(&b, src + (size_t)(x0[x] + 1 < sw ? x0[x] + 1 : x0[x]) * 4, 4);
      uint32_t w1 = wx[x], w0 = 256 - w1;
      uint32_t ev = (((a & 0x00ff00ffu) * w0 + (b & 0x00ff00ffu) * w1 + 0x00800080u) >> 8) & 0x00ff00ffu;
      uint32_t od = ((((a >> 8) & 0x00ff00ffu) * w0 + ((b >> 8) & 0x00ff00ffu) * w1 + 0x00800080u) >> 8) & 0x00ff00ffu;
      uint32_t o = ev | (od << 8);
      memcpy(dst + (size_t)x * 4, &o, 4);
    }
    return;
  }
  for (int x = 0; x < dw; x++) {
    const uint8_t *a = src + (size_t)x0[x] * bpp, *b = src + (size_t)(x0[x] + 1 < sw ? x0[x] + 1 : x0[x]) * bpp;
    uint32_t w1 = wx[x], w0 = 256 - w1;
    for (int k = 0; k < bpp; k++) dst[(size_t)x * bpp + k] = (uint8_t)((a[k] * w0 + b[k] * w1 + 128) >> 8);
  }
}

static void blend_rows(const uint8_t *a, const uint8_t *b, uint8_t *o, int n, int w7) {
  int i = 0;
#if PIXEL_NEON
  int16x8_t wv = vdupq_n_s16((int16_t)w7);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t va = vld1q_u8(a + i), vb = vld1q_u8(b + i);
    int16x8_t dlo = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(vb), vget_low_u8(va)));
    int16x8_t dhi = vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(vb), vget_high_u8(va)));
    int16x8_t lo = vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(va))), vrshrq_n_s16(vmulq_s16(dlo, wv), 7));
    int16x8_t hi = vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(va))), vrshrq_n_s16(vmulq_s16(dhi, wv), 7));
    vst1q_u8(o + i, vcombine_u8(vqmovun_s16(lo), vqmovun_s16(hi)));
  }
#endif
  for (; i < n; i++) {
    int d = (int)b[i] - (int)a[i];
    int v = a[i] + ((d * w7 + 64) >> 7);             /* as vrshrq_n_s16: floor((x + 64) / 128) */
    o[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }
}

int image_rescale(const uint8_t *src, int sw, int sh, int sstride, uint8_t *dst, int dw, int dh, int dstride,
                  int bpp) {
  int *x0 = (int *)malloc((size_t)dw * sizeof *x0);
  uint8_t *wx = (uint8_t *)malloc((size_t)dw);
  uint8_t *ra = (uint8_t *)malloc((size_t)dw * bpp), *rb = (uint8_t *)malloc((size_t)dw * bpp);
  if (!x0 || !wx || !ra || !rb) { free(x0); free(wx); free(ra); free(rb); return 0; }
  for (int x = 0; x < dw; x++) {
    int64_t f = (((int64_t)(2 * x + 1) * sw << 15) / dw) - 0x8000;    /* 16.16 */
    if (f < 0) f = 0;
    x0[x] = (int)(f >> 16);
    if (x0[x] > sw - 1) x0[x] = sw - 1;
    wx[x] = (uint8_t)(((f & 0xffff) + 128) >> 8 > 255 ? 255 : ((f & 0xffff) + 128) >> 8);
  }
  int ya = -1, yb = -1;
  for (int y = 0; y < dh; y++) {
    int64_t f = (((int64_t)(2 * y + 1) * sh << 15) / dh) - 0x8000;
    if (f < 0) f = 0;
    int y0 = (int)(f >> 16), y1;
    if (y0 > sh - 1) y0 = sh - 1;
    y1 = y0 + 1 < sh ? y0 + 1 : y0;
    int w7 = (int)(((f & 0xffff) + 256) >> 9);                      /* 0..128 */
    if (y0 != ya) {
      if (y0 == yb) { uint8_t *t = ra; ra = rb; rb = t; ya = yb; yb = -1; }
      else { scale_row(src + (size_t)y0 * sstride, sw, ra, dw, bpp, x0, wx); ya = y0; }
    }
    if (y1 != yb) { scale_row(src + (size_t)y1 * sstride, sw, rb, dw, bpp, x0, wx); yb = y1; }
    blend_rows(ra, rb, dst + (size_t)y * dstride, dw * bpp, w7);
  }
  free(x0); free(wx); free(ra); free(rb);
  return 1;
}
