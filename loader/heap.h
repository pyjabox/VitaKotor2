/* heap.h -- newlib heap telemetry and exhaustion handling (see heap.c) */

#ifndef __HEAP_H__
#define __HEAP_H__

#include <stddef.h>

// Arm the C++ new-handler. Call once, early.
void heap_init(void);

// Log one line of heap occupancy. `why` labels the line; NULL for the periodic
// sample. Cheap: mallinfo only.
void heap_log(const char *why);

// The same, plus the largest block the heap can still serve and a census of
// large allocations. Probes with malloc, so this is for the occasional sample
// and for failures, not every tick.
void heap_log_full(const char *why);

// Record an allocation for the large-request census. Ignores anything under
// 256 KB; callers need not filter.
void heap_note_alloc(unsigned n);

// Size of the most recent `new` the game made through our dynlib table, so the
// handler can say whether the failing request was large or the heap was simply
// full. Written on every new, read only when one fails.
extern volatile unsigned g_heap_last_new;

#endif
