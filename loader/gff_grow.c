/* gff_grow.c -- the engine's GFF writer and string reader, made safe for big
 * saves: field data grows in larger steps and starts zeroed, and strings with
 * impossible lengths read as empty.
 *
 * CResGFF::AddDataField makes room for a field's data at the end of one
 * buffer. When the buffer is full it allocates the needed size plus a fixed
 * step (the writer's +0x34, about 119 KB), copies everything across and frees
 * the old buffer. Saving a module writes its whole state into one GFF -- the
 * AI event queue with every queued script's stack, the creatures, their
 * effects -- and in the Khoonda battle that buffer reached 16 MB: about 137
 * reallocations, a gigabyte of copying, and at every step an old and a new
 * block of that size alive at once. On hardware the last of those, 16,302,359
 * bytes, found no contiguous block in the heap and the game crashed on the
 * SAVING screen (diagnostics/save-stuck-20261105).
 *
 * This is the same function with two changes: the step is at least an eighth
 * of the size, so a 16 MB buffer takes about 40 reallocations instead of 137
 * (below about 1 MB the step is the engine's own); and from 256 KB up the
 * buffer grows with newlib's realloc, in place whenever it can.
 * Everything else -- where the field goes, the data size in the header, the
 * returned pointer -- is the engine's code, transcribed:
 *
 *   used = header->field_data_size
 *   start = (off + old_len == used) ? off : used   (the last field grows in place)
 *   end = start + new_len
 *   if end >= capacity: capacity = end + step; new[], copy used bytes, delete[]
 *   if start != off: copy the field's old bytes to start (at most new_len:
 *     the engine copies old_len, past its own room when a moved field shrinks)
 *   header->field_data_size = end; return buffer + start
 *
 * The buffer comes from and goes back to the same operator new[] / delete[]
 * the engine's own calls reach (dynlib), so the GFF's destructor frees it as
 * before. CResGFF::ByteSwap is a no-op in this build (ByteSwap(unsigned long*)
 * is `bx lr`).
 *
 * The new room for each field starts zeroed, and CResGFF::ReadFieldCExoString
 * reads a string longer than 64 KB as empty; see the reader below for why.
 *
 * ux0:data/kotor2/gff_mode.txt: 0 leaves the engine's functions in place. */

#ifndef GFF_GROW_HOST_TEST
#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "dynlib.h"
#include "bigalloc.h"
#include "gff_grow.h"
#endif
#include <stdlib.h>

/* From this capacity up the buffer grows with newlib's realloc (below). */
#define GFF_REALLOC_MIN (256u * 1024u)

#if GFF_GROW

#define FLD(p, off, T) (*(T *)((uint8_t *)(p) + (off)))
/* Pointer fields of the engine's 32-bit object (the host test maps them to
 * 32-bit handles). */
#ifdef GFF_GROW_HOST_TEST
#define GET_PTR(p, off) ((uint8_t *)host_ptr_get(FLD(p, off, uint32_t)))
#define SET_PTR(p, off, v) host_ptr_set(&FLD(p, off, uint32_t), v)
#else
#define GET_PTR(p, off) FLD(p, off, uint8_t *)
#define SET_PTR(p, off, v) (FLD(p, off, uint8_t *) = (v))
#endif

#ifndef GFF_GROW_HOST_TEST
/* Every relocation slot of `sym` that still holds the engine's function gets
 * `repl`; returns the original, or 0 (and changes nothing) if any slot was
 * already taken. Same as proc_anim.c's. */
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
#endif

static uint32_t w_grows, w_big_grows, w_max_kb, w_realloc, w_moved;

