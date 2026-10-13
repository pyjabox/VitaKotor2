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
 * Games). Its label is not the layout's: CSWGuiMainMenu::LoadFromLayout sets
 * it in code (0x3ad57e) from a literal, "Google Play" (or Amazon's
 * "GameCircle"), so that literal is rewritten in place. The button writes
 * mods/menu.flag and restarts the game; nothing is lost, as it only exists on
 * the main menu. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bootmenu.h"
#include "bootui.h"
#include "config.h"
#include "lang.h"
#include "log.h"
#include "ls_text.h"
#include "main.h"
#include "modstack.h"
#include "so_util.h"

#define MENU_FLAG     DATA_PATH "/mods/menu.flag"
#define FONT_PATH     DATA_PATH "/assets/iosdialog.otf"
#define GPLAY_LABEL_OFF 0x72c3f6u    /* "Google Play", read by LoadFromLayout at 0x3ad57e */
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

/* A row is "NAME\tTAG" -- the name on the left of the bar, the tag on its
 * right -- or a single text, centred. */
enum { S_TITLE, S_SUB, S_FOOT, S_SOLID, S_MORE, S_ROW0, S_TAG0 = S_ROW0 + VISIBLE };
#define ROW_PAD 28

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

/* What each slot's texture holds, so a text is rendered only when it
 * changes: colour comes from the quad's tint, not the texture. */
typedef struct { int w, h, px, max_w; char s[ROW_LEN]; } Size;
static Size s_size[BOOTUI_SLOTS];

static void text(int slot, const char *s, int px, int max_w) {
  Size *z = &s_size[slot];
  if (z->px == px && z->max_w == max_w && !strcmp(z->s, s)) return;
  snprintf(z->s, sizeof z->s, "%s", s);
  z->px = px;
  z->max_w = max_w;
  if (!bootui_text(slot, s, px, max_w, px + 6, &z->w, &z->h)) z->w = z->h = 0;
}

/* Presses since the last call, from the pad's own sample buffer (one sample
 * per frame, the last 64 kept): a tap shorter than one of our frames still
 * counts, and two taps count twice. */
typedef struct { int up, down, ok, cancel, changed; } Presses;
static unsigned s_pad_prev;
static uint64_t s_pad_ts;

static Presses presses(const Buttons *b) {
  Presses p = {0, 0, 0, 0, 0};
  SceCtrlData buf[64];
  memset(buf, 0, sizeof buf);
  int n = sceCtrlPeekBufferPositiveExt2(0, buf, 64);
  for (;;) {                                   /* the next sample after s_pad_ts, oldest first */
    int k = -1;
    for (int i = 0; i < n; i++)
      if (buf[i].timeStamp > s_pad_ts && (k < 0 || buf[i].timeStamp < buf[k].timeStamp)) k = i;
    if (k < 0) break;
    s_pad_ts = buf[k].timeStamp;
    unsigned edge = buf[k].buttons & ~s_pad_prev;
    if (buf[k].buttons != s_pad_prev) p.changed = 1;
    s_pad_prev = buf[k].buttons;
    p.up += !!(edge & SCE_CTRL_UP);
    p.down += !!(edge & SCE_CTRL_DOWN);
    p.ok |= !!(edge & b->ok);
    p.cancel |= !!(edge & b->cancel);
  }
  return p;
}

static void put(BootQuad *q, int *n, int slot, float x, float y, float w, float h, const float *rgb) {
  q[(*n)++] = (BootQuad){slot, x, y, w, h, 0, 0, 1, 1, rgb[0], rgb[1], rgb[2], 1.0f, 1};
}

static void centred(BootQuad *q, int *n, int slot, float y, const float *rgb) {
  Size *z = &s_size[slot];
  if (z->w) put(q, n, slot, (SCREEN_W - z->w) * 0.5f, y, (float)z->w, (float)z->h, rgb);
}

/* Row `i` of the window: its name (or centred text) in S_ROW0 + i, its tag in
 * S_TAG0 + i. */
