/* font.c -- see font.h. */

#include <vitasdk.h>
#include <vitaGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "gl_state_filter.h"
#include "config.h"
#include "log.h"
#include "tga.h"

#define GLYPHS      256
#define ATLAS_PX    256.0f
#define CELL_PX     16.0f
#define LINE_GAP    1.15f     /* leading, as a multiple of the glyph box */
#define MAX_QUADS   320       /* one draw call's worth of characters */

typedef struct { float s0, t0, s1, t1, w; } Glyph;

static GLuint g_tex;
static Glyph  g_glyph[GLYPHS];
static int    g_ready;

/* Six vertices a glyph, accumulated across every line of a wrapped block and
 * handed to vitaGL in one glDrawArrays. Batching is not just for speed: whether
 * vitaGL copies client vertex data or lets the GPU read it in place depends on
 * how it was built (DRAW_SPEEDHACK), and a single flush per call means it never
 * matters which. */
static GLfloat g_pos[MAX_QUADS * 12];
static GLfloat g_uv[MAX_QUADS * 12];
static int     g_quads;

/* ---- metrics --------------------------------------------------------------- */

/* The .txi is plain text: a few key/value lines, then "upperleftcoords N"
 * followed by N rows of "u v z", then "lowerrightcoords N" and N more. The v
 * axis runs from the bottom, while tga_decode hands back rows from the top, so
 * every v becomes 1-v on the way in. */
static int txi_parse(const char *txt, unsigned len) {
  const char *p = txt, *end = txt + len;
  float ul[GLYPHS][2], lr[GLYPHS][2];
  int have_ul = 0, have_lr = 0;

  while (p < end) {
    const char *line = p;
    while (p < end && *p != '\n') p++;
    unsigned n = (unsigned)(p - line);
    if (p < end) p++;
    if (n == 0) continue;

    float (*dst)[2] = NULL;
    if (n >= 15 && memcmp(line, "upperleftcoords", 15) == 0) dst = ul;
    else if (n >= 16 && memcmp(line, "lowerrightcoords", 16) == 0) dst = lr;
    if (!dst) continue;

    int got = 0;
    while (got < GLYPHS && p < end) {
      const char *row = p;
      while (p < end && *p != '\n') p++;
      if (p < end) p++;
      float u = 0.0f, v = 0.0f;
      if (sscanf(row, "%f %f", &u, &v) != 2) break;
      dst[got][0] = u;
      dst[got][1] = v;
      got++;
    }
    if (dst == ul) have_ul = got; else have_lr = got;
  }

  if (have_ul < GLYPHS || have_lr < GLYPHS) {
    log_printf("[font] .txi gave %d upper-left and %d lower-right coords, need %d",
               have_ul, have_lr, GLYPHS);
    return 0;
  }

  for (int i = 0; i < GLYPHS; i++) {
    g_glyph[i].s0 = ul[i][0];
    g_glyph[i].s1 = lr[i][0];
    g_glyph[i].t0 = 1.0f - ul[i][1];
    g_glyph[i].t1 = 1.0f - lr[i][1];
    g_glyph[i].w  = (lr[i][0] - ul[i][0]) * ATLAS_PX;
    if (g_glyph[i].w < 0.0f) g_glyph[i].w = 0.0f;
  }
  return 1;
}

/* ---- public ---------------------------------------------------------------- */

int font_ready(void) { return g_ready; }

