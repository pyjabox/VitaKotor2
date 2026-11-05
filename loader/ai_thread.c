/* ai_thread.c -- run the server AI on core 1 while the game thread draws the scene.
 *
 * A frame on the game thread is: the sound update; the client (input, the
 * server's messages, client animation, then Scene::Render); the server (the
 * client's messages, CServerAIMaster::UpdateState -- the AI, up to its own
 * 10 ms budget, which busy fights fill -- then the updates for the client);
 * the swap. Beside it run only the GL worker and the sound.
 *
 * Client and server are not separate. Beyond their message layer (128 sends,
 * 127 handlers), server code calls about 240 client functions directly -- GUI,
 * sound, input, camera, animation, the client's objects -- and client code
 * about 260 server ones (libkotor2 2.0.4, direct calls). A thread running the
 * AI beside the whole client would race in hundreds of places. Scene::Render
 * is the exception: by direct calls it reaches drawing code only (Gob, Part,
 * the GL translation, ManageSceneBSP), one server function
 * (CServerExoApp::GetPartyTable) and no resource manager.
 *
 * So the AI moves into the drawing. When the next frame's Scene::Render
 * starts, a worker on core 1 runs UpdateState while the game thread draws;
 * Scene::Render waits for it before returning, and the server loop then skips
 * its own call. The server keeps its order -- its AI, then its updates for the
 * client -- except that a frame's updates now go out before that frame's AI,
 * so the client sees the AI's decisions one frame later. The 10 ms budget is
 * the engine's, unchanged.
 *
 * Where the two sides meet, the worker waits. Every client function server
 * code can call (ai_thread_gates.h) is intercepted through its relocation
 * slots, except plain lookups the drawing never changes: the party list and
 * its follow paths, object-id lookups, the pointers to the GUI, options and
 * module. Called by the worker while the scene is being drawn, such a function
 * waits for the drawing to end, and the rest of that AI pass runs alone. In
 * Vita3K that is the pass's last step for most passes: the loop at the end of
 * UpdateState that copies dirty flags to the client objects (GetClientObject).
 * The drawing's one server call, GetPartyTable from ManageSceneBSP's T3-M4
 * tutorial check, reads two ints of the party table and needs no wait; a
 * server function the drawing needed finished would make the game thread wait
 * for the AI instead (G_JOIN; none in 2.0.4). All of these calls go through
 * relocation slots (the PLT or a vtable), so none is missed by direct calls;
 * virtual calls the static map cannot follow, and fields read without a call,
 * are the remaining risk.
 *
 * Module loads, saves, unloads and the server's teardown bump an epoch. The AI
 * only moves into a frame's drawing if the server reached its AI in each of
 * the two frames before and nothing has bumped the epoch since; otherwise the
 * server runs it in its own loop, as the game does.
 *
 * ux0:data/kotor2/ai_thread.txt: 0 off (nothing hooked), 1 on, i the move
 * without the worker (the AI runs on the game thread when Scene::Render
 * starts), a on and off alternately every 10 s. Without the file:
 * AI_THREAD_DEFAULT. */

#include <vitasdk.h>
#include <kubridge.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "threads.h"
#include "ai_thread.h"
#include "gl_worker.h"
#include "ai_throttle.h"
#include "ai_budget.h"
#include "ai_census.h"
#include "nwvm.h"
#include "pc_prof.h"
#include "battle_ledger.h"

#if AI_THREAD

#define G_WAIT  1u
#define G_EPOCH 2u
#define G_JOIN  4u
typedef struct { const char *sym; unsigned flags; const char *name; } ai_gate_def_t;
#include "ai_thread_gates.h"
#define NDEFS ((int)(sizeof k_ai_gates / sizeof k_ai_gates[0]))

#define NSTUBS 1024                  /* slots we can take over: ai_gate_stubs has one each */
#define NHASH 1024                   /* symbol name -> k_ai_gates index, open addressing */
#define WORKER_STACK (1024 * 1024)
#define EV_GO     1u                 /* game -> worker: run a pass */
#define EV_CLOSED 2u                 /* game -> worker: the drawing has ended */
#define EV_DONE   4u                 /* worker -> game: the pass has ended */
#define EV_SERVED 8u                 /* game -> worker: a served call has been made (AI_SERVE) */
#define WINDOW_US 10000000u
#define UPDATE_SYM "_ZN15CServerAIMaster11UpdateStateEv"
#define RENDER_SYM "_ZN5Scene6RenderEv"
#define TIMER_SYM  "_ZN10CExoTimers22GetHighResolutionTimerEv"
#define NSITES 16                    /* where passes stopped, per window */

/* Gates that do not wait for these callers. GetClientCreature hands out the
 * client creature; from these server functions (hardware, 2026-10-16: the
 * first gate in nearly every pass) it is only read -- its id (+0x4), two
 * flags (+0x3c0, +0x3f8), a facing vector (+0x30..+0x38) -- or passed to a
 * gated client function or a free getter (disassembly of libkotor2 2.0.4).
 * SetAnimation and AIActionPlayAnimation (the next gates on hardware) only ask
 * it AnimationFireAndForget and GetAnimationLength, which read a 2DA row and
 * the model's animation list. The other callers write it, or tail-call client
 * code through the PLT with it (CancelCombat, GetPauseReadyAnimation), and
 * wait. GetClientObject is free inside GetClientCreature, whose own gate has
 * decided; elsewhere (the end of UpdateState copies dirty state into client
 * objects through their vtables) it waits. */
#define NCALLERS 14
static const struct { const char *gate; const char *callers[NCALLERS]; } k_free_from[] = {
  { "_ZN12CSWSCreature17GetClientCreatureEv", {
      "_ZN12CSWSCreature28WalkUpdateLocation_QuickWalkEi",
      "_ZN12CSWSCreature41WalkUpdateLocation_QuickWalk_FollowLeaderEv",
      "_ZN12CSWSCreature15DoSpotDetectionEPS_i",
      "_ZN12CSWSCreature17DoListenDetectionEPS_i",
      "_ZN12CSWSCreature15UpdateMineCheckEv",
      "_ZN12CSWSCreature21IsProjectileParryableEP10CSWSObjectiiRiS2_i",
      "_ZN21CSWSEffectListHandler18OnApplyForceShieldEP10CSWSObjectP11CGameEffecti",
      "_ZN25CSWVirtualMachineCommands23ExecuteCommandIsRunningEii",
      "_ZN12CSWSCreature12SetAnimationEi",
      /* The Khoonda battle (hardware, 2026-11-04: with the next two, 95% of
       * the passes' first stops in its heaviest phase; the AI then ran 2-6 ms
       * of its 29-73 before waiting out the drawing). SetCombatState checks
       * the client creature exists, then hands it to SetCombatState, deferred
       * below; GetPauseReadyAnimation tail-calls the client's, which only
       * reads (free from anywhere, k_free_any). */
      "_ZN12CSWSCreature14SetCombatStateEihi",
      "_ZN12CSWSCreature22GetPauseReadyAnimationEv",
      /* With the attack-animation calls served (AI_SERVE), the battle's next
       * stop (hardware, 2026-11-05): ResolveRangedSpecialAttack reads one byte
       * of the client creature's animation base (+0xc5, through GetAnimationBase
       * below) and nothing else of the client. Part of the AI_SERVE lever. */
      "_ZN12CSWSCreature26ResolveRangedSpecialAttackEP10CSWSObjecti", NULL } },
  { "_ZN10CSWSObject15GetClientObjectEv", {
      "_ZN12CSWSCreature17GetClientCreatureEv",
      "_ZN10CSWSObject21AIActionPlayAnimationEP20CSWSObjectActionNode", NULL } },
  { "_ZN10CSWCObject16GetAnimationBaseEv", {
      "_ZN12CSWSCreature26ResolveRangedSpecialAttackEP10CSWSObjecti", NULL } },
};
#define NFREE 20
/* Free callers that belong to the AI_SERVE lever (on with it). */
static const char *const k_lever_callers[] = { "_ZN12CSWSCreature26ResolveRangedSpecialAttackEP10CSWSObjecti",
                                               "_ZN12CSWSCreature8DoDamageEii" };
static int is_lever(const char *caller) {
  for (unsigned i = 0; i < sizeof k_lever_callers / sizeof k_lever_callers[0]; i++)
    if (!strcmp(caller, k_lever_callers[i])) return 1;
  return 0;
}

/* Gates that never wait: client functions that only read -- no store, and
 * nothing but lookups called (disassembly of libkotor2 2.0.4).
 * CSWCCreature::GetPauseReadyAnimation reads the creature, IsInjured, the
 * client options and the party; the server's tail-calls it, so its return
 * address is the server caller's, whoever that is. */
static const char *const k_free_any[] = {
  "_ZN12CSWCCreature22GetPauseReadyAnimationEv",
};
static uint8_t s_free_any[NDEFS];

/* CSWGuiMainInterface::ShowHelpForString(strref) acts on four strrefs and
 * returns at once for any other. CTlkTable::Fetch calls it for every string
 * the server fetches -- combat feedback, in a battle (hardware, 2026-11-04:
 * 26% of the first stops). Those calls go on; the four still wait. */
#define HELP_SYM "_ZN19CSWGuiMainInterface17ShowHelpForStringEi"
#define HELP_FN  0x347b4cu
static int s_help_def = -1;
static uint32_t s_help_ref[4];

/* Calls the worker queues instead of waiting for, while the scene is drawn,
 * and makes itself once the drawing has ended: before its next gated call, or
 * at the end of its pass. CSWCCreature::LookAt turns a creature's head through
 * its model, which the drawing animates. From the follow-leader walk (hardware,
 * 2026-10-16: the first gate in 168 of 169 passes while the party followed)
 * its result is unused, and the last call per creature is the one that
 * counts, so the queue keeps one per creature. */
/* by_object: a call replaces the queued one with the same first argument (the
 * last per creature counts); 0: every call is kept. */
static const struct { const char *gate; const char *caller; int by_object; } k_defer[] = {
  { "_ZN12CSWCCreature6LookAtEmf", "_ZN12CSWSCreature41WalkUpdateLocation_QuickWalk_FollowLeaderEv", 1 },
  /* The Khoonda battle's other first stops (hardware, 2026-11-04). Each result
   * is unused, and the last call per creature is the one that counts: the
   * client creature's combat state, the facing lock SetFacing clears, the
   * camera reset after a walk step. */
  { "_ZN12CSWCCreature14SetCombatStateEi", "_ZN12CSWSCreature14SetCombatStateEihi", 1 },
  { "_ZN12CSWCCreature26SetLockOrientationToObjectEmi", "_ZN25CSWVirtualMachineCommands23ExecuteCommandSetFacingEii", 1 },
  { "_ZN12CSWCCreature19ResetCameraToFollowEv", "_ZN12CSWSCreature28WalkUpdateLocation_QuickWalkEi", 1 },
  /* Next in the battle with those gone (hardware, 2026-11-04). The combat
   * message line shows one message: the last wins (CSWPartyMemberData::
   * SetCombatMessage stores it, then tail-calls the GUI's, so the caller is
   * the attack). Damage numbers over the creatures: every one is kept. */
  { "_ZN10CGuiInGame16SetCombatMessageEm", "_ZN12CSWSCreature23AIActionPhysicalAttacksEP20CSWSObjectActionNode", 1 },
  { "_ZN10CGuiInGame16SetCombatMessageEm", "_ZN12CSWSCreature26WalkUpdateLocationDistanceEfP6VectorS1_P13CExoArrayListImE", 1 },
  { "_ZN13CClientExoApp13AddFloatyTextEmhi", "_ZN21CSWSEffectListHandler13OnApplyDamageEP10CSWSObjectP11CGameEffecti", 0 },
  /* With the AI_SERVE lever, a next stop in the battle (Vita3K, 2026-11-05):
   * DoDamage's damage numbers; the result is unused (r0 is reloaded at once). */
  { "_ZN13CClientExoApp13AddFloatyTextEmhi", "_ZN12CSWSCreature8DoDamageEii", 0 },
};
#define NDEFER 8
#define NQUEUE 64

enum { M_OFF, M_THREAD, M_INLINE };

