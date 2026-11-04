/* test_plt_scan.c -- host test for loader/plt_scan.c against the real
 * libkotor2.so. Not built into the VPK.
 *
 * Build and run:
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -I loader -o /tmp/test_plt_scan \
 *       tools/test_plt_scan.c loader/plt_scan.c \
 *   && /tmp/test_plt_scan path/to/libkotor2.so [sites.txt]
 *
 * Checks every PLT entry decodes to its own .rel.plt slot, re-encodes every
 * site found and gets the original bytes back, encodes the direct branch each
 * site would get and decodes it back to the function. Prints the counts and
 * the hash plt_direct.c expects; with sites.txt, writes "site idx kind" lines
 * for the targets defined in the library (what plt_direct.c may patch), to be
 * compared with the list objdump gives. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../loader/elf.h"
#include "plt_scan.h"

static uint8_t *g_img;
static Elf32_Rel *g_relplt;
static Elf32_Sym *g_dynsym;
static uint32_t g_nplt, g_plt_first;
static unsigned g_fail;
static const plt_scan_ctx *g_ctx;
static FILE *g_out, *g_all;  /* PLT_ALL=file: every site, with a defined-here column */
static uint32_t g_hash = PLT_HASH_INIT, g_n[2], g_def[2], g_arm;

static uint16_t rd16(uint32_t a) { return (uint16_t)(g_img[a] | g_img[a + 1] << 8); }

static void check(int ok, const char *what, uint32_t site) {
  if (!ok) { if (g_fail++ < 20) fprintf(stderr, "FAIL %s at %x\n", what, site); }
}

