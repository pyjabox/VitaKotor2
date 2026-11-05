/* battle_ledger.c -- BATTLE_LEDGER (test builds): where a busy fight's frame
 * time goes, with timers instead of PC samples.
 *
 * The Khoonda battle runs at 8 FPS on hardware: a 121 ms frame is 29 ms before
 * the drawing, 64 ms of drawing and 28 ms waiting for the AI pass (logs of
 * 2026-11-05). A PC profile of the AI worker put about half of the pass in
 * scripts run by queued events (CSWSCreature::EventHandler), which
 * UpdateState runs without a budget, and a quarter in creatures' AIUpdate --
 * but 55-60% of those samples land on branch targets, so its self times are
 * unreliable. This measures instead, on the AI worker, without the time it
 * spends waiting at gates (ai_thread_idle_us):
 *  - every script run, by name: its own time and, for runs not inside another
 *    script, what started it (an event handler, the heartbeat/perception, the
 *    rest of AIUpdate, an action's DoCommand, a delayed command, other).
 *    Script runs meet at CVirtualMachineInternal::RunScriptFile (the
 *    relocation slot its two callers use), whose script is the top of the VM's
 *    script stack: depth at VM+0x28, 40-byte CVirtualMachineScripts from
 *    VM+0x2c, name (CExoString) at +24. CVirtualMachine::RunScript (97 call
 *    sites) says who asked for it; CVirtualMachine::RunScriptSituation marks a
 *    DoCommand action, and a situation run without it is a delayed command (a
 *    timed event);
 *  - the parts of a creature's AIUpdate that go through relocation slots:
 *    perception (SpawnInHeartbeatPerception), the mine check, the action
 *    queue, RunActions (every object's), the combat round timer, effects, combat
 *    information, the AI state;
 * and, on the game thread, the client and server main loops, plus the cost of
 * recording a GL command for the GL worker (gl_worker.c samples one in 64).
 * The AI thread (ai_thread.c) adds where each pass's wall time went and what
 * the AI was doing when the drawing ended; ai_census.c the AIUpdates per AI
 * level. All of it is logged at the end of the AI thread's 10 s windows. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ai_thread.h"
#include "gl_worker.h"
#include "battle_ledger.h"
#include "nwvm.h"

#if BATTLE_LEDGER

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static uintptr_t take_slots(const char *sym, uintptr_t repl) {
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
    if (pass == 0 && (!n || bad)) return 0;
  }
  return callable;
}

static int sym_range(const char *name, uintptr_t *lo, uintptr_t *hi) {
  for (int i = 0; i < kotor_mod.num_dynsym; i++) {
    const Elf32_Sym *s = &kotor_mod.dynsym[i];
    if (!s->st_value || !s->st_size || strcmp(kotor_mod.dynstr + s->st_name, name)) continue;
    *lo = kotor_mod.text_base + (s->st_value & ~1u);
    *hi = *lo + s->st_size;
    return 1;
  }
  return 0;
}

/* The exported function containing `addr`, as "Class::Method" (or the raw name). */
static void fn_name(uintptr_t addr, char *out, size_t n) {
  uint32_t off = (uint32_t)((addr & ~(uintptr_t)1) - kotor_mod.text_base);
  const char *best = NULL;
  for (int i = 0; i < kotor_mod.num_dynsym; i++) {
    const Elf32_Sym *s = &kotor_mod.dynsym[i];
    if (ELF32_ST_TYPE(s->st_info) != STT_FUNC || !s->st_size) continue;
    uint32_t lo = s->st_value & ~1u;
    if (off >= lo && off < lo + s->st_size) { best = kotor_mod.dynstr + s->st_name; break; }
  }
  if (!best) { snprintf(out, n, "+%x", (unsigned)off); return; }
  if (strncmp(best, "_ZN", 3)) { snprintf(out, n, "%s", best); return; }
  /* _ZN <len><id>... E: join the ids with "::" */
  const char *p = best + 3;
  size_t o = 0;
  out[0] = 0;
  while (*p >= '0' && *p <= '9' && o + 4 < n) {
    unsigned len = 0;
    while (*p >= '0' && *p <= '9') len = len * 10 + (unsigned)(*p++ - '0');
    if (o) { out[o++] = ':'; out[o++] = ':'; }
    for (unsigned k = 0; k < len && *p && o + 1 < n; k++) out[o++] = *p++;
    out[o] = 0;
  }
  if (!o) snprintf(out, n, "%s", best);
}

/* ---- Scripts ------------------------------------------------------------ */

enum { C_EVENT, C_PERCEPTION, C_AIUPDATE, C_DOCOMMAND, C_DELAYED, C_EXECSCRIPT, C_OTHER, C_N };
static const char *const k_cat[C_N] = { "event handlers", "heartbeat/perception", "other AIUpdate", "DoCommand actions",
                                        "delayed commands", "ExecuteScript", "other" };

#define NSITE 160
static struct { uintptr_t ret; uint8_t cat; char fn[48]; uint64_t us; uint32_t n; } s_site[NSITE];
static int s_nsite;

static int site_of(uintptr_t ret) {
  for (int i = 0; i < s_nsite; i++) if (s_site[i].ret == ret) return i;
  if (s_nsite == NSITE) return -1;
  int i = s_nsite;
  s_site[i].ret = ret;
  fn_name(ret, s_site[i].fn, sizeof s_site[i].fn);
  const char *f = s_site[i].fn;
  s_site[i].cat = strstr(f, "EventHandler") ? C_EVENT : strstr(f, "SpawnInHeartbeatPerception") ? C_PERCEPTION :
                  strstr(f, "AIUpdate") ? C_AIUPDATE : strstr(f, "ExecuteCommandExecuteScript") ? C_EXECSCRIPT : C_OTHER;
  s_site[i].us = 0;
  s_site[i].n = 0;
  __sync_synchronize();
  s_nsite = i + 1;
  return i;
}

#define NS 256
typedef struct { char name[17]; uint32_t runs, top; uint64_t self_us, top_us; } sstat_t;
static sstat_t s_ss[NS];
static uint32_t s_ss_full;

static sstat_t *script_slot(const char *name) {
  uint32_t h = 2166136261u;
  for (int k = 0; k < 16 && name[k]; k++) h = (h ^ (uint8_t)name[k]) * 16777619u;
  for (uint32_t k = 0; k < NS; k++) {
    sstat_t *e = &s_ss[(h + k) & (NS - 1)];
    if (!e->name[0]) { strncpy(e->name, name, 16); return e; }
    if (!strncmp(e->name, name, 16)) return e;
  }
  s_ss_full++;
  return NULL;
}

/* Worker only: the RunScript calls and script runs in progress. */
#define NEST 24
static int s_rs_site[NEST], s_rs_depth;
static struct { uint64_t t0, idle0, child; char name[17]; } s_fr[NEST];
static int s_depth, s_docmd;
static uint64_t w_cat_us[C_N];
static uint32_t w_cat_n[C_N];
static uint64_t w_game_us;            /* outermost script runs on other threads (the game thread) */
static uint32_t w_game_n;
static int s_game_depth;

typedef int (*runscript_t)(void *vm, void *name, uint32_t oid, int a);
static runscript_t o_runscript;
static int h_runscript(void *vm, void *name, uint32_t oid, int a) {
  if (nwvm_dialogue_skip(name, oid, (uintptr_t)__builtin_return_address(0))) return 1;
  if (!ai_thread_on_worker()) return o_runscript(vm, name, oid, a);
  int si = site_of((uintptr_t)__builtin_return_address(0));
  if (s_rs_depth < NEST) s_rs_site[s_rs_depth] = si;
  s_rs_depth++;
  int r = o_runscript(vm, name, oid, a);
  s_rs_depth--;
  return r;
}

typedef int (*rss_t)(void *vm, void *script, uint32_t oid, int a);
static rss_t o_rss;
static int h_rss(void *vm, void *script, uint32_t oid, int a) {
  if (!ai_thread_on_worker()) return o_rss(vm, script, oid, a);
  s_docmd++;
  int r = o_rss(vm, script, oid, a);
  s_docmd--;
  return r;
}