extern char ai_gate_stubs[];         /* NSTUBS ARM stubs of 8 bytes, at the end of this file */
uintptr_t ai_gate_enter(unsigned stub, uintptr_t ret, const uint32_t *args);
static int ai_gate_ret0(void) { return 0; }

static uintptr_t s_next[NSTUBS];     /* where each taken slot pointed before */
static uint16_t s_def[NSTUBS];       /* and its k_ai_gates entry */
static unsigned s_nstubs;

static int s_installed, s_mode, s_ab, s_ab_on = 1;
static void (*s_update)(void *master);
static void (*s_render)(void *scene);
static uint64_t (*s_timer)(void *timers);
static uintptr_t s_upd_lo, s_upd_hi;   /* UpdateState's code */
static struct { int def; uintptr_t lo, hi; int newer, lever; } s_free[NFREE];
static int s_nfree;
static struct { int def; uintptr_t lo, hi; int newer, by_object, lever; } s_defer[NDEFER];
/* AI_GATES_AB (test builds): the rules for these callers (the latest round)
 * on and off, AI_GATES_AB_PERIOD 10 s windows each. */
static int s_newer_off;
static const char *const k_newer[] = {
  "_ZN12CSWSCreature23AIActionPhysicalAttacksEP20CSWSObjectActionNode",
  "_ZN12CSWSCreature26WalkUpdateLocationDistanceEfP6VectorS1_P13CExoArrayListImE",
  "_ZN21CSWSEffectListHandler13OnApplyDamageEP10CSWSObjectP11CGameEffecti",
};
static int is_newer(const char *caller) {
  for (unsigned i = 0; i < sizeof k_newer / sizeof k_newer[0]; i++) if (!strcmp(caller, k_newer[i])) return 1;
  return 0;
}
static int s_ndefer;
/* Per gate, where its entries start in s_free / s_defer and how many: a gate
 * with none (all but two) decides without scanning. */
static uint8_t s_free_at[NDEFS], s_free_n[NDEFS], s_defer_at[NDEFS], s_defer_n[NDEFS];
/* Deferred calls: worker only. */
static struct { unsigned stub; uint32_t a0, a1, a2, a3; } s_q[NQUEUE];
static int s_nq, s_flushing;
static uint32_t w_deferred, w_flushed;
#if AI_THREAD_DIAG
static uint32_t d_gate_passes, d_cpu_reads;  /* the worker's own costs, per window */
static uint64_t d_cpu_read_us;
#endif

/* Shared between the game thread and the worker. */
static volatile int s_window;        /* drawing, with the worker running beside it */
static volatile uint32_t s_epoch;
static SceUID s_ev = -1;
static volatile uintptr_t s_wstack_lo, s_wstack_hi;
static void *s_job;
static volatile int s_wait_def = -1; /* the gate the worker waits at, for the stuck line */
#define DEF_GL (-2)                  /* worker_wait: a GL call (gl_worker's foreign hook) */
static uint32_t s_worker_glkey;      /* the worker's key for gl_worker_set_guest */
static uint32_t w_gl_waits, w_gl_calls;
/* One pass, written by the worker, read by the game thread after EV_DONE. */
static uint64_t s_p_t0, s_p_us, s_p_before_us;
static uint64_t s_p_cpu_us;              /* the worker's CPU time, this pass */
static int s_p_waited;
/* When the pass's levels were done: UpdateState's client sync (its loop of
 * GetClientObject calls, returning to SYNC_RET) started, since the pass's start. */
#define SYNC_RET 0x4a816bu
static int s_gco_def = -1, s_dlgact_def = -1;
static uint64_t s_p_sync_t;
static int s_p_mid_stop;                 /* the pass first waited at a gate before the client sync */
#if BATTLE_LEDGER
/* BATTLE_LEDGER (test builds): where each pass's wall time goes -- the
 * worker's CPU, its waits at gates (the client sync at UpdateState's end, or
 * mid-pass), and the rest, other threads holding core 1 -- and what the AI
 * was doing when the drawing ended. Written by the worker during a pass, read
 * by the game thread after EV_DONE. */
#define NB_MAX 24
static volatile uint64_t s_idle_total;   /* the worker's time at gates, all passes */
static uint64_t s_l_idle_sync, s_l_idle_mid, s_l_sync_t, s_l_sync_cpu;
static volatile uint64_t s_l_cpu0;       /* the worker's CPU time at the pass's start; 0 until it starts */
static int s_l_first;                    /* the pass's first wait: 0 none, 1 the client sync, 2 mid-pass */
static uint64_t s_l_idle_served;         /* the worker's wait for served calls, this pass */
static SceUID s_worker_thid = -1;
static struct { SceUID thid; const char *tag; uint64_t run0; } s_nb[NB_MAX];
static int s_nb_n;
static uint64_t s_l_nb[3];               /* other threads' CPU during the pass, by the core they last ran on */
typedef struct {
  uint32_t n, sync_late, n_x;
  uint64_t draw, wall, cpu, cpu_before, cpu_sync, idle_sync, idle_mid, idle_served, wait, sync_t, nb[3];
  uint64_t cpu_x, wall_x, idle_x;        /* passes whose exact CPU time is known (below) */
} fate_t;
/* runClocks only grows when a thread is switched out (the busy-spin
 * calibration reads 0 on hardware), so a read while the worker runs misses its
 * current slice. Read while it sleeps -- just before the next EV_GO -- it is
 * exact: the difference between two such reads is the pass in between. */
static uint64_t s_x_prev;
static int s_x_fate = -1;
static uint64_t s_x_wall, s_x_idle;
static fate_t w_fate[3];                 /* at the drawing's end: done or syncing, stopped at a gate, computing */
static struct { char tag[20]; uint64_t us[3]; } w_nb[NB_MAX];
static int w_nb_n;
static float s_clk_ratio;                /* runClocks per wall-clock us in a busy spin */
#endif
static struct { int def; uintptr_t ret; uint32_t n; } w_sites[NSITES];
static uint32_t w_hits[NDEFS];       /* passes that waited, by the gate they waited at */
static uint32_t w_epoch_hits[NDEFS]; /* epoch bumps, by function */

/* Game thread only. */
static uint64_t s_render_total;      /* Scene::Render with the AI beside it, and the join after it, since boot */
static void *s_master;
static int s_site_seen, s_ready, s_pass_done, s_pass_live, s_in_render, s_pinned;
static uint32_t s_site_epoch, s_ready_epoch;
static unsigned s_steady;

/* Per 10 s window. */
static uint64_t w_t0, w_prev_swap, w_frame_us, w_frame_max;
static unsigned w_frames, w_thread, w_inline, w_server, w_skipped, w_waited, w_joins;
static uint64_t w_pass_us, w_pass_max, w_before_us, w_render_us, w_join_us, w_join_max, w_server_us,
    w_early_us, w_cpu_us;

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static int cur_mode(void) { return s_ab && !s_ab_on ? M_OFF : s_mode; }

/* ---- The two sides meeting -------------------------------------------- */

static void note_site(int def, uintptr_t ret) {
  int free_slot = -1;
  for (int i = 0; i < NSITES; i++) {
    if (w_sites[i].n && w_sites[i].def == def && w_sites[i].ret == ret) { w_sites[i].n++; return; }
    if (!w_sites[i].n && free_slot < 0) free_slot = i;
  }
  if (free_slot >= 0) { w_sites[free_slot].def = def; w_sites[free_slot].ret = ret; w_sites[free_slot].n = 1; }
}

static int on_worker(void) {
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  return sp >= s_wstack_lo && sp < s_wstack_hi;
}

/* Make the deferred calls, in the order they were first queued. Worker only,
 * with the drawing over. */
static void flush_deferred(void) {
  if (s_flushing || !s_nq) return;
  s_flushing = 1;
  for (int i = 0; i < s_nq; i++)
    ((int (*)(uint32_t, uint32_t, uint32_t, uint32_t))s_next[s_q[i].stub])(s_q[i].a0, s_q[i].a1,
                                                                          s_q[i].a2, s_q[i].a3);
  w_flushed += (uint32_t)s_nq;
  s_nq = 0;
  s_flushing = 0;
}

static int defer_call(unsigned stub, const uint32_t *args, int by_object) {
  for (int i = 0; i < s_nq && by_object; i++)
    if (s_q[i].stub == stub && s_q[i].a0 == args[0]) {   /* same function, same object: last wins */
      s_q[i].a1 = args[1]; s_q[i].a2 = args[2]; s_q[i].a3 = args[3];
      w_deferred++;
      return 1;
    }
  if (s_nq >= NQUEUE) return 0;
  s_q[s_nq].stub = stub;
  s_q[s_nq].a0 = args[0]; s_q[s_nq].a1 = args[1]; s_q[s_nq].a2 = args[2]; s_q[s_nq].a3 = args[3];
  s_nq++;
  w_deferred++;
  return 1;
}

#if AI_SERVE
/* Served calls. The attack-animation lookups (Resolve*Attack -> CSWSCreature::
 * GetClientMelee/RangedAnimation -> the client's) compute and store the
 * attacker's client attack data (CSWCCreature::ResolveAttackIndex, Update
 * Melee/RangedAttackData, +0x3d8, +0x400; libkotor2 2.0.4), which the
 * drawing's animation events read. Instead of letting the worker wait for the
 * drawing's end, the game thread makes the call itself at the drawing's next
 * safe point (ai_thread_safe_point: between two Gob::Animate calls, or after
 * ManageSceneBSP), with no animation in progress: the data is never written
 * while it is read, only up to one frame earlier than after the drawing. The
 * calls the worker has queued (k_defer) go first, as they do after a wait --
 * except a queued camera reset, which must not land mid-drawing: then the
 * worker waits as before. Neither creates nor destroys objects, nor calls GL. */
