/* ls_text.h -- text for the boot loading screen (ls_text.c), rendered with
 * FreeType from the game's own UI font into RGBA images: white, with the
 * glyph coverage in alpha, to be tinted when drawn. */

#ifndef __LS_TEXT_H__
#define __LS_TEXT_H__

#include <stdint.h>

/* 1 if the font opened. */
int ls_text_open(const char *path);
void ls_text_close(void);

/* UTF-8 text at `px` pixels, wrapped to `max_w` (words longer than that are
 * cut), each line centred, `line_h` pixels apart. malloc'd RGBA; *w and *h
 * are the image size. NULL if nothing could be drawn. */
uint8_t *ls_text_image(const char *utf8, int px, int max_w, int line_h, int *w, int *h);

#endif
