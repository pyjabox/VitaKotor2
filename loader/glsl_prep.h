/* glsl_prep.h -- pre-pass over GLSL source before handing it to vitaGL. */
#ifndef GLSL_PREP_H
#define GLSL_PREP_H

/* Blank out every 'varying' keyword vitaGL's translator would latch onto but
 * that is not actually a live declaration -- i.e. one sitting inside a comment
 * or inside an inactive #if branch. Mutates src in place (same length, the
 * keyword is overwritten with spaces) and reports how many occurrences vitaGL
 * would have seen (raw) versus how many survive (live).
 *
 * Returns 1 on success, 0 if the source could not be parsed confidently, in
 * which case src is left completely untouched. */
int glsl_prep_strip_dead_varyings(char *src, int *out_raw, int *out_live);
const char *glsl_prep_last_error(void);

#endif