#if SCRIPT_DUMP
/* Bytecode to ux0:data/kotor2/dump/<name>.ncs, once a name. */
#define DUMP_DIR DATA_PATH "/dump"
static char s_dumped[512][17];
static int s_ndumped;
static volatile int s_dump_lock;
static void dump_code(const char *name, const uint8_t *code, uint32_t size) {
  if (!name || !name[0] || !code || !size || size > (4u << 20)) return;
  while (__sync_lock_test_and_set(&s_dump_lock, 1)) {}
  int seen = 0;
  for (int i = 0; i < s_ndumped && !seen; i++) seen = !strncmp(s_dumped[i], name, 16);
  if (!seen && s_ndumped < 512) {
    strncpy(s_dumped[s_ndumped], name, 16);
    s_dumped[s_ndumped++][16] = 0;
  }
  __sync_lock_release(&s_dump_lock);
  if (seen) return;
  char path[96];
  snprintf(path, sizeof path, DUMP_DIR "/%.16s.ncs", name);
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, code, size);
  sceIoClose(fd);
}
#endif

#if SCENE_TRACE
/* SCENE_TRACE (test builds): a scripted scene, step by step. Built for the
 * Khoonda retreat (2026-11-05): k_mil_death runs a_mil_retreat, which fades
 * to black until a script fades back in, then after DelayCommand(0.5) has
 * Zherron ActionStartConversation("zherron"); a line of that dialog runs
 * a_retreat_cs, which fades in. On the Vita it stayed black once. Logged:
 *  - story scripts (not the AI's own k_ai_/k_def_/k_hen_...), with OBJECT_SELF,
 *    always (25 a name before the scene);
 *  - from a_mil_retreat on, for TRACE_WINDOW_US: the engine commands those
 *    scripts make with their first arguments, AI events queued and fired,
 *    ClearAllActions with their caller, the dialog action's result,
 *    StartDialog, its conditional scripts and start entry, StopDialog, and
 *    every 2 s Zherron's and the PC's state;
 *  - the global fade's state whenever it changes. */
#define TRACE_MAX_LINES 6000u
#define TRACE_WINDOW_US 150000000ull
#define INVALID_OID 0x7f000000u
static volatile uint32_t s_tr_lines;
static volatile uint64_t s_tr_until;
static volatile uint32_t s_zherron = INVALID_OID;
static uintptr_t s_app_mgr, s_gvm;
static void *(*f_get_obj)(void *app, uint32_t oid);
static uint32_t (*f_pc_id)(void *app);
#define TR(...) do { if (s_tr_lines < TRACE_MAX_LINES) { s_tr_lines++; log_printf("[trace] " __VA_ARGS__); } } while (0)
static int trace_on(void) { return s_tr_until && sceKernelGetProcessTimeWide() < s_tr_until; }
static int noisy(const char *n) {
  return !strncmp(n, "k_ai_", 5) || !strncmp(n, "k_def_", 6) || !strncmp(n, "k_hen_", 6) || !strncmp(n, "k_oei_", 6) ||
         !strncmp(n, "k_sp1_", 6) || !strncmp(n, "k_trp_", 6) || !strncmp(n, "k_sup_", 6) || !strncmp(n, "k_wound", 7);
}
static void *server_app(void) {
  void *am = s_app_mgr ? *(void **)s_app_mgr : NULL;
  return am ? *(void **)((uint8_t *)am + 8) : NULL;
}
static uint8_t *sws_object(uint32_t oid) {
  void *app = server_app();
  if (!app || !f_get_obj || oid == INVALID_OID) return NULL;
  void *go = f_get_obj(app, oid);
  if (!go) return NULL;
  void *(*as)(void *) = (*(void *(***)(void *))go)[0x14 / 4];
  return as ? (uint8_t *)as(go) : NULL;
}
static const char *tag_of(uint32_t oid, char *buf, size_t n) {
  if (oid == INVALID_OID) { snprintf(buf, n, "INVALID"); return buf; }
  uint8_t *o = sws_object(oid);
  const char *t = o ? *(const char **)(o + 0x18) : NULL;
  snprintf(buf, n, "%.24s(%x)", t && t[0] ? t : "?", (unsigned)oid);
  if (t && !strcmp(t, "npc_zherron")) s_zherron = oid;
  return buf;
}
static uint8_t *vm_internal(void) {
  void *cvm = s_gvm ? *(void **)s_gvm : NULL;
  return cvm ? *(uint8_t **)((uint8_t *)cvm + 28) : NULL;
}
static const char *vm_script(uint8_t *vm) {
  int d = vm ? *(int *)(vm + 0x28) : -1;
  return d >= 0 && d < 8 ? *(const char **)(vm + 0x2c + d * 40 + 24) : NULL;
}
static void arm_trace(const char *why) {
  if (!trace_on()) TR("ARMED by %s: detailed trace for %u s", why, (unsigned)(TRACE_WINDOW_US / 1000000u));
  s_tr_until = sceKernelGetProcessTimeWide() + TRACE_WINDOW_US;
}
/* Watchdog: a_mil_retreat's first run and the first StartDialog after it. */
static volatile uint64_t s_retreat_t, s_dialog_t;
static volatile int s_wd_stage;
static struct { char n[17]; uint32_t c; } s_tr_names[96];
static int name_budget(const char *n) {        /* lines a name may still take before the scene */
  for (int i = 0; i < 96; i++) {
    if (!s_tr_names[i].n[0]) { snprintf(s_tr_names[i].n, sizeof s_tr_names[i].n, "%.16s", n); s_tr_names[i].c = 1; return 1; }
    if (!strncmp(s_tr_names[i].n, n, 16)) return ++s_tr_names[i].c <= 25;
  }
  return 0;
}
/* Lines a script may take inside the window: the scene's own scripts all of
 * theirs, others (a_check_mine runs on every heartbeat) 80. */
