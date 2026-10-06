/* gl_patch.c -- wire the companion's GLES2 imports to vitaGL.
 * Historical diagnostics and performance examples are inherited from
 * VitaKotor (KOTOR I) unless explicitly marked as KOTOR II.
 *
 * libObbVfs.so exposes android_port_gl* wrappers to the game, but the
 * wrappers themselves import the *real* gl* entry points (141 of them). Those
 * were deliberately left unresolved during skeleton bring-up; the first GL call
 * (OpenGLES20Implementation::init -> glGetIntegerv) therefore jumped through an
 * unresolved PLT slot and prefetch-aborted. This table resolves them to vitaGL.
 *
 * Three groups:
 *   (1) direct     -- integer/pointer signatures map straight to vitaGL.
 *   (2) float shims -- functions taking GLfloat BY VALUE. The Android .so is
 *       softfp (floats in core regs r0-r3); vitaGL here is hardfp (floats in
 *       VFP). A hardfp function whose params are declared uint32_t receives the
 *       softfp bit patterns in core regs unchanged; we reinterpret to float and
 *       call vitaGL normally (hardfp->hardfp). Pointer variants (*fv, Matrix*fv)
 *       need no shim -- pointers pass in core regs in both ABIs.
 *   (3) gap stubs  -- 14 symbols vitaGL does not implement; safe bring-up
 *       defaults (no-ops / zero-fill) so shader/query paths don't wild-branch.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "so_util.h"
#include "config.h"
#include "main.h"
#include "obb_cache.h"
#include "gl_state_filter.h"
#include "dxt_native.h"
#include "gl_patch.h"
#include "glsl_prep.h"
#include "dynlib.h"
#include "audio_patch.h"
#include "bink_patch.h"
#include "sdl_patch.h"
#include "log.h"
#include "loadscreen.h"

static inline float u2f(uint32_t u) { union { uint32_t u; float f; } c; c.u = u; return c.f; }

// Per-call GL trace budget lives in config.h (GL_TRACE_LIMIT) so it can be
// re-armed for bring-up without editing this file. Declared early so both
// glViewport_t and the GLLOG block below can gate on it.
static int g_gl_seq = 0;

/* Per-scene-stage timing of imported GL state/uniform calls. The 2026-09-10
 * spec's "next investigation": attribute the per-draw setup cost inside
 * RenderFlat/RenderEMLM/PartTriMesh::Draw to the underlying vitaGL calls
 * before suspecting engine math. Timed values carry a ~0.5-1 us/call
 * instrumentation floor (the closing timestamp syscall); subtract n x ~1 us
 * for fine interpretations. Forward declaration here; the counters and the
 * definition live with the stage tracker below.
 *
 * Telemetry builds only. The T08 heavy window issued ~6,550 of these calls per
 * frame (~22 per draw), and two timestamp syscalls each are several ms of frame
 * time -- a cost v0.2.0 kept paying with telemetry off. Release builds call
 * straight through. */
#if PERFORMANCE_TELEMETRY_ENABLE
static void gl_state_note(uint64_t us);
#define GL_TIME_STATE(call) do {                                     \
    uint64_t st_ = sceKernelGetProcessTimeWide();                    \
    call;                                                            \
    gl_state_note(sceKernelGetProcessTimeWide() - st_);              \
  } while (0)
#else
#define GL_TIME_STATE(call) do { call; } while (0)
#endif

/* Lifetime draw-call count, always maintained (one increment per draw).
 * Occlusion culling differences it around each Gob::Render to tell Gobs that
 * draw nothing from Gobs that are hidden. */
static volatile uint32_t g_gl_total_draws = 0;
uint32_t gl_patch_total_draws(void) { return g_gl_total_draws; }

/* -- (2) softfp->hardfp float-by-value shims -------------------------------- */
static void glClearColor_s(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  glClearColor(u2f(r), u2f(g), u2f(b), u2f(a));
}
static void glClearDepthf_s(uint32_t d) { glClearDepthf(u2f(d)); }
static void glDepthRangef_s(uint32_t n, uint32_t f) { glDepthRangef(u2f(n), u2f(f)); }
static void glLineWidth_s(uint32_t w) { glLineWidth(u2f(w)); }
static void glPolygonOffset_s(uint32_t factor, uint32_t units) { glPolygonOffset(u2f(factor), u2f(units)); }
static void glTexParameterf_s(uint32_t target, uint32_t pname, uint32_t param) {
  glTexParameterf(target, pname, u2f(param));
}
static void glUniform1f_s(uint32_t loc, uint32_t v0) { GL_TIME_STATE(glUniform1f(loc, u2f(v0))); }
static void glUniform2f_s(uint32_t loc, uint32_t v0, uint32_t v1) { GL_TIME_STATE(glUniform2f(loc, u2f(v0), u2f(v1))); }
static void glUniform3f_s(uint32_t loc, uint32_t v0, uint32_t v1, uint32_t v2) {
  GL_TIME_STATE(glUniform3f(loc, u2f(v0), u2f(v1), u2f(v2)));
}
static void glUniform4f_s(uint32_t loc, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
  GL_TIME_STATE(glUniform4f(loc, u2f(v0), u2f(v1), u2f(v2), u2f(v3)));
}
static void glVertexAttrib1f_s(uint32_t i, uint32_t v0) { GL_TIME_STATE(glVertexAttrib1f(i, u2f(v0))); }
static void glVertexAttrib2f_s(uint32_t i, uint32_t v0, uint32_t v1) { GL_TIME_STATE(glVertexAttrib2f(i, u2f(v0), u2f(v1))); }
static void glVertexAttrib3f_s(uint32_t i, uint32_t v0, uint32_t v1, uint32_t v2) {
  GL_TIME_STATE(glVertexAttrib3f(i, u2f(v0), u2f(v1), u2f(v2)));
}
static void glVertexAttrib4f_s(uint32_t i, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
  GL_TIME_STATE(glVertexAttrib4f(i, u2f(v0), u2f(v1), u2f(v2), u2f(v3)));
}

