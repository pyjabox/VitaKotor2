/* glsl_prep.c -- make KOTOR's ubershader survive vitaGL's varying scanner.
 *
 * vitaGL translates GLSL to Cg by *text-scanning* for "varying" declarations
 * (glsl_utils.c: strstr(text, "varying") with a space/tab after the keyword)
 * and reserving one GXM TEXCOORD slot per hit -- MAX_CG_TEXCOORD_ID is 10.
 * The scan runs before any preprocessing, so it counts declarations the real
 * compiler will throw away.
 *
 * KOTOR builds each vertex program by concatenating 10 chunk files into one
 * ubershader carrying 20 raw 'varying' declarations, of which typically only
 * 2-4 survive the #if guards. vitaGL reserves all 20, blows past slot 10,
 * force-binds the overflow to TEXCOORD9 and then faults inside the system
 * shader compiler during glLinkProgram. #if-guarding the declarations does not
 * help, because vitaGL never evaluates #if.
 *
 * So we do the elimination for it. We run the conditional-compilation subset of
 * the C preprocessor over the source and blank the keyword (7 chars -> 7
 * spaces, so every offset in the buffer is preserved) on any occurrence that is
 * either inside a comment or inside an inactive branch. What is left is exactly
 * the set of live declarations, and the real compiler still sees the original
 * text -- our blanks only ever land in regions it discards anyway.
 *
 * This is deliberately conservative: anything we cannot parse confidently
 * aborts the whole pass and the source goes through untouched.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "glsl_prep.h"

#define MAX_MACROS 256
#define MAX_COND_DEPTH 64
#define MAX_MARKS 512

static char g_last_error[160];

const char *glsl_prep_last_error(void) {
  return g_last_error[0] ? g_last_error : "unknown parser failure";
}

typedef struct {
  char name[64];
  long val;
} macro_t;

typedef struct {
  macro_t macros[MAX_MACROS];
  int nmacros;
  /* Conditional stack. active[] is the effective state at that depth, taken[]
   * records whether any branch of this #if chain has already been used. */
  int active[MAX_COND_DEPTH];
  int taken[MAX_COND_DEPTH];
  int depth;
  int err;
} prep_t;

/* ---------------------------------------------------------------- macros -- */

static macro_t *macro_find(prep_t *p, const char *name, int len) {
  for (int i = 0; i < p->nmacros; i++) {
    if ((int)strlen(p->macros[i].name) == len && !strncmp(p->macros[i].name, name, len))
      return &p->macros[i];
  }
  return NULL;
}

static void macro_define(prep_t *p, const char *name, int len, long val) {
  if (len <= 0 || len >= (int)sizeof(((macro_t *)0)->name)) return;
  macro_t *m = macro_find(p, name, len);
  if (!m) {
    if (p->nmacros >= MAX_MACROS) { p->err = 1; return; }
    m = &p->macros[p->nmacros++];
    memcpy(m->name, name, len);
    m->name[len] = '\0';
  }
  m->val = val;
}

static void macro_undef(prep_t *p, const char *name, int len) {
  macro_t *m = macro_find(p, name, len);
  if (!m) return;
  int idx = (int)(m - p->macros);
  p->macros[idx] = p->macros[--p->nmacros];
}

/* ------------------------------------------------------- expression eval -- */

/* Recursive-descent evaluator for the #if subset these shaders use:
 * defined(X), !, &&, ||, ==, !=, <, >, <=, >=, +, -, parentheses, decimal and
 * hex literals, and bare identifiers (undefined ones read as 0, per C). */

typedef struct {
  const char *s;
  prep_t *p;
} eval_t;

static void ev_ws(eval_t *e) {
  while (*e->s == ' ' || *e->s == '\t' || *e->s == '\r') e->s++;
}

static int is_id_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_id_char(char c) { return is_id_start(c) || (c >= '0' && c <= '9'); }

static long ev_or(eval_t *e);

