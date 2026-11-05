/* big_trace.h -- large allocations logged with their callers (big_trace.c). */
#ifndef BIG_TRACE_H
#define BIG_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"

#if BIG_TRACE
void big_trace_alloc(const char *kind, size_t n, void *p, int pool, uint32_t caller);
void big_trace_free(void *p);
#define BIG_TRACE_ALLOC(kind, n, p, pool) \
  do { if ((n) >= (size_t)BIG_TRACE_MIN_KB * 1024u) \
         big_trace_alloc(kind, n, p, pool, (uint32_t)(uintptr_t)__builtin_return_address(0)); } while (0)
#define BIG_TRACE_FREE(p) big_trace_free(p)
#else
#define BIG_TRACE_ALLOC(kind, n, p, pool) ((void)0)
#define BIG_TRACE_FREE(p) ((void)0)
#endif

#endif