/* -- (3) gap stubs: not implemented by vitaGL ------------------------------- */
static void glBlendColor_g(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { (void)r;(void)g;(void)b;(void)a; }
static void glCompressedTexSubImage2D_g(GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h,
                                        GLenum fmt, GLsizei sz, const void *d) {
  (void)t;(void)l;(void)xo;(void)yo;(void)w;(void)h;(void)fmt;(void)sz;(void)d;
}
static void glDetachShader_g(GLuint p, GLuint s) { (void)p;(void)s; }
static void glGetBufferPointervOES_g(GLenum target, GLenum pname, void **params) {
  (void)target; (void)pname;
  if (params) *params = NULL;
}
static void glGetRenderbufferParameteriv_g(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
static void glGetShaderPrecisionFormat_g(GLenum st, GLenum pt, GLint *range, GLint *precision) {
  (void)st;(void)pt;
  if (range) { range[0] = 127; range[1] = 127; }
  if (precision) *precision = 23;  /* highp float */
}
static void glGetTexParameterfv_g(GLenum t, GLenum p, GLfloat *params) { (void)t;(void)p; if (params) params[0] = 0.0f; }
static void glGetTexParameteriv_g(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
static void glGetUniformfv_g(GLuint prog, GLint loc, GLfloat *params) { (void)prog;(void)loc; if (params) params[0] = 0.0f; }
static void glGetUniformiv_g(GLuint prog, GLint loc, GLint *params) { (void)prog;(void)loc; if (params) params[0] = 0; }
static GLboolean glIsBuffer_g(GLuint b) { return b ? GL_TRUE : GL_FALSE; }
static GLboolean glIsShader_g(GLuint s) { return s ? GL_TRUE : GL_FALSE; }
static void glSampleCoverage_g(uint32_t value, GLboolean invert) { (void)value;(void)invert; }
static void glTexParameterfv_g(GLenum target, GLenum pname, const GLfloat *params) {
  if (params) glTexParameteri(target, pname, (GLint)params[0]);
}
static void glValidateProgram_g(GLuint p) { (void)p; }


/* -- GL lifecycle tracing ---------------------------------------------------
 * The game's GL init is otherwise invisible in the log. These wrappers trace
 * the shape of bring-up (capability queries, shader pipeline, first frame) so a
 * silent hang/abort in the GL region is localised. Shader compile/link failures
 * dump the vitaGL/vitashark info log -- the usual suspect for a stall here.
 */
static const GLubyte *glGetString_t(GLenum name) {
  log_printf("[GL] glGetString(0x%x) ...", (unsigned)name);
  const GLubyte *s = glGetString(name);
  log_printf("[GL] glGetString(0x%x) -> \"%s\"", (unsigned)name, s ? (const char *)s : "(null)");
  return s;
}
static GLuint glCreateShader_t(GLenum type) {
  log_printf("[GL] glCreateShader(0x%x) ...", (unsigned)type);   // log BEFORE: first
  GLuint id = glCreateShader(type);                              // call may lazily
  log_printf("[GL] glCreateShader(0x%x) -> %u", (unsigned)type, (unsigned)id);  // init vitashark
  return id;
}

static void glShaderSource_t(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
  log_printf("[GL] glShaderSource(sh=%u, count=%d) ===begin dump===", (unsigned)sh, count);
  // Dump the define header (str[0]) line-by-line -- its concrete macro values
  // decide which #if branches (and thus which varyings) are live. Also emit every
  // 'varying' source line across all strings so we can count declared varyings.
  char line[256];
  for (int s = 0; s < count && str; s++) {
    const char *p = str[s];
    if (!p) continue;
    int header = (s == 0);
    while (*p) {
      int n = 0;
      while (n < 255 && p[n] && p[n] != '\n') { line[n] = p[n]; n++; }
      line[n] = '\0';
      // Header: log all lines. Other chunks: only 'varying' declaration lines.
      if (header) {
        if (n > 0) log_printf("[GLSRC h] %s", line);
      } else {
        const char *v = line; while (*v == ' ' || *v == '\t') v++;
        if (!strncmp(v, "varying", 7)) log_printf("[GLSRC s%d] %s", s, line);
      }
      p += n;
      if (*p == '\n') p++;
    }
  }
  log_printf("[GL] glShaderSource(sh=%u) ===end dump===", (unsigned)sh);

  // vitaGL finds varying declarations by text-scanning for the keyword, with no
  // preprocessor evaluation at all -- so it reserves a GXM TEXCOORD slot for
  // every declaration in the ubershader, including the ~18 that the #if guards
  // discard and any that merely appear in a comment. That overflows
  // MAX_CG_TEXCOORD_ID (10), force-binds the excess to TEXCOORD9 and faults
  // inside the shader compiler at glLinkProgram. Do the dead-declaration
  // elimination here, on a concatenated copy, before vitaGL ever sees it.
  size_t total = 1;
  for (int s = 0; s < count && str; s++)
    total += (len && len[s] >= 0) ? (size_t)len[s] : (str[s] ? strlen(str[s]) : 0);

  char *joined = (char *)malloc(total);
  if (joined) {
    size_t off = 0;
    for (int s = 0; s < count && str; s++) {
      if (!str[s]) continue;
      size_t l = (len && len[s] >= 0) ? (size_t)len[s] : strlen(str[s]);
      memcpy(joined + off, str[s], l);
      off += l;
    }
    joined[off] = '\0';

    // SKINNING BISECT (log77). Characters explode into spikes, and every input to
    // the skinned shader has now been proven correct in turn:
    //   - .mdl/.mdx bytes match an offline LZMA decode exactly
    //   - attributes are contiguous GL_FLOAT at stride 64, nothing normalised
    //   - glUniform4fv delivers count=51 of clean near-identity bone rows
    //   - the LINKED program reports u_boneMatrices size=51 type=GL_FLOAT_VEC4,
    //     so ShaccCg did not collapse the array
    // Three theories disproved by measurement, so stop theorising about the one
    // remaining suspect (GXM's execution of `u_boneMatrices[indices.x]`, a
    // dynamic uniform-array read driven by a vertex attribute) and test it.
    // Forcing the ubershader's own USE_SKIN switch to 0 takes the `pos = a_position`
    // branch instead, bypassing the indexed read entirely while changing nothing
    // else. The rewrite is length-preserving ('1' -> '0'), so every offset in the
    // buffer -- and the varying pre-pass that runs next -- is unaffected.
    // Spikes gone  => the dynamic indexed read is the culprit, and the fix belongs
    //                 in vitaGL's GLSL->Cg translation.
    // Spikes stay  => skinning is exonerated and the fault is elsewhere entirely
    //                 (geometry/index buffers), which redirects the whole hunt.
    // Cost while enabled: characters render in BIND POSE -- static, but correctly
    // shaped, which is strictly better to look at than the current explosion.
    // This is a diagnostic, not the fix; revert once the cause is known.
    int skin_off = 0;
    if (SKIN_BISECT_DISABLE) {
      char *d = joined;
      while ((d = strstr(d, "#define USE_SKIN 1")) != NULL) {
        d[sizeof("#define USE_SKIN ") - 1] = '0';
        skin_off++;
        d += sizeof("#define USE_SKIN ") - 1;
      }
      if (skin_off)
        log_printf("[GL] SKIN BISECT sh=%u: forced USE_SKIN 1 -> 0 (%d site%s)",
                   (unsigned)sh, skin_off, skin_off == 1 ? "" : "s");
    }

    // Skinning bone-index rounding fix. The bisect above proved the dynamic
    // uniform-array read is what wrecks characters, and everything feeding it is
    // provably correct (attributes, bone data, and the linked program's
    // u_boneMatrices[51] all verified). What is left is the index arithmetic:
    //     ivec4 indices = ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0));
    // ivec4() TRUNCATES. Each bone occupies 3 consecutive vec4 rows, so the true
    // values are exact multiples of 3 -- but only if a_matrixIndices survives as
    // an exact integer. If ShaccCg demotes the attribute to fp16, a stored 3.0
    // can arrive as 2.9999, and 3.0*2.9999 = 8.9997 truncates to 8 instead of 9:
    // that vertex then reads rows 8/9/10 rather than 9/10/11 and is transformed
    // by an entirely different bone. Only indices that land just under an integer
    // are affected, which is exactly why the models are mostly right with some
    // vertices flung away, rather than uniformly wrong.
    // Adding 0.5 before truncation turns floor() into round-to-nearest, making
    // the index robust to +/-0.5 of error while changing nothing when the value
    // is already exact. The replacement is written to the same 46 characters as
    // the original so every offset in the buffer -- and the varying pre-pass that
    // runs next -- is untouched. `clamp` deliberately stays in the float domain:
    // GLSL ES 1.00 has no integer overload of clamp().
    int skin_fix = 0;
    if (SKIN_INDEX_ROUND_FIX) {
      static const char kOld[] = "ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0))";
      static const char kNew[] = "ivec4(clamp(3.*a_matrixIndices+.5,0.,50.))    ";
      char *d = joined;
      while ((d = strstr(d, kOld)) != NULL) {
        memcpy(d, kNew, sizeof(kNew) - 1);   // same length: offsets preserved
        skin_fix++;
        d += sizeof(kNew) - 1;
      }
      if (skin_fix)
        log_printf("[GL] SKIN FIX sh=%u: bone index round-to-nearest (%d site%s)",
                   (unsigned)sh, skin_fix, skin_fix == 1 ? "" : "s");
    }
    skin_off += skin_fix;   // either rewrite means we must pass OUR buffer on

    int post_brightness_fix = 0;
    {
      char *d = joined;
      while ((d = strstr(d, "float powamt")) != NULL) {
        char *eq = strchr(d, '=');
        char *semi = eq ? strchr(eq, ';') : NULL;
        if (!eq || !semi || semi - eq < 5 || semi - eq > 80) break;
        memcpy(eq + 1, " 1.0", 4);
        memset(eq + 5, ' ', (size_t)(semi - (eq + 5)));
        post_brightness_fix++;
        d = semi + 1;
      }
      if (post_brightness_fix)
        log_printf("[GL] POST FIX sh=%u: forced neutral brightness exponent",
                   (unsigned)sh);
    }
    skin_off += post_brightness_fix;

    int raw = 0, live = 0;
    if (glsl_prep_strip_dead_varyings(joined, &raw, &live)) {
      log_printf("[GL] glShaderSource(sh=%u): varyings %d seen by vitaGL -> %d live",
                 (unsigned)sh, raw, live);
      if (live > 10)
        log_printf("[GL] WARNING sh=%u: %d live varyings still exceeds the 10-slot "
                   "TEXCOORD budget -- link will overflow", (unsigned)sh, live);
      const GLchar *one = joined;
      glShaderSource(sh, 1, &one, NULL);
      free(joined);
      return;
    }
    // If the varying pre-pass declined we would normally hand back the game's
    // original strings -- but that would silently discard a USE_SKIN rewrite, so
    // pass our edited copy in that case.
    if (skin_off) {
      log_printf("[GL] glShaderSource(sh=%u): varying pre-pass declined, but "
                 "passing the SKIN-BISECT copy", (unsigned)sh);
      const GLchar *one = joined;
      glShaderSource(sh, 1, &one, NULL);
      free(joined);
      return;
    }
    log_printf("[GL] glShaderSource(sh=%u): varying pre-pass declined this source, "
               "passing it through unmodified: %s", (unsigned)sh,
               glsl_prep_last_error());
    log_flush();
    free(joined);
  }

  glShaderSource(sh, count, str, len);
}
static void glCompileShader_t(GLuint sh) {
  log_printf("[GL] glCompileShader(%u) ...", (unsigned)sh);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char info[512]; info[0] = '\0';
    glGetShaderInfoLog(sh, sizeof(info), NULL, info);
    log_printf("[GL] !!! shader %u COMPILE FAILED: %s", (unsigned)sh, info);
  } else {
    log_printf("[GL] shader %u compiled OK", (unsigned)sh);
  }
}
// vitaGL's glReleaseShaderCompiler calls shark_end(), which tears the runtime
// shader compiler down for good. That is a reasonable thing to honour on a
// desktop driver, but vitaGL is in VGL_MODE_POSTPONED here: GLSL is translated
// and compiled lazily inside *every* glLinkProgram. Once shark is ended,
// shark_compile_shader_extended returns NULL on its first instruction -- without
// invoking the compiler, so it emits no diagnostics at all -- vitaGL leaves
// shader->prog NULL, and glLinkProgram's set_default_attrib_binding() hands that
// NULL to sceGxmProgramGetParameterCount, which faults at FAR=0x24 inside
// SceGxm. libObbVfs.so imports this entry point, so the game can pull the
// rug out from under every shader it has not compiled yet. Refuse to do it: we
// keep the compiler alive for the life of the process.
static void glReleaseShaderCompiler_t(void) {
  extern GLboolean is_shark_online;
  log_printf("[GL] glReleaseShaderCompiler() IGNORED (would shark_end() and make "
             "every later shader compile fail silently; shark_online=%d)",
             (int)is_shark_online);
}
static GLuint   g_cur_prog = 0;           /* redundant program-switch shadow */
static unsigned g_prog_skipped_win = 0;
static unsigned g_links_frame = 0;
static uint64_t g_link_us_frame = 0;

static void glLinkProgram_t(GLuint p) {
  extern GLboolean is_shark_online;
  log_printf("[GL] glLinkProgram(%u) ... shark_online=%d", (unsigned)p,
             (int)is_shark_online);
  uint64_t link_start = sceKernelGetProcessTimeWide();
  glLinkProgram(p);
  g_link_us_frame += sceKernelGetProcessTimeWide() - link_start;
  g_links_frame++;
  g_cur_prog = 0;             /* relinking can change what this id draws with */
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char info[512]; info[0] = '\0';
    glGetProgramInfoLog(p, sizeof(info), NULL, info);
    log_printf("[GL] !!! program %u LINK FAILED: %s", (unsigned)p, info);
  } else {
    log_printf("[GL] program %u linked OK", (unsigned)p);
    // Skinned characters explode into spikes even though the vertex layout and
    // the bone data we hand GL are both provably correct (attributes are
    // contiguous GL_FLOAT at stride 64; glUniform4fv gets count=51 of clean
    // near-identity rows). kotor.vert indexes `uniform vec4 u_boneMatrices[51]`
    // DYNAMICALLY from a vertex attribute, so the remaining suspect is what the
    // COMPILED GXM program made of that array. vitaGL reports it faithfully:
    // glGetActiveUniform's size comes straight from
    // sceGxmProgramParameterGetArraySize. If u_boneMatrices comes back with
    // size=1 (or anything < 51), ShaccCg collapsed the array and every dynamic
    // index reads the same row -- which is exactly what flings vertices onto
    // wrong bones. Dump it once per program, alongside the attributes so the
    // active set can be matched against the a_* locations we already log.
    GLint nu = 0, na = 0;
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
    glGetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &na);
    log_printf("[vtx] prog %u: %d active uniforms, %d active attributes",
               (unsigned)p, (int)nu, (int)na);
    for (GLint i = 0; i < nu && i < 48; i++) {
      char nm[96]; GLint sz = -1; GLenum ty = 0; GLsizei len = 0;
      nm[0] = '\0';
      glGetActiveUniform(p, (GLuint)i, sizeof(nm), &len, &sz, &ty, nm);
      log_printf("[vtx]   uniform[%d] \"%.48s\" size=%d type=0x%x", (int)i, nm,
                 (int)sz, (unsigned)ty);
    }
    for (GLint i = 0; i < na && i < 24; i++) {
      char nm[96]; GLint sz = -1; GLenum ty = 0; GLsizei len = 0;
      nm[0] = '\0';
      glGetActiveAttrib(p, (GLuint)i, sizeof(nm), &len, &sz, &ty, nm);
      log_printf("[vtx]   attrib[%d] \"%.48s\" size=%d type=0x%x", (int)i, nm,
                 (int)sz, (unsigned)ty);
    }
  }
}
static void glViewport_t(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (g_gl_seq < GL_TRACE_LIMIT)  // silence with the rest of the per-call trace
    log_printf("[GL] glViewport(%d, %d, %d, %d)", x, y, (int)w, (int)h);
  glViewport(x, y, w, h);
  if (g_gl_gui_viewport_scope) {
    glScissor(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
  }
}
// Draw/clear accounting. The first few of each are logged in full; after that we
// only count, and gl_patch_on_swap() prints a per-frame-window summary. Both the
// "since last summary" and lifetime totals are tracked so a stuck render loop
// (identical draw count every window) is distinguishable from an advancing one.
int g_gl_gui_viewport_scope = 0;
static int g_clear_n = 0, g_draw_n = 0;
static unsigned g_arrays_win = 0, g_elements_win = 0, g_clears_win = 0;
static unsigned g_arrays_tot = 0, g_elements_tot = 0;
static unsigned g_clears_frame = 0;
#if DRAW_FRAME_BENCHMARK_ENABLE
static uint64_t g_draw_frame_draws = 0;
static uint64_t g_draw_frame_frames = 0;
static uint64_t g_draw_frame_start_us = 0;
#endif
static void glClear_t(GLbitfield mask) {
  if (g_clear_n < 5) log_printf("[GL] glClear(0x%x) #%d", (unsigned)mask, g_clear_n);
  g_clear_n++;
  g_clears_win++; g_clears_frame++;
  glClear(mask);
}
// Text-draw trace: g_gl_text_draw is raised around the game's GUI-string Draw, so
// these lines isolate the glyph draws from the thousands of ordinary scene draws.
// g_cur_tex mirrors the GL_TEXTURE_2D binding -- a glyph quad drawn with texture 0
// (or with a non-atlas texture) is the difference between "text never reaches GL"
// and "text reaches GL untextured".
int g_gl_text_draw = 0;
static unsigned g_cur_tex = 0;
static int g_textdraw_n = 0;
#define TEXTDRAW_LOG_MAX 80
#define GL_MAX_TEXUNITS 8
static unsigned g_active_unit = 0;
static GLuint g_bound2d[GL_MAX_TEXUNITS];
static uint64_t g_bound_texture_signature[GL_MAX_TEXUNITS];

// Draws bucketed by the SIZE of the bound texture. Keying on dimensions rather than
// texture id is deliberate: ids are recycled after glDeleteTextures (tex10 was
// re-uploaded six times under a different format), which made an id-keyed counter
// meaningless. A re-upload simply overwrites the dimension entry.
// 756x106 is the main-menu button art (20 of them, ios_mm_*_en.tga); if that bucket
// stays at 0 while the buttons are on screen, their quads never reach GL.
#define TEXDIM_MAX 512
static unsigned short g_tex_w[TEXDIM_MAX], g_tex_h[TEXDIM_MAX];
#define BUCKET_MAX 14
static unsigned short g_bk_w[BUCKET_MAX], g_bk_h[BUCKET_MAX];
static unsigned g_bk_n_draws[BUCKET_MAX];
static int g_bk_n = 0;

static void tex_note_size(unsigned tex, int w, int h) {
  if (tex < TEXDIM_MAX) { g_tex_w[tex] = (unsigned short)w; g_tex_h[tex] = (unsigned short)h; }
}
static void tex_note_draw(unsigned tex) {
  if (tex >= TEXDIM_MAX) return;
  unsigned short w = g_tex_w[tex], h = g_tex_h[tex];
  if (!w) return;                      // untextured or never uploaded
  for (int k = 0; k < g_bk_n; k++)
    if (g_bk_w[k] == w && g_bk_h[k] == h) { g_bk_n_draws[k]++; return; }
  if (g_bk_n < BUCKET_MAX) { g_bk_w[g_bk_n] = w; g_bk_h[g_bk_n] = h; g_bk_n_draws[g_bk_n++] = 1; }
}

/* Per-draw CPU cost is the remaining stutter lever: log119 hit ~745 draw calls a
 * frame at 13 fps (~103 us/draw), which is far too slow to be GPU fill. The two
 * usual causes are (a) client-side vertex arrays, which vitaGL must copy into the
 * circular pool on EVERY draw, and (b) redundant state changes. Count both so the
 * next tuning step is chosen from data rather than guessed. */
static unsigned g_draw_client_win = 0, g_draw_vbo_win = 0;
static unsigned g_prog_win = 0, g_texbind_win = 0, g_bufdata_win = 0;
static unsigned g_arrays_frame = 0, g_elements_frame = 0;
static unsigned g_draw_client_frame = 0, g_draw_vbo_frame = 0;
static unsigned g_prog_frame = 0, g_prog_skipped_frame = 0;
static unsigned g_texbind_frame = 0, g_bind_skipped_frame = 0;
static unsigned g_bufdata_frame = 0, g_texupload_frame = 0;
static uint64_t g_bufdata_bytes_frame = 0, g_texupload_bytes_frame = 0;

#define GL_STAGE_STACK_MAX 16
#define GL_STAGE_PROGRAM_MAX 64
typedef struct {
  uint8_t stage;
  uint64_t start_us;
  uint64_t child_us;
} GlStageFrame;

static const char *const g_stage_name[GL_STAGE_COUNT] = {
  "other", "single", "shadow", "dynamic", "static", "flare",
  "mesh", "gob", "hologram", "distort", "fade", "emitter"
};
static GlStageFrame g_stage_stack[GL_STAGE_STACK_MAX];
static unsigned g_stage_depth = 0, g_stage_overflow = 0, g_stage_mismatch = 0;
static unsigned g_stage_calls[GL_STAGE_COUNT];
static unsigned g_stage_arrays[GL_STAGE_COUNT], g_stage_elements[GL_STAGE_COUNT];
static uint64_t g_stage_exclusive_us[GL_STAGE_COUNT];
static unsigned g_stage_program[GL_STAGE_COUNT][GL_STAGE_PROGRAM_MAX + 1];
static uint64_t g_stage_draw_us[GL_STAGE_COUNT];
static uint64_t g_stage_program_us[GL_STAGE_COUNT][GL_STAGE_PROGRAM_MAX + 1];
static unsigned g_stage_index_client[GL_STAGE_COUNT], g_stage_index_ebo[GL_STAGE_COUNT];
static unsigned g_stage_program_unique[GL_STAGE_COUNT][GL_STAGE_PROGRAM_MAX + 1];
static unsigned g_stage_program_repeat[GL_STAGE_COUNT][GL_STAGE_PROGRAM_MAX + 1];
static unsigned g_draw_signature_overflow = 0;
/* Imported GL state/uniform call time per stage (GL_TIME_STATE), reset with
 * the other per-window stage counters. */
static uint64_t g_stage_state_us[GL_STAGE_COUNT];
static unsigned g_stage_state_n[GL_STAGE_COUNT];

#define GL_CALLEE_STACK_MAX 64
typedef struct {
  uint8_t callee;
  uint64_t start_us;
  uint64_t child_us;
} GlCalleeFrame;

static const char *const g_callee_name[GL_CALLEE_COUNT] = {
  "other", "visible", "gobRender", "partDraw", "triDraw", "flat",
  "lightmap", "envmap", "emlm", "vpEnable", "bindTex0", "proxyPart",
  "isRenderable", "getPath", "setInterleaved", "engineDraw", "poolLookup",
  "mainLoop", "msgpump"
};
static GlCalleeFrame g_callee_stack[GL_CALLEE_STACK_MAX];
static unsigned g_callee_depth = 0, g_callee_overflow = 0, g_callee_mismatch = 0;
static unsigned g_callee_calls[GL_CALLEE_COUNT], g_callee_draws[GL_CALLEE_COUNT];
static uint64_t g_callee_exclusive_us[GL_CALLEE_COUNT];

/* Passive per-window Gob census. Pointer identity is sufficient to determine
 * whether a view is dominated by a few high-draw objects or many ordinary
 * objects. No proprietary Gob fields are dereferenced. */
#define GL_GOB_IDENTITY_MAX 256
#define GL_GOB_STACK_MAX 16
typedef struct {
  const void *gob;
  char model[24];
  char room[24];
  unsigned calls, draws;
  unsigned multi_room;
} GlGobIdentity;
static GlGobIdentity g_gob_identity[GL_GOB_IDENTITY_MAX];
static GlGobIdentity *g_gob_stack[GL_GOB_STACK_MAX];
static unsigned g_gob_depth, g_gob_overflow, g_gob_table_overflow;

static void gl_draw_signature_note(unsigned stage, unsigned program, GLenum mode,
                                   GLsizei count, GLenum type, const void *indices);
static void gl_draw_signature_reset_frame(void);

void gl_perf_stage_enter(unsigned stage) {
  if (stage >= GL_STAGE_COUNT) stage = GL_STAGE_OUTSIDE;
  if (g_stage_depth >= GL_STAGE_STACK_MAX) {
    g_stage_overflow++;
    return;
  }
  GlStageFrame *frame = &g_stage_stack[g_stage_depth++];
  frame->stage = (uint8_t)stage;
  frame->start_us = sceKernelGetProcessTimeWide();
  frame->child_us = 0;
}

void gl_perf_stage_leave(unsigned stage) {
  if (g_stage_overflow) {
    g_stage_overflow--;
    return;
  }
  if (!g_stage_depth) {
    g_stage_mismatch++;
    return;
  }

  uint64_t now = sceKernelGetProcessTimeWide();
  GlStageFrame frame = g_stage_stack[--g_stage_depth];
  if (frame.stage != stage) g_stage_mismatch++;
  uint64_t elapsed = now - frame.start_us;
  unsigned id = frame.stage < GL_STAGE_COUNT ? frame.stage : GL_STAGE_OUTSIDE;
  g_stage_calls[id]++;
  g_stage_exclusive_us[id] += elapsed >= frame.child_us ? elapsed - frame.child_us : 0;
  if (g_stage_depth) g_stage_stack[g_stage_depth - 1].child_us += elapsed;
}

void gl_perf_callee_enter(unsigned callee) {
  if (callee >= GL_CALLEE_COUNT) callee = GL_CALLEE_OUTSIDE;
  if (g_callee_depth >= GL_CALLEE_STACK_MAX) {
    g_callee_overflow++;
    return;
  }
  GlCalleeFrame *frame = &g_callee_stack[g_callee_depth++];
  frame->callee = (uint8_t)callee;
  frame->start_us = sceKernelGetProcessTimeWide();
  frame->child_us = 0;
}

void gl_perf_callee_leave(unsigned callee) {
  if (g_callee_overflow) {
    g_callee_overflow--;
    return;
  }
  if (!g_callee_depth) {
    g_callee_mismatch++;
    return;
  }
  uint64_t now = sceKernelGetProcessTimeWide();
  GlCalleeFrame frame = g_callee_stack[--g_callee_depth];
  if (frame.callee != callee) g_callee_mismatch++;
  uint64_t elapsed = now - frame.start_us;
  unsigned id = frame.callee < GL_CALLEE_COUNT ? frame.callee : GL_CALLEE_OUTSIDE;
  g_callee_calls[id]++;
  g_callee_exclusive_us[id] += elapsed >= frame.child_us ? elapsed - frame.child_us : 0;
  if (g_callee_depth) g_callee_stack[g_callee_depth - 1].child_us += elapsed;
}

void gl_perf_gob_enter(const void *gob, const char *model_name) {
  if (g_gob_depth >= GL_GOB_STACK_MAX) {
    g_gob_overflow++;
    return;
  }
  GlGobIdentity *entry = NULL;
  unsigned slot = ((uintptr_t)gob >> 4) & (GL_GOB_IDENTITY_MAX - 1);
  for (unsigned probe = 0; probe < GL_GOB_IDENTITY_MAX; probe++) {
    GlGobIdentity *candidate = &g_gob_identity[slot];
    if (!candidate->gob || candidate->gob == gob) {
      if (!candidate->gob) candidate->gob = gob;
      /* Room telemetry may create this identity before Gob::Render reaches it.
       * Fill the independently resolved model name whenever it is still empty. */
      if (!candidate->model[0] && model_name) {
        strncpy(candidate->model, model_name, sizeof candidate->model - 1);
        candidate->model[sizeof candidate->model - 1] = '\0';
      }
      candidate->calls++;
      entry = candidate;
      break;
    }
    slot = (slot + 1) & (GL_GOB_IDENTITY_MAX - 1);
  }
  if (!entry) g_gob_table_overflow++;
  g_gob_stack[g_gob_depth++] = entry;
}

void gl_perf_gob_leave(const void *gob) {
  (void)gob;
  if (g_gob_overflow) {
    g_gob_overflow--;
    return;
  }
  if (g_gob_depth) g_gob_depth--;
}

void gl_perf_gob_note_room(const void *gob, const char *room_name) {
  if (!gob || !room_name) return;
  unsigned slot = ((uintptr_t)gob >> 4) & (GL_GOB_IDENTITY_MAX - 1);
  for (unsigned probe = 0; probe < GL_GOB_IDENTITY_MAX; probe++) {
    GlGobIdentity *entry = &g_gob_identity[slot];
    if (!entry->gob || entry->gob == gob) {
      if (!entry->gob) entry->gob = gob;
      if (!entry->room[0]) {
        strncpy(entry->room, room_name, sizeof entry->room - 1);
        entry->room[sizeof entry->room - 1] = '\0';
      } else if (strcmp(entry->room, room_name) != 0) {
        entry->multi_room = 1;
      }
      return;
    }
    slot = (slot + 1) & (GL_GOB_IDENTITY_MAX - 1);
  }
  g_gob_table_overflow++;
}

static inline unsigned gl_current_stage(void) {
  return g_stage_depth ? g_stage_stack[g_stage_depth - 1].stage : GL_STAGE_OUTSIDE;
}

#if PERFORMANCE_TELEMETRY_ENABLE
static void gl_state_note(uint64_t us) {
  unsigned stage = gl_current_stage();
  g_stage_state_us[stage] += us;
  g_stage_state_n[stage]++;
}
#endif

static inline unsigned gl_stage_note_draw(int elements) {
  unsigned stage = gl_current_stage();
  unsigned program = g_cur_prog <= GL_STAGE_PROGRAM_MAX ? g_cur_prog : GL_STAGE_PROGRAM_MAX;
  unsigned callee = g_callee_depth ? g_callee_stack[g_callee_depth - 1].callee : GL_CALLEE_OUTSIDE;
  if (elements) g_stage_elements[stage]++;
  else          g_stage_arrays[stage]++;
  g_stage_program[stage][program]++;
  g_callee_draws[callee]++;
  if (g_gob_depth && g_gob_stack[g_gob_depth - 1])
    g_gob_stack[g_gob_depth - 1]->draws++;
  return stage;
}
/* Attribute-offset high-water mark, for the geometry corruption. vitaGL used
 * shader attribute zero as a packed VBO's base even when another active
 * attribute started earlier in the vertex. KOTOR's skinned layout exposes it:
 * weights are shader attribute 0 at +0x20, while position is attribute 2 at
 * +0x00. Position-base underflow then lands in GXM's 16-bit attribute offset;
 * large absolute offsets also truncate in that field. The vitaGL patch uses the
 * lowest active memory offset as the stream base, leaving only 0x00..0x30 in
 * the attribute descriptors. Keep the high-water counters as a regression
 * witness for packed models above 64 KiB. */
static uintptr_t g_vap_max_off = 0;
static unsigned  g_vap_over64k = 0;
static unsigned g_bind_skipped_win = 0;   /* redundant binds we suppressed */
static unsigned g_nontex2d_binds = 0;     /* cube/3D binds -- the envmap path */

/* Live-texture census; the tallies live up here because the per-window stats
 * line below reports them. TEXKIND_MAX and the maintenance helpers are further
 * down with the rest of the texture-id bookkeeping. */
#define TEXKIND_MAX  4096
static uint32_t g_tex_bytes[TEXKIND_MAX];       /* per id, all mip levels */
static uint64_t g_tex_live = 0, g_tex_peak = 0; /* bytes currently uploaded */
static uint64_t g_tex_up = 0, g_tex_down = 0;   /* lifetime added / released */
static unsigned g_tex_n_live = 0, g_tex_untracked = 0;

/* 16-bit conversion state; up here because the stats line and the delete hook
 * both read it. The packer itself lives with the upload path further down. */
#define TEX16_NONE 0
#define TEX16_4444 1
#define TEX16_565  2
static uint8_t  g_tex16[TEXKIND_MAX];
static unsigned g_tex16_n = 0;
static uint64_t g_tex16_saved = 0;          /* bytes NOT spent, lifetime */

static GLuint g_cur_arraybuf = 0;        /* last glBindBuffer(GL_ARRAY_BUFFER) */
static GLuint g_cur_elementbuf = 0;      /* last GL_ELEMENT_ARRAY_BUFFER */
#define GL_ATTRIB_TRACK_MAX 16
static uint32_t g_attrib_enabled;
static struct {
  GLint size;
  GLenum type;
  GLboolean normalized;
  GLsizei stride;
  uintptr_t pointer;
  GLuint buffer;
} g_attrib_state[GL_ATTRIB_TRACK_MAX];

#define GL_DRAW_SIGNATURE_MAX 512
static uint64_t g_draw_signatures[GL_DRAW_SIGNATURE_MAX];

static inline uint64_t gl_draw_signature_mix(uint64_t hash, uint64_t value) {
  hash ^= value;
  return hash * 1099511628211ULL;
}

static void gl_draw_signature_note(unsigned stage, unsigned program, GLenum mode,
                                   GLsizei count, GLenum type, const void *indices) {
  if (g_cur_elementbuf) g_stage_index_ebo[stage]++;
  else                  g_stage_index_client[stage]++;

  uint64_t hash = 1469598103934665603ULL;
  hash = gl_draw_signature_mix(hash, stage);
  hash = gl_draw_signature_mix(hash, program);
  hash = gl_draw_signature_mix(hash, mode);
  hash = gl_draw_signature_mix(hash, (uint32_t)count);
  hash = gl_draw_signature_mix(hash, type);
  hash = gl_draw_signature_mix(hash, g_cur_elementbuf);
  hash = gl_draw_signature_mix(hash, (uintptr_t)indices);
  hash = gl_draw_signature_mix(hash, g_attrib_enabled);
  for (unsigned i = 0; i < GL_ATTRIB_TRACK_MAX; i++) {
    if (!(g_attrib_enabled & (1u << i))) continue;
    hash = gl_draw_signature_mix(hash, i);
    hash = gl_draw_signature_mix(hash, (uint32_t)g_attrib_state[i].size);
    hash = gl_draw_signature_mix(hash, g_attrib_state[i].type);
    hash = gl_draw_signature_mix(hash, g_attrib_state[i].normalized);
    hash = gl_draw_signature_mix(hash, (uint32_t)g_attrib_state[i].stride);
    hash = gl_draw_signature_mix(hash, g_attrib_state[i].pointer);
    hash = gl_draw_signature_mix(hash, g_attrib_state[i].buffer);
  }
  for (unsigned i = 0; i < GL_MAX_TEXUNITS; i++)
    hash = gl_draw_signature_mix(hash, g_bound_texture_signature[i]);
  if (!hash) hash = 1;

  unsigned slot = (unsigned)hash & (GL_DRAW_SIGNATURE_MAX - 1);
  for (unsigned probe = 0; probe < GL_DRAW_SIGNATURE_MAX; probe++) {
    if (!g_draw_signatures[slot]) {
      g_draw_signatures[slot] = hash;
      g_stage_program_unique[stage][program]++;
      return;
    }
    if (g_draw_signatures[slot] == hash) {
      g_stage_program_repeat[stage][program]++;
      return;
    }
    slot = (slot + 1) & (GL_DRAW_SIGNATURE_MAX - 1);
  }
  g_draw_signature_overflow++;
}

static void gl_draw_signature_reset_frame(void) {
  memset(g_draw_signatures, 0, sizeof g_draw_signatures);
}

static inline void draw_note_source(void) {
  if (g_cur_arraybuf) { g_draw_vbo_win++; g_draw_vbo_frame++; }
  else { g_draw_client_win++; g_draw_client_frame++; }
}

static void glDrawArrays_t(GLenum mode, GLint first, GLsizei count) {
  g_gl_total_draws++;
#if !PERFORMANCE_TELEMETRY_ENABLE
  if (loadscreen_active()) loadscreen_end();
#if DRAW_FRAME_BENCHMARK_ENABLE
  g_draw_frame_draws++;
#endif
  glDrawArrays(mode, first, count);
  return;
#else
  if (loadscreen_active()) loadscreen_end();   /* first real frame: hand over */
  if (g_draw_n < 20) log_printf("[GL] glDrawArrays(mode=0x%x, first=%d, count=%d) #%d",
                               (unsigned)mode, first, (int)count, g_draw_n);
  if (g_gl_text_draw && g_textdraw_n < TEXTDRAW_LOG_MAX)
    log_printf("[textdraw#%d] glDrawArrays(mode=0x%x, count=%d) tex=%u",
               g_textdraw_n++, (unsigned)mode, (int)count, g_cur_tex);
  tex_note_draw(g_cur_tex);
  unsigned stage = gl_stage_note_draw(0);
  unsigned program = g_cur_prog <= GL_STAGE_PROGRAM_MAX ? g_cur_prog : GL_STAGE_PROGRAM_MAX;
  g_draw_n++; g_arrays_win++; g_arrays_frame++; g_arrays_tot++; draw_note_source();
  uint64_t start = sceKernelGetProcessTimeWide();
  glDrawArrays(mode, first, count);
  uint64_t elapsed = sceKernelGetProcessTimeWide() - start;
  g_stage_draw_us[stage] += elapsed;
  g_stage_program_us[stage][program] += elapsed;
#endif
}
static void glDrawElements_t(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  g_gl_total_draws++;
#if !PERFORMANCE_TELEMETRY_ENABLE
  if (loadscreen_active()) loadscreen_end();
#if DRAW_FRAME_BENCHMARK_ENABLE
  g_draw_frame_draws++;
#endif
  glDrawElements(mode, count, type, idx);
  return;
#else
  if (loadscreen_active()) loadscreen_end();   /* first real frame: hand over */
  if (g_draw_n < 20) log_printf("[GL] glDrawElements(mode=0x%x, count=%d, type=0x%x) #%d",
                               (unsigned)mode, (int)count, (unsigned)type, g_draw_n);
  if (g_gl_text_draw && g_textdraw_n < TEXTDRAW_LOG_MAX)
    log_printf("[textdraw#%d] glDrawElements(mode=0x%x, count=%d) tex=%u",
               g_textdraw_n++, (unsigned)mode, (int)count, g_cur_tex);
  tex_note_draw(g_cur_tex);
  unsigned stage = gl_stage_note_draw(1);
  unsigned program = g_cur_prog <= GL_STAGE_PROGRAM_MAX ? g_cur_prog : GL_STAGE_PROGRAM_MAX;
  gl_draw_signature_note(stage, program, mode, count, type, idx);
  g_draw_n++; g_elements_win++; g_elements_frame++; g_elements_tot++; draw_note_source();
  uint64_t start = sceKernelGetProcessTimeWide();
  glDrawElements(mode, count, type, idx);
  uint64_t elapsed = sceKernelGetProcessTimeWide() - start;
  g_stage_draw_us[stage] += elapsed;
  g_stage_program_us[stage][program] += elapsed;
#endif
}
/* Stutter counter: slow frames per DRAW_FRAME window, plus the engine
 * resource-cache budget as libkotor2's CExoResMan sees it ([0] total phys,
 * [1] budget, [2] available). Read-only, a few loads per frame. */
static uint64_t g_slow_prev_swap_us, g_slow_sum_us, g_slow_max_us;
static unsigned g_slow_100, g_slow_200, g_slow_500, g_slow_1000;
static int **g_slow_resman;

static void slow_note_frame(uint64_t swap_end_us) {
  if (g_slow_prev_swap_us) {
    uint64_t d = swap_end_us - g_slow_prev_swap_us;
    g_slow_sum_us += d;
    if (d > g_slow_max_us) g_slow_max_us = d;
    if (d >= 100000) g_slow_100++;
    if (d >= 200000) g_slow_200++;
    if (d >= 500000) g_slow_500++;
    if (d >= 1000000) g_slow_1000++;
#if STALL_LOG_MS
    if (d >= STALL_LOG_MS * 1000u)
      log_printf("[stall] frame %u ms, end %llu ms", (unsigned)(d / 1000u),
                 (unsigned long long)(swap_end_us / 1000u));
#endif
  }
  g_slow_prev_swap_us = swap_end_us;
}

static void slow_report(uint64_t frames) {
  obb_cache_stats_t oc;
  obb_cache_stats(&oc);
  if (!g_slow_resman) g_slow_resman = (int **)so_symbol(&kotor_mod, "g_pExoResMan");
  unsigned avg10 = frames ? (unsigned)(g_slow_sum_us / frames / 100u) : 0;
  int total = 0, budget = 0, avail = 0;
  if (g_slow_resman && *g_slow_resman) {
    total = (*g_slow_resman)[0]; budget = (*g_slow_resman)[1]; avail = (*g_slow_resman)[2];
  }
  log_printf("[DRAW_FRAME] frame avg=%u.%u ms max=%u ms | slow frames >=100ms:%u >=200ms:%u "
             ">=500ms:%u >=1s:%u | resman total=%d KB budget=%d KB in-use=%d KB | "
             "obb cache hits=%u (%u KB) misses=%u items=%u (%u KB)",
             avg10 / 10, avg10 % 10, (unsigned)(g_slow_max_us / 1000u), g_slow_100,
             g_slow_200, g_slow_500, g_slow_1000, total / 1024, budget / 1024,
             (budget - avail) / 1024, (unsigned)oc.hits, (unsigned)(oc.hit_bytes / 1024u),
             (unsigned)oc.misses, (unsigned)oc.items, (unsigned)(oc.bytes / 1024u));
  g_slow_sum_us = g_slow_max_us = 0;
  g_slow_100 = g_slow_200 = g_slow_500 = g_slow_1000 = 0;
  {
    char gs[320];
    gl_state_filter_stats(gs, sizeof gs, (unsigned)frames);
    log_printf("[glsf] %s", gs);
  }
}

void gl_patch_on_swap(uint64_t swap_begin_us, uint64_t swap_end_us) {
#if !PERFORMANCE_TELEMETRY_ENABLE
  (void)swap_begin_us;
#if DRAW_FRAME_BENCHMARK_ENABLE
  slow_note_frame(swap_end_us);
  if (!g_draw_frame_start_us) {
    g_draw_frame_start_us = swap_end_us;
    g_draw_frame_draws = 0;
    g_draw_frame_frames = 0;
  } else {
    g_draw_frame_frames++;
    uint64_t elapsed = swap_end_us - g_draw_frame_start_us;
    if (elapsed >= DRAW_FRAME_BENCHMARK_INTERVAL_US) {
      uint64_t draws = g_draw_frame_draws;
      uint64_t frames = g_draw_frame_frames;
      uint64_t per_frame_x100 = frames ? draws * 100ULL / frames : 0;
      log_printf("[DRAW_FRAME] draws_per_frame=%llu.%02llu draws=%llu frames=%llu interval_us=%llu",
                 (unsigned long long)(per_frame_x100 / 100ULL),
                 (unsigned long long)(per_frame_x100 % 100ULL),
                 (unsigned long long)draws,
                 (unsigned long long)frames,
                 (unsigned long long)elapsed);
      slow_report(frames);
      dxt_native_report();
      g_draw_frame_draws = 0;
      g_draw_frame_frames = 0;
      g_draw_frame_start_us = swap_end_us;
    }
  }
#else
  (void)swap_end_us;
#endif
  return;
#else
  static unsigned frame = 0;
  static uint64_t prev_swap_end = 0, timing_sum = 0, timing_max = 0;
  static unsigned timing_n = 0, over50 = 0, over80 = 0;
  static uint64_t last_hitch_log = 0;
  static unsigned hitch_suppressed = 0, hitch_suppressed_max = 0;
  static io_perf_t prev_io;
  static audio_perf_t prev_audio;
  static engine_perf_t prev_engine;
  static sdl_perf_t prev_sdl;
  static unsigned policy_seen = 0;
  static unsigned skip_groups[6];
  io_perf_t io;
  audio_perf_t audio;
  engine_perf_t engine;
  sdl_perf_t sdl;
  io_perf_snapshot(&io);
  audio_perf_snapshot(&audio);
  engine_perf_snapshot(&engine, swap_begin_us);
  sdl_perf_snapshot(&sdl);
  frame++;

  if (engine.policy_seq != policy_seen) {
    unsigned bucket = engine.selected_skip == 0 ? 0 : engine.selected_skip == 1 ? 1 :
                      engine.selected_skip == 3 ? 2 : engine.selected_skip == 6 ? 3 :
                      engine.selected_skip == 10 ? 4 : 5;
    skip_groups[bucket]++;
    policy_seen = engine.policy_seq;
  }

  if (prev_swap_end) {
    uint64_t work_us = swap_begin_us - prev_swap_end;
    uint64_t swap_us = swap_end_us - swap_begin_us;
    uint64_t total_us = swap_end_us - prev_swap_end;
    timing_sum += total_us;
    if (total_us > timing_max) timing_max = total_us;
    timing_n++;
    if (total_us >= 50000u) over50++;
    if (total_us >= 80000u) over80++;

#if FRAME_HITCH_TRACE_MS > 0
    if (total_us >= (uint64_t)FRAME_HITCH_TRACE_MS * 1000u) {
      if (!last_hitch_log || swap_end_us - last_hitch_log >=
                              (uint64_t)FRAME_HITCH_LOG_GAP_MS * 1000u) {
        unsigned feed_n = audio.feed_count - prev_audio.feed_count;
        uint64_t feed_us = audio.feed_us - prev_audio.feed_us;
        log_printf("[hitch] f=%u total=%u.%u ms (work=%u.%u swap=%u.%u) "
                   "draw A/E=%u/%u client/vbo=%u/%u clear=%u "
                   "tex=%u(%u KB) binds=%u/%u prog=%u/%u buf=%u(%u KB) "
                   "link=%u(%u ms) | OBB card=%u/%u KB in %u ms cache=%u seek=%u "
                     "open=%d | audio feed=%u/%llu us lifetimeMax=%u underrun=%u "
                    "mix=%llu us glock=%u/%llu us "
                    "| engine game=%u/%u ms screen=%u/%u ms "
                    "policy ai=%.1f skip=%u next=%.1f fps=%.1f movie=%d "
                     "delay=%u/%u/%u ms lifetimeMax=%u ms "
                    "rateLimited=%u(max %u.%u ms)",
                   frame, (unsigned)(total_us / 1000u), (unsigned)(total_us % 1000u) / 100u,
                   (unsigned)(work_us / 1000u), (unsigned)(work_us % 1000u) / 100u,
                   (unsigned)(swap_us / 1000u), (unsigned)(swap_us % 1000u) / 100u,
                   g_arrays_frame, g_elements_frame, g_draw_client_frame, g_draw_vbo_frame,
                   g_clears_frame, g_texupload_frame,
                   (unsigned)(g_texupload_bytes_frame >> 10), g_texbind_frame,
                   g_bind_skipped_frame, g_prog_frame, g_prog_skipped_frame,
                   g_bufdata_frame, (unsigned)(g_bufdata_bytes_frame >> 10),
                   g_links_frame, (unsigned)(g_link_us_frame / 1000u),
                   io.reads - prev_io.reads,
                   (unsigned)((io.card_bytes - prev_io.card_bytes) >> 10),
                   (unsigned)((io.card_us - prev_io.card_us) / 1000u),
                   io.hits - prev_io.hits, io.seeks - prev_io.seeks, io_open_count(),
                    feed_n, (unsigned long long)feed_us, audio.feed_max_us,
                   audio.underruns - prev_audio.underruns,
                   (unsigned long long)(audio.mix_us - prev_audio.mix_us),
                   audio.glock_n - prev_audio.glock_n,
                   (unsigned long long)(audio.glock_us - prev_audio.glock_us),
                   engine.game_calls - prev_engine.game_calls,
                   (unsigned)((engine.game_us - prev_engine.game_us) / 1000u),
                    engine.screen_calls - prev_engine.screen_calls,
                    (unsigned)((engine.screen_us - prev_engine.screen_us) / 1000u),
                    (double)engine.selector_ai_ms, engine.selected_skip,
                    (double)engine.next_ai_ms, (double)engine.display_fps, engine.movie_fps,
                    sdl.delay_calls - prev_sdl.delay_calls,
                    (unsigned)((sdl.delay_requested_us - prev_sdl.delay_requested_us) / 1000u),
                    (unsigned)((sdl.delay_actual_us - prev_sdl.delay_actual_us) / 1000u),
                    sdl.delay_max_us / 1000u,
                    hitch_suppressed,
                   hitch_suppressed_max / 1000u, (hitch_suppressed_max % 1000u) / 100u);
        last_hitch_log = swap_end_us;
        hitch_suppressed = hitch_suppressed_max = 0;
      } else {
        hitch_suppressed++;
        if (total_us > hitch_suppressed_max) hitch_suppressed_max = (unsigned)total_us;
      }
    }
#endif
  }
  prev_swap_end = swap_end_us;
  prev_io = io;
  prev_audio = audio;
  prev_engine = engine;
  prev_sdl = sdl;
  engine_perf_presented();

  if (frame % 120 == 0) {  // ~ every couple seconds at 60fps
    log_printf("[GL] frame %u: this window drawArrays=%u drawElements=%u clears=%u"
               " | lifetime draws=%u | timing avg=%u.%u ms max=%u.%u ms >50=%u >80=%u",
               frame, g_arrays_win, g_elements_win, g_clears_win,
               g_arrays_tot + g_elements_tot,
               timing_n ? (unsigned)((timing_sum / timing_n) / 1000u) : 0,
               timing_n ? (unsigned)((timing_sum / timing_n) % 1000u) / 100u : 0,
               (unsigned)(timing_max / 1000u), (unsigned)(timing_max % 1000u) / 100u,
               over50, over80);
    char ab[256]; int o = 0;
    for (int k = 0; k < g_bk_n && o < (int)sizeof(ab) - 28; k++)
      o += snprintf(ab + o, sizeof(ab) - o, "%ux%u=%u ", g_bk_w[k], g_bk_h[k], g_bk_n_draws[k]);
    if (o) log_printf("[GL]   draws by texture size (lifetime): %s", ab);
    log_printf("[GL]   per-window: clientArrayDraws=%u vboDraws=%u  texBinds=%u "
               "(skipped %u = %u%%) progSwitches=%u (skipped %u = %u%%) "
               "bufferUploads=%u nonTex2DBinds=%u maxAttrOff=0x%x over64k=%u\n"
               "[GL]   textures live: %u KB in %u ids (peak %u KB); "
               "lifetime %u KB up / %u KB released, %u untracked ids; "
               "16-bit: %u ids, %u KB saved",
               g_draw_client_win, g_draw_vbo_win, g_texbind_win, g_bind_skipped_win,
               g_texbind_win ? (g_bind_skipped_win * 100 / g_texbind_win) : 0,
               g_prog_win, g_prog_skipped_win,
               g_prog_win ? (g_prog_skipped_win * 100 / g_prog_win) : 0,
               g_bufdata_win, g_nontex2d_binds,
               (unsigned)g_vap_max_off, g_vap_over64k,
               (unsigned)(g_tex_live >> 10), g_tex_n_live, (unsigned)(g_tex_peak >> 10),
               (unsigned)(g_tex_up >> 10), (unsigned)(g_tex_down >> 10), g_tex_untracked,
                g_tex16_n, (unsigned)(g_tex16_saved >> 10));
    log_printf("[GL]   policy groups skip 0/1/3/6/10/other=%u/%u/%u/%u/%u/%u "
               "latest ai=%.1f->%u next=%.1f displayFPS=%.1f movieFPS=%d; "
               "SDL_Delay lifetime=%u requested=%u ms actual=%u ms lifetimeMax=%u ms",
               skip_groups[0], skip_groups[1], skip_groups[2], skip_groups[3],
               skip_groups[4], skip_groups[5], (double)engine.selector_ai_ms,
               engine.selected_skip, (double)engine.next_ai_ms,
               (double)engine.display_fps, engine.movie_fps, sdl.delay_calls,
               (unsigned)(sdl.delay_requested_us / 1000u),
               (unsigned)(sdl.delay_actual_us / 1000u), sdl.delay_max_us / 1000u);
    char stage_buf[1536];
    stage_buf[0] = '\0';
    int so = 0;
    for (unsigned i = 0; i < GL_STAGE_COUNT && so < (int)sizeof(stage_buf) - 128; i++) {
      unsigned draws = g_stage_arrays[i] + g_stage_elements[i];
      if (!draws && !g_stage_calls[i] && !g_stage_exclusive_us[i] &&
          !g_stage_state_n[i]) continue;
      so += snprintf(stage_buf + so, sizeof(stage_buf) - so,
                     "%s=%u/%u/%u/ex=%llu.%u/dr=%llu.%u/st=%llu.%ums,n=%u ",
                     g_stage_name[i],
                     g_stage_calls[i], g_stage_arrays[i], g_stage_elements[i],
                     (unsigned long long)(g_stage_exclusive_us[i] / 1000u),
                     (unsigned)((g_stage_exclusive_us[i] % 1000u) / 100u),
                     (unsigned long long)(g_stage_draw_us[i] / 1000u),
                     (unsigned)((g_stage_draw_us[i] % 1000u) / 100u),
                     (unsigned long long)(g_stage_state_us[i] / 1000u),
                     (unsigned)((g_stage_state_us[i] % 1000u) / 100u),
                     g_stage_state_n[i]);
    }
    log_printf("[stage] calls/arrays/elements/exclusive/draw/state(ms,n) %sdepth=%u mismatch=%u overflow=%u",
               stage_buf, g_stage_depth, g_stage_mismatch, g_stage_overflow);

    char index_buf[512];
    index_buf[0] = '\0';
    int io = 0;
    for (unsigned i = 0; i < GL_STAGE_COUNT && io < (int)sizeof(index_buf) - 40; i++) {
      if (!g_stage_index_client[i] && !g_stage_index_ebo[i]) continue;
      io += snprintf(index_buf + io, sizeof(index_buf) - io, "%s=%u/%u ",
                     g_stage_name[i], g_stage_index_client[i], g_stage_index_ebo[i]);
    }
    log_printf("[indexsrc] client/ebo %ssigOverflow=%u", index_buf,
               g_draw_signature_overflow);

    char callee_buf[1024];
    callee_buf[0] = '\0';
    int co = 0;
    for (unsigned i = 0; i < GL_CALLEE_COUNT && co < (int)sizeof(callee_buf) - 72; i++) {
      if (!g_callee_calls[i] && !g_callee_draws[i] && !g_callee_exclusive_us[i]) continue;
      co += snprintf(callee_buf + co, sizeof(callee_buf) - co,
                     "%s=%u/%u/%llu.%ums ", g_callee_name[i],
                     g_callee_calls[i], g_callee_draws[i],
                     (unsigned long long)(g_callee_exclusive_us[i] / 1000u),
                     (unsigned)((g_callee_exclusive_us[i] % 1000u) / 100u));
    }
    log_printf("[callee] calls/draws/exclusive %sdepth=%u mismatch=%u overflow=%u",
               callee_buf, g_callee_depth, g_callee_mismatch, g_callee_overflow);

    struct {
      const void *gob;
      const char *model, *room;
      unsigned calls, draws, multi_room;
    } gob_top[12] = {{0}};
    unsigned gob_used = 0;
    for (unsigned i = 0; i < GL_GOB_IDENTITY_MAX; i++) {
      GlGobIdentity *entry = &g_gob_identity[i];
      if (!entry->gob) continue;
      gob_used++;
      if (!entry->draws || entry->draws <= gob_top[11].draws) continue;
      unsigned rank = 11;
      while (rank && entry->draws > gob_top[rank - 1].draws) {
        gob_top[rank] = gob_top[rank - 1];
        rank--;
      }
      gob_top[rank].gob = entry->gob;
      gob_top[rank].model = entry->model;
      gob_top[rank].room = entry->room;
      gob_top[rank].calls = entry->calls;
      gob_top[rank].draws = entry->draws;
      gob_top[rank].multi_room = entry->multi_room;
    }
    char gob_buf[1024];
    gob_buf[0] = '\0';
    int go = 0;
    for (unsigned i = 0; i < 12 && gob_top[i].gob &&
         go < (int)sizeof(gob_buf) - 96; i++)
      go += snprintf(gob_buf + go, sizeof(gob_buf) - go,
                     "%p[%.23s@%.23s%s]=%u/%u ", gob_top[i].gob,
                     gob_top[i].model && *gob_top[i].model ? gob_top[i].model : "?",
                     gob_top[i].room && *gob_top[i].room ? gob_top[i].room : "?",
                     gob_top[i].multi_room ? "+" : "",
                     gob_top[i].calls, gob_top[i].draws);
    log_printf("[gobid] ptr[model@room]=calls/draws %sused=%u tableOverflow=%u stackDepth=%u stackOverflow=%u",
               gob_buf, gob_used, g_gob_table_overflow, g_gob_depth,
               g_gob_overflow);

    struct { unsigned stage, program, draws; } top[12] = {{0}};
    for (unsigned s = 0; s < GL_STAGE_COUNT; s++) {
      for (unsigned p = 0; p <= GL_STAGE_PROGRAM_MAX; p++) {
        unsigned draws = g_stage_program[s][p];
        if (!draws || draws <= top[11].draws) continue;
        unsigned pos = 11;
        while (pos && draws > top[pos - 1].draws) {
          top[pos] = top[pos - 1];
          pos--;
        }
        top[pos].stage = s;
        top[pos].program = p;
        top[pos].draws = draws;
      }
    }
    char prog_buf[768];
    prog_buf[0] = '\0';
    int po = 0;
    for (unsigned i = 0; i < 12 && top[i].draws && po < (int)sizeof(prog_buf) - 48; i++) {
      po += snprintf(prog_buf + po, sizeof(prog_buf) - po,
                     "%s:p%s%u=%u/%llu.%ums/u%u/r%u ",
                     g_stage_name[top[i].stage],
                     top[i].program == GL_STAGE_PROGRAM_MAX ? "+" : "",
                     top[i].program, top[i].draws,
                     (unsigned long long)(g_stage_program_us[top[i].stage][top[i].program] / 1000u),
                     (unsigned)((g_stage_program_us[top[i].stage][top[i].program] % 1000u) / 100u),
                     g_stage_program_unique[top[i].stage][top[i].program],
                     g_stage_program_repeat[top[i].stage][top[i].program]);
    }
    log_printf("[stageprog] top draws %s", prog_buf);
    memset(skip_groups, 0, sizeof skip_groups);
    memset(g_stage_calls, 0, sizeof g_stage_calls);
    memset(g_stage_arrays, 0, sizeof g_stage_arrays);
    memset(g_stage_elements, 0, sizeof g_stage_elements);
    memset(g_stage_exclusive_us, 0, sizeof g_stage_exclusive_us);
    memset(g_stage_program, 0, sizeof g_stage_program);
    memset(g_stage_draw_us, 0, sizeof g_stage_draw_us);
    memset(g_stage_program_us, 0, sizeof g_stage_program_us);
    memset(g_stage_index_client, 0, sizeof g_stage_index_client);
    memset(g_stage_index_ebo, 0, sizeof g_stage_index_ebo);
    memset(g_stage_program_unique, 0, sizeof g_stage_program_unique);
    memset(g_stage_program_repeat, 0, sizeof g_stage_program_repeat);
    memset(g_stage_state_us, 0, sizeof g_stage_state_us);
    memset(g_stage_state_n, 0, sizeof g_stage_state_n);
    memset(g_callee_calls, 0, sizeof g_callee_calls);
    memset(g_callee_draws, 0, sizeof g_callee_draws);
    memset(g_callee_exclusive_us, 0, sizeof g_callee_exclusive_us);
    memset(g_gob_identity, 0, sizeof g_gob_identity);
    g_gob_table_overflow = 0;
    g_callee_mismatch = 0;
    g_draw_signature_overflow = 0;
    g_stage_mismatch = 0;
    g_bind_skipped_win = g_prog_skipped_win = 0;
    g_arrays_win = g_elements_win = g_clears_win = 0;
    g_draw_client_win = g_draw_vbo_win = 0;
    g_texbind_win = g_prog_win = g_bufdata_win = 0;
    timing_sum = timing_max = 0;
    timing_n = over50 = over80 = 0;
  }
  g_arrays_frame = g_elements_frame = g_clears_frame = 0;
  g_draw_client_frame = g_draw_vbo_frame = 0;
  g_prog_frame = g_prog_skipped_frame = 0;
  g_texbind_frame = g_bind_skipped_frame = 0;
  g_bufdata_frame = g_texupload_frame = g_links_frame = 0;
  g_bufdata_bytes_frame = g_texupload_bytes_frame = g_link_us_frame = 0;
  gl_draw_signature_reset_frame();
#endif
}
/* Capability-query getters. The companion's OpenGLES20Implementation::init()
 * fires ~18 of these before any other GL call, and feeds one straight into a
 * malloc size. If vitaGL doesn't handle an enum it leaves *params UNWRITTEN
 * (uninitialised) -> garbage count -> huge malloc / bad loop. The sentinel
 * pre-fill detects exactly that, and prints the value so we see the last query
 * before a hang. */
// log97: in-game, these getters alone emitted 6864 of the log's 19086 lines --
// the game polls GL_TEXTURE_BINDING_2D (0x8069) every few draws. Each line is a
// separate open/write/close on the memory card, so this was a large slice of the
// low frame rate. The *diagnostic* value is all in init (~18 cap queries) and the
// UNWRITTEN case; steady-state values are noise. Budget the normal case, and keep
// UNWRITTEN unconditional since it always signals a real vitaGL gap.
/* GLGET_TRACE_LIMIT lives in config.h so it can be re-armed without editing
 * this file. 0 disables the budgeted case; UNWRITTEN stays unconditional. */
static int g_glget_n = 0;
#define GLGETLOG(...) do { \
    if (g_glget_n < GLGET_TRACE_LIMIT) { \
      log_printf(__VA_ARGS__); \
      if (++g_glget_n == GLGET_TRACE_LIMIT) \
        log_printf("[GL] getter trace silenced after %d calls (steady state)", \
                   GLGET_TRACE_LIMIT); \
    } \
  } while (0)

