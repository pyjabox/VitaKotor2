/* modstack.c -- mods kept as separate folders, served to the game as one dlc/.
 *
 * The mobile release reads mods from dlc/mods_<language>, and the engine's
 * search chain (GetOverridePath, libkotor2 0x2c16ec) is
 *   dlc/mods_<lang>, dlc/mods_english, dlc/<lang>, localized/<lang>, localized/english
 * for resources, the talk table, movies and streamed voice alike. Mobile mods
 * ship one folder per language they were translated into (TSLRCM: english,
 * french, german, italian, spanish, russian; M4-78EP: english and russian),
 * and installing several meant merging them into the same folders. That works
 * in English. In any other language it fails three ways:
 *  - mods_<lang> outranks mods_english, so one mod's translated files beat the
 *    English-only mod that was meant to override them: in French, TSLRCM's
 *    files win over M4-78EP's changes to them;
 *  - a mods_<lang> without a dialog.tlk lets mods_english/dialog.tlk be found
 *    before localized/<lang>/dialog.tlk, and the whole game shows English;
 *  - English-only embedded strings come out blank (locstr_fallback.c).
 *
 * So each mod keeps its own folder, ux0:data/kotor2/mods/<Mod>/, as shipped
 * (<Mod>/dlc/mods_<tag>/..., <Mod>/mods_<tag>/..., or PC-style loose
 * override/, modules/ ... that apply to every language), and the game is shown
 * one dlc/: dlc/mods_<engine folder>/<path> is, from the first enabled mod
 * counting back from the end of the load order (folder names, sorted), that
 * mod's mods_<tag>/<path>, else its mods_english/<path>. A later mod beats an
 * earlier one in every language, as merging them in that order did in
 * English. Nothing else under dlc/ exists. A fan translation folder, if one is
 * chosen, sits under every mod.
 *
 * The talk table is not merged file by file. dlc/mods_<engine folder>/
 * dialog.tlk is the top-most mods_<tag>/dialog.tlk, else the translation's,
 * else the OBB's localized/<lang>/dialog.tlk. When an English mod table has
 * more entries than that (TSLRCM's 136,375 against the OBB's 136,329), its
 * extra entries are appended, in English, into cache/lang/<tag>/dialog.tlk,
 * which is rebuilt only when its inputs change.
 *
 * The view is an index of file names built once, before the game starts:
 * a lookup is a hash probe, and a miss costs no card access. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "k2res.h"
#include "log.h"
#include "modstack.h"

#define MODS_DIR      DATA_PATH "/mods"
#define DISABLED_FILE MODS_DIR "/disabled.txt"
#define DLC_DIR       DATA_PATH "/dlc"
#define CACHE_DIR     DATA_PATH "/cache"
#define TLK_CACHE     CACHE_DIR "/lang"
#define SRC_MAX       (MODSTACK_MAX * 2 + 4)
#define SERVED_MAX    8
#define TLK_HEAD      20
#define TLK_ENTRY     40

typedef struct {
  char name[64];
  char label[64];
  char root[256];        /* holds the mods_<tag>/ folders, or the content when loose */
  int loose, on;
} Mod;

typedef struct { char base[320]; } Src;
typedef struct { uint32_t h; uint16_t src; uint8_t dir; char *rel; } Ent;

static Mod s_mod[MODSTACK_MAX];
static int s_nmod;

static Src s_src[SRC_MAX];
static int s_nsrc, s_nsrc_mods;     /* sources that are mod or translation folders */
static Ent *s_ent;
static int s_nent, s_cap;
static int32_t *s_tab;
static uint32_t s_tabn;
static char s_viewdir[48];          /* "mods_french" */
static int s_built;

static struct { char from[256], to[256]; int used; } s_served[SERVED_MAX];
static int s_nserved;

static char s_tlk[320], s_tlk_member[64];
static int s_tlk_kind;

/* ---- small helpers ------------------------------------------------------------ */

static int path_kind(const char *p) {          /* 0 none, 1 file, 2 folder */
  SceIoStat st;
  memset(&st, 0, sizeof st);
  if (sceIoGetstat(p, &st) < 0) return 0;
  return SCE_S_ISDIR(st.st_mode) ? 2 : 1;
}

static uint32_t hash_rel(const char *s) {      /* case-insensitive, as FAT/exFAT */
  uint32_t h = 2166136261u;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 'A' && c <= 'Z') c += 32;
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