static const struct { const char *sym; uint8_t nstack; } k_serve[] = {
  { "_ZN12CSWSCreature23GetClientMeleeAnimationEmtiii", 2 },   /* this, target, anim, int; two ints on the stack */
  { "_ZN12CSWSCreature24GetClientRangedAnimationEiii", 0 },
};
static int8_t s_serve_stack[NDEFS];  /* stack arguments of a served gate, -1 for the others */
static int s_serve_on = AI_SERVE_DEFAULT == '1', s_camera_def = -1;
typedef uint32_t (*serve_fn_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static struct { uintptr_t fn; uint32_t a[8]; uint32_t result; } s_srv;
static volatile int s_srv_state;     /* 0 none, 1 asked, 2 being made by the game thread, 3 made */
static uint32_t s_srv_ret;           /* the worker's: the served call's result, for serve_ret */
static uint32_t w_served, w_served_late, w_serve_refused;
static uint64_t w_served_wait_us, w_served_wait_max, w_serve_us;

static uint32_t serve_ret(void) { return s_srv_ret; }

static int queue_has_camera(void) {
  for (int i = 0; i < s_nq; i++) if (s_def[s_q[i].stub] == s_camera_def) return 1;
  return 0;
}

/* Worker, with the drawing in progress: have the game thread make the call. */
static uint32_t serve_call(unsigned stub, const uint32_t *args, int nstack) {
  uint64_t t0 = now_us();
  s_srv.fn = s_next[stub];
  for (int i = 0; i < 4; i++) s_srv.a[i] = args[i];
  for (int i = 0; i < 4; i++) s_srv.a[4 + i] = i < nstack ? args[6 + i] : 0;   /* the stub pushed 6 words */
  __sync_synchronize();
  s_srv_state = 1;
  uint32_t out = 0, r;
  sceKernelWaitEventFlag(s_ev, EV_SERVED | EV_CLOSED, SCE_EVENT_WAITOR, &out, NULL);
  uint64_t waited = now_us() - t0;
  if (!(out & EV_SERVED) && __sync_bool_compare_and_swap(&s_srv_state, 1, 0)) {
    /* The drawing ended first: as after any wait, the queued calls, then this one. */
    flush_deferred();
    r = ((serve_fn_t)s_srv.fn)(s_srv.a[0], s_srv.a[1], s_srv.a[2], s_srv.a[3], s_srv.a[4], s_srv.a[5], s_srv.a[6],
                               s_srv.a[7]);
    w_served_late++;
  } else {
    if (!(out & EV_SERVED)) sceKernelWaitEventFlag(s_ev, EV_SERVED, SCE_EVENT_WAITOR, NULL, NULL);
    waited = now_us() - t0;
    __sync_synchronize();
    r = s_srv.result;
    w_served++;
  }
  s_srv_state = 0;
  sceKernelClearEventFlag(s_ev, ~EV_SERVED);
  w_served_wait_us += waited;
  if (waited > w_served_wait_max) w_served_wait_max = waited;
#if BATTLE_LEDGER
  s_idle_total += waited;
  s_l_idle_served += waited;
#endif
  return r;
}
#endif

/* Game thread, inside the drawing, with no animation in progress: make the
 * call the worker is waiting on, if any. */
void ai_thread_safe_point(void) {
#if AI_SERVE
  if (s_srv_state != 1 || !s_window || on_worker()) return;
  if (!__sync_bool_compare_and_swap(&s_srv_state, 1, 2)) return;
  uint64_t t0 = now_us();
  flush_deferred();                  /* the worker's queued calls came first (it waits) */
  s_srv.result = ((serve_fn_t)s_srv.fn)(s_srv.a[0], s_srv.a[1], s_srv.a[2], s_srv.a[3], s_srv.a[4], s_srv.a[5],
                                        s_srv.a[6], s_srv.a[7]);
  w_serve_us += now_us() - t0;
  __sync_synchronize();
  s_srv_state = 3;
  sceKernelSetEventFlag(s_ev, EV_SERVED);
#endif
}

#if AI_SERVE
#define LEVER_ON s_serve_on
#else
#define LEVER_ON 0
#endif

/* A gated call from the worker while the scene is drawn: goes on, is queued
 * for after the drawing, or waits for the drawing to end. */
enum { GD_FREE, GD_DEFER, GD_WAIT };
static int gate_decision(int def, uintptr_t ret, const uint32_t *args, int *by_object) {
  int nw = !s_newer_off;
  if (s_free_any[def]) return GD_FREE;
  if (def == s_help_def && args[1] != s_help_ref[0] && args[1] != s_help_ref[1] && args[1] != s_help_ref[2] &&
      args[1] != s_help_ref[3])
    return GD_FREE;
  for (int i = s_free_at[def], e = i + s_free_n[def]; i < e; i++)
    if (ret >= s_free[i].lo && ret < s_free[i].hi && (nw || !s_free[i].newer) &&
        (!s_free[i].lever || LEVER_ON))
      return GD_FREE;
  for (int i = s_defer_at[def], e = i + s_defer_n[def]; i < e; i++)
    if (ret >= s_defer[i].lo && ret < s_defer[i].hi && (nw || !s_defer[i].newer) && (!s_defer[i].lever || LEVER_ON)) {
      if (by_object) *by_object = s_defer[i].by_object;
      return GD_DEFER;
    }
  return GD_WAIT;
}

#if AI_GATE_CENSUS
/* Test builds: the gated calls the worker makes after its pass has stopped
 * once (the drawing over), by function and caller: where it would stop next. */
#define NCENSUS 128
static struct { uint16_t def; uintptr_t ret; uint32_t n; } s_census[NCENSUS];
static uint32_t s_census_lost;
static void census_note(int def, uintptr_t ret) {
  uint32_t h = ((uint32_t)ret * 2654435761u) >> 25;
  for (int i = 0; i < 8; i++, h = (h + 1) & (NCENSUS - 1)) {
    if (s_census[h].n && s_census[h].ret == ret && s_census[h].def == def) { s_census[h].n++; return; }
    if (!s_census[h].n) { s_census[h].def = (uint16_t)def; s_census[h].ret = ret; s_census[h].n = 1; return; }
  }
  s_census_lost++;
}
#endif

static void worker_wait(int def, uintptr_t ret) {
  uint64_t t = now_us();
  if (!s_p_waited) {
    s_p_waited = 1;
    s_p_before_us = t - s_p_t0;
    s_p_mid_stop = !(def == s_gco_def && ret - kotor_mod.text_base == SYNC_RET);
    if (def >= 0) {
      w_hits[def]++;
      note_site(def, ret);
    } else {
      w_gl_waits++;
    }
#if BATTLE_LEDGER
    s_l_first = (def == s_gco_def && ret - kotor_mod.text_base == SYNC_RET) ? 1 : 2;
#endif
  }
  s_wait_def = def;
  sceKernelWaitEventFlag(s_ev, EV_CLOSED, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
  s_wait_def = -1;
#if BATTLE_LEDGER
  {
    uint64_t d = now_us() - t;
    s_idle_total += d;
    if (def == s_gco_def && ret - kotor_mod.text_base == SYNC_RET) s_l_idle_sync += d;
    else s_l_idle_mid += d;
  }
#endif
  flush_deferred();                  /* they came first */
}

/* gl_worker asks this before dropping a GL call from a thread that is not the
 * game thread. From the worker during a pass: wait for the drawing to end; by
 * then the game thread has made the worker its guest (render_hook sets it
 * before it signals the end), so the call is recorded, in order, while the game
 * thread waits for the pass. Two GL calls from the worker were dropped on
 * hardware (2026-10-16, during loads): after a gate, a gated function runs on,
 * alone, and can reach the renderer. */
static int gl_foreign(void) {
  if (!on_worker()) return 0;
  w_gl_calls++;
  if (s_window) worker_wait(DEF_GL, (uintptr_t)__builtin_return_address(0));
  return 1;
}

/* The worker's own CPU time in microseconds (runClocks; PSP SysClock units,
 * and 0.907 per us in the audio thread's busy-spin calibration, which loses
 * the time that spin was preempted). A system call. */
static uint64_t worker_cpu_us(void) {
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  if (sceKernelGetThreadInfo(0, &ti) < 0) return 0;
  return (uint64_t)ti.runClocks;
}

#if BATTLE_LEDGER
/* A thread's CPU time (runClocks) and the core it is pinned to or last ran on. */
static uint64_t run_clocks(SceUID thid, int *core) {
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  if (sceKernelGetThreadInfo(thid, &ti) < 0) {
    if (core) *core = -1;
    return 0;
  }
  if (core) {
    int c;
    switch (ti.currentCpuAffinityMask & SCE_KERNEL_CPU_MASK_USER_ALL) {
      case SCE_KERNEL_CPU_MASK_USER_0: c = 0; break;
      case SCE_KERNEL_CPU_MASK_USER_1: c = 1; break;
      case SCE_KERNEL_CPU_MASK_USER_2: c = 2; break;
      default: c = ti.lastExecutedCpuId; break;
    }
    *core = c >= 0 && c < 3 ? c : -1;
  }
  return (uint64_t)ti.runClocks;
}

/* Worker, before the pass's clocks start: reset, and every other recorded
 * thread's CPU time as the baseline. */
static void ledger_pass_start(void) {
  s_l_idle_sync = s_l_idle_mid = s_l_idle_served = s_l_sync_t = s_l_sync_cpu = 0;
  s_l_first = 0;
  s_l_nb[0] = s_l_nb[1] = s_l_nb[2] = 0;
  s_nb_n = 0;
  int n = thread_census_count();
  for (int i = 0; i < n && s_nb_n < NB_MAX; i++) {
    SceUID id;
    const char *tag;
    if (!thread_census_get(i, &id, &tag) || id == s_worker_thid) continue;
    s_nb[s_nb_n].thid = id;
    s_nb[s_nb_n].tag = tag;
    s_nb[s_nb_n].run0 = run_clocks(id, NULL);
    s_nb_n++;
  }
}

/* Game thread, once the pass is over: what the other threads ran meanwhile. */
static void ledger_pass_others(void) {
  for (int i = 0; i < s_nb_n; i++) {
    int core;
    uint64_t r = run_clocks(s_nb[i].thid, &core);
    if (core < 0 || r < s_nb[i].run0) continue;
    uint64_t d = r - s_nb[i].run0;
    s_l_nb[core] += d;
    int k;
    for (k = 0; k < w_nb_n && strcmp(w_nb[k].tag, s_nb[i].tag); k++) {}
    if (k == w_nb_n) {
      if (w_nb_n == NB_MAX) continue;
      strncpy(w_nb[k].tag, s_nb[i].tag, sizeof w_nb[k].tag - 1);
      w_nb[k].us[0] = w_nb[k].us[1] = w_nb[k].us[2] = 0;
      w_nb_n++;
    }
    w_nb[k].us[core] += d;
  }
}

/* Game thread, after the join: file the pass by what the AI was doing when
 * the drawing ended. */
static void ledger_frame(uint64_t draw, uint64_t wait, uint64_t cpu_draw_end) {
  ledger_pass_others();
  int f = s_l_first == 1 ? 0 : s_l_first == 2 ? 1 : s_p_us <= draw ? 0 : 2;
  fate_t *F = &w_fate[f];
  s_x_fate = f;                          /* its exact CPU time comes with the next pass's start */
  s_x_wall = s_p_us;
  s_x_idle = s_l_idle_sync + s_l_idle_mid + s_l_idle_served;
  uint64_t c0 = s_l_cpu0;
  F->n++;
  F->draw += draw;
  F->wait += wait;
  F->wall += s_p_us;
  F->cpu += s_p_cpu_us;
  F->cpu_before += c0 > 1 && cpu_draw_end > c0 ? cpu_draw_end - c0 : 0;
  F->idle_sync += s_l_idle_sync;
  F->idle_mid += s_l_idle_mid;
  F->idle_served += s_l_idle_served;
  uint64_t st = s_l_sync_t ? s_l_sync_t : s_p_us;
  F->sync_t += st;
  F->cpu_sync += s_l_sync_t ? s_l_sync_cpu : s_p_cpu_us;
  if (st > draw) F->sync_late++;
  for (int c = 0; c < 3; c++) F->nb[c] += s_l_nb[c];
}

static void ledger_log(unsigned frames) {
  static const char *const k_fate[3] = { "done or syncing", "stopped at a gate", "computing" };
  uint32_t tot = w_fate[0].n + w_fate[1].n + w_fate[2].n;
  if (tot) {
    log_printf("[ledger] %u AI passes in %u frames; at the drawing's end: done or syncing %u%%, stopped at a gate "
               "%u%%, computing %u%% | CPU clock %.3f per us",
               (unsigned)tot, frames, (unsigned)(w_fate[0].n * 100u / tot), (unsigned)(w_fate[1].n * 100u / tot),
               (unsigned)(w_fate[2].n * 100u / tot), (double)s_clk_ratio);
    for (int f = 0; f < 3; f++) {
      fate_t *F = &w_fate[f];
      if (!F->n) continue;
      double n = F->n * 1000.0;
      int64_t other = (int64_t)F->wall - (int64_t)F->cpu - (int64_t)F->idle_sync - (int64_t)F->idle_mid -
                      (int64_t)F->idle_served;
      log_printf("[ledger] %s (%u): drawing %.1f ms, then the game waited %.1f | pass %.1f ms = CPU %.1f (%.1f of it "
                 "before the drawing's end) + at the client sync %.1f + at other gates %.1f + served %.1f + off the CPU "
                 "%.1f | levels "
                 "done at %.1f ms (CPU %.1f), after the drawing's end in %u%% | other threads' CPU meanwhile, cores "
                 "0/1/2: %.1f/%.1f/%.1f ms",
                 k_fate[f], (unsigned)F->n, F->draw / n, F->wait / n, F->wall / n, F->cpu / n, F->cpu_before / n,
                 F->idle_sync / n, F->idle_mid / n, F->idle_served / n, (double)other / n, F->sync_t / n, F->cpu_sync / n,
                 (unsigned)(F->sync_late * 100u / F->n), F->nb[0] / n, F->nb[1] / n, F->nb[2] / n);
      if (F->n_x) {
        double nx = F->n_x * 1000.0;
        int64_t off = (int64_t)F->wall_x - (int64_t)F->cpu_x - (int64_t)F->idle_x;
        log_printf("[ledger] %s, exact CPU (%u passes, read while the worker slept): pass %.1f ms = CPU %.1f + at gates "
                   "%.1f + off the CPU %.1f",
                   k_fate[f], (unsigned)F->n_x, F->wall_x / nx, F->cpu_x / nx, F->idle_x / nx, (double)off / nx);
      }
    }
    char b[480];
    int o = 0;
    b[0] = 0;
    for (int k = 0; k < 10 && o < (int)sizeof b - 48; k++) {
      int best = -1;
      uint64_t bv = 0;
      for (int i = 0; i < w_nb_n; i++) {
        uint64_t v = w_nb[i].us[0] + w_nb[i].us[1] + w_nb[i].us[2];
        if (v > bv) { bv = v; best = i; }
      }
      if (best < 0 || bv < tot * 50u) break;          /* under 0.05 ms a pass */
      o += snprintf(b + o, sizeof b - (size_t)o, " %s %.2f/%.2f/%.2f", w_nb[best].tag,
                    w_nb[best].us[0] / (tot * 1000.0), w_nb[best].us[1] / (tot * 1000.0),
                    w_nb[best].us[2] / (tot * 1000.0));
      w_nb[best].us[0] = w_nb[best].us[1] = w_nb[best].us[2] = 0;
    }
    log_printf("[ledger] other threads during AI passes, ms a pass on cores 0/1/2:%s", o ? b : " none");
  }
  memset(w_fate, 0, sizeof w_fate);
  w_nb_n = 0;
}
#endif

/* CExoTimers::GetHighResolutionTimer (microseconds), as UpdateState sees it on
 * the worker: advancing only while the worker runs. UpdateState shares its
 * 10 ms out by this clock. Counting the time spent waiting at gates starved the
 * AI (hardware, 2026-10-16: ~20 AIUpdates a frame against ~60); counting the
 * time the audio mixer and the game's own threads (priorities 160 and 159,
 * above the worker's) held core 1 still cost it a quarter (42-48 against
 * 59-70). The CPU time is read at most every 1000 us (a system call, charged to
 * the AI's own budget) and extrapolated from the real clock in between. The
 * clock never goes back: UpdateState subtracts a
 * level's start from its last read as unsigned 64-bit numbers, so a read that
 * came out lower than the extrapolation before it made the elapsed time huge
 * and ended the level on its budget at once (Vita3K, 2026-10-16: level 1 ended
 * that way in every pass, after 18 iterations against 45). Where the kernel
 * gives no CPU time (Vita3K), the real clock stands in. Every other caller,
 * and the game thread, get the real clock. */
#define CPU_READ_US 1000u
static int s_tc_live;
static uint64_t s_tc_base_t, s_tc_base_cpu, s_tc_last_t, s_tc_last_cpu, s_tc_out;

/* AI_BUDGET_SCALE: the worker's clock runs slower by this factor (1000 = the
 * engine's 10 ms), set before each pass from the previous drawing. The engine
 * gives the AI 10 ms a frame whatever the frame's length: at 8 FPS in the
 * Khoonda battle that is 80 ms of AI a second, and most enemies stood idle
 * until enough had died for the rest to get their turns (Vita3K at ~30 FPS:
 * the same creatures fought with our AI and the engine's alike). Beside the
 * drawing the AI's levels loop costs the frame nothing as long as it ends
 * before the drawing does -- only the client sync after it waits for the
 * drawing -- so the budget may grow to AI_BUDGET_DRAW_PCT of the last drawing,
 * up to AI_BUDGET_MAX_US. */
static volatile uint32_t s_scale_permille = 1000;
static uint64_t s_last_render_us;
static uint32_t w_scale_sum, w_scale_n;
static int s_scale_on = AI_BUDGET_SCALE;
#if AI_BUDGET_ADAPT
/* AI_BUDGET_ADAPT: the budget multiplier (permille), set after each pass from
 * when its levels were done against the drawing's length (an average of the
 * last few): the AI uses the time the drawing leaves on core 1, no more. The
 * measure is wall time, so time off the CPU and at gates counts against it. */
static int s_adapt_on = AI_BUDGET_ADAPT_DEFAULT == '1';
static uint32_t s_adapt_pm = 1000;
static uint64_t s_draw_ema;
static void adapt_budget(uint64_t draw) {
  s_draw_ema = s_draw_ema ? (s_draw_ema * 3 + draw) / 4 : draw;
  /* A pass stopped mid-way at a gate waits out the drawing whatever its
   * budget: it says nothing about the budget. Cutting on those (25-39% of the
   * battle's passes without the served calls) kept the multiplier at x1. */
  if (s_p_mid_stop) return;
  uint64_t done = s_p_sync_t ? s_p_sync_t : s_p_us;
  uint32_t pm = s_adapt_pm;
  if (done * 100u > s_draw_ema * 80u) pm = pm * 3u / 4u;
  else if (done * 100u < s_draw_ema * 60u) pm = pm + pm / 10u + 20u;
  if (pm < 1000u) pm = 1000u;
  if (pm > AI_BUDGET_ADAPT_MAX_PM) pm = AI_BUDGET_ADAPT_MAX_PM;
  s_adapt_pm = pm;
}
#endif
#if AI_LEVERS_AB
static int s_levers_arm;             /* 0 BASE, 1 LEVERS (served calls, their free callers, adaptive budget) */
static const char *const k_levers_tag[2] = { "BASE ", "LEVERS " };
static void levers_apply(void) {
#if AI_SERVE
  s_serve_on = s_levers_arm == 1;
#endif
#if AI_BUDGET_ADAPT
  s_adapt_on = s_levers_arm == 1;
  s_adapt_pm = 1000;
#endif
}
#endif

static uint64_t worker_clock(uint64_t t) {
  if (!s_tc_live) {
    s_tc_live = 1;
    s_tc_base_t = s_tc_last_t = s_tc_out = t;
    s_tc_base_cpu = s_tc_last_cpu = worker_cpu_us();
    return t;
  }
  if (t - s_tc_last_t >= CPU_READ_US) {
#if AI_THREAD_DIAG
    uint64_t r0 = now_us();
    uint64_t c = worker_cpu_us();
    d_cpu_read_us += now_us() - r0;
    d_cpu_reads++;
#else
    uint64_t c = worker_cpu_us();
#endif
    if (c && c >= s_tc_last_cpu) s_tc_last_cpu = c;
    else s_tc_last_cpu += t - s_tc_last_t;          /* no CPU time from the kernel */
    s_tc_last_t = t;
  }
  /* Elapsed CPU time since the pass's first read, extrapolated by the real
   * clock since the last CPU read, divided by the scale (fixed for a pass).
   * Never back. A test build carried the growth from one pass into the next
   * (2026-11-05): the clock stood still for whole passes, no level ever ended
   * on its budget, and the AI updated ~300 objects a frame instead of 15-110. */
  uint64_t e = (s_tc_last_cpu - s_tc_base_cpu) + (t - s_tc_last_t);
  uint32_t pm = s_scale_permille;
  uint64_t v = s_tc_base_t + e * 1000u / (pm ? pm : 1000u);
  if (v < s_tc_out) v = s_tc_out;
  s_tc_out = v;
  return v;
}

/* UpdateState's call sites the level hooks key on (return addresses). */
#define US_T_START 0x4a7e7eu        /* clock read at a level's start */
#define US_EVP_TOP 0x4a8004u        /* EventPending in an iteration's event loop */
#define US_EVP     0x4a80d2u        /* EventPending after AIUpdate (or after the budget check) */
#if AI_THREAD_DIAG || AI_LEVEL_FAIR
static int (*s_evp)(void *master, uint32_t day, uint32_t time);
#endif

#if AI_LEVEL_FAIR
/* AI_LEVEL_FAIR: fair AI levels beside the drawing. UpdateState (libkotor2
 * 0x4a7dc4, read in the disassembly) runs each level 4..0 as a loop: the walk
 * pass, then while EventPending (0x4a8000) GetPendingEvent + dispatch, then the
 * budget check, GetNextObject + AIUpdate, a clock read, and EventPending again
 * (0x4a80ce): if an event is due the level ends. What a level used is measured
 * from its first clock read, so the events dispatched at its top count against
 * its share, and what it used comes off what is left for the levels below. In
 * a battle every enemy turn queues shouts (type-10 events, 66% of them from
 * CSWSObject::BroadcastDialog), so level 2 -- enemies in combat -- ended after
 * 1-2 creatures with most of its share spent on events, whatever the budget
 * (hardware, 2026-11-05: doubling the budget gave level 2 +30% and levels 1-0
 * the rest). On the worker:
 *  - levels 4-1 go on past a due event: the next iteration's event loop
 *    dispatches it before the next AIUpdate, the order the engine's next level
 *    would have given it (level 0 keeps the engine's rule);
 *  - the events' time is taken off the clock UpdateState reads (one offset,
 *    grown by each event loop's length: the clock stays monotonic), so a
 *    level's share buys AIUpdates. The events themselves run as before: all
 *    that are due, every pass.
 * The server-loop placement is the engine's own. */
static int s_fair_on = AI_LEVEL_FAIR_DEFAULT == '1';
static void *s_timers;               /* CExoTimers, from UpdateState's clock reads */
static int s_lvl_idx = -1;           /* levels started this pass: 0 = level 4 .. 4 = level 0 */
static int s_ev_in;
static uint64_t s_ev_c0, s_ev_off;
static uint32_t w_fair_cont;         /* levels kept going past a due event, this window */
static uint64_t w_fair_ev_us;        /* event time kept off the budget clock (worker clock) */
static void fair_pass_start(void) {
  s_lvl_idx = -1;
  s_ev_in = 0;
  s_ev_off = 0;
}
#endif

#if AI_THREAD_DIAG
/* AI_THREAD_DIAG (test builds): why each AI level ends, in both placements.
 * UpdateState (libkotor2 2.0.4, reconstructed) shares out 10000 us: each level,
 * 4 down to 0, gets 60% of what is left (level 0 all of it), reads the clock
 * once at its start, then loops: the walk pass, the due events, a budget
 * check, GetNextObject + AIUpdate, three clock reads, EventPending. A level
 * ends when its clock time passes its share (budget), when an event has come
 * due (event), or when the list wraps or runs out (wrap); what it used comes
 * off what is left. The probes key on the call sites' return addresses. */
#define US_T0      0x4a8038u        /* clock read before AIUpdate: one per iteration */
#define US_T_LAST  0x4a80b2u        /* clock read the budget check uses */
#define US_GPE     0x4a7f6au        /* GetPendingEvent in the event loop */
#define NTYPES 32
typedef struct { uint32_t iters, ex_budget, ex_event, ex_wrap, levels; uint64_t clock_us, wall_us, cpu_us; } dlvl_t;
static dlvl_t d_lvl[2][5];          /* [beside the drawing][level 4..0] */
static uint32_t d_passes[2], d_events[2], d_types[2][NTYPES];
static int d_arm, d_level = -1, d_last_ev, d_iter_started;
static uint64_t d_remaining, d_budget, d_start_clock, d_last_clock, d_wall0, d_cpu0;
static int (*s_gpe)(void *master, uint32_t *day, uint32_t *time, uint32_t *caller, uint32_t *target,
                    uint32_t *type, void **data);

/* Long passes (AI_LONG_PASS_US and over): the slowest AIUpdate and the slowest
 * event of the pass, logged with the pass's time span (the log's clock), so a
 * PC profile of the worker (PC_PROF_TARGET_AI) can be read for just those. */
#ifndef AI_LONG_PASS_US
#define AI_LONG_PASS_US 60000u
#endif
static uint64_t d_pass_t0, d_pass_cpu0, d_upd_t0, d_upd_max, d_ev_t0, d_ev_max;
#if BATTLE_LEDGER
static uint64_t d_ev_idle0, d_type_us[2][NTYPES], d_ev_us[2];   /* events' time, without waits at gates */
static uint32_t d_type_n[2][NTYPES];
#endif
static void *d_upd_obj, *d_upd_max_obj;
static uint32_t d_upd_n, d_ev_type, d_ev_max_type, d_ev_n, d_long_logged;

/* The class of an engine object, from its vtable's symbol. */
static const char *vt_name(void *obj) {
  if (!obj) return "-";
  uintptr_t vt = *(uintptr_t *)obj;
  if (vt < kotor_mod.text_base) return "?";
  uint32_t off = (uint32_t)(vt - kotor_mod.text_base);
  for (int i = 0; i < kotor_mod.num_dynsym; i++) {
    const Elf32_Sym *s = &kotor_mod.dynsym[i];
    if (ELF32_ST_TYPE(s->st_info) != STT_OBJECT || off < s->st_value || off >= s->st_value + s->st_size) continue;
    const char *n = kotor_mod.dynstr + s->st_name;
    if (!strncmp(n, "_ZTV", 4)) return n + 4;
  }
  return "?";
}
static void diag_event_close(void) {
  if (!d_ev_t0) return;
  uint64_t d = now_us() - d_ev_t0;
#if BATTLE_LEDGER
  {
    uint64_t idle = s_idle_total - d_ev_idle0, dd = d > idle ? d - idle : 0;
    uint32_t ty = d_ev_type < NTYPES ? d_ev_type : 0;
    d_type_us[d_arm][ty] += dd;
    d_type_n[d_arm][ty]++;
    d_ev_us[d_arm] += dd;
  }
#endif
  if (d > d_ev_max) { d_ev_max = d; d_ev_max_type = d_ev_type; }
  d_ev_t0 = 0;
}

static uint64_t thread_cpu_us(void) {
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  return sceKernelGetThreadInfo(0, &ti) >= 0 ? (uint64_t)ti.runClocks : 0;
}

static void diag_end_level(void) {
  if (d_level < 0 || d_level > 4) return;
  dlvl_t *L = &d_lvl[d_arm][d_level];
  uint64_t used = d_iter_started ? d_last_clock - d_start_clock : 0;
  if (used > d_budget) L->ex_budget++;
  else if (d_last_ev) L->ex_event++;
  else L->ex_wrap++;
  L->levels++;
  L->clock_us += used;
  L->wall_us += now_us() - d_wall0;
  L->cpu_us += thread_cpu_us() - d_cpu0;
  d_remaining -= used < d_budget ? used : d_budget;
}

static void diag_pass_start(int arm) {
  d_arm = arm;
  d_level = -1;
  d_remaining = ai_budget_us();
  d_passes[arm]++;
  d_pass_t0 = now_us();
  d_pass_cpu0 = thread_cpu_us();
  d_upd_t0 = d_upd_max = d_ev_t0 = d_ev_max = 0;
  d_upd_max_obj = NULL;
  d_upd_n = d_ev_n = 0;
}

static void diag_pass_end(void) {
  diag_end_level();
  diag_event_close();
  d_level = -1;
  uint64_t t1 = now_us(), wall = t1 - d_pass_t0;
  if (wall >= AI_LONG_PASS_US && d_long_logged < 400) {
    d_long_logged++;
    void *o = d_upd_max_obj;
    log_printf("[aithread] long pass %s: %.3f-%.3f s, %.1f ms wall, %.1f ms CPU; %u updates, slowest %.1f ms (%.40s "
               "id %08x); %u events, slowest %.1f ms (type %u)",
               d_arm ? "beside the drawing" : "in the server loop", d_pass_t0 / 1e6, t1 / 1e6, wall / 1000.0,
               (thread_cpu_us() - d_pass_cpu0) / 1000.0, (unsigned)d_upd_n, d_upd_max / 1000.0, vt_name(o),
               o ? (unsigned)((uint32_t *)o)[1] : 0u, (unsigned)d_ev_n, d_ev_max / 1000.0, (unsigned)d_ev_max_type);
  }
}

static void diag_clock(uint32_t off, uint64_t clock) {
  if (off == US_T_START || off == US_T0) diag_event_close();
  if (off == US_T0) {                     /* AIUpdate is next */
    d_upd_t0 = now_us();
    d_upd_obj = ai_throttle_cur_obj;
    d_upd_n++;
  } else if (off == US_T_LAST && d_upd_t0) {
    uint64_t d = now_us() - d_upd_t0;
    if (d > d_upd_max) { d_upd_max = d; d_upd_max_obj = d_upd_obj; }
    d_upd_t0 = 0;
  }
  if (off == US_T_START) {
    diag_end_level();
    d_level++;
    d_budget = d_level < 4 ? d_remaining * 60 / 100 : d_remaining;
    d_start_clock = d_last_clock = clock;
    d_wall0 = now_us();
    d_cpu0 = thread_cpu_us();
    d_last_ev = 0;
    d_iter_started = 0;
  } else if (off == US_T0) {
    if (d_level >= 0 && d_level <= 4) d_lvl[d_arm][d_level].iters++;
  } else if (off == US_T_LAST) {
    d_last_clock = clock;
    d_iter_started = 1;
  }
}

static int gpe_hook(void *master, uint32_t *day, uint32_t *time, uint32_t *caller, uint32_t *target,
                    uint32_t *type, void **data) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  int ours = ret >= s_upd_lo && ret < s_upd_hi;
  if (ours) diag_event_close();            /* the event before this one is done */
  int r = s_gpe(master, day, time, caller, target, type, data);
  if (r == 1 && ours) {
    battle_ledger_trace_event(type ? *type : 0, caller ? *caller : 0, target ? *target : 0);
    d_events[d_arm]++;
    d_types[d_arm][type && *type < NTYPES ? *type : 0]++;
    d_ev_t0 = now_us();
#if BATTLE_LEDGER
    d_ev_idle0 = s_idle_total;
#endif
    d_ev_type = type ? *type : 0;
    d_ev_n++;
  }
  return r;
}
#endif

static uint64_t timer_hook(void *timers) {
  uint64_t t = s_timer(timers);
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  if (ret < s_upd_lo || ret >= s_upd_hi) return t;
  uint32_t off = (uint32_t)((ret & ~(uintptr_t)1) - kotor_mod.text_base);
  if (on_worker()) {
    t = worker_clock(t);
#if AI_LEVEL_FAIR
    s_timers = timers;
    if (off == US_T_START) s_lvl_idx++;
    if (s_fair_on) t -= s_ev_off;       /* event time is not the levels' (AI_LEVEL_FAIR) */
#endif
  }
#if AI_THREAD_DIAG
  diag_clock(off, t);
#else
  (void)off;
#endif
  return t;
}

#if AI_THREAD_DIAG || AI_LEVEL_FAIR
/* CServerAIMaster::EventPending, from UpdateState: AI_LEVEL_FAIR's two changes
 * (above) and AI_THREAD_DIAG's record of why a level ended. */
static int evp_hook(void *master, uint32_t day, uint32_t time) {
  int r = s_evp(master, day, time);
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  if (ret < s_upd_lo || ret >= s_upd_hi) return r;
  uint32_t off = (uint32_t)((ret & ~(uintptr_t)1) - kotor_mod.text_base);
#if AI_LEVEL_FAIR
  if (s_fair_on && on_worker() && s_timers) {
    if (off == US_EVP_TOP) {
      if (r == 1 && !s_ev_in) {          /* the iteration's event loop starts */
        s_ev_in = 1;
        s_ev_c0 = worker_clock(s_timer(s_timers));
      } else if (r != 1 && s_ev_in) {    /* and ends: its time off the levels' clock */
        uint64_t d = worker_clock(s_timer(s_timers)) - s_ev_c0;
        s_ev_off += d;
        w_fair_ev_us += d;
        s_ev_in = 0;
      }
    } else if (off == US_EVP && r == 1 && s_lvl_idx >= 0 && s_lvl_idx <= 3) {
      w_fair_cont++;
      r = 0;                             /* the level goes on; its next iteration dispatches the event first */
    }
  }
#endif
#if AI_THREAD_DIAG
  if (off == US_EVP) d_last_ev = r;
#endif
  return r;
}
#endif

/* s_window is cleared before EV_CLOSED is set: a worker that has consumed
 * EV_CLOSED then sees the window closed at its next gate and does not wait for
 * a second EV_CLOSED that will not come. */
static void close_window(void) {
  if (!s_window) return;
  s_window = 0;
  __sync_synchronize();
  sceKernelSetEventFlag(s_ev, EV_CLOSED);
}

static void wait_done(void) {
  if (!s_pass_live) return;
  SceUInt timeout = 3000000;
  if (sceKernelWaitEventFlag(s_ev, EV_DONE, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, NULL,
                             &timeout) < 0) {
    int def = s_wait_def;
    log_printf("[aithread] the game thread has waited 3 s for the AI (worker %s%s); still waiting",
               def >= 0 ? "waiting at " : "running", def >= 0 ? k_ai_gates[def].name : "");
    sceKernelWaitEventFlag(s_ev, EV_DONE, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
  }
  /* The worker never waits after EV_DONE: take back an EV_CLOSED it did not
   * consume, so the next pass starts with the window open. */
  sceKernelPollEventFlag(s_ev, EV_CLOSED, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, NULL);
  s_pass_live = 0;
}

/* Every taken slot leads here, through its stub, with the call's arguments
 * saved on the stack. Returns where the call goes on. */
uintptr_t ai_gate_enter(unsigned stub, uintptr_t ret, const uint32_t *args) {
  int def = s_def[stub];
  unsigned fl = k_ai_gates[def].flags;
#if SCENE_TRACE
  /* Only where the AI thread acts unlike the engine (the worker inside the
   * drawing), and the dialog action wherever it runs. */
  if (battle_ledger_tracing() && !(def == s_gco_def && ret - kotor_mod.text_base == SYNC_RET) &&
      ((s_window && on_worker()) || def == s_dlgact_def)) {
    const char *what;
    int worker = on_worker();
    if (!worker) what = "game thread, goes on";
    else if (!s_window) what = "AI worker, drawing over, goes on";
    else if (!(fl & G_WAIT)) what = "AI worker, not a waiting gate";
    else {
      int d = gate_decision(def, ret, args, NULL);
      what = d == GD_FREE ? "AI worker, free" : d == GD_DEFER ? "AI worker, deferred" : "AI worker, waits for the drawing";
#if AI_SERVE
      if (d == GD_WAIT && s_serve_on && s_serve_stack[def] >= 0) what = "AI worker, served by the game thread";
#endif
    }
    battle_ledger_trace_gate(k_ai_gates[def].name, ret, what, (void *)(uintptr_t)args[0], def == s_dlgact_def);
  }
#endif
  if (def == s_gco_def && !s_p_sync_t && ret - kotor_mod.text_base == SYNC_RET && on_worker()) {
    s_p_sync_t = now_us() - s_p_t0;       /* the levels are done: UpdateState's client sync starts */
#if BATTLE_LEDGER
    s_l_sync_t = s_p_sync_t;
    uint64_t c = worker_cpu_us();         /* 0 where the kernel keeps no CPU time (Vita3K) */
    s_l_sync_cpu = c > s_l_cpu0 ? c - s_l_cpu0 : 0;
#endif
  }
  if (fl & G_EPOCH) {
    __atomic_add_fetch(&s_epoch, 1, __ATOMIC_SEQ_CST);
    w_epoch_hits[def]++;
  }
  if (!s_window) {
#if AI_GATE_CENSUS
    if ((fl & G_WAIT) && on_worker() && gate_decision(def, ret, args, NULL) == GD_WAIT) census_note(def, ret);
#endif
    if (s_nq && (fl & G_WAIT) && on_worker()) flush_deferred();   /* before this call */
  } else {
    if (on_worker()) {
      if (fl & G_WAIT) {
#if AI_THREAD_DIAG
        d_gate_passes++;
#endif
        int by_object = 1;
        int d = gate_decision(def, ret, args, &by_object);
        if (d == GD_DEFER && defer_call(stub, args, by_object)) return (uintptr_t)&ai_gate_ret0;
#if AI_SERVE
        if (d == GD_WAIT && s_serve_on && s_serve_stack[def] >= 0) {
          if (!queue_has_camera()) {
            s_srv_ret = serve_call(stub, args, s_serve_stack[def]);
            return (uintptr_t)&serve_ret;
          }
          w_serve_refused++;
        }
#endif
        if (d != GD_FREE) worker_wait(def, ret);
      }
    } else if (fl & G_JOIN) {
      /* The drawing needs the server: let the AI finish first. */
      uint64_t t = now_us();
      gl_worker_set_guest(s_worker_glkey);
      close_window();
      wait_done();
      gl_worker_set_guest(0);
      w_joins++;
      w_early_us += now_us() - t;
    }
  }
  return s_next[stub];
}

/* ---- The worker -------------------------------------------------------- */

static void *worker_main(void *arg) {
  (void)arg;
  SceKernelThreadInfo info;
  memset(&info, 0, sizeof info);
  info.size = sizeof info;
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t lo = 0, hi = 0;
  if (sceKernelGetThreadInfo(0, &info) >= 0 && info.stack && info.stackSize > 0) {
    lo = (uintptr_t)info.stack;
    hi = lo + (uintptr_t)info.stackSize;
  }
  if (!(sp >= lo && sp < hi)) {      /* the kernel's numbers do not hold: our own frame then */
    lo = sp - (WORKER_STACK - 16 * 1024);
    hi = sp + 1024;
  }
  sceKernelChangeThreadCpuAffinityMask(0, CPU_AUX_A);
  sceKernelChangeThreadPriority(0, AI_THREAD_PRIORITY);
  thread_census_add(sceKernelGetThreadId(), "ai-worker");
#if PC_PROF
  pc_prof_note_ai(sceKernelGetThreadId(), lo, hi);
#endif
  s_worker_glkey = gl_worker_tls_key();
#if BATTLE_LEDGER
  s_worker_thid = sceKernelGetThreadId();
  for (int k = 0; k < 5; k++) {              /* runClocks against the wall clock, least preempted of 5 */
    uint64_t c0 = worker_cpu_us(), t0 = now_us(), t1;
    while ((t1 = now_us()) - t0 < 10000u) {}
    uint64_t c1 = worker_cpu_us();
    float r = (float)(c1 - c0) / (float)(t1 - t0);
    if (r > s_clk_ratio) s_clk_ratio = r;
  }
#endif
  s_wstack_lo = lo;
  __sync_synchronize();
  s_wstack_hi = hi;
  int engine_tls = 0;
  for (;;) {
    sceKernelWaitEventFlag(s_ev, EV_GO, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
    if (!engine_tls) engine_tls = thread_engine_tls_init();   /* SetLastError & co. (threads.h) */
    s_p_waited = 0;
    s_p_before_us = 0;
    s_p_sync_t = 0;
    s_p_mid_stop = 0;
    s_tc_live = 0;                   /* UpdateState's first clock read starts the CPU clock */
#if AI_LEVEL_FAIR
    fair_pass_start();
#endif
#if BATTLE_LEDGER
    ledger_pass_start();
#endif
    uint64_t cpu0 = worker_cpu_us();
    s_p_t0 = now_us();
#if BATTLE_LEDGER
    s_l_cpu0 = cpu0 ? cpu0 : 1;
#endif
#if AI_THREAD_DIAG
    diag_pass_start(1);
#endif
    s_update(s_job);
#if AI_THREAD_DIAG
    diag_pass_end();
#endif
    flush_deferred();                /* the drawing is over by now, or EV_DONE waits */
    s_p_us = now_us() - s_p_t0;
    s_p_cpu_us = worker_cpu_us() - cpu0;
    sceKernelSetEventFlag(s_ev, EV_DONE);
  }
  return NULL;
}

/* ---- The two hooks ----------------------------------------------------- */

/* CServerAIMaster::UpdateState, from the server loop (its only caller). */
static void update_hook(void *master) {
  s_master = master;
  s_site_seen = 1;
  s_site_epoch = s_epoch;
  if (s_pass_done) {                 /* this frame's drawing ran it */
    w_skipped++;
    return;
  }
  uint64_t t = now_us();
#if AI_THREAD_DIAG
  diag_pass_start(0);
#endif
  s_update(master);
#if AI_THREAD_DIAG
  diag_pass_end();
#endif
  w_server_us += now_us() - t;
  w_server++;
}

/* Scene::Render (its vtable entry and its PLT slot). */
static void render_hook(void *scene) {
  int mode = cur_mode();
  if (!s_pinned) {
    /* Core 1 is the worker's: keep the game thread on core 0 in every mode, so
     * the A/B compares the AI's placement only (pinning alone changed nothing,
     * THREAD_PINNING_AB, 2026-10-15). */
    s_pinned = 1;
    sceKernelChangeThreadCpuAffinityMask(0, CPU_GAME);
  }
  if (mode == M_OFF || s_in_render || s_pass_done || !s_ready || s_ready_epoch != s_epoch ||
      !s_master) {
    s_render(scene);
    return;
  }
  s_in_render = 1;
  s_pass_done = 1;
  if (mode == M_INLINE) {
    uint64_t t = now_us();
#if AI_THREAD_DIAG
    diag_pass_start(1);
#endif
    s_update(s_master);
#if AI_THREAD_DIAG
    diag_pass_end();
#endif
    uint64_t d = now_us() - t;
    w_inline++;
    w_pass_us += d;
    if (d > w_pass_max) w_pass_max = d;
    s_render(scene);
  } else {
    s_job = s_master;
    s_pass_live = 1;
    s_window = 1;
#if BATTLE_LEDGER
    s_l_cpu0 = 0;
#endif
    {
      uint32_t pm = 1000;
      if (s_scale_on && s_last_render_us) {
        uint64_t target = s_last_render_us * AI_BUDGET_DRAW_PCT / 100u;
        if (target > AI_BUDGET_MAX_US) target = AI_BUDGET_MAX_US;
        if (target > 10000u) pm = (uint32_t)(target / 10u);
      }
#if AI_BUDGET_ADAPT
      if (s_adapt_on) pm = s_adapt_pm;
#endif
      s_scale_permille = pm;
      w_scale_sum += pm;
      w_scale_n++;
    }
#if BATTLE_LEDGER
    if (s_worker_thid >= 0) {
      uint64_t c = run_clocks(s_worker_thid, NULL);
      if (s_x_prev && s_x_fate >= 0 && c >= s_x_prev) {
        fate_t *F = &w_fate[s_x_fate];
        F->n_x++;
        F->cpu_x += c - s_x_prev;
        F->wall_x += s_x_wall;
        F->idle_x += s_x_idle;
      }
      s_x_prev = c;
      s_x_fate = -1;
    }
#endif
    __sync_synchronize();
    sceKernelSetEventFlag(s_ev, EV_GO);
    uint64_t t0 = now_us();
    s_render(scene);
    uint64_t t1 = now_us();
    s_last_render_us = t1 - t0;
#if BATTLE_LEDGER
    uint64_t cpu_draw_end = s_worker_thid >= 0 ? run_clocks(s_worker_thid, NULL) : 0;
#endif
    gl_worker_set_guest(s_worker_glkey);   /* before the end is signalled: no gap */
    close_window();
    wait_done();
    gl_worker_set_guest(0);
    uint64_t t2 = now_us();
    s_render_total += t2 - t0;
    w_thread++;
    w_render_us += t1 - t0;
    w_join_us += t2 - t1;
    if (t2 - t1 > w_join_max) w_join_max = t2 - t1;
    w_pass_us += s_p_us;
    w_cpu_us += s_p_cpu_us;
    if (s_p_us > w_pass_max) w_pass_max = s_p_us;
    if (s_p_waited) {
      w_waited++;
      w_before_us += s_p_before_us;
    } else {
      w_before_us += s_p_us;
    }
#if BATTLE_LEDGER
    ledger_frame(t1 - t0, t2 - t1, cpu_draw_end);
#endif
#if AI_BUDGET_ADAPT
    if (s_adapt_on) adapt_budget(t1 - t0);
#endif
  }
  s_in_render = 0;
}

/* ---- Per frame --------------------------------------------------------- */

static void log_window(uint64_t now) {
  const char *tag = s_ab ? (s_ab_on ? (s_mode == M_INLINE ? "INLINE " : "THREAD ") : "OFF ") : "";
#if AI_LEVERS_AB
  tag = k_levers_tag[s_levers_arm];
#endif
#if NWVM_AB
  tag = nwvm_phase_ours() ? "NWVM " : "ENGINE ";
#endif
  unsigned passes = w_thread + w_inline;
  char gates[200];
  int g = 0;
  gates[0] = 0;
  for (int k = 0; k < 5; k++) {      /* the gates that stopped most passes */
    int best = -1;
    for (int i = 0; i < NDEFS; i++)
      if (w_hits[i] && (best < 0 || w_hits[i] > w_hits[best])) best = i;
    if (best < 0 || g >= (int)sizeof gates - 8) break;
    g += snprintf(gates + g, sizeof gates - (size_t)g, " %s:%u", k_ai_gates[best].name,
                  (unsigned)w_hits[best]);
    w_hits[best] = 0;
  }
  char epochs[160];
  int e = 0;
  epochs[0] = 0;
  for (int i = 0; i < NDEFS && e < (int)sizeof epochs - 8; i++)
    if (w_epoch_hits[i]) {
      e += snprintf(epochs + e, sizeof epochs - (size_t)e, " %s:%u", k_ai_gates[i].name,
                    (unsigned)w_epoch_hits[i]);
      w_epoch_hits[i] = 0;
    }
  log_printf("[aithread] %s%u frames, %.1f ms (worst %.1f) | AI in the drawing %u frames%s: "
             "%.2f ms a pass (worst %.2f; %.2f ms on the CPU), %.2f ms before it waited or ended; waited at a gate %u; "
             "drawing %.2f ms, then waited %.2f ms (worst %.2f); early joins %u (%.2f ms) | "
             "AI in the server loop %u frames, %.2f ms; skipped there %u",
             tag, w_frames, w_frames ? (double)w_frame_us / w_frames / 1000.0 : 0.0,
             (double)w_frame_max / 1000.0, passes, w_inline ? " (on the game thread)" : "",
             passes ? (double)w_pass_us / passes / 1000.0 : 0.0, (double)w_pass_max / 1000.0,
             w_thread ? (double)w_cpu_us / w_thread / 1000.0 : 0.0,
             w_thread ? (double)w_before_us / w_thread / 1000.0 : 0.0, w_waited,
             w_thread ? (double)w_render_us / w_thread / 1000.0 : 0.0,
             w_thread ? (double)w_join_us / w_thread / 1000.0 : 0.0, (double)w_join_max / 1000.0,
             w_joins, (double)w_early_us / 1000.0, w_server,
             w_server ? (double)w_server_us / w_server / 1000.0 : 0.0, w_skipped);
  log_printf("[aithread] %sAI budget beside the drawing: x%.2f on average (%s, %u%% of the last drawing, at most "
             "%u ms)", tag, w_scale_n ? (double)w_scale_sum / w_scale_n / 1000.0 : 1.0,
             s_scale_on ? "scaled" : "the engine's 10 ms", (unsigned)AI_BUDGET_DRAW_PCT,
             (unsigned)(AI_BUDGET_MAX_US / 1000u));
  w_scale_sum = w_scale_n = 0;
#if AI_LEVEL_FAIR
  if (s_fair_on)
    log_printf("[aithread] %sfair levels: %.1f a frame went on past a due event; events kept off the budget "
               "%.2f ms a frame", tag, w_frames ? (double)w_fair_cont / w_frames : 0.0,
               w_frames ? (double)w_fair_ev_us / w_frames / 1000.0 : 0.0);
  w_fair_cont = 0;
  w_fair_ev_us = 0;
#endif
  if (g || e)
    log_printf("[aithread] waited at:%s | loads, saves, unloads:%s", g ? gates : " none",
               e ? epochs : " none");
  {                                  /* where in libkotor2 the passes stopped */
    char sites[240];
    int n = 0;
    sites[0] = 0;
    for (int k = 0; k < 6; k++) {
      int best = -1;
      for (int i = 0; i < NSITES; i++)
        if (w_sites[i].n && (best < 0 || w_sites[i].n > w_sites[best].n)) best = i;
      if (best < 0 || n >= (int)sizeof sites - 40) break;
      n += snprintf(sites + n, sizeof sites - (size_t)n, " %s<-%x:%u", k_ai_gates[w_sites[best].def].name,
                    (unsigned)(w_sites[best].ret - kotor_mod.text_base), (unsigned)w_sites[best].n);
      w_sites[best].n = 0;
    }
    if (n) log_printf("[aithread] stopped from:%s", sites);
    memset(w_sites, 0, sizeof w_sites);
  }
#if AI_GATE_CENSUS
  {
    char out[400];
    int n = 0;
    out[0] = 0;
    for (int k = 0; k < 10 && n < (int)sizeof out - 60; k++) {
      int best = -1;
      for (int i = 0; i < NCENSUS; i++)
        if (s_census[i].n && (best < 0 || s_census[i].n > s_census[best].n)) best = i;
      if (best < 0) break;
      n += snprintf(out + n, sizeof out - (size_t)n, " %s<-%x:%u", k_ai_gates[s_census[best].def].name,
                    (unsigned)((s_census[best].ret & ~(uintptr_t)1) - kotor_mod.text_base), (unsigned)s_census[best].n);
      s_census[best].n = 0;
    }
    if (n) log_printf("[aithread] gated calls after the first stop:%s%s", out, s_census_lost ? " (table full)" : "");
    memset(s_census, 0, sizeof s_census);
    s_census_lost = 0;
  }
#endif
#if AI_GATES_AB
  {
    static unsigned s_ab_windows;
    log_printf("[aithread] battle gate rules were %s in this window", s_newer_off ? "OFF" : "ON");
    if (++s_ab_windows % AI_GATES_AB_PERIOD == 0) s_newer_off = !s_newer_off;
  }
#endif
  if (w_deferred)
    log_printf("[aithread] deferred %u calls during the drawing, made %u after it", (unsigned)w_deferred,
               (unsigned)w_flushed);
  w_deferred = w_flushed = 0;
  if (w_gl_calls)
    log_printf("[aithread] the AI reached GL %u times while the scene was drawn (%u passes waited); "
               "recorded after the drawing, as the guest", (unsigned)w_gl_calls, (unsigned)w_gl_waits);
  w_gl_calls = w_gl_waits = 0;
#if AI_SERVE
  if (w_served || w_served_late || w_serve_refused)
    log_printf("[aithread] %sserved at the drawing's safe points: %u calls (%.1f a frame), the worker waited %.2f ms "
               "each (worst %.2f); %u made by the worker after the drawing ended; %u not served (camera reset "
               "queued); the game thread spent %.2f ms a frame making them",
               tag, (unsigned)w_served, w_frames ? (double)w_served / w_frames : 0.0,
               (w_served + w_served_late) ? (double)w_served_wait_us / (w_served + w_served_late) / 1000.0 : 0.0,
               w_served_wait_max / 1000.0, (unsigned)w_served_late, (unsigned)w_serve_refused,
               w_frames ? w_serve_us / (w_frames * 1000.0) : 0.0);
  w_served = w_served_late = w_serve_refused = 0;
  w_served_wait_us = w_served_wait_max = w_serve_us = 0;
#endif
#if BATTLE_LEDGER
  log_printf("[ledger] window: %s", tag[0] ? tag : "(no A/B)");
  ledger_log(w_frames);
  battle_ledger_report(w_frames);
  ai_census_report(tag);
#endif
#if AI_THREAD_DIAG
  for (int arm = 0; arm < 2; arm++) {
    if (!d_passes[arm]) continue;
#if BATTLE_LEDGER
    {
      char et[300];
      int q = 0;
      et[0] = 0;
      for (int k = 0; k < 8 && q < (int)sizeof et - 32; k++) {
        int best = -1;
        for (int i = 0; i < NTYPES; i++)
          if (d_type_us[arm][i] && (best < 0 || d_type_us[arm][i] > d_type_us[arm][best])) best = i;
        if (best < 0) break;
        q += snprintf(et + q, sizeof et - (size_t)q, " %d: %.2f a pass, %.2f ms", best,
                      (double)d_type_n[arm][best] / d_passes[arm], d_type_us[arm][best] / (d_passes[arm] * 1000.0));
        d_type_us[arm][best] = 0;
      }
      log_printf("[ledger] events %s: %.2f ms a pass (without waits at gates); by type:%s",
                 arm ? "beside the drawing" : "in the server loop", d_ev_us[arm] / (d_passes[arm] * 1000.0),
                 q ? et : " none");
      memset(d_type_us[arm], 0, sizeof d_type_us[arm]);
      memset(d_type_n[arm], 0, sizeof d_type_n[arm]);
      d_ev_us[arm] = 0;
    }
#endif
    char lv[400], ty[200];
    int n = 0, m = 0;
    lv[0] = ty[0] = 0;
    for (int l = 0; l < 5 && n < (int)sizeof lv - 90; l++) {
      dlvl_t *L = &d_lvl[arm][l];
      if (!L->levels) continue;
      n += snprintf(lv + n, sizeof lv - (size_t)n,
                    " | L%d %.1f iter, end b/e/w %u/%u/%u, clock %.2f cpu %.2f wall %.2f ms", 4 - l,
                    (double)L->iters / d_passes[arm], (unsigned)L->ex_budget, (unsigned)L->ex_event,
                    (unsigned)L->ex_wrap, (double)L->clock_us / d_passes[arm] / 1000.0,
                    (double)L->cpu_us / d_passes[arm] / 1000.0, (double)L->wall_us / d_passes[arm] / 1000.0);
    }
    for (int k = 0; k < 8 && m < (int)sizeof ty - 16; k++) {
      int best = -1;
      for (int i = 0; i < NTYPES; i++)
        if (d_types[arm][i] && (best < 0 || d_types[arm][i] > d_types[arm][best])) best = i;
      if (best < 0) break;
      m += snprintf(ty + m, sizeof ty - (size_t)m, " %d:%u", best, (unsigned)d_types[arm][best]);
      d_types[arm][best] = 0;
    }
    log_printf("[aithread] diag %s: %u passes, %.2f events a pass (types:%s)%s",
               arm ? "beside the drawing" : "in the server loop", (unsigned)d_passes[arm],
               (double)d_events[arm] / d_passes[arm], m ? ty : " none", lv);
  }
  if (d_passes[1])
    log_printf("[aithread] diag worker costs: %.1f CPU-time reads a pass, %.1f us each; %.0f gate checks a pass",
               (double)d_cpu_reads / d_passes[1], d_cpu_reads ? (double)d_cpu_read_us / d_cpu_reads : 0.0,
               (double)d_gate_passes / d_passes[1]);
  d_cpu_reads = d_gate_passes = 0;
  d_cpu_read_us = 0;
  memset(d_lvl, 0, sizeof d_lvl);
  memset(d_passes, 0, sizeof d_passes);
  memset(d_events, 0, sizeof d_events);
  memset(d_types, 0, sizeof d_types);
#endif
  for (int i = 0; i < NDEFS; i++) w_hits[i] = 0;
  w_frames = w_thread = w_inline = w_server = w_skipped = w_waited = w_joins = 0;
  w_frame_us = w_frame_max = w_pass_us = w_pass_max = w_before_us = w_render_us = w_join_us =
      w_join_max = w_server_us = w_early_us = w_cpu_us = 0;
  w_t0 = now;
  if (s_ab) s_ab_on = !s_ab_on;
#if AI_LEVERS_AB
  s_levers_arm = (s_levers_arm + 1) % 2;
  levers_apply();
#endif
#if AI_BUDGET_SCALE_AB
  ai_census_report(s_scale_on ? "SCALED " : "10MS ");
  s_scale_on = !s_scale_on;
#endif
#if NWVM_AB
  ai_census_report(nwvm_phase_ours() ? "NWVM " : "ENGINE ");
  nwvm_ab_window_end();
#endif
}

void ai_thread_on_swap(void) {
  if (!s_installed) return;
  uint64_t now = now_us();
  /* The next frame's drawing may run the AI if the server reached its AI in
   * this frame and the one before, with nothing loaded, saved or torn down since. */
  int steady = s_site_seen && s_site_epoch == s_epoch;
  s_steady = steady ? s_steady + 1 : 0;
  s_ready = s_steady >= 2;
  s_ready_epoch = s_epoch;
  s_site_seen = 0;
  s_pass_done = 0;
  if (w_prev_swap) {
    uint64_t d = now - w_prev_swap;
    w_frame_us += d;
    if (d > w_frame_max) w_frame_max = d;
    w_frames++;
  }
  w_prev_swap = now;
  if (!w_t0) w_t0 = now;
  if (now - w_t0 >= WINDOW_US) log_window(now);
}

/* ---- Install ----------------------------------------------------------- */

static uint32_t name_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

static int16_t s_hash[NHASH];        /* k_ai_gates index + 1, 0 empty */

static int lookup(const char *name) {
  for (uint32_t i = name_hash(name) & (NHASH - 1), n = 0; n < NHASH; i = (i + 1) & (NHASH - 1), n++) {
    int v = s_hash[i];
    if (!v) return -1;
    if (!strcmp(k_ai_gates[v - 1].sym, name)) return v - 1;
  }
  return -1;
}

/* [lo, hi) of an exported function, from the dynamic symbol table. */
static int sym_range(const char *name, uintptr_t *lo, uintptr_t *hi) {
  for (int i = 0; i < kotor_mod.num_dynsym; i++) {
    const Elf32_Sym *sym = &kotor_mod.dynsym[i];
    if (!sym->st_value || !sym->st_size || strcmp(kotor_mod.dynstr + sym->st_name, name)) continue;
    *lo = kotor_mod.text_base + (sym->st_value & ~1u);
    *hi = *lo + sym->st_size;
    return 1;
  }
  return 0;
}

static void patch(uintptr_t *slot, uintptr_t value) {
  kuKernelCpuUnrestrictedMemcpy(slot, &value, sizeof value);
}

void ai_thread_install(void) {
  char m[4] = {0};
  m[0] = AI_THREAD_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/ai_thread.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    char b[4] = {0};
    if (sceIoRead(fd, b, sizeof b - 1) >= 1) m[0] = b[0];
    sceIoClose(fd);
  }
  if (m[0] != '1' && m[0] != 'i' && m[0] != 'a') {
    log_printf("[aithread] off (ai_thread.txt '%c'): the AI runs in the server loop", m[0] ? m[0] : '0');
    return;
  }
  s_mode = m[0] == 'i' ? M_INLINE : M_THREAD;
  s_ab = m[0] == 'a';

  for (int d = 0; d < NDEFS; d++) {
    uint32_t i = name_hash(k_ai_gates[d].sym) & (NHASH - 1);
    while (s_hash[i]) i = (i + 1) & (NHASH - 1);
    s_hash[i] = (int16_t)(d + 1);
  }

  /* Find every slot first; take none unless all of it is there. */
  static uintptr_t *gate_slot[NSTUBS];
  static uint16_t gate_def[NSTUBS];
  uintptr_t *upd_slot[4], *ren_slot[4], *tim_slot[8];
  unsigned nupd = 0, nren = 0, ntim = 0, ngate = 0, overflow = 0;
#if AI_THREAD_DIAG || AI_LEVEL_FAIR
  uintptr_t *evp_slot[4];
  unsigned nevp = 0;
#endif
#if AI_THREAD_DIAG
  uintptr_t *gpe_slot[4];
  unsigned ngpe = 0;
#endif
  uint8_t found[NDEFS];
  memset(found, 0, sizeof found);
  for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
    Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
        &kotor_mod.relplt[i - kotor_mod.num_reldyn];
    unsigned type = ELF32_R_TYPE(rel->r_info);
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;
    const char *name = kotor_mod.dynstr + kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)].st_name;
    if (name[0] != '_' || name[1] != 'Z') continue;
    uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
    if (!strcmp(name, UPDATE_SYM)) {
      if (nupd < 4) upd_slot[nupd++] = slot;
      continue;
    }
    if (!strcmp(name, RENDER_SYM)) {
      if (nren < 4) ren_slot[nren++] = slot;
      continue;
    }
    if (!strcmp(name, TIMER_SYM)) {
      if (ntim < 8) tim_slot[ntim++] = slot;
      continue;
    }
#if AI_THREAD_DIAG || AI_LEVEL_FAIR
    if (!strcmp(name, "_ZN15CServerAIMaster12EventPendingEmm")) {
      if (nevp < 4) evp_slot[nevp++] = slot;
      continue;
    }
#endif
#if AI_THREAD_DIAG
    if (!strcmp(name, "_ZN15CServerAIMaster15GetPendingEventEPmS0_S0_S0_S0_PPv")) {
      if (ngpe < 4) gpe_slot[ngpe++] = slot;
      continue;
    }
#endif
    int d = lookup(name);
    if (d < 0) continue;
    if (ngate >= NSTUBS) { overflow++; continue; }
    gate_slot[ngate] = slot;
    gate_def[ngate] = (uint16_t)d;
    found[d] = 1;
    ngate++;
  }
  unsigned missing = 0;
  for (int d = 0; d < NDEFS; d++) missing += !found[d];
  int same = nupd && nren && ntim && sym_range(UPDATE_SYM, &s_upd_lo, &s_upd_hi);
  for (unsigned i = 1; i < nupd; i++) same &= *upd_slot[i] == *upd_slot[0];
  for (unsigned i = 1; i < nren; i++) same &= *ren_slot[i] == *ren_slot[0];
  for (unsigned i = 1; i < ntim; i++) same &= *tim_slot[i] == *tim_slot[0];
  if (!same || overflow) {
    log_printf("[aithread] DISABLED: UpdateState slots %u, Scene::Render slots %u, timer slots %u%s, "
               "gate slots %u (%u over)", nupd, nren, ntim, same ? "" : " (not all alike, or no "
               "UpdateState size)", ngate, overflow);
    return;
  }
  s_gco_def = lookup("_ZN10CSWSObject15GetClientObjectEv");
  s_dlgact_def = lookup("_ZN10CSWSObject20AIActionDialogObjectEP20CSWSObjectActionNode");
