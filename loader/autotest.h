/* autotest.h -- scripted controller input for unattended Vita3K smoke tests.
 *
 * Built only with -DKOTOR_AUTOTEST=ON (defines AUTOTEST_ENABLE=1 and links
 * with --wrap=sceCtrlPeekBufferPositive2). Production builds compile these to
 * empty inlines. See autotest.c for the script. */

#ifndef __AUTOTEST_H__
#define __AUTOTEST_H__

#include <stdint.h>

#ifndef AUTOTEST_ENABLE
#define AUTOTEST_ENABLE 0
#endif

#if AUTOTEST_ENABLE
/* fopen wrapper: lets the script see which GUI/module the game is opening. */
void autotest_note_open(const char *path);
/* SDL swap hook, game thread. */
void autotest_on_swap(uint64_t now_us);
#else
static inline void autotest_note_open(const char *path) { (void)path; }
static inline void autotest_on_swap(uint64_t now_us) { (void)now_us; }
#endif

#endif
