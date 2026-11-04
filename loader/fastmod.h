/* fastmod.h -- x % n by multiplication, for x < 2^16 and a divisor fixed over
 * many uses (proc_anim.c). The Cortex-A9 has no divide instruction; libgcc's
 * __aeabi_idivmod is a call and a loop. Checked against % for every x below
 * 2^16 and every n up to 70000 by tools/test_fastmod.c. */
#ifndef FASTMOD_H
#define FASTMOD_H

#include <stdint.h>

typedef struct { uint32_t n, m; } fastmod_t;

static inline fastmod_t fastmod_init(uint32_t n) {   /* n >= 1 */
  fastmod_t f = { n, n > 1 ? 0xffffffffu / n + 1 : 0 };
  return f;
}

static inline uint32_t fastmod(fastmod_t f, uint32_t x) {   /* x < 2^16 */
  if (f.n == 1) return 0;
  uint32_t q = (uint32_t)(((uint64_t)x * f.m) >> 32);
  int32_t r = (int32_t)(x - q * f.n);
  if (r < 0) r += (int32_t)f.n;
  else if ((uint32_t)r >= f.n) r -= (int32_t)f.n;
  return (uint32_t)r;
}

#endif
