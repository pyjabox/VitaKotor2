/* ini_defaults.h -- graphics options set in the game's swkotor2.ini (ini_defaults.c). */

#ifndef __INI_DEFAULTS_H__
#define __INI_DEFAULTS_H__

/* In main(), before the game starts and reads its options. */
void ini_defaults_apply(void);
/* A whole-number value from [Graphics Options], or def if absent. */
int ini_graphics_int(const char *key, int def);

#endif
