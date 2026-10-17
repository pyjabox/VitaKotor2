/* cpu_offload.c -- two cuts to the game thread's frame (CPU_OFFLOAD).
 *
 * Hardware profile, Telos, 2026-10-17 (ASLgl on the GL worker, 54 ms frames,
 * the game thread 91-95% busy): 8.7 ms a frame measuring text, about 6 ms a
 * frame rebuilding animated textures. Both are taken off it here.
 *
 * Text measurement. A GUI label wraps its text every frame:
 * IosLabel::Draw -> IosControl::DrawTextWithBackground -> DrawTextWrapped,
 * which measures each candidate line, word by word, with the font's
 * FTFont::Advance (FTGL; through the vtable at +0x4c). The text does not
 * change from frame to frame, so the answers are kept:
 *  - each size is its own FTTextureFont, made once by SetupTruetypeFont and
 *    never freed, so the font object names the face and the size;
 *  - FTFontImpl::AdvanceI reads len characters and the one after them (for
 *    kerning, and the spacing it adds when that one is not NUL), decoding
 *    UTF-8; the key is a 64-bit hash of the font, len, the 24-byte spacing
 *    and the bytes of those len + 1 characters. A string that ends before
 *    len characters is not cached (FTGL would read past its NUL);
 *  - FTFont::Advance is softfp: the FTPoint (three doubles) goes on the
 *    stack, the float comes back in r0 -- integer carriers here.
 *
 * Animated textures. Each frame Scene::Render calls
 * AurTextureForceControllerUpdate, which binds every texture with a
 * controller that was used in the frame before; CAurTexture::Bind runs the
 * controller's Control(): water (a height field, simulated, rescaled,
 * blurred), ring distortion, random noise. Control() works on the
 * controller's own buffers and ends by handing a buffer to the texture's
 * glImage (vtable +0xb4), the only GL in it; the rest calls the texture's
 * size getters, Distort, Height2Channel, new[]/delete[] and rand. So:
 *  - on the game thread, a controller's Control() uploads what the worker
 *    made for it last time (glImage, in the GL stream as before) and queues
 *    it again;
 *  - a worker on core 1 runs the engine's Control(); its glImage call is
 *    caught there and copies width x height x bytes per pixel aside;
 *  - a controller still being worked on when the game binds it again is
 *    skipped that frame (its texture keeps the last image);
 *  - destroying a controller waits for the worker to be done with it.
 * The textures are one frame behind, which cannot be seen.
 *
 * ux0:data/kotor2/offload_mode.txt: 0 off (nothing hooked), 1 on, a on and
 * off alternately every 10 s (both at once). Without the file:
 * CPU_OFFLOAD_DEFAULT. Every 10 s an [offload] line: text calls, hits and the
 * time spent in DrawTextWrapped; controllers run, skipped, and the time each
 * thread spent on them. CPU_OFFLOAD_VERIFY: every cached width is checked
 * against FTGL. */

#include <vitasdk.h>
#include <kubridge.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "threads.h"
#include "cpu_offload.h"

#if CPU_OFFLOAD

static inline uint32_t tls_key(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return v;
}
static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static int s_installed, s_ab, s_on;
static volatile uint32_t s_game_key;           /* set at the first swap */
static inline int on_game_thread(void) { return s_game_key && tls_key() == s_game_key; }

/* Point every relocation slot of sym that holds the engine's own function at
 * repl; returns the original, 0 if nothing changed (as in gl_state_filter.c). */
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

/* Window counters (game thread, except the worker's). */
static uint32_t w_frames, w_tc_calls, w_tc_hits, w_tc_uncached, w_tc_verify_bad;
static uint64_t w_frame_us, w_text_us;
static uint32_t w_ctl_runs, w_ctl_skips, w_ctl_missed, w_ctl_uploads, w_ctl_fallbacks;
static uint32_t s_frame;                       /* swaps so far */
static uint64_t w_ctl_game_us;
static volatile uint32_t w_ctl_worker_us, w_ctl_worker_runs;

/* ---- text measurement ------------------------------------------------------------ */
typedef struct { uint64_t v[3]; } ftpoint_t;              /* three doubles, on the stack */
typedef uint32_t (*advance_t)(void *, const char *, int, ftpoint_t);
static advance_t o_advance;

#define TC_N 4096u
typedef struct { uint64_t h; uintptr_t font; int32_t len; uint32_t val; } tc_ent_t;
static tc_ent_t s_tc[TC_N];