#if AI_SERVE
  memset(s_serve_stack, -1, sizeof s_serve_stack);
  for (unsigned k = 0; k < sizeof k_serve / sizeof k_serve[0]; k++) {
    int def = lookup(k_serve[k].sym);
    if (def >= 0) s_serve_stack[def] = (int8_t)k_serve[k].nstack;
    else log_printf("[aithread] served call %s not found: it waits", k_serve[k].sym);
  }
  s_camera_def = lookup("_ZN12CSWCCreature19ResetCameraToFollowEv");
#endif
#if AI_LEVERS_AB
  levers_apply();
#endif
  for (unsigned f = 0; f < sizeof k_free_from / sizeof k_free_from[0]; f++) {
    int def = lookup(k_free_from[f].gate);
    for (int c = 0; def >= 0 && c < NCALLERS && k_free_from[f].callers[c] && s_nfree < NFREE; c++) {
      uintptr_t lo, hi;
      if (!sym_range(k_free_from[f].callers[c], &lo, &hi)) {
        log_printf("[aithread] free caller %s not found: its calls wait", k_free_from[f].callers[c]);
        continue;
      }
      if (!s_free_n[def]) s_free_at[def] = (uint8_t)s_nfree;
      s_free_n[def]++;
      s_free[s_nfree].def = def;
      s_free[s_nfree].lo = lo;
      s_free[s_nfree].hi = hi;
      s_free[s_nfree].newer = is_newer(k_free_from[f].callers[c]);
      s_free[s_nfree].lever = is_lever(k_free_from[f].callers[c]);
      s_nfree++;
    }
  }
  for (unsigned f = 0; f < sizeof k_free_any / sizeof k_free_any[0]; f++) {
    int def = lookup(k_free_any[f]);
    if (def >= 0) s_free_any[def] = 1;
    else log_printf("[aithread] free gate %s not found", k_free_any[f]);
  }
  {
    /* ShowHelpForString's four strrefs: movw r0, #0xbe5c, then three from its
     * literal pool. Anything unexpected: every call waits, as before. */
    int def = lookup(HELP_SYM);
    const uint16_t *c = (const uint16_t *)(kotor_mod.text_base + HELP_FN);
    uintptr_t fn = so_symbol(&kotor_mod, HELP_SYM);
    if (def >= 0 && fn == kotor_mod.text_base + HELP_FN + 1 && c[2] == 0xf64b && c[3] == 0x605c) {
      const uint32_t *lit = (const uint32_t *)(kotor_mod.text_base + 0x347bacu);
      s_help_ref[0] = 0xbe5c;
      s_help_ref[1] = lit[0];
      s_help_ref[2] = lit[1];
      s_help_ref[3] = lit[2];
      s_help_def = def;
    } else {
      log_printf("[aithread] ShowHelpForString not as expected: all its calls wait");
    }
  }
  for (unsigned f = 0; f < sizeof k_defer / sizeof k_defer[0] && s_ndefer < NDEFER; f++) {
    int def = lookup(k_defer[f].gate);
    uintptr_t lo, hi;
    if (def < 0 || !sym_range(k_defer[f].caller, &lo, &hi)) {
      log_printf("[aithread] deferral %s from %s not set up: it waits", k_defer[f].gate, k_defer[f].caller);
      continue;
    }
    if (!s_defer_n[def]) s_defer_at[def] = (uint8_t)s_ndefer;
    s_defer_n[def]++;
    s_defer[s_ndefer].def = def;
    s_defer[s_ndefer].lo = lo;
    s_defer[s_ndefer].hi = hi;
    s_defer[s_ndefer].newer = is_newer(k_defer[f].caller);
    s_defer[s_ndefer].by_object = k_defer[f].by_object;
    s_defer[s_ndefer].lever = is_lever(k_defer[f].caller);
    s_ndefer++;
  }

  s_ev = sceKernelCreateEventFlag("ai_thread", SCE_EVENT_WAITMULTIPLE, 0, NULL);
  if (s_ev < 0) {
    log_printf("[aithread] DISABLED: event flag 0x%08X", (unsigned)s_ev);
    return;
  }
  s_update = (void (*)(void *))*upd_slot[0];
  s_render = (void (*)(void *))*ren_slot[0];
  s_timer = (uint64_t (*)(void *))*tim_slot[0];
  if (s_mode == M_THREAD) {
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK);
    if (pthread_create(&th, &attr, worker_main, NULL) != 0) {
      log_printf("[aithread] DISABLED: no worker thread");
      return;
    }
    while (!s_wstack_hi) sceKernelDelayThread(1000);