static int is_builtin_tag(const char *t) {
  static const char *const k[] = { "english", "french", "german", "italian", "spanish" };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcasecmp(t, k[i])) return 1;
  return 0;
}

/* Any mods_* folder in `dir`. */
static int has_lang_dirs(const char *dir) {
  SceUID d = sceIoDopen(dir);
  if (d < 0) return 0;
  int found = 0;
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (!found && sceIoDread(d, &e) > 0) {
    found = SCE_S_ISDIR(e.d_stat.st_mode) && !strncasecmp(e.d_name, "mods_", 5) && e.d_name[5];
    memset(&e, 0, sizeof e);
  }
  sceIoDclose(d);
  return found;
}

/* Game data straight in `dir`, the way PC mods are laid out. */
static int has_content(const char *dir) {
  static const char *const k[] = { "override", "modules", "lips", "streamvoice", "streammusic",
                                   "streamsounds", "movies", "texturepacks", "dialog.tlk" };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, k[i]);
    if (path_kind(p)) return 1;
  }
  return 0;
}

/* The only folder in `dir`, if there is exactly one: an archive unpacked into
 * its own folder (mm478ep/dlc/...). */
static int only_subdir(const char *dir, char *out, size_t sz) {
  SceUID d = sceIoDopen(dir);
  if (d < 0) return 0;
  int n = 0;
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (sceIoDread(d, &e) > 0) {
    if (SCE_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.') {
      if (++n == 1 && snprintf(out, sz, "%s/%s", dir, e.d_name) >= (int)sz) n = 2;   /* does not fit: unusable */
    }
    memset(&e, 0, sizeof e);
  }
  sceIoDclose(d);
  return n == 1;
}

static int find_root(const char *base, char *out, size_t sz, int *loose) {
  char b[256], p[300];
  snprintf(b, sizeof b, "%s", base);
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1 && !only_subdir(base, b, sizeof b)) break;
    snprintf(p, sizeof p, "%s", b);
    if (has_lang_dirs(p)) { *loose = 0; return snprintf(out, sz, "%s", p) < (int)sz; }
    snprintf(p, sizeof p, "%s/dlc", b);
    if (has_lang_dirs(p)) { *loose = 0; return snprintf(out, sz, "%s", p) < (int)sz; }
    if (has_content(b)) { snprintf(out, sz, "%s", b); *loose = 1; return 1; }
  }
  return 0;
}

/* Folder names below the device in `p` ("ux0:data/kotor2/mods" is 3). */
static int depth_of(const char *p) {
  const char *c = strchr(p, ':');
  int n = 0;
  for (c = c ? c + 1 : p; *c; c++)
    if (*c != '/' && (c[-1] == '/' || c[-1] == ':')) n++;
  return n;
}

static int by_name(const void *a, const void *b) {
  return strcasecmp(((const Mod *)a)->name, ((const Mod *)b)->name);
}

/* ---- the mod list -------------------------------------------------------------- */