static struct { char n[17]; uint32_t c; } s_win_names[128];
static int window_budget(const char *n) {
  if (!strcmp(n, "a_mil_retreat") || !strcmp(n, "a_retreat_cs") || !strcmp(n, "k_mil_death") || !strncmp(n, "tr_", 3))
    return 1;
  for (int i = 0; i < 128; i++) {
    if (!s_win_names[i].n[0]) { snprintf(s_win_names[i].n, sizeof s_win_names[i].n, "%.16s", n); s_win_names[i].c = 1; return 1; }
    if (!strncmp(s_win_names[i].n, n, 16)) return ++s_win_names[i].c <= 80;
  }
  return 0;
}
static void trace_script(uint8_t *vmi, const char *nm, int depth, int start, int r) {
  if (!nm || !nm[0] || noisy(nm)) return;
  if (start && (!strcmp(nm, "a_mil_retreat") || !strcmp(nm, "k_battle_death") || !strcmp(nm, "a_retreat_cs")))
    arm_trace(nm);
  if (start && !strcmp(nm, "a_mil_retreat") && depth >= 1) {   /* the run itself; its closures run at depth 0 */
    s_retreat_t = sceKernelGetProcessTimeWide();
    s_dialog_t = 0;
    s_wd_stage = 0;
  }
  int on = trace_on();
  if (!on && !(start ? name_budget(nm) : 0)) return;
  if (on && !window_budget(nm)) return;
  uint32_t self = *(uint32_t *)(vmi + 0x18c + depth * 4);
  char tb[48];
  if (start) TR("run %s self=%s on %s, depth %d", nm, tag_of(self, tb, sizeof tb),
                ai_thread_on_worker() ? "the AI worker" : "the game thread", depth);
  else if (on) TR("end %s -> %d", nm, r);
}
static const char *s_cmd_name[880];
static int (*o_exec_cmd)(void *impl, int cmd, int argc);
static int h_exec_cmd(void *impl, int cmd, int argc) {
  if (trace_on() && s_tr_lines < TRACE_MAX_LINES) {
    uint8_t *vm = vm_internal();
    const char *sn = vm_script(vm);
    /* Story scripts: every command. The AI's own scripts (k_ai_master and
     * friends) only when they run on Zherron, and only the commands that can
     * replace his queued conversation (actions, ClearAllActions, combat). */
    int zh = 0;
    if (sn && noisy(sn) && s_zherron != INVALID_OID) {
      int dd = *(int *)(vm + 0x28);
      zh = dd >= 0 && dd < 8 && *(uint32_t *)(vm + 0x18c + dd * 4) == s_zherron;
    }
    if (sn && (zh || !noisy(sn)) && cmd >= 0 && cmd < 880 && (zh || window_budget(sn))) {
      if (!s_cmd_name[cmd]) {
        static char names[880][40];
        const uint32_t *tab = *(const uint32_t *const *)((uint8_t *)impl + 12);
        char full[96];
        fn_name(tab[2 * cmd], full, sizeof full);
        const char *p = strstr(full, "ExecuteCommand");
        snprintf(names[cmd], sizeof names[cmd], "%.39s", p ? p + 14 : full);
        s_cmd_name[cmd] = names[cmd];
      }
      if (zh) {
        static uint32_t zn;
        const char *c = s_cmd_name[cmd];
        if (!(strstr(c, "Action") || strstr(c, "Clear") || strstr(c, "Conversation") || strstr(c, "Combat") ||
              strstr(c, "Commandable") || strstr(c, "Faction")) || zn++ >= 300)
          return o_exec_cmd(impl, cmd, argc);
      }
      char args[200];
      int o = 0;
      args[0] = 0;
      int32_t sp = *(int32_t *)(vm + 0x1ac);
      int8_t *ty = *(int8_t **)(vm + 0x1ac + 12);
      int32_t *va = *(int32_t **)(vm + 0x1ac + 16);
      for (int k = 0; k < argc && k < 4 && sp - 1 - k >= 0 && ty && va && o < 160; k++) {
        int i = sp - 1 - k;
        char tb[48];
        switch (ty[i]) {
          case 3: o += snprintf(args + o, sizeof args - o, " %d", (int)va[i]); break;
          case 4: { float f; memcpy(&f, &va[i], 4); o += snprintf(args + o, sizeof args - o, " %g", (double)f); break; }
          case 5: { const char *s = va[i] ? *(const char **)(uintptr_t)va[i] : NULL;
                    o += snprintf(args + o, sizeof args - o, " \"%.24s\"", s ? s : ""); break; }
          case 6: o += snprintf(args + o, sizeof args - o, " %s", tag_of((uint32_t)va[i], tb, sizeof tb)); break;
          default: o += snprintf(args + o, sizeof args - o, " t%d", ty[i]); break;
        }
      }
      char tb[48];
      int d = *(int *)(vm + 0x28);
      TR("  %s: %s(%d args:%s) self=%s", sn, s_cmd_name[cmd], argc, args,
         tag_of(d >= 0 && d < 8 ? *(uint32_t *)(vm + 0x18c + d * 4) : INVALID_OID, tb, sizeof tb));
    }
  }
  return o_exec_cmd(impl, cmd, argc);
}
static int (*o_dlgact)(void *obj, void *node);
static int h_dlgact(void *obj, void *node) {
  int r = o_dlgact(obj, node);
  static uint32_t n;
  static int last = -99;
  if (r != last || n < 40) {
    n++;
    last = r;
    char tb[48];
    int (*dead)(void *) = (*(int (***)(void *))obj)[0x98 / 4];
    TR("AIActionDialogObject owner=%s dead=%d -> %d (call %u)", tag_of(*(uint32_t *)((uint8_t *)obj + 4), tb, sizeof tb),
       dead ? dead(obj) : -1, r, (unsigned)n);
  }
  return r;
}
static int (*o_startdlg)(void *obj, uint32_t target, const void *resref, int a);
static int h_startdlg(void *obj, uint32_t target, const void *resref, int a) {
  int r = o_startdlg(obj, target, resref, a);
  if (r && s_retreat_t && !s_dialog_t) s_dialog_t = sceKernelGetProcessTimeWide();
  char t1[48], t2[48];
  const char *s = resref ? *(const char *const *)resref : NULL;
  TR("StartDialog owner=%s target=%s dialog \"%.24s\" %d -> %d", tag_of(*(uint32_t *)((uint8_t *)obj + 4), t1, sizeof t1),
     tag_of(target, t2, sizeof t2), s ? s : "", a, r);
  return r;
}
static int (*o_startentry)(void *dlg, void *obj);
static int h_startentry(void *dlg, void *obj) {
  int r = o_startentry(dlg, obj);
  char tb[48];
  TR("GetStartEntry for %s -> %d", obj ? tag_of(*(uint32_t *)((uint8_t *)obj + 4), tb, sizeof tb) : "-", r);
  return r;
}
static int (*o_checkscript)(void *dlg, void *obj, const void *ref);
static int h_checkscript(void *dlg, void *obj, const void *ref) {
  int r = o_checkscript(dlg, obj, ref);
  if (trace_on()) {
    char tb[48];
    TR("  dialog condition %.16s for %s -> %d", ref ? (const char *)ref : "", obj ? tag_of(*(uint32_t *)((uint8_t *)obj + 4),
       tb, sizeof tb) : "-", r);
  }
  return r;
}
static int (*o_stopdlg)(void *obj);
static int h_stopdlg(void *obj) {
  char tb[48];
  TR("StopDialog owner=%s", tag_of(*(uint32_t *)((uint8_t *)obj + 4), tb, sizeof tb));
  return o_stopdlg(obj);
}
static int (*o_clearall)(void *obj, int a);
static int h_clearall(void *obj, int a) {
  if (trace_on()) {
    static uint32_t n;
    if (n++ < 400) {
      char tb[48], fn[64], sb[80];
      fn_name((uintptr_t)__builtin_return_address(0), fn, sizeof fn);
      /* From a script: which one, on whom (the VM's top script). */
      uint8_t *vm = vm_internal();
      const char *sn = vm_script(vm);
      sb[0] = 0;
      if (sn) {
        char t2[48];
        int d = *(int *)(vm + 0x28);
        snprintf(sb, sizeof sb, ", script %.16s self=%s", sn, tag_of(*(uint32_t *)(vm + 0x18c + d * 4), t2, sizeof t2));
      }
      TR("ClearAllActions(%d) on %s from %s%s", a, tag_of(*(uint32_t *)((uint8_t *)obj + 4), tb, sizeof tb), fn, sb);
    }
  }
  return o_clearall(obj, a);
}
/* Timed events (type 1: DelayCommand, AssignCommand) and anything Zherron
 * sends or gets; not the perception, effect and shout traffic of a battle. */
static int event_wanted(uint32_t type, uint32_t caller, uint32_t target) {
  return type == 1 || caller == s_zherron || target == s_zherron;
}
static int (*o_addev)(void *m, uint32_t days, uint32_t time, uint32_t caller, uint32_t target, uint32_t type, void *data);
static void *volatile s_ai_master;   /* CServerAIMaster: its event list is the watchdog's */
static int h_addev(void *m, uint32_t days, uint32_t time, uint32_t caller, uint32_t target, uint32_t type, void *data) {
  if (!s_ai_master) s_ai_master = m;
  if (trace_on() && event_wanted(type, caller, target)) {
    static uint32_t n;
    if (n++ < 600) {
      char t1[48], t2[48], fn[64];
      fn_name((uintptr_t)__builtin_return_address(0), fn, sizeof fn);
      TR("event queued: type %u in %u.%03u s, %s -> %s, from %s", (unsigned)type, (unsigned)(time / 1000),
         (unsigned)(time % 1000), tag_of(caller, t1, sizeof t1), tag_of(target, t2, sizeof t2), fn);
    }
  }
  return o_addev(m, days, time, caller, target, type, data);
}
static int (*o_fading)(void *gui);
static volatile int s_fading_last = -1;
static int h_fading(void *gui) {
  int r = o_fading(gui);
  if (r != s_fading_last) {
    s_fading_last = r;
    TR("IsGlobalFading -> %d (asked from %s)", r, ai_thread_on_worker() ? "the AI worker" : "the game thread");
  }
  return r;
}
static uint8_t *server_internal(void) {
  uint8_t *app = server_app();
  return app ? *(uint8_t **)(app + 4) : NULL;
}
static uint8_t *client_internal(void) {
  void *am = s_app_mgr ? *(void **)s_app_mgr : NULL;
  uint8_t *capp = am ? *(uint8_t **)((uint8_t *)am + 4) : NULL;
  return capp ? *(uint8_t **)(capp + 4) : NULL;
}
/* What CSWSObject::AIActionDialogObject waits on (it puts a wait action in
 * front and tries again): the client's area-not-ready word (internal +0x288),
 * paused by combat (the server's pause bit 2) and the client's auto-pause bit
 * (internal +0x384). The server's pause bits are at internal +0x10078; any
 * pause stops the world timer timed events use (CServerExoAppInternal::
 * SetPauseState), except for objects on the pause's exclusion list. */
