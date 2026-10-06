/* ini_defaults.c -- graphics options set through the game's own swkotor2.ini.
 *
 * The game reads [Graphics Options] at start-up (CClientOptions::LoadOptions)
 * and applies each key through its own setters, e.g. Shadows -> SetShadows ->
 * AurEnableShadows / AurDisableShadows. Setting a key here, before the game
 * starts, switches the feature through the engine's own path; no engine memory
 * is touched. Vita3K, 2026-10-06: with Shadows=0, Scene::RenderShadows never
 * ran (both the projected and the stencil shadow passes).
 *
 * INI_NO_SHADOWS writes Shadows=0 and Soft Shadows=0 at every launch. The
 * in-game option still works until the next launch. Every other line of the
 * file is kept as it is; the file is rewritten only when a value changes. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "log.h"
#include "ini_defaults.h"

#define INI_PATH  DATA_PATH "/swkotor2.ini"
#define INI_MAX   (64 * 1024)

typedef struct { const char *key, *value; int found; } ini_key_t;

static char *read_file(const char *path, int *len) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) { *len = 0; return calloc(1, 1); }
  char *buf = malloc(INI_MAX + 1);
  int n = buf ? sceIoRead(fd, buf, INI_MAX) : -1;
  sceIoClose(fd);
  if (n < 0) { free(buf); return NULL; }
  buf[n] = '\0';
  *len = n;
  return buf;
}

/* "Key=..." at the start of a line, compared without case like the game's ini. */
static int is_key(const char *line, const char *end, const char *key) {
  size_t k = strlen(key);
  return (size_t)(end - line) > k && !strncasecmp(line, key, k) && line[k] == '=';
}

/* Sets each key in [section] (adding missing keys right after the header, and
 * the section itself at the end if it is missing). Returns the number of
 * values changed or added. */
static int set_keys(char **text, int *len, const char *section, ini_key_t *keys, int nkeys) {
  char *in = *text, *out = malloc(*len + 512), *o = out;
  if (!out) return 0;
  int changed = 0, in_section = 0, section_seen = 0;
  const char *p = in, *eof = in + *len;
  while (p < eof) {
    const char *e = memchr(p, '\n', eof - p);
    const char *next = e ? e + 1 : eof, *end = e ? e : eof;
    if (end > p && end[-1] == '\r') end--;
    if (*p == '[') {
      in_section = !section_seen && (size_t)(end - p) == strlen(section) + 2 &&
                   !strncasecmp(p + 1, section, strlen(section)) && p[strlen(section) + 1] == ']';
      if (in_section) {
        section_seen = 1;
        memcpy(o, p, next - p); o += next - p;
        /* Keys the section lacks go right after its header. */
        for (int i = 0; i < nkeys; i++) {
          int present = 0;
          for (const char *q = next; q < eof && *q != '['; ) {
            const char *qe = memchr(q, '\n', eof - q);
            if (is_key(q, qe ? qe : eof, keys[i].key)) { present = 1; break; }
            q = qe ? qe + 1 : eof;
          }
          if (!present) {
            o += sprintf(o, "%s=%s%s", keys[i].key, keys[i].value, end != e ? "\r\n" : "\n");
            keys[i].found = 2;
            changed++;
          }
        }
        p = next;
        continue;
      }
    } else if (in_section) {
      int replaced = 0;
      for (int i = 0; i < nkeys && !replaced; i++) {
        if (!is_key(p, end, keys[i].key)) continue;
        const char *v = p + strlen(keys[i].key) + 1;
        if ((size_t)(end - v) != strlen(keys[i].value) || strncmp(v, keys[i].value, end - v)) {
          log_printf("[ini] [%s] %s=%.*s -> %s", section, keys[i].key, (int)(end - v), v, keys[i].value);
          o += sprintf(o, "%s=%s", keys[i].key, keys[i].value);
          memcpy(o, end, next - end); o += next - end;   /* keep the line ending */
          changed++;
          replaced = 1;
        }
        keys[i].found = 1;
      }
      if (replaced) { p = next; continue; }
    }
    memcpy(o, p, next - p); o += next - p;
    p = next;
  }
  if (!section_seen) {
    if (o > out && o[-1] != '\n') *o++ = '\n';
    o += sprintf(o, "[%s]\n", section);
    for (int i = 0; i < nkeys; i++) { o += sprintf(o, "%s=%s\n", keys[i].key, keys[i].value); changed++; }
  }
  free(in);
  *text = out;
  *len = (int)(o - out);
  return changed;
}

void ini_defaults_apply(void) {
#if INI_NO_SHADOWS
  ini_key_t keys[] = {{"Shadows", "0", 0}, {"Soft Shadows", "0", 0}};
  int len = 0;
  char *text = read_file(INI_PATH, &len);
  if (!text) { log_printf("[ini] %s not readable; shadows left to the game", INI_PATH); return; }
  int changed = set_keys(&text, &len, "Graphics Options", keys, 2);
  if (changed) {
    SceUID fd = sceIoOpen(INI_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int w = fd >= 0 ? sceIoWrite(fd, text, len) : fd;
    if (fd >= 0) sceIoClose(fd);
    log_printf("[ini] %s: %d value(s) set (Shadows=0, Soft Shadows=0), write %s", INI_PATH, changed,
               w == len ? "ok" : "FAILED");
  } else {
    log_printf("[ini] Shadows=0 and Soft Shadows=0 already set");
  }
  free(text);
#endif
}
