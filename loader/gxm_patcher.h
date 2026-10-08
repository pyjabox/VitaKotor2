/* gxm_patcher.h -- keep vitaGL's vertex-program pool from filling up.
 *
 * vitaGL asks the GXM shader patcher for a patched vertex program on every
 * draw (custom_shaders.c) and never gives one back. Each new pair of shader
 * and vertex layout takes vertex USSE memory from a fixed pool, 1 MB unless
 * vglSetupShaderPatcher says otherwise, so a long session walks through
 * enough areas to fill it. After that every create for a new layout fails,
 * vitaGL ignores the error and draws with the program it had, built for
 * another layout, and the GPU reads positions from the wrong bytes: the world
 * geometry tears into spikes until relaunch. Found and fixed in the KOTOR I
 * port (VitaKotor 32ffdb8: pool at 1014 of 1024 KB after 20 minutes); the
 * vitaGL revision, its draw path and the offset patch here are the same.
 *
 * The pool is GXMP_VERTEX_USSE_KB (set before vglInitExtended). Past that,
 * this keeps one reference per variant, remembers the frame it was last
 * bound, and when a create fails, releases the ones no draw has bound for a
 * while and tries again. Repeat requests are answered from a cache instead of
 * the patcher (GXMP_MEMO).
 *
 * Every function here runs on the thread that runs vitaGL: the GL worker's,
 * or the game's with the worker off. ux0:data/kotor2/patcher_mode.txt
 * containing 0 leaves vitaGL's requests alone (the pool stays enlarged).
 */
#pragma once

#include <stdint.h>

/* Bytes for vglSetupShaderPatcher, called before vglInitExtended. */
uint32_t gxmp_vertex_usse_bytes(void);
#define GXMP_BUFFER_MEM        (1 * 1024 * 1024)
#define GXMP_FRAGMENT_USSE_MEM (1 * 1024 * 1024)

/* Start managing variants, after vglInitExtended. Everything created before
 * (vitaGL's own clear and blit programs, made once and bound forever) stays
 * put, and so does every fixed-function variant, which vitaGL re-binds from a
 * stored pointer. No-op if patcher_mode.txt says 0. */
void gxmp_arm(void);

/* After each vglSwapBuffers, on the same thread: frame count, and the
 * [gxmp] line every GXMP_REPORT_S. */
void gxmp_on_swap(void);
