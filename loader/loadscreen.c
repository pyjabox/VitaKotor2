/* loadscreen.c -- see loadscreen.h. */

/* so_util.h reaches elf.h, which wants __BEGIN_DECLS, so the system headers
 * have to come first. */
#include <vitasdk.h>
#include <vitaGL.h>
#include <stdlib.h>
#include <string.h>

#include "loadscreen.h"
#include "gl_state_filter.h"
#include "config.h"
#include "log.h"
#if LOADSCREEN_ART
#include "font.h"
#include "hints.h"
#include "main.h"
#include "obbzip.h"
#include "so_util.h"
#include "tga.h"
#endif

#define TIM_PATH  DATA_PATH "/startup.tim"
#define TIM_MAGIC 0x314D4954u    /* "TIM1" */

typedef struct { unsigned magic; unsigned warm; unsigned us_warm, us_cold; } Tim;

static SceUID   g_owner  = -1;   /* only this thread may draw */
static int      g_active = 0;
static int      g_warm   = 0;
static int      g_busy   = 0;    /* reentrancy: draw() runs GL, GL ticks us */
static uint64_t g_t0     = 0;
static uint64_t g_last   = 0;
static uint64_t g_expect = 0;    /* us this boot is predicted to take */
static int      g_game_gl = 0;   /* the game has started issuing GL of its own */
static int      g_art     = 0;   /* the art loaded, so the freeze applies */
static unsigned g_probes  = 0;
static uint64_t g_probe_t = 0;
#if LOADSCREEN_ART
static GLuint   g_tex    = 0;    /* background art, 0 when we never got any */
static GLuint   g_logo   = 0;
static float    g_logo_s0, g_logo_t0, g_logo_s1, g_logo_t1, g_logo_aspect;
static int      g_nhint  = 0;
static unsigned g_seed   = 0;

/* Shown before the game's own hints. The boot screen is the only place the port
 * can explain itself, and the first line is the one most likely to be read. */
static const char *const g_tip[] = {
  "First boot after copying the game data is slower: the archive index is "
  "being built. Later boots skip it.",
  "The bar estimates from how long your last boot took, so it is a guess "
  "until the game takes the screen.",
};
#define TIP_COUNT ((int)(sizeof g_tip / sizeof g_tip[0]))
#endif

/* Filled rectangles via scissor+clear: no shaders, no buffers, no textures, and
 * no dependence on the projection matrix, so it draws the same whether or not
 * the art path has set one up. Coordinates are GL window space -- origin at the
 * bottom-left, unlike the GUI extents everything else is expressed in. */
