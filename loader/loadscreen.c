/* loadscreen.c -- see loadscreen.h. */

#include <vitasdk.h>
#include <vitaGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loadscreen.h"
#include "config.h"
#include "log.h"
#include "k2res.h"
#include "ls_text.h"
#include "gl_worker.h"
#include "gl_state_filter.h"

#if LOADSCREEN_ENABLE

#define TIM_PATH    DATA_PATH "/startup.tim"
#define TIM_MAGIC   0x324D4954u    /* "TIM2": us from begin to the freeze */
#define FONT_PATH   DATA_PATH "/assets/iosdialog.otf"
#define DLC_TLK     DATA_PATH "/dlc/mods_english/dialog.tlk"
#define STR_LOADING 42493          /* LBL_LOADING's STRREF */
#define RES_TPC     3007
#define RES_2DA     2017
#define MAX_ART     128
#define MAX_HINTS   128

/* loadscreen_p.gui (data/gui.bif): an 800x600 panel, stretched over the
 * screen as the game does. Extents in panel units. */
#define GX(x) ((float)(x) * (float)SCREEN_W / 800.0f)
#define GY(y) ((float)(y) * (float)SCREEN_H / 600.0f)
#define LOGO_X 537
#define LOGO_Y 9
#define LOGO_W 200
#define LOGO_H 200
#define BAR_Y 472                  /* PB_PROGRESS: full width */
#define BAR_H 20
#define BOX_X 280                  /* LBL_LOADING, border DIMENSION 16, INNEROFFSET 9 */
#define BOX_Y 422
#define BOX_W 240
#define BOX_H 26
#define BOX_DIM 16
#define BOX_INNER 9
#define HINT_X 120                 /* LBL_HINT */
#define HINT_Y 496
#define HINT_W 560
/* Text sizes in screen pixels, matched to the game's own screen. */
#define LOADING_PX 18
#define HINT_PX 20
#define HINT_LINE 24

/* The GUI's colours: text and bar from TEXT/PROGRESS COLOR, the box from its BORDER. */
static const float kText[3] = {0.102f, 0.698f, 0.549f};
static const float kBox[3] = {0.051f, 0.349f, 0.271f};
static const float kBar = 0.698f;

enum { T_ART, T_LOGO, T_CORNER, T_EDGE, T_FILL, T_BAR, T_LOADING, T_HINT, T_N };
static const char *const kNamed[T_N] = {NULL, "kotor2logo", "uibit_brdr_16bct", "uibit_brdr_16bet",
                                        "uibit_fill_2bt", "uibit_fill_16g", NULL, NULL};

/* GL thread only. */
static GLuint g_tex[T_N];
/* Game thread. */
static int g_tw[T_N], g_th[T_N];
static unsigned g_have;            /* bit per texture that was uploaded */

static SceUID g_owner = -1;
static int g_active, g_warm, g_frozen, g_busy, g_off;
static uint64_t g_t0, g_last, g_expect, g_freeze_us;
static unsigned g_seed;
static int g_hints[MAX_HINTS], g_nhint, g_slot = -1;
static SceUID g_tlk_fd = -1, g_patch_fd = -1;
static uint64_t g_tlk_base;
static char g_art_name[20];

static const char *const kColdTip =
    "First launch after installing or copying the game data is slower: the archive index is being "
    "built. Later launches skip it.";

/* ---- GL thread ------------------------------------------------------------------ */

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
  for (int i = 0; i < T_N; i++)
    if (g_tex[i]) { glDeleteTextures(1, &g_tex[i]); g_tex[i] = 0; }
}

/* Vertex storage per quad of a frame: nothing then depends on whether vitaGL
 * copies client arrays at draw time. */
#define QUADS 16
static GLfloat g_pos[QUADS][8], g_uv[QUADS][8];
static int g_q;

