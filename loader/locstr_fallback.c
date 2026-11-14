/* locstr_fallback.c -- English text where a mod has no translation.
 *
 * A CExoLocString (dialogue lines, item names, journal entries in .dlg/.uti/
 * .jrl files) holds either a talk-table StrRef or text embedded per language,
 * substring id = language * 2 + gender. CExoLocString::GetString(lang, out,
 * gender) (0x5555a8, const 0x555694) tries the embedded text for the asked
 * language, then the StrRef in the talk table.
 *
 * Mods written for the English game embed English only: M4-78EP's mobile
 * build carries 13,652 such strings and no talk table, and its readme says it
 * works in the English version only. In French every one of them comes back
 * empty -- blank lines, unnamed items, conversations that stop.
 *
 * Such a string's StrRef is -1, and CTlkTable::FetchInternal answers -1 with
 * "" and success (0x56a11c), so GetString returns 1 with an empty text: what
 * counts is the text, not the result. The server's own messages
 * (WriteCExoLocStringServer, GetLocStringServerTime, HasValidString) already
 * retry in English through GetStringLoc; GetString's callers do not. One of
 * them is CSWSDialog::SendDialogReplyNode, which gives up on a reply list
 * that mixes empty and non-empty lines (0x4c4790): the Sith Academy's
 * detention terminal in 703KOR (M4-78EP termtort.dlg) offers "Play the last
 * Security recording." (English only) beside talk-table lines, so in French
 * the terminal stopped opening once its first part was done (2026-11-14).
 *
 * Both GetString variants are hooked through their relocation slots (the
 * engine calls its exported functions through the PLT): when one comes back
 * empty and the language is not English, the English text (substring 0, as
 * the English game reads it whatever the gender, then 1) is returned
 * instead. Strings with text in the language are untouched. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "locstr_fallback.h"

typedef int (*get_fn)(void *self, int lang, void *out, unsigned char gender);
typedef int (*internal_fn)(const void *self, int id, void *out);
typedef struct { const char *s; uint32_t cap; } exostr_t;   /* CExoString: text, buffer size */

/* The originals, set before the first slot is redirected to the hooks. */
static uintptr_t s_get, s_get_const;
static internal_fn s_internal;
static volatile unsigned s_used;

static int empty(const void *out) {
  const exostr_t *e = (const exostr_t *)out;
  return !e->s || !e->s[0];           /* CExoString::GetLength: s ? strlen(s) : 0 */
}

/* GetString's answer `r` for a language other than English, made English
 * when it has no text and the string has an English one. */
static int fallback(const void *self, void *out, int r) {
  if (r && !empty(out)) return r;
  if (!s_internal(self, 0, out) && !s_internal(self, 1, out)) return r;
  if (__sync_add_and_fetch(&s_used, 1) == 1)
    log_printf("[lang] first string with no text in this language: English used instead");
  return 1;
}

static int get_hook(void *self, int lang, void *out, unsigned char gender) {
  int r = ((get_fn)s_get)(self, lang, out, gender);
  return lang ? fallback(self, out, r) : r;
}

static int get_const_hook(void *self, int lang, void *out, unsigned char gender) {
  int r = ((get_fn)s_get_const)(self, lang, out, gender);
  return lang ? fallback(self, out, r) : r;
}

/* Point every relocation slot of `sym` at `repl`, once the original is in
 * *orig (as ai_throttle_install does). */
static int hook_slots(const char *sym, uintptr_t repl, uintptr_t *orig) {
  uintptr_t original = so_symbol(&kotor_mod, sym);
  unsigned n = 0, bad = 0;
  uintptr_t callable = 0;
  if (!original) return 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *s = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + s->st_name, sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if ((*slot ^ original) & ~(uintptr_t)1) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0) {
      if (!n || bad) {
        log_printf("[lang] fallback DISABLED: %s slots %u, already hooked %u", sym, n, bad);
        return 0;
      }
      *orig = callable;
      __sync_synchronize();
    }
  }
  return (int)n;
}

void locstr_fallback_install(int lang) {
  if (!lang) return;
  s_internal = (internal_fn)so_symbol(&kotor_mod, "_ZNK13CExoLocString17GetStringInternalEiP10CExoString");
  if (!s_internal) {
    log_printf("[lang] fallback DISABLED: CExoLocString::GetStringInternal not found");
    return;
  }
  int a = hook_slots("_ZN13CExoLocString9GetStringEiP10CExoStringh", (uintptr_t)&get_hook, &s_get);
  int b = hook_slots("_ZNK13CExoLocString9GetStringEiP10CExoStringh", (uintptr_t)&get_const_hook, &s_get_const);
  log_printf("[lang] English fallback for untranslated mod text: %d + %d call slots", a, b);
}
