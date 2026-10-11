/* ai_budget.h -- server AI master time budget (ai_budget.c). */

#ifndef __AI_BUDGET_H__
#define __AI_BUDGET_H__

/* After the engine is loaded and relocated, before the game thread starts. */
void ai_budget_install(void);
/* The same with another budget, in microseconds (10000: the engine's own). */
void ai_budget_install_us(unsigned us);

#endif