static void on_site(void *u, uint32_t site, uint32_t idx, int kind) {
  (void)u;
  uint16_t back[2];
  uint32_t entry = g_plt_first + 12 * idx;
  if (kind == PLT_SITE_CALL) {
    check(thumb_enc_blx(site, entry, back) && back[0] == rd16(site) && back[1] == rd16(site + 2), "blx re-encode", site);
  }
  g_n[kind]++;
  {
    uint32_t i2; int k2;
    check(plt_site_at(g_ctx, site, &i2, &k2) && i2 == idx && k2 == kind, "plt_site_at", site);
  }
  Elf32_Sym *s = &g_dynsym[ELF32_R_SYM(g_relplt[idx].r_info)];
  if (g_all) fprintf(g_all, "%x %u %d %d\n", site, idx, kind, s->st_shndx != SHN_UNDEF && ELF32_ST_TYPE(s->st_info) == STT_FUNC);
  if (s->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(s->st_info) != STT_FUNC || !s->st_value) return;
  uint32_t def = s->st_value;
  uint16_t ins[2];
  int ok;
  if (def & 1) ok = kind == PLT_SITE_CALL ? thumb_enc_bl(site, def, ins) : thumb_enc_bw(site, def, ins);
  else { ok = kind == PLT_SITE_CALL && thumb_enc_blx(site, def, ins); if (ok) g_arm++; }
  if (kind == PLT_SITE_TAIL && !(def & 1)) return;   /* plt_direct.c leaves these alone */
  check(ok, "direct encode", site);
  /* Decode what we would write: it must land on the function. */
  uint32_t s1 = (ins[0] >> 10) & 1, j1 = (ins[1] >> 13) & 1, j2 = (ins[1] >> 11) & 1;
  uint32_t i1 = !(j1 ^ s1), i2 = !(j2 ^ s1);
  uint32_t v = s1 << 24 | i1 << 23 | i2 << 22 | (uint32_t)(ins[0] & 0x3ff) << 12 | (uint32_t)(ins[1] & 0x7ff) << 1;
  int32_t off = (int32_t)(v << 7) >> 7;
  uint32_t t = (def & 1) ? site + 4 + (uint32_t)off : ((site + 4) & ~3u) + (uint32_t)(off & ~3);
  check(t == (def & ~1u), "direct decode", site);
  g_def[kind]++;
  g_hash = plt_hash_step(g_hash, site, idx, kind);
  if (g_out) fprintf(g_out, "%x %u %d\n", site, idx, kind);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s libkotor2.so [sites.txt]\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  g_img = malloc((size_t)n);
  if (!g_img || fread(g_img, 1, (size_t)n, f) != (size_t)n) return 2;
  fclose(f);
  Elf32_Ehdr *eh = (Elf32_Ehdr *)g_img;
  Elf32_Shdr *sh = (Elf32_Shdr *)(g_img + eh->e_shoff);
  const char *shstr = (const char *)g_img + sh[eh->e_shstrndx].sh_offset;
  Elf32_Shdr *plt = 0, *text = 0, *relplt = 0, *dynsym = 0;
  for (int i = 0; i < eh->e_shnum; i++) {
    const char *nm = shstr + sh[i].sh_name;
    if (sh[i].sh_addr != sh[i].sh_offset && sh[i].sh_type != SHT_NOBITS && sh[i].sh_addr < 0x700000)
      fprintf(stderr, "note: %s address %x != offset %x\n", nm, sh[i].sh_addr, sh[i].sh_offset);
    if (!strcmp(nm, ".plt")) plt = &sh[i];
    else if (!strcmp(nm, ".text")) text = &sh[i];
    else if (!strcmp(nm, ".rel.plt")) relplt = &sh[i];
    else if (!strcmp(nm, ".dynsym")) dynsym = &sh[i];
  }
  if (!plt || !text || !relplt || !dynsym) { fprintf(stderr, "sections missing\n"); return 2; }
  g_relplt = (Elf32_Rel *)(g_img + relplt->sh_offset);
  g_dynsym = (Elf32_Sym *)(g_img + dynsym->sh_offset);
  g_nplt = relplt->sh_size / sizeof(Elf32_Rel);
  g_plt_first = plt->sh_addr + 20;
  if (plt->sh_size != 20 + 12 * g_nplt) { fprintf(stderr, "PLT size %x for %u entries\n", plt->sh_size, g_nplt); return 1; }
  for (uint32_t i = 0; i < g_nplt; i++)
    check(plt_entry_got(g_img, g_plt_first + 12 * i) == g_relplt[i].r_offset, "PLT entry -> its .rel.plt slot", g_plt_first + 12 * i);
  if (argc > 2) g_out = fopen(argv[2], "w");
  if (getenv("PLT_ALL")) g_all = fopen(getenv("PLT_ALL"), "w");
  /* The sweep stops at the first ARM function (libgcc's, at the end). */
  uint32_t arm_lo = text->sh_addr + text->sh_size;
  Elf32_Sym *sy = g_dynsym;
  for (uint32_t i = 0; i < dynsym->sh_size / sizeof(Elf32_Sym); i++)
    if (ELF32_ST_TYPE(sy[i].st_info) == STT_FUNC && sy[i].st_shndx != SHN_UNDEF && !(sy[i].st_value & 1) &&
        sy[i].st_value >= text->sh_addr && sy[i].st_value < arm_lo)
      arm_lo = sy[i].st_value;
  plt_scan_ctx c = { g_img, g_plt_first, g_nplt, text->sh_addr, arm_lo };
  g_ctx = &c;
  uint32_t total = plt_scan(&c, on_site, 0);
  if (g_out) fclose(g_out);
  if (g_all) fclose(g_all);
  printf("plt %x+%x (%u entries), text %x-%x\n", plt->sh_addr, plt->sh_size, g_nplt, c.text_lo, c.text_hi);
  printf("sites: %u (blx %u, b.w via veneer %u); to functions defined here: blx %u (%u to ARM code), b.w %u\n",
         total, g_n[0], g_n[1], g_def[0], g_arm, g_def[1]);
  printf("hash 0x%08x\n", g_hash);
  printf("%s (%u failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