#if BATTLE_LEDGER
    log_printf("[ledger] armed: AI passes filed by what the AI did when the drawing ended; worker CPU clock %.3f "
               "per us (busy spin); GetClientObject gate %d", (double)s_clk_ratio, s_gco_def);
#endif
  }

  for (unsigned i = 0; i < ngate; i++) {
    s_next[i] = *gate_slot[i];
    s_def[i] = gate_def[i];
  }
  s_nstubs = ngate;
  __sync_synchronize();
  for (unsigned i = 0; i < ngate; i++) patch(gate_slot[i], (uintptr_t)ai_gate_stubs + 8u * i);
  for (unsigned i = 0; i < nupd; i++) patch(upd_slot[i], (uintptr_t)&update_hook);
  for (unsigned i = 0; i < nren; i++) patch(ren_slot[i], (uintptr_t)&render_hook);
  for (unsigned i = 0; i < ntim; i++) patch(tim_slot[i], (uintptr_t)&timer_hook);
  if (s_mode == M_THREAD) glw_foreign_hook = gl_foreign;
#if AI_LEVEL_FAIR && !AI_THREAD_DIAG
  /* The call sites the hook keys on are libkotor2 2.0.4's: anywhere else, off. */
  if (nevp && s_upd_lo == kotor_mod.text_base + 0x4a7dc4u) {
    s_evp = (int (*)(void *, uint32_t, uint32_t))*evp_slot[0];
    for (unsigned i = 0; i < nevp; i++) patch(evp_slot[i], (uintptr_t)&evp_hook);
  } else {
    s_fair_on = 0;
  }