static long ev_primary(eval_t *e) {
  ev_ws(e);
  if (*e->s == '(') {
    e->s++;
    long v = ev_or(e);
    ev_ws(e);
    if (*e->s != ')') { e->p->err = 1; return 0; }
    e->s++;
    return v;
  }
  if (*e->s == '!') { e->s++; return !ev_primary(e); }
  if (*e->s == '-') { e->s++; return -ev_primary(e); }
  if (*e->s == '+') { e->s++; return ev_primary(e); }
  if (*e->s >= '0' && *e->s <= '9') {
    char *end = NULL;
    long v = strtol(e->s, &end, 0); /* base 0 -> handles 0x1101 */
    if (end == e->s) { e->p->err = 1; return 0; }
    e->s = end;
    return v;
  }
  if (is_id_start(*e->s)) {
    const char *id = e->s;
    while (is_id_char(*e->s)) e->s++;
    int len = (int)(e->s - id);
    if (len == 7 && !strncmp(id, "defined", 7)) {
      ev_ws(e);
      int paren = 0;
      if (*e->s == '(') { paren = 1; e->s++; ev_ws(e); }
      const char *nm = e->s;
      while (is_id_char(*e->s)) e->s++;
      int nlen = (int)(e->s - nm);
      if (nlen == 0) { e->p->err = 1; return 0; }
      if (paren) {
        ev_ws(e);
        if (*e->s != ')') { e->p->err = 1; return 0; }
        e->s++;
      }
      return macro_find(e->p, nm, nlen) != NULL;
    }
    macro_t *m = macro_find(e->p, id, len);
    return m ? m->val : 0; /* undefined identifier evaluates to 0 */
  }
  e->p->err = 1;
  return 0;
}

static long ev_rel(eval_t *e) {
  long v = ev_primary(e);
  for (;;) {
    ev_ws(e);
    if (e->s[0] == '<' && e->s[1] == '=') { e->s += 2; v = (v <= ev_primary(e)); }
    else if (e->s[0] == '>' && e->s[1] == '=') { e->s += 2; v = (v >= ev_primary(e)); }
    else if (e->s[0] == '<') { e->s++; v = (v < ev_primary(e)); }
    else if (e->s[0] == '>') { e->s++; v = (v > ev_primary(e)); }
    else return v;
  }
}

static long ev_eq(eval_t *e) {
  long v = ev_rel(e);
  for (;;) {
    ev_ws(e);
    if (e->s[0] == '=' && e->s[1] == '=') { e->s += 2; v = (v == ev_rel(e)); }
    else if (e->s[0] == '!' && e->s[1] == '=') { e->s += 2; v = (v != ev_rel(e)); }
    else return v;
  }
}

static long ev_and(eval_t *e) {
  long v = ev_eq(e);
  for (;;) {
    ev_ws(e);
    if (e->s[0] == '&' && e->s[1] == '&') { e->s += 2; long r = ev_eq(e); v = (v && r); }
    else return v;
  }
}

static long ev_or(eval_t *e) {
  long v = ev_and(e);
  for (;;) {
    ev_ws(e);
    if (e->s[0] == '|' && e->s[1] == '|') { e->s += 2; long r = ev_and(e); v = (v || r); }
    else return v;
  }
}

static int eval_cond(prep_t *p, const char *expr) {
  eval_t e = { expr, p };
  long v = ev_or(&e);
  ev_ws(&e);
  if (*e.s != '\0') p->err = 1; /* trailing junk -> we did not understand it */
  return v != 0;
}

/* --------------------------------------------------------- directives ---- */

static int cur_active(prep_t *p) {
  return p->depth == 0 ? 1 : p->active[p->depth - 1];
}

static int parent_active(prep_t *p) {
  return p->depth <= 1 ? 1 : p->active[p->depth - 2];
}