void modstack_scan(void) {
  s_nmod = 0;

  /* An install from before this layout: TSLRCM and M4-78EP merged by hand into
   * dlc/. It keeps working as one mod, enabled until the user says otherwise. */
  if (has_lang_dirs(DLC_DIR)) {
    Mod *m = &s_mod[s_nmod++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", MODSTACK_LEGACY);
    snprintf(m->label, sizeof m->label, "DLC FOLDER (OLD LAYOUT)");
    snprintf(m->root, sizeof m->root, "%s", DLC_DIR);
  }
  int first = s_nmod;

  SceUID d = sceIoDopen(MODS_DIR);
  if (d >= 0) {
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
      const char *n = e.d_name;
      if (SCE_S_ISDIR(e.d_stat.st_mode) && n[0] != '.') {
        char base[256];
        Mod m;
        memset(&m, 0, sizeof m);
        if (strlen(n) >= sizeof m.name || snprintf(base, sizeof base, "%s/%s", MODS_DIR, n) >= (int)sizeof base) {
          log_printf("[mods] skipping %s: folder name longer than %u bytes", n, (unsigned)sizeof m.name - 1);
        } else if (s_nmod == MODSTACK_MAX) {
          log_printf("[mods] skipping %s: only %d mods are listed", n, MODSTACK_MAX);
        } else if (!find_root(base, m.root, sizeof m.root, &m.loose)) {
          log_printf("[mods] skipping %s: no mods_<language> folders and no game files in it", n);
        } else {
          snprintf(m.name, sizeof m.name, "%s", n);
          snprintf(m.label, sizeof m.label, "%s", n);
          s_mod[s_nmod++] = m;
          /* The Vita creates no file more than 10 folders below ux0:
           * (measured over FTP, 2026-10-13), and a voice line sits 4 below
           * a language folder (streamvoice/802/kaah/802kaah001.wav). */
          int deepest = depth_of(m.root) + (m.loose ? 0 : 1) + 4;
          if (deepest > 10)
            log_printf("!!! [mods] %s: its voice files would be %d folders deep, and the Vita stops at 10 -- "
                       "move the %s folder up into mods/%s/", n, deepest, m.loose ? "game" : "mods_<language>", n);
        }
      }
      memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
  }
  qsort(&s_mod[first], (size_t)(s_nmod - first), sizeof s_mod[0], by_name);

  /* Everything is on unless mods/disabled.txt names it, so a mod copied to the
   * card is used without a trip through the menu. */
  for (int i = 0; i < s_nmod; i++) s_mod[i].on = 1;
  SceUID fd = sceIoOpen(DISABLED_FILE, SCE_O_RDONLY, 0);
  if (fd >= 0) {
    char buf[2048];
    int n = sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
      while (*line == ' ' || *line == '\t') line++;
      char *end = line + strlen(line);
      while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      for (int i = 0; i < s_nmod; i++)
        if (!strcasecmp(s_mod[i].name, line)) s_mod[i].on = 0;
    }
  }
  for (int i = 0; i < s_nmod; i++)
    log_printf("[mods] %d. %s (%s%s)%s", i + 1, s_mod[i].name, s_mod[i].loose ? "loose files in " : "",
               s_mod[i].root, s_mod[i].on ? "" : " -- disabled");
  if (!s_nmod) log_printf("[mods] none in %s -- stock game", MODS_DIR);
}

int modstack_count(void) { return s_nmod; }
const char *modstack_name(int i) { return (i >= 0 && i < s_nmod) ? s_mod[i].name : NULL; }
const char *modstack_label(int i) { return (i >= 0 && i < s_nmod) ? s_mod[i].label : NULL; }
int modstack_enabled(int i) { return (i >= 0 && i < s_nmod) ? s_mod[i].on : 0; }
void modstack_set_enabled(int i, int on) { if (i >= 0 && i < s_nmod) s_mod[i].on = on ? 1 : 0; }

