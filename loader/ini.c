/* ini.c -- see ini.h. Ported from the KOTOR I loader (VitaKotor). */
#include "ini.h"

#include <string.h>

static int lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int is_space(int c) {
  return c == ' ' || c == '\t';
}

/* Compare a span of the file [b,e) against a NUL-terminated name, ignoring
 * case. The span is not NUL-terminated -- we never copy the file's text just to
 * compare it. */
static int span_eq(const char *b, const char *e, const char *name) {
  while (b < e && *name) {
    if (lower((unsigned char)*b) != lower((unsigned char)*name)) return 0;
    b++;
    name++;
  }
  return b == e && *name == '\0';
}

static const char *skip_space(const char *p, const char *e) {
  while (p < e && is_space((unsigned char)*p)) p++;
  return p;
}

static const char *trim_back(const char *b, const char *e) {
  while (e > b && is_space((unsigned char)e[-1])) e--;
  return e;
}

int ini_get(const char *text, const char *section, const char *key,
            char *out, size_t outsz) {
  if (outsz > 0) out[0] = '\0';
  if (!text || !section || !key) return 0;

  int in_section = 0;
  const char *p = text;

  while (*p) {
    /* Split off one line; both CR and LF end it, so CRLF and LF files read
     * the same and a lone CR (old Mac saves) does not swallow the file. */
    const char *eol = p;
    while (*eol && *eol != '\n' && *eol != '\r') eol++;

    const char *b = skip_space(p, eol);
    const char *e = trim_back(b, eol);

    if (b < e && *b != ';' && *b != '#') {
      if (*b == '[') {
        /* A header ends the previous section whether or not it is ours, so a
         * key below [Sound Options] is never read as a [Game Options] key. */
        const char *close = b + 1;
        while (close < e && *close != ']') close++;
        in_section = span_eq(skip_space(b + 1, close),
                             trim_back(b + 1, close), section);
      } else if (in_section) {
        const char *eq = b;
        while (eq < e && *eq != '=') eq++;
        if (eq < e && span_eq(b, trim_back(b, eq), key)) {
          const char *v = skip_space(eq + 1, e);
          /* An inline comment is not part of the value: someone leaving
           * "fr ; was de" behind should still get "fr". */
          const char *ve = v;
          while (ve < e && *ve != ';' && *ve != '#') ve++;
          ve = trim_back(v, ve);
          if (outsz > 0) {
            size_t n = (size_t)(ve - v);
            if (n > outsz - 1) n = outsz - 1;
            memcpy(out, v, n);
            out[n] = '\0';
          }
          return 1;
        }
      }
    }

    p = eol;
    while (*p == '\n' || *p == '\r') p++;
  }
  return 0;
}

/* ---------------------------------------------------------------- ini_set */

/* Output that is always measured and only sometimes written: one pass gives
 * both the length the caller needs and as much of the text as fits. */
typedef struct {
  char *p;
  size_t cap;     /* 0 means measure only */
  size_t n;       /* bytes the result needs, whether or not they were stored */
} Out;

static void emit(Out *o, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (o->cap && o->n + 1 < o->cap) o->p[o->n] = s[i];
    o->n++;
  }
}

static void emit_str(Out *o, const char *s) {
  emit(o, s, strlen(s));
}

/* Whatever the file already uses. A file we are creating gets CRLF: the engine
 * writes CRLF, and it will rewrite this file the first time the user changes a
 * setting in-game. */
static const char *detect_eol(const char *text) {
  for (const char *p = text; *p; p++) {
    if (*p == '\r') return p[1] == '\n' ? "\r\n" : "\r";
    if (*p == '\n') return "\n";
  }
  return "\r\n";
}

