/* dxt_native.c -- upload the game's DXT textures compressed (DXT_NATIVE).
 *
 * ASLgl::glCompressedTexImage2D decompresses DXT1/3/5 to RGBA8 on the CPU for
 * every mip level the engine uploads (DecompressDXT*_8888). For level 0 it
 * also regenerates and uploads the whole mip chain with gluBuild2DMipmaps,
 * which the engine's calls for the next levels then overwrite. It starts with
 * a blocking glGetIntegerv(GL_TEXTURE_BINDING_2D) whose result it never uses.
 * Hardware stall profile, 2026-10-06 (real-vita-stallprof-20261006):
 *  - CPU DXT decode and mip generation were 35% of loading stall time;
 *  - texture creation and upload another 15%;
 *  - texture creation was the largest named cause of gameplay hitches.
 *
 * vitaGL samples S3TC natively (UBC1/2/3), one mip level per call. For a
 * power-of-two 2D DXT upload the hook hands vitaGL the compressed levels
 * instead. First it records GL_RGBA as the bound texture's format, exactly as
 * gles2-bc's glTexImage2D does for the decoded upload
 * (OpenGLESState::setBoundTextureFormat + setTextureFormat), because the uber
 * shader reads it. Every other upload goes to ASLgl as before.
 *
 * The hook places the blocks itself rather than through vitaGL's upload:
 *  - vitaGL 38d2f97 swizzles a compressed level with an asynchronous GPU copy
 *    (sceGxmTransferCopy, added upstream on 2026-05-09). In game, some effect
 *    textures came out scrambled on hardware with it
 *    (real-vita-dxtnative-20261006). The copy's layout itself is right: with
 *    each copy waited for, a startup self-test matched the CPU swizzle on
 *    every level of DXT1/DXT5 chains from 4x4 to 512x512, square or not
 *    (real-vita-dxtcpu-20261006). What goes wrong is what the copy overlaps
 *    while it is still running;
 *  - vitaGL grows a texture's block at each new level (vgl_realloc). A moved
 *    block is freed at once, while that copy can still be writing into it.
 *    That corrupts the heap; in Vita3K it was a host crash.
 * So on the GL thread each level is allocated without data (vitaGL clears it,
 * no copy). The hook then swizzles the blocks in with vitaGL's own CPU
 * swizzler, the path vitaGL used before that change. At a chain's first level
 * after 0, the block is first grown once to the chain's last full-block level,
 * so the remaining levels land in place. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>
#include <stdio.h>
#include <vitaGL.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "dxt_native.h"
#include "gl_worker.h"

#if DXT_NATIVE

#define GLES_CTX_IN_ASLGL   0x180c    /* ASLgl::g_context + 0x180c: OpenGLES20Context * */
#define GLES_STATE_IN_CTX   0x12068   /* OpenGLES20Context + 0x12068: OpenGLESState */

typedef void (*cti2d_t)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static cti2d_t o_cti2d, o_cti2d_arb;
static void (*s_set_bound_fmt)(void *state, int fmt);
static void (*s_set_fmt)(void *state);
static uint8_t *s_aslgl_ctx;
static uint32_t s_native, s_fallback, s_small, s_native_bytes;

/* ux0:data/kotor2/dxt_mode.txt: 0 sends every upload to ASLgl as before. */
static int s_mode = 1;
static int is_dxt(GLenum f) {
  return f == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || f == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ||
         f == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT || f == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}
static int pot(GLsizei v) { return v > 0 && !(v & (v - 1)); }

static int is_dxt1(GLenum f) { return f == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || f == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; }
static uint32_t level_bytes(GLsizei w, GLsizei h, int bpb) { return (uint32_t)((w + 3) / 4) * ((h + 3) / 4) * bpb; }
/* Where vitaGL keeps a level in the chain (gpu_get_compressed_mip_offset). */
static uint32_t level_offset(int level, GLsizei w, GLsizei h, int bpb) {
  uint32_t off = 0;
  for (int l = 0; l < level; l++) {
    off += level_bytes(w, h, bpb);
    if (w > 1) w /= 2;
    if (h > 1) h /= 2;
  }
  return off;
}
static int last_full_block_level(GLsizei w, GLsizei h) {
  int l = 0;
  while (w / 2 >= 4 && h / 2 >= 4) { w /= 2; h /= 2; l++; }
  return l;
}

/* vitaGL's CPU swizzler (texture_swizzler.cpp), called as its compressed
 * upload did before the GPU copy: sizes in 4x4 blocks. */