/* Bytes FTGL's UTF-8 iterator takes for a lead byte. */
static inline int u8len(uint8_t c) {
  return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF8 ? 4 : c < 0xFC ? 5 : 6;
}
#define FNV_P 1099511628211ull
static inline uint64_t mix32(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xff; h *= FNV_P; }
  return h;
}
/* 0: not cacheable. */
static int tc_key(void *font, const uint8_t *s, int len, const ftpoint_t *sp, uint64_t *out) {
  uint64_t h = 1469598103934665603ull;
  h = mix32(h, (uint32_t)(uintptr_t)font);
  h = mix32(h, (uint32_t)len);
  for (int i = 0; i < 3; i++) { h = mix32(h, (uint32_t)sp->v[i]); h = mix32(h, (uint32_t)(sp->v[i] >> 32)); }
  int lim = len < 0 ? 0x7fffffff : len + 1, cps = 0;
  uint32_t i = 0;
  while (cps < lim) {
    uint8_t c = s[i];
    if (!c) {
      if (len >= 0 && cps < len) return 0;
      h ^= 0; h *= FNV_P;
      break;
    }
    int n = u8len(c);
    for (int k = 0; k < n; k++) {
      uint8_t b = s[i + k];
      if (!b && k) return 0;                 /* a NUL inside a sequence: leave it to FTGL */
      h ^= b; h *= FNV_P;
    }
    i += (uint32_t)n;
    cps++;
    if (i > 4096) return 0;
  }
  *out = h ^ (h >> 29);
  return 1;
}

static uint32_t h_advance(void *font, const char *s, int len, ftpoint_t sp) {
  if (!s_on || !s || !on_game_thread()) return o_advance(font, s, len, sp);
  w_tc_calls++;
  uint64_t h;
  if (!tc_key(font, (const uint8_t *)s, len, &sp, &h)) { w_tc_uncached++; return o_advance(font, s, len, sp); }
  tc_ent_t *e = &s_tc[h & (TC_N - 1)];
  if (e->h == h && e->font == (uintptr_t)font && e->len == len) {
    w_tc_hits++;
#if CPU_OFFLOAD_VERIFY
    uint32_t real = o_advance(font, s, len, sp);
    if (real != e->val && w_tc_verify_bad++ < 8)
      log_printf("[offload] VERIFY advance: cached %08x, FTGL %08x (len %d)", (unsigned)e->val, (unsigned)real, len);
#endif
    return e->val;
  }
  uint32_t v = o_advance(font, s, len, sp);
  e->h = h; e->font = (uintptr_t)font; e->len = len; e->val = v;
  return v;
}

/* The whole text path's time on the game thread, at its outermost call. */
typedef void (*dtw_t)(const char *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                      uint32_t, uint32_t);
static dtw_t o_dtw;
static int s_dtw_depth;
static void h_dtw(const char *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                  uint32_t h, uint32_t i, uint32_t j) {
  if (s_dtw_depth++ || !on_game_thread()) { o_dtw(t, a, b, c, d, e, f, g, h, i, j); s_dtw_depth--; return; }
  uint64_t t0 = now_us();
  o_dtw(t, a, b, c, d, e, f, g, h, i, j);
  w_text_us += now_us() - t0;
  s_dtw_depth--;
}

/* ---- animated textures -------------------------------------------------------------- */
enum { C_WATER, C_RING, C_RANDOM, NCLS };
static const char *const k_ctl_sym[NCLS] = {
  "_ZN22WaterTextureController7ControlEv", "_ZN28RingDistortTextureController7ControlEv",
  "_ZN23RandomTextureController7ControlEv",
};
static const char *const k_dtor_sym[NCLS][2] = {
  {"_ZN22WaterTextureControllerD2Ev", "_ZN22WaterTextureControllerD0Ev"},
  {"_ZN28RingDistortTextureControllerD2Ev", "_ZN28RingDistortTextureControllerD0Ev"},
  {"_ZN23RandomTextureControllerD2Ev", "_ZN23RandomTextureControllerD0Ev"},
};
typedef void (*control_t)(void *);
typedef void *(*dtor_t)(void *);
typedef uint32_t (*glimage_t)(void *, uint32_t, uint8_t *);
static control_t o_control[NCLS];
static dtor_t o_dtor[NCLS][2];
static glimage_t o_glimage;