static void row_text(int i, const char *row) {
  char name[ROW_LEN];
  const char *tab = strchr(row, '\t');
  snprintf(name, sizeof name, "%.*s", tab ? (int)(tab - row) : (int)strlen(row), row);
  text(S_ROW0 + i, name, ROW_PX, tab ? HL_W / 2 : HL_W - 2 * ROW_PAD);
  int room = HL_W - 3 * ROW_PAD - s_size[S_ROW0 + i].w;
  text(S_TAG0 + i, tab ? tab + 1 : "", ROW_PX, room > 40 ? room : 40);
}

static void row_quads(BootQuad *q, int *n, int i, const char *row, float y, int on) {
  float x0 = (SCREEN_W - HL_W) * 0.5f;
  Size *name = &s_size[S_ROW0 + i], *tag = &s_size[S_TAG0 + i];
  if (!strchr(row, '\t')) {
    centred(q, n, S_ROW0 + i, y, on ? kOn : kOff);
    return;
  }
  if (name->w) put(q, n, S_ROW0 + i, x0 + ROW_PAD, y, (float)name->w, (float)name->h, on ? kOn : kOff);
  if (tag->w) put(q, n, S_TAG0 + i, x0 + HL_W - ROW_PAD - tag->w, y, (float)tag->w, (float)tag->h, on ? kTitle : kOff);
}

/* 1: confirmed, *sel the row; 0: cancelled; -1: no input for IDLE_S. */
static int pick(const Buttons *b, const char *title, const char *sub, char rows[][ROW_LEN], int n, int *sel) {
  if (n <= 0) return 0;
  uint64_t t_open = sceKernelGetProcessTimeWide();
  text(S_TITLE, title, TITLE_PX, SCREEN_W - 80);
  text(S_SUB, sub, SUB_PX, SCREEN_W - 120);
  text(S_FOOT, b->footer, FOOT_PX, SCREEN_W - 80);
  text(S_MORE, "...", ROW_PX, 200);
  int top = 0, first = 1;
  if (*sel < 0 || *sel >= n) *sel = 0;
  uint64_t idle = sceKernelGetProcessTimeWide();
  for (;;) {
    Presses p = presses(b);
    if (p.changed) idle = sceKernelGetProcessTimeWide();
    *sel = ((*sel - p.up + p.down) % n + n) % n;
    if (p.ok) return 1;
    if (p.cancel) return 0;
    if (sceKernelGetProcessTimeWide() - idle > (uint64_t)IDLE_S * 1000000u) {
      log_printf("[bootmenu] no input for %d s -- starting with the saved settings", IDLE_S);
      return -1;
    }

    /* Only rows whose text changed are rendered again (text()). */
    if (*sel < top) top = *sel;
    if (*sel >= top + VISIBLE) top = *sel - VISIBLE + 1;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (int i = 0; i < VISIBLE && top + i < n; i++) row_text(i, rows[top + i]);
    unsigned ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
    if (first) {
      log_printf("[bootmenu] \"%s\" up in %u ms (%d rows)", title,
                 (unsigned)((sceKernelGetProcessTimeWide() - t_open) / 1000), n);
      first = 0;
    } else if (ms > 30) {
      log_printf("[bootmenu] rows rendered in %u ms", ms);
    }

    BootQuad q[BOOTUI_QUADS];
    int k = 0;
    put(q, &k, S_SOLID, (SCREEN_W - HL_W) * 0.5f, ROWS_Y + (*sel - top) * ROW_STEP - (HL_H - ROW_PX) * 0.5f - 2,
        HL_W, HL_H, kHl);
    centred(q, &k, S_TITLE, TITLE_Y, kTitle);
    centred(q, &k, S_SUB, SUB_Y, kOff);
    for (int i = 0; i < VISIBLE && top + i < n; i++)
      row_quads(q, &k, i, rows[top + i], ROWS_Y + i * ROW_STEP, top + i == *sel);
    if (top > 0) centred(q, &k, S_MORE, ROWS_Y - ROW_STEP + 8, kOff);
    if (top + VISIBLE < n) centred(q, &k, S_MORE, ROWS_Y + VISIBLE * ROW_STEP - 8, kOff);
    centred(q, &k, S_FOOT, FOOT_Y, kTitle);
    bootui_frame(q, k, kBg);
    sceKernelDelayThread(16 * 1000);
  }
}

