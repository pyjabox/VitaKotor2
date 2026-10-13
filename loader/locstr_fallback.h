/* locstr_fallback.h -- English text where a mod has no translation
 * (see locstr_fallback.c). */
#ifndef LOCSTR_FALLBACK_H
#define LOCSTR_FALLBACK_H

/* After libkotor2 is relocated, before SDL_main. `lang` is the engine
 * language (getCurrentLanguage); nothing is hooked for English. */
void locstr_fallback_install(int lang);

#endif