static void fill(int x, int y, int w, int h, float r, float g, float b) {
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

/* Without art, the bar is its own screen and sits dead centre. */
#define BAR_W 600
#define BAR_H 12
#define BAR_X ((SCREEN_W - BAR_W) / 2)
#define BAR_Y ((SCREEN_H - BAR_H) / 2)

static void draw_plain(float frac) {
  gl_state_filter_forget();
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  fill(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, 0.16f, 0.17f, 0.20f);
  fill(BAR_X, BAR_Y, BAR_W, BAR_H, 0.09f, 0.10f, 0.13f);
  int w = (int)(BAR_W * frac);
  if (w > 0) fill(BAR_X, BAR_Y, w, BAR_H, 0.79f, 0.64f, 0.15f);
}

#if LOADSCREEN_ART

/* The art is stretched over the whole framebuffer, so bar coordinates given in
 * art pixels scale the same way the groove they sit in does. Scissor
 * coordinates start at the bottom-left, art rows at the top. */
#define ART_SX(x) ((x) * SCREEN_W / ART_W)
#define ART_SY(y) ((y) * SCREEN_H / ART_H)

/* Sampled from the mobile game's own screen: the PB_PROGRESS fill is a flat
 * cyan. Its recessed track is painted into the background art, so only the
 * filled part gets drawn -- painting a track here would double it up. */
#define BAR_R 0.024f
#define BAR_G 0.671f
#define BAR_B 0.949f

/* Decode one TGA out of the archive. The caller owns the buffer. */
static unsigned char *read_tga(ObbZip *z, const char *entry, int *w, int *h) {
  unsigned len = 0;
  void *raw = obbzip_read(z, entry, &len);
  if (!raw) { log_printf("[loadscreen] %s missing from patch.obb", entry); return NULL; }
  unsigned char *rgba = tga_decode(raw, len, w, h);
  free(raw);
  return rgba;
}

static GLuint upload(const unsigned char *rgba, int w, int h) {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  return t;
}

/* The logo file is 778x419 with wide transparent margins on three sides, so the
 * drawn quad uses the opaque bounding box rather than the whole image -- which
 * is also where its aspect ratio has to come from. */
static void logo_bounds(const unsigned char *rgba, int w, int h) {
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (rgba[((size_t)y * w + x) * 4 + 3] > 8) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  if (x1 < x0 || y1 < y0) { x0 = y0 = 0; x1 = w - 1; y1 = h - 1; }
  g_logo_s0 = (float)x0 / (float)w;
  g_logo_s1 = (float)(x1 + 1) / (float)w;
  g_logo_t0 = (float)y0 / (float)h;
  g_logo_t1 = (float)(y1 + 1) / (float)h;
  g_logo_aspect = (float)(x1 - x0 + 1) / (float)(y1 - y0 + 1);
}

/* Pull the screen's pieces out of patch.obb and upload them. Every failure path
 * leaves the piece it was loading absent -- no background means the plain bar,
 * no logo or font means the rest still draws. The boot screen is decoration and
 * must never be the thing that stops a boot. */
static void art_load(void) {
  uint64_t t0 = sceKernelGetProcessTimeWide();

  ObbZip *z = obbzip_open(OBB_PATCH_PATH);
  if (!z) {
    log_printf("[loadscreen] cannot read %s as a zip -- plain bar", OBB_PATCH_PATH);
    return;
  }

  int n = obbzip_match(z, "override/load_", ".tga", -1, NULL, 0);
  if (n <= 0) {
    log_printf("[loadscreen] no override/load_*.tga in patch.obb -- plain bar");
    obbzip_close(z);
    return;
  }

  /* Reading the directory for the list means no hardcoded name table to drift
   * out of step with whatever version of the game data is installed. */
  g_seed = (unsigned)sceKernelGetProcessTimeWide();
  char name[128];
  name[0] = '\0';
  int pick = (int)(g_seed % (unsigned)n);
  obbzip_match(z, "override/load_", ".tga", pick, name, sizeof name);

  int w = 0, h = 0;
  unsigned char *rgba = name[0] ? read_tga(z, name, &w, &h) : NULL;
  if (!rgba) { obbzip_close(z); return; }
  g_tex = upload(rgba, w, h);
  free(rgba);
  g_art = 1;
  log_printf("[loadscreen] art %s (%dx%d, %u of %d) -> tex %u in %ums",
             name, w, h, (unsigned)pick, n, (unsigned)g_tex,
             (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));

  int lw = 0, lh = 0;
  unsigned char *logo = read_tga(z, LOGO_TGA_ENTRY, &lw, &lh);
  if (logo) {
    logo_bounds(logo, lw, lh);
    g_logo = upload(logo, lw, lh);
    free(logo);
    log_printf("[loadscreen] logo %dx%d (opaque %.0f%% x %.0f%%, aspect %d/100) -> tex %u",
               lw, lh, (g_logo_s1 - g_logo_s0) * 100.0f, (g_logo_t1 - g_logo_t0) * 100.0f,
               (int)(g_logo_aspect * 100.0f), (unsigned)g_logo);
  }

  font_load(z);
  obbzip_close(z);

  if (font_ready())
    g_nhint = hints_load((LzmaUncompressFn)so_symbol(&lzma_mod, "LzmaUncompress"));

  log_printf("[loadscreen] ready in %ums (art %s, logo %s, font %s, %d hints)",
             (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000),
             g_tex ? "yes" : "no", g_logo ? "yes" : "no",
             font_ready() ? "yes" : "no", g_nhint);
}

/* Which line to show right now: a loader tip first, then the game's own hints.
 * The screen freezes partway through the boot, so putting the port's own
 * explanation first is the only way to be sure it is ever read. */
static const char *current_line(unsigned elapsed_s) {
  unsigned slot = elapsed_s / LOADSCREEN_HINT_SECONDS;
  /* Only the first slot is ours. The screen freezes at the game's first GL
   * call -- about three slots into a warm boot -- so spending more than one on
   * the port would crowd out the hints entirely on exactly the boots that are
   * short enough not to need explaining. */
  if (slot == 0) return g_tip[g_seed % (unsigned)TIP_COUNT];
  if (g_nhint <= 0) return NULL;
  return hints_get((int)((g_seed + slot - 1) % (unsigned)g_nhint));
}

/* One textured quad, in screen pixels. Each caller gets its own vertex storage
 * so nothing depends on whether vitaGL copies client arrays at draw time. */
#define QUAD(tag)                                                              \
  static GLfloat tag##_pos[8], tag##_uv[8];

static void blit(GLuint tex, GLfloat *pos, GLfloat *uv, float x, float y,
                 float w, float h, float s0, float t0, float s1, float t1,
                 int blend) {
  pos[0]=x;   pos[1]=y;    pos[2]=x+w; pos[3]=y;
  pos[4]=x+w; pos[5]=y+h;  pos[6]=x;   pos[7]=y+h;
  uv[0]=s0; uv[1]=t0;  uv[2]=s1; uv[3]=t0;
  uv[4]=s1; uv[5]=t1;  uv[6]=s0; uv[7]=t1;

  if (blend) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, tex);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, pos);
  glTexCoordPointer(2, GL_FLOAT, 0, uv);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  if (blend) glDisable(GL_BLEND);
}