/* line/linelen describe one logical source line with comments already stripped. */
static void do_directive(prep_t *p, const char *line, int linelen) {
  char buf[1024];
  if (linelen >= (int)sizeof(buf)) { p->err = 1; return; }
  memcpy(buf, line, linelen);
  buf[linelen] = '\0';

  char *s = buf;
  while (*s == ' ' || *s == '\t') s++;
  if (*s != '#') return;
  s++;
  while (*s == ' ' || *s == '\t') s++;

  const char *d = s;
  while (is_id_char(*s)) s++;
  int dlen = (int)(s - d);
  while (*s == ' ' || *s == '\t') s++;

#define DIR_IS(str) (dlen == (int)sizeof(str) - 1 && !strncmp(d, str, dlen))

  if (DIR_IS("if") || DIR_IS("ifdef") || DIR_IS("ifndef")) {
    if (p->depth >= MAX_COND_DEPTH) { p->err = 1; return; }
    int val;
    if (DIR_IS("if")) {
      val = cur_active(p) ? eval_cond(p, s) : 0;
      /* Still parse inactive branches for syntax we do not know, but do not let
       * an unparsed inactive branch poison the result -- we simply treat the
       * whole nest as unknown and bail out. */
    } else {
      const char *nm = s;
      while (is_id_char(*s)) s++;
      int nlen = (int)(s - nm);
      if (nlen == 0) { p->err = 1; return; }
      int def = macro_find(p, nm, nlen) != NULL;
      val = DIR_IS("ifdef") ? def : !def;
    }
    int act = cur_active(p) && val;
    p->active[p->depth] = act;
    p->taken[p->depth] = act;
    p->depth++;
    return;
  }

  if (DIR_IS("elif")) {
    if (p->depth == 0) { p->err = 1; return; }
    int lvl = p->depth - 1;
    if (p->taken[lvl] || !parent_active(p)) {
      p->active[lvl] = 0;
    } else {
      int val = eval_cond(p, s);
      p->active[lvl] = val;
      if (val) p->taken[lvl] = 1;
    }
    return;
  }

  if (DIR_IS("else")) {
    if (p->depth == 0) { p->err = 1; return; }
    int lvl = p->depth - 1;
    p->active[lvl] = (!p->taken[lvl] && parent_active(p));
    if (p->active[lvl]) p->taken[lvl] = 1;
    return;
  }

  if (DIR_IS("endif")) {
    if (p->depth == 0) { p->err = 1; return; }
    p->depth--;
    return;
  }

  if (DIR_IS("define")) {
    if (!cur_active(p)) return;
    const char *nm = s;
    while (is_id_char(*s)) s++;
    int nlen = (int)(s - nm);
    if (nlen == 0) { p->err = 1; return; }
    if (*s == '(') { p->err = 1; return; } /* function-like: not our business */
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') {
      macro_define(p, nm, nlen, 1); /* bare #define X -- only ever tested with #ifdef */
    } else {
      /* Value must be an integer constant expression for us to track it. If it
       * is not (a float, a vector, whatever), record it as defined-but-opaque:
       * #ifdef still works, and any #if that reads it will fail the parse and
       * abort the pass rather than guess. */
      eval_t e = { s, p };
      int saved_err = p->err;
      p->err = 0;
      long v = ev_or(&e);
      ev_ws(&e);
      int ok = (!p->err && *e.s == '\0');
      p->err = saved_err;
      macro_define(p, nm, nlen, ok ? v : 0);
    }
    return;
  }

  if (DIR_IS("undef")) {
    if (!cur_active(p)) return;
    const char *nm = s;
    while (is_id_char(*s)) s++;
    macro_undef(p, nm, (int)(s - nm));
    return;
  }

  /* #version, #extension, #pragma, #line, #error -- irrelevant here. */
#undef DIR_IS
}

/* ------------------------------------------------------------- the pass -- */

/* Match vitaGL's own detection rule exactly: the literal "varying" followed by
 * a space or tab, and (unlike vitaGL, which does not check) starting on a token
 * boundary so we never blank the tail of an identifier. */
static int is_varying_hit(const char *base, const char *at) {
  if (strncmp(at, "varying", 7)) return 0;
  if (at[7] != ' ' && at[7] != '\t') return 0;
  if (at > base && is_id_char(at[-1])) return 0;
  return 1;
}

static long macro_value(prep_t *p, const char *name, long fallback) {
  macro_t *m = macro_find(p, name, (int)strlen(name));
  return m ? m->val : fallback;
}

/* KOTOR concatenates fixed-function chunks whose interface declarations are
 * unconditional even when the generated program cannot execute those paths.
 * Its desktop driver drops the unused outputs, but vitaGL assigns TEXCOORDs
 * before GLSL dead-code elimination. Mirror the proven KOTOR 1 shader guards
 * here without shipping modified game assets: removing only the qualifier keeps
 * the declaration as a private global, so dead helper functions still parse. */