int font_load(ObbZip *z) {
  if (g_ready) return 1;

  SceUID fd = sceIoOpen(FONT_TXI_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) { log_printf("[font] no %s", FONT_TXI_PATH); return 0; }
  SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  char *txi = (sz > 0 && sz < 256 * 1024) ? malloc((size_t)sz + 1) : NULL;
  int ok = txi && sceIoRead(fd, txi, (unsigned)sz) == (int)sz;
  sceIoClose(fd);
  if (ok) { txi[sz] = '\0'; ok = txi_parse(txi, (unsigned)sz); }
  free(txi);
  if (!ok) return 0;

  unsigned len = 0;
  void *raw = obbzip_read(z, FONT_TGA_ENTRY, &len);
  if (!raw) { log_printf("[font] no %s in patch.obb", FONT_TGA_ENTRY); return 0; }
  int w = 0, h = 0;
  unsigned char *rgba = tga_decode(raw, len, &w, &h);
  free(raw);
  if (!rgba) return 0;

  glGenTextures(1, &g_tex);
  glBindTexture(GL_TEXTURE_2D, g_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  free(rgba);

  g_ready = 1;
  log_printf("[font] %s %dx%d -> tex %u", FONT_TGA_ENTRY, w, h, (unsigned)g_tex);
  return 1;
}

void font_free(void) {
  if (!g_ready) return;
  glDeleteTextures(1, &g_tex);
  g_tex = 0;
  g_ready = 0;
}

float font_line_height(float scale) { return CELL_PX * LINE_GAP * scale; }

int font_measure(const char *s, int n) {
  if (!g_ready || !s) return 0;
  if (n < 0) n = (int)strlen(s);
  float w = 0.0f;
  for (int i = 0; i < n; i++) w += g_glyph[(unsigned char)s[i]].w;
  return (int)(w + 0.5f);
}

/* Append one line to the batch. Returns the advance in pixels. */
static float push(const char *s, int n, float x, float y, float scale) {
  float pen = x, hh = CELL_PX * scale;
  for (int i = 0; i < n && g_quads < MAX_QUADS; i++) {
    const Glyph *gl = &g_glyph[(unsigned char)s[i]];
    float gw = gl->w * scale;
    if (gw > 0.0f && s[i] != ' ') {
      float x0 = pen, x1 = pen + gw, y0 = y, y1 = y + hh;
      GLfloat *P = g_pos + g_quads * 12, *U = g_uv + g_quads * 12;
      P[0]=x0; P[1]=y0;  P[2]=x1; P[3]=y0;  P[4]=x1;  P[5]=y1;
      P[6]=x0; P[7]=y0;  P[8]=x1; P[9]=y1;  P[10]=x0; P[11]=y1;
      U[0]=gl->s0; U[1]=gl->t0;  U[2]=gl->s1; U[3]=gl->t0;  U[4]=gl->s1;  U[5]=gl->t1;
      U[6]=gl->s0; U[7]=gl->t0;  U[8]=gl->s1; U[9]=gl->t1;  U[10]=gl->s0; U[11]=gl->t1;
      g_quads++;
    }
    pen += gw;
  }
  return pen - x;
}

static void flush(float r, float g, float b, float a) {
  if (!g_quads) return;
  gl_state_filter_forget();   /* vitaGL state changed behind gles2-bc */

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_tex);
  glColor4f(r, g, b, a);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, g_pos);
  glTexCoordPointer(2, GL_FLOAT, 0, g_uv);
  glDrawArrays(GL_TRIANGLES, 0, g_quads * 6);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  g_quads = 0;
}

void font_draw(const char *s, int n, float x, float y, float scale,
               float r, float g, float b, float a) {
  if (!g_ready || !s) return;
  if (n < 0) n = (int)strlen(s);
  g_quads = 0;
  push(s, n, x, y, scale);
  flush(r, g, b, a);
}

int font_draw_wrapped(const char *s, float cx, float y, float max_w, float scale,
                      float r, float g, float b, float a) {
  if (!g_ready || !s) return 0;

  int lines = 0, start = 0, len = (int)strlen(s);
  g_quads = 0;
  while (start < len && lines < 8) {
    /* Longest run of whole words that still fits. Falls back to a hard cut so
     * one absurd word cannot spin this forever. */
    int best = -1;
    for (int i = start; i <= len; i++) {
      if (i != len && s[i] != ' ') continue;
      if (font_measure(s + start, i - start) * scale > max_w) break;
      best = i;
    }
    int stop = best;
    if (stop <= start) {
      stop = start + 1;
      while (stop < len && font_measure(s + start, stop - start + 1) * scale <= max_w)
        stop++;
    }

    push(s + start, stop - start,
         cx - font_measure(s + start, stop - start) * scale * 0.5f,
         y + lines * font_line_height(scale), scale);
    lines++;

    start = stop;
    while (start < len && s[start] == ' ') start++;
  }
  flush(r, g, b, a);
  return lines;
}