static const char *pause_str(char *b, size_t n) {
  uint8_t *si = server_internal(), *ci = client_internal();
  snprintf(b, n, "pause bits %d, auto-paused %d, area not ready %d, fading %d", si ? si[0x10078] : -1,
           ci ? ci[0x384] & 1 : -1, ci ? (int)*(uint32_t *)(ci + 0x288) : -1, s_fading_last);
  return b;
}
/* Pauses: the server's (any reason) and the client's auto-pause, with who
 * asked; logged from the start, as one could come before the scene. */
static uint32_t (*o_setpause)(void *si, uint32_t type, int on);
static uint32_t h_setpause(void *si, uint32_t type, int on) {
  static uint32_t n;
  int before = ((uint8_t *)si)[0x10078];
  uint32_t r = o_setpause(si, type, on);
  int after = ((uint8_t *)si)[0x10078];
  if (before != after && n++ < 150) {   /* changes only: callers may repeat it every frame */
    char fn[64];
    fn_name((uintptr_t)__builtin_return_address(0), fn, sizeof fn);
    TR("server SetPauseState(%u, %d) from %s: pause bits %d -> %d", (unsigned)(type & 0xff), on, fn, before, after);
  }
  return r;
}
static uint32_t (*o_autopause)(void *ci, int on, uint32_t reason);
static uint32_t h_autopause(void *ci, int on, uint32_t reason) {
  static uint32_t n, nd;
  int before = ((uint8_t *)ci)[0x384] & 1;
  uint32_t r = o_autopause(ci, on, reason);
  int after = ((uint8_t *)ci)[0x384] & 1;
  /* Changes, and a few asks that changed nothing (postponed or refused). */
  if ((before != after && n++ < 150) || (before == after && on && nd++ < 30)) {
    char fn[64];
    fn_name((uintptr_t)__builtin_return_address(0), fn, sizeof fn);
    TR("client SetAutoPaused(%d, reason %u) from %s: auto-paused %d -> %d", on, (unsigned)(reason & 0xff), fn, before,
       after);
  }
  return r;
}
void battle_ledger_trace_event(uint32_t type, uint32_t caller, uint32_t target) {
  if (!trace_on() || !event_wanted(type, caller, target)) return;
  static uint32_t n;
  if (n++ >= 600) return;
  char t1[48], t2[48];
  TR("event fired: type %u, %s -> %s", (unsigned)type, tag_of(caller, t1, sizeof t1), tag_of(target, t2, sizeof t2));
}
int battle_ledger_tracing(void) { return trace_on(); }
/* ai_thread.c: a gated client call while the trace runs, with what the AI
 * thread does with it; for the dialog action, its owner. */
void battle_ledger_trace_gate(const char *fn, uintptr_t ret, const char *what, void *arg0, int dialog_action) {
  static uint32_t n, nd;
  if (!trace_on() || (dialog_action ? nd++ >= 300 : n++ >= 500)) return;
  char caller[64];
  fn_name(ret, caller, sizeof caller);
  if (dialog_action && arg0) {
    char tb[48], pb[96];
    int (*dead)(void *) = (*(int (***)(void *))arg0)[0x98 / 4];
    TR("gate %s from %s: %s; owner=%s dead=%d; %s", fn, caller, what,
       tag_of(*(uint32_t *)((uint8_t *)arg0 + 4), tb, sizeof tb), dead ? dead(arg0) : -1, pause_str(pb, sizeof pb));
  } else {
    TR("gate %s from %s: %s", fn, caller, what);
  }
}
/* An object's state, with its whole action queue: CExoLinkedList at +0x100
 * (internal {head, tail, count}; node {prev, next, data}); for each action the
 * first two words of its CSWSObjectActionNode. */
static void snapshot(const char *who, uint32_t oid) {
  uint8_t *o = sws_object(oid);
  if (!o) { TR("state %s: no object", who); return; }
  int (*dead)(void *) = (*(int (***)(void *))o)[0x98 / 4];
  uint8_t *q = *(uint8_t **)(o + 0x100);
  int cnt = q ? *(int *)(q + 8) : -1;
  char acts[160];
  int ao = 0;
  acts[0] = 0;
  uint8_t *node = q ? *(uint8_t **)q : NULL;
  for (int i = 0; node && i < 10 && ao < 130; i++, node = *(uint8_t **)(node + 4)) {
    const uint32_t *a = *(const uint32_t **)(node + 8);
    ao += snprintf(acts + ao, sizeof acts - (size_t)ao, " %x/%x", a ? (unsigned)a[0] : 0u, a ? (unsigned)a[1] : 0u);
  }
  char tb[48], pb[96];
  TR("state %s %s: dead %d, %d actions [%s ], AI level %d; %s", who, tag_of(oid, tb, sizeof tb), dead ? dead(o) : -1, cnt,
     acts, *(int *)(o + 0x78), pause_str(pb, sizeof pb));
}
/* The AI master's pending events (CServerAIMaster+0x54: CExoLinkedList of
 * 24-byte {day, time of day ms, caller, target, type, data}, sorted by time)
 * against the world timer they are due by (*(server internal +0x10048)). The
 * timed ones (type 1) and Zherron's in full, the rest counted by type. Read on
 * the game thread at the client loop's start, while the AI worker sleeps. */
static void (*f_world_time)(void *timer, uint32_t *day, uint32_t *ms);
static void dump_events(const char *why) {
  uint8_t *m = s_ai_master;
  if (!m) { TR("events (%s): AI master not seen yet", why); return; }
  uint8_t *si = *(uint8_t **)m;
  void *wt = si ? *(void **)(si + 0x10048) : NULL;
  uint32_t day = 0, tod = 0;
  if (wt && f_world_time) f_world_time(wt, &day, &tod);
  uint8_t *li = *(uint8_t **)(m + 0x54);
  int cnt = li ? *(int *)(li + 8) : -1;
  char pb[96];
  TR("watchdog (%s): %d AI events pending at world time day %u, %u ms; %s", why, cnt, (unsigned)day, (unsigned)tod,
     pause_str(pb, sizeof pb));
  uint32_t by_type[32] = { 0 }, shown = 0;
  uint8_t *node = li ? *(uint8_t **)li : NULL;
  for (int i = 0; node && i < 3000; i++, node = *(uint8_t **)(node + 4)) {
    const uint32_t *ev = *(const uint32_t **)(node + 8);
    if (!ev) continue;
    if (event_wanted(ev[4], ev[2], ev[3]) && shown < 40) {
      char t1[48], t2[48];
      shown++;
      TR("  pending: type %u due day %u, %u ms (%+d ms from now), %s -> %s", (unsigned)ev[4], (unsigned)ev[0],
         (unsigned)ev[1], ev[0] == day ? (int)(ev[1] - tod) : 0, tag_of(ev[2], t1, sizeof t1), tag_of(ev[3], t2, sizeof t2));
    } else {
      by_type[ev[4] < 32 ? ev[4] : 31]++;
    }
  }
  char b[200];
  int o = 0;
  b[0] = 0;
  for (int t = 0; t < 32 && o < 170; t++)
    if (by_type[t]) o += snprintf(b + o, sizeof b - (size_t)o, " %d:%u", t, (unsigned)by_type[t]);
  TR("  others by type:%s", o ? b : " none");
}
static void trace_tick(void) {
  static uint64_t last;
  if (!trace_on()) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  void *app = server_app();
  /* Watchdog: the retreat's conversation normally starts within a second;
   * dump once just after the retreat (the 0.5 s DelayCommand should be
   * pending), then at 10, 30 and 60 s if no StartDialog came. */
  if (s_retreat_t && !s_dialog_t && s_wd_stage < 4) {
    static const uint32_t at_ms[4] = { 200, 10000, 30000, 60000 };
    uint64_t el = now - s_retreat_t;
    if (el >= at_ms[s_wd_stage] * 1000ull) {
      char why[64];
      if (s_wd_stage == 0) snprintf(why, sizeof why, "%.2f s after the retreat", (double)el / 1e6);
      else snprintf(why, sizeof why, "STALL: no conversation %.0f s after the retreat", (double)el / 1e6);
      s_wd_stage++;
      dump_events(why);
      if (s_zherron != INVALID_OID) snapshot("Zherron", s_zherron);
      if (app && f_pc_id) snapshot("PC", f_pc_id(app));
      last = now;
      return;
    }
  }
  if (now - last < 2000000u) return;
  last = now;
  if (s_zherron != INVALID_OID) snapshot("Zherron", s_zherron);
  if (app && f_pc_id) snapshot("PC", f_pc_id(app));
}
#else
void battle_ledger_trace_event(uint32_t type, uint32_t caller, uint32_t target) { (void)type; (void)caller; (void)target; }
int battle_ledger_tracing(void) { return 0; }
void battle_ledger_trace_gate(const char *fn, uintptr_t ret, const char *what, void *arg0, int dialog_action) {
  (void)fn; (void)ret; (void)what; (void)arg0; (void)dialog_action;
}
#endif