extern void SwizzleTexData64Bpp(uint8_t *dst, uint8_t *src, uint32_t x, uint32_t y, uint32_t width,
                                uint32_t height, uint32_t stride, uint32_t tileSize);
extern void SwizzleTexData128Bpp(uint8_t *dst, uint8_t *src, uint32_t x, uint32_t y, uint32_t width,
                                 uint32_t height, uint32_t stride, uint32_t tileSize);
static void swizzle_level(uint8_t *dst, const void *src, GLsizei w, GLsizei h, int bpb) {
  uint32_t bw = (uint32_t)w / 4, bh = (uint32_t)h / 4, tile = bw < bh ? bw : bh;
  if (bpb == 8) SwizzleTexData64Bpp(dst, (uint8_t *)src, 0, 0, bw, bh, bw, tile);
  else SwizzleTexData128Bpp(dst, (uint8_t *)src, 0, 0, bw, bh, bw, tile);
}

/* GL thread: levels not placed because the bound texture was not the one
 * expected (diagnostic, read by the report). */
static volatile uint32_t s_misplaced;

/* On the GL thread, in call order. args: format, level, width, height, size,
 * level to grow the chain to first (0: none). */
static void level_on_gl(const uint32_t *a, const void *data) {
  GLenum fmt = a[0];
  int level = (int)a[1], grow = (int)a[5], bpb = is_dxt1(fmt) ? 8 : 16;
  GLsizei w = (GLsizei)a[2], h = (GLsizei)a[3];
  if (grow > level) {
    GLsizei gw = w >> (grow - level), gh = h >> (grow - level);
    glCompressedTexImage2D(GL_TEXTURE_2D, grow, fmt, gw, gh, 0, level_bytes(gw, gh, bpb), NULL);
  }
  glCompressedTexImage2D(GL_TEXTURE_2D, level, fmt, w, h, 0, (GLsizei)a[4], NULL);
  SceGxmTexture *t = vglGetGxmTexture(GL_TEXTURE_2D);
  uint8_t *base = vglGetTexDataPointer(GL_TEXTURE_2D);
  GLsizei w0 = t ? (GLsizei)sceGxmTextureGetWidth(t) : 0, h0 = t ? (GLsizei)sceGxmTextureGetHeight(t) : 0;
  if (!base || w0 >> level != w || h0 >> level != h) { s_misplaced++; return; }
  swizzle_level(base + level_offset(level, w0, h0, bpb), data, w, h, bpb);
}

/* The chain whose level 0 came last (game thread). */
static struct { GLenum fmt; GLsizei w, h; int last, top, grown; } s_chain;
static uint32_t s_grown, s_short, s_stray;

/* The previous chain: count it if it was grown and the engine stopped before
 * its last level (the levels left are zero, black when minified). */
static void chain_close(void) {
  if (s_chain.grown && s_chain.top < s_chain.last) s_short++;
  memset(&s_chain, 0, sizeof s_chain);
}

static void upload(cti2d_t orig, GLenum target, GLint level, GLenum fmt, GLsizei w, GLsizei h, GLint border,
                   GLsizei size, const void *data) {
  void *ctx = s_aslgl_ctx && s_mode ? *(void **)(s_aslgl_ctx + GLES_CTX_IN_ASLGL) : NULL;
  /* Whether a texture goes native is decided by values every level of it
   * shares (target, format, power-of-two size), so one texture never mixes
   * native and decoded levels. Level 0 under one 4x4 block stays decoded. */
  if (target != GL_TEXTURE_2D || !is_dxt(fmt) || !pot(w) || !pot(h) || border || !data || !ctx ||
      (level == 0 && (w < 4 || h < 4))) {
    s_fallback++;
    orig(target, level, fmt, w, h, border, size, data);
    return;
  }
  /* Blocks are placed whole, so the mip chain ends at the last full block. */
  if (w < 4 || h < 4) { s_small++; return; }
  void *state = (uint8_t *)ctx + GLES_STATE_IN_CTX;
  s_set_bound_fmt(state, GL_RGBA);     /* what the decoded upload records */
  s_set_fmt(state);
  uint32_t args[6] = {fmt, (uint32_t)level, (uint32_t)w, (uint32_t)h, (uint32_t)size, 0};
  if (level == 0) {
    chain_close();
    s_chain.fmt = fmt;
    s_chain.w = w;
    s_chain.h = h;
    s_chain.last = last_full_block_level(w, h);
  } else if (!s_chain.w || fmt != s_chain.fmt || w != s_chain.w >> level || h != s_chain.h >> level ||
             level > s_chain.last) {
    s_stray++;                         /* not the next level of that chain: no grow */
  } else {
    if (!s_chain.grown) {
      args[5] = (uint32_t)s_chain.last;
      s_chain.grown = 1;
      s_grown++;
    }
    if (level > s_chain.top) s_chain.top = level;
  }
  glw_call(level_on_gl, args, 6, data, (uint32_t)size);
  s_native++;
  s_native_bytes += (uint32_t)size;
}