static void glGetIntegerv_t(GLenum pname, GLint *params) {
  const GLint SENT = 0x0BADF00D;
  if (params) *params = SENT;
  glGetIntegerv(pname, params);
  if (params && *params == SENT)
    log_printf("[GL] glGetIntegerv(0x%x) -> UNWRITTEN (vitaGL ignored enum)", (unsigned)pname);
  else
    GLGETLOG("[GL] glGetIntegerv(0x%x) -> %d", (unsigned)pname, params ? *params : 0);
  // 0xd57 is init()'s LAST cap query; the hang is just past here. Arm heap
  // tracing now so OpenGLESState::init's allocations become visible.
  if (MEM_TRACE_ENABLE && pname == 0xd57 && !g_mem_trace) {
    log_printf("[GL] --- arming heap trace (entering GL engine setup) ---");
    g_mem_trace = 1;
  }
}
static void glGetBooleanv_t(GLenum pname, GLboolean *params) {
  const GLboolean SENT = 0x5A;
  if (params) *params = SENT;
  glGetBooleanv(pname, params);
  if (params && *params == SENT)
    log_printf("[GL] glGetBooleanv(0x%x) -> UNWRITTEN", (unsigned)pname);
  else
    GLGETLOG("[GL] glGetBooleanv(0x%x) -> %d", (unsigned)pname, params ? *params : 0);
}
static void glGetFloatv_t(GLenum pname, GLfloat *params) {
  union { uint32_t u; float f; } sent = { .u = 0x0BADF00D };
  if (params) *params = sent.f;
  glGetFloatv(pname, params);
  union { uint32_t u; float f; } got = { .f = params ? *params : 0.0f };
  if (params && got.u == sent.u)
    log_printf("[GL] glGetFloatv(0x%x) -> UNWRITTEN", (unsigned)pname);
  else
    GLGETLOG("[GL] glGetFloatv(0x%x) -> bits 0x%x", (unsigned)pname, (unsigned)got.u);
}

