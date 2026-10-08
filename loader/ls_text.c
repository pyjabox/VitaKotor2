/* ls_text.c -- see ls_text.h. The game's mobile UI draws its text with
 * FreeType from assets/iosdialog.otf; this uses the same font. */

#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "ls_text.h"

#define MAX_CHARS 1024
#define MAX_LINES 8

static FT_Library s_lib;
static FT_Face s_face;

int ls_text_open(const char *path) {
  if (s_face) return 1;
  if (FT_Init_FreeType(&s_lib)) return 0;
  if (FT_New_Face(s_lib, path, 0, &s_face)) {
    FT_Done_FreeType(s_lib);
    s_lib = NULL;
    s_face = NULL;
    return 0;
  }
  return 1;
}

void ls_text_close(void) {
  if (s_face) FT_Done_Face(s_face);
  if (s_lib) FT_Done_FreeType(s_lib);
  s_face = NULL;
  s_lib = NULL;
}

static int utf8_decode(const char *s, uint32_t *cp, int max) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && n < max) {
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    cp[n++] = c;
  }
  return n;
}

static int advance(uint32_t c) {
  if (FT_Load_Char(s_face, c, FT_LOAD_DEFAULT)) return 0;
  return (int)(s_face->glyph->advance.x >> 6);
}

uint8_t *ls_text_image(const char *utf8, int px, int max_w, int line_h, int *pw, int *ph) {
  if (!s_face || !utf8 || px <= 0 || max_w <= 0) return NULL;
  if (FT_Set_Pixel_Sizes(s_face, 0, (FT_UInt)px)) return NULL;
  static uint32_t cp[MAX_CHARS];
  static int adv[MAX_CHARS];
  int n = utf8_decode(utf8, cp, MAX_CHARS);
  for (int i = 0; i < n; i++) adv[i] = advance(cp[i]);

  /* Greedy wrap at spaces; a word wider than the line is cut. */
  int start[MAX_LINES], end[MAX_LINES], width[MAX_LINES], lines = 0, i = 0;
  while (i < n && lines < MAX_LINES) {
    while (i < n && cp[i] == ' ') i++;
    if (i >= n) break;
    int s = i, w = 0, last_space = -1, w_at_space = 0;
    for (; i < n && cp[i] != '\n'; i++) {
      if (cp[i] == ' ') { last_space = i; w_at_space = w; }
      if (w + adv[i] > max_w && i > s) {
        if (last_space > s) { i = last_space; w = w_at_space; }
        break;
      }
      w += adv[i];
    }
    start[lines] = s;
    end[lines] = i;
    width[lines] = w;
    lines++;
    if (i < n && cp[i] == '\n') i++;
  }
  if (lines == 0) return NULL;

  int img_w = 0;
  for (int l = 0; l < lines; l++)
    if (width[l] > img_w) img_w = width[l];
  img_w += 2;
  int img_h = line_h * (lines - 1) + px + px / 3;
  int ascent = (int)(s_face->size->metrics.ascender >> 6);
  if (ascent <= 0 || ascent > px) ascent = px * 4 / 5;
  uint8_t *a = calloc((size_t)img_w, (size_t)img_h);
  if (!a) return NULL;

  for (int l = 0; l < lines; l++) {
    int x = (img_w - width[l]) / 2, base = l * line_h + ascent;
    for (int k = start[l]; k < end[l]; k++) {
      if (FT_Load_Char(s_face, cp[k], FT_LOAD_RENDER)) continue;
      FT_GlyphSlot g = s_face->glyph;
      FT_Bitmap *b = &g->bitmap;
      for (unsigned r = 0; r < b->rows; r++) {
        int y = base - g->bitmap_top + (int)r;
        if (y < 0 || y >= img_h) continue;
        for (unsigned c = 0; c < b->width; c++) {
          int xx = x + g->bitmap_left + (int)c;
          if (xx < 0 || xx >= img_w) continue;
          uint8_t v = b->buffer[r * b->pitch + c], *d = a + (size_t)y * img_w + xx;
          if (v > *d) *d = v;
        }
      }
      x += (int)(g->advance.x >> 6);
    }
  }

  uint8_t *rgba = malloc((size_t)img_w * img_h * 4);
  if (rgba)
    for (size_t p = 0; p < (size_t)img_w * img_h; p++) {
      rgba[p * 4] = rgba[p * 4 + 1] = rgba[p * 4 + 2] = 255;
      rgba[p * 4 + 3] = a[p];
    }
  free(a);
  if (!rgba) return NULL;
  *pw = img_w;
  *ph = img_h;
  return rgba;
}