/* Leaves no state behind. loadscreen_end() is called from inside the game's
 * FIRST glDrawArrays -- after it has bound its program, textures and arrays --
 * so teardown cannot happen there without clobbering that draw. Cleaning up
 * per frame instead means the handover only has to delete textures. */
static void art_draw(float frac) {
  unsigned elapsed_s =
      (unsigned)((sceKernelGetProcessTimeWide() - g_t0) / 1000000);
  QUAD(bg)
  QUAD(logo)

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  /* Top-down ortho so vertex coordinates read the same way round as the art's
   * own rows, and so uv 0,0 lands on the first row tga_decode produced. */
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  blit(g_tex, bg_pos, bg_uv, 0.0f, 0.0f, SCREEN_W, SCREEN_H,
       0.0f, 0.0f, 1.0f, 1.0f, 0);

  if (g_logo) {
    float lw = (float)ART_SX(ART_LOGO_W);
    float lh = lw / g_logo_aspect;
    blit(g_logo, logo_pos, logo_uv, (SCREEN_W - lw) * 0.5f,
         (float)ART_SY(ART_LOGO_BOTTOM) - lh, lw, lh,
         g_logo_s0, g_logo_t0, g_logo_s1, g_logo_t1, 1);
  }

  if (font_ready()) {
    float lx = (float)ART_SX(ART_LOAD_CX) - font_measure("LOADING", -1) * 0.5f;
    font_draw("LOADING", -1, lx, (float)ART_SY(ART_LOAD_Y), 1.0f,
              0.588f, 0.667f, 0.784f, 1.0f);

    const char *line = current_line(elapsed_s);
    if (line)
      font_draw_wrapped(line, (float)ART_SX(ART_HINT_CX), (float)ART_SY(ART_HINT_Y),
                        (float)ART_SX(ART_HINT_W), 1.0f,
                        0.431f, 0.627f, 0.922f, 1.0f);
  }

  /* Put the projection back to identity rather than leaving our ortho behind.
   * The game is shader-based and reads none of this, but a stale matrix is the
   * sort of thing that only shows up two bugs later. Modelview is untouched
   * since we loaded identity into it above. */
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);

  int bx = ART_SX(ART_BAR_X);
  int bw = ART_SX(ART_BAR_W);
  int bh = ART_SY(ART_BAR_H);
  int by = SCREEN_H - ART_SY(ART_BAR_Y) - bh;   /* art top-down -> GL bottom-up */
  int w = (int)(bw * frac);
  if (w > 0) fill(bx, by, w, bh, BAR_R, BAR_G, BAR_B);
  glDisable(GL_SCISSOR_TEST);
}
#endif  /* LOADSCREEN_ART */