static uintptr_t s_sit_lo, s_sit_hi;  /* CVirtualMachineInternal::RunScriptSituation */
typedef int (*rsf_t)(void *vmi, int a);
static rsf_t o_rsf;
static int h_rsf(void *vmi, int a) {
  uint64_t t0 = now_us();
#if SCENE_TRACE
  int tdepth = *(int *)((uint8_t *)vmi + 0x28);
  const char *tnm = tdepth >= 0 && tdepth < 8 ? *(const char **)((uint8_t *)vmi + 0x2c + tdepth * 40 + 24) : NULL;
  char tname[17] = { 0 };
  if (tnm) { strncpy(tname, tnm, 16); trace_script((uint8_t *)vmi, tname, tdepth, 1, 0); }
#define TRACE_END(r_) do { if (tname[0]) trace_script((uint8_t *)vmi, tname, tdepth, 0, (r_)); } while (0)
#else
#define TRACE_END(r_) do {} while (0)
#endif
  if (!ai_thread_on_worker()) {
    int outer = s_game_depth++ == 0;
    int r = o_rsf(vmi, a);
    TRACE_END(r);
    s_game_depth--;
    if (outer) {
      w_game_us += now_us() - t0;
      w_game_n++;
    }
    return r;
  }
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  int sit = ret >= s_sit_lo && ret < s_sit_hi;
  int depth = *(int *)((uint8_t *)vmi + 0x28);
  const char *nm = NULL;
  if (depth >= 0 && depth < 8) nm = *(const char **)((uint8_t *)vmi + 0x2c + depth * 40 + 24);
#if SCRIPT_DUMP
  if (nm && depth >= 0 && depth < 8) {
    const uint8_t *slot = (const uint8_t *)vmi + 0x2c + depth * 40;
    dump_code(nm, *(const uint8_t *const *)(slot + 16), *(const uint32_t *)(slot + 20));
  }
#endif
  int top = s_depth == 0, cat = C_OTHER, site = -1;
  if (top) {
    if (sit) cat = s_docmd ? C_DOCOMMAND : C_DELAYED;
    else if (s_rs_depth > 0 && s_rs_depth <= NEST && (site = s_rs_site[s_rs_depth - 1]) >= 0) cat = s_site[site].cat;
  }
  int d = s_depth++;
  if (d < NEST) {
    s_fr[d].t0 = t0;
    s_fr[d].idle0 = ai_thread_idle_us();
    s_fr[d].child = 0;
    strncpy(s_fr[d].name, nm && nm[0] ? nm : "?", 16);
    s_fr[d].name[16] = 0;
  }
  int r = o_rsf(vmi, a);
  TRACE_END(r);
  s_depth--;
  if (d < NEST) {
    uint64_t wall = now_us() - s_fr[d].t0, idle = ai_thread_idle_us() - s_fr[d].idle0;
    uint64_t incl = wall > idle ? wall - idle : 0;
    uint64_t self = incl > s_fr[d].child ? incl - s_fr[d].child : 0;
    if (d > 0) s_fr[d - 1].child += incl;
    sstat_t *e = script_slot(s_fr[d].name);
    if (e) {
      e->runs++;
      e->self_us += self;
      if (top) { e->top++; e->top_us += incl; }
    }
    if (top) {
      w_cat_us[cat] += incl;
      w_cat_n[cat]++;
      if (site >= 0) { s_site[site].us += incl; s_site[site].n++; }
    }
  }
  return r;
}

/* ---- Parts of AIUpdate ---------------------------------------------------- */

enum { P_PERC, P_MINE, P_QUEUE, P_ACTIONS, P_ROUND, P_EFFECTS, P_COMBATINFO, P_AISTATE, P_N };
static const char *const k_part[P_N] = { "perception", "mine check", "action queue", "RunActions", "combat round timer",
                                         "effects", "combat info", "AI state" };
static uint64_t w_part_us[P_N];
static uint32_t w_part_n[P_N];

#define PART_BEGIN                                        \
  int w_ = ai_thread_on_worker();                         \
  uint64_t t0_ = 0, i0_ = 0;                              \
  if (w_) { t0_ = now_us(); i0_ = ai_thread_idle_us(); }
#define PART_END(k)                                                         \
  if (w_) {                                                                 \
    uint64_t d_ = now_us() - t0_, i_ = ai_thread_idle_us() - i0_;           \
    w_part_us[k] += d_ > i_ ? d_ - i_ : 0;                                  \
    w_part_n[k]++;                                                          \
  }

static uint32_t (*o_perc)(void *, int);
static uint32_t h_perc(void *s, int a) { PART_BEGIN uint32_t r = o_perc(s, a); PART_END(P_PERC) return r; }
static uint32_t (*o_mine)(void *);
static uint32_t h_mine(void *s) { PART_BEGIN uint32_t r = o_mine(s); PART_END(P_MINE) return r; }
static uint32_t (*o_queue)(void *);
static uint32_t h_queue(void *s) { PART_BEGIN uint32_t r = o_queue(s); PART_END(P_QUEUE) return r; }
static uint32_t (*o_actions)(void *, uint32_t, uint32_t, uint64_t);
static uint32_t h_actions(void *s, uint32_t a, uint32_t b, uint64_t c) {
  PART_BEGIN uint32_t r = o_actions(s, a, b, c); PART_END(P_ACTIONS) return r;
}
static uint32_t (*o_round)(void *);
static uint32_t h_round(void *s) { PART_BEGIN uint32_t r = o_round(s); PART_END(P_ROUND) return r; }
static uint32_t (*o_effects)(void *, uint32_t, uint32_t);
static uint32_t h_effects(void *s, uint32_t a, uint32_t b) {
  PART_BEGIN uint32_t r = o_effects(s, a, b); PART_END(P_EFFECTS) return r;
}
static uint32_t (*o_cinfo)(void *);
static uint32_t h_cinfo(void *s) { PART_BEGIN uint32_t r = o_cinfo(s); PART_END(P_COMBATINFO) return r; }
static uint32_t (*o_aistate)(void *);
static uint32_t h_aistate(void *s) { PART_BEGIN uint32_t r = o_aistate(s); PART_END(P_AISTATE) return r; }

/* ---- The game thread's main loops ----------------------------------------- */

static int (*o_cloop)(void *), (*o_sloop)(void *);
static uint64_t w_cloop_us, w_sloop_us;
static uint32_t w_cloop_n, w_sloop_n;
static uint64_t w_cloop_draw;
static int h_cloop(void *s) {
#if SCENE_TRACE
  trace_tick();
#endif
  uint64_t t = now_us(), r0 = ai_thread_render_us();
  int r = o_cloop(s);
  uint64_t d = now_us() - t, rd = ai_thread_render_us() - r0;
  w_cloop_us += d > rd ? d - rd : 0;
  w_cloop_draw += rd;
  w_cloop_n++;
  return r;
}
static int h_sloop(void *s) {
  if (ai_thread_on_worker()) return o_sloop(s);
  uint64_t t = now_us();
  int r = o_sloop(s);
  w_sloop_us += now_us() - t;
  w_sloop_n++;
  return r;
}