/* ---- the screens ----------------------------------------------------------------- */

/* Every language says what it has: text, and voices where they exist (the
 * OBB's for English, a voice pack for the others). */
static void lang_row(char *out, int i) {
  const Lang *l = lang_get(i);
  const char *what = l->kind == LANG_TRANSLATION ? (l->voices ? "FAN TRANSLATION + VOICES" : "FAN TRANSLATION")
                     : l->kind == LANG_MOD       ? "FROM A MOD"
                     : l->voices                 ? "TEXT + VOICES"
                                                 : "TEXT";
  snprintf(out, ROW_LEN, "%s\t%s", l->label, what);
}

/* The enabled mods' names while two fit, a count after that. */
static void mods_summary(char *out, size_t sz) {
  int on = 0;
  for (int i = 0; i < modstack_count(); i++) on += modstack_enabled(i);
  if (!modstack_count()) { snprintf(out, sz, "NONE INSTALLED"); return; }
  if (!on) { snprintf(out, sz, "NONE (STOCK GAME)"); return; }
  if (on > 2) { snprintf(out, sz, "%d OF %d ON", on, modstack_count()); return; }
  size_t at = 0;
  out[0] = 0;
  for (int i = 0, k = 0; i < modstack_count(); i++)
    if (modstack_enabled(i))
      at += (size_t)snprintf(out + at, at < sz ? sz - at : 0, "%s%s", k++ ? ", " : "", modstack_label(i));
}

static char s_rows[ROWS_MAX][ROW_LEN];

/* 1 if a language was picked (into *lang). */
static int language_screen(const Buttons *b, int *lang) {
  int n = lang_count();
  for (int i = 0; i < n; i++) lang_row(s_rows[i], i);
  int sel = *lang;
  int r = pick(b, "CHOOSE A LANGUAGE", "VOICES OTHER THAN ENGLISH COME FROM A VOICE PACK ON THE CARD (SEE THE README)",
               s_rows, n, &sel);
  if (r == 1) *lang = sel;
  return r;
}

static int mods_screen(const Buttons *b) {
  int n = modstack_count(), sel = 0;
  for (;;) {
    for (int i = 0; i < n; i++)
      snprintf(s_rows[i], ROW_LEN, "%s\t%s", modstack_label(i), modstack_enabled(i) ? "ON" : "OFF");
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
  presses(&b);          /* the presses buffered while the game was starting are not for the menu */

  int lang = lang_current();
  int was_on[MODSTACK_MAX];
  for (int i = 0; i < modstack_count(); i++) was_on[i] = modstack_enabled(i);
  int sel = flag ? 1 : 0, done = 0;
  while (!done) {
    char mods[96], hub[3][ROW_LEN];
    mods_summary(mods, sizeof mods);
    snprintf(hub[0], ROW_LEN, "LANGUAGE\t%s", lang_get(lang)->label);
    snprintf(hub[1], ROW_LEN, "MODS\t%s", mods);
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
  memset(s_size, 0, sizeof s_size);
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

void bootmenu_install_button(void) {
  uintptr_t f = so_symbol(&kotor_mod, "_ZN14CSWGuiMainMenu18OnGooglePlayButtonEP13CSWGuiControl");
  if (!f) {
    log_printf("!!! [bootmenu] OnGooglePlayButton not found -- no MODS button");
    return;
  }
  hook_thumb(f, (uintptr_t)&OnGooglePlayButton_hook);
  /* The label, set from this literal by LoadFromLayout after the layout has
   * been read. Only that function uses it. */
  static const char from[] = "Google Play", to[] = "MODS";
  char *lit = (char *)(kotor_mod.text_base + GPLAY_LABEL_OFF);
  int relabelled = !memcmp(lit, from, sizeof from);
  if (relabelled) kuKernelCpuUnrestrictedMemcpy(lit, to, sizeof to);
  log_printf("[bootmenu] the main menu's Google Play button opens this menu%s",
             relabelled ? ", labelled MODS" : " (its label is not where expected: left alone)");
}