enum { S_IDLE, S_BUSY, S_READY };
#define NCTL 64
#define CAP_MAX (4u << 20)
typedef struct {
  void *ctl;                       /* NULL: free */
  int cls;
  volatile uint32_t state;
  uint8_t fallback;                /* runs on the game thread for good */
  /* the worker's glImage, kept for the game thread */
  void *tex;
  uint32_t b, bytes, cap, uploads;
  uint8_t *buf;
  uint32_t kick_frame;             /* s_frame + 1 of its last run (0: none) */
} cs_t;
static cs_t s_cs[NCTL];
static volatile uint32_t s_queue[2];   /* bit i: slot i waiting for the worker */
static SceUID s_ev = -1;
#define EV_WORK 1u
#define EV_DONE 2u
static volatile uint32_t s_worker_key;
static volatile int s_wslot = -1;      /* the slot the worker is running (its glImage goes there) */

static int slot_of(void *ctl, int cls, int create) {
  int free_i = -1;
  for (int i = 0; i < NCTL; i++) {
    if (s_cs[i].ctl == ctl) return i;
    if (!s_cs[i].ctl && free_i < 0) free_i = i;
  }
  if (!create || free_i < 0) return -1;
  cs_t *c = &s_cs[free_i];
  c->cls = cls; c->state = S_IDLE; c->fallback = 0; c->tex = NULL; c->bytes = 0; c->uploads = 0; c->kick_frame = 0;
  __sync_synchronize();
  c->ctl = ctl;
  return free_i;
}

static inline uint32_t vcall0(void *obj, unsigned off) {
  return ((uint32_t (*)(void *))(*(void ***)obj)[off / 4])(obj);
}

/* On the worker, a controller's upload is kept; anywhere else it goes to GL. */
static uint32_t h_glimage(void *tex, uint32_t b, uint8_t *data) {
  int i = s_wslot;
  if (i < 0 || tls_key() != s_worker_key) return o_glimage(tex, b, data);
  cs_t *c = &s_cs[i];
  uint32_t w = vcall0(tex, 0x4c), h = vcall0(tex, 0x50), bpp = vcall0(tex, 0x58);   /* GetWidth/Height/BPP */
  uint32_t n = w * h * bpp;
  if (!data || !n || n > CAP_MAX || c->uploads) {
    c->fallback = 1;                   /* not the shape this was built for: back to the game thread */
    return 0;
  }
  if (c->cap < n) {
    uint8_t *nb = realloc(c->buf, n);
    if (!nb) { c->fallback = 1; return 0; }
    c->buf = nb;
    c->cap = n;
  }
  memcpy(c->buf, data, n);
  c->tex = tex; c->b = b; c->bytes = n; c->uploads = 1;
  return 1;
}

