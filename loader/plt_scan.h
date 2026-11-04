/* plt_scan.h -- find the engine's calls that go through its own PLT
 * (plt_direct.c). Pure C on a memory image of libkotor2, so the host test
 * (tools/test_plt_scan.c) runs exactly what the Vita runs. */
#ifndef PLT_SCAN_H
#define PLT_SCAN_H

#include <stdint.h>

/* img[v] is the byte at module address v (file offset = address for the
 * text segment of libkotor2, and the load base on the Vita). */
typedef struct {
  const uint8_t *img;
  uint32_t plt_first;    /* address of PLT entry 0 (after the 20-byte PLT0) */
  uint32_t nplt;         /* entries, = .rel.plt entries */
  uint32_t text_lo, text_hi;   /* Thumb code to sweep: libkotor2's .text up to its
                                 * first ARM function (libgcc), veneers skipped */
} plt_scan_ctx;

enum { PLT_SITE_CALL = 0,   /* blx <plt entry>        -> bl/blx <function> */
       PLT_SITE_TAIL = 1 }; /* b.w <thumb->arm veneer> -> b.w <function>  */

/* Address of the GOT slot PLT entry e loads, or 0 if e is not the standard
 * three-instruction ARM stub (add ip, pc / add ip, ip / ldr pc, [ip]!). */
uint32_t plt_entry_got(const uint8_t *img, uint32_t e);

/* PLT entry a veneer at v jumps to (bx pc; nop; ldr ip, [pc]; add pc, ip, pc;
 * .word), or 0 if v is not such a veneer. */
uint32_t plt_veneer_target(const uint8_t *img, uint32_t v);

/* Linear Thumb-2 sweep of [text_lo, text_hi), as objdump -M force-thumb does,
 * stepping over veneers. Calls cb for
 * every blx to a PLT entry and every b.w to a veneer for one; idx is the
 * entry (the .rel.plt index). Returns the number of sites. */
typedef void (*plt_site_cb)(void *user, uint32_t site, uint32_t idx, int kind);
uint32_t plt_scan(const plt_scan_ctx *c, plt_site_cb cb, void *user);

/* Whether the instruction at p is (still) such a site, and which. */
int plt_site_at(const plt_scan_ctx *c, uint32_t p, uint32_t *idx, int *kind);

/* Thumb-2 encoders; 0 if the offset is out of range or misaligned.
 * bl/b.w: to is a Thumb address (bit 0 ignored). blx: to is ARM, 4-aligned. */
int thumb_enc_bl(uint32_t from, uint32_t to, uint16_t out[2]);
int thumb_enc_bw(uint32_t from, uint32_t to, uint16_t out[2]);
int thumb_enc_blx(uint32_t from, uint32_t to, uint16_t out[2]);

/* FNV-1a over the site list, to check a scan against the one verified on
 * the host. */
static inline uint32_t plt_hash_step(uint32_t h, uint32_t site, uint32_t idx, int kind) {
  uint32_t w[3] = { site, idx, (uint32_t)kind };
  for (int i = 0; i < 3; i++)
    for (int b = 0; b < 4; b++) { h ^= (w[i] >> (8 * b)) & 0xffu; h *= 16777619u; }
  return h;
}
#define PLT_HASH_INIT 2166136261u

#endif
