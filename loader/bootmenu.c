/* bootmenu.c -- language and mods, chosen before the game starts.
 *
 * The language is bound at startup -- the engine asks getCurrentLanguage
 * while SDL_main sets up and builds its search paths from the answer -- and
 * the mods are the files it indexes then, so the only place to choose either
 * is before the game runs, on a screen the loader draws itself (bootui.c), in
 * the window after vitaGL is up and before the loading screen's clock starts.
 * The design is the KOTOR I port's language picker (VitaKotor langsel.c):
 *  - it opens on a first run (no Language in swkotor2.ini), when L is held at
 *    any moment from launch until it would open (a watcher thread latches
 *    it; the Vita's L reads as SCE_CTRL_L1 under the Ext2 reader), or after
 *    the main menu's MODS button;
 *  - confirm and cancel follow the console's own enter-button setting;
 *  - a minute with no input boots with what was saved, so a pad that never
 *    answers cannot hold the console on this screen.
 * One screen leads to the others: LANGUAGE, MODS, START. START saves what
 * was chosen; cancelling here keeps what was saved.
 *
 * The MODS button is the main menu's Google Play button
 * (CSWGuiMainMenu::OnGooglePlayButton, which on Android signs in to Play
 * Games). The layout the game loads, override/mainmenua8x6_p.gui, labels it
 * with embedded text, so a copy with "Google Play" shortened in place to
 * "MODS" is served in its place (modstack_serve). The button writes
 * mods/menu.flag and restarts the game; nothing is lost, as it only exists on
 * the main menu. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bootmenu.h"
#include "bootui.h"
#include "config.h"
#include "k2res.h"
#include "lang.h"
#include "log.h"
#include "ls_text.h"
#include "main.h"
#include "modstack.h"
#include "so_util.h"

#define MENU_FLAG     DATA_PATH "/mods/menu.flag"
#define FONT_PATH     DATA_PATH "/assets/iosdialog.otf"
#define GUI_MEMBER    "override/mainmenua8x6_p.gui"
#define GUI_CACHE     DATA_PATH "/cache/mainmenua8x6_p.gui"
#define L_MASK        (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)
#define IDLE_S        60
#define ROWS_MAX      (MODSTACK_MAX + LANG_MAX + 2)
#define ROW_LEN       128

/* Layout, screen pixels. */
#define TITLE_Y   36
#define TITLE_PX  30
#define SUB_Y     84
#define SUB_PX    17
#define ROWS_Y    132
#define ROW_STEP  40
#define ROW_PX    24
#define VISIBLE   8
#define HL_W      700
#define HL_H      36
#define FOOT_Y    500
#define FOOT_PX   17

enum { S_TITLE, S_SUB, S_FOOT, S_SOLID, S_MORE, S_ROW0 };

/* The loading screen's colours (loadscreen_p.gui). */
static const float kBg[3] = {0.0f, 0.0f, 0.0f};
static const float kTitle[3] = {0.102f, 0.698f, 0.549f};
static const float kOn[3] = {0.60f, 0.95f, 0.82f};
static const float kOff[3] = {0.26f, 0.52f, 0.44f};
static const float kHl[3] = {0.051f, 0.349f, 0.271f};

/* ---- the L latch ---------------------------------------------------------------- */

static volatile int s_watching, s_latched;
static volatile unsigned s_samples, s_bits;
static uint64_t s_watch_t0;

static int watch_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  while (s_watching) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) > 0) {
      s_samples++;
      s_bits |= pad.buttons;
      if ((pad.buttons & L_MASK) && !s_latched) {
        s_latched = 1;
        log_printf("[bootmenu] L held at +%u ms -- the menu will open",
                   (unsigned)((sceKernelGetProcessTimeWide() - s_watch_t0) / 1000));
      }
    }
    sceKernelDelayThread(20 * 1000);
  }
  return sceKernelExitDeleteThread(0);
}

void bootmenu_watch_begin(void) {
  s_watch_t0 = sceKernelGetProcessTimeWide();
  s_watching = 1;
  SceUID th = sceKernelCreateThread("bootmenu_watch", watch_thread, 0x10000100, 0x1000, 0, 0, NULL);
  if (th < 0) {
    s_watching = 0;
    log_printf("!!! [bootmenu] cannot start the L watcher (0x%08x)", (unsigned)th);
    return;
  }
  sceKernelStartThread(th, 0, NULL);
}

/* ---- drawing a list -------------------------------------------------------------- */

typedef struct {
  unsigned ok, cancel;
  const char *footer;
} Buttons;