static void draw(float frac) {
  gl_state_filter_forget();   /* vitaGL state changed behind gles2-bc */
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;

#if LOADSCREEN_ART
  if (g_tex) art_draw(frac);
  else
#endif
    draw_plain(frac);

  glDisable(GL_SCISSOR_TEST);
  vglSwapBuffers(GL_FALSE);
}

/* ---- persisted duration estimate ------------------------------------------ */

static void tim_read(Tim *t) {
  t->magic = TIM_MAGIC; t->warm = 0;
  t->us_warm = LOADSCREEN_DEFAULT_WARM_S * 1000000u;
  t->us_cold = LOADSCREEN_DEFAULT_COLD_S * 1000000u;
  SceUID fd = sceIoOpen(TIM_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  Tim d;
  if (sceIoRead(fd, &d, sizeof d) == (int)sizeof d && d.magic == TIM_MAGIC) {
    if (d.us_warm > 1000000u && d.us_warm < 600000000u) t->us_warm = d.us_warm;
    if (d.us_cold > 1000000u && d.us_cold < 600000000u) t->us_cold = d.us_cold;
  }
  sceIoClose(fd);
}

static void tim_write(unsigned us) {
  Tim t;
  tim_read(&t);
  if (g_warm) t.us_warm = us; else t.us_cold = us;
  t.magic = TIM_MAGIC;
  SceUID fd = sceIoOpen(TIM_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, &t, sizeof t);
  sceIoClose(fd);
}

/* ---- public ---------------------------------------------------------------- */

void loadscreen_begin(int warm) {
#if LOADSCREEN_ENABLE
  Tim t; tim_read(&t);
  g_warm   = warm ? 1 : 0;
  g_expect = warm ? t.us_warm : t.us_cold;
  g_owner  = sceKernelGetThreadId();
  g_t0     = sceKernelGetProcessTimeWide();
  g_last   = 0;
  g_active = 1;
  log_printf("[loadscreen] on (thread 0x%x, %s cache, expecting %us)",
             (unsigned)g_owner, warm ? "warm" : "cold", (unsigned)(g_expect / 1000000));
  /* Held across art_load() too: g_tex goes non-zero at glGenTextures, a moment
   * before glTexImage2D gives it any content, and a tick landing in that gap
   * would draw an undefined texture. */
  g_busy = 1;
#if LOADSCREEN_ART
  art_load();
#endif
  draw(0.0f);
  g_busy = 0;
#else
  (void)warm;
#endif
}

int loadscreen_active(void) { return g_active; }

/* Record what the game has bound. Every call here is a pure query, and we are
 * in the loader's own wrapper before the real GL call runs, so vitaGL is not
 * re-entered. The values start at -1 so an enum vitaGL declines to answer reads
 * as unknown rather than as a plausible zero. */
static void probe_gl_state(const char *when) {
  GLint fbo = -1, prog = -1, vbo = -1, ibo = -1, tex = -1;
  GLint vp[4] = { -1, -1, -1, -1 };
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ibo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  glGetIntegerv(GL_VIEWPORT, vp);
  log_printf("[loadscreen:probe %u/%u] %s: fbo=%d prog=%d vbo=%d ibo=%d tex=%d "
             "viewport=%d,%d %dx%d", g_probes + 1, (unsigned)LOADSCREEN_PROBE_MAX,
             when, fbo, prog, vbo, ibo, tex, vp[0], vp[1], vp[2], vp[3]);
  g_probes++;
}

void loadscreen_note_gl(void) {
#if LOADSCREEN_ENABLE
  if (!g_active) return;

  /* Raise the flag before the thread check, and before anything below can
   * return early: a plain int write is safe from any thread, and GL issued by
   * some other thread is just as good a reason to stop the art. Everything
   * after this only makes sense on the thread that owns the screen. */
  int first = !g_game_gl;
  g_game_gl = 1;

  if (sceKernelGetThreadId() != g_owner) return;

  if (first) {
    uint64_t now = sceKernelGetProcessTimeWide();
    unsigned ms = (unsigned)((now - g_t0) / 1000);
    if (g_art)
      log_printf("[loadscreen] frozen at %u.%03us -- the game has begun issuing "
                 "GL, so the art stops here and the last frame stays on screen",
                 ms / 1000, ms % 1000);
    else
      log_printf("[loadscreen] game began issuing GL at %u.%03us (plain bar, "
                 "keeps drawing)", ms / 1000, ms % 1000);
    g_probe_t = now;
    probe_gl_state("at the game's first GL call");
#if LOADSCREEN_ART
    /* Nothing will ever draw it again. vitaGL defers the actual free until the
     * texture is no longer referenced, so this cannot pull memory out from
     * under a frame still in flight. Doing it here rather than at handover also
     * leaves loadscreen_end() with no GL to issue at all, which matters because
     * it runs inside the game's first draw call. */
    if (g_tex)  { glDeleteTextures(1, &g_tex);  g_tex = 0; }
    if (g_logo) { glDeleteTextures(1, &g_logo); g_logo = 0; }
    font_free();
    hints_free();
    g_nhint = 0;
#endif
  } else if (g_probes < LOADSCREEN_PROBE_MAX) {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - g_probe_t >= (uint64_t)LOADSCREEN_PROBE_MS * 1000) {
      g_probe_t = now;
      probe_gl_state("while frozen");
    }
  }

  /* GLLOG used to call this directly. The plain bar still wants it; the art
   * path stops itself inside the tick. */
  loadscreen_tick();
#endif
}

