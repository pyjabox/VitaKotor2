/* test_fastmod.c -- loader/fastmod.h against %, exhaustively. Not built into the VPK.
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -I loader -o /tmp/test_fastmod tools/test_fastmod.c && /tmp/test_fastmod */
#include <stdio.h>
#include "fastmod.h"
int main(void) {
  unsigned long long bad = 0, n_tested = 0;
  for (uint32_t n = 1; n <= 70000; n++) {
    fastmod_t f = fastmod_init(n);
    for (uint32_t x = 0; x < 65536; x++) {
      n_tested++;
      if (fastmod(f, x) != x % n) { if (bad++ < 5) printf("FAIL x=%u n=%u: %u vs %u\n", x, n, fastmod(f, x), x % n); }
    }
  }
  printf("%llu cases, %llu wrong: %s\n", n_tested, bad, bad ? "FAILED" : "OK");
  return bad != 0;
}