static uint8_t *h_add_data_field(void *gff, uint32_t off, uint32_t old_len, uint32_t new_len) {
  uint8_t *hdr = GET_PTR(gff, 0x40);
  uint8_t *buf = GET_PTR(gff, 0x5c);
  if (!hdr || !buf) return NULL;
  uint32_t used = FLD(hdr, 0x24, uint32_t);
  uint32_t start = used == old_len + off ? off : used;
  uint32_t end = start + new_len;
  if (end >= FLD(gff, 0x60, uint32_t)) {
    uint32_t step = FLD(gff, 0x34, uint32_t), eighth = end / 8u;
    uint32_t cap = end + (eighth > step ? eighth : step);
    FLD(gff, 0x60, uint32_t) = cap;
    uint32_t have = FLD(hdr, 0x24, uint32_t);
    uint8_t *nb = NULL;
    /* Big buffers grow with newlib's realloc, which extends a block in place
     * when the memory after it is free -- at the top of the arena, where a
     * growing buffer ends up, that is every time after the first move. No
     * second copy then: a 40 MB save record (Dantooine, 2026-11-05) needs
     * 40 MB, not 40 + 35. The block stays newlib's, so the engine's delete[]
     * (operator delete -> free) still frees it. */
    if (cap >= GFF_REALLOC_MIN) {
      if (!bigalloc_owns(buf)) {
        uint8_t *was = buf;
        nb = (uint8_t *)realloc(buf, cap);
        if (nb) { w_realloc++; if (nb != was) w_moved++; }
      } else if ((nb = (uint8_t *)malloc(cap)) != NULL) {
        memcpy(nb, buf, have);
        k2_delete_array(buf);
        w_realloc++;
        w_moved++;
      }
    }
    if (!nb) {
      nb = (uint8_t *)k2_new_array(cap);        /* pool or jumbo; throws like the engine's on failure */
      memcpy(nb, buf, have);
      k2_delete_array(buf);
    }
    SET_PTR(gff, 0x5c, nb);
    buf = nb;
    w_grows++;
    if (eighth > step) w_big_grows++;
    if ((cap >> 10) > w_max_kb) w_max_kb = cap >> 10;
  }
  /* The engine copies old_len bytes here, which runs past the room it made
   * when a moved field shrinks; the bytes inside the field are the same. */
  uint32_t kept = old_len < new_len ? old_len : new_len;
  if (start != off) memcpy(buf + start, buf + off, kept);
  /* The new room starts zeroed, as the engine's writers assume: an empty
   * CExoString gets no length word written at all (WriteFieldCExoString skips
   * the store when the length is 0), so without this its length is whatever
   * the buffer held -- and loading reads that back as the string's length. */
  if (new_len > kept) memset(buf + start + kept, 0, new_len - kept);
  FLD(hdr, 0x24, uint32_t) = end;
  return buf + start;
}

/* ---- reading: CResGFF::ReadFieldCExoString ---------------------------------
 *
 * Saves written before the zero fill above carry random lengths for empty
 * strings. The engine's reader only checks a length against the data after it,
 * so a random length that fits -- 18.9 MB for one "Value" in the Dantooine save
 * of 2026-11-05 -- becomes a string of that size made of the bytes that follow,
 * which the next save writes back for real: the 14.8 MB save of Nov 4 was
 * 82.5 MB after one session. Same function, transcribed (field by label, type
 * 10, data at least 4 bytes, length + 4 within it), with two changes: a length
 * above GFF_STRING_MAX reads as the empty string it was, and the bounds check
 * cannot wrap (the engine's `len + 4` overflows for lengths near 4 GB). */
#define GFF_STRING_MAX (64u * 1024u)
typedef uint32_t (*by_label_t)(void *gff, void *st, const char *label);
typedef void *(*get_field_t)(void *gff, void *st, uint32_t idx);
typedef uint8_t *(*get_data_t)(void *gff, void *field, uint32_t *avail);
typedef void *(*str_len_t)(void *self, const char *s, int len);
typedef void *(*str_copy_t)(void *self, const void *other);
static by_label_t f_by_label;
static get_field_t f_get_field;
static get_data_t f_get_data;
static str_len_t f_str_len;
static str_copy_t f_str_copy;
static uint32_t w_str_fixed, w_str_fixed_max;
static char w_str_fixed_label[20];