void modstack_save(void) {
  char buf[MODSTACK_MAX * 66];
  int n = 0;
  for (int i = 0; i < s_nmod; i++)
    if (!s_mod[i].on) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s\r\n", s_mod[i].name);
  if (!n) {
    sceIoRemove(DISABLED_FILE);
  } else {
    sceIoMkdir(MODS_DIR, 0777);
    SceUID fd = sceIoOpen(DISABLED_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int wrote = fd >= 0 ? sceIoWrite(fd, buf, (SceSize)n) : -1;
    if (fd >= 0) sceIoClose(fd);
    if (wrote != n) log_printf("!!! [mods] could not write %s -- the choice lasts this run only", DISABLED_FILE);
  }
  for (int i = 0; i < s_nmod; i++)
    log_printf("[mods] %s %s", s_mod[i].on ? "enabled " : "disabled", s_mod[i].name);
}

int modstack_extra_langs(char tags[][32], int max) {
  int n = 0;
  for (int i = 0; i < s_nmod; i++) {
    if (!s_mod[i].on || s_mod[i].loose) continue;
    SceUID d = sceIoDopen(s_mod[i].root);
    if (d < 0) continue;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
      const char *t = e.d_name + 5;
      if (SCE_S_ISDIR(e.d_stat.st_mode) && !strncasecmp(e.d_name, "mods_", 5) && *t && strlen(t) < 32 &&
          !is_builtin_tag(t)) {
        char p[320];
        snprintf(p, sizeof p, "%s/%s/dialog.tlk", s_mod[i].root, e.d_name);
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= !strcasecmp(tags[k], t);
        if (!dup && n < max && path_kind(p) == 1) {
          snprintf(tags[n], 32, "%s", t);
          for (char *c = tags[n]; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
          n++;
        }
      }
      memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
  }
  return n;
}

/* ---- the index ---------------------------------------------------------------- */

static Ent *find_ent(const char *rel) {
  if (!s_tab) return NULL;
  uint32_t h = hash_rel(rel);
  for (uint32_t i = h & (s_tabn - 1);; i = (i + 1) & (s_tabn - 1)) {
    int32_t k = s_tab[i];
    if (k < 0) return NULL;
    if (s_ent[k].h == h && !strcasecmp(s_ent[k].rel, rel)) return &s_ent[k];
  }
}

static void tab_insert(int k) {
  for (uint32_t i = s_ent[k].h & (s_tabn - 1);; i = (i + 1) & (s_tabn - 1))
    if (s_tab[i] < 0) { s_tab[i] = k; return; }
}

static int grow(void) {
  if (s_nent == s_cap) {
    int cap = s_cap ? s_cap * 2 : 1024;
    Ent *e = realloc(s_ent, (size_t)cap * sizeof *e);
    if (!e) return 0;
    s_ent = e;
    s_cap = cap;
  }
  if ((uint32_t)(s_nent + 1) * 2 > s_tabn) {
    uint32_t n = s_tabn ? s_tabn * 2 : 4096;
    int32_t *t = malloc(n * sizeof *t);
    if (!t) return 0;
    free(s_tab);
    s_tab = t;
    s_tabn = n;
    memset(s_tab, 0xff, n * sizeof *s_tab);
    for (int k = 0; k < s_nent; k++) tab_insert(k);
  }
  return 1;
}

/* First add wins: sources are walked from the highest priority down. */
static void add_ent(const char *rel, int src, int dir) {
  if (find_ent(rel) || !grow()) return;
  char *r = strdup(rel);
  if (!r) return;
  Ent *e = &s_ent[s_nent];
  e->h = hash_rel(rel);
  e->src = (uint16_t)src;
  e->dir = (uint8_t)dir;
  e->rel = r;
  tab_insert(s_nent++);
}

static void walk(int src, const char *real, const char *rel, int depth) {
  if (depth > 8) return;
  SceUID d = sceIoDopen(real);
  if (d < 0) return;
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (sceIoDread(d, &e) > 0) {
    const char *n = e.d_name;
    int dir = SCE_S_ISDIR(e.d_stat.st_mode);
    int dots = n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]));
    /* The talk tables are chosen as a whole, not by priority (pick_tlk). */
    int tlk = !rel[0] && !dir && (!strcasecmp(n, "dialog.tlk") || !strcasecmp(n, "dialogf.tlk"));
    if (!dots && !tlk) {
      char r[384], p[512];
      snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", n);
      add_ent(r, src, dir);
      if (dir) {
        snprintf(p, sizeof p, "%s/%s", real, n);
        walk(src, p, r, depth + 1);
      }
    }
    memset(&e, 0, sizeof e);
  }
  sceIoDclose(d);
}

static int add_src(const char *base) {
  if (s_nsrc == SRC_MAX || snprintf(s_src[s_nsrc].base, sizeof s_src[0].base, "%s", base) >= (int)sizeof s_src[0].base)
    return -1;
  return s_nsrc++;
}

/* A file outside every source (a cached talk table): its own source. */
static void add_file(const char *rel, const char *real) {
  char dir[320];
  if (snprintf(dir, sizeof dir, "%s", real) >= (int)sizeof dir) return;
  char *slash = strrchr(dir, '/');
  if (!slash) return;
  *slash = 0;
  int src = add_src(dir);
  if (src >= 0) add_ent(rel, src, 0);
  Ent *e = find_ent(rel);
  if (e && e->src != src && src >= 0) {          /* replace whatever the walk put there */
    free(e->rel);
    e->rel = strdup(slash + 1);
    e->src = (uint16_t)src;
    e->dir = 0;
  }
}

/* ---- the talk table ------------------------------------------------------------ */

typedef struct { SceUID fd; uint64_t off, size; uint32_t lang, n, str; } Tlk;

static int tlk_open_at(Tlk *t, SceUID fd, uint64_t off, uint64_t size) {
  uint8_t h[TLK_HEAD];
  t->fd = fd;
  t->off = off;
  t->size = size;
  if (fd < 0 || sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) < 0 || sceIoRead(fd, h, sizeof h) != (int)sizeof h ||
      memcmp(h, "TLK V3.0", 8))
    return 0;
  memcpy(&t->lang, h + 8, 4);
  memcpy(&t->n, h + 12, 4);
  memcpy(&t->str, h + 16, 4);
  return t->str >= TLK_HEAD + (uint64_t)t->n * TLK_ENTRY && t->str <= size;
}

