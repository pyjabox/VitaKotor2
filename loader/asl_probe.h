/* asl_probe.h -- measure the engine's calls into ASLgl (asl_probe.c, test builds). */
#ifndef ASL_PROBE_H
#define ASL_PROBE_H

/* Takes over every ASLgl function slot. Call after every other installer. */
void asl_probe_install(void);

/* Once per frame, at the swap: an [aslprobe] report every 10 s. */
void asl_probe_on_swap(void);

#endif