/* -- entry tracing for post-init setup calls --------------------------------
 * init() completes; the hang is the first GXM-touching GL call after it, which
 * is otherwise unlogged. These wrappers log on ENTRY (before calling through),
 * with a global sequence number, so the LAST line in the log is the call that
 * blocked -- even if it never returns. Restricted to the state/object-setup
 * surface a GL engine touches right after capability query. */
// log.c flushes every line to the SD card with its own open/write/close (~15ms).
// Tracing every GL call (~50/frame) therefore throttled the whole game to ~1 fps
// and starved its frame timers. Trace only the first GL_TRACE_LIMIT calls (see
// declaration above u2f) -- that covers init and the first frames, where every
// historical hang lived -- then auto-silence so steady state runs at native
// speed. Frame summaries, shader, link, and crash logging are unaffected.
#define GLLOG(fmt, ...) do { \
    int _s = g_gl_seq++; \
    if (_s < GL_TRACE_LIMIT) log_printf("[GL#%d] " fmt, _s, ##__VA_ARGS__); \
    else if (_s == GL_TRACE_LIMIT) \
      log_printf("[GL] per-call GL trace silenced after %d calls (steady state; " \
                 "frame/shader/link/crash logs continue)", GL_TRACE_LIMIT); \
    loadscreen_note_gl(); \
  } while (0)
