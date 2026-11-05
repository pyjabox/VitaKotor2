/* battle_ledger.h -- BATTLE_LEDGER (test builds): where a busy fight's frame
 * time goes, measured with timers rather than PC samples (battle_ledger.c). */
#ifndef BATTLE_LEDGER_H
#define BATTLE_LEDGER_H
/* Takes its relocation slots; call with the other installers in main(),
 * before the call rewrite (plt_direct) runs. */
void battle_ledger_install(void);
/* Logs and resets the [ledger] lines; called at the end of each of the AI
 * thread's 10 s windows with that window's frame count. */
void battle_ledger_report(unsigned frames);
/* SCENE_TRACE: an AI event fired (ai_thread.c's GetPendingEvent probe). */
#include <stdint.h>
void battle_ledger_trace_event(uint32_t type, uint32_t caller, uint32_t target);
/* SCENE_TRACE: 1 while the scene trace runs; a gated call and its fate. */
int battle_ledger_tracing(void);
void battle_ledger_trace_gate(const char *fn, uintptr_t ret, const char *what, void *arg0, int dialog_action);
#endif