size_t ini_set(const char *text, const char *section, const char *key,
               const char *value, char *out, size_t outsz) {
  Out o = { out, outsz, 0 };
  if (outsz > 0) out[0] = '\0';
  if (!text) text = "";
  if (!value) value = "";
  if (!section || !key) {
    emit_str(&o, text);
    goto done;
  }

  const char *eol_str = detect_eol(text);

  /* Scan for the line to rewrite, and failing that for somewhere to put a new
   * one. The line walk matches ini_get's exactly -- a comment is not a key and
   * a header ends the section above it -- so the key this rewrites is always
   * the key ini_get would have read back. */
  const char *hit_line = NULL;   /* start of the key's line, indentation and all */
  const char *hit_val = NULL;    /* where its value begins */
  const char *hit_eol = NULL;    /* its line terminator */
  const char *insert_at = NULL;  /* just past the first matching section header */

  int in_section = 0;
  const char *p = text;
  while (*p && !hit_line) {
    const char *eol = p;
    while (*eol && *eol != '\n' && *eol != '\r') eol++;

    const char *b = skip_space(p, eol);
    const char *e = trim_back(b, eol);

    const char *next = eol;
    while (*next == '\n' || *next == '\r') next++;

    if (b < e && *b != ';' && *b != '#') {
      if (*b == '[') {
        const char *close = b + 1;
        while (close < e && *close != ']') close++;
        in_section = span_eq(skip_space(b + 1, close),
                             trim_back(b + 1, close), section);
        if (in_section && !insert_at) insert_at = next;
      } else if (in_section) {
        const char *eq = b;
        while (eq < e && *eq != '=') eq++;
        if (eq < e && span_eq(b, trim_back(b, eq), key)) {
          hit_line = p;
          hit_val = skip_space(eq + 1, e);
          hit_eol = eol;
        }
      }
    }
    p = next;
  }

  if (hit_line) {
    /* Everything up to where the value starts is the user's: indentation, the
     * key as they spelled it, the spaces around the '='. Everything from the
     * value to the end of the line goes, which is how a stale inline comment
     * is dropped. */
    emit(&o, text, (size_t)(hit_line - text));
    emit(&o, hit_line, (size_t)(hit_val - hit_line));
    emit_str(&o, value);
    emit_str(&o, hit_eol);
  } else if (insert_at) {
    emit(&o, text, (size_t)(insert_at - text));
    emit_str(&o, key);
    emit(&o, "=", 1);
    emit_str(&o, value);
    emit_str(&o, eol_str);
    emit_str(&o, insert_at);
  } else {
    size_t len = strlen(text);
    emit(&o, text, len);
    /* A last line with no terminator would otherwise have the new header
     * welded onto the end of it. */
    if (len && text[len - 1] != '\n' && text[len - 1] != '\r')
      emit_str(&o, eol_str);
    emit(&o, "[", 1);
    emit_str(&o, section);
    emit(&o, "]", 1);
    emit_str(&o, eol_str);
    emit_str(&o, key);
    emit(&o, "=", 1);
    emit_str(&o, value);
    emit_str(&o, eol_str);
  }

done:
  if (outsz > 0) out[o.n < outsz ? o.n : outsz - 1] = '\0';
  return o.n;
}

/* One table for both directions, so a code and its id can never drift apart. */
static const struct { const char *code; int id; } kCodes[] = {
  { "en", INI_LANG_EN }, { "fr", INI_LANG_FR }, { "de", INI_LANG_DE },
  { "it", INI_LANG_IT }, { "es", INI_LANG_ES },
};

int ini_language_id(const char *code) {
  if (!code) return INI_LANG_EN;
  for (size_t i = 0; i < sizeof kCodes / sizeof kCodes[0]; i++)
    if (span_eq(code, code + strlen(code), kCodes[i].code)) return kCodes[i].id;
  return INI_LANG_EN;
}

const char *ini_language_code(int id) {
  for (size_t i = 0; i < sizeof kCodes / sizeof kCodes[0]; i++)
    if (kCodes[i].id == id) return kCodes[i].code;
  return "en";
}
