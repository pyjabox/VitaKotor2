/* plt_scan.c -- see plt_scan.h. */
#include "plt_scan.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t arm_imm(uint32_t w) {          /* data-processing modified immediate */
  uint32_t rot = ((w >> 8) & 0xf) * 2, v = w & 0xff;
  return rot ? (v >> rot) | (v << (32 - rot)) : v;
}

uint32_t plt_entry_got(const uint8_t *img, uint32_t e) {
  uint32_t a = rd32(img + e), b = rd32(img + e + 4), c = rd32(img + e + 8);
  if ((a & 0xfffff000u) != 0xe28fc000u) return 0;   /* add ip, pc, #x  */
  if ((b & 0xfffff000u) != 0xe28cc000u) return 0;   /* add ip, ip, #y  */
  if ((c & 0xfffff000u) != 0xe5bcf000u) return 0;   /* ldr pc, [ip, #z]! */
  return e + 8 + arm_imm(a) + arm_imm(b) + (c & 0xfff);
}

uint32_t plt_veneer_target(const uint8_t *img, uint32_t v) {
  if (v & 3) return 0;
  if (rd16(img + v) != 0x4778 || rd16(img + v + 2) != 0x46c0) return 0;   /* bx pc; nop */
  if (rd32(img + v + 4) != 0xe59fc000u || rd32(img + v + 8) != 0xe08cf00fu) return 0;
  return v + 16 + rd32(img + v + 12);           /* add pc, ip, pc: pc reads v+8+8 */
}

/* Branch offset of a 32-bit Thumb B.W/BL/BLX: S:I1:I2:imm10:imm11:0. */
static int32_t t32_off(uint16_t h1, uint16_t h2) {
  uint32_t s = (h1 >> 10) & 1, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1;
  uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
  uint32_t v = s << 24 | i1 << 23 | i2 << 22 | (uint32_t)(h1 & 0x3ff) << 12 | (uint32_t)(h2 & 0x7ff) << 1;
  return (int32_t)(v << 7) >> 7;
}

static int plt_index(const plt_scan_ctx *c, uint32_t t, uint32_t *idx) {
  if (t < c->plt_first) return 0;
  uint32_t d = t - c->plt_first;
  if (d % 12 || d / 12 >= c->nplt) return 0;
  *idx = d / 12;
  return 1;
}

/* The instruction at p: its size, and whether it is one of our sites. */
static int decode_at(const plt_scan_ctx *c, uint32_t p, uint32_t *idx, int *kind) {
  uint16_t h1 = rd16(c->img + p);
  if ((h1 & 0xf800) < 0xe800) return 2;                 /* 16-bit instruction */
  if (p + 4 > c->text_hi) return 0;
  uint16_t h2 = rd16(c->img + p + 2);
  *kind = -1;
  if ((h1 & 0xf800) == 0xf000) {
    if ((h2 & 0xd001) == 0xc000) {                       /* BLX imm: to ARM */
      uint32_t t = ((p + 4) & ~3u) + (uint32_t)(t32_off(h1, h2) & ~3);
      if (plt_index(c, t, idx)) *kind = PLT_SITE_CALL;
    } else if ((h2 & 0xd000) == 0x9000) {                /* B.W (T4) */
      uint32_t t = p + 4 + (uint32_t)t32_off(h1, h2);
      if (t >= c->text_lo && t + 16 <= c->text_hi) {
        uint32_t e = plt_veneer_target(c->img, t);
        if (e && plt_index(c, e, idx)) *kind = PLT_SITE_TAIL;
      }
    }
  }
  return 4;
}

uint32_t plt_scan(const plt_scan_ctx *c, plt_site_cb cb, void *user) {
  uint32_t n = 0;
  for (uint32_t p = c->text_lo; p + 2 <= c->text_hi;) {
    /* Veneers are Thumb, then ARM words: step over them whole, also when
     * the sweep comes in on the wrong halfword. */
    uint32_t v = p & ~3u;
    if (v >= c->text_lo && v + 16 <= c->text_hi && rd16(c->img + v) == 0x4778 && plt_veneer_target(c->img, v)) {
      p = v + 16;
      continue;
    }
    uint32_t idx;
    int kind = -1;
    int sz = decode_at(c, p, &idx, &kind);
    if (!sz) break;
    if (sz == 4 && kind >= 0) { cb(user, p, idx, kind); n++; }
    p += (uint32_t)sz;
  }
  return n;
}

int plt_site_at(const plt_scan_ctx *c, uint32_t p, uint32_t *idx, int *kind) {
  *kind = -1;
  return p >= c->text_lo && p + 4 <= c->text_hi && decode_at(c, p, idx, kind) == 4 && *kind >= 0;
}

static int enc(uint32_t from, uint32_t to, uint16_t base2, int blx, uint16_t out[2]) {
  int32_t off = blx ? (int32_t)(to - ((from + 4) & ~3u)) : (int32_t)((to & ~1u) - (from + 4));
  if (off < -16777216 || off > 16777214) return 0;
  if (blx ? (to & 3) : (off & 1)) return 0;
  uint32_t u = (uint32_t)off;
  uint32_t s = (u >> 24) & 1, i1 = (u >> 23) & 1, i2 = (u >> 22) & 1;
  uint32_t j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
  out[0] = (uint16_t)(0xf000 | s << 10 | ((u >> 12) & 0x3ff));
  out[1] = (uint16_t)(base2 | j1 << 13 | j2 << 11 | (blx ? ((u >> 1) & 0x7fe) : ((u >> 1) & 0x7ff)));
  return 1;
}

int thumb_enc_bl(uint32_t from, uint32_t to, uint16_t out[2]) { return enc(from, to, 0xd000, 0, out); }
int thumb_enc_bw(uint32_t from, uint32_t to, uint16_t out[2]) { return enc(from, to, 0x9000, 0, out); }
int thumb_enc_blx(uint32_t from, uint32_t to, uint16_t out[2]) { return enc(from, to, 0xc000, 1, out); }
