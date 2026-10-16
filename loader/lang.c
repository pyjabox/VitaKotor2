/* lang.c -- which language the game runs in.
 *
 * The Android game asks Java for the device language
 * (ASPYR.getCurrentLanguage, answered by jni_patch.c) and runs in what it is
 * told: 0 English, 1 French, 2 Italian, 3 German, 4 Spanish. The patch OBB has
 * all five talk tables but only English voices, lip sync and voiced movies;
 * the engine looks each of those up under localized/<language>/ before
 * localized/english/, file by file, so a voice pack copied to
 * ux0:data/kotor2/localized/<language>/ plays wherever it has the line.
 *
 * The choice is kept in swkotor2.ini, [Game Options] Language=fr, as on the
 * KOTOR I port; the engine reads no such key and keeps it when it rewrites the
 * file. Two more kinds of language run with the engine in English:
 *  - fan translations, ux0:data/kotor2/translations/<Name>/ holding a
 *    dialog.tlk (and anything else the translation changes, laid out like a
 *    mods_<language> folder), saved as Translation=<Name>;
 *  - languages an enabled mod brings with its own talk table (TSLRCM's
 *    mods_russian), saved as Translation=<tag>.
 * modstack.c serves each mod's mods_<tag> folder, falling back to its own
 * mods_english. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "ini.h"
#include "lang.h"
#include "log.h"
#include "modstack.h"

#define INI_FILE DATA_PATH "/swkotor2.ini"
#define TR_DIR   DATA_PATH "/translations"
#define INI_MAX  (64 * 1024)

/* Indexed by the engine's number; the names as each language writes itself. */
static const struct { const char *tag, *label; } kObb[INI_LANG_COUNT] = {
  [INI_LANG_EN] = { "english", "ENGLISH" },
  [INI_LANG_FR] = { "french", "FRAN\xC3\x87" "AIS" },
  [INI_LANG_IT] = { "italian", "ITALIANO" },
  [INI_LANG_DE] = { "german", "DEUTSCH" },
  [INI_LANG_ES] = { "spanish", "ESPA\xC3\x91" "OL" },
};

static Lang s_lang[LANG_MAX];
static int s_n, s_cur, s_asked;
static char s_tr_dir[160];

static char *read_ini(void) {
  char *t = malloc(INI_MAX + 1);
  if (!t) return NULL;
  int n = 0;
  SceUID fd = sceIoOpen(INI_FILE, SCE_O_RDONLY, 0);
  if (fd >= 0) {
    n = sceIoRead(fd, t, INI_MAX);
    sceIoClose(fd);
  }
  t[n > 0 ? n : 0] = 0;
  return t;
}