static void quad(int slot, float x, float y, float w, float h, float s0, float t0, float s1, float t1,
                 const float *rgb, float a) {
  if (!g_tex[slot] || w <= 0.0f || h <= 0.0f || g_q >= QUADS) return;
  GLfloat *p = g_pos[g_q], *uv = g_uv[g_q];
  g_q++;
  p[0] = x;     p[1] = y;     p[2] = x + w; p[3] = y;
  p[4] = x + w; p[5] = y + h; p[6] = x;     p[7] = y + h;
  uv[0] = s0; uv[1] = t0; uv[2] = s1; uv[3] = t0;
  uv[4] = s1; uv[5] = t1; uv[6] = s0; uv[7] = t1;
  glBindTexture(GL_TEXTURE_2D, g_tex[slot]);
  glColor4f(rgb ? rgb[0] : 1.0f, rgb ? rgb[1] : 1.0f, rgb ? rgb[2] : 1.0f, a);
  glVertexPointer(2, GL_FLOAT, 0, p);
  glTexCoordPointer(2, GL_FLOAT, 0, uv);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* a[0]: progress x 65536; a[1], a[2]: the two text images' sizes, w << 16 | h. */
static void gl_frame(const uint32_t *a, const void *data) {
  (void)data;
  float frac = (float)a[0] / 65536.0f;
  float lw = (float)(a[1] >> 16), lh = (float)(a[1] & 0xffff), hw = (float)(a[2] >> 16), hh = (float)(a[2] & 0xffff);
  g_q = 0;

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);    /* top-down, like the GUI */
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glEnable(GL_TEXTURE_2D);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  quad(T_ART, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, NULL, 1.0f);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  quad(T_LOGO, GX(LOGO_X), GY(LOGO_Y), GX(LOGO_W), GY(LOGO_H), 0, 0, 1, 1, NULL, 1.0f);

  /* PB_PROGRESS: the fill texture is cut, not squeezed, as the bar grows. */
  const float grey[3] = {kBar, kBar, kBar};
  quad(T_BAR, 0, GY(BAR_Y), SCREEN_W * frac, GY(BAR_H), 0, 0, frac, 1, grey, 1.0f);

  /* LBL_LOADING's border: fill, four mirrored corners, top and bottom edges
   * (the box is lower than two corners, so there are no side edges). */
  float bx = GX(BOX_X), by = GY(BOX_Y), bw = GX(BOX_W), bh = GY(BOX_H);
  float cw = GX(BOX_DIM), ch = GY(BOX_DIM);
  if (ch > bh * 0.5f) ch = bh * 0.5f;
  quad(T_FILL, bx + GX(BOX_INNER), by + GY(BOX_INNER), bw - 2 * GX(BOX_INNER), bh - 2 * GY(BOX_INNER), 0, 0, 1, 1,
       kBox, 1.0f);
  quad(T_CORNER, bx, by, cw, ch, 0, 0, 1, 1, kBox, 1.0f);
  quad(T_CORNER, bx + bw - cw, by, cw, ch, 1, 0, 0, 1, kBox, 1.0f);
  quad(T_CORNER, bx, by + bh - ch, cw, ch, 0, 1, 1, 0, kBox, 1.0f);
  quad(T_CORNER, bx + bw - cw, by + bh - ch, cw, ch, 1, 1, 0, 0, kBox, 1.0f);
  quad(T_EDGE, bx + cw, by, bw - 2 * cw, ch, 0, 0, 1, 1, kBox, 1.0f);
  quad(T_EDGE, bx + cw, by + bh - ch, bw - 2 * cw, ch, 0, 1, 1, 0, kBox, 1.0f);
  quad(T_LOADING, bx + (bw - lw) * 0.5f, by + (bh - lh) * 0.5f, lw, lh, 0, 0, 1, 1, kText, 1.0f);
  quad(T_HINT, GX(HINT_X) + (GX(HINT_W) - hw) * 0.5f, GY(HINT_Y) + 6.0f, hw, hh, 0, 0, 1, 1, kText, 1.0f);

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
}

/* ---- game thread ------------------------------------------------------------------ */

static void upload(int slot, uint8_t *rgba, int w, int h) {
  if (!rgba) return;
  uint32_t a[3] = {(uint32_t)slot, (uint32_t)w, (uint32_t)h};
  glw_call(gl_upload, a, 3, rgba, (uint32_t)(w * h * 4));
  free(rgba);
  g_tw[slot] = w;
  g_th[slot] = h;
  g_have |= 1u << slot;
}

