/* bootui.c -- see bootui.h. Moved out of loadscreen.c so the boot menu draws
 * the same way. */

#include <vitasdk.h>
#include <vitaGL.h>
#include <stdlib.h>
#include <string.h>

#include "bootui.h"
#include "config.h"
#include "ls_text.h"
#include "gl_worker.h"
#include "gl_state_filter.h"

/* ---- GL thread ------------------------------------------------------------------ */

static GLuint g_tex[BOOTUI_SLOTS];
static volatile uint32_t g_frames_done;   /* written here, read by the game thread */

/* Vertex storage per quad of a frame: nothing then depends on whether vitaGL
 * copies client arrays at draw time. */
static GLfloat g_pos[BOOTUI_QUADS][8], g_uv[BOOTUI_QUADS][8];

static void gl_upload(const uint32_t *a, const void *rgba) {
  uint32_t slot = a[0];
  if (g_tex[slot]) glDeleteTextures(1, &g_tex[slot]);
  glGenTextures(1, &g_tex[slot]);
  glBindTexture(GL_TEXTURE_2D, g_tex[slot]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)a[1], (GLsizei)a[2], 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
}

static void gl_free(const uint32_t *a, const void *data) {
  (void)a;
  (void)data;
  for (int i = 0; i < BOOTUI_SLOTS; i++)
    if (g_tex[i]) { glDeleteTextures(1, &g_tex[i]); g_tex[i] = 0; }
}

/* a[0]: quad count; a[1..3]: background colour x 65536; data: the quads. */
static void gl_frame(const uint32_t *a, const void *data) {
  const BootQuad *q = data;
  int n = (int)a[0];
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glClearColor((float)a[1] / 65536.0f, (float)a[2] / 65536.0f, (float)a[3] / 65536.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);    /* top-down, like the GUI */
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glEnable(GL_TEXTURE_2D);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  int blending = 0;
  for (int i = 0; i < n && i < BOOTUI_QUADS; i++, q++) {
    if (q->slot < 0 || q->slot >= BOOTUI_SLOTS || !g_tex[q->slot] || q->w <= 0.0f || q->h <= 0.0f) continue;
    if (q->blend != blending) {
      if (q->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
      blending = q->blend;
    }
    GLfloat *p = g_pos[i], *uv = g_uv[i];
    p[0] = q->x;        p[1] = q->y;        p[2] = q->x + q->w; p[3] = q->y;
    p[4] = q->x + q->w; p[5] = q->y + q->h; p[6] = q->x;        p[7] = q->y + q->h;
    uv[0] = q->s0; uv[1] = q->t0; uv[2] = q->s1; uv[3] = q->t0;
    uv[4] = q->s1; uv[5] = q->t1; uv[6] = q->s0; uv[7] = q->t1;
    glBindTexture(GL_TEXTURE_2D, g_tex[q->slot]);
    glColor4f(q->r, q->g, q->b, q->a);
    glVertexPointer(2, GL_FLOAT, 0, p);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  }

  /* Back to the defaults the GL worker's copy of the state assumes. */
  glDisable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  vglSwapBuffers(GL_FALSE);
  g_frames_done++;
}

/* ---- game thread ---------------------------------------------------------------- */

void bootui_upload(int slot, uint8_t *rgba, int w, int h) {
  if (!rgba) return;
  if (slot >= 0 && slot < BOOTUI_SLOTS && w > 0 && h > 0) {
    uint32_t a[3] = {(uint32_t)slot, (uint32_t)w, (uint32_t)h};
    glw_call(gl_upload, a, 3, rgba, (uint32_t)(w * h * 4));
  }
  free(rgba);
}

void bootui_solid(int slot) {
  uint8_t *px = malloc(4);
  if (!px) return;
  memset(px, 0xff, 4);
  bootui_upload(slot, px, 1, 1);
}

int bootui_text(int slot, const char *utf8, int px, int max_w, int line_h, int *w, int *h) {
  int tw = 0, th = 0;
  uint8_t *img = utf8 && *utf8 ? ls_text_image(utf8, px, max_w, line_h, &tw, &th) : NULL;
  if (w) *w = img ? tw : 0;
  if (h) *h = img ? th : 0;
  if (!img) return 0;
  bootui_upload(slot, img, tw, th);
  return 1;
}

void bootui_frame(const BootQuad *q, int n, const float bg[3]) {
  static uint32_t sent;
  /* The worker's swap is not paced (FRAMES_AHEAD applies to the game's own),
   * and a menu left open queued frames faster than they were shown: the next
   * texture upload then waited behind the whole backlog (77 s in Vita3K).
   * At most two frames in flight, and never stuck on a worker that stopped. */
  uint64_t t0 = sceKernelGetProcessTimeWide();
  while (sent - g_frames_done >= 2 && sceKernelGetProcessTimeWide() - t0 < 2000000u) sceKernelDelayThread(1000);
  if (sent - g_frames_done >= 2) sent = g_frames_done + 1;    /* gave up waiting: count afresh */
  if (n > BOOTUI_QUADS) n = BOOTUI_QUADS;
  uint32_t a[4] = {(uint32_t)n, (uint32_t)(bg[0] * 65536.0f), (uint32_t)(bg[1] * 65536.0f),
                   (uint32_t)(bg[2] * 65536.0f)};
  glw_call(gl_frame, a, 4, q, (uint32_t)((size_t)n * sizeof *q));
  sent++;
  /* One command per frame: without this it waits for a batch to fill, which
   * held the screen back for seconds on hardware. */
  glw_publish();
  gl_state_filter_forget();
}

void bootui_free(void) {
  /* After the last frame in the queue, so it frees nothing still in use;
   * vitaGL also defers the actual free until the GPU is done with it. */
  glw_call(gl_free, NULL, 0, NULL, 0);
}