/* Stages of the client's main loop before the drawing (CClientExoAppInternal::
 * MainLoop calls them through relocation slots), game thread only. Float
 * arguments travel in core registers (the engine is softfp): passed on as bits. */
enum { G_CLIENTAI, G_NET, G_GUIUPD, G_INPUT, G_CAMERA, G_PARTY, G_RESMAN, G_SOUND, G_N };
static const char *const k_stage[G_N] = { "client AI", "server messages", "GUI update", "input", "camera", "party",
                                          "resource manager", "sound" };
static uint64_t w_stage_us[G_N], w_stage_draw[G_N];  /* own time; the drawing and AI wait inside */
static uint32_t w_stage_n[G_N];
#define STAGE_BEGIN                                       \
  int g_ = !ai_thread_on_worker();                        \
  uint64_t t0_ = 0, r0_ = 0;                              \
  if (g_) { t0_ = now_us(); r0_ = ai_thread_render_us(); }
#define STAGE_END(k)                                                        \
  if (g_) {                                                                 \
    uint64_t d_ = now_us() - t0_, r_ = ai_thread_render_us() - r0_;         \
    w_stage_us[k] += d_ > r_ ? d_ - r_ : 0;                                 \
    w_stage_draw[k] += r_;                                                  \
    w_stage_n[k]++;                                                         \
  }
static uint32_t (*o_cai)(void *);
static uint32_t h_cai(void *s) { STAGE_BEGIN uint32_t r = o_cai(s); STAGE_END(G_CLIENTAI) return r; }
static uint32_t (*o_net)(void *, int);
static uint32_t h_net(void *s, int a) { STAGE_BEGIN uint32_t r = o_net(s, a); STAGE_END(G_NET) return r; }
static uint32_t (*o_guiupd)(void *, uint32_t);
static uint32_t h_guiupd(void *s, uint32_t f) { STAGE_BEGIN uint32_t r = o_guiupd(s, f); STAGE_END(G_GUIUPD) return r; }
static uint32_t (*o_input)(void *, uint32_t);
static uint32_t h_input(void *s, uint32_t f) { STAGE_BEGIN uint32_t r = o_input(s, f); STAGE_END(G_INPUT) return r; }
static uint32_t (*o_camera)(void *, uint32_t);
static uint32_t h_camera(void *s, uint32_t f) { STAGE_BEGIN uint32_t r = o_camera(s, f); STAGE_END(G_CAMERA) return r; }
static uint32_t (*o_party)(void *, uint32_t);
static uint32_t h_party(void *s, uint32_t f) { STAGE_BEGIN uint32_t r = o_party(s, f); STAGE_END(G_PARTY) return r; }
static uint32_t (*o_resman)(void *, uint32_t);
static uint32_t h_resman(void *s, uint32_t a) { STAGE_BEGIN uint32_t r = o_resman(s, a); STAGE_END(G_RESMAN) return r; }
static uint32_t (*o_sound)(void *);
static uint32_t h_sound(void *s) { STAGE_BEGIN uint32_t r = o_sound(s); STAGE_END(G_SOUND) return r; }

/* Stages of the drawing (Scene::Render calls them through its vtable or the
 * PLT), game thread, inclusive: what the drawing spends besides ManageSceneBSP
 * and DoGobBuckets ([scene] line). */
enum { D_EMITTERS, D_TEXCTL, D_PASS, D_STATIC, D_DYNAMIC, D_MESH, D_EMITBUCKET, D_BIN, D_CAP, D_SABER, D_DISTORT,
       D_GOBSHADOWS, D_FLARES, D_FADE, D_BLUR, D_N };
static const struct { const char *sym, *name; } k_draw[D_N] = {
  { "_ZN5Scene14UpdateEmittersEv", "UpdateEmitters" },
  { "_Z31AurTextureForceControllerUpdatev", "texture controllers" },
  { "_ZN5Scene16RenderSinglePassEv", "RenderSinglePass" },
  { "_ZN5Scene20RenderStaticGeometryEv", "RenderStaticGeometry" },
  { "_ZN5Scene21RenderDynamicGeometryEv", "RenderDynamicGeometry" },
  { "_ZN5Scene13DoMeshBucketsEv", "DoMeshBuckets" },
  { "_ZN5Scene15DoEmitterBucketEv", "DoEmitterBucket" },
  { "_ZN5Scene11DoBinBucketEv", "DoBinBucket" },
  { "_ZN5Scene11DoCapBucketEv", "DoCapBucket" },
  { "_ZN5Scene18DoLightSaberBucketEv", "DoLightSaberBucket" },
  { "_ZN5Scene19DoDistortionBucketsEv", "DoDistortionBuckets" },
  { "_ZN5Scene12DoGobShadowsEv", "DoGobShadows" },
  { "_ZN5Scene16RenderLensFlaresEv", "RenderLensFlares" },
  { "_ZN5Scene13DoFadeBucketsEv", "DoFadeBuckets" },
  { "_ZN5Scene13DoBlurBucketsEv", "DoBlurBuckets" },
};
static uint32_t (*o_draw[D_N])(void *);
static uint64_t w_draw_us[D_N];
static uint32_t w_draw_n[D_N];
#define DRAW_HOOK(k)                                                        \
  static uint32_t h_draw_##k(void *s) {                                     \
    if (ai_thread_on_worker()) return o_draw[k](s);                         \
    uint64_t t0 = now_us();                                                 \
    uint32_t r = o_draw[k](s);                                              \
    w_draw_us[k] += now_us() - t0;                                          \
    w_draw_n[k]++;                                                          \
    return r;                                                               \
  }
DRAW_HOOK(0) DRAW_HOOK(1) DRAW_HOOK(2) DRAW_HOOK(3) DRAW_HOOK(4) DRAW_HOOK(5) DRAW_HOOK(6) DRAW_HOOK(7)
DRAW_HOOK(8) DRAW_HOOK(9) DRAW_HOOK(10) DRAW_HOOK(11) DRAW_HOOK(12) DRAW_HOOK(13) DRAW_HOOK(14)
static uint32_t (*const k_draw_hook[D_N])(void *) = {
  h_draw_0, h_draw_1, h_draw_2, h_draw_3, h_draw_4, h_draw_5, h_draw_6, h_draw_7,
  h_draw_8, h_draw_9, h_draw_10, h_draw_11, h_draw_12, h_draw_13, h_draw_14,
};

#if SCRIPT_DUMP
/* Game thread, between frames, with the VM idle: the command table, then the
 * listed scripts, read the way SaveScriptSituation reads one (ReadScriptFile
 * puts it in the VM's next slot; DeleteScript and the depth back down). */