static void buttons(Buttons *b) {
  int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  SceAppUtilInitParam init;
  SceAppUtilBootParam boot;
  memset(&init, 0, sizeof init);
  memset(&boot, 0, sizeof boot);
  sceAppUtilInit(&init, &boot);          /* harmless if something already did */
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
  if (enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
    b->ok = SCE_CTRL_CIRCLE;
    b->cancel = SCE_CTRL_CROSS;
    b->footer = "UP DOWN  MOVE        CIRCLE  SELECT        CROSS  BACK";
  } else {
    b->ok = SCE_CTRL_CROSS;
    b->cancel = SCE_CTRL_CIRCLE;
    b->footer = "UP DOWN  MOVE        CROSS  SELECT        CIRCLE  BACK";
  }
}

typedef struct { int w, h; } Size;
static Size s_size[BOOTUI_SLOTS];

static void text(int slot, const char *s, int px, int max_w) {
  Size *z = &s_size[slot];
  if (!bootui_text(slot, s, px, max_w, px + 6, &z->w, &z->h)) z->w = z->h = 0;
}

static void put(BootQuad *q, int *n, int slot, float x, float y, float w, float h, const float *rgb) {
  q[(*n)++] = (BootQuad){slot, x, y, w, h, 0, 0, 1, 1, rgb[0], rgb[1], rgb[2], 1.0f, 1};
}

static void centred(BootQuad *q, int *n, int slot, float y, const float *rgb) {
  Size *z = &s_size[slot];
  if (z->w) put(q, n, slot, (SCREEN_W - z->w) * 0.5f, y, (float)z->w, (float)z->h, rgb);
}

/* 1: confirmed, *sel the row; 0: cancelled; -1: no input for IDLE_S. */
static int pick(const Buttons *b, const char *title, const char *sub, char rows[][ROW_LEN], int n, int *sel) {
  if (n <= 0) return 0;
  text(S_TITLE, title, TITLE_PX, SCREEN_W - 80);
  text(S_SUB, sub, SUB_PX, SCREEN_W - 120);
  text(S_FOOT, b->footer, FOOT_PX, SCREEN_W - 80);
  text(S_MORE, "...", ROW_PX, 200);
  int top = -1, shown_sel = -1;
  if (*sel < 0 || *sel >= n) *sel = 0;

  SceCtrlData pad;
  memset(&pad, 0, sizeof pad);
  sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
  unsigned prev = pad.buttons;
  uint64_t idle = sceKernelGetProcessTimeWide();
  for (;;) {
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    unsigned edge = pad.buttons & ~prev;
    if (pad.buttons != prev) idle = sceKernelGetProcessTimeWide();
    prev = pad.buttons;
    if (edge & SCE_CTRL_UP) *sel = (*sel + n - 1) % n;
    if (edge & SCE_CTRL_DOWN) *sel = (*sel + 1) % n;
    if (edge & b->ok) return 1;
    if (edge & b->cancel) return 0;
    if (sceKernelGetProcessTimeWide() - idle > (uint64_t)IDLE_S * 1000000u) {
      log_printf("[bootmenu] no input for %d s -- starting with the saved settings", IDLE_S);
      return -1;
    }

    /* Row texts change colour with the selection, so the visible ones are
     * redrawn when it moves; nothing is rendered while it stands still. */
    int t = top < 0 ? 0 : top;
    if (*sel < t) t = *sel;
    if (*sel >= t + VISIBLE) t = *sel - VISIBLE + 1;
    if (t != top || *sel != shown_sel) {
      top = t;
      shown_sel = *sel;
      for (int i = 0; i < VISIBLE; i++)
        if (top + i < n) text(S_ROW0 + i, rows[top + i], ROW_PX, HL_W - 24);
        else s_size[S_ROW0 + i].w = 0;
    }

    BootQuad q[BOOTUI_QUADS];
    int k = 0;
    put(q, &k, S_SOLID, (SCREEN_W - HL_W) * 0.5f, ROWS_Y + (*sel - top) * ROW_STEP - (HL_H - ROW_PX) * 0.5f - 2,
        HL_W, HL_H, kHl);
    centred(q, &k, S_TITLE, TITLE_Y, kTitle);
    centred(q, &k, S_SUB, SUB_Y, kOff);
    for (int i = 0; i < VISIBLE && top + i < n; i++)
      centred(q, &k, S_ROW0 + i, ROWS_Y + i * ROW_STEP, top + i == *sel ? kOn : kOff);
    if (top > 0) centred(q, &k, S_MORE, ROWS_Y - ROW_STEP + 8, kOff);
    if (top + VISIBLE < n) centred(q, &k, S_MORE, ROWS_Y + VISIBLE * ROW_STEP - 8, kOff);
    centred(q, &k, S_FOOT, FOOT_Y, kTitle);
    bootui_frame(q, k, kBg);
    sceKernelDelayThread(16 * 1000);
  }
}