static int is_dir(const char *p) {
  SceIoStat st;
  memset(&st, 0, sizeof st);
  return sceIoGetstat(p, &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

static int is_file(const char *p) {
  SceIoStat st;
  memset(&st, 0, sizeof st);
  return sceIoGetstat(p, &st) >= 0 && !SCE_S_ISDIR(st.st_mode);
}

static void set_case(char *s, int up) {
  for (; *s; s++) {
    if (up && *s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
    if (!up && *s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
  }
}

static void list(void) {
  s_n = 0;
  for (int i = 0; i < INI_LANG_COUNT; i++) {
    Lang *l = &s_lang[s_n++];
    memset(l, 0, sizeof *l);
    snprintf(l->tag, sizeof l->tag, "%s", kObb[i].tag);
    snprintf(l->label, sizeof l->label, "%s", kObb[i].label);
    snprintf(l->code, sizeof l->code, "%s", ini_language_code(i));
    l->engine = i;
    l->kind = LANG_OBB;
    l->voices = i == INI_LANG_EN;          /* the OBB's; the others need a pack */
    if (i != INI_LANG_EN) {
      char p[160];
      snprintf(p, sizeof p, "%s/localized/%s/streamvoice", DATA_PATH, l->tag);
      l->voices = is_dir(p);
    }
  }

  SceUID d = sceIoDopen(TR_DIR);
  if (d >= 0) {
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0 && s_n < LANG_MAX) {
      char p[320];
      int fits = strlen(e.d_name) < 32 && snprintf(p, sizeof p, "%s/%s/dialog.tlk", TR_DIR, e.d_name) < (int)sizeof p;
      if (SCE_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.' && fits && is_file(p)) {
        Lang *l = &s_lang[s_n++];
        memset(l, 0, sizeof *l);
        snprintf(l->code, sizeof l->code, "%.31s", e.d_name);
        snprintf(l->tag, sizeof l->tag, "%.31s", e.d_name);
        set_case(l->tag, 0);
        snprintf(l->label, sizeof l->label, "%.31s", e.d_name);
        set_case(l->label, 1);
        l->kind = LANG_TRANSLATION;
        snprintf(p, sizeof p, "%s/%s/streamvoice", TR_DIR, e.d_name);
        l->voices = is_dir(p);
      } else if (SCE_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.') {
        log_printf("[lang] skipping translations/%s: no dialog.tlk in it", e.d_name);
      }
      memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
  }

  char tags[8][32];
  int n = modstack_extra_langs(tags, 8);
  for (int k = 0; k < n && s_n < LANG_MAX; k++) {
    int dup = 0;
    for (int i = 0; i < s_n; i++) dup |= !strcasecmp(s_lang[i].tag, tags[k]);
    if (dup) continue;
    Lang *l = &s_lang[s_n++];
    memset(l, 0, sizeof *l);
    snprintf(l->tag, sizeof l->tag, "%.31s", tags[k]);
    snprintf(l->code, sizeof l->code, "%.31s", tags[k]);
    snprintf(l->label, sizeof l->label, "%.31s", tags[k]);
    set_case(l->label, 1);
    l->kind = LANG_MOD;
  }
}

static void apply(void) {
  const Lang *l = &s_lang[s_cur];
  s_tr_dir[0] = 0;
  if (l->kind == LANG_TRANSLATION) snprintf(s_tr_dir, sizeof s_tr_dir, "%s/%s", TR_DIR, l->code);
  log_printf("[lang] %s: %s, engine language %d, %s", l->label,
             l->kind == LANG_OBB ? "the game's own" : l->kind == LANG_TRANSLATION ? "fan translation" : "from a mod",
             l->engine, !l->voices ? "English voices" : l->engine == INI_LANG_EN && l->kind == LANG_OBB ? "voices" :
             "voice pack on the card");
}

/* The entry swkotor2.ini's Language and Translation name. */
static int find(const char *code, const char *tr) {
  if (tr && tr[0])
    for (int i = 0; i < s_n; i++)
      if (s_lang[i].kind != LANG_OBB && !strcasecmp(s_lang[i].code, tr)) return i;
  return ini_language_id(code);         /* built-ins sit at their own number */
}

void lang_init(void) {
  char code[16] = "", tr[64] = "";
  char *t = read_ini();
  if (t) {
    s_asked = ini_get(t, "Game Options", "Language", code, sizeof code);
    ini_get(t, "Game Options", "Translation", tr, sizeof tr);
    free(t);
  }
  list();
  s_cur = find(code, tr);
  if (tr[0] && s_lang[s_cur].kind == LANG_OBB)
    log_printf("[lang] Translation=%s is not on the card -- using Language=%s", tr, code[0] ? code : "en");
  apply();
}

void lang_refresh(void) {
  Lang was = s_lang[s_cur];
  list();
  s_cur = find(was.kind == LANG_OBB ? was.code : "en", was.kind == LANG_OBB ? NULL : was.code);
  apply();
}

int lang_count(void) { return s_n; }
const Lang *lang_get(int i) { return (i >= 0 && i < s_n) ? &s_lang[i] : NULL; }
int lang_current(void) { return s_cur; }
int lang_asked(void) { return s_asked; }
int lang_engine_id(void) { return s_lang[s_cur].engine; }
const char *lang_tag(void) { return s_lang[s_cur].tag; }
const char *lang_engine_folder(void) { return kObb[s_lang[s_cur].engine].tag; }
const char *lang_translation_dir(void) { return s_tr_dir[0] ? s_tr_dir : NULL; }

/* `text` with key=value set; NULL on no memory. */
static char *with(const char *text, const char *key, const char *value) {
  size_t need = ini_set(text, "Game Options", key, value, NULL, 0);
  char *out = malloc(need + 1);
  if (out) ini_set(text, "Game Options", key, value, out, need + 1);
  return out;
}

void lang_choose(int i) {
  if (i < 0 || i >= s_n) return;
  s_cur = i;
  s_asked = 1;
  apply();
  const Lang *l = &s_lang[i];
  char *t = read_ini();
  if (!t) return;
  char had[64];
  char *a = with(t, "Language", l->kind == LANG_OBB ? l->code : "en");
  char *b = a;
  if (a && (l->kind != LANG_OBB || ini_get(t, "Game Options", "Translation", had, sizeof had)))
    b = with(a, "Translation", l->kind == LANG_OBB ? "" : l->code);
  int ok = 0;
  if (b) {
    /* Through a temporary file: an interrupted write loses the new choice,
     * not the user's settings. */
    SceUID fd = sceIoOpen(INI_FILE ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int n = (int)strlen(b);
    ok = fd >= 0 && sceIoWrite(fd, b, (SceSize)n) == n;
    if (fd >= 0) sceIoClose(fd);
    if (ok) {
      sceIoRemove(INI_FILE);
      ok = sceIoRename(INI_FILE ".tmp", INI_FILE) >= 0;
    }
  }
  log_printf("%s[lang] %s Language=%s%s%s in %s", ok ? "" : "!!! ", ok ? "saved" : "could not save",
             l->kind == LANG_OBB ? l->code : "en", l->kind == LANG_OBB ? "" : " Translation=",
             l->kind == LANG_OBB ? "" : l->code, INI_FILE);
  if (b != a) free(b);
  free(a);
  free(t);
}