static void *h_read_cexostring(void *ret, void *gff, void *st, const char *label, int *ok, const void *def) {
  if (st && label) {
    uint32_t idx = f_by_label(gff, st, label);
    uint8_t *field = (uint8_t *)f_get_field(gff, st, idx);
    if (field && FLD(field, 0, uint32_t) == 10) {
      uint32_t avail = 0;
      uint8_t *d = f_get_data(gff, field, &avail);
      if (d && avail > 3) {
        uint32_t len = FLD(d, 0, uint32_t);
        if ((uint64_t)len + 4u <= avail) {
          if (len > GFF_STRING_MAX) {
            w_str_fixed++;
            if (len > w_str_fixed_max) {
              w_str_fixed_max = len;
              strncpy(w_str_fixed_label, label, sizeof w_str_fixed_label - 1);
            }
            len = 0;
          }
          *ok = 1;
          f_str_len(ret, (const char *)d + 4, (int)len);
          return ret;
        }
      }
    }
  }
  *ok = 0;
  f_str_copy(ret, def);
  return ret;
}

#ifndef GFF_GROW_HOST_TEST
/* ---- saving script situations without their code ---------------------------
 *
 * Every pending scripted event (a DelayCommand, an action with a script
 * attached) is saved by CVirtualMachineInternal::SaveScriptSituation with a
 * full copy of its script's compiled code ("CodeSize", "Code"). The Dantooine
 * battle save of 2026-11-05 had 306 of them, all the same 121,624-byte script:
 * a 37 MB record for one area, too big to build or read back on the Vita.
 *
 * The engine already knows a situation without code: when it cannot read a
 * script file it saves CodeSize 0 and no Code, LoadScriptSituation then leaves
 * the code empty, and SetUpScriptSituation reads the script by name when the
 * event fires. So when a situation's code is byte for byte the script file the
 * engine would read by that name (ReadScriptFile, checked once per script and
 * save, the way SaveScriptSituation itself reads it), it is saved that way.
 * Anything else -- a script that differs from its file, or cannot be read --
 * keeps its copy. A situation with no code in memory -- most of them: the code
 * is read by name when the event fires -- had the engine read the file just to
 * embed it; it is saved as it is held, without code. The engine's own writes
 * are what change, picked out by their return addresses in SaveScriptSituation
 * (one pair per branch): CodeSize is written as 0 and Code is not written. */
typedef int (*save_sit_t)(void *vm, void *script, void *gff, void *st);
typedef int (*read_script_t)(void *vm, void *name);
typedef void (*delete_script_t)(void *vm, void *slot);
typedef int (*write_int_t)(void *gff, void *st, int value, const char *label);
typedef int (*write_void_t)(void *gff, void *st, const void *p, uint32_t size, const char *label);
static save_sit_t o_save_sit;
static read_script_t f_read_script;
static delete_script_t f_delete_script;
static write_int_t o_write_int;
static write_void_t o_write_void;
static volatile int s_slim_now;
static SceUID s_slim_thread;
#define SLIM_SITE_INT 0x58a482u          /* WriteFieldINT("CodeSize"), code in memory */
#define SLIM_SITE_VOID 0x58a494u         /* WriteFieldVOID("Code"), code in memory */
#define SLIM_SITE_INT_FILE 0x58a4d0u     /* the same two, code read from the file */
#define SLIM_SITE_VOID_FILE 0x58a4eeu
#define SLIM_MIN 4096u
#define SLIM_CACHE 8
static struct { char name[32]; uint32_t size; uint8_t *code; int ok; } s_ver[SLIM_CACHE];
static int s_nver;
static uint64_t s_ver_t;
static uint32_t w_slim, w_full, w_checked, w_differ, w_lazy;
static uint64_t w_slim_bytes, w_full_bytes;

/* The script file's code for this name, read the way SaveScriptSituation reads
 * it (ReadScriptFile puts it in the VM's next slot; DeleteScript and the depth
 * back down), kept for the rest of this save. */
