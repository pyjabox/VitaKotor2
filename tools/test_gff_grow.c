/* test_gff_grow.c -- host test: gff_grow.c's CResGFF::AddDataField against the
 * engine's, transcribed separately from the disassembly (libkotor2 0x56d8e0).
 *
 *   cc -O1 -g -fsanitize=address -I loader tools/test_gff_grow.c -o /tmp/tg && /tmp/tg
 *
 * Both run the same random sequence of field writes on their own fake CResGFF:
 * new fields appended, the last field resized in place, earlier fields moved
 * to the end. After every call the returned offset, the header's data size
 * and the used bytes must agree; only the capacity and the number of
 * reallocations may differ. */
#define GFF_GROW_HOST_TEST 1
#define GFF_GROW 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 32-bit handles for the engine object's pointer fields. */
static void *h_tab[64];
static void *host_ptr_get(uint32_t h) { return h ? h_tab[h] : NULL; }
static uint32_t host_ptr_put(void *p) {
  for (uint32_t i = 1; i < 64; i++) if (h_tab[i] == p) return i;
  for (uint32_t i = 1; i < 64; i++) if (!h_tab[i]) { h_tab[i] = p; return i; }
  abort();
}
static void host_ptr_drop(void *p) { for (uint32_t i = 1; i < 64; i++) if (h_tab[i] == p) h_tab[i] = NULL; }
/* A field's handle now names v (the old pointer may already be gone: realloc). */
static void host_ptr_set(uint32_t *field, void *v) {
  if (*field) h_tab[*field] = v;
  else *field = host_ptr_put(v);
}
static int bigalloc_owns(const void *p) { (void)p; return 0; }
static unsigned n_new;
static void *k2_new_array(size_t n) { n_new++; return malloc(n); }
static void k2_delete_array(void *p) { host_ptr_drop(p); free(p); }

#include "../loader/gff_grow.c"

/* The engine's, from the disassembly: header +0x24 data size, +0x34 step,
 * +0x40 header, +0x5c buffer, +0x60 capacity. */
static unsigned n_new_ref;
#define BUF(g) ((uint8_t *)host_ptr_get(*(uint32_t *)((g) + 0x5c)))
static uint8_t *ref_add_data_field(uint8_t *gff, uint32_t off, uint32_t old_len, uint32_t new_len) {
  uint8_t *hdr = host_ptr_get(*(uint32_t *)(gff + 0x40));
  if (!hdr || !BUF(gff)) return NULL;
  uint32_t r6 = *(uint32_t *)(hdr + 0x24);
  if (r6 == old_len + off) r6 = off;
  uint32_t r9 = r6 + new_len;
  if (r9 >= *(uint32_t *)(gff + 0x60)) {
    uint32_t cap = *(uint32_t *)(gff + 0x34) + r9;
    *(uint32_t *)(gff + 0x60) = cap;
    uint8_t *nb = malloc(cap);
    n_new_ref++;
    uint8_t *old = BUF(gff);
    memcpy(nb, old, *(uint32_t *)(hdr + 0x24));
    host_ptr_drop(old);
    free(old);
    *(uint32_t *)(gff + 0x5c) = host_ptr_put(nb);
  }
  if (r6 != off) memcpy(BUF(gff) + r6, BUF(gff) + off, old_len);
  *(uint32_t *)(hdr + 0x24) = r9;
  return BUF(gff) + r6;
}

typedef struct { uint8_t obj[0x80]; uint8_t hdr[0x40]; } fake_t;
static void fake_init(fake_t *f, uint32_t step) {
  memset(f, 0, sizeof *f);
  *(uint32_t *)(f->obj + 0x40) = host_ptr_put(f->hdr);
  *(uint32_t *)(f->obj + 0x34) = step;
  *(uint32_t *)(f->obj + 0x60) = step;
  *(uint32_t *)(f->obj + 0x5c) = host_ptr_put(malloc(step));
}

int main(void) {
  srand(12345);
  unsigned fails = 0, calls = 0;
  for (int run = 0; run < 40; run++) {
    fake_t a, b;
    uint32_t step = run & 1 ? 121856u : 4096u;
    fake_init(&a, step);
    fake_init(&b, step);
    struct { uint32_t off, len; } fields[512];
    int nf = 0;
    uint32_t target = run < 20 ? (1u << 20) : (24u << 20);
    while (*(uint32_t *)(a.hdr + 0x24) < target) {
      uint32_t off, old_len, len;
      int pick = nf ? rand() % (nf + 4) : nf;
      if (pick >= nf || nf == 0) {            /* a new field */
        off = 0; old_len = 0;
      } else {                                /* resize an existing field */
        off = fields[pick].off; old_len = fields[pick].len;
      }
      len = (uint32_t)(rand() % 3 == 0 ? rand() % 200000 : rand() % 64);
      /* A field moved to the end never shrinks here: the engine's own copy
       * would run past its buffer (see gff_grow.c). */
      if (pick < nf && nf && fields[pick].off + fields[pick].len != *(uint32_t *)(a.hdr + 0x24) && len < old_len)
        len = old_len;
      uint32_t used_before = *(uint32_t *)(a.hdr + 0x24);
      int in_place = pick < nf && nf && used_before == off + old_len;
      uint8_t *pa = h_add_data_field(a.obj, off, old_len, len);
      uint8_t *pb = ref_add_data_field(b.obj, off, old_len, len);
      calls++;
      uint32_t oa = (uint32_t)(pa - BUF(a.obj));
      uint32_t ob = (uint32_t)(pb - BUF(b.obj));
      uint32_t ua = *(uint32_t *)(a.hdr + 0x24), ub = *(uint32_t *)(b.hdr + 0x24);
      if (oa != ob || ua != ub) { fails++; printf("run %d: offset %u/%u size %u/%u\n", run, oa, ob, ua, ub); break; }
      /* The new room (past what the field kept) starts zeroed. */
      uint32_t kept = (pick < nf && nf) ? (old_len < len ? old_len : len) : 0;
      (void)in_place;
      for (uint32_t i = kept; i < len; i++)
        if (pa[i]) { fails++; printf("run %d: byte %u of new room not zero\n", run, i); break; }
      /* The caller writes the field's new bytes. */
      for (uint32_t i = 0; i < len; i++) pa[i] = pb[i] = (uint8_t)(rand());
      if (pick < nf && nf) { fields[pick].off = oa; fields[pick].len = len; }
      else if (nf < 512) { fields[nf].off = oa; fields[nf].len = len; nf++; }
      if (memcmp(BUF(a.obj), BUF(b.obj), ua)) {
        fails++; printf("run %d: contents differ\n", run); break;
      }
    }
    if (run == 39 || run == 38)
      printf("run %d: %u KB of field data, %u reallocations here against %u in the engine's\n", run,
             *(uint32_t *)(a.hdr + 0x24) >> 10, n_new, n_new_ref);
    n_new = n_new_ref = 0;
    { void *x = BUF(a.obj), *y = BUF(b.obj); host_ptr_drop(x); host_ptr_drop(y); free(x); free(y); }
    host_ptr_drop(a.hdr); host_ptr_drop(b.hdr);
  }
  printf("%u calls, %u failures\n", calls, fails);
  return fails != 0;
}
