/* ai_budget.c -- the server AI master's time budget per frame.
 *
 * CServerAIMaster::UpdateState updates AI objects until a fixed budget is
 * spent: `movw r4, #10000` (microseconds, CExoTimers) at libkotor2+0x4a7e00,
 * shared out over its five AI levels. Objects it does not reach this frame are
 * updated on a later one. On the Vita the master used all 10 ms each frame,
 * though the AIUpdates themselves took ~2.5-3 ms; the rest is per-object loop
 * work (two or three high-resolution timer reads, ~17 object lookups, events).
 *
 * Hardware, 2026-10-04 (real-vita-fixab-20261004), busy scenes:
 *   budget 10 ms: master 7.0-9.2 ms/frame, ~250 objects (every one) per frame
 *   budget  5 ms: 5.3-6.4 ms, ~65% of objects per frame
 *   budget  3 ms: 3.4-5.3 ms, ~35% (creatures every ~3 frames)
 * The user played the 3 ms windows and noticed nothing; AI_BUDGET_US picks it.
 *
 * The instruction is checked before it is rewritten; any other bytes leave the
 * engine alone. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_budget.h"

#define AI_BUDGET_OFF 0x4a7e00u
static const uint8_t k_engine_insn[] = {0x42, 0xf2, 0x10, 0x74};   /* movw r4, #10000 */

/* Thumb-2 MOVW r4, #imm16 (encoding T3). */
static void movw_r4(uint8_t out[4], uint16_t imm) {
  uint16_t hw1 = 0xf240 | ((imm >> 11) & 1) << 10 | (imm >> 12);
  uint16_t hw2 = ((imm >> 8) & 7) << 12 | 4 << 8 | (imm & 0xff);
  out[0] = hw1 & 0xff; out[1] = hw1 >> 8;
  out[2] = hw2 & 0xff; out[3] = hw2 >> 8;
}

void ai_budget_install(void) {
  if (!AI_BUDGET_US || AI_BUDGET_US == 10000) return;
  uint8_t *at = (uint8_t *)(kotor_mod.text_base + AI_BUDGET_OFF);
  if (memcmp(at, k_engine_insn, sizeof k_engine_insn)) {
    log_printf("[aibudget] NOT applied: unexpected bytes at +0x%x", AI_BUDGET_OFF);
    return;
  }
  uint8_t insn[4];
  movw_r4(insn, AI_BUDGET_US);
  kuKernelCpuUnrestrictedMemcpy(at, insn, sizeof insn);
  kuKernelFlushCaches(at, sizeof insn);
  log_printf("[aibudget] server AI budget 10000 -> %u us per frame", (unsigned)AI_BUDGET_US);
}