static int slim_ok(void *vm, void *script) {
  const uint8_t *code = FLD(script, 16, const uint8_t *);
  uint32_t size = FLD(script, 20, uint32_t);
  const char *name = FLD(script, 24, const char *);
  uint64_t now = sceKernelGetProcessTimeWide();
  if (now - s_ver_t > 3000000u) {                /* a new save: forget the last one's files */
    for (int i = 0; i < s_nver; i++) free(s_ver[i].code);
    s_nver = 0;
  }
  s_ver_t = now;
  int e = -1;
  for (int i = 0; i < s_nver; i++)
    if (!strncmp(s_ver[i].name, name, sizeof s_ver[i].name - 1)) { e = i; break; }
  if (e < 0) {
    if (strlen(name) >= sizeof s_ver[0].name || s_nver == SLIM_CACHE) return 0;
    e = s_nver++;
    strcpy(s_ver[e].name, name);
    s_ver[e].ok = 0;
    s_ver[e].code = NULL;
    s_ver[e].size = 0;
    w_checked++;
    if (f_read_script(vm, (uint8_t *)script + 24) == 0) {
      uint8_t *slot = (uint8_t *)vm + 44 + FLD(vm, 40, uint32_t) * 40u;
      const uint8_t *fcode = FLD(slot, 16, const uint8_t *);
      uint32_t fsize = FLD(slot, 20, uint32_t);
      if (fcode && fsize && (s_ver[e].code = malloc(fsize)) != NULL) {
        memcpy(s_ver[e].code, fcode, fsize);
        s_ver[e].size = fsize;
        s_ver[e].ok = 1;
      }
      f_delete_script(vm, slot);
      FLD(vm, 40, uint32_t)--;
    }
  }
  int same = s_ver[e].ok && s_ver[e].size == size && !memcmp(s_ver[e].code, code, size);
  if (!same) w_differ++;
  return same;
}

static int h_save_sit(void *vm, void *script, void *gff, void *st) {
  uint32_t size = FLD(script, 20, uint32_t);
  int lazy = !FLD(script, 16, void *) && FLD(script, 24, char *);
  int slim = lazy || (FLD(script, 16, void *) && size >= SLIM_MIN && FLD(script, 24, char *) && slim_ok(vm, script));
  if (slim) {
    s_slim_thread = sceKernelGetThreadId();
    s_slim_now = 1;
    if (lazy) w_lazy++;
    else { w_slim++; w_slim_bytes += size; }
  } else if (FLD(script, 16, void *)) { w_full++; w_full_bytes += size; }
  int r = o_save_sit(vm, script, gff, st);
  s_slim_now = 0;
  return r;
}

static int slim_site(uint32_t ret, uint32_t a, uint32_t b) {
  if (!s_slim_now) return 0;
  uint32_t off = ret - 1 - kotor_mod.text_base;
  return (off == a || off == b) && sceKernelGetThreadId() == s_slim_thread;
}
static int h_write_int(void *gff, void *st, int value, const char *label) {
  if (slim_site((uint32_t)(uintptr_t)__builtin_return_address(0), SLIM_SITE_INT, SLIM_SITE_INT_FILE)) value = 0;
  return o_write_int(gff, st, value, label);
}
static int h_write_void(void *gff, void *st, const void *p, uint32_t size, const char *label) {
  if (slim_site((uint32_t)(uintptr_t)__builtin_return_address(0), SLIM_SITE_VOID, SLIM_SITE_VOID_FILE)) return 1;
  return o_write_void(gff, st, p, size, label);
}

