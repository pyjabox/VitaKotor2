/* tga.h -- the small corner of the TGA format KOTOR's GUI art actually uses.
 *
 * Not a general decoder. It handles exactly what the boot screen reads: type 2
 * (uncompressed truecolour) for the load_*.tga backgrounds and the font atlas,
 * and type 10 (run-length) for the logo, both at 24 or 32 bits. No colour maps,
 * no 16-bit, no greyscale.
 *
 * Output is always RGBA8888 in top-down row order, whatever the file's origin
 * bit says, so callers never have to think about which way up a texture is. */

#ifndef __TGA_H__
#define __TGA_H__

/* Decode `len` bytes of TGA into a malloc'd RGBA8888 buffer the caller owns,
 * writing the dimensions to *w and *h. NULL if the data is not a TGA this
 * understands; the reason is logged. */
unsigned char *tga_decode(const void *data, unsigned len, int *w, int *h);

#endif
