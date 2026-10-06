/* pixel_ops.h -- vectorised pixel loops of image uploads (pixel_ops.c). */

#ifndef __PIXEL_OPS_H__
#define __PIXEL_OPS_H__

#include <stdint.h>

/* w*h 16-bit pixels from tightly packed 8-bit RGBA (rgba=1: RGBA4444) or RGB
 * (rgba=0: RGB565), with the 4x4 ordered dither. tex16_pack_scalar is the
 * reference loop. */
void tex16_pack(uint16_t *dst, const unsigned char *src, int w, int h, int rgba);
void tex16_pack_scalar(uint16_t *dst, const unsigned char *src, int w, int h, int rgba);

/* Bilinear rescale of an 8-bit image (bpp 1-4 bytes per pixel), rows at the
 * given strides in bytes. 0 if out of memory. */
int image_rescale(const uint8_t *src, int sw, int sh, int sstride, uint8_t *dst, int dw, int dh, int dstride,
                  int bpp);

#endif