static int tlk_open(Tlk *t, const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof st);
  if (sceIoGetstat(path, &st) < 0) return 0;
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (tlk_open_at(t, fd, 0, (uint64_t)st.st_size)) return 1;
  if (fd >= 0) sceIoClose(fd);
  t->fd = -1;
  return 0;
}

static int copy_range(SceUID in, uint64_t off, uint64_t len, SceUID out, uint8_t *buf, uint32_t bufsz) {
  if (sceIoLseek(in, (SceOff)off, SCE_SEEK_SET) < 0) return 0;
  while (len) {
    uint32_t n = len > bufsz ? bufsz : (uint32_t)len;
    if (sceIoRead(in, buf, n) != (int)n || sceIoWrite(out, buf, n) != (int)n) return 0;
    len -= n;
  }
  return 1;
}

/* `base`'s entries, then `ext`'s past them; base's language and strings
 * unchanged, ext's strings appended after them. */
static int tlk_compose(const char *out, Tlk *base, Tlk *ext) {
  uint32_t k = ext->n - base->n;
  uint8_t *ents = malloc((size_t)k * TLK_ENTRY);
  uint32_t *from = malloc((size_t)k * sizeof *from);    /* each extra string's offset in ext */
  uint32_t bufsz = 256 * 1024;
  uint8_t *buf = malloc(bufsz);
  char tmp[300];
  snprintf(tmp, sizeof tmp, "%s.tmp", out);
  SceUID o = -1;
  int ok = 0;
  if (!ents || !from || !buf) goto done;
  if (sceIoLseek(ext->fd, (SceOff)(ext->off + TLK_HEAD + (uint64_t)base->n * TLK_ENTRY), SCE_SEEK_SET) < 0 ||
      sceIoRead(ext->fd, ents, k * TLK_ENTRY) != (int)(k * TLK_ENTRY))
    goto done;
  /* The extra entries' strings go after base's whole string block. */
  uint32_t at = (uint32_t)(base->size - base->str);
  for (uint32_t i = 0; i < k; i++) {
    uint32_t so, sl;
    memcpy(&so, ents + i * TLK_ENTRY + 28, 4);
    memcpy(&sl, ents + i * TLK_ENTRY + 32, 4);
    if ((uint64_t)ext->str + so + sl > ext->size) sl = 0;
    from[i] = so;
    memcpy(ents + i * TLK_ENTRY + 28, &at, 4);
    memcpy(ents + i * TLK_ENTRY + 32, &sl, 4);
    at += sl;
  }
  o = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (o < 0) goto done;
  uint8_t h[TLK_HEAD];
  uint32_t n = ext->n, str = TLK_HEAD + n * TLK_ENTRY;
  memcpy(h, "TLK V3.0", 8);
  memcpy(h + 8, &base->lang, 4);
  memcpy(h + 12, &n, 4);
  memcpy(h + 16, &str, 4);
  if (sceIoWrite(o, h, sizeof h) != (int)sizeof h) goto done;
  if (!copy_range(base->fd, base->off + TLK_HEAD, (uint64_t)base->n * TLK_ENTRY, o, buf, bufsz)) goto done;
  if (sceIoWrite(o, ents, k * TLK_ENTRY) != (int)(k * TLK_ENTRY)) goto done;
  if (!copy_range(base->fd, base->off + base->str, base->size - base->str, o, buf, bufsz)) goto done;
  for (uint32_t i = 0; i < k; i++) {
    uint32_t sl;
    memcpy(&sl, ents + i * TLK_ENTRY + 32, 4);
    if (sl && !copy_range(ext->fd, ext->off + ext->str + from[i], sl, o, buf, bufsz)) goto done;
  }
  ok = 1;
done:
  if (o >= 0) sceIoClose(o);
  free(ents);
  free(from);
  free(buf);
  if (ok) {
    sceIoRemove(out);
    ok = sceIoRename(tmp, out) >= 0;
  }
  if (!ok) sceIoRemove(tmp);
  return ok;
}

typedef struct { const char *name; uint64_t off, size; } Member;
static int member_cb(const char *n, unsigned len, uint64_t off, uint64_t size, void *ctx) {
  Member *m = ctx;
  if (strlen(m->name) != len || strncasecmp(n, m->name, len)) return 0;
  m->off = off;
  m->size = size;
  return 1;
}

