#ifndef MINE_TRACE_H
#define MINE_TRACE_H

/* Diagnostics (MINE_TRACE): log each mine a creature enters, when the engine
 * handles it and whether it may fire. See mine_trace.c. */
void mine_trace_install(void);
void mine_trace_on_swap(void);

#endif
