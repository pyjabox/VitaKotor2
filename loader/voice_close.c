/* voice_close.c -- close the voice line a destroyed sound source forgets.
 *
 * CExoStreamingSoundSourceInternal keeps the FILE of the line it plays at
 * +0xc4. Its destructor (libkotor2 0x573180) has two paths. A source that never
 * got an FMOD channel closes the stream, then fcloses the FILE. One that had a
 * channel ([+0] != -1, with the sound system up: [[+0x28]+0xc] != 0) stops the
 * channel, closes the stream and zeroes +0xc4 -- without the fclose.
 * CGuiInGame::HandleDialogSound destroys its source at the next line, so every
 * dialogue line played leaks its FILE. With the game's own voices that costs
 * nothing: they come out of the archive, one shared handle. A voice pack's
 * lines are loose files: ramfile.c serves them from RAM, 256 at a time.
 *
 * Hardware, 2026-10-18, French voice pack, three long sessions: of the voice
 * lines opened, exactly half were never closed (the other half are
 * InitializeSource's header probes, which it fcloses itself). After ~512 lines,
 * one to two hours of play, the RAM table was full; from then on each line
 * held a card handle, and 400 s later all 64 a process may hold were taken: no
 * save, no save list, and English voices (from the archive) instead of French.
 *
 * The hook reads the two fields before the destructor runs. On the path that
 * forgets the FILE (+0xc4 zeroed), it closes it afterwards through the game's
 * own fclose -- its relocation slot, the loader's wrapper, which also returns
 * RAM copies -- as the engine's other path does: CloseStream, then fclose. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "voice_close.h"

#if VOICE_CLOSE

#define DTOR_SYM "_ZN32CExoStreamingSoundSourceInternalD1Ev"

static uintptr_t take_slots(const char *sym, uintptr_t repl, unsigned *n_out) {
  uintptr_t original = so_symbol(&kotor_mod, sym), callable = 0;
  unsigned n = 0, bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
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

/* What the engine's own calls to an import reach: the value in its slot. */
static uintptr_t import_slot(const char *sym) {
  for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
    Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
    if (ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT) continue;
    Elf32_Sym *s = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
    if (!strcmp(kotor_mod.dynstr + s->st_name, sym)) return *(uintptr_t *)(kotor_mod.text_base + rel->r_offset);
  }
  return 0;
}

typedef void *(*dtor_t)(void *self);
static dtor_t o_dtor;
static int (*s_fclose)(FILE *f);
static unsigned s_closed;

static void *h_dtor(void *self) {
  uint8_t *s = self;
  FILE *f = *(FILE **)(s + 0xc4);
  void *snd = *(void **)(s + 0x28);
  int forgets = f && snd && *(void **)((uint8_t *)snd + 0xc) && *(int32_t *)s != -1;
  void *r = o_dtor(self);
  if (forgets && !*(FILE **)(s + 0xc4)) {        /* that path zeroed it; the other closed it */
    s_fclose(f);
    unsigned n = ++s_closed;
    if (n <= 3 || n % 256 == 0)
      log_printf("[ramfile] voice line closed for a destroyed sound source (%u so far)", n);
  }
  return r;
}

void voice_close_install(void) {
  s_fclose = (int (*)(FILE *))import_slot("fclose");
  unsigned n = 0;
  if (s_fclose) o_dtor = (dtor_t)take_slots(DTOR_SYM, (uintptr_t)h_dtor, &n);
  log_printf("[ramfile] voice close: %s (destructor %u slots, fclose %s)", o_dtor ? "armed" : "NOT armed", n,
             s_fclose ? "found" : "missing");
}

#else
void voice_close_install(void) {}
#endif
