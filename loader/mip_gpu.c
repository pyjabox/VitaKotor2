/* mip_gpu.c -- mip chains of uncompressed images made by vitaGL (MIPGEN_GPU).
 *
 * The engine builds the mip chain of every uncompressed image (GUI art, the
 * minimap, portraits, loading screens) with its own gluBuild2DMipmaps: it
 * shrinks each level on the CPU and uploads it through ASLgl. vitaGL ignores
 * the pixels of a level above 0 of an uncompressed texture and downsamples
 * level 0 on the GPU instead (_glTexImage2D_FlatIMPL -> gpu_alloc_mipmaps),
 * so the CPU work, the 16-bit conversion of each level and its trip through
 * the GL worker are all thrown away. For a power-of-two image the hook
 * uploads level 0 through ASLgl as the engine does, then has vitaGL make the
 * chain in one pass (glGenerateMipmap: the same gpu_alloc_mipmaps). Every
 * other image goes to the engine's gluBuild2DMipmaps as before.
 *
 * Test builds (MIPGEN_AB): every other image takes the engine's path, and the
 * [mipgen] line times both. ux0:data/kotor2/mipgen_mode.txt containing 0
 * turns the hook off. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>
#include <vitaGL.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "stall_parts.h"
#include "mip_gpu.h"

#if MIPGEN_GPU

typedef GLint (*build_t)(GLenum, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*teximage_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static build_t o_build;
static teximage_t s_aslgl_teximage;
static int s_mode = 1;
static uint32_t s_n[2], s_max_us[2];       /* [0] the engine's path, [1] vitaGL's */
static uint64_t s_us[2];

static int pot(GLsizei v) { return v > 0 && !(v & (v - 1)); }

static GLint h_build(GLenum target, GLint ifmt, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int gpu = s_mode && target == GL_TEXTURE_2D && pot(w) && pot(h) && data;
#if MIPGEN_AB
  static unsigned k;
  if (gpu && (k++ & 1)) gpu = 0;
#endif
  GLint r = 0;
  if (gpu) {
    s_aslgl_teximage(target, 0, ifmt, w, h, 0, fmt, type, data);
    glGenerateMipmap(GL_TEXTURE_2D);
  } else {
    r = o_build(target, ifmt, w, h, fmt, type, data);
  }
  uint32_t us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
  s_n[gpu]++;
  s_us[gpu] += us;
  if (us > s_max_us[gpu]) s_max_us[gpu] = us;
  stall_part(SP_MIPGEN, us, 0);
  return r;
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

void mip_gpu_install(void) {
  SceUID fd = sceIoOpen("ux0:data/kotor2/mipgen_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char c = 0; if (sceIoRead(fd, &c, 1) == 1 && (c == '0' || c == '1')) s_mode = c - '0'; sceIoClose(fd); }
  o_build = (build_t)so_symbol(&kotor_mod, "gluBuild2DMipmaps");
  s_aslgl_teximage = (teximage_t)so_symbol(&kotor_mod, "_ZN5ASLgl12glTexImage2DEjiiiiijjPKv");
  if (!o_build || !s_aslgl_teximage) {
    log_printf("[mipgen] DISABLED: symbol missing (build %p, ASLgl upload %p)", (void *)o_build,
               (void *)s_aslgl_teximage);
    return;
  }
  unsigned n = 0;
  uintptr_t a = take_slots("gluBuild2DMipmaps", (uintptr_t)h_build, &n);
  if (a) o_build = (build_t)a;
  log_printf("[mipgen] armed: mode %d, %u slots%s", s_mode, n, MIPGEN_AB ? ", A/B every other image" : "");
}

void mip_gpu_report(void) {
  if (!s_n[0] && !s_n[1]) return;
  log_printf("[mipgen] engine path %u images, avg %u us, max %u us | vitaGL path %u images, avg %u us, max %u us",
             s_n[0], s_n[0] ? (unsigned)(s_us[0] / s_n[0]) : 0, s_max_us[0], s_n[1],
             s_n[1] ? (unsigned)(s_us[1] / s_n[1]) : 0, s_max_us[1]);
  memset(s_n, 0, sizeof s_n);
  memset(s_us, 0, sizeof s_us);
  memset(s_max_us, 0, sizeof s_max_us);
}

#else
void mip_gpu_install(void) {}
void mip_gpu_report(void) {}
#endif