typedef struct { const char *name; uint64_t off, size; } Member;
static int member_cb(const char *n, unsigned len, uint64_t off, uint64_t size, void *ctx) {
  int left = 0;
  for (Member *m = ctx; m->name; m++) {
    if (!m->size && strlen(m->name) == len && !memcmp(n, m->name, len)) { m->off = off; m->size = size; }
    left += !m->size;
  }
  return left == 0;
}

typedef struct {
  struct { uint64_t off; uint32_t size; char name[17]; } art[MAX_ART];
  int nart;
  uint64_t off[T_N];
  uint32_t size[T_N];
} ErfPick;
static int erf_cb(const char *rr, unsigned type, uint64_t off, uint32_t size, void *ctx) {
  ErfPick *e = ctx;
  if (type != RES_TPC) return 0;
  if (!strncmp(rr, "load_", 5) && e->nart < MAX_ART) {
    e->art[e->nart].off = off;
    e->art[e->nart].size = size;
    snprintf(e->art[e->nart].name, sizeof e->art[0].name, "%s", rr);
    e->nart++;
  }
  for (int i = 0; i < T_N; i++)
    if (kNamed[i] && !strcmp(rr, kNamed[i])) { e->off[i] = off; e->size[i] = size; }
  return 0;
}

static void load_tpc(SceUID fd, uint64_t off, uint32_t size, int slot) {
  uint8_t *t = size ? k2_read(fd, off, size) : NULL;
  int w = 0, h = 0;
  uint8_t *rgba = t ? k2tpc_rgba(t, size, &w, &h) : NULL;
  free(t);
  upload(slot, rgba, w, h);
}

static void text_upload(int slot, const char *s, int px, int max_w, int line_h) {
  int w = 0, h = 0;
  uint8_t *img = s && *s ? ls_text_image(s, px, max_w, line_h, &w, &h) : NULL;
  if (img) upload(slot, img, w, h);
  else g_have &= ~(1u << slot), g_tw[slot] = g_th[slot] = 0;
}

static unsigned ms_since(uint64_t *t) {
  uint64_t now = sceKernelGetProcessTimeWide();
  unsigned ms = (unsigned)((now - *t) / 1000);
  *t = now;
  return ms;
}

