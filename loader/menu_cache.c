/* menu_cache.c -- the action menus rebuilt when something changed, not
 * every frame (MENU_CACHE).
 *
 * CSWGuiMainInterface::PopulateMenus (libkotor2 0x34cbc8; its int argument is
 * unused) clears the seven quick-action lists and refills each through
 * CSWCCreature::GetPersonalActions -- every usable item, power, stance --
 * then the target menu (CSWGuiTargetActionMenu::PopulateMenus), the six action
 * buttons and the description. Two callers run it every frame:
 *   - CSWGuiMainInterface::Draw (call at 0x34b9a2);
 *   - CClientExoAppInternal::MainLoop, through CGuiInGame::RePopulateMainInterface
 *     (call at 0x372030), when the main interface panel is not up.
 * In the Khoonda battle it was 13% of the game thread (hardware profile hw4,
 * 2026-10-17), nine tenths of it in GetUsableItems over a long inventory.
 *
 * Those two per-frame calls (known by their return address) are skipped when
 * the leader, the target and the six selected actions are what they were at
 * the last run and that run is less than MENU_CACHE_MS old. Every other call
 * -- the game's own repopulates on combat mode, a click in the world, a
 * leader change -- runs as before and counts as a run. What a skip leaves on
 * screen is the last run's state; item counts and usability catch up within
 * MENU_CACHE_MS.
 *
 * ux0:data/kotor2/menu_mode.txt: 0 off (nothing hooked), 1 on, a on and off
 * alternately every 10 s; digits after the letter set MENU_CACHE_MS. Without
 * the file: MENU_CACHE_DEFAULT. A [menu] line every 10 s. */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "menu_cache.h"

#if MENU_CACHE

#define SYM_POPULATE "_ZN19CSWGuiMainInterface13PopulateMenusEi"
#define SITE_DRAW    0x34b9a2u     /* CSWGuiMainInterface::Draw: blx PopulateMenus */
#define SITE_LOOP    0x372030u     /* MainLoop: blx RePopulateMainInterface (tail-calls PopulateMenus) */

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

typedef void (*populate_t)(void *self, int arg);
typedef void *(*get_t)(void *self);
static populate_t o_populate;
static get_t f_get_party, f_get_pc;
static void **s_app;                     /* &g_pAppManager */
static uintptr_t s_ret_draw, s_ret_loop; /* return addresses of the per-frame calls, Thumb bit clear */
static int s_installed, s_ab, s_on;
static uint32_t s_ms = MENU_CACHE_MS;

typedef struct {
  void *self, *leader;
  uint32_t target;                       /* this+0x88: the target's object id */
  uint32_t sel[6];                       /* this+0x1da4: the action each button shows */
} menu_sig_t;
static menu_sig_t s_last;
static int s_have;
static uint64_t s_last_run;

static uint32_t w_frame_calls, w_event_calls, w_runs, w_skips;
static uint64_t w_us;

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static void read_sig(void *self, menu_sig_t *g) {
  memset(g, 0, sizeof *g);
  g->self = self;
  void *app = s_app ? *s_app : NULL;
  void *client = app ? *(void **)((uint8_t *)app + 4) : NULL;
  void *party = client ? f_get_party(client) : NULL;
  g->leader = party ? f_get_pc(party) : NULL;
  g->target = *(uint32_t *)((uint8_t *)self + 0x88);
  memcpy(g->sel, (uint8_t *)self + 0x1da4, sizeof g->sel);
}

static void h_populate(void *self, int arg) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  int per_frame = ret == s_ret_draw || ret == s_ret_loop;
  if (per_frame) w_frame_calls++; else w_event_calls++;
  menu_sig_t g;
  read_sig(self, &g);
  uint64_t t0 = now_us();
  if (s_on && per_frame && s_have && t0 - s_last_run < (uint64_t)s_ms * 1000u && !memcmp(&g, &s_last, sizeof g)) {
    w_skips++;
    return;
  }
  o_populate(self, arg);
  uint64_t t1 = now_us();
  w_us += t1 - t0;
  w_runs++;
  s_last = g;
  s_have = 1;
  s_last_run = t1;
}

void menu_cache_on_swap(void) {
  if (!s_installed) return;
  static uint64_t t0;
  static uint32_t frames;
  uint64_t now = now_us();
  frames++;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  log_printf("[menu] %s%u frames | per-frame calls %u, game's own %u | ran %u (%.2f ms a frame, %u us each), skipped %u",
             s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)frames, (unsigned)w_frame_calls, (unsigned)w_event_calls,
             (unsigned)w_runs, frames ? w_us / 1000.0 / frames : 0.0, w_runs ? (unsigned)(w_us / w_runs) : 0,
             (unsigned)w_skips);
  w_frame_calls = w_event_calls = w_runs = w_skips = 0;
  w_us = 0;
  frames = 0;
  t0 = now;
  if (s_ab) s_on = !s_on;
}

/* The two per-frame call sites, as this libkotor2 has them. */
static int site_is(uint32_t at, uint16_t h1, uint16_t h2) {
  const uint16_t *p = (const uint16_t *)(kotor_mod.text_base + at);
  return p[0] == h1 && p[1] == h2;
}

void menu_cache_install(void) {
  char b[12] = {0};
  b[0] = MENU_CACHE_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/menu_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) { sceIoRead(fd, b, sizeof b - 1); sceIoClose(fd); }
  char m = b[0];
  unsigned ms = 0;
  for (int i = 1; b[i]; i++) if (b[i] >= '0' && b[i] <= '9') ms = ms * 10 + (unsigned)(b[i] - '0');
  if (ms) s_ms = ms;
  if (m != '1' && m != 'a') { log_printf("[menu] off (menu_mode '%c')", m); return; }
  /* blx PopulateMenus@plt (0x25b224) and blx RePopulateMainInterface@plt (0x2656f4). */
  if (!site_is(SITE_DRAW, 0xf70f, 0xec40) || !site_is(SITE_LOOP, 0xf6f3, 0xeb60)) {
    log_printf("[menu] NOT armed: the per-frame calls are not where this build expects them");
    return;
  }
  s_app = (void **)so_symbol(&kotor_mod, "g_pAppManager");
  f_get_party = (get_t)so_symbol(&kotor_mod, "_ZN13CClientExoApp10GetSWPartyEv");
  f_get_pc = (get_t)so_symbol(&kotor_mod, "_ZN8CSWParty18GetPlayerCharacterEv");
  if (!s_app || !f_get_party || !f_get_pc) {
    log_printf("[menu] NOT armed: g_pAppManager %d, GetSWParty %d, GetPlayerCharacter %d", !!s_app, !!f_get_party, !!f_get_pc);
    return;
  }
  s_ret_draw = kotor_mod.text_base + SITE_DRAW + 4;
  s_ret_loop = kotor_mod.text_base + SITE_LOOP + 4;
  unsigned n = 0;
  o_populate = (populate_t)take_slots(SYM_POPULATE, (uintptr_t)h_populate, &n);
  if (!o_populate) { log_printf("[menu] NOT armed: PopulateMenus slots not found or already taken"); return; }
  s_ab = m == 'a';
  s_on = 1;
  s_installed = 1;
  log_printf("[menu] armed: PopulateMenus %u slots; the per-frame calls run when the leader, target or a selected "
             "action changed, or every %u ms%s", n, (unsigned)s_ms, s_ab ? "; A/B: on and off alternately every 10 s" : "");
}

#else
void menu_cache_install(void) {}
void menu_cache_on_swap(void) {}
#endif