static void glEnable_e(GLenum cap) { GLLOG("glEnable(0x%x)", (unsigned)cap); GL_TIME_STATE(glEnable(cap)); }
static void glDisable_e(GLenum cap) { GLLOG("glDisable(0x%x)", (unsigned)cap); GL_TIME_STATE(glDisable(cap)); }
/* Redundant-bind filter.
 *
 * log120: texBinds=1562368 against vboDraws=1549697 -- the game rebinds a texture
 * before essentially EVERY draw, and vitaGL's glBindTexture does real state work
 * each time. Skipping binds that do not change anything is free and safe, as long
 * as the shadow state is exact:
 *   - tracked PER TEXTURE UNIT (glBindTexture affects the active unit only)
 *   - GL_TEXTURE_2D only; every other target passes straight through
 *   - the whole shadow is dropped on glDeleteTextures, because ids get recycled
 *     and a stale entry would silently draw with the wrong texture
 *   - and binding ANY other target to a unit clears that unit's entry. Desktop
 *     GL keeps one binding per target per unit, so a cube bind would leave the
 *     2D binding intact and skipping the next 2D bind would be correct -- but
 *     GXM has a single texture per sampler slot and vitaGL is not obliged to
 *     model GL's per-target state. The shadow must not assume it does: kotor.vert
 *     samples u_texture2Sampler as a real GL_SAMPLER_CUBE for the environment
 *     map, so exactly the shiny-armour materials bind a cube to a unit that
 *     otherwise carries a 2D texture. Forgetting the entry costs one redundant
 *     bind on the rare cube path and cannot draw the wrong texture.
 * Set GL_FILTER_REDUNDANT_BINDS to 0 in config.h to rule this out. */
static void glActiveTexture_e(GLenum t) {
  GLLOG("glActiveTexture(0x%x)", (unsigned)t);
  unsigned u = (unsigned)t - 0x84C0u;               /* GL_TEXTURE0 */
  g_active_unit = (u < GL_MAX_TEXUNITS) ? u : 0;
  GL_TIME_STATE(glActiveTexture(t));
}

/* Cube-texture lifetime, for the armour.
 *
 * log151 settled that the envmap itself is fine: all six faces arrive with full
 * mip chains at t=25s (faces seen 0x3f) and the cube is bound ~19k times across
 * the session. Yet the armour is correct at boot, correct in the Upper City, and
 * wrong again after returning to the Lower City -- so what breaks is tied to an
 * AREA TRANSITION, and the cube is only ever uploaded that one time.
 *
 * The obvious way that goes wrong: an area unload deletes textures, the cube's
 * id is freed, and a later glGenTextures hands the SAME id back for an ordinary
 * 2D texture. The game keeps sampling u_texture2Sampler through that id and gets
 * whatever 2D image now owns it -- which is exactly "smeared with a reflection
 * of the room". Nothing in GL complains, because the id is perfectly valid.
 *
 * Remember which ids were uploaded as cubes and say so, loudly, when one is
 * deleted or turns up bound as GL_TEXTURE_2D. Either line names the bug. */
/* Track what each texture id CURRENTLY is, not what it once was.
 *
 * The first version of this kept a list of ids that had ever been uploaded as a
 * cube and never forgot them, so once id 32 was recycled every later mention of
 * 32 fired again -- log152 has 50 such lines and only the first pair means
 * anything. Recycling an id is legal GL and by itself proves nothing.
 *
 * What would be a real fault is a MISMATCH: the game binding GL_TEXTURE_CUBE_MAP
 * to an id whose most recent upload was 2D. That is the one that puts a flat
 * image behind u_texture2Sampler and smears a character in reflections. So keep
 * one byte per id, set it on upload, clear it on delete, and only shout when a
 * bind disagrees with it. */
#define TEXKIND_MAX  4096
#define TEXKIND_NONE 0
#define TEXKIND_2D   1
#define TEXKIND_CUBE 2
static uint8_t  g_tex_kind[TEXKIND_MAX];

/* ---- live texture census ---------------------------------------------------
 * vitaGL's free pools fall from 79 MB vram / 87 MB ram at boot to single-digit
 * vram and 57 MB ram after 49 minutes (log155), and the geometry corruption
 * shows up late in exactly those long sessions. But vram is NOT a one-way
 * drain -- it oscillates (1.3 MB free at t=185, 13.7 at t=547, 0.6 at t=2175,
 * 7.0 at t=2717), so memory is genuinely being recycled and the pool is simply
 * running at its ceiling. The ram pool is the one with a real trend, about
 * 30 MB gone over the session.
 *
 * "The pool is full" and "we are leaking" look identical from a free-bytes
 * counter, and they need opposite fixes. So count what is actually LIVE: bytes
 * of texture we have uploaded and not yet seen deleted. If live bytes track the
 * pool drain, the game is holding more and more texture and the fix is upstream
 * eviction. If live bytes sit flat while the pools keep falling, vitaGL is not
 * reclaiming what we delete and the fix is in the allocator.
 *
 * Keyed by texture id, which is exactly what glDeleteTextures gives back. A
 * level-0 upload replaces the id's contents, so it resets the tally first and
 * the mip levels that follow add onto it. Padded widths are charged at what is
 * really allocated, not what the game asked for. */

static unsigned fmt_bpp(GLenum f) { return (f == GL_RGBA) ? 4u : (f == GL_RGB) ? 3u : 2u; }

static void tex_charge(GLuint t, GLenum target, GLint level, GLsizei w, GLsizei h, unsigned bpp) {
  if (!t || t >= TEXKIND_MAX) { g_tex_untracked++; return; }
  /* A cube uploads six faces at the same id and level; only a 2D base level
   * means "this id now holds a different image". */
  if (target != GL_TEXTURE_2D) level = 1;
  uint32_t add = (uint32_t)((w > 0 ? w : 0)) * (uint32_t)((h > 0 ? h : 0)) * bpp;
  if (level == 0) {                       /* new base image: drop the old tally */
    if (g_tex_bytes[t]) { g_tex_live -= g_tex_bytes[t]; g_tex_down += g_tex_bytes[t]; }
    else                { g_tex_n_live++; }
    g_tex_bytes[t] = 0;
  }
  g_tex_bytes[t] += add;
  g_tex_live     += add;
  g_tex_up       += add;
  if (g_tex_live > g_tex_peak) g_tex_peak = g_tex_live;
}

static void tex_release(GLuint t) {
  if (!t || t >= TEXKIND_MAX || !g_tex_bytes[t]) return;
  g_tex_live -= g_tex_bytes[t];
  g_tex_down += g_tex_bytes[t];
  g_tex_bytes[t] = 0;
  if (g_tex_n_live) g_tex_n_live--;
}
static GLuint   g_cur_cubetex = 0;

static void tex_kind_set(GLuint t, uint8_t k) {
  if (t && t < TEXKIND_MAX) {
    if (g_tex_kind[t] && g_tex_kind[t] != k)
      log_printf("[GL] texture %u changes kind %s -> %s", (unsigned)t,
                 g_tex_kind[t] == TEXKIND_CUBE ? "CUBE" : "2D",
                 k == TEXKIND_CUBE ? "CUBE" : "2D");
    g_tex_kind[t] = k;
  }
}
static uint8_t tex_kind_get(GLuint t) {
  return (t && t < TEXKIND_MAX) ? g_tex_kind[t] : TEXKIND_NONE;
}