static void pick_tlk(const char *tag, const char *engine_folder, const char *translation) {
  int english = !strcasecmp(tag, "english");
  char p[320];
  Tlk base = { .fd = -1 }, ext = { .fd = -1 };
  SceUID obb = -1;
  char base_name[320] = "";
  s_tlk_kind = 0;

  /* The top-most table in this language; a loose mod's counts as English. */
  for (int i = s_nmod - 1; i >= 0 && base.fd < 0; i--) {
    if (!s_mod[i].on || (s_mod[i].loose && !english)) continue;
    if (s_mod[i].loose) snprintf(p, sizeof p, "%s/dialog.tlk", s_mod[i].root);
    else snprintf(p, sizeof p, "%s/mods_%s/dialog.tlk", s_mod[i].root, tag);
    if (tlk_open(&base, p)) snprintf(base_name, sizeof base_name, "%s", p);
  }
  if (base.fd < 0 && translation) {
    snprintf(p, sizeof p, "%s/dialog.tlk", translation);
    if (tlk_open(&base, p)) snprintf(base_name, sizeof base_name, "%s", p);
  }
  /* English text for entries the table lacks: the largest English mod table. */
  char ext_name[320] = "";
  if (!english) {
    for (int i = s_nmod - 1; i >= 0; i--) {
      if (!s_mod[i].on) continue;
      if (s_mod[i].loose) snprintf(p, sizeof p, "%s/dialog.tlk", s_mod[i].root);
      else snprintf(p, sizeof p, "%s/mods_english/dialog.tlk", s_mod[i].root);
      Tlk t;
      if (!tlk_open(&t, p)) continue;
      if (ext.fd < 0 || t.n > ext.n) {
        if (ext.fd >= 0) sceIoClose(ext.fd);
        ext = t;
        snprintf(ext_name, sizeof ext_name, "%s", p);
      } else {
        sceIoClose(t.fd);
      }
    }
  }
  snprintf(s_tlk_member, sizeof s_tlk_member, "localized/%s/dialog.tlk", engine_folder);
  if (base.fd < 0 && ext.fd >= 0) {
    obb = sceIoOpen(OBB_PATCH_PATH, SCE_O_RDONLY, 0);
    Member m = { s_tlk_member, 0, 0 };
    if (obb >= 0) k2obb_scan(obb, member_cb, &m);
    if (m.size && tlk_open_at(&base, obb, m.off, m.size)) snprintf(base_name, sizeof base_name, "patch OBB %s", s_tlk_member);
    else base.fd = -1;
  }

  if (ext.fd >= 0 && base.fd >= 0 && ext.n > base.n) {
    char dir[160], out[200], keyp[200], key[700], old[700];
    snprintf(dir, sizeof dir, "%s/%s", TLK_CACHE, tag);
    snprintf(out, sizeof out, "%s/dialog.tlk", dir);
    snprintf(keyp, sizeof keyp, "%s/dialog.key", dir);
    SceIoStat est;
    memset(&est, 0, sizeof est);
    sceIoGetstat(ext_name, &est);
    snprintf(key, sizeof key, "%s %llu %u | %s %llu %u %02u%02u%02u%02u%02u", base_name,
             (unsigned long long)base.size, (unsigned)base.n, ext_name, (unsigned long long)ext.size, (unsigned)ext.n,
             est.st_mtime.month, est.st_mtime.day, est.st_mtime.hour, est.st_mtime.minute, est.st_mtime.second);
    int n = 0;
    SceUID fd = sceIoOpen(keyp, SCE_O_RDONLY, 0);
    if (fd >= 0) { n = sceIoRead(fd, old, sizeof old - 1); sceIoClose(fd); }
    old[n > 0 ? n : 0] = 0;
    int fresh = !strcmp(old, key) && path_kind(out) == 1;
    if (!fresh) {
      uint64_t t0 = sceKernelGetProcessTimeWide();
      sceIoMkdir(CACHE_DIR, 0777);
      sceIoMkdir(TLK_CACHE, 0777);
      sceIoMkdir(dir, 0777);
      fresh = tlk_compose(out, &base, &ext);
      if (fresh) {
        fd = sceIoOpen(keyp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) { sceIoWrite(fd, key, (SceSize)strlen(key)); sceIoClose(fd); }
      }
      log_printf("[mods] talk table %s: %s (%u entries) + %u English entries from %s, in %u ms", out,
                 fresh ? "built" : "FAILED", (unsigned)base.n, (unsigned)(ext.n - base.n), ext_name,
                 (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    } else {
      log_printf("[mods] talk table %s: cached (%u + %u English entries)", out, (unsigned)base.n,
                 (unsigned)(ext.n - base.n));
    }
    if (fresh) {
      snprintf(s_tlk, sizeof s_tlk, "%s", out);
      s_tlk_kind = 1;
    }
  }
  if (!s_tlk_kind && base.fd >= 0 && base.fd != obb) {
    snprintf(s_tlk, sizeof s_tlk, "%s", base_name);
    s_tlk_kind = 1;
  }
  if (s_tlk_kind == 1) add_file("dialog.tlk", s_tlk);
  else s_tlk_kind = 2;
  log_printf("[mods] talk table: %s", s_tlk_kind == 1 ? s_tlk : s_tlk_member);

  if (base.fd >= 0 && base.fd != obb) sceIoClose(base.fd);
  if (ext.fd >= 0) sceIoClose(ext.fd);
  if (obb >= 0) sceIoClose(obb);

  /* The female table, where a language has one: top-most only. */
  for (int i = s_nmod - 1; i >= 0; i--) {
    if (!s_mod[i].on || s_mod[i].loose) continue;
    snprintf(p, sizeof p, "%s/mods_%s/dialogf.tlk", s_mod[i].root, tag);
    if (path_kind(p) == 1) { add_file("dialogf.tlk", p); return; }
  }
  if (translation) {
    snprintf(p, sizeof p, "%s/dialogf.tlk", translation);
    if (path_kind(p) == 1) add_file("dialogf.tlk", p);
  }
}

/* ---- building the view --------------------------------------------------------- */

void modstack_build(const char *tag, const char *engine_folder, const char *translation) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  for (int k = 0; k < s_nent; k++) free(s_ent[k].rel);
  s_nent = 0;
  if (s_tab) memset(s_tab, 0xff, s_tabn * sizeof *s_tab);
  s_nsrc = 0;
  snprintf(s_viewdir, sizeof s_viewdir, "mods_%s", engine_folder);

  int english = !strcasecmp(tag, "english");
  for (int i = s_nmod - 1; i >= 0; i--) {
    if (!s_mod[i].on) continue;
    char p[320];
    if (s_mod[i].loose) {
      add_src(s_mod[i].root);
      continue;
    }
    snprintf(p, sizeof p, "%s/mods_%s", s_mod[i].root, tag);
    if (path_kind(p) == 2) add_src(p);
    snprintf(p, sizeof p, "%s/mods_english", s_mod[i].root);
    if (!english && path_kind(p) == 2) add_src(p);
  }
  if (translation) add_src(translation);
  s_nsrc_mods = s_nsrc;
  for (int i = 0; i < s_nsrc_mods; i++) walk(i, s_src[i].base, "", 0);
  pick_tlk(tag, engine_folder, translation);
  s_built = 1;

  for (int i = 0; i < s_nsrc_mods; i++) log_printf("[mods] layer %d: %s", i + 1, s_src[i].base);
  log_printf("[mods] dlc/%s: %d files and folders from %d layer%s, indexed in %u ms", s_viewdir, s_nent, s_nsrc_mods,
             s_nsrc_mods == 1 ? "" : "s", (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}

/* ---- lookups ------------------------------------------------------------------- */

/* `in` with "ux0:/" read as "ux0:", "//" and "/./" collapsed and the trailing
 * '/' dropped (FAT stats "dir/" as missing). */
static const char *normalise(const char *in, char *out, size_t sz) {
  if (!in || sz < 2) return NULL;
  size_t n = 0;
  if (!strncasecmp(in, "ux0:/", 5)) { memcpy(out, "ux0:", 4); n = 4; in += 5; }
  for (; *in && n + 1 < sz; in++) {
    if (*in == '/' && n && out[n - 1] == '/') continue;
    if (*in == '.' && n && out[n - 1] == '/' && (in[1] == '/' || !in[1])) continue;
    out[n++] = *in;
  }
  while (n > 4 && out[n - 1] == '/') n--;
  out[n] = 0;
  return out;
}

/* The part of `p` after ux0:data/kotor2/dlc, or NULL if it is not inside. */
static const char *in_dlc(const char *p) {
  size_t l = sizeof DLC_DIR - 1;
  if (strncasecmp(p, DLC_DIR, l) || (p[l] && p[l] != '/')) return NULL;
  p += l;
  while (*p == '/') p++;
  return p;
}

int modstack_resolve(const char *path, char *out, int outsz) {
  if (!s_built || !path) return MS_PASS;
  char p[512];
  if (!normalise(path, p, sizeof p)) return MS_PASS;
  for (int i = 0; i < s_nserved; i++)
    if (!strcasecmp(p, s_served[i].from)) {
      snprintf(out, (size_t)outsz, "%s", s_served[i].to);
      if (!s_served[i].used++) log_printf("[mods] %s served from %s", s_served[i].from, s_served[i].to);
      return MS_FILE;
    }
  const char *rest = in_dlc(p);
  if (!rest) return MS_PASS;
  if (!*rest) {
    snprintf(out, (size_t)outsz, "%s", DLC_DIR);
    return MS_VDIR;
  }
  size_t n = strcspn(rest, "/");
  if (!s_nent || n != strlen(s_viewdir) || strncasecmp(rest, s_viewdir, n)) return MS_ABSENT;
  rest += n;
  while (*rest == '/') rest++;
  if (!*rest) {
    snprintf(out, (size_t)outsz, "%s", s_src[0].base);
    return MS_VDIR;
  }
  Ent *e = find_ent(rest);
  if (!e) return MS_ABSENT;
  snprintf(out, (size_t)outsz, "%s/%s", s_src[e->src].base, e->rel);
  return e->dir ? MS_VDIR : MS_FILE;
}

int modstack_find(const char *rel, char *out, int outsz) {
  Ent *e = s_built ? find_ent(rel) : NULL;
  if (!e || e->dir) return 0;
  snprintf(out, (size_t)outsz, "%s/%s", s_src[e->src].base, e->rel);
  return 1;
}

typedef struct { int n, i; const char **name; uint8_t *dir; } VDir;

void *modstack_opendir(const char *path) {
  char p[512];
  if (!s_built || !normalise(path, p, sizeof p)) return NULL;
  const char *rest = in_dlc(p);
  if (!rest) return NULL;
  VDir *v = calloc(1, sizeof *v);
  if (!v) return NULL;
  if (!*rest) {                               /* dlc/ itself: the view's one folder */
    v->name = malloc(sizeof *v->name);
    v->dir = malloc(1);
    if (s_nent && v->name && v->dir) {
      v->name[0] = s_viewdir;
      v->dir[0] = 1;
      v->n = 1;
    }
    return v;
  }
  size_t n = strcspn(rest, "/");
  if (!s_nent || n != strlen(s_viewdir) || strncasecmp(rest, s_viewdir, n)) { free(v); return NULL; }
  rest += n;
  while (*rest == '/') rest++;
  size_t rl = strlen(rest);
  if (rl) {
    Ent *e = find_ent(rest);
    if (!e || !e->dir) { free(v); return NULL; }
  }
  v->name = malloc((size_t)s_nent * sizeof *v->name);
  v->dir = malloc((size_t)s_nent);
  if (!v->name || !v->dir) { free(v->name); free(v->dir); free(v); return NULL; }
  for (int k = 0; k < s_nent; k++) {
    const char *r = s_ent[k].rel;
    if (rl) {
      if (strncasecmp(r, rest, rl) || r[rl] != '/') continue;
      r += rl + 1;
    }
    if (!*r || strchr(r, '/')) continue;
    v->name[v->n] = r;
    v->dir[v->n] = s_ent[k].dir;
    v->n++;
  }
  return v;
}

const char *modstack_readdir(void *dir, int *is_dir) {
  VDir *v = dir;
  if (!v || v->i >= v->n) return NULL;
  if (is_dir) *is_dir = v->dir[v->i];
  return v->name[v->i++];
}

void modstack_closedir(void *dir) {
  VDir *v = dir;
  if (!v) return;
  free(v->name);
  free(v->dir);
  free(v);
}

void modstack_serve(const char *path, const char *real) {
  if (s_nserved == SERVED_MAX) return;
  normalise(path, s_served[s_nserved].from, sizeof s_served[0].from);
  snprintf(s_served[s_nserved].to, sizeof s_served[0].to, "%s", real);
  s_nserved++;
}

int modstack_tlk(char *path, int psz, char *member, int msz) {
  if (s_tlk_kind == 1) snprintf(path, (size_t)psz, "%s", s_tlk);
  else snprintf(member, (size_t)msz, "%s", s_tlk_member[0] ? s_tlk_member : "localized/english/dialog.tlk");
  return s_tlk_kind == 1 ? 1 : 2;
}