/* ---- the screens ----------------------------------------------------------------- */

static void lang_row(char *out, int i) {
  const Lang *l = lang_get(i);
  snprintf(out, ROW_LEN, "%s%s%s", l->label, l->voices ? "   + VOICES" : "",
           l->kind == LANG_TRANSLATION ? "   (FAN TRANSLATION)" : l->kind == LANG_MOD ? "   (FROM A MOD)" : "");
}

static void mods_summary(char *out, size_t sz) {
  int n = 0;
  size_t at = 0;
  out[0] = 0;
  for (int i = 0; i < modstack_count(); i++) {
    if (!modstack_enabled(i)) continue;
    at += (size_t)snprintf(out + at, at < sz ? sz - at : 0, "%s%s", n++ ? ", " : "", modstack_label(i));
  }
  if (!modstack_count()) snprintf(out, sz, "NONE INSTALLED");
  else if (!n) snprintf(out, sz, "NONE (STOCK GAME)");
  else if (at >= sz && sz > 4) memcpy(out + sz - 4, "...", 4);
}

static char s_rows[ROWS_MAX][ROW_LEN];

/* 1 if a language was picked (into *lang). */
static int language_screen(const Buttons *b, int *lang) {
  int n = lang_count();
  for (int i = 0; i < n; i++) lang_row(s_rows[i], i);
  int sel = *lang;
  int r = pick(b, "CHOOSE A LANGUAGE", "VOICES: A VOICE PACK IN LOCALIZED/<LANGUAGE>/ ON THE CARD. SEE THE README.",
               s_rows, n, &sel);
  if (r == 1) *lang = sel;
  return r;
}

static int mods_screen(const Buttons *b) {
  int n = modstack_count(), sel = 0;
  for (;;) {
    for (int i = 0; i < n; i++)
      snprintf(s_rows[i], ROW_LEN, "%s  %s", modstack_enabled(i) ? "[X]" : "[  ]", modstack_label(i));
    snprintf(s_rows[n], ROW_LEN, "DONE");
    int r = pick(b, "MODS", "LATER FOLDERS WIN WHERE TWO MODS CHANGE THE SAME FILE. SAVES ARE SHARED BETWEEN SETUPS.",
                 s_rows, n + 1, &sel);
    if (r != 1) return r;
    if (sel == n) return 1;
    modstack_set_enabled(sel, !modstack_enabled(sel));
  }
}

void bootmenu_run(void) {
  SceCtrlData pad;
  memset(&pad, 0, sizeof pad);
  sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
  int held = s_latched || (pad.buttons & L_MASK) != 0;
  s_watching = 0;
  SceIoStat st;
  int flag = sceIoGetstat(MENU_FLAG, &st) >= 0;
  if (flag) sceIoRemove(MENU_FLAG);
  int first = !lang_asked();
  log_printf("[bootmenu] trigger: first run %d, L %d (latch %d, %u samples, buttons 0x%08x), MODS button %d", first,
             held, s_latched, s_samples, s_bits, flag);
#if AUTOTEST_ENABLE
  /* Scripted runs set the language in swkotor2.ini; a menu would only wait. */
  if (!held && !flag) return;
#endif
  if (!held && !flag && !first) return;
  if (!ls_text_open(FONT_PATH)) {
    log_printf("[bootmenu] no font (%s) -- not shown", FONT_PATH);
    return;
  }
  Buttons b;
  buttons(&b);
  bootui_solid(S_SOLID);

  int lang = lang_current();
  int was_on[MODSTACK_MAX];
  for (int i = 0; i < modstack_count(); i++) was_on[i] = modstack_enabled(i);
  int sel = flag ? 1 : 0, done = 0;
  while (!done) {
    char mods[96], hub[3][ROW_LEN];
    mods_summary(mods, sizeof mods);
    snprintf(hub[0], ROW_LEN, "LANGUAGE:  %s%s", lang_get(lang)->label, lang_get(lang)->voices ? "  + VOICES" : "");
    snprintf(hub[1], ROW_LEN, "MODS:  %s", mods);
    snprintf(hub[2], ROW_LEN, "START");
    int r = pick(&b, "KOTOR II", "CHANGE THIS LATER: HOLD L WHILE THE GAME STARTS, OR PRESS MODS IN THE MAIN MENU",
                 hub, 3, &sel);
    if (r == 1 && sel == 0) {
      if (language_screen(&b, &lang) < 0) r = -1;
      else sel = 2;
    } else if (r == 1 && sel == 1) {
      if (modstack_count() && mods_screen(&b) < 0) r = -1;
      else sel = 2;
    } else if (r == 1) {
      done = 1;
      int changed = 0;
      for (int i = 0; i < modstack_count(); i++) changed |= was_on[i] != modstack_enabled(i);
      if (changed) {
        /* Mods can bring languages: list them again and find the choice by name. */
        Lang want = *lang_get(lang);
        modstack_save();
        lang_refresh();
        lang = 0;
        for (int i = 0; i < lang_count(); i++)
          if (lang_get(i)->kind == want.kind && !strcmp(lang_get(i)->code, want.code)) lang = i;
      }
      /* Saved even when unchanged: a first run's answer is the question gone. */
      lang_choose(lang);
    }
    if (r <= 0) {
      done = 1;
      for (int i = 0; i < modstack_count(); i++) modstack_set_enabled(i, was_on[i]);
      log_printf("[bootmenu] left without saving");
    }
  }
  bootui_free();
  ls_text_close();
}

