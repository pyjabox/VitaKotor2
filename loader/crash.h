/* crash.h -- hardware CPU-fault capture to log.txt (see crash.c) */

#ifndef __CRASH_H__
#define __CRASH_H__

// Register a kubridge exception handler for data/prefetch-abort and undefined
// instruction faults. On a fault the handler dumps registers + the faulting
// PC/LR resolved against the loaded module bases to log.txt, then parks.
// Wire this BEFORE handing off to the game so any fault self-reports.
void crash_init(void);

#endif
