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

/* Rendered glyphs, kept while the font is open. Each FT_Load_Char of this CFF
 * font costs real time on the Vita, and the boot menu and the loading screen
 * draw the same few dozen letters at a handful of sizes over and over: the
 * menu re-rendering eight rows of uncached text took most of a second per
 * button press on hardware (2026-10-13). */
#define GLYPHS 1024                /* power of two */
typedef struct { uint32_t cp; int px, used, left, top, adv; unsigned w, h; uint8_t *bits; } Glyph;
static Glyph s_glyph[GLYPHS];
static int s_nglyph;

static void glyphs_clear(void) {
  for (int i = 0; i < GLYPHS; i++) free(s_glyph[i].bits);
  memset(s_glyph, 0, sizeof s_glyph);
  s_nglyph = 0;
}

/* `cp` at the size last set with FT_Set_Pixel_Sizes, which is `px`. */
static const Glyph *glyph(uint32_t cp, int px) {
  if (s_nglyph >= GLYPHS / 2) glyphs_clear();      /* keep probes short; refill as used */
  uint32_t h = (cp * 2654435761u + (uint32_t)px * 40503u) & (GLYPHS - 1);
  Glyph *g = &s_glyph[h];
  while (g->used && !(g->cp == cp && g->px == px)) g = &s_glyph[(++h) & (GLYPHS - 1)];
  if (g->used) return g;
  g->used = 1;
  g->cp = cp;
  g->px = px;
  s_nglyph++;
  if (FT_Load_Char(s_face, cp, FT_LOAD_RENDER)) return g;      /* nothing to draw, no advance */
  FT_GlyphSlot slot = s_face->glyph;
  FT_Bitmap *b = &slot->bitmap;
  g->left = slot->bitmap_left;
  g->top = slot->bitmap_top;
  g->adv = (int)(slot->advance.x >> 6);
  if (b->width && b->rows && (g->bits = malloc((size_t)b->width * b->rows))) {
    g->w = b->width;
    g->h = b->rows;
    for (unsigned r = 0; r < b->rows; r++) memcpy(g->bits + (size_t)r * b->width, b->buffer + (size_t)r * b->pitch, b->width);
  }
  return g;
}

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
  glyphs_clear();
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


uint8_t *ls_text_image(const char *utf8, int px, int max_w, int line_h, int *pw, int *ph) {
  if (!s_face || !utf8 || px <= 0 || max_w <= 0) return NULL;
  if (FT_Set_Pixel_Sizes(s_face, 0, (FT_UInt)px)) return NULL;
  static uint32_t cp[MAX_CHARS];
  static int adv[MAX_CHARS];
  int n = utf8_decode(utf8, cp, MAX_CHARS);
  for (int i = 0; i < n; i++) adv[i] = glyph(cp[i], px)->adv;

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
      const Glyph *g = glyph(cp[k], px);
      for (unsigned r = 0; r < g->h; r++) {
        int y = base - g->top + (int)r;
        if (y < 0 || y >= img_h) continue;
        for (unsigned c = 0; c < g->w; c++) {
          int xx = x + g->left + (int)c;
          if (xx < 0 || xx >= img_w) continue;
          uint8_t v = g->bits[r * g->w + c], *d = a + (size_t)y * img_w + xx;
          if (v > *d) *d = v;
        }
      }
      x += g->adv;
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
