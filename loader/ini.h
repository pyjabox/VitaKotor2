/* ini.h -- read and write one key of an ini file's text.
 *
 * Ported from the KOTOR I loader (VitaKotor). swkotor2.ini is the file a user
 * can edit without rebuilding the VPK, and the engine ignores keys it does not
 * know, so [Game Options] Language= survives the game writing the file back.
 *
 * Free of any VitaSDK dependency: the caller supplies the file's text, so this
 * compiles and is tested on the host (tools/test_ini.c). */
#ifndef INI_H
#define INI_H

#include <stddef.h>

/* Copy the value of `key` inside `[section]` into `out` (always NUL-terminated
 * when outsz > 0). Section and key match case-insensitively, as the engine's
 * own reader does; whitespace around the key and the value is stripped, and a
 * ';' or '#' starts a comment. 1 if found, 0 otherwise (out is then ""). A
 * value longer than outsz-1 is truncated and still reported as found. */
int ini_get(const char *text, const char *section, const char *key,
            char *out, size_t outsz);

/* `text` with `key` set to `value` inside `[section]`, into `out` (always
 * NUL-terminated when outsz > 0). Returns the length the result needs, as
 * snprintf does: a return >= outsz means it was truncated and must not be
 * written. Everything outside the one line that changes comes back byte for
 * byte; an existing key is replaced in place, a section without it gains it
 * under the header, a missing section is appended. Line endings follow the
 * file, CRLF (what the engine writes) when there is nothing to copy. */
size_t ini_set(const char *text, const char *section, const char *key,
               const char *value, char *out, size_t outsz);

/* The language numbers ASPYR.getCurrentLanguage() returns on Android, the
 * same as on the KOTOR I port: Italian before German. libkotor2 reads them that
 * way (2 picks the "it_" subtitles and the Italian folders, 3 the German ones);
 * anything else is English. The talk tables' headers and the GFF language
 * substrings number the other way round (German 2, Italian 3): never take
 * these from a dialog.tlk. */
#define INI_LANG_EN 0
#define INI_LANG_FR 1
#define INI_LANG_IT 2
#define INI_LANG_DE 3
#define INI_LANG_ES 4
#define INI_LANG_COUNT 5

/* Two-letter code to id; anything unknown is English, the engine's own
 * fallback. */
int ini_language_id(const char *code);

/* The inverse, never NULL: an id outside 0..4 is "en". */
const char *ini_language_code(int id);

#endif
