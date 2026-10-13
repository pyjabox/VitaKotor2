/* lang.h -- which language the game runs in (see lang.c). */
#ifndef LANG_H
#define LANG_H

#define LANG_MAX 24

enum { LANG_OBB, LANG_TRANSLATION, LANG_MOD };

typedef struct {
  char tag[32];       /* mods_<tag> and localized/<tag>: "french", or a fan/mod tag */
  char label[48];     /* the menu's row, UTF-8 */
  char code[32];      /* what swkotor2.ini says: "fr", or the translation's name */
  int engine;         /* getCurrentLanguage: 0 English .. 4 Spanish */
  int kind;
  int voices;         /* voices in this language: English's in the OBB, a pack on the card */
} Lang;

/* Main thread, after modstack_scan(): list the languages, read swkotor2.ini. */
void lang_init(void);
/* Again after the mod choice changed: mods can bring languages. */
void lang_refresh(void);

int lang_count(void);
const Lang *lang_get(int i);
int lang_current(void);
/* 1 if swkotor2.ini already names a language (no first-run question). */
int lang_asked(void);
/* Make language `i` current and save it to swkotor2.ini. */
void lang_choose(int i);

int lang_engine_id(void);
const char *lang_tag(void);
/* The folder the engine names: dlc/mods_<it>, localized/<it>. */
const char *lang_engine_folder(void);
/* The fan translation's folder, or NULL. */
const char *lang_translation_dir(void);

#endif