void loadscreen_tick(void) {
#if LOADSCREEN_ENABLE
  if (!g_active || g_busy) return;
  if (g_art && g_game_gl) return;   /* frozen: see loadscreen.h */
  if (sceKernelGetThreadId() != g_owner) return;    /* never GL off-thread */

  uint64_t now = sceKernelGetProcessTimeWide();
  if (now - g_last < LOADSCREEN_REDRAW_MS * 1000) return;
  g_last = now;

  /* Clamp below full: the estimate is from the previous boot and this one may
   * be slower. A bar that creeps to 99% and waits is honest; one that reads
   * 100% while the game is still loading is exactly the bug being fixed. */
  float f = g_expect ? (float)(now - g_t0) / (float)g_expect : 0.0f;
  if (f > 0.99f) f = 0.99f;

  g_busy = 1; draw(f); g_busy = 0;
#endif
}

void loadscreen_end(void) {
#if LOADSCREEN_ENABLE
  if (!g_active) return;
  g_active = 0;
  if (sceKernelGetThreadId() != g_owner) return;

  unsigned took = (unsigned)(sceKernelGetProcessTimeWide() - g_t0);

  /* This runs INSIDE the game's first glDrawArrays, after it has bound its own
   * program, buffers and texture. The art path issues no GL here at all: it
   * stopped drawing at the freeze and its texture went with it, so there is
   * nothing left to do but record the timing. Only the plain bar, which has
   * always drawn this final frame safely, still does. */
  if (!(g_art && g_game_gl)) {
    g_busy = 1;
    draw(1.0f);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    g_busy = 0;
  }

  tim_write(took);
  log_printf("[loadscreen] off after %u.%us (%s cache, %s) -- estimate saved for "
             "next boot", took / 1000000, (took / 100000) % 10,
             g_warm ? "warm" : "cold",
             g_art ? (g_game_gl ? "art, frozen early" : "art") : "plain bar");
#endif
}
