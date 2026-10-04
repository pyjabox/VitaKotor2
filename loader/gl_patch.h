/* gl_patch.h -- GLES2 -> vitaGL resolution table (see gl_patch.c) */

#ifndef __GL_PATCH_H__
#define __GL_PATCH_H__

#include "so_util.h"

const so_default_dynlib *gl_get_dynlib(void);
extern const int gl_dynlib_size;
uintptr_t gl_lookup_symbol(const char *name);

enum {
  GL_STAGE_OUTSIDE = 0,
  GL_STAGE_SINGLE_PASS,
  GL_STAGE_SHADOWS,
  GL_STAGE_DYNAMIC,
  GL_STAGE_STATIC,
  GL_STAGE_LENS_FLARES,
  GL_STAGE_MESH_BUCKETS,
  GL_STAGE_GOB_BUCKETS,
  GL_STAGE_HOLOGRAM_BUCKETS,
  GL_STAGE_DISTORTION_BUCKETS,
  GL_STAGE_FADE_BUCKETS,
  GL_STAGE_EMITTER_BUCKET,
  GL_STAGE_COUNT
};

/* Scene wrappers in main.c maintain this stack. Draw wrappers charge work to
 * its deepest active stage, so nested stages do not double-count draws. */
void gl_perf_stage_enter(unsigned stage);
void gl_perf_stage_leave(unsigned stage);

enum {
  GL_CALLEE_OUTSIDE = 0,
  GL_CALLEE_GOB_VISIBILITY,
  GL_CALLEE_GOB_RENDER,
  GL_CALLEE_GOB_PART_DRAW,
  GL_CALLEE_TRIMESH_DRAW,
  GL_CALLEE_RENDER_FLAT,
  GL_CALLEE_RENDER_LIGHTMAPPED,
  GL_CALLEE_RENDER_ENVMAPPED,
  GL_CALLEE_RENDER_EMLM,
  GL_CALLEE_VERTEX_PROGRAM_ENABLE,
  GL_CALLEE_MATERIAL_BIND_TEXTURE0,
  GL_CALLEE_PROXY_PART_DRAW,
  GL_CALLEE_IS_PART_RENDERABLE,
  GL_CALLEE_GET_RENDER_PATH,
  GL_CALLEE_SET_INTERLEAVED_BUFFER,
  GL_CALLEE_GLRENDER_DRAW_ELEMENTS,
  GL_CALLEE_POOL_LOOKUP,
  GL_CALLEE_MAINLOOP,
  GL_CALLEE_MSGPUMP,
  GL_CALLEE_COUNT
};

/* Independent nested timing for engine render callees below DoGobBuckets. */
void gl_perf_callee_enter(unsigned callee);
void gl_perf_callee_leave(unsigned callee);

/* Passive Gob identity census. Gob::Render wrappers supply only the object
 * pointer; draw wrappers count submissions made while that render is active.
 * No proprietary object fields are dereferenced. */
void gl_perf_gob_enter(const void *gob, const char *model_name);
void gl_perf_gob_leave(const void *gob);
void gl_perf_gob_note_room(const void *gob, const char *room_name);

/* Called once per presented frame from the SDL swap hook. Emits a periodic
 * summary (draws/clears since the last summary) so we can tell a live, advancing
 * render loop from one that is stuck repeating an identical frame. */
void gl_patch_on_swap(uint64_t swap_begin_us, uint64_t swap_end_us);

/* Lifetime count of glDrawArrays + glDrawElements, maintained in every build.
 * Wraps at 2^32; callers difference successive reads. */
uint32_t gl_patch_total_draws(void);

/* Set around CAurGUIStringInternal::Draw (see main.c) so the GL layer logs the
 * draw calls the text path actually issues, with the texture bound at the time.
 * Text has metrics and an uploaded atlas yet renders nothing, so the open question
 * is whether glyph quads reach GL at all -- and if so, against which texture.
 * Scoped this way the trace stays tiny instead of drowning the log. */
extern int g_gl_text_draw;

/* Scope for KOTOR's nested AurGUI viewport stack. */
extern int g_gl_gui_viewport_scope;

#endif
