/* mine_trace.c -- what happens when a creature walks onto a mine (MINE_TRACE, diagnostics).
 *
 * Hardware, Khoonda battle (TSLRCM), 2026-10-17: enemies walked over the mines
 * the player set and none went off, while the AI was starved (2-3 creatures
 * of level 4 took 25-70 ms a frame).
 *
 * The engine's path, libkotor2 2.0.4:
 *  - Movement checks the whole step, not only where it ends:
 *    CSWSCreature::UpdateSubareasOnMoveTo -> CSWSArea::IntersectingLineSegment
 *    and InSubAreas -> HandleSubAreaStateChanges, which queues a signal event
 *    (CServerAIMaster::AddEventDeltaTime, EVENT_SIGNAL_EVENT, CScriptEvent of
 *    type 0xc "entered") on the trigger.
 *  - CSWSTrigger::EventHandler, when the queue delivers it: stores the
 *    creature at trigger+0x2dc; not a transition (+0x2f4 == 0) and a mine
 *    (+0x2fc != 0) -> OnEnterMine(GetInteger(0)).
 *  - OnEnterMine -> GetCanFireMineOnObject (mine owner +0x328, its faction and
 *    reputation, trap immunity, feats; the creature's position is NOT looked
 *    at again) -> the mine's scripts and effects.
 * So a late event still fires; a mine that never fires had no event queued,
 * or was refused. This logs each step:
 *   [mine] queued   trigger, creature, the event's delay, the thread
 *   [mine] handled  trigger, creature, ms since queued, can fire (0/1)
 * and a [mine] summary line every 10 s with counts. */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "mine_trace.h"

#if MINE_TRACE

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static uintptr_t take_slots(const char *sym, uintptr_t repl, unsigned *n_out) {
  uintptr_t original = so_symbol(&kotor_mod, sym), callable = 0;
  unsigned n = 0, bad = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] : &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *s = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + s->st_name, sym)) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) { if (!pass) bad++; continue; }
      if (pass == 0) { n++; if (!callable) callable = *slot; continue; }
      kuKernelCpuUnrestrictedMemcpy(slot, &repl, sizeof repl);
    }
    if (pass == 0 && (!n || bad)) { if (n_out) *n_out = 0; return 0; }
  }
  if (n_out) *n_out = n;
  return callable;
}

/* ---- engine pieces ------------------------------------------------------------------------ */
typedef uint32_t (*aedt_t)(void *ai, uint32_t day, uint32_t time, uint32_t caller, uint32_t target, uint32_t ev,
                           void *data);
typedef uint32_t (*oem_t)(void *trig, int arg);
typedef int (*cfm_t)(void *trig, uint32_t id, int arg);
typedef void *(*get_trig_t)(void *server, uint32_t id);
typedef uint32_t (*ev_objid_t)(void *ev, int i);

static aedt_t o_aedt;
static oem_t o_oem;
static cfm_t o_cfm;
static get_trig_t f_get_trig;
static ev_objid_t f_ev_objid;
static void **p_app;                        /* &g_pAppManager */
static const uint32_t *p_ev_signal, *p_ev_entered;

static void *trigger_of(uint32_t id) {
  void *app = p_app ? *p_app : NULL;
  void *server = app ? *(void **)((uint8_t *)app + 0x8) : NULL;
  return server ? f_get_trig(server, id) : NULL;
}
static int is_mine(const uint8_t *t) { return t && !*(const uint32_t *)(t + 0x2f4) && *(const uint32_t *)(t + 0x2fc); }

/* ---- queued enters, matched when handled -------------------------------------------------- */
#define REC_N 64
typedef struct { uint32_t trig, crea; uint64_t t; } rec_t;
static rec_t s_rec[REC_N];
static volatile uint32_t s_rec_next;

static uint32_t w_queued, w_handled, w_fired, w_refused, w_unmatched, w_lines;

static uint32_t h_aedt(void *ai, uint32_t day, uint32_t time, uint32_t caller, uint32_t target, uint32_t ev,
                       void *data) {
  int signal_enter = p_ev_signal && ev == *p_ev_signal && data && *(const uint16_t *)data == 0xc;
  int entered = p_ev_entered && ev == *p_ev_entered;
  if (signal_enter || entered) {
    const uint8_t *t = trigger_of(target);
    if (is_mine(t)) {
      uint32_t crea = signal_enter ? f_ev_objid(data, 0) : caller;
      uint32_t i = __sync_fetch_and_add(&s_rec_next, 1) % REC_N;
      s_rec[i].trig = target; s_rec[i].crea = crea; s_rec[i].t = now_us();
      w_queued++;
      if (w_lines++ < 300)
        log_printf("[mine] queued: trigger %08x, creature %08x (%s), delay %u.%03u s, owner %08x, thread %08x",
                   (unsigned)target, (unsigned)crea, signal_enter ? "signal 0xc" : "ENTERED_TRIGGER",
                   (unsigned)time / 1000u, (unsigned)time % 1000u, (unsigned)*(const uint32_t *)(t + 0x328),
                   (unsigned)sceKernelGetThreadId());
    }
  }
  return o_aedt(ai, day, time, caller, target, ev, data);
}