#endif
#if AI_THREAD_DIAG
  if (nevp && ngpe && s_upd_lo == kotor_mod.text_base + 0x4a7dc4u) {
    s_evp = (int (*)(void *, uint32_t, uint32_t))*evp_slot[0];
    s_gpe = (int (*)(void *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, void **))*gpe_slot[0];
    for (unsigned i = 0; i < nevp; i++) patch(evp_slot[i], (uintptr_t)&evp_hook);
    for (unsigned i = 0; i < ngpe; i++) patch(gpe_slot[i], (uintptr_t)&gpe_hook);
    log_printf("[aithread] diag armed: level ends and events, EventPending %u slots, GetPendingEvent %u",
               nevp, ngpe);
  } else {
#if AI_LEVEL_FAIR
    s_fair_on = 0;
#endif
    log_printf("[aithread] diag NOT armed (UpdateState at +%x, slots %u/%u)",
               (unsigned)(s_upd_lo - kotor_mod.text_base), nevp, ngpe);
  }
#endif
#if AI_LEVEL_FAIR
  log_printf("[aithread] fair AI levels %s (EventPending %u slots)", s_fair_on ? "on" : "off", nevp);
#endif
  s_installed = 1;
  log_printf("[aithread] levers: served attack-animation calls %s, adaptive budget %s%s", 
#if AI_SERVE
             s_serve_on ? "on" : "off",
#else
             "not built",
#endif
#if AI_BUDGET_ADAPT
             s_adapt_on ? "on" : "off",
#else
             "not built",
#endif
             AI_LEVERS_AB ? "; A/B: BASE and LEVERS (both, with their free callers) in turn, 10 s each" : "");
  log_printf("[aithread] armed: %s%s; %u gate slots for %d functions (%u not in this build), "
             "UpdateState %u slots, Scene::Render %u, timer %u; %d free callers, %d deferred; worker stack %08x-%08x",
             s_mode == M_INLINE ? "the AI at the start of the drawing, on the game thread" :
                                  "the AI on core 1 while the scene is drawn",
             s_ab ? ", alternating with the server loop every 10 s" : "", ngate, NDEFS - (int)missing,
             missing, nupd, nren, ntim, s_nfree, s_ndefer, (unsigned)s_wstack_lo, (unsigned)s_wstack_hi);
}