static void *worker_main(void *arg) {
  (void)arg;
  sceKernelChangeThreadCpuAffinityMask(0, CPU_AUX_A);
  sceKernelChangeThreadPriority(0, CPU_OFFLOAD_PRIORITY);
  thread_census_add(sceKernelGetThreadId(), "texctl-worker");
  s_worker_key = tls_key();
  for (;;) {
    sceKernelWaitEventFlag(s_ev, EV_WORK, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
    for (;;) {
      int i = -1;
      for (int k = 0; k < 2 && i < 0; k++) {
        uint32_t q = s_queue[k];
        if (q) {
          int bit = __builtin_ctz(q);
          __sync_fetch_and_and(&s_queue[k], ~(1u << bit));
          i = k * 32 + bit;
        }
      }
      if (i < 0) break;
      cs_t *c = &s_cs[i];
      c->uploads = 0;
      s_wslot = i;
      uint64_t t0 = now_us();
      o_control[c->cls](c->ctl);
      w_ctl_worker_us += (uint32_t)(now_us() - t0);
      w_ctl_worker_runs++;
      s_wslot = -1;
      __sync_synchronize();
      c->state = c->uploads && !c->fallback ? S_READY : S_IDLE;
      sceKernelSetEventFlag(s_ev, EV_DONE);
    }
  }
  return NULL;
}

static void wait_not_busy(cs_t *c) {
  while (c->state == S_BUSY) {
    SceUInt32 timeout = 2000;
    sceKernelWaitEventFlag(s_ev, EV_DONE, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, &timeout);
  }
}

static void upload(cs_t *c) {
  if (c->tex && c->bytes) { o_glimage(c->tex, c->b, c->buf); w_ctl_uploads++; }
  c->bytes = 0;
}

static void control_hook(void *ctl, int cls) {
  if (!s_on || !on_game_thread() || s_worker_key == 0) {
    uint64_t t0 = now_us();
    o_control[cls](ctl);
    if (on_game_thread()) { w_ctl_game_us += now_us() - t0; w_ctl_runs++; }
    return;
  }
  uint64_t t0 = now_us();
  int i = slot_of(ctl, cls, 1);
  if (i < 0 || s_cs[i].fallback) {
    if (i >= 0 && s_cs[i].state == S_BUSY) wait_not_busy(&s_cs[i]);
    if (i >= 0 && s_cs[i].state == S_READY) { upload(&s_cs[i]); s_cs[i].state = S_IDLE; }
    if (i >= 0) w_ctl_fallbacks++;
    o_control[cls](ctl);
    w_ctl_game_us += now_us() - t0;
    w_ctl_runs++;
    return;
  }
  cs_t *c = &s_cs[i];
  __sync_synchronize();
  /* Once a frame: the engine's Control() returns at once when called again
   * in the frame it last ran (each controller keeps that frame at +8). */
  if (c->kick_frame == s_frame + 1) { w_ctl_skips++; return; }
  if (c->state == S_BUSY) { w_ctl_missed++; return; }   /* a whole frame on the worker: no new image */
  if (c->state == S_READY) upload(c);
  c->kick_frame = s_frame + 1;
  c->state = S_BUSY;
  __sync_synchronize();
  __sync_fetch_and_or(&s_queue[i / 32], 1u << (i % 32));
  sceKernelSetEventFlag(s_ev, EV_WORK);
  w_ctl_game_us += now_us() - t0;
  w_ctl_runs++;
}
static void h_control_water(void *c) { control_hook(c, C_WATER); }
static void h_control_ring(void *c) { control_hook(c, C_RING); }
static void h_control_random(void *c) { control_hook(c, C_RANDOM); }
static void *const k_control_hooks[NCLS] = {(void *)h_control_water, (void *)h_control_ring, (void *)h_control_random};

/* A controller going away: the worker must be done with it, and what it made
 * is dropped. */
static void *dtor_hook(void *ctl, int cls, int deleting) {
  for (int i = 0; i < NCTL; i++) {
    cs_t *c = &s_cs[i];
    if (c->ctl != ctl) continue;
    wait_not_busy(c);
    c->state = S_IDLE;
    c->bytes = 0;
    c->tex = NULL;
    __sync_synchronize();
    c->ctl = NULL;
  }
  return o_dtor[cls][deleting](ctl);
}
#define DTOR_HOOKS(n, cls)                                                    \
  static void *h_dtor2_##n(void *c) { return dtor_hook(c, cls, 0); }          \
  static void *h_dtor0_##n(void *c) { return dtor_hook(c, cls, 1); }
DTOR_HOOKS(water, C_WATER)
DTOR_HOOKS(ring, C_RING)
DTOR_HOOKS(random, C_RANDOM)
static void *const k_dtor_hooks[NCLS][2] = {
  {(void *)h_dtor2_water, (void *)h_dtor0_water},
  {(void *)h_dtor2_ring, (void *)h_dtor0_ring},
  {(void *)h_dtor2_random, (void *)h_dtor0_random},
};

/* Off: the worker finishes, what it made goes up, the game thread runs the
 * controllers again. */
static void go_off(void) {
  for (int i = 0; i < NCTL; i++) {
    cs_t *c = &s_cs[i];
    if (!c->ctl) continue;
    wait_not_busy(c);
    if (c->state == S_READY) upload(c);
    c->state = S_IDLE;
  }
  s_on = 0;
}

int cpu_offload_phase(void) { return s_installed ? s_on : -1; }

/* ---- on and off, report --------------------------------------------------------------- */
void cpu_offload_on_swap(void) {
  if (!s_installed) return;
  if (!s_game_key) s_game_key = tls_key();
  s_frame++;
  static uint64_t t0, prev;
  uint64_t now = now_us();
  if (prev) {
    uint64_t d = now - prev;
    w_frame_us += d;
    w_frames++;
    if (d > 250000) memset(s_tc, 0, sizeof s_tc);   /* a load: start the text cache again */
  }
  prev = now;
  if (!t0) { t0 = now; return; }
  if (now - t0 < 10000000u) return;
  double f = w_frames ? (double)w_frames : 1.0;
  uint32_t wus = w_ctl_worker_us, wruns = w_ctl_worker_runs;
  w_ctl_worker_us = 0; w_ctl_worker_runs = 0;
  log_printf("[offload] %s%u frames, %.1f ms | text: %.2f ms a frame in DrawTextWrapped, %.0f widths asked, %.0f "
             "cached, %.0f not cacheable%s | textures: %.2f ms a frame on the game thread, %.2f on the worker; "
             "%.1f controller runs, %.2f uploads, %.2f repeats in a frame, %.2f missed (still on the worker), %.2f on the game thread",
             s_ab ? (s_on ? "ON " : "OFF ") : "", (unsigned)w_frames, w_frames ? (double)w_frame_us / w_frames / 1000.0 : 0.0,
             (double)w_text_us / f / 1000.0, w_tc_calls / f, w_tc_hits / f, w_tc_uncached / f,
             CPU_OFFLOAD_VERIFY ? (w_tc_verify_bad ? " VERIFY FAILED" : " (verified)") : "",
             (double)w_ctl_game_us / f / 1000.0, (double)wus / f / 1000.0, w_ctl_runs / f, w_ctl_uploads / f,
             w_ctl_skips / f, w_ctl_missed / f, w_ctl_fallbacks / f);
  (void)wruns;
  w_frames = 0; w_frame_us = 0; w_text_us = 0;
  w_tc_calls = w_tc_hits = w_tc_uncached = 0;
  w_ctl_runs = w_ctl_skips = w_ctl_missed = w_ctl_uploads = w_ctl_fallbacks = 0;
  w_ctl_game_us = 0;
  t0 = now;
  if (s_ab) {
    if (s_on) go_off();
    else s_on = 1;
  }
}

void cpu_offload_install(void) {
  char m = CPU_OFFLOAD_DEFAULT;
  SceUID fd = sceIoOpen(DATA_PATH "/offload_mode.txt", SCE_O_RDONLY, 0);
  if (fd >= 0) {
    char b = 0;
    if (sceIoRead(fd, &b, 1) == 1) m = b;
    sceIoClose(fd);
  }
  if (m != '1' && m != 'a') {
    log_printf("[offload] off (offload_mode '%c')", m);
    return;
  }
  s_ab = m == 'a';

  unsigned n_adv = 0, n_dtw = 0, n_img = 0, n_ctl = 0, n_dtor = 0;
  o_advance = (advance_t)take_slots("_ZN6FTFont7AdvanceEPKci7FTPoint", (uintptr_t)h_advance, &n_adv);
  o_dtw = (dtw_t)take_slots("_Z15DrawTextWrappedPKcbiiiiiiiib", (uintptr_t)h_dtw, &n_dtw);

  s_ev = sceKernelCreateEventFlag("cpu_offload", SCE_EVENT_WAITMULTIPLE, 0, NULL);
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  int worker = s_ev >= 0 && pthread_create(&th, &attr, worker_main, NULL) == 0;
  if (worker) {
    o_glimage = (glimage_t)take_slots("_ZN11CAurTexture7glImageEbPh", (uintptr_t)h_glimage, &n_img);
    for (int k = 0; k < NCLS && o_glimage; k++) {
      unsigned a = 0, b = 0, c = 0;
      o_dtor[k][0] = (dtor_t)take_slots(k_dtor_sym[k][0], (uintptr_t)k_dtor_hooks[k][0], &a);
      o_dtor[k][1] = (dtor_t)take_slots(k_dtor_sym[k][1], (uintptr_t)k_dtor_hooks[k][1], &b);
      /* Control only once both destructors are covered. */
      if (o_dtor[k][0] && o_dtor[k][1])
        o_control[k] = (control_t)take_slots(k_ctl_sym[k], (uintptr_t)k_control_hooks[k], &c);
      n_dtor += a + b;
      n_ctl += c;
    }
  }
  s_on = 1;
  s_installed = 1;
  log_printf("[offload] armed: text widths %u slots, DrawTextWrapped %u; texture controllers %u slots "
             "(destructors %u, glImage %u), worker %s%s%s",
             n_adv, n_dtw, n_ctl, n_dtor, n_img, worker ? "on core 1" : "NOT STARTED",
             s_ab ? "; A/B: on and off alternately every 10 s" : "",
             CPU_OFFLOAD_VERIFY ? "; VERIFY: every cached width checked" : "");
}

#else
void cpu_offload_install(void) {}
void cpu_offload_on_swap(void) {}
int cpu_offload_phase(void) { return -1; }
#endif