static void glDeleteTextures_e(GLsizei n, const GLuint *tex) {
  /* ids are recycled, so any cached binding may now mean a different texture */
  if (tex)
    for (GLsizei i = 0; i < n; i++) {
      if (tex_kind_get(tex[i]) == TEXKIND_CUBE)
        log_printf("[GL] cube texture %u deleted", (unsigned)tex[i]);
      if (tex[i] && tex[i] < TEXKIND_MAX) g_tex_kind[tex[i]] = TEXKIND_NONE;
      tex_release(tex[i]);
      for (unsigned u = 0; u < GL_MAX_TEXUNITS; u++)
        if ((GLuint)g_bound_texture_signature[u] == tex[i])
          g_bound_texture_signature[u] = 0;
      if (tex[i] < TEXKIND_MAX && g_tex16[tex[i]] != TEX16_NONE) {
        g_tex16[tex[i]] = TEX16_NONE;      /* ids recycle; do not inherit a format */
        if (g_tex16_n) g_tex16_n--;
      }
    }
  memset(g_bound2d, 0, sizeof g_bound2d);
  glDeleteTextures(n, tex);
}
static void glPixelStorei_e(GLenum p, GLint v) { GLLOG("glPixelStorei(0x%x,%d)", (unsigned)p, v); glPixelStorei(p, v); }
static void glGenTextures_e(GLsizei n, GLuint *t) { GLLOG("glGenTextures(%d)", (int)n); glGenTextures(n, t); }
static void glBindTexture_e(GLenum tg, GLuint t) {
  GLLOG("glBindTexture(0x%x,%u)", (unsigned)tg, (unsigned)t);
  g_texbind_win++; g_texbind_frame++;
  g_bound_texture_signature[g_active_unit] = ((uint64_t)tg << 32) | t;
  if (tg == GL_TEXTURE_CUBE_MAP) {
    g_cur_cubetex = t;
    if (tex_kind_get(t) == TEXKIND_2D) {          /* THE fault, if it happens */
      static unsigned w1 = 0;
      if (w1 < 16)
        log_printf("[GL] *** MISMATCH: binding CUBE_MAP to id %u whose last upload "
                   "was 2D -- the envmap is sampling a flat texture", (unsigned)t);
      w1++;
    }
  } else if (tg == GL_TEXTURE_2D && tex_kind_get(t) == TEXKIND_CUBE) {
    static unsigned w2 = 0;
    if (w2 < 16)
      log_printf("[GL] *** MISMATCH: binding 2D to id %u whose last upload was a "
                 "CUBE", (unsigned)t);
    w2++;
  }
#if GL_FILTER_REDUNDANT_BINDS
  if (tg == GL_TEXTURE_2D) {
    if (g_bound2d[g_active_unit] == t) {
      g_bind_skipped_win++; g_bind_skipped_frame++; g_cur_tex = t; return;
    }
    g_bound2d[g_active_unit] = t;
  } else {
    g_bound2d[g_active_unit] = 0;        /* unit may no longer hold that 2D texture */
    g_nontex2d_binds++;
  }
#endif
  if (tg == GL_TEXTURE_2D) g_cur_tex = t;  // mirrored for the text-draw trace
  GL_TIME_STATE(glBindTexture(tg, t));
}
// --- skinning diagnostics --------------------------------------------------
// Character models render as exploding spikes even though their .mdl/.mdx bytes
// are byte-identical to an offline LZMA decode -- so the DATA is right and the
// way it reaches the GPU is not. kotor.vert's skinned path is:
//     uniform vec4 u_boneMatrices[51];        // 17 bones x 3 vec4 rows
//     attribute vec4 a_matrixWeights, a_matrixIndices;
//     ivec4 indices = ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0));
//     pos = weights.x * vec3(dot(u_boneMatrices[indices.x], a_position), ...);
// i.e. DYNAMIC indexing of a uniform array from a vertex attribute. Two things
// can break it independently, and spikes look the same either way:
//   - a_matrixIndices arriving in the wrong format (e.g. UNSIGNED_BYTE flagged
//     normalized -> 0..255 collapses to 0..1 -> 3.0*x picks bone 0-3 for every
//     vertex), so the wrong bone transforms the vertex;
//   - the 51-element uniform array not being uploaded/addressed contiguously by
//     vitaGL, so indices.x+1 / +2 read someone else's rows.
// These get their OWN log budget: the general GLLOG trace is spent during init
// (4000 calls) long before a character is ever drawn, which is why the model
// path has been invisible. Log each DISTINCT attribute configuration once, and
// the first few large glUniform4fv uploads (the bone array).
#define VAP_SLOTS 24
static struct { GLuint idx; GLint size; GLenum type; GLboolean norm; GLsizei stride; uintptr_t off; } g_vap[VAP_SLOTS];
static unsigned g_vap_used = 0;

static void glVertexAttribPointer_e(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                    GLsizei stride, const void *pointer) {
#if PERFORMANCE_TELEMETRY_ENABLE
  /* Diagnostics only: the attribute shadow feeds draw signatures, and the
   * distinct-layout log is a 24-entry linear scan on every call (several calls
   * per draw). The skinning investigation it served is closed. */
  if (index < GL_ATTRIB_TRACK_MAX) {
    g_attrib_state[index].size = size;
    g_attrib_state[index].type = type;
    g_attrib_state[index].normalized = normalized;
    g_attrib_state[index].stride = stride;
    g_attrib_state[index].pointer = (uintptr_t)pointer;
    g_attrib_state[index].buffer = g_cur_arraybuf;
  }
  unsigned i;
  for (i = 0; i < g_vap_used; i++)
    if (g_vap[i].idx == index && g_vap[i].size == size && g_vap[i].type == type &&
        g_vap[i].norm == normalized && g_vap[i].stride == stride &&
        g_vap[i].off == (uintptr_t)pointer)
      break;
  if (i == g_vap_used && g_vap_used < VAP_SLOTS) {
    g_vap[g_vap_used].idx = index; g_vap[g_vap_used].size = size;
    g_vap[g_vap_used].type = type; g_vap[g_vap_used].norm = normalized;
    g_vap[g_vap_used].stride = stride; g_vap[g_vap_used].off = (uintptr_t)pointer;
    g_vap_used++;
    log_printf("[vtx] attrib idx=%u size=%d type=0x%x norm=%d stride=%d off=0x%x",
               (unsigned)index, (int)size, (unsigned)type, (int)normalized,
               (int)stride, (unsigned)(uintptr_t)pointer);
  }
  /* Only meaningful when a VBO is bound: with no buffer, `pointer` is a real
   * client address and comparing it to 0xFFFF is nonsense. log151 reported
   * maxAttrOff=0x985daa2c -- an address inside libKOTOR -- and counted 2.35M
   * "over 64 KB", which measured nothing at all. */
  if (g_cur_arraybuf) {
    if ((uintptr_t)pointer > g_vap_max_off) g_vap_max_off = (uintptr_t)pointer;
    if ((uintptr_t)pointer > 0xFFFFu) g_vap_over64k++;
  }
#endif
  GL_TIME_STATE(glVertexAttribPointer(index, size, type, normalized, stride, pointer));
}

static void glEnableVertexAttribArray_e(GLuint index) {
  if (index < GL_ATTRIB_TRACK_MAX) g_attrib_enabled |= 1u << index;
  GL_TIME_STATE(glEnableVertexAttribArray(index));
}

static void glDisableVertexAttribArray_e(GLuint index) {
  if (index < GL_ATTRIB_TRACK_MAX) g_attrib_enabled &= ~(1u << index);
  GL_TIME_STATE(glDisableVertexAttribArray(index));
}

static unsigned g_u4fv_n = 0;
/* KOTOR II's generic parameter uploader always calls glUniform4fv(..., 3, ...),
 * but kotorpost1.frag declares u_fragmentShaderParams[2].  vitaGL's uniform
 * path trusts count and copies all three vec4s into a two-vec4 allocation.  On
 * a warm custom-shader-cache launch, the extra 16 bytes overwrite the adjacent
 * u_matrices uniform entry and its SceGxmProgramParameter pointer becomes NULL.
 *
 * Record the linked array capacity with the location.  Besides producing a
 * precise runtime diagnostic, this lets the import boundary contain the bad
 * Android-driver call before it can corrupt vitaGL's heap.  Program is part of
 * the key because GL locations are only unique within a linked program. */
static struct { GLint loc; GLuint prog; GLsizei capacity; } g_frag_params[24];
static unsigned g_frag_params_n = 0, g_frag_params_logs = 0;
static unsigned g_frag_clamp_logs = 0;
static void glUniform4fv_e(GLint location, GLsizei count, const GLfloat *v) {
  // The bone array is the only uniform uploaded in bulk; log its first rows so a
  // garbage/identity matrix set is obvious.
  if (count > 8 && g_u4fv_n < 12) {
    log_printf("[vtx] glUniform4fv(loc=%d, count=%d) rows0-2: "
               "[%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
               (int)location, (int)count,
               v ? v[0] : 0.f, v ? v[1] : 0.f, v ? v[2] : 0.f, v ? v[3] : 0.f,
               v ? v[4] : 0.f, v ? v[5] : 0.f, v ? v[6] : 0.f, v ? v[7] : 0.f,
               v ? v[8] : 0.f, v ? v[9] : 0.f, v ? v[10] : 0.f, v ? v[11] : 0.f);
    g_u4fv_n++;
  }
  GLsizei safe_count = count;
  if (v) {
    for (unsigned i = 0; i < g_frag_params_n; i++) {
      if (g_frag_params[i].prog != g_cur_prog || g_frag_params[i].loc != location)
        continue;
      if (g_frag_params_logs < 24) {
        log_printf("[GLPOST] prog=%u count=%d capacity=%d params: "
                   "[%.3f %.3f %.3f %.3f] "
                   "[%.3f %.3f %.3f %.3f]", (unsigned)g_frag_params[i].prog,
                   (int)count, (int)g_frag_params[i].capacity,
                   v[0], v[1], v[2], v[3],
                   count > 1 ? v[4] : 0.f, count > 1 ? v[5] : 0.f,
                   count > 1 ? v[6] : 0.f, count > 1 ? v[7] : 0.f);
        g_frag_params_logs++;
      }
      if (g_frag_params[i].capacity > 0 && safe_count > g_frag_params[i].capacity) {
        safe_count = g_frag_params[i].capacity;
        if (g_frag_clamp_logs < 24) {
          log_printf("[GLPOST] CLAMP prog=%u loc=%d requested=%d linked=%d "
                     "(prevented %u-byte uniform overwrite)",
                     (unsigned)g_cur_prog, (int)location, (int)count,
                     (int)safe_count, (unsigned)(count - safe_count) * 4u * sizeof(GLfloat));
          g_frag_clamp_logs++;
        }
      }
      break;
    }
  }
  GL_TIME_STATE(glUniform4fv(location, safe_count, v));
}

// Name->location for the skinning inputs, so the attrib indices above can be tied
// to a_matrixIndices/a_matrixWeights and the uniform loc to u_boneMatrices.
static GLint glGetAttribLocation_e(GLuint prog, const GLchar *name) {
  GLint l = glGetAttribLocation(prog, name);
  if (name && (strstr(name, "matrix") || strstr(name, "position") || strstr(name, "normal")))
    log_printf("[vtx] attribLoc prog=%u \"%s\" -> %d", (unsigned)prog, name, (int)l);
  return l;
}
static GLint glGetUniformLocation_e(GLuint prog, const GLchar *name) {
  GLint l = glGetUniformLocation(prog, name);
  if (l >= 0 && name && !strcmp(name, "u_fragmentShaderParams") &&
      g_frag_params_n < sizeof(g_frag_params) / sizeof(g_frag_params[0])) {
    GLsizei capacity = 0;
    GLint active = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &active);
    for (GLint i = 0; i < active; i++) {
      char uniform_name[96];
      GLsizei uniform_name_len = 0;
      GLint uniform_size = 0;
      GLenum uniform_type = 0;
      uniform_name[0] = '\0';
      glGetActiveUniform(prog, (GLuint)i, sizeof(uniform_name), &uniform_name_len,
                         &uniform_size, &uniform_type, uniform_name);
      if (!strcmp(uniform_name, "u_fragmentShaderParams") ||
          !strcmp(uniform_name, "u_fragmentShaderParams[0]")) {
        capacity = uniform_size;
        break;
      }
    }
    g_frag_params[g_frag_params_n].loc = l;
    g_frag_params[g_frag_params_n].prog = prog;
    g_frag_params[g_frag_params_n].capacity = capacity;
    g_frag_params_n++;
    log_printf("[GLPOST] linked capacity prog=%u loc=%d vec4s=%d",
               (unsigned)prog, (int)l, (int)capacity);
  }
  if (name && strstr(name, "bone"))
    log_printf("[vtx] uniformLoc prog=%u \"%s\" -> %d", (unsigned)prog, name, (int)l);
  return l;
}

static GLsizei gl_rt_align_w(GLsizei w) { return (w > 0 && (w & 7)) ? ((w + 7) & ~7) : w; }

/* ---- 16-bit texture conversion --------------------------------------------
 * See GL_TEX16_CONVERT in config.h. Halves the texture working set so it fits
 * the Vita's fixed 96 MB of CDRAM instead of thrashing against it.
 *
 * Ordered 4x4 Bayer dithering is not decoration: at four bits a channel a plain
 * truncation posterises every gradient into visible steps, and a sky or a
 * saber glow is nothing but gradient. Dithering trades those steps for noise
 * the eye ignores at this screen size.
 *
 * Which ids were converted is remembered, because glTexSubImage2D goes straight
 * through to vitaGL: a byte-typed sub-upload into a 4444 texture would be read
 * as though it were already 4444, and corrupt it. */

static const uint8_t k_bayer4[16] = { 0, 8, 2,10, 12, 4,14, 6,  3,11, 1, 9, 15, 7,13, 5 };

/* Writes w*h uint16 into dst. src is tightly packed 8-bit RGB(A). */
static void tex16_pack(uint16_t *dst, const unsigned char *src, int w, int h, int kind) {
  for (int y = 0; y < h; y++) {
    const unsigned char *sp = src + (size_t)y * w * (kind == TEX16_4444 ? 4 : 3);
    uint16_t *d = dst + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      unsigned dth = k_bayer4[((y & 3) << 2) | (x & 3)];
      if (kind == TEX16_4444) {
        unsigned r4 = (sp[0] + dth) >> 4; if (r4 > 15) r4 = 15;
        unsigned g4 = (sp[1] + dth) >> 4; if (g4 > 15) g4 = 15;
        unsigned b4 = (sp[2] + dth) >> 4; if (b4 > 15) b4 = 15;
        unsigned a4 = (sp[3] + dth) >> 4; if (a4 > 15) a4 = 15;
        d[x] = (uint16_t)((r4 << 12) | (g4 << 8) | (b4 << 4) | a4);
        sp += 4;
      } else {
        unsigned r5 = (sp[0] + (dth >> 1)) >> 3; if (r5 > 31) r5 = 31;
        unsigned g6 = (sp[1] + (dth >> 2)) >> 2; if (g6 > 63) g6 = 63;
        unsigned b5 = (sp[2] + (dth >> 1)) >> 3; if (b5 > 31) b5 = 31;
        d[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        sp += 3;
      }
    }
  }
}

/* The one 2D upload path: convert when we can, charge what was really spent,
 * hand it to vitaGL. Always uploads exactly once. */
static void tex_upload2d(GLenum tg, GLint l, GLint ifmt, GLsizei w, GLsizei h,
                         GLint b, GLenum f, GLenum ty, const void *px) {
#if GL_TEX16_CONVERT
  int kind = (f == GL_RGBA) ? TEX16_4444 : (f == GL_RGB) ? TEX16_565 : TEX16_NONE;
  if (px && kind != TEX16_NONE && ty == GL_UNSIGNED_BYTE && w > 0 && h > 0) {
    uint16_t *cv = (uint16_t *)malloc((size_t)w * h * 2);
    if (cv) {
      tex16_pack(cv, (const unsigned char *)px, w, h, kind);
      GLenum ty16 = (kind == TEX16_4444) ? GL_UNSIGNED_SHORT_4_4_4_4
                                         : GL_UNSIGNED_SHORT_5_6_5;
      if (g_cur_tex && g_cur_tex < TEXKIND_MAX && l == 0) {
        if (g_tex16[g_cur_tex] == TEX16_NONE) g_tex16_n++;
        g_tex16[g_cur_tex] = (uint8_t)kind;
      }
      g_tex16_saved += (uint64_t)w * h * (fmt_bpp(f) - 2u);
      tex_charge(g_cur_tex, tg, l, w, h, 2u);
      glTexImage2D(tg, l, ifmt, w, h, b, f, ty16, cv);
      free(cv);
      return;
    }
  }
#endif
  tex_charge(g_cur_tex, tg, l, w, h, fmt_bpp(f));
  glTexImage2D(tg, l, ifmt, w, h, b, f, ty, px);
}