static void h_cti2d(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d) {
  upload(o_cti2d, t, l, f, w, h, b, s, d);
}
static void h_cti2d_arb(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d) {
  upload(o_cti2d_arb, t, l, f, w, h, b, s, d);
}

/* As in gl_state_filter.c: point every relocation slot of sym that holds the
 * engine's own function at repl; returns the original, 0 if nothing changed. */
static uintptr_t take_slots(const char *sym, uintptr_t repl, unsigned *n_out) {
  uintptr_t original = so_symbol(&kotor_mod, sym), callable = 0;
  unsigned n = 0, bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *s = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + s->st_name, sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0 && (!n || bad)) { if (n_out) *n_out = 0; return 0; }
  }
  if (n_out) *n_out = n;
  return callable;
}

void dxt_native_install(void) {
  SceUID fd = sceIoOpen("ux0:data/kotor2/dxt_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char c = 0; if (sceIoRead(fd, &c, 1) == 1 && c >= '0' && c <= '1') s_mode = c - '0'; sceIoClose(fd); }
  log_printf("[dxt] mode %d", s_mode);
  s_aslgl_ctx = (uint8_t *)so_symbol(&kotor_mod, "_ZN5ASLgl9g_contextE");
  s_set_bound_fmt = (void (*)(void *, int))so_symbol(&kotor_mod,
      "_ZN8OpenGLES9OpenGLES213OpenGLESState21setBoundTextureFormatEi");
  s_set_fmt = (void (*)(void *))so_symbol(&kotor_mod, "_ZN8OpenGLES9OpenGLES213OpenGLESState16setTextureFormatEv");
  o_cti2d = (cti2d_t)so_symbol(&kotor_mod, "_ZN5ASLgl22glCompressedTexImage2DEjijiiiiPKv");
  o_cti2d_arb = (cti2d_t)so_symbol(&kotor_mod, "_ZN5ASLgl25glCompressedTexImage2DARBEjijiiiiPKv");
  if (!s_aslgl_ctx || !s_set_bound_fmt || !s_set_fmt || !o_cti2d || !o_cti2d_arb) {
    log_printf("[dxt] DISABLED: symbol missing (ctx %p fmt %p/%p upload %p/%p)", (void *)s_aslgl_ctx,
               (void *)s_set_bound_fmt, (void *)s_set_fmt, (void *)o_cti2d, (void *)o_cti2d_arb);
    return;
  }
  unsigned n1 = 0, n2 = 0;
  uintptr_t a = take_slots("_ZN5ASLgl22glCompressedTexImage2DEjijiiiiPKv", (uintptr_t)h_cti2d, &n1);
  if (a) o_cti2d = (cti2d_t)a;
  uintptr_t b = take_slots("_ZN5ASLgl25glCompressedTexImage2DARBEjijiiiiPKv", (uintptr_t)h_cti2d_arb, &n2);
  if (b) o_cti2d_arb = (cti2d_t)b;
  log_printf("[dxt] armed: native S3TC uploads, slots %u %u (glCompressedTexImage2D, ARB)", n1, n2);
}


void dxt_native_report(void) {
  if (!s_native && !s_fallback && !s_small) return;
  static uint32_t misplaced0;
  uint32_t misplaced = s_misplaced;
  log_printf("[dxt] uploads: %u native (%u KB compressed), %u levels skipped, %u through ASLgl | chains grown %u, "
             "short %u, stray levels %u, misplaced %u", s_native, s_native_bytes / 1024u, s_small, s_fallback,
             s_grown, s_short, s_stray, misplaced - misplaced0);
  misplaced0 = misplaced;
  s_native = s_fallback = s_small = s_native_bytes = s_grown = s_short = s_stray = 0;
}

#else
void dxt_native_install(void) {}
void dxt_native_report(void) {}
#endif
