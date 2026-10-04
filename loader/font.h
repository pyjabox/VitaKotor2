/* font.h -- the game's own bitmap font, for the boot screen's text.
 *
 * dialogfont16x16b.tga is a 256x256 atlas of 16x16 cells indexed straight by
 * byte value, and the matching .txi -- which the VPK already ships at
 * app0:fonts/ for the game's own use -- carries per-glyph UVs. Those make the
 * font proportional: 'i' is 3 pixels wide, 'W' is 15, everything 16 tall. So
 * there is no metrics table to invent and nothing to hardcode.
 *
 * Text is the reason this exists at all: the boot screen wants the word
 * LOADING and a rotating hint line, and the game's FreeType path is not usable
 * that early.
 *
 * Drawing assumes the caller has already set up a top-down ortho projection
 * over the framebuffer, as loadscreen.c's art path does. Every entry point is a
 * no-op when the font failed to load. */

#ifndef __FONT_H__
#define __FONT_H__

#include "obbzip.h"

/* Load the atlas from an open KOTOR II patch OBB and metrics from app0:fonts/.
 * 1 on success; 0 leaves every other call inert. */
int font_load(ObbZip *z);

/* Release the atlas texture. Call while the loader still owns GL. */
int font_ready(void);
void font_free(void);

/* Width in pixels of the first `n` bytes of `s` at scale 1. Pass n < 0 for the
 * whole string. */
int font_measure(const char *s, int n);

/* Draw one line with its left edge at x and its top at y, in screen pixels. */
void font_draw(const char *s, int n, float x, float y, float scale,
               float r, float g, float b, float a);

/* Break `s` on spaces so no line exceeds `max_w` pixels at `scale`, and draw it
 * centred horizontally on `cx`, starting with its first line's top at `y`.
 * Returns the number of lines drawn. */
int font_draw_wrapped(const char *s, float cx, float y, float max_w, float scale,
                      float r, float g, float b, float a);

/* Line advance in pixels at the given scale, including leading. */
float font_line_height(float scale);

#endif