static void assets_load(void) {
  uint64_t t0 = sceKernelGetProcessTimeWide(), ts = t0;
  unsigned ms_obb, ms_erf, ms_art, ms_gui, ms_hints, ms_font;
  g_patch_fd = sceIoOpen(OBB_PATCH_PATH, SCE_O_RDONLY, 0);
  Member pm[] = {{"texturepacks/swpc_tex_gui.erf", 0, 0}, {"localized/english/dialog.tlk", 0, 0}, {NULL, 0, 0}};
  if (g_patch_fd >= 0) k2obb_scan(g_patch_fd, member_cb, pm);
  ms_obb = ms_since(&ts);

  static ErfPick e;
  memset(&e, 0, sizeof e);
  if (pm[0].size) k2erf_scan(g_patch_fd, pm[0].off, erf_cb, &e);
  ms_erf = ms_since(&ts);
  g_seed = (unsigned)sceKernelGetProcessTimeWide();
  if (e.nart) {
    int pick = (int)(g_seed % (unsigned)e.nart);
    snprintf(g_art_name, sizeof g_art_name, "%s", e.art[pick].name);
    load_tpc(g_patch_fd, e.art[pick].off, e.art[pick].size, T_ART);
  }
  ms_art = ms_since(&ts);
  for (int i = 0; i < T_N; i++)
    if (kNamed[i]) load_tpc(g_patch_fd, e.off[i], e.size[i], i);
  ms_gui = ms_since(&ts);

  /* The talk table the game itself uses: the DLC's if present. */
  g_tlk_fd = sceIoOpen(DLC_TLK, SCE_O_RDONLY, 0);
  g_tlk_base = 0;
  if (g_tlk_fd < 0 && pm[1].size) { g_tlk_fd = g_patch_fd; g_tlk_base = pm[1].off; }

  /* loadscreenhints.2da through chitin.key and data/2da.bif in the main OBB. */
  SceUID mfd = sceIoOpen(OBB_MAIN_PATH, SCE_O_RDONLY, 0);
  Member mm[] = {{"chitin.key", 0, 0}, {"data/2da.bif", 0, 0}, {NULL, 0, 0}};
  if (mfd >= 0) k2obb_scan(mfd, member_cb, mm);
  uint8_t *key = mm[0].size ? k2_read(mfd, mm[0].off, (uint32_t)mm[0].size) : NULL;
  char bif[64];
  uint32_t idx = 0, len = 0;
  if (key && mm[1].size && k2key_find(key, (uint32_t)mm[0].size, "loadscreenhints", RES_2DA, bif, sizeof bif, &idx) &&
      !strcmp(bif, "data/2da.bif")) {
    uint8_t *d = k2bif_read(mfd, mm[1].off, idx, &len);
    if (d) g_nhint = k2_2da_ints(d, len, "gameplayhint", g_hints, MAX_HINTS);
    free(d);
  }
  free(key);
  if (mfd >= 0) sceIoClose(mfd);
  ms_hints = ms_since(&ts);

  int font = ls_text_open(FONT_PATH);
  if (font) {
    char s[256];
    if (g_tlk_fd < 0 || !k2tlk_get(g_tlk_fd, g_tlk_base, STR_LOADING, s, sizeof s)) snprintf(s, sizeof s, "Loading");
    for (char *c = s; *c; c++)
      if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    text_upload(T_LOADING, s, LOADING_PX, (int)GX(BOX_W), LOADING_PX);
  }
  ms_font = ms_since(&ts);
  log_printf("[loadscreen] ready in %u ms (archive table %u, texture pack %u, picture %u, logo and box %u, hints %u, "
             "font %u): art %s (%d), logo %s, box %s, font %s, %d hints, talk table %s",
             (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), ms_obb, ms_erf, ms_art, ms_gui, ms_hints, ms_font,
             g_art_name[0] ? g_art_name : "none", e.nart, g_have & (1u << T_LOGO) ? "yes" : "no",
             g_have & (1u << T_CORNER) ? "yes" : "no", font ? "yes" : "no", g_nhint,
             g_tlk_fd < 0 ? "none" : g_tlk_base ? "patch OBB" : "DLC");
}

/* The hint for this moment: on a cold boot the port's one explanation first,
 * then the game's gameplay hints. */
static void hint_update(unsigned elapsed_s) {
  int slot = (int)(elapsed_s / LOADSCREEN_HINT_SECONDS);
  if (slot == g_slot) return;
  g_slot = slot;
  int own = g_warm ? 0 : 1;
  char s[1024];
  s[0] = 0;
  if (slot < own) snprintf(s, sizeof s, "%s", kColdTip);
  else if (g_nhint > 0 && g_tlk_fd >= 0)
    k2tlk_get(g_tlk_fd, g_tlk_base, g_hints[(g_seed + (unsigned)(slot - own)) % (unsigned)g_nhint], s, sizeof s);
  text_upload(T_HINT, s, HINT_PX, (int)GX(HINT_W), HINT_LINE);
}

static void draw(float frac) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  hint_update((unsigned)((sceKernelGetProcessTimeWide() - g_t0) / 1000000));
  uint32_t a[3] = {(uint32_t)(frac * 65536.0f), (uint32_t)g_tw[T_LOADING] << 16 | (uint32_t)g_th[T_LOADING],
                   (uint32_t)g_tw[T_HINT] << 16 | (uint32_t)g_th[T_HINT]};
  glw_call(gl_frame, a, 3, NULL, 0);
  /* One command per frame: without this it waits for a batch to fill, which
   * held the screen back for seconds on hardware. */
  glw_publish();
  gl_state_filter_forget();
}

/* ---- timing ---------------------------------------------------------------------- */

typedef struct { unsigned magic, us_warm, us_cold; } Tim;

