/* modstack.h -- mods kept as separate folders, served to the game as one dlc/
 * (see modstack.c). */
#ifndef MODSTACK_H
#define MODSTACK_H

#define MODSTACK_MAX 32
#define MODSTACK_LEGACY "@dlc"     /* mods/active.txt's name for an old merged dlc/ */

/* Main thread, before the game opens anything: list ux0:data/kotor2/mods/
 * (and an old-style merged dlc/), and read mods/active.txt. */
void modstack_scan(void);

int modstack_count(void);
const char *modstack_name(int i);      /* folder name, or MODSTACK_LEGACY */
const char *modstack_label(int i);     /* for the menu */
int modstack_enabled(int i);
void modstack_set_enabled(int i, int on);
void modstack_save(void);              /* mods/active.txt, load order */

/* Languages that enabled mods bring and the OBB does not: a mods_<tag> folder
 * with its own dialog.tlk (TSLRCM's mods_russian). Lowercase tags. */
int modstack_extra_langs(char tags[][32], int max);

/* Build the view for a language. `tag`: the mods_<tag> folders to prefer, each
 * mod falling back to its own mods_english; `engine_folder`: the folder the
 * engine asks for (dlc/mods_<engine_folder>); `translation`: a fan
 * translation folder to put under everything, or NULL. Game thread, before
 * SDL_main, which is what makes the lookups below lock-free. */
void modstack_build(const char *tag, const char *engine_folder, const char *translation);

/* Answers for the file hooks. `path` is a Vita path (ux0:data/kotor2/...). */
#define MS_PASS    0    /* not ours: use the path as it is */
#define MS_FILE    1    /* a file of the view: open `out` */
#define MS_VDIR    2    /* a folder of the view: `out` is a real folder to stat; list it with modstack_opendir */
#define MS_ABSENT (-1)  /* inside dlc/ but not in the view: does not exist */
int modstack_resolve(const char *path, char *out, int outsz);

/* A folder of the view, for readdir. NULL if `path` is not one. */
void *modstack_opendir(const char *path);
const char *modstack_readdir(void *dir, int *is_dir);    /* NULL at the end */
void modstack_closedir(void *dir);

/* A file of the view by its path inside it ("override/iosdialog.otf"). */
int modstack_find(const char *rel, char *out, int outsz);

/* Answer `path` (a Vita path) with `real`, e.g. a patched GUI layout. */
void modstack_serve(const char *path, const char *real);

/* The talk table the game will read: 1 and a card path in `path`, 2 and a
 * patch OBB member in `member`. */
int modstack_tlk(char *path, int psz, char *member, int msz);

#endif