static int kotor_inactive_varying(prep_t *p, const char *line) {
  static const char *const per_fragment[] = {
    "v_ambientAndEmission", "v_normal",
    "v_light0Vector", "v_light0HalfVector",
    "v_light1Vector", "v_light1HalfVector",
    "v_light2Vector", "v_light2HalfVector",
  };
  if (macro_value(p, "PER_FRAGMENT_LIGHTING_ENABLED", 0) == 0) {
    for (unsigned i = 0; i < sizeof(per_fragment) / sizeof(per_fragment[0]); i++)
      if (strstr(line, per_fragment[i])) return 1;
  }

  for (int i = 0; i < 6; i++) {
    char varying[32], enabled[32];
    snprintf(varying, sizeof(varying), "v_clipPlane%dDistance", i);
    snprintf(enabled, sizeof(enabled), "CLIP_PLANE%d_ENABLED", i);
    if (strstr(line, varying) && macro_value(p, enabled, -1) == 0) return 1;
  }

  for (int i = 0; i < 3; i++) {
    char varying[24], texcoord[28], texture[24];
    snprintf(varying, sizeof(varying), "v_texCoord%d", i);
    snprintf(texcoord, sizeof(texcoord), "TEXCOORD%d_ENABLED", i);
    snprintf(texture, sizeof(texture), "TEXTURE%d_ENABLED", i);
    if (strstr(line, varying) &&
        (macro_value(p, texcoord, -1) == 0 || macro_value(p, texture, -1) == 0))
      return 1;
  }

  if (macro_value(p, "FOG_ENABLED", -1) == 0 &&
      (strstr(line, "v_fogFactor") || strstr(line, "v_eyeDistance")))
    return 1;

  return 0;
}

int glsl_prep_strip_dead_varyings(char *src, int *out_raw, int *out_live) {
  prep_t p;
  memset(&p, 0, sizeof(p));
  g_last_error[0] = '\0';

  int marks[MAX_MARKS];
  int nmarks = 0;
  int raw = 0, live = 0;
  int in_block_comment = 0;

  char line[1024];
  const char *cur = src;
  int line_no = 1;

  while (*cur) {
    const char *eol = strchr(cur, '\n');
    int len = eol ? (int)(eol - cur) : (int)strlen(cur);
    if (len >= (int)sizeof(line) - 1) {
      snprintf(g_last_error, sizeof(g_last_error), "line %d exceeds parser buffer", line_no);
      return 0;
    }

    /* Build a comment-free copy of the line for directive parsing, and record
     * which columns of the original are commented out. */
    int code_len = 0;
    int commented[1024];
    int cap = len < (int)sizeof(line) - 1 ? len : (int)sizeof(line) - 1;
    int in_line_comment = 0;
    for (int i = 0; i < cap; i++) {
      int is_comment = 1;
      if (in_block_comment) {
        if (cur[i] == '*' && i + 1 < cap && cur[i + 1] == '/') { in_block_comment = 0; commented[i] = 1; i++; commented[i] = 1; continue; }
      } else if (in_line_comment) {
        /* rest of line */
      } else if (cur[i] == '/' && i + 1 < cap && cur[i + 1] == '/') {
        in_line_comment = 1;
      } else if (cur[i] == '/' && i + 1 < cap && cur[i + 1] == '*') {
        in_block_comment = 1;
        commented[i] = 1; i++; commented[i] = 1; continue;
      } else {
        is_comment = 0;
        line[code_len++] = cur[i];
      }
      commented[i] = is_comment;
    }
    line[code_len] = '\0';

    /* A directive line is one whose first non-blank code character is '#'. */
    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;
    if (*t == '#') {
      do_directive(&p, line, code_len);
      if (p.err) {
        snprintf(g_last_error, sizeof(g_last_error), "line %d directive: %.110s", line_no, line);
        return 0;
      }
    } else {
      int active = cur_active(&p);
      for (int i = 0; i + 7 <= cap; i++) {
        if (!is_varying_hit(cur, cur + i)) continue;
        raw++; /* vitaGL would latch onto this one */
        if (commented[i] || !active || kotor_inactive_varying(&p, line))
          {
            if (nmarks >= MAX_MARKS) {
              snprintf(g_last_error, sizeof(g_last_error), "line %d exceeds varying mark budget", line_no);
              return 0;
            }
            marks[nmarks++] = (int)(cur - src) + i;
          }
        else
          live++;
        i += 6;
      }
    }

    if (!eol) break;
    cur = eol + 1;
    line_no++;
  }

  if (p.depth != 0 || p.err) {
    snprintf(g_last_error, sizeof(g_last_error), "end of source with conditional depth %d", p.depth);
    return 0;
  }

  for (int i = 0; i < nmarks; i++)
    memset(src + marks[i], ' ', 7);

  if (out_raw) *out_raw = raw;
  if (out_live) *out_live = live;
  return 1;
}