/* One stub per taken slot: r12 = its index, then the common part saves the
 * arguments (r0-r3; the engine is softfp, so none travel in VFP registers),
 * asks ai_gate_enter (with the caller's return address and the saved
 * arguments) where to go, and
 * jumps there with lr untouched, so the
 * target returns straight to the original caller. 24 bytes pushed keep the
 * stack 8-byte aligned for the call. */
#define AI_STR2(x) #x
#define AI_STR(x) AI_STR2(x)
__asm__(
    ".pushsection .text.ai_gate_stubs, \"ax\", %progbits\n"
    ".syntax unified\n"
    ".arm\n"
    ".balign 4\n"
    ".global ai_gate_stubs\n"
    "ai_gate_stubs:\n"
    ".set ai_gate_i, 0\n"
    ".rept " AI_STR(NSTUBS) "\n"
    "  movw ip, #ai_gate_i\n"
    "  b ai_gate_common\n"
    "  .set ai_gate_i, ai_gate_i + 1\n"
    ".endr\n"
    "ai_gate_common:\n"
    "  push {r0-r3, ip, lr}\n"
    "  mov r0, ip\n"
    "  mov r1, lr\n"
    "  mov r2, sp\n"
    "  bl ai_gate_enter\n"
    "  mov ip, r0\n"
    "  pop {r0-r3}\n"
    "  add sp, sp, #4\n"
    "  pop {lr}\n"
    "  bx ip\n"
    ".popsection\n"
#if defined(__thumb__)
    ".thumb\n"
#endif
);

int ai_thread_on_worker(void) { return on_worker(); }
uint64_t ai_thread_render_us(void) { return s_render_total; }
uint64_t ai_thread_idle_us(void) {
#if BATTLE_LEDGER
  return s_idle_total;
#else
  return 0;
#endif
}

#else
void ai_thread_install(void) {}
void ai_thread_on_swap(void) {}
int ai_thread_on_worker(void) { return 0; }
uint64_t ai_thread_idle_us(void) { return 0; }
uint64_t ai_thread_render_us(void) { return 0; }
void ai_thread_safe_point(void) {}
#endif