static volatile int s_in_oem;
static volatile int s_cfm_ret = -1;
static int h_cfm(void *trig, uint32_t id, int arg) {
  int r = o_cfm(trig, id, arg);
  if (s_in_oem) s_cfm_ret = r;
  return r;
}

static uint32_t h_oem(void *trig, int arg) {
  const uint8_t *t = trig;
  uint32_t tid = *(const uint32_t *)(t + 0x4), crea = *(const uint32_t *)(t + 0x2dc);
  uint64_t now = now_us(), q = 0;
  for (int k = 0; k < REC_N; k++)
    if (s_rec[k].trig == tid && s_rec[k].crea == crea && s_rec[k].t > q) q = s_rec[k].t;
  s_cfm_ret = -1;
  s_in_oem = 1;
  uint32_t r = o_oem(trig, arg);
  s_in_oem = 0;
  int can = s_cfm_ret;
  w_handled++;
  if (can > 0) w_fired++;
  else if (can == 0) w_refused++;
  if (!q) w_unmatched++;
  if (w_lines++ < 300) {
    if (q)
      log_printf("[mine] handled: trigger %08x, creature %08x, %u ms after queued, arg %d, can fire %d",
                 (unsigned)tid, (unsigned)crea, (unsigned)((now - q) / 1000u), arg, can);
    else
      log_printf("[mine] handled: trigger %08x, creature %08x, not seen queued, arg %d, can fire %d",
                 (unsigned)tid, (unsigned)crea, arg, can);
  }
  return r;
}

void mine_trace_on_swap(void) {
  if (!o_aedt) return;
  static uint64_t t0;
  uint64_t now = now_us();
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  if (w_queued || w_handled)
    log_printf("[mine] 10 s: %u enters queued, %u handled (%u fired, %u refused, %u not seen queued)",
               (unsigned)w_queued, (unsigned)w_handled, (unsigned)w_fired, (unsigned)w_refused,
               (unsigned)w_unmatched);
  w_queued = w_handled = w_fired = w_refused = w_unmatched = 0;
  t0 = now;
}

void mine_trace_install(void) {
  f_get_trig = (get_trig_t)so_symbol(&kotor_mod, "_ZN13CServerExoApp24GetTriggerByGameObjectIDEm");
  f_ev_objid = (ev_objid_t)so_symbol(&kotor_mod, "_ZN12CScriptEvent11GetObjectIDEi");
  p_app = (void **)so_symbol(&kotor_mod, "g_pAppManager");
  p_ev_signal = (const uint32_t *)so_symbol(&kotor_mod, "_ZN15CServerAIMaster18EVENT_SIGNAL_EVENTE");
  p_ev_entered = (const uint32_t *)so_symbol(&kotor_mod, "_ZN15CServerAIMaster21EVENT_ENTERED_TRIGGERE");
  if (!f_get_trig || !f_ev_objid || !p_app || !p_ev_signal) {
    log_printf("[mine] NOT armed: engine symbols missing (%d %d %d %d)", !!f_get_trig, !!f_ev_objid, !!p_app,
               !!p_ev_signal);
    return;
  }
  unsigned n_cfm = 0, n_oem = 0, n_aedt = 0;
  o_cfm = (cfm_t)take_slots("_ZN11CSWSTrigger22GetCanFireMineOnObjectEmi", (uintptr_t)h_cfm, &n_cfm);
  o_oem = (oem_t)take_slots("_ZN11CSWSTrigger11OnEnterMineEi", (uintptr_t)h_oem, &n_oem);
  if (o_oem)
    o_aedt = (aedt_t)take_slots("_ZN15CServerAIMaster17AddEventDeltaTimeEmmmmmPv", (uintptr_t)h_aedt, &n_aedt);
  log_printf("[mine] armed: AddEventDeltaTime %u slots, OnEnterMine %u, GetCanFireMineOnObject %u; "
             "EVENT_SIGNAL_EVENT %u, EVENT_ENTERED_TRIGGER %u",
             n_aedt, n_oem, n_cfm, (unsigned)*p_ev_signal, p_ev_entered ? (unsigned)*p_ev_entered : 0u);
}

#else
void mine_trace_install(void) {}
void mine_trace_on_swap(void) {}
#endif