static void glTexImage2D_e(GLenum tg, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *px) {
  GLLOG("glTexImage2D(0x%x, l=%d, %dx%d, fmt=0x%x)", (unsigned)tg, l, (int)w, (int)h, (unsigned)f);
  g_texupload_frame++;
  if (w > 0 && h > 0) g_texupload_bytes_frame += (uint64_t)w * h * fmt_bpp(f);
  if (f == GL_LUMINANCE && ty == GL_UNSIGNED_BYTE && w > 0 && h > 0)
    bink_patch_note_texture_upload((unsigned)w, (unsigned)h);
  // The environment map is a real cube: kotor.vert declares u_texture2Sampler as
  // GL_SAMPLER_CUBE and the shiny-armour material is the USE_CUBEMAP variant.
  // Nothing in any log so far shows a single cube face being uploaded, so before
  // theorising about why armour renders flat white, record whether the faces
  // arrive at all -- and how many of the six.
  if (tg >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && tg <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
    static unsigned nc = 0, faces = 0;
    tex_kind_set(g_cur_cubetex, TEXKIND_CUBE);
    faces |= 1u << (tg - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    /* Only level 0 past the first cap: log151 spent all 48 lines on ONE cube's
     * mip chain and went blind afterwards, so a re-upload after an area change
     * -- the thing actually in question -- could not have been seen. */
    if (nc < 48 || (l == 0 && nc < 4096))
      log_printf("[GL] cube face %u: l=%d %dx%d ifmt=0x%x fmt=0x%x data=%s "
                 "(faces seen 0x%02x)",
                 (unsigned)(tg - GL_TEXTURE_CUBE_MAP_POSITIVE_X), l, (int)w, (int)h,
                 (unsigned)ifmt, (unsigned)f, px ? "yes" : "NULL", faces);
    nc++;
    tex_charge(g_cur_cubetex, tg, l, w, h, fmt_bpp(f));   /* cubes bind their own id */
    glTexImage2D(tg, l, ifmt, w, h, b, f, ty, px);
    return;                        // none of the 2D heuristics below apply
  }
  if (tg != GL_TEXTURE_2D) { tex_charge(g_cur_tex, tg, l, w, h, fmt_bpp(f));
                              glTexImage2D(tg, l, ifmt, w, h, b, f, ty, px); return; }
  // Square power-of-two base levels are the font atlases; naming the texture id
  // here is what lets the text-draw trace say whether a glyph quad used one.
  if (l == 0) {
    tex_kind_set(g_cur_tex, TEXKIND_2D);
    tex_note_size(g_cur_tex, (int)w, (int)h);
    if (w == h && (w == 256 || w == 512 || w == 1024))
      log_printf("[GL] atlas candidate: tex=%u %dx%d fmt=0x%x", g_cur_tex, (int)w, (int)h, (unsigned)f);
  }
  // NPOT WIDTH SHEAR TEST (log73). Backgrounds render progressively skewed while
  // every character/GUI surface is clean, and the split falls exactly on width
  // alignment -- tallying every upload in the log:
  //     w%8==0 : 8,16,32,64,128,256,512,1024 (12 sizes)  -- all render correctly
  //     w%8==7 : 847x480, 423x240  -- precisely the sheared backgrounds
  //     w%8==4 : 756x106, 4x4
  // vitaGL lays texture data out at VGL_ALIGN(w,8) stride (gpu_alloc_texture in
  // gpu_utils.c copies row-by-row into aligned_w*bpp) but then hands GXM the
  // UNALIGNED w via vglInitLinearTexture. If GXM does not re-align internally,
  // each row is read one-to-seven pixels early and the error accumulates down the
  // image -- which is exactly the diagonal banding on screen.
  // Rather than patch vitaGL blind, prove it here: for an odd width, upload a
  // padded copy whose width IS 8-aligned, replicating the last column into the
  // pad so edge sampling stays sane. If the shear disappears, the alignment
  // theory is right and the real fix belongs in vitaGL (strided texture init).
  // Cost is one staging buffer per NPOT upload; these are a handful of
  // backgrounds, not a per-frame path.
  int bpp = (f == GL_RGBA) ? 4 : (f == GL_RGB) ? 3 : 0;
  /* The pad makes the texture WIDER than the game asked for, and the game still
   * samples u across [0,1] -- so whatever sits in the pad columns is drawn as
   * part of the picture. Replicating the last column was fine for the 860x478
   * background this was written for (0.5% of one edge column) and ruinous for
   * everything small: log155 pads 2x2 -> 8x2 and 4x4 -> 8x4, where the real
   * image ends up squeezed into the left quarter and the remaining
   * three quarters are one flat colour. A soft gradient becomes a hard-edged
   * rectangle, which is exactly what the minimap frame and the fog panel on the
   * character and new-game screens look like.
   *
   * Resample the row onto the padded width instead of extending it. u in [0,1]
   * then spans the whole picture again at any size, subrect texcoords keep
   * pointing at the same texels, and vitaGL still gets the 8-aligned width its
   * stride assumes. The only cost is a horizontal resample of at most seven
   * columns' worth of ratio -- 0.5% on the background, exact-in-effect on the
   * tiny gradients, and no hard edge anywhere.
   *
   * Kept separate from the RENDER TARGET padding below, which is the actual fix
   * for the diagonal background shear and is not gated by this flag. */
#if GL_NPOT_WIDTH_PAD
  if (px && bpp && l == 0 && w > 0 && h > 0 && (w & 7)) {
    int aw = (w + 7) & ~7;
    unsigned char *pad = (unsigned char *)malloc((size_t)aw * h * bpp);
    if (pad) {
      const unsigned char *src = (const unsigned char *)px;
      /* 16.16 fixed point, sampling at column centres so the edges stay put. */
      const int step = (int)(((int64_t)w << 16) / aw);
      for (int y = 0; y < h; y++) {
        unsigned char *drow = pad + (size_t)y * aw * bpp;
        const unsigned char *srow = src + (size_t)y * w * bpp;
        int pos = step / 2 - 32768;
        for (int x = 0; x < aw; x++, pos += step) {
          int p  = pos < 0 ? 0 : pos;
          int x0 = p >> 16, fr = p & 0xFFFF;
          if (x0 >= w - 1) { x0 = w - 1; fr = 0; }
          int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
          const unsigned char *a = srow + (size_t)x0 * bpp;
          const unsigned char *c = srow + (size_t)x1 * bpp;
          unsigned char *d = drow + (size_t)x * bpp;
          for (int k = 0; k < bpp; k++)
            d[k] = (unsigned char)((a[k] * (65536 - fr) + c[k] * fr) >> 16);
        }
      }
      log_printf("[GL] NPOT resample: %dx%d -> %dx%d (bpp=%d)", (int)w, (int)h, aw, (int)h, bpp);
      tex_upload2d(tg, l, ifmt, aw, h, b, f, ty, pad);
      free(pad);
      return;
    }
  }
#endif
  // Storage-only allocation (data == NULL) is the FBO colour attachment; pad its
  // width to match the renderbuffer above so the two stay dimension-consistent.
  if (!px && l == 0) {
    GLsizei aw = gl_rt_align_w(w);
    if (aw != w)
      log_printf("[GL] RT pad: color tex %dx%d -> %dx%d", (int)w, (int)h, (int)aw, (int)h);
    tex_charge(g_cur_tex, tg, l, aw, h, fmt_bpp(f));
    glTexImage2D(tg, l, ifmt, aw, h, b, f, ty, px);
    return;
  }
  tex_upload2d(tg, l, ifmt, w, h, b, f, ty, px);
}
/* Must convert exactly as the base upload did, or vitaGL reads byte data as
 * 16-bit and the texture turns to noise. */
static void glTexSubImage2D_e(GLenum tg, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h,
                              GLenum f, GLenum ty, const void *px) {
  GLLOG("glTexSubImage2D(0x%x, l=%d, %dx%d)", (unsigned)tg, l, (int)w, (int)h);
  g_texupload_frame++;
  if (w > 0 && h > 0) g_texupload_bytes_frame += (uint64_t)w * h * fmt_bpp(f);
#if GL_TEX16_CONVERT
  uint8_t kind = (g_cur_tex && g_cur_tex < TEXKIND_MAX) ? g_tex16[g_cur_tex] : TEX16_NONE;
  if (px && kind != TEX16_NONE && ty == GL_UNSIGNED_BYTE && w > 0 && h > 0 &&
      ((kind == TEX16_4444 && f == GL_RGBA) || (kind == TEX16_565 && f == GL_RGB))) {
    uint16_t *cv = (uint16_t *)malloc((size_t)w * h * 2);
    if (cv) {
      tex16_pack(cv, (const unsigned char *)px, w, h, kind);
      glTexSubImage2D(tg, l, xo, yo, w, h, f,
                      (kind == TEX16_4444) ? GL_UNSIGNED_SHORT_4_4_4_4
                                           : GL_UNSIGNED_SHORT_5_6_5, cv);
      free(cv);
      return;
    }
  }
#endif
  glTexSubImage2D(tg, l, xo, yo, w, h, f, ty, px);
}
static void glTexParameteri_e(GLenum tg, GLenum p, GLint v) { GLLOG("glTexParameteri(0x%x,0x%x,%d)", (unsigned)tg, (unsigned)p, v); glTexParameteri(tg, p, v); }
static void glGenFramebuffers_e(GLsizei n, GLuint *f) { GLLOG("glGenFramebuffers(%d)", (int)n); glGenFramebuffers(n, f); }
static void glBindFramebuffer_e(GLenum tg, GLuint f) { GLLOG("glBindFramebuffer(0x%x,%u)", (unsigned)tg, (unsigned)f); glBindFramebuffer(tg, f); }
static void glGenRenderbuffers_e(GLsizei n, GLuint *r) { GLLOG("glGenRenderbuffers(%d)", (int)n); glGenRenderbuffers(n, r); }
static void glBindRenderbuffer_e(GLenum tg, GLuint r) { GLLOG("glBindRenderbuffer(0x%x,%u)", (unsigned)tg, (unsigned)r); glBindRenderbuffer(tg, r); }
// The scrambled chargen/menu backgrounds are RENDER TARGETS, not uploads: every
// `glTexImage2D(847x480, data=NULL)` is immediately followed by a matching
// glRenderbufferStorage for the depth attachment, i.e. the game renders its 3D
// scene off-screen and composites the result. Those sizes are 847x480 and
// 423x240 -- both width%8 == 7 -- while every surface that draws CORRECTLY is
// power-of-two. vitaGL allocates texture memory at VGL_ALIGN(w,8) stride, so a
// 847-wide target is backed by an 848-wide surface; if the render/sample paths
// disagree about which width is authoritative, every row slips by up to 7 pixels
// and the error accumulates down the image -- exactly the diagonal shear on screen.
// Round render-target dimensions up to a multiple of 8 so allocation, rendering
// and sampling all agree. Colour texture and depth renderbuffer must be padded
// TOGETHER or the FBO becomes dimension-incomplete. The game keeps its own
// viewport (847 wide) inside the slightly larger surface, so the only cost is
// ~0.1% of UV scale -- invisible next to the shear it replaces.

static void glRenderbufferStorage_e(GLenum tg, GLenum ifmt, GLsizei w, GLsizei h) {
  GLsizei aw = gl_rt_align_w(w);
  GLLOG("glRenderbufferStorage(0x%x,0x%x,%dx%d)", (unsigned)tg, (unsigned)ifmt, (int)w, (int)h);
  if (aw != w)
    log_printf("[GL] RT pad: renderbuffer %dx%d -> %dx%d", (int)w, (int)h, (int)aw, (int)h);
  glRenderbufferStorage(tg, ifmt, aw, h);
}
static void glFramebufferRenderbuffer_e(GLenum tg, GLenum at, GLenum rt, GLuint r) { GLLOG("glFramebufferRenderbuffer(at=0x%x)", (unsigned)at); glFramebufferRenderbuffer(tg, at, rt, r); }
static void glFramebufferTexture2D_e(GLenum tg, GLenum at, GLenum tt, GLuint t, GLint l) { GLLOG("glFramebufferTexture2D(at=0x%x,tex=%u)", (unsigned)at, (unsigned)t); glFramebufferTexture2D(tg, at, tt, t, l); }
static GLenum glCheckFramebufferStatus_e(GLenum tg) { GLLOG("glCheckFramebufferStatus(0x%x) ...", (unsigned)tg); GLenum s = glCheckFramebufferStatus(tg); log_printf("[GL]  -> status 0x%x", (unsigned)s); return s; }
static void glGenBuffers_e(GLsizei n, GLuint *b) { GLLOG("glGenBuffers(%d)", (int)n); glGenBuffers(n, b); }
static void glBindBuffer_e(GLenum tg, GLuint b) {
  GLLOG("glBindBuffer(0x%x,%u)", (unsigned)tg, (unsigned)b);
  if (tg == GL_ARRAY_BUFFER) g_cur_arraybuf = b;
  else if (tg == GL_ELEMENT_ARRAY_BUFFER) g_cur_elementbuf = b;
  glBindBuffer(tg, b);
}
static void glDeleteBuffers_e(GLsizei n, const GLuint *buffers) {
  if (buffers) {
    for (GLsizei i = 0; i < n; i++) {
      if (g_cur_arraybuf == buffers[i]) g_cur_arraybuf = 0;
      if (g_cur_elementbuf == buffers[i]) g_cur_elementbuf = 0;
      for (unsigned a = 0; a < GL_ATTRIB_TRACK_MAX; a++)
        if (g_attrib_state[a].buffer == buffers[i]) g_attrib_state[a].buffer = 0;
    }
  }
  glDeleteBuffers(n, buffers);
}
static void glBufferData_e(GLenum tg, GLsizeiptr sz, const void *d, GLenum u) {
  GLLOG("glBufferData(0x%x, %d bytes)", (unsigned)tg, (int)sz);
  g_bufdata_win++; g_bufdata_frame++;
  if (sz > 0) g_bufdata_bytes_frame += (uint64_t)sz;
  glBufferData(tg, sz, d, u);
}
static GLuint glCreateProgram_e(void) { GLLOG("glCreateProgram()"); GLuint p = glCreateProgram(); log_printf("[GL]  -> program %u", (unsigned)p); return p; }
/* Redundant program-switch filter.
 *
 * Inherited KOTOR I example: a heavy scene issued roughly 480 draws and 43
 * glUseProgram calls per frame, against about 140 draws in a lighter view, with ten
 * programs in the whole session -- so most of those switches re-select the
 * program that is already current.
 *
 * That is not free here the way it is on desktop GL. vitaGL's glUseProgram does
 * no GXM work at all; it sets cur_program and then marks EVERY uniform dirty
 * (dirty_vert_unifs = 0xFFFF, dirty_frag_unifs = 0xFFFFFFFF, custom_shaders.c
 * ~2462). The next draw therefore re-uploads the program's entire uniform set,
 * u_boneMatrices[51] and u_lightData[15] included, for a call that changed
 * nothing.
 *
 * Skipping it is safe, and checked against vitaGL rather than assumed:
 *   - uniforms are per-program state and persist across a re-select, so not
 *     re-marking them dirty cannot lose a value;
 *   - glUniform* flags its own slot via flag_dirty_vert_unif, so writes are
 *     still tracked while the filter is active;
 *   - gxm.c ~796 re-dirties everything at each frame end regardless ("just to be
 *     safe"), so the per-frame invalidation never depended on this call.
 * The shadow is dropped whenever a program is linked or deleted, since either
 * can change what an id means.
 * Set GL_FILTER_REDUNDANT_PROGS to 0 in config.h to rule this out. */
static void glUseProgram_e(GLuint p) {
  GLLOG("glUseProgram(%u)", (unsigned)p);
  g_prog_win++; g_prog_frame++;
#if GL_FILTER_REDUNDANT_PROGS
  if (p && p == g_cur_prog) { g_prog_skipped_win++; g_prog_skipped_frame++; return; }
  g_cur_prog = p;
#endif
  GL_TIME_STATE(glUseProgram(p));
}
static void glDeleteProgram_e(GLuint p) { g_cur_prog = 0; glDeleteProgram(p); }
static void glScissor_e(GLint x, GLint y, GLsizei w, GLsizei h) { GLLOG("glScissor(%d,%d,%d,%d)", x, y, (int)w, (int)h); GL_TIME_STATE(glScissor(x, y, w, h)); }
static void glClearStencil_e(GLint s) { GLLOG("glClearStencil(%d)", s); glClearStencil(s); }
static GLenum glGetError_e(void) { GLenum e = glGetError(); GLLOG("glGetError() -> 0x%x", (unsigned)e); return e; }
static void glHint_e(GLenum t, GLenum m) { GLLOG("glHint(0x%x,0x%x)", (unsigned)t, (unsigned)m); glHint(t, m); }
static void glFrontFace_e(GLenum m) { GLLOG("glFrontFace(0x%x)", (unsigned)m); GL_TIME_STATE(glFrontFace(m)); }
static void glCullFace_e(GLenum m) { GLLOG("glCullFace(0x%x)", (unsigned)m); GL_TIME_STATE(glCullFace(m)); }
static void glDepthFunc_e(GLenum f) { GLLOG("glDepthFunc(0x%x)", (unsigned)f); GL_TIME_STATE(glDepthFunc(f)); }
static void glColorMask_e(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { GLLOG("glColorMask(%d%d%d%d)", r, g, b, a); GL_TIME_STATE(glColorMask(r, g, b, a)); }
static void glBlendFunc_e(GLenum s, GLenum d) { GLLOG("glBlendFunc(0x%x,0x%x)", (unsigned)s, (unsigned)d); GL_TIME_STATE(glBlendFunc(s, d)); }
static void glDepthMask_e(GLboolean f) { GLLOG("glDepthMask(%d)", f); GL_TIME_STATE(glDepthMask(f)); }

/* Timed pass-throughs so per-stage state attribution covers the uniform and
 * constant-vertex-attrib entry points that were previously bound directly. */
static void glUniform1i_t(GLint l, GLint v) { GL_TIME_STATE(glUniform1i(l, v)); }
static void glUniform1iv_t(GLint l, GLsizei n, const GLint *v) { GL_TIME_STATE(glUniform1iv(l, n, v)); }
static void glUniform1fv_t(GLint l, GLsizei n, const GLfloat *v) { GL_TIME_STATE(glUniform1fv(l, n, v)); }
static void glUniform2i_t(GLint l, GLint a, GLint b) { GL_TIME_STATE(glUniform2i(l, a, b)); }
static void glUniform2iv_t(GLint l, GLsizei n, const GLint *v) { GL_TIME_STATE(glUniform2iv(l, n, v)); }
static void glUniform2fv_t(GLint l, GLsizei n, const GLfloat *v) { GL_TIME_STATE(glUniform2fv(l, n, v)); }
static void glUniform3i_t(GLint l, GLint a, GLint b, GLint c) { GL_TIME_STATE(glUniform3i(l, a, b, c)); }
static void glUniform3iv_t(GLint l, GLsizei n, const GLint *v) { GL_TIME_STATE(glUniform3iv(l, n, v)); }
static void glUniform3fv_t(GLint l, GLsizei n, const GLfloat *v) { GL_TIME_STATE(glUniform3fv(l, n, v)); }
static void glUniform4i_t(GLint l, GLint a, GLint b, GLint c, GLint d) { GL_TIME_STATE(glUniform4i(l, a, b, c, d)); }
static void glUniform4iv_t(GLint l, GLsizei n, const GLint *v) { GL_TIME_STATE(glUniform4iv(l, n, v)); }
static void glUniformMatrix2fv_t(GLint l, GLsizei n, GLboolean tr, const GLfloat *v) { GL_TIME_STATE(glUniformMatrix2fv(l, n, tr, v)); }
static void glUniformMatrix3fv_t(GLint l, GLsizei n, GLboolean tr, const GLfloat *v) { GL_TIME_STATE(glUniformMatrix3fv(l, n, tr, v)); }
static void glUniformMatrix4fv_t(GLint l, GLsizei n, GLboolean tr, const GLfloat *v) { GL_TIME_STATE(glUniformMatrix4fv(l, n, tr, v)); }
static void glVertexAttrib1fv_t(GLuint i, const GLfloat *v) { GL_TIME_STATE(glVertexAttrib1fv(i, v)); }
static void glVertexAttrib2fv_t(GLuint i, const GLfloat *v) { GL_TIME_STATE(glVertexAttrib2fv(i, v)); }
static void glVertexAttrib3fv_t(GLuint i, const GLfloat *v) { GL_TIME_STATE(glVertexAttrib3fv(i, v)); }
static void glVertexAttrib4fv_t(GLuint i, const GLfloat *v) { GL_TIME_STATE(glVertexAttrib4fv(i, v)); }

static const so_default_dynlib gl_dynlib[] = {
  /* (2) float-by-value shims */
  { "glClearColor",                      (uintptr_t)&glClearColor_s },
  { "glClearDepthf",                     (uintptr_t)&glClearDepthf_s },
  { "glDepthRangef",                     (uintptr_t)&glDepthRangef_s },
  { "glLineWidth",                       (uintptr_t)&glLineWidth_s },
  { "glPolygonOffset",                   (uintptr_t)&glPolygonOffset_s },
  { "glTexParameterf",                   (uintptr_t)&glTexParameterf_s },
  { "glUniform1f",                       (uintptr_t)&glUniform1f_s },
  { "glUniform2f",                       (uintptr_t)&glUniform2f_s },
  { "glUniform3f",                       (uintptr_t)&glUniform3f_s },
  { "glUniform4f",                       (uintptr_t)&glUniform4f_s },
  { "glVertexAttrib1f",                  (uintptr_t)&glVertexAttrib1f_s },
  { "glVertexAttrib2f",                  (uintptr_t)&glVertexAttrib2f_s },
  { "glVertexAttrib3f",                  (uintptr_t)&glVertexAttrib3f_s },
  { "glVertexAttrib4f",                  (uintptr_t)&glVertexAttrib4f_s },
  /* (3) gap stubs (not in vitaGL) */
  { "glBlendColor",                      (uintptr_t)&glBlendColor_g },
  { "glCompressedTexSubImage2D",         (uintptr_t)&glCompressedTexSubImage2D_g },
  { "glDetachShader",                    (uintptr_t)&glDetachShader_g },
  { "glGetRenderbufferParameteriv",      (uintptr_t)&glGetRenderbufferParameteriv_g },
  { "glGetShaderPrecisionFormat",        (uintptr_t)&glGetShaderPrecisionFormat_g },
  { "glGetTexParameterfv",               (uintptr_t)&glGetTexParameterfv_g },
  { "glGetTexParameteriv",               (uintptr_t)&glGetTexParameteriv_g },
  { "glGetUniformfv",                    (uintptr_t)&glGetUniformfv_g },
  { "glGetUniformiv",                    (uintptr_t)&glGetUniformiv_g },
  { "glIsBuffer",                        (uintptr_t)&glIsBuffer_g },
  { "glIsShader",                        (uintptr_t)&glIsShader_g },
  { "glSampleCoverage",                  (uintptr_t)&glSampleCoverage_g },
  { "glTexParameterfv",                  (uintptr_t)&glTexParameterfv_g },
  { "glValidateProgram",                 (uintptr_t)&glValidateProgram_g },
  /* (1t) traced lifecycle wrappers */
  { "glGetString",                       (uintptr_t)&glGetString_t },
  { "glCreateShader",                    (uintptr_t)&glCreateShader_t },
  { "glShaderSource",                    (uintptr_t)&glShaderSource_t },
  { "glCompileShader",                   (uintptr_t)&glCompileShader_t },
  { "glLinkProgram",                     (uintptr_t)&glLinkProgram_t },
  { "glViewport",                        (uintptr_t)&glViewport_t },
  { "glClear",                           (uintptr_t)&glClear_t },
  { "glDrawArrays",                      (uintptr_t)&glDrawArrays_t },
  { "glDrawElements",                    (uintptr_t)&glDrawElements_t },
  { "glGetIntegerv",                     (uintptr_t)&glGetIntegerv_t },
  { "glGetBooleanv",                     (uintptr_t)&glGetBooleanv_t },
  { "glGetFloatv",                       (uintptr_t)&glGetFloatv_t },
  /* (1e) entry-traced setup calls */
  { "glEnable",                          (uintptr_t)&glEnable_e },
  { "glDisable",                         (uintptr_t)&glDisable_e },
  { "glActiveTexture",                   (uintptr_t)&glActiveTexture_e },
  { "glPixelStorei",                     (uintptr_t)&glPixelStorei_e },
  { "glGenTextures",                     (uintptr_t)&glGenTextures_e },
  { "glBindTexture",                     (uintptr_t)&glBindTexture_e },
  { "glTexImage2D",                      (uintptr_t)&glTexImage2D_e },
  { "glTexParameteri",                   (uintptr_t)&glTexParameteri_e },
  { "glGenFramebuffers",                 (uintptr_t)&glGenFramebuffers_e },
  { "glBindFramebuffer",                 (uintptr_t)&glBindFramebuffer_e },
  { "glGenRenderbuffers",                (uintptr_t)&glGenRenderbuffers_e },
  { "glBindRenderbuffer",                (uintptr_t)&glBindRenderbuffer_e },
  { "glRenderbufferStorage",             (uintptr_t)&glRenderbufferStorage_e },
  { "glFramebufferRenderbuffer",         (uintptr_t)&glFramebufferRenderbuffer_e },
  { "glFramebufferTexture2D",            (uintptr_t)&glFramebufferTexture2D_e },
  { "glCheckFramebufferStatus",          (uintptr_t)&glCheckFramebufferStatus_e },
  { "glGenBuffers",                      (uintptr_t)&glGenBuffers_e },
  { "glBindBuffer",                      (uintptr_t)&glBindBuffer_e },
  { "glBufferData",                      (uintptr_t)&glBufferData_e },
  { "glCreateProgram",                   (uintptr_t)&glCreateProgram_e },
  { "glUseProgram",                      (uintptr_t)&glUseProgram_e },
  { "glScissor",                         (uintptr_t)&glScissor_e },
  { "glClearStencil",                    (uintptr_t)&glClearStencil_e },
  { "glGetError",                        (uintptr_t)&glGetError_e },
  { "glHint",                            (uintptr_t)&glHint_e },
  { "glFrontFace",                       (uintptr_t)&glFrontFace_e },
  { "glCullFace",                        (uintptr_t)&glCullFace_e },
  { "glDepthFunc",                       (uintptr_t)&glDepthFunc_e },
  { "glColorMask",                       (uintptr_t)&glColorMask_e },
  { "glBlendFunc",                       (uintptr_t)&glBlendFunc_e },
  { "glDepthMask",                       (uintptr_t)&glDepthMask_e },
  /* (1) direct maps to vitaGL */
  { "glAttachShader",                    (uintptr_t)&glAttachShader },
  { "glBindAttribLocation",              (uintptr_t)&glBindAttribLocation },
  { "glBlendEquation",                   (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparate",           (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFuncSeparate",               (uintptr_t)&glBlendFuncSeparate },
  { "glBufferSubData",                   (uintptr_t)&glBufferSubData },
  // GLES OES buffer-mapping ext: identical signatures to vitaGL's core maps.
  { "glMapBufferOES",                    (uintptr_t)&glMapBuffer },
  { "glUnmapBufferOES",                  (uintptr_t)&glUnmapBuffer },
  { "glGetBufferPointervOES",            (uintptr_t)&glGetBufferPointervOES_g },
  { "glBindFramebufferOES",              (uintptr_t)&glBindFramebuffer_e },
  { "glDeleteFramebuffersOES",           (uintptr_t)&glDeleteFramebuffers },
  { "glGenFramebuffersOES",              (uintptr_t)&glGenFramebuffers_e },
  { "glFramebufferRenderbufferOES",      (uintptr_t)&glFramebufferRenderbuffer_e },
  { "glFramebufferTexture2DOES",         (uintptr_t)&glFramebufferTexture2D_e },
  { "glBlendEquationOES",                (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparateOES",        (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFuncSeparateOES",            (uintptr_t)&glBlendFuncSeparate },
  { "glCompressedTexImage2D",            (uintptr_t)&glCompressedTexImage2D },
  { "glCopyTexImage2D",                  (uintptr_t)&glCopyTexImage2D },
  { "glCopyTexSubImage2D",               (uintptr_t)&glCopyTexSubImage2D },
  { "glDeleteBuffers",                   (uintptr_t)&glDeleteBuffers_e },
  { "glDeleteFramebuffers",              (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram",                   (uintptr_t)&glDeleteProgram_e },
  { "glDeleteRenderbuffers",             (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader",                    (uintptr_t)&glDeleteShader },
  { "glDeleteTextures",                  (uintptr_t)&glDeleteTextures_e },
  { "glDisableVertexAttribArray",        (uintptr_t)&glDisableVertexAttribArray_e },
  { "glEnableVertexAttribArray",         (uintptr_t)&glEnableVertexAttribArray_e },
  { "glFinish",                          (uintptr_t)&glFinish },
  { "glFlush",                           (uintptr_t)&glFlush },
  { "glGenerateMipmap",                  (uintptr_t)&glGenerateMipmap },
  { "glGetActiveAttrib",                 (uintptr_t)&glGetActiveAttrib },
  { "glGetActiveUniform",                (uintptr_t)&glGetActiveUniform },
  { "glGetAttachedShaders",              (uintptr_t)&glGetAttachedShaders },
  { "glGetAttribLocation",               (uintptr_t)&glGetAttribLocation_e },
  { "glGetBufferParameteriv",            (uintptr_t)&glGetBufferParameteriv },
  { "glGetFramebufferAttachmentParameteriv",   (uintptr_t)&glGetFramebufferAttachmentParameteriv },
  { "glGetProgramInfoLog",               (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv",                    (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog",                (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv",                     (uintptr_t)&glGetShaderiv },
  { "glGetShaderSource",                 (uintptr_t)&glGetShaderSource },
  { "glGetUniformLocation",              (uintptr_t)&glGetUniformLocation_e },
  { "glGetVertexAttribfv",               (uintptr_t)&glGetVertexAttribfv },
  { "glGetVertexAttribiv",               (uintptr_t)&glGetVertexAttribiv },
  { "glGetVertexAttribPointerv",         (uintptr_t)&glGetVertexAttribPointerv },
  { "glIsEnabled",                       (uintptr_t)&glIsEnabled },
  { "glIsFramebuffer",                   (uintptr_t)&glIsFramebuffer },
  { "glIsProgram",                       (uintptr_t)&glIsProgram },
  { "glIsRenderbuffer",                  (uintptr_t)&glIsRenderbuffer },
  { "glIsTexture",                       (uintptr_t)&glIsTexture },
  { "glReadPixels",                      (uintptr_t)&glReadPixels },
  { "glReleaseShaderCompiler",           (uintptr_t)&glReleaseShaderCompiler_t },
  { "glShaderBinary",                    (uintptr_t)&glShaderBinary },
  { "glStencilFunc",                     (uintptr_t)&glStencilFunc },
  { "glStencilFuncSeparate",             (uintptr_t)&glStencilFuncSeparate },
  { "glStencilMask",                     (uintptr_t)&glStencilMask },
  { "glStencilMaskSeparate",             (uintptr_t)&glStencilMaskSeparate },
  { "glStencilOp",                       (uintptr_t)&glStencilOp },
  { "glStencilOpSeparate",               (uintptr_t)&glStencilOpSeparate },
  { "glTexParameteriv",                  (uintptr_t)&glTexParameteriv },
  { "glTexSubImage2D",                   (uintptr_t)&glTexSubImage2D_e },
  { "glUniform1fv",                      (uintptr_t)&glUniform1fv_t },
  { "glUniform1i",                       (uintptr_t)&glUniform1i_t },
  { "glUniform1iv",                      (uintptr_t)&glUniform1iv_t },
  { "glUniform2fv",                      (uintptr_t)&glUniform2fv_t },
  { "glUniform2i",                       (uintptr_t)&glUniform2i_t },
  { "glUniform2iv",                      (uintptr_t)&glUniform2iv_t },
  { "glUniform3fv",                      (uintptr_t)&glUniform3fv_t },
  { "glUniform3i",                       (uintptr_t)&glUniform3i_t },
  { "glUniform3iv",                      (uintptr_t)&glUniform3iv_t },
  { "glUniform4fv",                      (uintptr_t)&glUniform4fv_e },
  { "glUniform4i",                       (uintptr_t)&glUniform4i_t },
  { "glUniform4iv",                      (uintptr_t)&glUniform4iv_t },
  { "glUniformMatrix2fv",                (uintptr_t)&glUniformMatrix2fv_t },
  { "glUniformMatrix3fv",                (uintptr_t)&glUniformMatrix3fv_t },
  { "glUniformMatrix4fv",                (uintptr_t)&glUniformMatrix4fv_t },
  { "glVertexAttrib1fv",                 (uintptr_t)&glVertexAttrib1fv_t },
  { "glVertexAttrib2fv",                 (uintptr_t)&glVertexAttrib2fv_t },
  { "glVertexAttrib3fv",                 (uintptr_t)&glVertexAttrib3fv_t },
  { "glVertexAttrib4fv",                 (uintptr_t)&glVertexAttrib4fv_t },
  { "glVertexAttribPointer",             (uintptr_t)&glVertexAttribPointer_e },
};

const int gl_dynlib_size = sizeof(gl_dynlib);
uintptr_t gl_lookup_symbol(const char *name) {
  for (unsigned i = 0; i < sizeof(gl_dynlib) / sizeof(gl_dynlib[0]); i++)
    if (!strcmp(gl_dynlib[i].symbol, name)) return gl_dynlib[i].func;
  return 0;
}
const so_default_dynlib *gl_get_dynlib(void) { return gl_dynlib; }