static void slim_install(void) {
  char m = '1';
  SceUID fd = sceIoOpen(DATA_PATH "/slim_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char b = 0; if (sceIoRead(fd, &b, 1) == 1) m = b; sceIoClose(fd); }
  if (m == '0') { log_printf("[gff] slim script situations off (slim_mode '0')"); return; }
  /* The two calls in SaveScriptSituation, as this build has them. */
  static const uint16_t at_int[] = {0xf4d6, 0xe980}, at_void[] = {0xf4e3, 0xeb48};
  static const uint16_t at_int_f[] = {0xf4d6, 0xe958}, at_void_f[] = {0xf4e3, 0xeb1c};
  uintptr_t ss = so_symbol(&kotor_mod, "_ZN23CVirtualMachineInternal19SaveScriptSituationEP21CVirtualMachineScriptP7CResGFFP10CResStruct");
  f_read_script = (read_script_t)so_symbol(&kotor_mod, "_ZN23CVirtualMachineInternal14ReadScriptFileEP10CExoString");
  f_delete_script = (delete_script_t)so_symbol(&kotor_mod, "_ZN23CVirtualMachineInternal12DeleteScriptEP21CVirtualMachineScript");
  if (ss != kotor_mod.text_base + 0x58a450u + 1 ||
      memcmp((const void *)(kotor_mod.text_base + SLIM_SITE_INT - 4), at_int, sizeof at_int) ||
      memcmp((const void *)(kotor_mod.text_base + SLIM_SITE_VOID - 4), at_void, sizeof at_void) ||
      memcmp((const void *)(kotor_mod.text_base + SLIM_SITE_INT_FILE - 4), at_int_f, sizeof at_int_f) ||
      memcmp((const void *)(kotor_mod.text_base + SLIM_SITE_VOID_FILE - 4), at_void_f, sizeof at_void_f) || !f_read_script ||
      !f_delete_script) {
    log_printf("[gff] slim script situations NOT armed: SaveScriptSituation is not the expected code");
    return;
  }
  o_write_int = (write_int_t)take_slots("_ZN7CResGFF13WriteFieldINTEP10CResStructiPc", (uintptr_t)h_write_int, NULL);
  o_write_void = (write_void_t)take_slots("_ZN7CResGFF14WriteFieldVOIDEP10CResStructPKvmPc", (uintptr_t)h_write_void, NULL);
  if (o_write_int && o_write_void)
    o_save_sit = (save_sit_t)take_slots("_ZN23CVirtualMachineInternal19SaveScriptSituationEP21CVirtualMachineScriptP7CResGFFP10CResStruct",
                                        (uintptr_t)h_save_sit, NULL);
  log_printf("[gff] slim script situations %s: a situation whose code is its script file is saved without the copy",
             o_save_sit ? "armed" : "NOT armed (slots taken)");
}

/* The engine's instructions around the growth, as this build has them: the
 * step load, the new[] call and the copy that follows (AddDataField+0x34). */
static int engine_matches(uintptr_t fn) {
  static const uint16_t expect[] = {0x6b60, 0x4448, 0x6620, 0xf4eb, 0xed96, 0x4605, 0x6c20};
  const uint16_t *code = (const uint16_t *)((fn & ~(uintptr_t)1) + 0x34);
  return fn == kotor_mod.text_base + 0x56d8e0u + 1 && !memcmp(code, expect, sizeof expect);
}

static int s_installed;

void gff_grow_install(void) {
  char m = '1';
  SceUID fd = sceIoOpen(DATA_PATH "/gff_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { char b = 0; if (sceIoRead(fd, &b, 1) == 1) m = b; sceIoClose(fd); }
  if (m == '0') { log_printf("[gff] off (gff_mode '0')"); return; }
  uintptr_t fn = so_symbol(&kotor_mod, "_ZN7CResGFF12AddDataFieldEmmm");
  if (!engine_matches(fn)) {
    log_printf("[gff] NOT armed: CResGFF::AddDataField is not the expected code");
    return;
  }
  unsigned n = 0;
  if (!take_slots("_ZN7CResGFF12AddDataFieldEmmm", (uintptr_t)h_add_data_field, &n)) {
    log_printf("[gff] NOT armed: CResGFF::AddDataField slots not found or taken");
    return;
  }
  s_installed = 1;
  log_printf("[gff] armed: CResGFF::AddDataField %u slots, steps of at least an eighth, new room zeroed", n);

  /* The reader: the engine's code as this build has it (prologue, and the
   * length check `adds r0,#4; cmp r5,r0; bcs`), and the functions it calls. */
  static const uint16_t expect_rd[] = {0xb5f0, 0xaf03, 0xe92d, 0x0bc0, 0xe9d7, 0x9802, 0x4604, 0xb322, 0xb31b};
  static const uint16_t expect_chk[] = {0x3004, 0x4285, 0xd209};
  uintptr_t rd = so_symbol(&kotor_mod, "_ZN7CResGFF19ReadFieldCExoStringEP10CResStructPcRiRK10CExoString");
  f_by_label = (by_label_t)so_symbol(&kotor_mod, "_ZN7CResGFF15GetFieldByLabelEP10CResStructPc");
  f_get_field = (get_field_t)so_symbol(&kotor_mod, "_ZN7CResGFF8GetFieldEP10CResStructm");
  f_get_data = (get_data_t)so_symbol(&kotor_mod, "_ZN7CResGFF12GetDataFieldEP12CResGFFFieldRm");
  f_str_len = (str_len_t)so_symbol(&kotor_mod, "_ZN10CExoStringC1EPKci");
  f_str_copy = (str_copy_t)so_symbol(&kotor_mod, "_ZN10CExoStringC1ERKS_");
  if (rd != kotor_mod.text_base + 0x56e214u + 1 || memcmp((const void *)(rd - 1), expect_rd, sizeof expect_rd) ||
      memcmp((const void *)(rd - 1 + 0x54), expect_chk, sizeof expect_chk) || !f_by_label || !f_get_field ||
      !f_get_data || !f_str_len || !f_str_copy) {
    log_printf("[gff] reader NOT armed: CResGFF::ReadFieldCExoString is not the expected code");
    return;
  }
  unsigned nr = 0;
  if (!take_slots("_ZN7CResGFF19ReadFieldCExoStringEP10CResStructPcRiRK10CExoString", (uintptr_t)h_read_cexostring,
                  &nr)) {
    log_printf("[gff] reader NOT armed: CResGFF::ReadFieldCExoString slots not found or taken");
    return;
  }
  log_printf("[gff] reader armed: CResGFF::ReadFieldCExoString %u slots, strings over %u KB read as empty", nr,
             GFF_STRING_MAX / 1024u);
  slim_install();
}

/* After a save: how many times the field data grew and how far. */
void gff_grow_report(const char *why) {
  if (!s_installed) return;
  if (w_str_fixed) {
    log_printf("[gff] %s%u strings read as empty (lengths up to %u KB, e.g. \"%s\"): written by a save "
               "that left empty strings' lengths unset", why ? why : "", (unsigned)w_str_fixed,
               (unsigned)(w_str_fixed_max >> 10), w_str_fixed_label);
    w_str_fixed = w_str_fixed_max = 0;
  }
  if (w_slim || w_full || w_lazy) {
    log_printf("[gff] %sscript situations: %u saved without their code (%u KB not written), %u held without code "
               "and saved so, %u with code (%u KB); %u scripts read to compare, %u situations differ from their file",
               why ? why : "", (unsigned)w_slim, (unsigned)(w_slim_bytes >> 10), (unsigned)w_lazy, (unsigned)w_full,
               (unsigned)(w_full_bytes >> 10), (unsigned)w_checked, (unsigned)w_differ);
    w_slim = w_full = w_checked = w_differ = w_lazy = 0;
    w_slim_bytes = w_full_bytes = 0;
  }
  if (!w_grows) return;
  log_printf("[gff] %s%u reallocations (%u by an eighth; %u through realloc, %u of them moved), "
             "largest buffer %u KB", why ? why : "", (unsigned)w_grows, (unsigned)w_big_grows,
             (unsigned)w_realloc, (unsigned)w_moved, (unsigned)w_max_kb);
  w_grows = w_big_grows = w_max_kb = w_realloc = w_moved = 0;
}
#endif

#else
void gff_grow_install(void) {}
void gff_grow_report(const char *why) { (void)why; }
#endif