static void dump_listed(void) {
  static int done;
  if (done) return;
  uintptr_t g = so_symbol(&kotor_mod, "g_pVirtualMachine");
  void *cvm = g ? *(void **)g : NULL;
  uint8_t *vm = cvm ? *(uint8_t **)((uint8_t *)cvm + 28) : NULL;
  if (!vm) return;
  uint8_t *impl = *(uint8_t **)(vm + 0x3d4);
  if (!impl || *(int *)(vm + 0x1ac) != 0 || *(int *)(vm + 0x1c4) != 0) return;
  int (*rd)(void *, void *) = (int (*)(void *, void *))so_symbol(&kotor_mod,
      "_ZN23CVirtualMachineInternal14ReadScriptFileEP10CExoString");
  void (*del)(void *, void *) = (void (*)(void *, void *))so_symbol(&kotor_mod,
      "_ZN23CVirtualMachineInternal12DeleteScriptEP21CVirtualMachineScript");
  if (!rd || !del) return;
  done = 1;
  sceIoMkdir(DUMP_DIR, 0777);
  /* ExecuteCommand: number <= 876, table at implementer+12, 8 bytes each
   * (function, this adjustment; bit 0 of the adjustment marks a virtual). */
  {
    const uint32_t *tab = *(const uint32_t *const *)(impl + 12);
    SceUID fd = sceIoOpen(DUMP_DIR "/actions.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    for (int i = 0; tab && fd >= 0 && i <= 876; i++) {
      char line[160], nm[120];
      uint32_t fn = tab[2 * i], adj = tab[2 * i + 1];
      if (adj & 1) snprintf(nm, sizeof nm, "virtual+%u", (unsigned)fn);
      else if (fn) fn_name(fn, nm, sizeof nm);
      else snprintf(nm, sizeof nm, "-");
      int n = snprintf(line, sizeof line, "%d %s\n", i, nm);
      sceIoWrite(fd, line, n);
    }
    if (fd >= 0) sceIoClose(fd);
  }
  static const char *const k_default[] = {
    "k_saedhe_user", "a_mil_retreat", "tr_battle", "k_fab_spawn01", "k_fab_spawn01_ak", "a_misiadoors", "a_move_tag",
    "a_bg_music", "a_check_mine", "k_merc_death", "k_mil_death", "k_sp1_generic", "k_oei_userdef", "a_attonspirit",
    "k_def_spn_t_none", "k_saedhe_spawn", "k_saedhe_death", "k_saedhe_hb", "k_saedhe_dlg", NULL };
  char list[2048];
  int nl = 0;
  SceUID lf = sceIoOpen(DATA_PATH "/dump_scripts.txt", SCE_O_RDONLY, 0);
  if (lf >= 0) { nl = sceIoRead(lf, list, sizeof list - 1); sceIoClose(lf); }
  if (nl < 0) nl = 0;
  list[nl] = 0;
  const char *names[128];
  int nn = 0;
  for (char *p = list; *p && nn < 128;) {
    while (*p == '\n' || *p == '\r' || *p == ' ') *p++ = 0;
    if (!*p) break;
    names[nn++] = p;
    while (*p && *p != '\n' && *p != '\r' && *p != ' ') p++;
  }
  for (int i = 0; k_default[i] && nn < 128; i++) names[nn++] = k_default[i];
  int ok = 0, missing = 0;
  for (int i = 0; i < nn; i++) {
    struct { const char *s; uint32_t len; } ex = { names[i], (uint32_t)strlen(names[i]) };
    int d0 = *(int *)(vm + 0x28);
    if (rd(vm, &ex) == 0 && *(int *)(vm + 0x28) == d0 + 1) {
      uint8_t *slot = vm + 0x2c + (d0 + 1) * 40;
      dump_code(names[i], *(const uint8_t **)(slot + 16), *(const uint32_t *)(slot + 20));
      del(vm, slot);
      *(int *)(vm + 0x28) = d0;
      ok++;
    } else {
      missing++;
    }
  }
  log_printf("[ledger] dump: command table and %d of %d listed scripts written to " DUMP_DIR " (%d not found)", ok, nn,
             missing);
}
#endif

/* ---- Install and report ----------------------------------------------------- */

void battle_ledger_install(void) {
  uintptr_t lo, hi;
  if (sym_range("_ZN23CVirtualMachineInternal18RunScriptSituationEP21CVirtualMachineScriptmi", &lo, &hi)) {
    s_sit_lo = lo;
    s_sit_hi = hi;
  }
#define TAKE(var, sym, hook) (var = (__typeof__(var))take_slots(sym, (uintptr_t)&hook))
  TAKE(o_runscript, "_ZN15CVirtualMachine9RunScriptEP10CExoStringmi", h_runscript);
  TAKE(o_rss, "_ZN15CVirtualMachine18RunScriptSituationEPvmi", h_rss);
  TAKE(o_rsf, "_ZN23CVirtualMachineInternal13RunScriptFileEi", h_rsf);
  TAKE(o_perc, "_ZN12CSWSCreature26SpawnInHeartbeatPerceptionEi", h_perc);
  TAKE(o_mine, "_ZN12CSWSCreature15UpdateMineCheckEv", h_mine);
  TAKE(o_queue, "_ZN12CSWSCreature17UpdateActionQueueEv", h_queue);
  TAKE(o_actions, "_ZN10CSWSObject10RunActionsEmmy", h_actions);
  TAKE(o_round, "_ZN12CSWSCreature22UpdateCombatRoundTimerEv", h_round);
  TAKE(o_effects, "_ZN10CSWSObject16UpdateEffectListEmm", h_effects);
  TAKE(o_cinfo, "_ZN17CSWSCreatureStats23UpdateCombatInformationEv", h_cinfo);
  TAKE(o_aistate, "_ZN12CSWSCreature14ComputeAIStateEv", h_aistate);
  TAKE(o_cloop, "_ZN13CClientExoApp8MainLoopEv", h_cloop);
  TAKE(o_sloop, "_ZN13CServerExoApp8MainLoopEv", h_sloop);
  TAKE(o_cai, "_ZN15CClientAIMaster11UpdateStateEv", h_cai);
  TAKE(o_net, "_ZN9CNetLayer21ProcessReceivedFramesEi", h_net);
  TAKE(o_guiupd, "_ZN13CSWGuiManager6UpdateEf", h_guiupd);
  TAKE(o_input, "_ZN21CClientExoAppInternal12ProcessInputEf", h_input);
  TAKE(o_camera, "_ZN21CClientExoAppInternal12UpdateCameraEf", h_camera);
  TAKE(o_party, "_ZN8CSWParty6UpdateEf", h_party);
  TAKE(o_resman, "_ZN10CExoResMan6UpdateEm", h_resman);
  TAKE(o_sound, "_ZN21CClientExoAppInternal17UpdateSoundEngineEv", h_sound);
#if SCENE_TRACE
  s_app_mgr = so_symbol(&kotor_mod, "g_pAppManager");
  s_gvm = so_symbol(&kotor_mod, "g_pVirtualMachine");
  f_get_obj = (void *(*)(void *, uint32_t))so_symbol(&kotor_mod, "_ZN13CServerExoApp13GetGameObjectEm");
  f_pc_id = (uint32_t (*)(void *))so_symbol(&kotor_mod, "_ZN13CServerExoApp19GetPlayerCreatureIdEv");
  TAKE(o_exec_cmd, "_ZN25CSWVirtualMachineCommands14ExecuteCommandEii", h_exec_cmd);
  TAKE(o_dlgact, "_ZN10CSWSObject20AIActionDialogObjectEP20CSWSObjectActionNode", h_dlgact);
  TAKE(o_startdlg, "_ZN10CSWSObject11StartDialogEmRK10CExoStringi", h_startdlg);
  TAKE(o_startentry, "_ZN10CSWSDialog13GetStartEntryEP10CSWSObject", h_startentry);
  TAKE(o_checkscript, "_ZN10CSWSDialog11CheckScriptEP10CSWSObjectRK7CResRef", h_checkscript);
  TAKE(o_stopdlg, "_ZN10CSWSObject10StopDialogEv", h_stopdlg);
  TAKE(o_clearall, "_ZN10CSWSObject15ClearAllActionsEi", h_clearall);
  TAKE(o_addev, "_ZN15CServerAIMaster17AddEventDeltaTimeEmmmmmPv", h_addev);
  TAKE(o_fading, "_ZN10CGuiInGame14IsGlobalFadingEv", h_fading);
  TAKE(o_setpause, "_ZN21CServerExoAppInternal13SetPauseStateEhi", h_setpause);
  TAKE(o_autopause, "_ZN21CClientExoAppInternal13SetAutoPausedEih", h_autopause);
  f_world_time = (void (*)(void *, uint32_t *, uint32_t *))so_symbol(&kotor_mod, "_ZN11CWorldTimer12GetWorldTimeEPmS0_");
  log_printf("[trace] armed: commands %s, dialog action %s, StartDialog %s, start entry %s, conditions %s, StopDialog %s, "
             "ClearAllActions %s, events %s, fading %s, server pause %s, auto-pause %s, world time %s; objects %s",
             o_exec_cmd ? "yes" : "NO", o_dlgact ? "yes" : "NO", o_startdlg ? "yes" : "NO", o_startentry ? "yes" : "NO",
             o_checkscript ? "yes" : "NO", o_stopdlg ? "yes" : "NO", o_clearall ? "yes" : "NO", o_addev ? "yes" : "NO",
             o_fading ? "yes" : "NO", o_setpause ? "yes" : "NO", o_autopause ? "yes" : "NO", f_world_time ? "yes" : "NO",
             s_app_mgr && f_get_obj && s_gvm ? "yes" : "NO");
#endif
#undef TAKE
  {
    char miss[300];
    int o = 0;
    miss[0] = 0;
    for (int k = 0; k < D_N; k++) {
      o_draw[k] = (uint32_t (*)(void *))take_slots(k_draw[k].sym, (uintptr_t)k_draw_hook[k]);
      if (!o_draw[k] && o < (int)sizeof miss - 32) o += snprintf(miss + o, sizeof miss - (size_t)o, " %s", k_draw[k].name);
    }
    log_printf("[ledger] drawing stages timed; not hooked:%s", o ? miss : " none");
  }
  log_printf("[ledger] client stages: client AI %s, server messages %s, GUI update %s, input %s, camera %s, party %s, "
             "resource manager %s, sound %s",
             o_cai ? "yes" : "NO", o_net ? "yes" : "NO", o_guiupd ? "yes" : "NO", o_input ? "yes" : "NO",
             o_camera ? "yes" : "NO", o_party ? "yes" : "NO", o_resman ? "yes" : "NO", o_sound ? "yes" : "NO");
  log_printf("[ledger] hooks: RunScript %s, RunScriptSituation %s, RunScriptFile %s (situations %s); AIUpdate parts "
             "%s%s%s%s%s%s%s%s; main loops client %s, server %s",
             o_runscript ? "yes" : "NO", o_rss ? "yes" : "NO", o_rsf ? "yes" : "NO", s_sit_hi ? "found" : "NOT FOUND",
             o_perc ? "P" : "-", o_mine ? "M" : "-", o_queue ? "Q" : "-", o_actions ? "A" : "-", o_round ? "R" : "-",
             o_effects ? "E" : "-", o_cinfo ? "C" : "-", o_aistate ? "S" : "-", o_cloop ? "yes" : "NO",
             o_sloop ? "yes" : "NO");
}

