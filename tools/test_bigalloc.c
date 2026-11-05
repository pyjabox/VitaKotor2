/* test_bigalloc.c -- host test of loader/bigalloc.c: the pool, jumbo blocks
 * for requests larger than a segment, and handing empty segments back when a
 * jumbo block finds no room.
 *
 *   cc -O1 -g -fsanitize=address -DBIGALLOC_HOST_TEST -I loader -I tools/host \
 *      tools/test_bigalloc.c loader/bigalloc.c -o /tmp/tb && /tmp/tb */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bigalloc.h"

extern size_t bigalloc_host_budget, bigalloc_host_used;

void log_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}

static unsigned fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)
#define MB (1024u * 1024u)

int main(void) {
  bigalloc_init();
  CHECK(!bigalloc(100 * 1024), "small request served");

  /* Pool blocks. */
  void *a = bigalloc(3 * MB), *b = bigalloc(3 * MB);
  CHECK(a && b && bigalloc_owns(a) && bigalloc_owns(b), "pool blocks");
  memset(a, 1, 3 * MB);
  memset(b, 2, 3 * MB);

  /* A jumbo block: larger than a segment, owned, sized, freed. */
  void *j = bigalloc(16 * MB + 12345);
  CHECK(j && bigalloc_owns(j), "jumbo block");
  CHECK(bigalloc_size(j) == 16 * MB + 12345, "jumbo size %zu", bigalloc_size(j));
  CHECK(!bigalloc_owns((char *)j + 17 * MB), "past the jumbo block");
  memset(j, 3, 16 * MB + 12345);
  bigfree(j);
  CHECK(!bigalloc_owns(j), "jumbo freed but still owned");

  /* The GFF writer's pattern: grow by realloc from 1 MB to 24 MB, check data. */
  size_t n = 1 * MB;
  unsigned char *g = big_malloc(n);
  for (size_t i = 0; i < n; i++) g[i] = (unsigned char)(i * 7);
  while (n < 24 * MB) {
    size_t m = n + n / 8;
    g = big_realloc(g, m);
    CHECK(g, "realloc to %zu", m);
    for (size_t i = n; i < m; i++) g[i] = (unsigned char)(i * 7);
    n = m;
  }
  int bad = 0;
  for (size_t i = 0; i < n; i++) bad |= g[i] != (unsigned char)(i * 7);
  CHECK(!bad, "data after growth");
  CHECK(bigalloc_owns(g), "grown block is a jumbo block");
  big_free(g);

  /* No room left in the system: a jumbo block hands empty segments back. */
  bigfree(a);
  bigfree(b);                     /* the first segment is now wholly free */
  void *c = bigalloc(6 * MB);     /* lands in that segment again */
  CHECK(c, "pool block after frees");
  bigfree(c);
  bigalloc_host_budget = bigalloc_host_used + 10 * MB;   /* 10 MB of system left */
  void *k = bigalloc(17 * MB);    /* needs the empty segment's 8 MB back */
  CHECK(k && bigalloc_owns(k), "jumbo after handing a segment back (used %zu MB)", bigalloc_host_used / MB);
  void *p = bigalloc(2 * MB);     /* the pool takes a segment again if it can */
  CHECK(!p || bigalloc_owns(p), "pool after release");
  if (p) bigfree(p);
  void *x = bigalloc(30 * MB);    /* nothing left: NULL, the caller falls back */
  CHECK(!x, "jumbo beyond the budget");
  bigfree(k);
  bigalloc_host_budget = (size_t)-1;

  /* Pointers that are not ours. */
  void *m = malloc(64);
  CHECK(!bigalloc_owns(m), "malloc pointer owned");
  free(m);

  bigalloc_log("end: ");
  printf("%u failures\n", fails);
  return fails != 0;
}