static void tim_read(Tim *t) {
  t->magic = TIM_MAGIC;
  t->us_warm = LOADSCREEN_DEFAULT_WARM_S * 1000000u;
  t->us_cold = LOADSCREEN_DEFAULT_COLD_S * 1000000u;
  SceUID fd = sceIoOpen(TIM_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  Tim d;
  if (sceIoRead(fd, &d, sizeof d) == (int)sizeof d && d.magic == TIM_MAGIC) {
    if (d.us_warm > 1000000u && d.us_warm < 300000000u) t->us_warm = d.us_warm;
    if (d.us_cold > 1000000u && d.us_cold < 300000000u) t->us_cold = d.us_cold;
  }
  sceIoClose(fd);
}

static void tim_write(unsigned us) {
  Tim t;
  tim_read(&t);
  if (g_warm) t.us_warm = us; else t.us_cold = us;
  SceUID fd = sceIoOpen(TIM_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, &t, sizeof t);
  sceIoClose(fd);
}

static int switched_off(void) {
  SceUID fd = sceIoOpen(DATA_PATH "/loadscreen_mode.txt", SCE_O_RDONLY, 0);
  char c = 0;
  if (fd < 0) return 0;
  int off = sceIoRead(fd, &c, 1) == 1 && c == '0';
  sceIoClose(fd);
  return off;
}

/* ---- public ---------------------------------------------------------------------- */

void loadscreen_begin(int warm) {
  if (switched_off()) {
    g_off = 1;
    log_printf("[loadscreen] off (loadscreen_mode.txt)");
    return;
  }
  Tim t;
  tim_read(&t);
  g_warm = warm ? 1 : 0;
  g_expect = warm ? t.us_warm : t.us_cold;
  g_owner = sceKernelGetThreadId();
  g_t0 = sceKernelGetProcessTimeWide();
  g_active = 1;
  g_busy = 1;
  log_printf("[loadscreen] on (%s archive index, expecting %u s to the game's first GL)", warm ? "warm" : "cold",
             (unsigned)(g_expect / 1000000));
  assets_load();
  draw(0.0f);
  g_last = sceKernelGetProcessTimeWide();
  g_busy = 0;
#if LOADSCREEN_TEST_HOLD_S
  while (sceKernelGetProcessTimeWide() - g_t0 < LOADSCREEN_TEST_HOLD_S * 1000000ull) {
    sceKernelDelayThread(50 * 1000);
    loadscreen_tick();
  }
#endif
}

int loadscreen_active(void) { return g_active; }

void loadscreen_tick(void) {
  if (!g_active || g_busy || g_frozen) return;
  if (sceKernelGetThreadId() != g_owner) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (now - g_last < LOADSCREEN_REDRAW_MS * 1000) return;
  g_last = now;
  /* Below full until the game takes the screen: the estimate is from the
   * previous boot, and this one may be slower. */
  float f = g_expect ? (float)(now - g_t0) / (float)g_expect : 0.0f;
  if (f > 0.99f) f = 0.99f;
  g_busy = 1;
  draw(f);
  g_busy = 0;
}

void loadscreen_note_gl(void) {
  if (!g_active || g_frozen) return;
  g_frozen = 1;
  if (sceKernelGetThreadId() != g_owner) return;
  g_freeze_us = sceKernelGetProcessTimeWide() - g_t0;
  /* After the last frame in the queue, so it frees nothing still in use;
   * vitaGL also defers the actual free until the GPU is done with it. */
  glw_call(gl_free, NULL, 0, NULL, 0);
  ls_text_close();
  if (g_tlk_fd >= 0 && g_tlk_fd != g_patch_fd) sceIoClose(g_tlk_fd);
  if (g_patch_fd >= 0) sceIoClose(g_patch_fd);
  g_tlk_fd = g_patch_fd = -1;
  log_printf("[loadscreen] frozen at %u.%u s after start: the game issues GL from here", (unsigned)(g_freeze_us / 1000000),
             (unsigned)(g_freeze_us / 100000 % 10));
}

void loadscreen_end(void) {
  if (!g_active) return;
  g_active = 0;
  unsigned took = (unsigned)(sceKernelGetProcessTimeWide() - g_t0);
  if (g_freeze_us) tim_write((unsigned)g_freeze_us);
  log_printf("[loadscreen] the game's first frame %u.%u s after start (%s index); estimate saved", took / 1000000,
             took / 100000 % 10, g_warm ? "warm" : "cold");
}

#else
void loadscreen_begin(int warm) { (void)warm; }
void loadscreen_tick(void) {}
void loadscreen_end(void) {}
int loadscreen_active(void) { return 0; }
void loadscreen_note_gl(void) {}
#endif