/* ---- the MODS button -------------------------------------------------------------- */

static int OnGooglePlayButton_hook(void *self, void *control) {
  (void)self;
  (void)control;
  sceIoMkdir(DATA_PATH "/mods", 0777);
  SceUID fd = sceIoOpen(MENU_FLAG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) {
    log_printf("!!! [bootmenu] cannot write %s (0x%08x) -- not restarting", MENU_FLAG, (unsigned)fd);
    return 0;
  }
  sceIoClose(fd);
  log_printf("[bootmenu] MODS button -- restarting into the menu");
  log_flush();
  int r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
  /* Still here: the restart was refused. The flag stays on the card, so the
   * menu opens on the next launch all the same. */
  log_printf("!!! [bootmenu] sceAppMgrLoadExec failed (0x%08x) -- the menu opens on the next launch", (unsigned)r);
  return 0;
}

typedef struct { const char *name; uint64_t off, size; } Member;
static int member_cb(const char *n, unsigned len, uint64_t off, uint64_t size, void *ctx) {
  Member *m = ctx;
  if (strlen(m->name) != len || strncasecmp(n, m->name, len)) return 0;   /* the OBB says mainmenuA8x6_p */
  m->off = off;
  m->size = size;
  return 1;
}

/* The main menu layout with its Google Play button labelled MODS, cached. */
static int relabel_layout(void) {
  SceIoStat st;
  if (sceIoGetstat(GUI_CACHE, &st) >= 0 && st.st_size > 0) return 1;
  SceUID fd = sceIoOpen(OBB_MAIN_PATH, SCE_O_RDONLY, 0);
  Member m = { GUI_MEMBER, 0, 0 };
  if (fd >= 0) k2obb_scan(fd, member_cb, &m);
  uint8_t *d = m.size && m.size < 1024 * 1024 ? k2_read(fd, m.off, (uint32_t)m.size) : NULL;
  if (fd >= 0) sceIoClose(fd);
  if (!d) {
    log_printf("[bootmenu] %s not found in the main OBB -- the button keeps its label", GUI_MEMBER);
    return 0;
  }
  /* The label is a CExoString: a 32-bit length, then the text. */
  static const uint8_t from[] = "\x0b\x00\x00\x00" "Google Play";
  uint8_t *at = NULL;
  int count = 0;
  for (uint32_t i = 0; i + sizeof from - 1 <= m.size; i++)
    if (!memcmp(d + i, from, sizeof from - 1)) { at = d + i; count++; }
  int ok = 0;
  if (memcmp(d, "GUI V3.2", 8) || count != 1) {
    log_printf("[bootmenu] %s: %d \"Google Play\" labels -- left alone", GUI_MEMBER, count);
  } else {
    memcpy(at, "\x04\x00\x00\x00" "MODS", 8);
    sceIoMkdir(DATA_PATH "/cache", 0777);
    SceUID o = sceIoOpen(GUI_CACHE ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    ok = o >= 0 && sceIoWrite(o, d, (SceSize)m.size) == (int)m.size;
    if (o >= 0) sceIoClose(o);
    ok = ok && sceIoRename(GUI_CACHE ".tmp", GUI_CACHE) >= 0;
    if (!ok) sceIoRemove(GUI_CACHE ".tmp");
  }
  free(d);
  return ok;
}

void bootmenu_install_button(void) {
  uintptr_t f = so_symbol(&kotor_mod, "_ZN14CSWGuiMainMenu18OnGooglePlayButtonEP13CSWGuiControl");
  if (!f) {
    log_printf("!!! [bootmenu] OnGooglePlayButton not found -- no MODS button");
    return;
  }
  hook_thumb(f, (uintptr_t)&OnGooglePlayButton_hook);
  int relabelled = relabel_layout();
  if (relabelled) modstack_serve(DATA_PATH "/" GUI_MEMBER, GUI_CACHE);
  log_printf("[bootmenu] the main menu's Google Play button opens this menu%s",
             relabelled ? ", labelled MODS" : "");
}
