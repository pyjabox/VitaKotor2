/* bloom_ctl.h -- runtime switch for the mobile bloom chain (bloom_ctl.c). */

#ifndef __BLOOM_CTL_H__
#define __BLOOM_CTL_H__

#include <stdint.h>

void bloom_ctl_install(void);
void bloom_ctl_set_off(int off);
/* Bloom draws skipped since the last call. */
uint32_t bloom_ctl_skipped(void);

#endif
