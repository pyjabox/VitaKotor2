/* bloom_ctl.c -- switch off Aspyr's mobile bloom.
 *
 * FrameBufferModificationsEndIos (Scene::RenderSinglePass's frame-buffer
 * effects on this build) always runs a bloom chain through RenderModificationQuad
 * (call sites as libkotor2.so offsets):
 *   0x50a26a  downsample the scene into the half-resolution bloom buffer
 *   0x50a2d8  horizontal blur (kotorbloom.frag)            ~6 ms GPU
 *   0x50a342  vertical blur back into the bloom buffer     ~6 ms GPU
 *   0x50a484  scene copy (kept)
 *   0x50a548  final composite to the display (kept)
 *   0x50a566  additive bloom quad on top of the composite
 * With bloom off, the hook on RenderModificationQuad's GOT and PLT slots skips
 * the four bloom draws by return address; the copy and the composite still run,
 * so the frame is the scene without glow. BLOOM_DISABLE (config.h, default 1)
 * selects it at startup. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "bloom_ctl.h"

typedef struct { uint32_t call_off; uint16_t insn0, insn1; uint32_t ret_off; } call_site_t;
static const call_site_t k_skip[4] = {
  {0x50a26a, 0x4780, 0, 0x50a26c},          /* blx r0: downsample */
  {0x50a2d8, 0x47a0, 0, 0x50a2da},          /* blx r4: blur H */
  {0x50a342, 0x47a0, 0, 0x50a344},          /* blx r4: blur V */
  {0x50a566, 0xf56c, 0xebbc, 0x50a56a},     /* blx RenderModificationQuad@plt: bloom add */
};
static uintptr_t s_skip_ra[4];
static void (*s_orig)(void);
static int s_installed, s_off;
static uint32_t s_skipped;

static void quad_hook(void) {
  if (s_off) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    for (int i = 0; i < 4; i++)
      if (ra == s_skip_ra[i]) { s_skipped++; return; }
  }
  s_orig();
}

void bloom_ctl_install(void) {
  static const char *const k_sym = "_Z22RenderModificationQuadv";
  for (int i = 0; i < 4; i++) {
    const uint16_t *p = (const uint16_t *)(kotor_mod.text_base + k_skip[i].call_off);
    if (p[0] != k_skip[i].insn0 || (k_skip[i].insn1 && p[1] != k_skip[i].insn1)) {
      log_printf("[bloom] DISABLED: unexpected code at +0x%x (%04x %04x)",
                 (unsigned)k_skip[i].call_off, p[0], p[1]);
      return;
    }
    s_skip_ra[i] = kotor_mod.text_base + k_skip[i].ret_off;
  }
  uintptr_t original = so_symbol(&kotor_mod, k_sym);
  unsigned n = 0, bad = 0;
  uintptr_t callable = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + sym->st_name, k_sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      uintptr_t repl = (uintptr_t)&quad_hook;
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0) {
      if (!n || bad) {
        log_printf("[bloom] DISABLED: RenderModificationQuad slots %u, already hooked %u", n, bad);
        return;
      }
      s_orig = (void (*)(void))callable;
      __sync_synchronize();
    }
  }
  s_installed = 1;
  log_printf("[bloom] armed: RenderModificationQuad x%u slots", n);
}

void bloom_ctl_set_off(int off) { s_off = s_installed && off; }

uint32_t bloom_ctl_skipped(void) { uint32_t v = s_skipped; s_skipped = 0; return v; }