void battle_ledger_report(unsigned frames) {
#if SCRIPT_DUMP
  dump_listed();
#endif
  double f = frames ? frames : 1, fms = f * 1000.0;
  char b[600];
  int o = 0;
  uint64_t tot = 0;
  b[0] = 0;
  for (int c = 0; c < C_N; c++) {
    tot += w_cat_us[c];
    if (!w_cat_n[c]) continue;
    o += snprintf(b + o, sizeof b - (size_t)o, "%s %s %.2f (%.1f runs)", o ? "," : "", k_cat[c], w_cat_us[c] / fms,
                  w_cat_n[c] / f);
  }
  log_printf("[ledger] scripts on the AI worker, ms a frame without waits at gates: %.2f in all; started by:%s | on "
             "other threads %.2f ms (%.1f runs)",
             tot / fms, o ? b : " none", w_game_us / fms, w_game_n / f);
  memset(w_cat_us, 0, sizeof w_cat_us);
  memset(w_cat_n, 0, sizeof w_cat_n);
  w_game_us = w_game_n = 0;

  o = 0;
  b[0] = 0;
  for (int k = 0; k < 14 && o < (int)sizeof b - 64; k++) {
    int best = -1;
    for (int i = 0; i < NS; i++)
      if (s_ss[i].runs && (best < 0 || s_ss[i].self_us > s_ss[best].self_us)) best = i;
    if (best < 0) break;
    sstat_t *e = &s_ss[best];
    o += snprintf(b + o, sizeof b - (size_t)o, " %s %.2f (%.1f runs, %.1f outermost: %.2f)", e->name, e->self_us / fms,
                  e->runs / f, e->top / f, e->top_us / fms);
    e->runs = 0;
  }
  log_printf("[ledger] scripts by own time, ms a frame:%s%s", o ? b : " none", s_ss_full ? " (table full)" : "");
  for (int i = 0; i < NS; i++) s_ss[i].runs = s_ss[i].top = 0, s_ss[i].self_us = s_ss[i].top_us = 0;

  o = 0;
  b[0] = 0;
  int ns = s_nsite;
  for (int i = 0; i < ns; i++)              /* one entry per function: fold its other call sites in */
    for (int j = i + 1; j < ns; j++)
      if (s_site[j].n && !strcmp(s_site[i].fn, s_site[j].fn)) {
        s_site[i].us += s_site[j].us;
        s_site[i].n += s_site[j].n;
        s_site[j].us = 0;
        s_site[j].n = 0;
      }
  for (int k = 0; k < 8 && o < (int)sizeof b - 80; k++) {
    int best = -1;
    for (int i = 0; i < ns; i++)
      if (s_site[i].n && (best < 0 || s_site[i].us > s_site[best].us)) best = i;
    if (best < 0) break;
    o += snprintf(b + o, sizeof b - (size_t)o, " %s %.2f (%.1f)", s_site[best].fn, s_site[best].us / fms,
                  s_site[best].n / f);
    s_site[best].n = 0;
  }
  log_printf("[ledger] outermost script runs by caller, ms a frame (runs):%s", o ? b : " none");
  for (int i = 0; i < ns; i++) s_site[i].us = 0, s_site[i].n = 0;

  o = 0;
  b[0] = 0;
  for (int k = 0; k < P_N; k++)
    o += snprintf(b + o, sizeof b - (size_t)o, "%s %s %.2f (%.1f)", k ? "," : "", k_part[k], w_part_us[k] / fms,
                  w_part_n[k] / f);
  log_printf("[ledger] AIUpdate parts on the AI worker, ms a frame without waits at gates (calls):%s", b);
  memset(w_part_us, 0, sizeof w_part_us);
  memset(w_part_n, 0, sizeof w_part_n);

  unsigned long long rs, rc;
  unsigned rn, rcn, calls;
  glw_rec_stats(&rs, &rn, &rc, &rcn, &calls);
  double pair = rcn ? (double)rc / rcn : 0.0, per = rn ? (double)rs / rn - pair : 0.0;
  log_printf("[ledger] game thread, ms a frame: client loop %.1f without the drawing and AI wait (%.1f of those inside; "
             "%.2f calls), server loop %.1f (%.2f calls) | GL "
             "commands recorded %.0f a frame, %.3f us each (%u sampled; timer pair %.3f us) = %.1f ms a frame",
             w_cloop_us / fms, w_cloop_draw / fms, w_cloop_n / f, w_sloop_us / fms, w_sloop_n / f, calls / f, per,
             (unsigned)rn, pair,
             per * calls / fms);
  w_cloop_us = w_sloop_us = w_cloop_draw = 0;
  w_cloop_n = w_sloop_n = 0;

  o = 0;
  b[0] = 0;
  for (int k = 0; k < G_N; k++)
    o += snprintf(b + o, sizeof b - (size_t)o, "%s %s %.2f%s (%.1f)", k ? "," : "", k_stage[k], w_stage_us[k] / fms,
                  w_stage_draw[k] ? " + the drawing" : "", w_stage_n[k] / f);
  log_printf("[ledger] client stages on the game thread, ms a frame without the drawing and AI wait (calls):%s", b);
  o = 0;
  b[0] = 0;
  for (int k = 0; k < D_N; k++)
    if (o_draw[k] && w_draw_n[k] && o < (int)sizeof b - 48)
      o += snprintf(b + o, sizeof b - (size_t)o, "%s %s %.2f (%.1f)", o ? "," : "", k_draw[k].name, w_draw_us[k] / fms,
                    w_draw_n[k] / f);
  log_printf("[ledger] drawing stages, ms a frame inclusive (calls):%s", o ? b : " none");
  memset(w_draw_us, 0, sizeof w_draw_us);
  memset(w_draw_n, 0, sizeof w_draw_n);
  memset(w_stage_us, 0, sizeof w_stage_us);
  memset(w_stage_draw, 0, sizeof w_stage_draw);
  memset(w_stage_n, 0, sizeof w_stage_n);
}

#else
void battle_ledger_install(void) {}
void battle_ledger_report(unsigned frames) { (void)frames; }
void battle_ledger_trace_event(uint32_t type, uint32_t caller, uint32_t target) { (void)type; (void)caller; (void)target; }
int battle_ledger_tracing(void) { return 0; }
void battle_ledger_trace_gate(const char *fn, uintptr_t ret, const char *what, void *arg0, int dialog_action) {
  (void)fn; (void)ret; (void)what; (void)arg0; (void)dialog_action;
}
#endif
