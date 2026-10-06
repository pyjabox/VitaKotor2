/* main.c -- KOTOR II PS Vita loader
 * Adapted from VitaKotor and TheOfficialFloW's so-loader work.
 * Historical log-number comments and diagnostic narratives in this file are
 * inherited from VitaKotor (KOTOR I) unless explicitly marked as KOTOR II.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitaGL.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "so_util.h"
#include "dynlib.h"
#include "loadscreen.h"
#include "jni_patch.h"
#include "audio_patch.h"
#include "bink_patch.h"
#include "fs_patch.h"
#include "input_patch.h"
#include "sdl_patch.h"
#include "gl_patch.h"
#include "gl_worker.h"
#include "ime_patch.h"
#include "occlusion_cull.h"
#include "ai_list_cache.h"
#include "cansee_cache.h"
#include "bloom_ctl.h"
#include "gl_state_filter.h"
#include "ai_budget.h"
#include "dxt_native.h"
#include "ini_defaults.h"
#include "crash.h"
#include "heap.h"
#include "bigalloc.h"
#include "log.h"

#include <pthread.h>

so_module kotor_mod;
so_module port_mod;
so_module miniz_mod;
so_module lzma_mod;
so_module cxx_mod;

unsigned int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

int debugPrintf(const char *text, ...) {
  va_list args;
  char buf[1024];
  va_start(args, text);
  vsnprintf(buf, sizeof(buf), text, args);
  va_end(args);
  log_printf("%s", buf);
  return 0;
}

// The in-game save/load panel can be built from the desktop `saveload_p` layout,
// which does not contain the optional mobile control stored at this+0xee4.
// CSWGuiSaveLoad's constructor nevertheless dereferences it unconditionally at
// libkotor2.so+0x35f628, causing the observed null-vtable call (PC 0) when Load is
// reopened after visiting Save. The block only enables pulsing alpha on that
// optional control; skipping it is safe and leaves all save/load logic intact.

// Save/load investigation probes. These preserve the original calls and only
// report which stage is reached, its arguments, and the list state afterward.
static void (*SavePopulate_orig)(void *self) = NULL;
static int (*GetDirectoryList_orig)(void *self, void *list, const void *path,
                                    unsigned type, int directories, int sort) = NULL;
static int (*FindFirstFileA_orig)(const char *pattern, void *data) = NULL;
static int (*FindNextFileA_orig)(int handle, void *data) = NULL;
static int (*FindClose_orig)(int handle) = NULL;
static unsigned s_find_trace_n = 0;
static uintptr_t (*SaveEntryLoadData_orig)(void *self, const void *directory) = NULL;
static void (*SavePrompt_orig)(void *self, void *control) = NULL;
static void (*SaveWrite_orig)(void *self, const void *name) = NULL;
static void (*SaveOk_orig)(void *self, void *control) = NULL;

static const char *save_exostr(const void *s) {
  const char *p = s ? *(const char *const *)s : NULL;
  return p ? p : "(empty)";
}

static void trace_find_data(const char *tag, int rc, const void *data) {
  if (s_find_trace_n++ >= 160) return;
  const unsigned char *d = (const unsigned char *)data;
  const char *name = data ? (const char *)data + 0x2c : "(null)";
  log_printf("[save-find] %s rc=%d attr=0x%08x size=%u name=\"%.96s\"",
             tag, rc, data ? *(const unsigned *)d : 0,
             data ? *(const unsigned *)(d + 0x20) : 0, name);
}

static int FindFirstFileA_probe(const char *pattern, void *data) {
  log_printf("[save-find] FindFirstFileA pattern=\"%s\" data=%p", pattern ? pattern : "(null)", data);
  int rc = FindFirstFileA_orig(pattern, data);
  trace_find_data("first", rc, data);
  return rc;
}

static int FindNextFileA_probe(int handle, void *data) {
  int rc = FindNextFileA_orig(handle, data);
  trace_find_data("next", rc, data);
  return rc;
}

static int FindClose_probe(int handle) {
  int rc = FindClose_orig(handle);
  log_printf("[save-find] FindClose handle=%d -> %d", handle, rc);
  return rc;
}

static int GetDirectoryList_probe(void *self, void *list, const void *path,
                                  unsigned type, int directories, int sort) {
  int before = list ? *(int *)((char *)list + 4) : -1;
  log_printf("[save-find] GetDirectoryList ENTER path=\"%s\" type=%u dirs=%d sort=%d count=%d",
             save_exostr(path), type, directories, sort, before);
  int rc = GetDirectoryList_orig(self, list, path, type, directories, sort);
  int count = list ? *(int *)((char *)list + 4) : -1;
  void *data = list ? *(void **)list : NULL;
  log_printf("[save-find] GetDirectoryList EXIT rc=%d count=%d data=%p", rc, count, data);
  for (int i = 0; data && i < count && i < 24; i++)
    log_printf("[save-find]   result[%d]=\"%s\"", i, save_exostr((char *)data + i * 8));
  return rc;
}

static void SavePopulate_probe(void *self) {
  log_printf("[save-trace] PopulateGameList ENTER self=%p flags=0x%02x optional=%p",
             self, self ? *(unsigned char *)((char *)self + 0x268) : 0,
             self ? *(void **)((char *)self + 0xee4) : NULL);
  SavePopulate_orig(self);
  log_printf("[save-trace] PopulateGameList EXIT selected=%p list.count=%d list.data=%p",
             self ? *(void **)((char *)self + 0x1f00) : NULL,
             self ? *(int *)((char *)self + 0x1ef4) : -1,
             self ? *(void **)((char *)self + 0x1ef0) : NULL);
}

static uintptr_t SaveEntryLoadData_probe(void *self, const void *directory) {
  log_printf("[save-trace] Entry::LoadData ENTER self=%p dir=\"%s\"", self,
             save_exostr(directory));
  uintptr_t rc = SaveEntryLoadData_orig(self, directory);
  log_printf("[save-trace] Entry::LoadData EXIT rc=0x%08x slot=%d flags=0x%04x title=\"%s\"",
             (unsigned)rc, self ? *(int *)((char *)self + 0x1e4) : -999,
             self ? *(unsigned short *)((char *)self + 0x1e0) : 0,
             self ? save_exostr((char *)self + 0x200) : "(null)");
  return rc;
}

static void SavePrompt_probe(void *self, void *control) {
  log_printf("[save-trace] PromptForSaveName self=%p control=%p slot=%d flags=0x%04x",
             self, control, control ? *(int *)((char *)control + 0x1e4) : -999,
             control ? *(unsigned short *)((char *)control + 0x1e0) : 0);
  SavePrompt_orig(self, control);
}

static void SaveWrite_probe(void *self, const void *name) {
  log_printf("[save-trace] WriteGame ENTER self=%p name=\"%s\" selected=%p",
             self, save_exostr(name),
             self ? *(void **)((char *)self + 0x1f00) : NULL);
  SaveWrite_orig(self, name);
  log_printf("[save-trace] WriteGame EXIT");
}

static void SaveOk_probe(void *self, void *control) {
  log_printf("[save-trace] SaveName OK self=%p owner=%p text=\"%s\"",
             self, self ? *(void **)((char *)self + 0x88) : NULL,
             self ? save_exostr((char *)self + 0x5b8) : "(null)");
  SaveOk_orig(self, control);
}

static int install_save_probe(const char *symbol, void *probe, void **original) {
  uintptr_t at = so_symbol(&kotor_mod, symbol);
  if (!at) {
    log_printf("[save-trace] symbol missing: %s", symbol);
    return 0;
  }
  size_t len = thumb_patch_len(at);
  *original = (void *)build_thumb_trampoline(at, len);
  if (!*original) {
    log_printf("[save-trace] trampoline failed: %s len=%u", symbol, (unsigned)len);
    return 0;
  }
  hook_thumb(at, (uintptr_t)probe);
  log_printf("[save-trace] hooked %s at=%p len=%u", symbol, (void *)at, (unsigned)len);
  return 1;
}

static void install_save_traces(void) {
  install_save_probe("_ZN16CExoBaseInternal16GetDirectoryListEP13CExoArrayListI10CExoStringERKS1_tii",
                     (void *)&GetDirectoryList_probe, (void **)&GetDirectoryList_orig);
  install_save_probe("FindFirstFileA", (void *)&FindFirstFileA_probe,
                     (void **)&FindFirstFileA_orig);
  install_save_probe("FindNextFileA", (void *)&FindNextFileA_probe,
                     (void **)&FindNextFileA_orig);
  install_save_probe("FindClose", (void *)&FindClose_probe,
                     (void **)&FindClose_orig);
  // Do not trampoline PopulateGameList itself. Its entry probe corrupted `this`
  // (the original received r0/r4 == -1 at +0x35fe00), so the instrumentation
  // caused the save-menu crash before enumeration could finish. The lower-level
  // GetDirectoryList/Find probes provide the required enumeration trace without
  // changing this GUI object's call boundary.
  log_printf("[save-trace] PopulateGameList entry hook disabled (ABI safety)");
  log_printf("[save-trace] Entry::LoadData hook disabled (ABI safety)");
  log_printf("[save-trace] PromptForSaveName hook disabled (ABI safety)");
  log_printf("[save-trace] WriteGame hook disabled (ABI safety)");
  log_printf("[save-trace] SaveName OK hook disabled (ABI safety)");
}

static void (*SaveNamePanel_OnPanelAdded_orig)(void *self) = NULL;

static void SaveNamePanel_OnPanelAdded_hook(void *self) {
  SaveNamePanel_OnPanelAdded_orig(self);

  // OnPanelAdded focuses the edit box, but skips platform text input whenever a
  // joystick is connected. That is always true on Vita. The edit text's
  // CExoString lives at panel+0x5b8 and stores its char pointer in word zero.
  const char *current = self ? *(const char **)((char *)self + 0x5b8) : "";
  // KOTOR II skips the Android platform-keyboard call when a joystick is
  // connected. Vita always has one, so invoke the same native IME used by the
  // working KOTOR I port. Text is returned through HandleWMCharMessage.
  log_printf("[save] save-name panel added; opening native IME with \"%s\"",
             current ? current : "");
  ime_show_keyboard(current ? current : "", 16);
}

static void patch_saveload_optional_control(void) {
  uintptr_t at = kotor_mod.text_base + 0x35f628;
  const uint32_t expected = 0x0ee4f8d4u; // ldr.w r0, [r4, #0xee4]
  if (*(const uint32_t *)at != expected) {
    log_printf("[save] optional-control patch REFUSED at %p: got 0x%08x",
               (void *)at, *(const unsigned *)at);
    return;
  }

  // b.n +0x16: skip 35f628..35f640 and resume at 35f642 (`movs r0,#2`).
  const uint16_t branch = 0xe00bu;
  kuKernelCpuUnrestrictedMemcpy((void *)at, &branch, sizeof(branch));
  kuKernelFlushCaches((void *)at, sizeof(branch));
  log_printf("[save] optional mobile control guarded at libkotor2+0x35f628");

  // PopulateGameList displays the cloud-status modal for states 0, 2, and 3.
  // Vita has no cloud backend or completion callback, so use state 4: the
  // function's own terminal/no-cloud state. This bypasses the modal while
  // retaining normal local-save enumeration.
  int *cloud = (int *)so_symbol(&kotor_mod, "g_CloudSynchStatus");
  if (cloud) {
    *cloud = 4;
    log_printf("[save] cloud sync disabled for local saves (status=4)");
  } else {
    log_printf("[save] g_CloudSynchStatus symbol missing");
  }

  uintptr_t added = so_symbol(&kotor_mod,
      "_ZN19CSWGuiSaveNamePanel12OnPanelAddedEv");
  if (added) {
    size_t len = thumb_patch_len(added);
    SaveNamePanel_OnPanelAdded_orig =
        (void (*)(void *))build_thumb_trampoline(added, len);
    if (SaveNamePanel_OnPanelAdded_orig) {
      hook_thumb(added, (uintptr_t)&SaveNamePanel_OnPanelAdded_hook);
      log_printf("[save] save-name OnPanelAdded IME hook installed");
    }
  }
}

void fatal_error(const char *fmt, ...) {
  va_list args;
  char buf[1024];
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  log_printf("[CRASH] %s", buf);

  // Show the message on-screen too, then wait so it can be read.
  vglInit(0);
  // (LiveArea/dialog wiring comes later; for now just spin.)
  while (1)
    sceKernelDelayThread(1000 * 1000);
}

int check_kubridge(void) {
  int search_unk[2] = {0};
  return _vshKernelSearchModuleByName("kubridge", search_unk);
}

int file_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

// Load + relocate + resolve a module against all three stub/wiring tables.
static int load_module(so_module *mod, const char *path, uintptr_t addr) {
  log_printf("Loading %s @ 0x%08x", path, (unsigned)addr);
  if (!file_exists(path)) {
    log_printf("  file not found on device: %s", path);
    return -1;
  }
  int res = so_load(mod, path, addr);
  if (res < 0) {
    // so_load return codes: <0 sceIoOpen (file), memblock alloc fail,
    // -1 bad ELF magic, -2 missing .dynamic/.dynsym/.rel sections.
    log_printf("  so_load failed: 0x%08x (%d)", (unsigned)res, res);
    return res;
  }
  so_relocate(mod);
  // Pass 0 also resolves cross-module (e.g. KOTOR -> libandroid_port exports).
  so_resolve(mod, default_dynlib, default_dynlib_size, 0);
  so_resolve(mod, (so_default_dynlib *)audio_get_dynlib(), audio_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)jni_get_dynlib(), jni_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)fs_get_dynlib(), fs_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)sdl_get_dynlib(), sdl_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)gl_get_dynlib(), gl_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)ime_get_dynlib(), ime_dynlib_size, 1);
  so_flush_caches(mod);
  return 0;
}

static uint64_t disk_space_left_vita(void) {
  SceIoDevInfo info;
  memset(&info, 0, sizeof(info));
  int rc = sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info));
  uint64_t free_bytes = rc >= 0 ? (uint64_t)info.free_size : 512u * 1024u * 1024u;
  log_printf("[FS] disk free: %llu MB (devctl=0x%08x)",
             (unsigned long long)(free_bytes / (1024u * 1024u)), (unsigned)rc);
  return free_bytes;
}

static unsigned report_unresolved(so_module *mod, const char *tag) {
  unsigned count = 0;
  for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
    Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] :
      &mod->relplt[i - mod->num_reldyn];
    unsigned type = ELF32_R_TYPE(rel->r_info);
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT &&
        type != R_ARM_JUMP_SLOT) continue;
    unsigned sym_index = ELF32_R_SYM(rel->r_info);
    if (!sym_index || sym_index >= (unsigned)mod->num_dynsym) continue;
    Elf32_Sym *sym = &mod->dynsym[sym_index];
    if (sym->st_shndx != SHN_UNDEF) continue;
    const char *name = mod->dynstr + sym->st_name;
    if (!sym->st_name || !*name) continue;
    uintptr_t *slot = (uintptr_t *)(mod->text_base + rel->r_offset);
    uintptr_t value = *slot;
    int unresolved = !value || value < 0x80000000u ||
      (value >= mod->text_base && value < mod->text_base + mod->text_size);
    if (!unresolved) continue;
    log_printf("[resolve] %s unresolved: %s slot=%p value=%p", tag,
               name, (void *)slot, (void *)value);
    count++;
  }
  log_printf("[resolve] %s unresolved total=%u", tag, count);
  return count;
}

// Game thread's kernel UID, published by game_main_thread so the watchdog can
// sample it. -1 until the game thread starts.
static volatile SceUID g_game_thid = -1;

// Watchdog: every 3s, snapshot the game thread. runClocks rising => it's
// spinning (CPU loop); runClocks frozen + status WAITING => blocked on a sync
// object (waitType/waitId name it). This is how we localise a hang that makes
// no traceable calls, without a userland PC read.
static uint64_t g_thread_last_run[GAME_THREADS_MAX];

/* Defined with the sound probes further down; the watchdog is its clock. */
static void sound_pipeline_census(void);
static void visibility_census(void);

static void *watchdog_thread(void *arg) {
  (void)arg;
  uint64_t last_run = 0;
  for (;;) {
    sceKernelDelayThread(3 * 1000 * 1000);
    /* Heap occupancy, sampled here because this is the one thread that already
     * ticks on a fixed schedule. One line per 3s costs ~0.3ms and is what tells
     * us whether headroom drains steadily, steps down per area, or falls off a
     * cliff -- the three explanations need different fixes. */
    {
      /* Every tick for the cheap figures; every tenth for the ones that probe
         the allocator, which is often enough to watch contiguity decay without
         poking a struggling heap two hundred times a run. */
      static unsigned tick = 0;
      if (tick++ % 10 == 0) heap_log_full(NULL); else heap_log(NULL);
    }
    /* vitaGL's own heaps, which we have never measured. They are fixed at
     * vglInitExtended and hold every texture and vertex buffer the game
     * uploads, so they are a second place to run out -- and vitaGL does not
     * check its allocations, so exhaustion there is silent: the GPU reads
     * whatever is at the pointer and the geometry smears, while the GUI (small,
     * per-frame) keeps drawing correctly. log146 ended exactly like that after
     * 44 minutes in one area with the newlib heap still healthy, which is what
     * this line is here to confirm or rule out. */
    /* Held-button mask, reported only when it moves. A bit that stays set with
     * nothing held is a missed release, and the game will act as though that
     * button is held forever -- so the interesting event is a transition that
     * never comes back to 0, and printing every tick would bury it. */
    {
      static unsigned last_mask = 0;
      static int mask_seen = 0;
      unsigned m = sdl_gamepad_mask();
      if (!mask_seen || m != last_mask) {
        log_printf("[input] held-button mask: 0x%x -> 0x%x", last_mask, m);
        last_mask = m;
        mask_seen = 1;
      }
    }
    /* Input census, every fourth tick (12s). Two lines: what the hardware is
     * doing, and what the game consumed from SDL in the same window. This tells
     * us whether a dead stick originated in the pad, SDL, or the game. */
    {
      static unsigned itick = 0;
      if (itick++ % 4 == 0) { input_probe_census(); sdl_input_census(); }
    }
    /* Sound, on the same clock and for the same reason. The pipeline line is
     * short, so every fourth tick (12s); the stats line is long and used to
     * arrive every 128th createSound, so every eighth (24s) keeps its old
     * density without inheriting its habit of thinning out exactly when the
     * game goes quiet. */
    {
      static unsigned stick = 0;
      if (stick % 4 == 0) sound_pipeline_census();
      if (stick % 4 == 0) visibility_census();
      if (stick % 8 == 0) audio_log_stats();
      stick++;
    }
    log_printf("[vgl] free: vram %u/%u KB, ram %u/%u KB",
               (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u),
               (unsigned)(vglMemTotal(VGL_MEM_VRAM) / 1024u),
               (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024u),
               (unsigned)(vglMemTotal(VGL_MEM_RAM) / 1024u));
    SceUID thid = g_game_thid;
    if (thid < 0)
      continue;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int r = sceKernelGetThreadInfo(thid, &info);
    if (r < 0) {
      log_printf("[wd] getThreadInfo(0x%x) failed 0x%08x", thid, (unsigned)r);
      continue;
    }
    uint64_t run = (uint64_t)info.runClocks;
    const char *st = info.status == SCE_THREAD_RUNNING ? "RUNNING"
                   : info.status == SCE_THREAD_READY   ? "READY"
                   : info.status == SCE_THREAD_WAITING ? "WAITING"
                   : info.status == SCE_THREAD_DORMANT ? "DORMANT"
                   : info.status == SCE_THREAD_DELETED ? "DELETED(stackoverflow?)"
                   : "?";
    log_printf("[wd] status=%s(0x%x) waitType=0x%x waitId=0x%x cpu=%d prio=%d "
               "runClk=%llu dClk=%llu %s",
               st, (unsigned)info.status, (unsigned)info.waitType,
               (unsigned)info.waitId, (int)info.currentCpuId,
               (int)info.currentPriority, (unsigned long long)run,
               (unsigned long long)(run - last_run),
               (run == last_run) ? "<< FROZEN (blocked)" : "(running)");
    last_run = run;

    // The game thread being healthy tells us nothing when the stall is on a
    // worker (log72: main thread renders the loading screen forever while all
    // I/O stops). Sweep every thread the game created and report the ones that
    // are NOT accumulating runtime -- those are the blocked ones, and `entry`
    // feeds straight into addr2line against libkotor2.so / libObbVfs.so.
    for (int i = 0; i < g_game_threads_n && i < GAME_THREADS_MAX; i++) {
      SceUID t = g_game_threads[i].thid;
      if (t < 0 || t == thid)
        continue;
      SceKernelThreadInfo ti;
      memset(&ti, 0, sizeof(ti));
      ti.size = sizeof(ti);
      if (sceKernelGetThreadInfo(t, &ti) < 0)
        continue;
      uint64_t r2 = (uint64_t)ti.runClocks;
      uint64_t prev = g_thread_last_run[i];
      g_thread_last_run[i] = r2;
      log_printf("[wd:t%d] thid=0x%08x entry=%p status=0x%x waitType=0x%x "
                 "waitId=0x%x prio=%d runClk=%llu dClk=%llu %s",
                 i, (unsigned)t, (void *)g_game_threads[i].entry,
                 (unsigned)ti.status, (unsigned)ti.waitType, (unsigned)ti.waitId,
                 (int)ti.currentPriority, (unsigned long long)r2,
                 (unsigned long long)(r2 - prev),
                 (r2 == prev) ? "<< FROZEN (blocked)" : "(running)");
    }
  }
  return NULL;
}

// Mount the OBB game-data archives. On Android the Java layer mounts them and
// calls these natives; we have no Java, so SDL_main would spin forever waiting
// on g_obbMounted && g_patchObbMounted. Calling the game's own mountObb/
// mountPatchObb builds the ObbFile (miniz) objects into g_mainObb/g_patchObb
// AND sets the flags, so later OBB reads work (just forcing the flags would
// leave g_mainObb null -> crash). See RECON / obb-mount memory.
// Verify an OBB is actually a readable zip before we rely on it: true 64-bit
// size (sceIo) + first 4 bytes (a zip starts "PK\3\4"). mountObb sets its flag
// unconditionally even if ObbFile/miniz init fails, so a missing/bad file only
// shows up later as a null-central-directory DATA_ABORT in GetDirectoryList.
static int check_obb(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) {
    log_printf("    [obb] MISSING: %s", path);
    return 0;
  }
  unsigned char m[4] = {0};
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd >= 0) { sceIoRead(fd, m, 4); sceIoClose(fd); }
  int ok = (m[0] == 'P' && m[1] == 'K');
  log_printf("    [obb] %s size=%lld magic=%02x%02x%02x%02x %s", path,
             (long long)st.st_size, m[0], m[1], m[2], m[3],
             ok ? "(zip ok)" : "(NOT A ZIP!)");
  return ok;
}

// On Android these live under the app's private storage and the OS/installer
// guarantees they exist. Here nothing creates them, so the very first thing a
// New Game does -- `access("./gameinprogress/")` then `opendir(...)` -- fails.
// That pair is the last thing logged before the chargen DATA_ABORT in BOTH
// log59 and log61, at exactly the same point.
//
// Only the WRITABLE save/scratch dirs are created. Deliberately NOT created:
// override/, modules/, portraits/, movies/, errortex/ -- those are read paths
// served out of the OBB, and an empty real directory could plausibly make the
// game stop falling back to the archive.
static void ensure_writable_dirs(void) {
  static const char *dirs[] = {
    DATA_PATH "/gameinprogress",
    DATA_PATH "/currentgame",
    DATA_PATH "/saves",
    DATA_PATH "/rebootdata",
    DATA_PATH "/dlc",
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    int r = sceIoMkdir(dirs[i], 0777);
    // 0x80010011 == SCE_ERROR_ERRNO_EEXIST: already there, which is fine.
    log_printf("[FS] mkdir %s -> 0x%08x%s", dirs[i], (unsigned)r,
               (r >= 0 || (unsigned)r == 0x80010011u) ? " (ok)" : " (FAILED)");
  }
}

static void mount_obbs(void) {
  void (*mountObb)(void *, void *, void *) =
      (void *)so_symbol(&kotor_mod, "Java_com_aspyr_kotor_KOTOR_mountObb");
  void (*mountPatchObb)(void *, void *, void *) =
      (void *)so_symbol(&kotor_mod, "Java_com_aspyr_kotor_KOTOR_mountPatchObb");
  void *env = jni_get_env();

  int main_ok = check_obb(OBB_MAIN_PATH);
  int patch_ok = check_obb(OBB_PATCH_PATH);
  if (!main_ok || !patch_ok)
    fatal_error("OBB archives invalid/empty at %s. Copy the real .obb game data "
                "(main ~2.1GB, patch ~453MB) there; check ux0: free space.",
                DATA_PATH);

  g_io_trace = 1;   // trace miniz's fopen/fseek/ftell/fread through the mount
  if (mountObb) {
    log_printf(">>> mountObb(\"%s\")", OBB_MAIN_PATH);
    mountObb(env, NULL, (void *)OBB_MAIN_PATH);   // fake jstring == char* path
    log_printf("<<< mountObb returned");
  } else {
    log_printf("!!! Java_com_aspyr_kotor_KOTOR_mountObb not found");
  }
  if (mountPatchObb) {
    log_printf(">>> mountPatchObb(\"%s\")", OBB_PATCH_PATH);
    mountPatchObb(env, NULL, (void *)OBB_PATCH_PATH);
    log_printf("<<< mountPatchObb returned");
  } else {
    log_printf("!!! Java_com_aspyr_kotor_KOTOR_mountPatchObb not found");
  }
  g_io_trace = 0;

  // Confirm the flags the SDL_main wait loop polls actually flipped.
  uint8_t *obb = (uint8_t *)so_symbol(&port_mod, "g_obbMounted");
  uint8_t *patch = (uint8_t *)so_symbol(&port_mod, "g_patchObbMounted");
  log_printf("    g_obbMounted=%d g_patchObbMounted=%d",
             obb ? *obb : -1, patch ? *patch : -1);

  // Did the ObbFile/miniz actually load? GetDirectoryList faulted reading
  // *(*g_patchObb + 0x68) == null. Log the ObbFile ptr and that field for both.
  void **g_main = (void **)so_symbol(&port_mod, "g_mainObb");
  void **g_patch = (void **)so_symbol(&port_mod, "g_patchObb");
  if (g_main && *g_main)
    log_printf("    g_mainObb=%p  [+0x68]=%p", *g_main, ((void **)((char *)*g_main + 0x68))[0]);
  else
    log_printf("    g_mainObb=%p (null!)", g_main ? *g_main : (void *)0);
  if (g_patch && *g_patch)
    log_printf("    g_patchObb=%p [+0x68]=%p", *g_patch, ((void **)((char *)*g_patch + 0x68))[0]);
  else
    log_printf("    g_patchObb=%p (null!)", g_patch ? *g_patch : (void *)0);

  // Now that the archives are mounted, let ordinary file opens reach them too.
  // Without this, only resource-manager reads saw the OBB and anything opened as
 // a plain file (modules/*.rim among them) failed.
  sdl_obb_fallback_init(so_symbol(&port_mod, "_ZN7ObbFile10RWFromFileEPKc"),
                        (uintptr_t)g_main, (uintptr_t)g_patch);
}

// ---- FONT FIX: per-call null-guard on CAurGUIStringInternal text methods -----
// The loadscreen lays out/draws GUI text at ~103s, but the GUI font atlas
// (d2xfont16x16b, dialogfont16x16b) doesn't load until ~111s. Both text methods --
// WrapStrings(int) (layout) and Draw(float) (render) -- fetch fontInfo the same way
// (font = *(this+0x18); virtual GetFontInfo at vtable+0x38) and deref it WITH NO
// NULL CHECK, so before the font loads they fault: WrapStrings at libKOTOR+0x42e0be
// `ldr sl,[fp,#0x24]`, Draw at +0x42ebf0 `vldr s22,[fp,#12]` (fp=fontInfo=0). An
// all-or-nothing hook can't win (an unconditional no-op kills menu text too), so we
// install per-call entry guards that reproduce the function's own fontInfo lookup
// and SKIP only when it's null, otherwise chain to the real function via a
// trampoline. Reproducing the lookup is safe: it's exactly what the game does, and
// GetFontInfo returns null cleanly (it doesn't fault) when there's no font.
//
// NOTE (still open): even at the menu fontInfo stays null -- the TXI->CAurFontInfo
// metrics parse (CAurFontInfo::ParseField) never runs, so GUI text does not yet
// render. These guards keep the app ALIVE (no crash) through the whole boot;
// getting actual text is a separate fix (populate CAurFontInfo / +0x38).
// Shared: reproduce CAurGUIStringInternal's fontInfo lookup. Returns the CAurFontInfo
// pointer (may be null == font not loaded) without ever faulting.
static void *gui_string_fontinfo(void *self) {
  if (!self) return NULL;
  void *font = *(void **)((char *)self + 0x18);       // CAurTexture* for this string
  if (!font) return NULL;
  void **vtbl = *(void ***)font;
  void *(*getFontInfo)(void *) = (void *(*)(void *))vtbl[14]; // vtable + 0x38
  return getFontInfo(font);
}

// ---- WrapStrings(int): layout -------------------------------------------------
static int (*WrapStrings_orig)(void *self, int arg) = NULL;
static int WrapStrings_noop(void *self, int arg) { (void)self; (void)arg; return 0; }

static int WrapStrings_guard(void *self, int arg) {
  if (!gui_string_fontinfo(self)) {
    static int once = 0;
    if (!once) { once = 1; log_printf("[font] WrapStrings: fontInfo null -> skip layout (no font yet)"); }
    return 0;
  }
  static int once2 = 0;
  if (!once2) { once2 = 1; log_printf("[font] WrapStrings: fontInfo live -> layout (text enabled)"); }
  return WrapStrings_orig(self, arg);
}

// ---- Draw(float): render ------------------------------------------------------
// The float arg arrives softfp (in r1); keep it opaque (uint32_t) so we never touch
// s0 and it passes straight through in r1 to the original softfp function.
static void (*Draw_orig)(void *self, uint32_t xf) = NULL;
static void Draw_noop(void *self, uint32_t xf) { (void)self; (void)xf; }

static void Draw_guard(void *self, uint32_t xf) {
  if (!gui_string_fontinfo(self)) {
    static int once = 0;
    if (!once) { once = 1; log_printf("[font] Draw: fontInfo null -> skip render (no font yet)"); }
    return;
  }
  static int once2 = 0;
  if (!once2) { once2 = 1; log_printf("[font] Draw: fontInfo live -> render (text enabled)"); }
  // Scope the GL text-draw trace to exactly this call so glyph draws are
  // distinguishable from scene draws (see g_gl_text_draw in gl_patch.h).
  g_gl_text_draw = 1;
  Draw_orig(self, xf);
  g_gl_text_draw = 0;
}

// ---- FONT METRICS: serve bundled .txi as a MEMORY-backed resource --------------
// CAurTextureBasic::Init loads the font's glyph metrics via
// AurResGet(resref, ".txi", &size, flag=1). AurResGet checks the OBB resource index
// first; on a miss it has a filesystem fallback (sprintf -> SDL_RWFromFile), but that
// fallback is GATED on flag==0 (+0x40e490: `cmp r9,#0; beq fallback`). Init passes
// flag=1, so it never runs and the .txi is never found -- Aspyr ships the override
// font TGAs without their .txi. No metrics -> fontInfo NULL -> no GUI text at all.
//
// Simply forcing flag=0 is NOT enough, and is actively worse: see the layout table in
// config.h. The fallback builds an RWops-backed object, which sends AurResGetNextLine
// down its unbounded streaming scanner and off the end of the heap.
//
// So: let the game take its own fallback (which builds AND REGISTERS the object --
// registry insert at +0x40e410; AurResGetNextLine reads the most recently registered
// entry at +0x40e898, and AurResFree scans the same list), then convert the object in
// place to the memory-backed shape the bounded scanner expects.
#if FONT_TXI_MEMORY_INJECT
static void *(*AurResGet_orig)(char *resref, char *type, int *size, int flag) = NULL;

static int is_txi_type(const char *t) {
  return t && (!strcmp(t, ".txi") || !strcmp(t, "txi"));
}

// Field offsets of AurResGet's 28-byte resource object, as words.
enum {
  RES_RWOPS = 0,  // 0 => memory-backed (bounded scanner); non-0 => streaming
  RES_UNK1  = 1,  // OBB path sets 0xffffffff
  RES_LINE  = 2,  // line buffer; scanner allocates it lazily, sized by RES_LINECAP
  RES_DATA  = 3,  // the bytes the bounded scanner reads
  RES_SIZE  = 4,  // the bound it checks against
  RES_LINECAP = 5,
  RES_FREE  = 6,  // AurResFree hands THIS to the game's allocator
};

// Objects we converted, so AurResFree_hook can recognise them. Only a handful of
// fonts ever load, so a flat array beats any bookkeeping.
// The scanner CONSUMES the resource: at +0x40e900 it does `strd r3,r0,[r4,#12]`,
// advancing [+12] past each line and decrementing [+16]. So [+12] is NOT the
// allocation base by the time anything frees it -- keep our own copy, or free()
// reads a chunk header out of the file's own text (log44 died exactly that way).
#define TXI_INJECT_MAX 8
static void *txi_injected[TXI_INJECT_MAX];
static void *txi_injected_base[TXI_INJECT_MAX];
static int   txi_injected_n = 0;

// A stand-in for the SDL_RWops that normally sits at [+0]. AurResFree only ever
// touches offset +16 (rwops->close), so that is the only slot we populate. Its
// purpose is to steer AurResFree away from the allocator -- see AurResFree_hook.
static uintptr_t txi_dummy_rwops[8];
static int txi_dummy_close(void *ctx) { (void)ctx; return 0; }

// Chargen crashes because animBase->GetModel(255) (vtable slot 3 -- proven via
// CSWCObject::GetModel, which is `r0=this[0x68]; r2=vptr[12]; bx r2`) returns
// NULL: the class-selection creatures have no model. No .mdl/.mdx ever reaches
// the FS layer because models live inside the OBB's data/*.bzf archives, so the
// only place to see the failure is the resource gateway itself. Log the misses.
// (An SDL_RWFromFile MISS is NOT a failure -- ios_mm_new_en.tga misses too and
// still draws; the game falls back to the OBB. A NULL from AurResGet is real.)
static unsigned g_res_miss_n = 0;
#define RES_MISS_LOG_MAX 300

// Model resources go through here too. Log the object AurResGet just built so the
// provider's answer (res[12]/res[16], set from AurGetResource + its out-size) can
// be compared against the archive's real entry sizes, which we know exactly:
// pmbbs.mdl is 192904 unpacked / 86608 packed in player.bzf, gui3D_room.mdl 1553 /
// 530 in models.bzf. A short or zero res[16] means the provider failed and the
// header check is reading a buffer nobody filled.
static int is_model_type(const char *t) {
  return t && (!strcmp(t, ".mdl") || !strcmp(t, ".mdx") ||
               !strcmp(t, "mdl")  || !strcmp(t, "mdx"));
}
static unsigned g_resget_mdl_n = 0;

static void *AurResGet_hook(char *resref, char *type, int *size, int flag) {
  void *r = AurResGet_orig(resref, type, size, flag);

  if (is_model_type(type) && g_resget_mdl_n < 48) {
    const uint32_t *o = (const uint32_t *)r;
    log_printf("[model] AurResGet(\"%.16s\",\"%.4s\",flag=%d) -> %p%s",
               resref ? resref : "?", type, flag, r,
               !r ? "  <<< NULL" : "");
    if (r)
      log_printf("[model]   res[0]=%08x [3]=%08x m8=%u [4]=%d [5]=%d [6]=%08x",
                 o[0], o[3], (unsigned)(o[3] & 7u), (int)o[4], (int)o[5], o[6]);
    g_resget_mdl_n++;
  }

  if (!r) {
    if (g_res_miss_n < RES_MISS_LOG_MAX)
      log_printf("[res] MISS #%u \"%.16s\" type=\"%.8s\" flag=%d",
                 g_res_miss_n, resref ? resref : "?", type ? type : "?", flag);
    g_res_miss_n++;
  }

  if (r || !is_txi_type(type)) return r;

  // A .txi missed the OBB index. Re-run with the fallback enabled so the game
  // builds + registers the object and calls SDL_RWFromFile -> SDL_RWFromFile_hook
  // -> our VPK-bundled app0:fonts/<resref>.txi.
  r = AurResGet_orig(resref, type, size, 0);
  if (!r) return NULL;

  uint32_t *o = (uint32_t *)r;
  unsigned int len = 0;
  void *buf = sdl_slurp_rwops_close((void *)o[RES_RWOPS], &len);

  o[RES_RWOPS]   = 0;    // memory-backed -> take the BOUNDED scanner
  o[RES_UNK1]    = 0xffffffff;
  o[RES_LINE]    = 0;
  o[RES_DATA]    = (uint32_t)buf;
  o[RES_SIZE]    = len;  // 0 on failure -> scanner returns NULL at +0x40e890, no fault
  o[RES_LINECAP] = 8192; // what the OBB path uses; 500000 was the streaming buffer
  o[RES_FREE]    = 0;    // leak `buf` rather than hand a foreign pointer to the
                         // game's allocator -- a few KB, once, per font
  if (size) *size = (int)len;  // the fallback path writes a literal 1 here

  if (buf && txi_injected_n < TXI_INJECT_MAX) {
    txi_injected[txi_injected_n]      = r;
    txi_injected_base[txi_injected_n] = buf;  // the allocation base, not [+12]
    txi_injected_n++;
  }

  // Log every success, not just the first: only the handful of fonts we bundle can
  // get this far (texture .txi requests miss at SDL_RWFromFile and leave buf NULL),
  // and knowing WHICH fonts were served is what identified the two missing ones.
  if (buf)
    log_printf("[font] .txi injected as memory resource: %s len=%u", resref ? resref : "?", len);
  return r;
}

// AurResFree(+0x40e740) branches on [+0] exactly like AurResGetNextLine does:
//
//   [+0]!=0 -> ((SDL_RWops*)[+0])->close(), clear [+0], then registry removal
//   [+0]==0 -> CAuroraInterface::ReleaseResource([+24]) , then registry removal
//
// ReleaseResource is NOT null-safe: its first act is `ldrh r1,[r0,#-6]` (+0x4b0fcc),
// reading a 2-byte type tag out of an inline block header. Our [+24]=0 therefore
// faulted at 0xfffffffa (log43, straight after a successful parse). We cannot point
// [+24] at our malloc'd buffer either -- ReleaseResource would then hand a foreign
// pointer to the game's allocator.
//
// So at free time we put a dummy RWops back at [+0]. AurResFree takes the close
// branch, calls our no-op, and skips the allocator entirely -- while still doing the
// registry removal that keeps the game's bookkeeping correct.
static void (*AurResFree_orig)(void *res, int i) = NULL;
static void AurResFree_hook(void *res, int i) {
  for (int k = 0; k < txi_injected_n; k++) {
    if (txi_injected[k] != res) continue;

    uint32_t *o = (uint32_t *)res;
    free(txi_injected_base[k]);  // the base we recorded -- NEVER o[RES_DATA]
    o[RES_DATA]  = 0;
    o[RES_SIZE]  = 0;
    o[RES_FREE]  = 0;
    o[RES_RWOPS] = (uint32_t)&txi_dummy_rwops;  // -> close branch, no ReleaseResource

    txi_injected_n--;
    txi_injected[k]      = txi_injected[txi_injected_n];
    txi_injected_base[k] = txi_injected_base[txi_injected_n];
    break;
  }
  AurResFree_orig(res, i);
}

static void install_aurresget_hook(void) {
  txi_dummy_rwops[4] = (uintptr_t)&txi_dummy_close;  // +16 == rwops->close

  uintptr_t f = so_symbol(&kotor_mod, "_Z10AurResFreePvi");
  if (f) {
    AurResFree_orig = (void (*)(void *, int))build_thumb_trampoline(f, thumb_patch_len(f));
    if (AurResFree_orig) {
      hook_thumb(f, (uintptr_t)&AurResFree_hook);
      log_printf("[font] AurResFree HOOKED: f=0x%08x tramp=%p (skip ReleaseResource for injected .txi)",
                 (unsigned)f, (void *)AurResFree_orig);
    }
  }
  // Without the free hook the injection below would fault on teardown, so don't arm it.
  if (!AurResFree_orig) {
    log_printf("[font] AurResFree hook FAILED -- .txi injection NOT installed (would crash on free)");
    return;
  }

  uintptr_t a = so_symbol(&kotor_mod, "_Z9AurResGetPcS_Pib");
  if (!a) { log_printf("[font] AurResGet symbol missing -- .txi injection NOT installed"); return; }
  AurResGet_orig = (void *(*)(char *, char *, int *, int))build_thumb_trampoline(a, thumb_patch_len(a));
  if (AurResGet_orig) {
    hook_thumb(a, (uintptr_t)&AurResGet_hook);
    log_printf("[font] AurResGet HOOKED: a=0x%08x tramp=%p (.txi -> memory resource)", (unsigned)a, (void *)AurResGet_orig);
  } else {
    log_printf("[font] AurResGet trampoline FAILED -- .txi injection NOT installed");
  }
}
#endif  // FONT_TXI_MEMORY_INJECT

// ---- GUI IMAGE PIPELINE probe ----------------------------------------------
// Widget images load but never draw. log50/51: exactly 4 textured quads per frame
// (1024x512 + 512x1024 + 2x 32x32) with 756x106 (the ten ios_mm_*_en.tga menu
// buttons) and 256x256 (font atlas) at ZERO for entire runs -- on every screen, and
// a GUI screen with no background art renders pure black. DPI was not the cause.
//
// log52 settled the emit side. FlushBuffer opens with
//     r5 = &cm_nGUIBufferSizeUsed; ldr r0,[r5]; cmp r0,#1; blt <store 0, return>
// and it ran 97921 times with the draw histogram frozen at 32x32/512x1024/1024x512
// -- so every flush early-outs on an empty buffer and the loss is upstream, on the
// accumulate side. (The old counter hooked CAurGUIImageInternal::Draw(float), a
// 112-byte WEAK convenience overload nothing calls; the live entries are the 8-arg
// Draw/DrawBuffered(ffffhfRK6Vectorf) pair. imageDraw=0 measured dead code.)
//
// So probe the widget-level entry instead. CSWGuiImage::Draw(float) is three
// stacked silent early-outs before it dispatches to image->vtable[0x1c]:
//     r0 = this[0x24]        cbz    -> return   (image object null)
//     r1 = this[0x0c]        cmp 0  \ itt ne
//     r1 = this[0x10]        cmpne 0/ bne draw  -> else return  (extent w/h zero)
// A zero extent silences every widget on every screen while non-widget background
// art still renders -- which is the symptom, and it explains why the histogram did
// not move when the game switched from the main menu to chargen. Log which gate
// bites plus the raw values; the field offsets are inferred, the values are not.
//
// Floats arrive in core registers (softfp caller), hence uint32_t params. Both
// hooked prologues are 8 clean bytes (push/add r7/str.w) with no PC-relative loads.
static void log_screen_globals(const char *when);  // defined with the probes below
static void gui_autoscale_if_needed(void *self, int w, int h);  // defined with ScaleExt below

static void (*SWImgDraw_orig)(void *self, uint32_t f) = NULL;
static void (*FlushBuf_orig)(void *self, uint32_t f) = NULL;
static const volatile int32_t *g_gui_buf_used = NULL;
static unsigned g_flush_n = 0, g_flush_nonempty = 0;
static int32_t g_flush_max_used = 0;
static unsigned g_sw_n = 0, g_sw_gate_obj = 0, g_sw_gate_w = 0, g_sw_gate_h = 0, g_sw_pass = 0;

/* KOTOR uses AurGUISetupViewport/AurGUICloseViewport as a nested GUI clipping
 * stack, but glViewport is only a coordinate transform. Mirror this semantic
 * GUI boundary to scissor while preserving any caller-owned scissor state. */
#define GUI_VIEWPORT_STACK_MAX 16
typedef struct {
  GLboolean enabled;
  GLint box[4];
} GuiScissorState;

static int (*AurGUISetupViewport_orig)(int x, int y, int w, int h,
                                       const void *color, uint32_t clear,
                                       uint32_t alpha) = NULL;
static void (*AurGUICloseViewport_orig)(void) = NULL;
static GuiScissorState g_gui_scissor_stack[GUI_VIEWPORT_STACK_MAX];
static unsigned g_gui_scissor_depth = 0;
static unsigned g_gui_scissor_bypass_depth = 0;
static int g_gui_scissor_unbalanced_close_logged = 0;

static uintptr_t *find_jump_slot(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_relplt; i++) {
    Elf32_Rel *rel = &mod->relplt[i];
    if (ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT) continue;
    Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
    if (strcmp(mod->dynstr + sym->st_name, name) != 0) continue;
    return (uintptr_t *)(mod->text_base + rel->r_offset);
  }
  return NULL;
}

#if GUI_RESOURCE_TRACE_ENABLE
/* Behavior-preserving post-module GUI trace. All hooks replace libkotor2 PLT
 * slots; no function-entry trampolines or engine resource behavior changes. */
static uintptr_t (*GuiStartLoad_orig)(void *, const void *, int, int, int);
static void *(*GuiDemand_orig)(void *, void *);
static void *(*GuiGetKeyEntry_orig)(void *, const void *, unsigned, void *, void *);
static void *(*GuiBorderAssign_orig)(void *, const void *);
static uintptr_t (*GuiAddResourceDir_orig)(void *, const void *);
static uintptr_t (*GuiRemoveResourceDir_orig)(void *, const void *);
static uintptr_t (*GuiAddKeyTable_orig)(void *, const void *, unsigned long, unsigned long);
static uintptr_t (*GuiRemoveKeyTable_orig)(void *, const void *, unsigned long);
static unsigned g_gui_layout_depth, g_gui_demand_n, g_gui_key_n, g_gui_border_n;
static char g_gui_layout_now[17], g_gui_layout_last[17];

static void gui_fixed_name(char out[17], const void *ref) {
  memset(out, 0, 17);
  if (!ref) { strcpy(out, "(null)"); return; }
  memcpy(out, ref, 16);
  for (int i = 0; i < 16; i++)
    if (out[i] && ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)) out[i] = '.';
}

static const char *gui_exostr(const void *s) {
  const char *p = s ? *(const char *const *)s : NULL;
  return p ? p : "(empty)";
}

static uintptr_t GuiStartLoad_trace(void *self, const void *layout, int a, int b, int c) {
  char name[17]; gui_fixed_name(name, layout);
  memcpy(g_gui_layout_now, name, 17); memcpy(g_gui_layout_last, name, 17);
  g_gui_layout_depth++;
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  log_printf("[gui:res] StartLoad ENTER panel=%p layout=\"%s\" from=0x%06x depth=%u",
             self, name, (unsigned)(lr - kotor_mod.text_base), g_gui_layout_depth);
  uintptr_t rc = GuiStartLoad_orig(self, layout, a, b, c);
  log_printf("[gui:res] StartLoad EXIT panel=%p layout=\"%s\" rc=%p", self, name, (void *)rc);
  if (g_gui_layout_depth) g_gui_layout_depth--;
  if (!g_gui_layout_depth) g_gui_layout_now[0] = 0;
  return rc;
}

static void *GuiDemand_trace(void *manager, void *res) {
  void *rc = GuiDemand_orig(manager, res);
  if (g_gui_layout_depth || !rc) {
    const uint32_t *w = (const uint32_t *)res;
    log_printf("[gui:res] Demand mgr=%p res=%p id=0x%08x data=%p size=%u -> %p layout=\"%s\" [#%u]",
               manager, res, res ? w[2] : 0, res ? (void *)(uintptr_t)w[3] : NULL,
               res ? w[4] : 0, rc, g_gui_layout_depth ? g_gui_layout_now : g_gui_layout_last,
               ++g_gui_demand_n);
  }
  return rc;
}

static void *GuiGetKeyEntry_trace(void *manager, const void *ref, unsigned type,
                                  void *table_out, void *entry_out) {
  void *rc = GuiGetKeyEntry_orig(manager, ref, type, table_out, entry_out);
  if (g_gui_layout_depth || type == 2017) {
    char name[17]; gui_fixed_name(name, ref);
    void *table = table_out ? *(void **)table_out : NULL;
    void *entry = entry_out ? *(void **)entry_out : NULL;
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[gui:res] GetKeyEntry mgr=%p ref=\"%s\" type=%u -> %p table=%p entry=%p from=0x%06x layout=\"%s\" [#%u]",
               manager, name, type, rc, table, entry, (unsigned)(lr - kotor_mod.text_base),
               g_gui_layout_depth ? g_gui_layout_now : g_gui_layout_last, ++g_gui_key_n);
  }
  return rc;
}

static void *GuiBorderAssign_trace(void *dst, const void *src) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  if ((uintptr_t)src < 0x10000 || g_gui_layout_depth || g_gui_border_n < 12)
    log_printf("[gui:res] BorderAssign dst=%p src=%p from=0x%06x last=\"%s\" [#%u]%s",
               dst, src, (unsigned)(lr - kotor_mod.text_base), g_gui_layout_last,
               ++g_gui_border_n, (uintptr_t)src < 0x10000 ? " <<< LOW SOURCE" : "");
  return GuiBorderAssign_orig(dst, src);
}

static uintptr_t GuiAddResourceDir_trace(void *manager, const void *name) {
  uintptr_t rc = GuiAddResourceDir_orig(manager, name);
  log_printf("[gui:res] AddResourceDirectory mgr=%p name=\"%.64s\" -> 0x%08x",
             manager, gui_exostr(name), (unsigned)rc);
  return rc;
}
static uintptr_t GuiRemoveResourceDir_trace(void *manager, const void *name) {
  uintptr_t rc = GuiRemoveResourceDir_orig(manager, name);
  log_printf("[gui:res] RemoveResourceDirectory mgr=%p name=\"%.64s\" -> 0x%08x",
             manager, gui_exostr(name), (unsigned)rc);
  return rc;
}
static uintptr_t GuiAddKeyTable_trace(void *manager, const void *name,
                                      unsigned long type, unsigned long flags) {
  uintptr_t rc = GuiAddKeyTable_orig(manager, name, type, flags);
  log_printf("[gui:res] AddKeyTable mgr=%p name=\"%.64s\" type=%lu flags=0x%lx -> 0x%08x",
             manager, gui_exostr(name), type, flags, (unsigned)rc);
  return rc;
}
static uintptr_t GuiRemoveKeyTable_trace(void *manager, const void *name, unsigned long type) {
  uintptr_t rc = GuiRemoveKeyTable_orig(manager, name, type);
  log_printf("[gui:res] RemoveKeyTable mgr=%p name=\"%.64s\" type=%lu -> 0x%08x",
             manager, gui_exostr(name), type, (unsigned)rc);
  return rc;
}

static int replace_gui_trace_slot(const char *name, uintptr_t replacement, void **original) {
  uintptr_t *slot = find_jump_slot(&kotor_mod, name);
  if (!slot) { log_printf("[gui:res] PLT slot missing: %s", name); return 0; }
  *original = (void *)*slot;
  kuKernelCpuUnrestrictedMemcpy(slot, &replacement, sizeof replacement);
  log_printf("[gui:res] PLT trace installed: %s slot=%p original=%p", name, slot, *original);
  return 1;
}

static void install_gui_resource_trace(void) {
  replace_gui_trace_slot("_ZN11CSWGuiPanel19StartLoadFromLayoutERK7CResRefiii", (uintptr_t)&GuiStartLoad_trace, (void **)&GuiStartLoad_orig);
  replace_gui_trace_slot("_ZN10CExoResMan6DemandEP4CRes", (uintptr_t)&GuiDemand_trace, (void **)&GuiDemand_orig);
  replace_gui_trace_slot("_ZN10CExoResMan11GetKeyEntryERK7CResReftPP12CExoKeyTablePP14CKeyTableEntry", (uintptr_t)&GuiGetKeyEntry_trace, (void **)&GuiGetKeyEntry_orig);
  replace_gui_trace_slot("_ZN18CSWGuiBorderParamsaSERKS_", (uintptr_t)&GuiBorderAssign_trace, (void **)&GuiBorderAssign_orig);
  replace_gui_trace_slot("_ZN10CExoResMan20AddResourceDirectoryERK10CExoString", (uintptr_t)&GuiAddResourceDir_trace, (void **)&GuiAddResourceDir_orig);
  replace_gui_trace_slot("_ZN10CExoResMan23RemoveResourceDirectoryERK10CExoString", (uintptr_t)&GuiRemoveResourceDir_trace, (void **)&GuiRemoveResourceDir_orig);
  replace_gui_trace_slot("_ZN10CExoResMan11AddKeyTableERK10CExoStringmm", (uintptr_t)&GuiAddKeyTable_trace, (void **)&GuiAddKeyTable_orig);
  replace_gui_trace_slot("_ZN10CExoResMan14RemoveKeyTableERK10CExoStringm", (uintptr_t)&GuiRemoveKeyTable_trace, (void **)&GuiRemoveKeyTable_orig);
}
#endif

#if SAVE_LIST_TRACE_ENABLE
/* Behavior-preserving save-list diagnostics. These replace existing PLT slots,
 * avoiding the function-entry trampolines that previously corrupted call state. */
static uintptr_t (*SaveGetDirectoryList_orig)(void *, void *, const void *,
                                               unsigned, int, int);
static uintptr_t (*SaveLoadData_slot_orig)(void *, const void *);
static unsigned g_save_list_calls, g_save_load_calls;

static uintptr_t SaveGetDirectoryList_trace(void *base, void *list,
                                             const void *path, unsigned type,
                                             int directories, int sort) {
  uintptr_t rc = SaveGetDirectoryList_orig(base, list, path, type,
                                            directories, sort);
  const char *path_text = path ? *(const char *const *)path : NULL;
  int count = list ? *(const int *)((const char *)list + 4) : -1;
  const char *items = list ? *(const char *const *)list : NULL;
  log_printf("[save-list] directories path=\"%s\" type=%u dirs=%d "
             "sort=%d rc=%p count=%d call=%u",
             path_text ? path_text : "(empty)", type, directories, sort,
             (void *)rc, count, ++g_save_list_calls);
  for (int i = 0; items && i < count && i < 32; i++) {
    const char *name = *(const char *const *)(items + i * 8);
    log_printf("[save-list]   directory[%d]=\"%s\"", i,
               name ? name : "(empty)");
  }
  return rc;
}

static uintptr_t SaveLoadData_slot_trace(void *entry, const void *directory) {
  const char *name = directory ? *(const char *const *)directory : NULL;
  uintptr_t rc = SaveLoadData_slot_orig(entry, directory);
  int slot = entry ? *(const int *)((const char *)entry + 0x1e4) : -999;
  unsigned flags = entry ?
      *(const unsigned short *)((const char *)entry + 0x1e0) : 0;
  const char *title = entry ?
      *(const char *const *)((const char *)entry + 0x200) : NULL;
  log_printf("[save-list] LoadData dir=\"%s\" rc=%p slot=%d "
             "flags=0x%04x title=\"%s\" entry=%p call=%u",
             name ? name : "(empty)", (void *)rc, slot, flags,
             title ? title : "(empty)", entry, ++g_save_load_calls);
  return rc;
}

static void install_save_list_trace(void) {
  uintptr_t *dir_slot = find_jump_slot(
      &kotor_mod,
      "_ZN8CExoBase16GetDirectoryListEP13CExoArrayListI10CExoStringERKS1_tii");
  uintptr_t *load_slot = find_jump_slot(
      &kotor_mod,
      "_ZN19CSWGuiSaveLoadEntry8LoadDataERK10CExoString");
  if (!dir_slot || !load_slot) {
    log_printf("[save-list] install FAILED directory-slot=%p load-slot=%p",
               dir_slot, load_slot);
    return;
  }
  SaveGetDirectoryList_orig = (void *)*dir_slot;
  SaveLoadData_slot_orig = (void *)*load_slot;
  uintptr_t dir_replacement = (uintptr_t)&SaveGetDirectoryList_trace;
  uintptr_t load_replacement = (uintptr_t)&SaveLoadData_slot_trace;
  kuKernelCpuUnrestrictedMemcpy(dir_slot, &dir_replacement,
                                sizeof dir_replacement);
  kuKernelCpuUnrestrictedMemcpy(load_slot, &load_replacement,
                                sizeof load_replacement);
  log_printf("[save-list] PLT trace installed directory=%p->%p "
             "load=%p->%p", dir_slot, SaveGetDirectoryList_orig,
             load_slot, SaveLoadData_slot_orig);
}
#endif

#if OPTIONS_RESOURCE_DIRECTORY_FIX
/* HandleSaveButton registers OPTIONS: only to open OPTIONS:OPT immediately
 * afterward.  Through the Android OBB VFS, enumerating that pseudo-directory
 * produces a broad key table which shadows the real GUI tables.  Preserve the
 * alias/file lookup and report the directory registration as successful, but
 * do not create a resource table for this one exact pseudo-directory. */
static uintptr_t (*OptionsAddResourceDirectory_orig)(void *, const void *);

static uintptr_t OptionsAddResourceDirectory_fix(void *manager, const void *name) {
  const char *text = name ? *(const char *const *)name : NULL;
  if (text && strcmp(text, "OPTIONS:") == 0) {
    log_printf("[gui:fix] suppressing OPTIONS: resource-directory table; "
               "OPTIONS:OPT file lookup remains unchanged (mgr=%p)", manager);
    return 1;
  }
  return OptionsAddResourceDirectory_orig(manager, name);
}

static void install_options_resource_directory_fix(void) {
  const char *symbol = "_ZN10CExoResMan20AddResourceDirectoryERK10CExoString";
  uintptr_t *slot = find_jump_slot(&kotor_mod, symbol);
  if (!slot) {
    log_printf("[gui:fix] OPTIONS: fix FAILED: AddResourceDirectory PLT slot missing");
    return;
  }
  OptionsAddResourceDirectory_orig =
      (uintptr_t (*)(void *, const void *))*slot;
  uintptr_t replacement = (uintptr_t)&OptionsAddResourceDirectory_fix;
  kuKernelCpuUnrestrictedMemcpy(slot, &replacement, sizeof replacement);
  log_printf("[gui:fix] OPTIONS: resource-directory fix installed via PLT "
             "slot=%p original=%p", slot, OptionsAddResourceDirectory_orig);
}
#endif

static void gui_scissor_restore(const GuiScissorState *state) {
  glScissor(state->box[0], state->box[1], state->box[2], state->box[3]);
  if (state->enabled) glEnable(GL_SCISSOR_TEST);
  else                glDisable(GL_SCISSOR_TEST);
}

static int AurGUISetupViewport_scissor(int x, int y, int w, int h,
                                       const void *color, uint32_t clear,
                                       uint32_t alpha) {
  if (g_gui_scissor_depth >= GUI_VIEWPORT_STACK_MAX) {
    log_printf("[gui:viewport] stack overflow at depth=%u", g_gui_scissor_depth);
    int rc = AurGUISetupViewport_orig(x, y, w, h, color, clear, alpha);
    if (rc) g_gui_scissor_bypass_depth++;
    return rc;
  }

  GuiScissorState *state = &g_gui_scissor_stack[g_gui_scissor_depth++];
  state->enabled = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_SCISSOR_BOX, state->box);
  g_gl_gui_viewport_scope++;
  int rc = AurGUISetupViewport_orig(x, y, w, h, color, clear, alpha);
  if (!rc) {
    g_gl_gui_viewport_scope--;
    g_gui_scissor_depth--;
    gui_scissor_restore(state);
  }
  return rc;
}

static void AurGUICloseViewport_scissor(void) {
  if (g_gui_scissor_bypass_depth) {
    AurGUICloseViewport_orig();
    g_gui_scissor_bypass_depth--;
    return;
  }
  if (!g_gui_scissor_depth) {
    if (!g_gui_scissor_unbalanced_close_logged) {
      log_printf("[gui:viewport] close without matching successful setup");
      g_gui_scissor_unbalanced_close_logged = 1;
    }
    AurGUICloseViewport_orig();
    return;
  }

  GuiScissorState state = g_gui_scissor_stack[g_gui_scissor_depth - 1];
  AurGUICloseViewport_orig();
  g_gl_gui_viewport_scope--;
  g_gui_scissor_depth--;
  gui_scissor_restore(&state);
}

static void install_gui_viewport_fix(void) {
  uintptr_t *setup_slot = find_jump_slot(&kotor_mod,
      "_Z19AurGUISetupViewportiiiiRK6Vectorbf");
  uintptr_t *close_slot = find_jump_slot(&kotor_mod,
      "_Z19AurGUICloseViewportv");
  if (!setup_slot || !close_slot) {
    log_printf("[gui:viewport] AurGUI PLT replacement FAILED setup=%p close=%p",
               (void *)setup_slot, (void *)close_slot);
    return;
  }

  AurGUISetupViewport_orig =
      (int (*)(int, int, int, int, const void *, uint32_t, uint32_t))*setup_slot;
  AurGUICloseViewport_orig = (void (*)(void))*close_slot;
  uintptr_t setup_replacement = (uintptr_t)&AurGUISetupViewport_scissor;
  uintptr_t close_replacement = (uintptr_t)&AurGUICloseViewport_scissor;
  kuKernelCpuUnrestrictedMemcpy(setup_slot, &setup_replacement,
                                sizeof setup_replacement);
  kuKernelCpuUnrestrictedMemcpy(close_slot, &close_replacement,
                                sizeof close_replacement);
  log_printf("[gui:viewport] nested AurGUI clipping enabled via PLT "
             "setup_slot=%p close_slot=%p", (void *)setup_slot,
             (void *)close_slot);
}

/* Which widgets actually went through ScaleExtentForResolution.
 *
 * Two theories about the oversized minimap and the fog panel have now died on
 * hardware -- the extent counters (log155: 1200 loaded, 2047 scaled) and the
 * NPOT pad content (log156: resampled 81 times, boxes unchanged). The counter
 * comparison was never evidence in the first place: ExtentLoad and ScaleExtent
 * are tallies over DIFFERENT objects, one widget can be scaled repeatedly, and
 * SetExtent installs extents that ExtentLoad never saw. A total tells you
 * nothing about whether THIS widget was scaled.
 *
 * So record the identity, not the count. Every widget that passes through
 * ScaleExtent goes in this set; any large image reports, once, whether its own
 * pointer is in it. An element left at authored size inside a frame scaled by
 * 0.7083 is 1.41x too big for that frame, which is precisely how both the
 * minimap and the fog panel overflow. If the offending widget comes back
 * scaled=NO, that is the bug and the fix is to scale it. If it comes back
 * scaled=YES, the extent path is exonerated for good and the cause is in the
 * draw itself. */
#define GUI_PTRSET_SLOTS 1024              /* power of two; open addressing */
typedef struct { uint32_t slot[GUI_PTRSET_SLOTS]; unsigned n, overflow; } GuiPtrSet;
static GuiPtrSet g_gui_scaled;             /* widgets ScaleExtent has touched */
static GuiPtrSet g_gui_reported;           /* big images already logged once */

static unsigned gui_ptr_hash(uint32_t p) { return ((p >> 2) * 2654435761u) & (GUI_PTRSET_SLOTS - 1); }

/* Returns 1 if p was ALREADY present. Insert-and-test in one pass so the draw
 * path can use it directly as a once-only gate. */
static int gui_ptrset_add(GuiPtrSet *s, uint32_t p) {
  if (!p) return 1;
  unsigned h = gui_ptr_hash(p);
  for (unsigned i = 0; i < GUI_PTRSET_SLOTS; i++) {
    unsigned k = (h + i) & (GUI_PTRSET_SLOTS - 1);
    if (s->slot[k] == p) return 1;
    if (!s->slot[k]) { s->slot[k] = p; s->n++; return 0; }
  }
  s->overflow++;                            /* full: report rather than lie */
  return 1;
}
static int gui_ptrset_has(const GuiPtrSet *s, uint32_t p) {
  unsigned h = gui_ptr_hash(p);
  for (unsigned i = 0; i < GUI_PTRSET_SLOTS; i++) {
    unsigned k = (h + i) & (GUI_PTRSET_SLOTS - 1);
    if (s->slot[k] == p) return 1;
    if (!s->slot[k]) return 0;
  }
  return 0;
}
static unsigned g_bigimg_logged = 0;
#if GUI_AUTOSCALE_UNSCALED_IMAGES
static unsigned g_autoscaled = 0;
#endif

static void SWImgDraw_probe(void *self, uint32_t f) {
  const uint32_t *o = (const uint32_t *)self;
  uint32_t img = o[9];  // +0x24
  uint32_t w = o[3];    // +0x0c
  uint32_t h = o[4];    // +0x10
  if (!img)     g_sw_gate_obj++;
  else if (!w)  g_sw_gate_w++;
  else if (!h)  g_sw_gate_h++;
  else          g_sw_pass++;
  /* The boxes are big. Report each large image once, with the one fact that
   * separates the two remaining theories. Capped, and the cap is printed --
   * a capped counter read as a finding has cost this port two hardware runs. */
  if (img && (int)w >= 200 && (int)h >= 200 && g_bigimg_logged < 200) {
    uint32_t sp = (uint32_t)(uintptr_t)self;
    if (!gui_ptrset_add(&g_gui_reported, sp)) {
      g_bigimg_logged++;
      log_printf("[gui] big image #%u self=0x%08x img=0x%08x w=%d h=%d scaled=%s"
                 "  (scaled set %u entries, %u overflowed)",
                 g_bigimg_logged, sp, (unsigned)img, (int)w, (int)h,
                 gui_ptrset_has(&g_gui_scaled, sp) ? "YES" : "NO",
                 g_gui_scaled.n, g_gui_scaled.overflow);
    }
    gui_autoscale_if_needed(self, (int)w, (int)h);
  }
  if ((g_sw_n++ % 2400) == 0) {
    log_printf("[gui] SWImage::Draw n=%u gates objnull=%u w0=%u h0=%u PASS=%u "
               "(last img=0x%08x w=%d h=%d)",
               g_sw_n, g_sw_gate_obj, g_sw_gate_w, g_sw_gate_h, g_sw_pass,
               (unsigned)img, (int)w, (int)h);
    // Also sample at draw time: the globals may be set long after ImgInit ran.
    if ((g_sw_n % 24000) == 1) log_screen_globals("at draw");
  }
  SWImgDraw_orig(self, f);
}

static void FlushBuf_probe(void *self, uint32_t f) {
  int32_t used = g_gui_buf_used ? *g_gui_buf_used : -1;
  if (used > 0) g_flush_nonempty++;
  if (used > g_flush_max_used) g_flush_max_used = used;
  if ((g_flush_n++ % 2400) == 0)
    log_printf("[gui] flushBuffer=%u bufUsed=%d nonEmpty=%u maxUsed=%d swDraw=%u",
               g_flush_n, (int)used, g_flush_nonempty, (int)g_flush_max_used, g_sw_n);
  FlushBuf_orig(self, f);
}

// log53: 8161/8161 widget draws bailed on extent.width==0, with extent.height==480
// and a valid image object. SetExtent proves the layout -- it does
//     vld1.32 {d16-d17},[r1] ; adds r5,r4,#4 ; vst1.32 {d16-d17},[r5]
// i.e. it blits the whole CSWGuiExtent {x,y,w,h} to this+0x04..+0x13, so
// +0x0c IS width and +0x10 IS height. 480 is KOTOR's authoring height (GUIs are
// laid out in a 640x480 virtual space), so height survived the trip and width did
// not -- width is computed somewhere else and lands at 0.
//
// Find that somewhere: log the incoming extent AND the caller. The hook is
// installed as LDR PC,[PC] over the first 8 bytes -- a branch, not a call -- so LR
// still holds the original call site and __builtin_return_address(0) recovers it.
// Resolve the printed offset against libkotor2.so's dynamic symbols.
// Prologue is 8 clean bytes (push/add r7/mov r4,r0/ldr r0,[r0,#36]); the vld1 that
// would matter starts at +8. Signature is (this, const CSWGuiExtent*) -- no floats.
static void (*SetExtent_orig)(void *self, const void *ext) = NULL;
static unsigned g_se_n = 0, g_se_w0 = 0;

// log56: SetExtent NEVER receives a good width -- because it is not how the extent
// gets in. CSWGuiImage::Initialize writes it DIRECTLY:
//     vld1.32 {d16-d17},[r1] ; adds r1,r0,#4 ; vst1.32 {d16-d17},[r1] ; b.w SetParams
// no SetExtent call at all. Every SetExtent we logged was downstream code
// re-applying &this->extent (SetImage/operator= pass this+4 to themselves) long
// after it was already zero. So hook the real writer and log what it is handed.
// Site is 0-mod-4 and the first 8 bytes are vld1(4)+adds(2)+adds(2) -- clean.
static void (*ImgInit_orig)(void *self, const void *ext, const void *params) = NULL;
static unsigned g_ii_n = 0, g_ii_w0 = 0;

// log57: the port asks JNI for GetScreenHeightPixel (we answer 544) and
// GetScreenHeightInch -- and NEVER asks for a width. So width is derived inside
// libKOTOR, and `g_nScreenWidth`/`g_nScreenHeight` (adjacent globals, 0x5b0538 /
// 0x5b053c) are where it lands. A zero width there would explain every symptom at
// once: extents with a good h and w=0, on every widget, on every screen.
static const volatile int32_t *g_scr_w = NULL, *g_scr_h = NULL;
static const volatile int32_t *g_scr_wp2 = NULL, *g_scr_hp2 = NULL;

static void log_screen_globals(const char *when) {
  log_printf("[gui] screen: g_nScreenWidth=%d g_nScreenHeight=%d "
             "cm_nScreenWidthPow2=%d cm_nScreenHeightPow2=%d  (%s)",
             g_scr_w ? (int)*g_scr_w : -1, g_scr_h ? (int)*g_scr_h : -1,
             g_scr_wp2 ? (int)*g_scr_wp2 : -1, g_scr_hp2 ? (int)*g_scr_hp2 : -1,
             when);
}

static void ImgInit_probe(void *self, const void *ext, const void *params) {
  const int32_t *e = (const int32_t *)ext;
  if (e && e[2] == 0) g_ii_w0++;
  if (g_ii_n < 96 || (g_ii_n % 240) == 0) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[gui] ImgInit #%u self=%p ext={x=%d y=%d w=%d h=%d} w0=%u from off=0x%06x",
               g_ii_n, self, e ? (int)e[0] : -1, e ? (int)e[1] : -1,
               e ? (int)e[2] : -1, e ? (int)e[3] : -1, g_ii_w0,
               (unsigned)(lr - kotor_mod.text_base));
    log_screen_globals("at ImgInit");
  }
  g_ii_n++;
  ImgInit_orig(self, ext, params);
}

static void SetExtent_probe(void *self, const void *ext) {
  const int32_t *e = (const int32_t *)ext;
  if (e && e[2] == 0) g_se_w0++;
  // First 64 in full (that covers main-menu construction), then thin out.
  if (g_se_n < 64 || (g_se_n % 240) == 0) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[gui] SetExtent #%u self=%p ext={x=%d y=%d w=%d h=%d} w0=%u "
               "from off=0x%06x",
               g_se_n, self, e ? (int)e[0] : -1, e ? (int)e[1] : -1,
               e ? (int)e[2] : -1, e ? (int)e[3] : -1, g_se_w0,
               (unsigned)(lr - kotor_mod.text_base));
  }
  g_se_n++;
  SetExtent_orig(self, ext);
}

// Upstream of SetExtent: CSWGuiControl::Load calls CSWGuiExtent::Load, which reads
// four INT fields -- "LEFT","TOP","WIDTH","HEIGHT" -> extent+0,+4,+8,+12 (labels
// resolved from its literal pool). extent+8 is the width that reads 0.
//
// But note 0x49f358: `cbz r0, 0x49f3b6` -- if GetStructFromStruct("EXTENT") fails,
// ALL FOUR reads are skipped and the caller's CSWGuiExtent keeps whatever stale
// stack bytes it had. w=0/h=480 is exactly what uninitialized stack looks like, so
// "EXTENT struct not found" and "WIDTH field read returned the 0 default" are both
// live and they need different fixes.
//
// Distinguish them without changing behaviour: stamp the 16-byte extent with a
// sentinel, run the real Load, then see which words the callee actually wrote. Any
// word still holding the sentinel was never written -- restore the caller's
// original bytes there so the game sees exactly what it would have seen.
#define EXT_SENTINEL 0x5A5A5A5A
static unsigned g_sx_n = 0;      /* ScaleExtent calls, read by the totals line */
static int (*ExtLoad_orig)(void *self, void *gff, void *st) = NULL;
static unsigned g_xl_n = 0, g_xl_skipped = 0, g_xl_w0 = 0;

static int ExtLoad_probe(void *self, void *gff, void *st) {
  int32_t *e = (int32_t *)self;
  int32_t saved[4] = {e[0], e[1], e[2], e[3]};
  for (int i = 0; i < 4; i++) e[i] = EXT_SENTINEL;

  int rc = ExtLoad_orig(self, gff, st);

  unsigned unwritten = 0;
  for (int i = 0; i < 4; i++) {
    if (e[i] == EXT_SENTINEL) { unwritten |= (1u << i); e[i] = saved[i]; }
  }
  if (unwritten == 0xF) g_xl_skipped++;  // EXTENT struct not found -> nothing read
  if (e[2] == 0) g_xl_w0++;

  if (g_xl_n < 64 || (g_xl_n % 240) == 0)
    log_printf("[gui] ExtentLoad #%u rc=%d {L=%d T=%d W=%d H=%d} unwritten=0x%x "
               "skipped=%u w0=%u",
               g_xl_n, rc, (int)e[0], (int)e[1], (int)e[2], (int)e[3],
               unwritten, g_xl_skipped, g_xl_w0);
  if ((g_xl_n % 240) == 0)
    log_printf("[gui] extent totals: %u loaded, %u scaled  (a gap here is real, "
               "both counters are lifetime)", g_xl_n, g_sx_n);
  g_xl_n++;
  return rc;
}

// --- chargen: is a model ever even requested? ------------------------------
// log62 ruled out the missing gameinprogress/ dir (created, still faults at
// 0x24acb8) and the resource layer (only .txi misses in the whole chargen run).
// The creature has a valid animation base but animBase->GetModel(255) returns
// NULL, and no model resource is ever REQUESTED. So watch the loader itself:
// CSWCAnimBase::LoadModel(const CResRef&, unsigned char). Never called => the
// creature is never given a model (setup bug, upstream). Called => log the
// resref and the part id, and the failure is inside model loading.
// Returns a value (callers do `blx LoadModel ; cbz r0`), so the probe must pass it
// through -- declaring it void left r0 undefined on return and could have silently
// turned a successful load into a "failed" one at the call site.
static void *(*LoadModel_orig)(void *self, const void *resref, unsigned part) = NULL;
static unsigned g_lm_n = 0;

// log63 closed the chain. CSWCAnimBase::GetModel is five instructions:
//     cmp r1,#255 ; ite eq ; ldreq r0,[r0,#0xb8] ; movne r0,#0 ; bx lr
// so GetModel(255) is literally `return this->[0xb8]`. LoadModel IS called with
// the right resrefs (pmbbs/pmbbm/pmbbl/pfbbl/pfbbm/pfbbs, part=255) and the model
// DATA does load (~190KB + ~86KB new[] right after each call) -- but this+0xb8
// stays NULL, so the chargen draw null-derefs. Sample the field either side of
// the call: still NULL afterwards => LoadModel bails internally after reading the
// data, and the next step is bisecting its 424 bytes.
// log64 traced the whole chain:
//   LoadModel(resref,255) -> this[0xb8] = NewCAurObject(name,"body",NULL,NULL)
//   NewCAurObject: RWops args are NULL, so it takes the load-by-NAME path ->
//     FindModel("pmbbs") / "pmbbs_x" / "pmbbs_z"; returns NULL if the base one is
//   FindModel -> BinaryFindModel: `count = table[4]; if (count < 1) return NULL`
// i.e. an EMPTY model registry answers every lookup with NULL. g_nModelsRead is
// the engine's own count of models read into that registry -- if it is 0, nothing
// ever populated it and that (not chargen) is the real bug.
static const volatile int32_t *g_models_read = NULL;

// log65 narrowed it one more hop. g_nModelsRead is NOT 0 (it climbs 4,6,8,10,12,14
// -- 2 per LoadModel), so the registry IS being fed and the *lookup* is what fails.
// FindModel's load-on-miss path is:
//     IODispatcher::ReadSync(name) -> MaxTree*
//     MaxTree::AsModel()  ==  `if ((this[0x4c] & 0x7f) != 2) return NULL;`
//     strcasecmp(loaded->name, requested) -> mismatch writes AR_ERROR.LOG
// No RWFromFile fires in the LoadModel window, so AR_ERROR.LOG is never opened and
// the name-mismatch branch is NOT taken. That leaves ReadSync returning NULL, or
// returning a tree whose type tag != 2 (read fine, parsed as the wrong node type).
// Log the pointer and that tag byte to separate the two.
static void *(*ReadSync_orig)(void *self, char *name) = NULL;
static unsigned g_rs_n = 0;

// ReadSync's four exits, in order:
//   (1) AurResGet(name,".mdl",NULL,1) == NULL         -> NULL
//   (2) AurResGetDataBytes(4, res)    == NULL         -> NULL
//   (3) first byte != 0  -> the non-binary-MDL branch (binary MDL starts 0x00)
//   (4) MaxTree::AsModel() tag != 2                   -> NULL
// (1) is already excluded: no [res] MISS is logged for pmbbs, so AurResGet
// succeeds. Probe the header fetch to separate (2) from (3)/(4). Same resource
// family as the .txi work -- note the flag=1 (OBB blob) vs flag=0 (RWops stream)
// split that bit us there.
static void *(*ResDataBytes_orig)(unsigned long n, void *res) = NULL;
static unsigned g_rdb_n = 0;

// log67 pinned the divergence to WHICH BUFFER res[12] points at. AurResGet's OBB
// path is `r5 = AurGetResource(resref,type,&size); res[12] = res[24] = r5`, and
// AurResGetDataBytes' blob path just returns that cursor unchecked. Aligning the
// 15 observed reads mod 8 splits them cleanly:
//   working .mdl reads  -> ptr % 8 == 0   (plain decompressed new[](size) buffer)
//   every failing read  -> ptr % 8 == 6   (pool block: new[](size+6), data at +6,
//                                          the 6-byte inline header ReleaseResource
//                                          reads back via `ldrh [r0,#-6]`)
// The archive itself is exonerated: cgbody_light/pmbbs/pfbbl all LZMA-decompress
// offline to a clean `00 00 00 00` binary-MDL signature, so the bytes exist and
// the header check is right to reject what it was handed. What we do NOT know is
// what the pool block actually CONTAINS, and that is the whole question:
//   5d 00 00 00 01 ... -> the raw LZMA stream: decompression never ran
//   uninitialised junk -> it ran, but into the other buffer / it failed
//   valid MDL, shifted -> cursor/offset arithmetic is off
// So dump the bytes, the 6-byte block header, the res fields, and the thread id
// (a loader thread racing the pool would explain why menu models load and chargen
// ones do not).
static const char *g_rs_name = NULL;   // resref of the ReadSync in flight

static void *ResDataBytes_probe(unsigned long n, void *res) {
  void *p = ResDataBytes_orig(n, res);
  if (g_rdb_n < 96) {
    const uint32_t *o = (const uint32_t *)res;
    char hex[48], hdr[24];
    hex[0] = hdr[0] = 0;
    if (p) {
      const unsigned char *b = (const unsigned char *)p;
      for (int i = 0; i < 12; i++) sprintf(hex + i * 3, "%02x ", b[i]);
      for (int i = 0; i < 6; i++)  sprintf(hdr + i * 3, "%02x ", b[i - 6]);
    }
    log_printf("[model] RDB(%lu,%p) -> %p m8=%u \"%.16s\" res[0]=%08x [3]=%08x "
               "[4]=%d [6]=%08x hdr:%s| %s tid=%08x",
               n, res, p, p ? (unsigned)((uintptr_t)p & 7u) : 9u,
               g_rs_name ? g_rs_name : "-", o[0], o[3], (int)o[4], o[6],
               hdr, hex, (unsigned)sceKernelGetThreadId());
  }
  g_rdb_n++;
  return p;
}

static void *ReadSync_probe(void *self, char *name) {
  const char *prev = g_rs_name;
  g_rs_name = name;                 // tag the RDB reads this ReadSync makes
  void *r = ReadSync_orig(self, name);
  g_rs_name = prev;
  if (g_rs_n < 48) {
    int tag = r ? (int)(*(unsigned char *)((char *)r + 0x4c) & 0x7f) : -1;
    log_printf("[model] ReadSync(\"%.24s\") -> %p tag=%d%s",
               name ? name : "?", r, tag,
               !r          ? "  <<< NULL (read failed)"
               : tag != 2  ? "  <<< NOT A MODEL (AsModel returns NULL)"
                           : "  ok");
  }
  g_rs_n++;
  return r;
}

static void *LoadModel_probe(void *self, const void *resref, unsigned part) {
  void *before = *(void **)((char *)self + 0xb8);
  void *rc = LoadModel_orig(self, resref, part);
  void *after = *(void **)((char *)self + 0xb8);
  if (g_lm_n < 64)
    log_printf("[model] LoadModel #%u resref=\"%.16s\" part=%u this+0xb8: %p -> %p%s"
               "  g_nModelsRead=%d",
               g_lm_n, resref ? (const char *)resref : "?", part & 0xff,
               before, after, after ? "" : "  <<< STILL NULL",
               g_models_read ? (int)*g_models_read : -1);
  g_lm_n++;
  return rc;
}

// --- touch calibration: where ARE the widgets after scaling? ---------------
// Touch is fluid but lands off the buttons. ExtentLoad logs the AUTHORED extent
// (1024x768 space); what the hit-test and the renderer actually use is the
// SCALED one (x screenHeight/768 = 0.625 here). Log both ends so the button's
// real on-screen rect can be compared against the normalized touch coords that
// actually activate it -- measurement, not arithmetic guesswork.
// CSWGuiObject keeps its extent at this+0x08 (SetExtent/ScaleExtent both use it).
static void (*ScaleExt_orig)(void *self, uint32_t fscale) = NULL;

static void ScaleExt_probe(void *self, uint32_t fscale) {
  const int32_t *e = (const int32_t *)((const char *)self + 8);
  int32_t b[4] = {e[0], e[1], e[2], e[3]};
  gui_ptrset_add(&g_gui_scaled, (uint32_t)(uintptr_t)self);
  ScaleExt_orig(self, fscale);
  /* Cadence matched to ExtentLoad's on purpose. At a flat cap of 48 this went
   * quiet at t=145s while ExtentLoad ran on to #2400, and comparing the two
   * logged counts then "showed" 25 extents that were never scaled -- an
   * artifact of the cap, not a finding. Whether some extents really do skip
   * ScaleExtentForResolution is still open, and it matters: an element left at
   * authored size inside a frame scaled to 0.7083 is 1.41x too big for it,
   * which is what the minimap, the fog box and the save list all look like.
   * The save rows load as {L=471 T=358..567 W=300 H=30} in a 768-tall layout;
   * unscaled, T=567 falls off a 544-tall screen and lands on the buttons. */
  if (g_sx_n < 64 || (g_sx_n % 240) == 0) {
    float sc; memcpy(&sc, &fscale, 4);
    log_printf("[gui] ScaleExtent #%u self=0x%08x {L=%d T=%d W=%d H=%d} x%.4f -> {L=%d T=%d W=%d H=%d}",
               g_sx_n, (unsigned)(uintptr_t)self,
               (int)b[0], (int)b[1], (int)b[2], (int)b[3], sc,
               (int)e[0], (int)e[1], (int)e[2], (int)e[3]);
  }
  g_sx_n++;
}

/* Hand a never-scaled image the resolution scale the game applies to every
 * other widget. See GUI_AUTOSCALE_UNSCALED_IMAGES in config.h for why this is
 * restricted to large, sub-screen-height images: the pillarbox wings come
 * through at exactly the screen height already in device pixels, and a blanket
 * rescale would wreck every widget that is already correct.
 *
 * Calls the original through the trampoline, so it does not re-enter the probe;
 * the widget is added to the scaled set first so it can never be scaled twice
 * however many times it is drawn. */
static void gui_autoscale_if_needed(void *self, int w, int h) {
#if GUI_AUTOSCALE_UNSCALED_IMAGES
  if (!ScaleExt_orig || !g_scr_h) return;
  int sh = (int)*g_scr_h;
  if (sh <= 0 || h >= sh) return;            /* already device-space */
  if (w < 200 || h < 200) return;            /* only the elements log157 flagged */
  uint32_t sp = (uint32_t)(uintptr_t)self;
  if (gui_ptrset_add(&g_gui_scaled, sp)) return;   /* already scaled, or seen */
  float sc = (float)sh / 768.0f;             /* the factor the game uses itself */
  uint32_t bits; memcpy(&bits, &sc, 4);
  ScaleExt_orig(self, bits);
  if (g_autoscaled < 64)
    log_printf("[gui] autoscaled self=0x%08x %dx%d by x%.4f "
               "(never went through ScaleExtentForResolution)", sp, w, h, sc);
  g_autoscaled++;
#else
  (void)self; (void)w; (void)h;
#endif
}

// log68 read the failing block's contents and they are UNWRITTEN: the bytes are
// `10 40 40 81 10 40 40 81 ...` -- two identical pointers into OUR loader's .bss
// (0x8140xxxx), i.e. the fd/bk of a newlib free-list chunk. The block is otherwise
// perfect: its 6-byte header carries the right type tag (`d2 07` = 2002 = MDL) and
// res[4] is the exact unpacked size from the archive (4960 for cgbody_light, 192904
// for pmbbs). So AurGetResource located the entry, sized it, allocated and tagged a
// block -- and never decompressed into it. Working reads hold the real MDL bytes
// (`00 00 00 00 05 06 ...`, byte-identical to an offline LZMA decode).
//
// The correlation is exact and wider than the chargen crash: EVERY ptr%8==6 read is
// garbage and every ptr%8==0 read is filled, so gui3D_room.mdx and mainmenu.mdx are
// broken too -- ReadSync just never checks the .mdx, which is why the menu looked
// fine. That points at the decompressor, not at chargen.
//
// libandroid_port implements the OBB/BZF provider and imports LzmaUncompress from
// libLzmaLib (DT_NEEDED is present and so_resolve_link should bind it). Hook it and
// log both sizes in/out plus the SZ_ code, which separates the three candidates:
//   never called      -> the miniz/OBB read upstream failed
//   rc != 0           -> decode failed (1 DATA, 2 MEM, 4 UNSUPPORTED, 6 INPUT_EOF)
//   rc == 0, destLen  -> it "succeeded" into a buffer that is not this block
// Signature: int LzmaUncompress(u8 *dest, size_t *destLen, const u8 *src,
//                               size_t *srcLen, const u8 *props, size_t propsSize)
static int (*LzmaUncompress_orig)(unsigned char *, size_t *, const unsigned char *,
                                  size_t *, const unsigned char *, size_t) = NULL;
static unsigned g_lz_n = 0;

static int LzmaUncompress_probe(unsigned char *dest, size_t *destLen,
                                const unsigned char *src, size_t *srcLen,
                                const unsigned char *props, size_t propsSize) {
  size_t dl_in = destLen ? *destLen : 0;
  size_t sl_in = srcLen ? *srcLen : 0;
  int rc = LzmaUncompress_orig(dest, destLen, src, srcLen, props, propsSize);
  if (g_lz_n < 64) {
    char p[24];
    p[0] = 0;
    if (props)
      for (unsigned i = 0; i < propsSize && i < 5; i++) sprintf(p + i * 3, "%02x ", props[i]);
    log_printf("[lzma] #%u dest=%p m8=%u destLen=%u->%u src=%p srcLen=%u->%u "
               "props(%u):%s rc=%d out:%02x %02x %02x %02x",
               g_lz_n, dest, (unsigned)((uintptr_t)dest & 7u),
               (unsigned)dl_in, (unsigned)(destLen ? *destLen : 0), src,
               (unsigned)sl_in, (unsigned)(srcLen ? *srcLen : 0),
               (unsigned)propsSize, p, rc,
               dest ? dest[0] : 0, dest ? dest[1] : 0,
               dest ? dest[2] : 0, dest ? dest[3] : 0);
  }
  g_lz_n++;
  return rc;
}

static void install_lzma_probe(void) {
  uintptr_t lu = so_symbol(&lzma_mod, "LzmaUncompress");
  if (!lu) { log_printf("[lzma] LzmaUncompress symbol MISSING in libLzmaLib"); return; }
  // Confirm the companion's import actually bound here -- a silently unresolved
  // (ret0-stubbed) LzmaUncompress would produce exactly the unwritten block we see.
  uintptr_t imp = so_symbol(&port_mod, "LzmaUncompress");
  log_printf("[lzma] libLzmaLib LzmaUncompress=0x%08x  companion sees 0x%08x",
             (unsigned)lu, (unsigned)imp);
  LzmaUncompress_orig = (int (*)(unsigned char *, size_t *, const unsigned char *,
                                 size_t *, const unsigned char *, size_t))
      build_thumb_trampoline(lu, thumb_patch_len(lu));
  if (LzmaUncompress_orig) {
    hook_thumb(lu, (uintptr_t)&LzmaUncompress_probe);
    log_printf("[lzma] LzmaUncompress PROBED");
  } else {
    log_printf("[lzma] LzmaUncompress trampoline FAILED");
  }
}

// log71: chargen reaches the portrait screen, then DATA_ABORTs at libKOTOR+0x2bb016
// inside CSWGuiQuickPanel::OnSelectPortraitButton. That function does, unguarded:
//     GetModel(255) -> ldr r1,[r0]      (body -- fine now)
//     GetModel(254) -> ldr r1,[r0]      (part 254, r0 == NULL -> fault)
// The two GetModel bodies decide it:
//     CSWCAnimBaseHead::GetModel(p): p==254 -> this[0x44]; p==255 -> base; else 0
//     CSWCAnimBase::GetModel(p):     p==255 -> this[0xb8]; else 0
// and this[0x44] is written in exactly one place --
//     CSWCAnimBaseHead::LoadModel(resref, 254) @ 0x1c4875:
//         CResRef::CopyToString(buf); this[0x44] = NewCAurObject(buf, <type>, 0, 0)
// which is a DIFFERENT override from the CSWCAnimBase::LoadModel we already hook,
// so every head load so far has been invisible to us. Model DATA is now known good
// (pmbbs .mdl/.mdx bytes match an offline LZMA decode exactly), so this is about
// whether the head load is attempted at all and what NewCAurObject answers.
//
// NewCAurObject is the single funnel for instantiating any model by name, so
// logging it gives the whole picture in one line per attempt: which resrefs are
// asked for, with which type tag, and which come back NULL.
static void *(*NewCAurObject_orig)(char *name, char *type, void *rw1, void *rw2) = NULL;
static unsigned g_nao_n = 0;

static void *NewCAurObject_probe(char *name, char *type, void *rw1, void *rw2) {
  void *r = NewCAurObject_orig(name, type, rw1, rw2);
  if (g_nao_n < 96)
    log_printf("[model] NewCAurObject(\"%.20s\", \"%.12s\", rw=%p/%p) -> %p%s",
               name ? name : "(null)", type ? type : "(null)", rw1, rw2, r,
               r ? "" : "  <<< NULL");
  g_nao_n++;
  return r;
}

static void *(*HeadLoadModel_orig)(void *self, const void *resref, unsigned part) = NULL;
static unsigned g_hlm_n = 0;

static void *HeadLoadModel_probe(void *self, const void *resref, unsigned part) {
  void *before = *(void **)((char *)self + 0x44);
  void *r = HeadLoadModel_orig(self, resref, part);
  void *after = *(void **)((char *)self + 0x44);
  if (g_hlm_n < 64)
    log_printf("[model] Head::LoadModel part=%u this=%p +0x44: %p -> %p rc=%p%s",
               part & 0xff, self, before, after, r,
               after ? "" : "  <<< HEAD STILL NULL");
  g_hlm_n++;
  return r;
}

static void install_head_probe(void) {
  uintptr_t nao = so_symbol(&kotor_mod, "_Z13NewCAurObjectPcS_P9SDL_RWopsS1_");
  if (nao) {
    NewCAurObject_orig = (void *(*)(char *, char *, void *, void *))
        build_thumb_trampoline(nao, thumb_patch_len(nao));
    if (NewCAurObject_orig) {
      hook_thumb(nao, (uintptr_t)&NewCAurObject_probe);
      log_printf("[model] NewCAurObject PROBED: 0x%08x", (unsigned)nao);
    }
  } else {
    log_printf("[model] NewCAurObject symbol missing");
  }

  uintptr_t hlm = so_symbol(&kotor_mod, "_ZN16CSWCAnimBaseHead9LoadModelERK7CResRefh");
  if (hlm) {
    HeadLoadModel_orig = (void *(*)(void *, const void *, unsigned))
        build_thumb_trampoline(hlm, thumb_patch_len(hlm));
    if (HeadLoadModel_orig) {
      hook_thumb(hlm, (uintptr_t)&HeadLoadModel_probe);
      log_printf("[model] CSWCAnimBaseHead::LoadModel PROBED: 0x%08x", (unsigned)hlm);
    }
  } else {
    log_printf("[model] CSWCAnimBaseHead::LoadModel symbol missing");
  }

}

// log73: the module load stalls with NOBODY blocked. The game thread keeps running
// SDL_main's frame loop (the SDL_Delay LR resolves to SDL_main+0x1ccd, the frame
// limiter) at ~40fps forever, and the game never calls pthread_create at all -- the
// thread registry stayed empty -- so there is no worker to be stuck. The module
// load is therefore a state machine driven from the main loop, and it has simply
// stopped advancing: all resource I/O ceases at a fixed point (t=333s here, 665s in
// log72, same place both runs) and never resumes. No crash, no fault.
//
// So stop guessing at the state and read it. KOTOR's pipeline is
//   CServerExoAppInternal::StartNewModule / ExecuteLoadModule
//     -> CSWSModule::LoadModuleStart(name, flag)   @ 0x387025
//     -> ... staged work, progress bar driven by LoadScreenUpdate(a,b,c,d)
//     -> CSWSModule::LoadModuleFinish()            @ 0x388571
// If Start returns but Finish never runs, the bar freezes exactly as observed
// (~20% in the photo). LoadScreenUpdate's arguments are the stage counters, so
// logging them ON CHANGE gives a compact trace of how far the load got and which
// step it died on, without spamming a per-frame call.
//
// NOTE (learned the hard way): every probe
// here declares a void* return and passes it through. If the real function returns
// void the caller ignores r0 and nothing is harmed; if it returns a value we
// preserve it. Declaring `void` is the unsafe choice, not the neutral one.
// Shared helpers for the load/resource probes (defined here so the load probes
// below can use them). CExoString keeps its char* at offset 0.
static const char *exostr(const void *s) {
  const char *p = s ? *(const char *const *)s : NULL;
  return p ? p : "(empty)";
}

static volatile int g_in_lms = 0;

static void dump_res(const char *tag, void *res) {
  const uint32_t *w = (const uint32_t *)res;
  const unsigned char *b = (const unsigned char *)res;
  char txt[0x41];
  for (int i = 0; i < 0x40; i++)
    txt[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
  txt[0x40] = 0;
  log_printf("[res] %s CRes=%p "
             "%08x %08x %08x %08x %08x %08x %08x %08x "
             "%08x %08x %08x %08x %08x %08x %08x %08x  \"%s\"",
             tag, res, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
             w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], txt);
}

static void *(*LoadModuleStart_orig)(void *self, const void *name, int flag) = NULL;
static void *(*LoadModuleFinish_orig)(void *self) = NULL;
static void *(*LoadScreenUpdate_orig)(int a, int b, int c, int d) = NULL;

static void *LoadModuleStart_probe(void *self, const void *name, int flag) {
  audio_streaming_pause(1);
  // m_sModuleName lives at CSWSModule+0x5c (LoadModuleStart compares it against
  // the argument at +0x3870b6 and skips AddModuleResources when they match --
  // which is what happens here, because CSWSModule's constructor already
  // registered the resources). The CRes it then demands is at CSWSModule+8.
  const void *cur = (const char *)self + 0x5c;
  void *res = self ? ((void **)self)[2] : NULL;
  log_printf("[load] LoadModuleStart ENTER flag=%d  m_sModuleName=\"%.48s\" "
             "arg=\"%.48s\"  CRes(this+8)=%p",
             flag, exostr(cur), exostr(name), res);
  if (res) dump_res("before Demand", res);
  g_in_lms = 1;
  void *rc = LoadModuleStart_orig(self, name, flag);
  g_in_lms = 0;
  log_printf("[load] LoadModuleStart EXIT rc=%p", rc);
  if (res) dump_res("after Demand", res);
  return rc;
}

static void *LoadModuleFinish_probe(void *self) {
  log_printf("[load] LoadModuleFinish ENTER");
  void *rc = LoadModuleFinish_orig(self);
  audio_streaming_pause(0);
  log_printf("[load] LoadModuleFinish EXIT rc=%p", rc);
  return rc;
}

static void *LoadScreenUpdate_probe(int a, int b, int c, int d) {
  static int la = -1, lb = -1, lc = -1, ld = -1;
  static unsigned n = 0, since = 0;
  if (a != la || b != lb || c != lc || d != ld) {
    if (n < 256)
      log_printf("[load] LoadScreenUpdate(%d, %d, %d, %d)  [%u calls since last change]",
                 a, b, c, d, since);
    n++; since = 0;
    la = a; lb = b; lc = c; ld = d;
  } else {
    since++;
  }
  return LoadScreenUpdate_orig(a, b, c, d);
}

static void hook_named(const char *sym, uintptr_t probe, void **orig, const char *tag) {
  uintptr_t a = so_symbol(&kotor_mod, sym);
  if (!a) { log_printf("[load] %s symbol missing", tag); return; }
  *orig = (void *)build_thumb_trampoline(a, thumb_patch_len(a));
  if (!*orig) { log_printf("[load] %s trampoline FAILED", tag); return; }
  hook_thumb(a, probe);
  log_printf("[load] %s PROBED: 0x%08x", tag, (unsigned)a);
}

static void install_audio_load_barrier(void) {
  hook_named("_ZN10CSWSModule15LoadModuleStartERK10CExoStringi",
             (uintptr_t)&LoadModuleStart_probe, (void **)&LoadModuleStart_orig,
             "CSWSModule::LoadModuleStart audio barrier");
  hook_named("_ZN10CSWSModule16LoadModuleFinishEv",
             (uintptr_t)&LoadModuleFinish_probe, (void **)&LoadModuleFinish_orig,
             "CSWSModule::LoadModuleFinish audio barrier");
  log_printf("[snd] module-load stream barrier installed: start=%p finish=%p",
             (void *)LoadModuleStart_orig, (void *)LoadModuleFinish_orig);
}

#define DEFINE_SCENE_STAGE_WRAPPER(name, stage_id) \
  static void (*name##_orig)(void *self) = NULL; \
  static void name##_probe(void *self) { \
    gl_perf_stage_enter(stage_id); \
    name##_orig(self); \
    gl_perf_stage_leave(stage_id); \
  }

DEFINE_SCENE_STAGE_WRAPPER(SceneRenderSinglePass, GL_STAGE_SINGLE_PASS)
DEFINE_SCENE_STAGE_WRAPPER(SceneRenderDynamic, GL_STAGE_DYNAMIC)
DEFINE_SCENE_STAGE_WRAPPER(SceneRenderStatic, GL_STAGE_STATIC)
DEFINE_SCENE_STAGE_WRAPPER(SceneRenderLensFlares, GL_STAGE_LENS_FLARES)
DEFINE_SCENE_STAGE_WRAPPER(SceneDoMeshBuckets, GL_STAGE_MESH_BUCKETS)

static void (*SceneDoGobBuckets_orig)(void *self) = NULL;
static void (*SceneSetVisibility)(void *scene, void *from, void *to,
                                  int enabled) = NULL;
static unsigned g_room_vis_samples;
static unsigned g_active_room_samples;
static void (*CollectActiveRooms_orig)(void *scene, void *rooms) = NULL;
static void *(*CSWSAreaLoadRooms_orig)(void *layout) = NULL;
/* AddToArea is an Android softfp function: x/y travel as raw bits in r2/r3
 * and z/loading on the stack. Use integer carriers so the Vita hardfp compiler
 * preserves that ABI exactly when forwarding through the hook. */
static void (*CSWSDoorAddToArea_orig)(void *door, void *area,
                                      uint32_t x_bits, uint32_t y_bits,
                                      uint32_t z_bits, int loading) = NULL;
static int (*CLYTGetRoomCount)(void *layout) = NULL;
static int (*RoomMeshGetEdgeVertices)(void *mesh, int edge,
                                      float *a, float *b) = NULL;
static void **g_current_camera;
static void *g_sws_rooms;
static int g_sws_room_count;
static uint64_t g_portal_candidate_since[1024];
static unsigned char g_portal_filter_applied[1024];
static uintptr_t g_portal_filter_current;
/* Generic Gob-only refinement. Static room meshes always remain in the active
 * list. This transient set contains only Gobs owned by applied occluded rooms
 * and by no retained active room. It is rebuilt after every active-room pass. */
#define PORTAL_HIDDEN_GOBS_MAX 4096
static const void *g_portal_hidden_gobs[PORTAL_HIDDEN_GOBS_MAX];
static unsigned g_portal_hidden_gob_count;
static int (*GobVisibilityCheck_orig)(void *self) = NULL;
static int GobVisibilityCheck_probe(void *self);
#define DOOR_CENSUS_MAX 256
static void *g_door_census[DOOR_CENSUS_MAX];
static unsigned g_door_census_count;
static int g_perf_vis_unlink_active;
static void *g_perf_vis_unlink_from;
static void *g_perf_vis_unlink_to;

static int room_list_snapshot(const void *list, const void ***items, unsigned *count) {
  uintptr_t data = 0;
  int n = 0;
  memcpy(&data, list, sizeof data);
  memcpy(&n, (const char *)list + 4, sizeof n);
  if (n < 0 || n > 1024 || (n && !data)) return 0;
  *items = (const void **)data;
  *count = (unsigned)n;
  return 1;
}

static int room_ptr_in_list(const void **items, unsigned count,
                            const void *room) {
  for (unsigned i = 0; i < count; i++)
    if (items[i] == room) return 1;
  return 0;
}

static unsigned room_gob_count(uintptr_t room, int *valid) {
  uintptr_t node = 0;
  const void **gobs = NULL;
  unsigned count = 0;
  memcpy(&node, (void *)(room + 0x58), sizeof node);
  *valid = node && room_list_snapshot((void *)(node + 0x74), &gobs, &count);
  return *valid ? count : 0;
}

static int fixed_name_equal_ci(const char *a, const char *b, unsigned limit) {
  if (!a || !b) return 0;
  for (unsigned i = 0; i < limit; i++) {
    unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
    if (ca >= 'a' && ca <= 'z') ca -= 'a' - 'A';
    if (cb >= 'a' && cb <= 'z') cb -= 'a' - 'A';
    if (ca != cb) return 0;
    if (!ca) return 1;
  }
  return 1;
}

static void *find_sws_room(const char *aur_name, int *index) {
  if (!g_sws_rooms || g_sws_room_count <= 0 || g_sws_room_count > 1024)
    return NULL;
  for (int i = 0; i < g_sws_room_count; i++) {
    void *room = (char *)g_sws_rooms + i * 0x4c;
    /* CSWRoom+0x20 is its fixed CResRef, populated by SetRoomInfo while the
     * area's LYT is loaded. */
    if (fixed_name_equal_ci((char *)room + 0x20, aur_name, 16)) {
      if (index) *index = i;
      return room;
    }
  }
  return NULL;
}

/* Capture the server area's already-built walkmesh room array. LoadRooms
 * creates 0x4c-byte CSWSRoom records, transforms their meshes to world space,
 * then computes pairwise edge adjacency. Read only after the original returns. */
static void *CSWSAreaLoadRooms_probe(void *layout) {
  int count = CLYTGetRoomCount ? CLYTGetRoomCount(layout) : 0;
  /* Room loading precedes GIT door loading. Reset the passive door census for
   * the new module before AddToArea begins registering its door objects. */
  g_door_census_count = 0;
  memset(g_portal_candidate_since, 0, sizeof g_portal_candidate_since);
  memset(g_portal_filter_applied, 0, sizeof g_portal_filter_applied);
  g_portal_filter_current = 0;
  g_portal_hidden_gob_count = 0;
  void *rooms = CSWSAreaLoadRooms_orig(layout);
  if (rooms && count > 0 && count <= 1024) {
    g_sws_rooms = rooms;
    g_sws_room_count = count;
    log_printf("[room-adj] captured layout=%p rooms=%p count=%d",
               layout, rooms, count);
  } else {
    g_sws_rooms = NULL;
    g_sws_room_count = 0;
    log_printf("[room-adj] unavailable layout=%p rooms=%p count=%d",
               layout, rooms, count);
  }
  return rooms;
}

static void CSWSDoorAddToArea_probe(void *door, void *area,
                                    uint32_t x_bits, uint32_t y_bits,
                                    uint32_t z_bits, int loading) {
  CSWSDoorAddToArea_orig(door, area, x_bits, y_bits, z_bits, loading);
  if (!door || !area || g_door_census_count >= DOOR_CENSUS_MAX) return;
  for (unsigned i = 0; i < g_door_census_count; i++)
    if (g_door_census[i] == door) return;
  g_door_census[g_door_census_count++] = door;

  unsigned long object_id = 0;
  float pos[3] = {0};
  unsigned char state = 0, target_state = 0, linked = 0;
  memcpy(&object_id, (char *)door + 4, sizeof object_id);
  memcpy(pos, (char *)door + 0x94, sizeof pos);
  memcpy(&state, (char *)door + 0x31c, sizeof state);
  memcpy(&target_state, (char *)door + 0x31d, sizeof target_state);
  memcpy(&linked, (char *)door + 0x3d4, sizeof linked);
  log_printf("[door-census] index=%u door=%p id=%08lx area=%p pos=(%.2f,%.2f,%.2f) state=%u target=%u linked=%u",
             g_door_census_count - 1, door, object_id, area,
             pos[0], pos[1], pos[2], (unsigned)state,
             (unsigned)target_state, (unsigned)linked);
}

static float plane_distance(const float plane[4], const float point[3]) {
  return plane[0] * point[0] + plane[1] * point[1] +
         plane[2] * point[2] + plane[3];
}

#define PORTAL_DRY_MAX_PLANES 48
#define PORTAL_DRY_MAX_DEPTH 8

typedef struct PortalDryVolume {
  float planes[PORTAL_DRY_MAX_PLANES][4];
  int count;
} PortalDryVolume;

static int portal_quad_intersects(const PortalDryVolume *volume,
                                  const float quad[4][3]) {
  for (int p = 0; p < volume->count; p++) {
    int outside = 1;
    for (int v = 0; v < 4; v++)
      if (plane_distance(volume->planes[p], quad[v]) <= 0.001f) {
        outside = 0;
        break;
      }
    if (outside) return 0;
  }
  return 1;
}

static int portal_add_side_plane(PortalDryVolume *volume,
                                 const float camera[3],
                                 const float a[3], const float b[3],
                                 const float center[3]) {
  if (volume->count >= PORTAL_DRY_MAX_PLANES) return 0;
  float av[3] = {a[0] - camera[0], a[1] - camera[1], a[2] - camera[2]};
  float bv[3] = {b[0] - camera[0], b[1] - camera[1], b[2] - camera[2]};
  float *plane = volume->planes[volume->count];
  plane[0] = av[1] * bv[2] - av[2] * bv[1];
  plane[1] = av[2] * bv[0] - av[0] * bv[2];
  plane[2] = av[0] * bv[1] - av[1] * bv[0];
  float n2 = plane[0] * plane[0] + plane[1] * plane[1] +
             plane[2] * plane[2];
  if (n2 < 1e-8f) return 0;
  plane[3] = -(plane[0] * camera[0] + plane[1] * camera[1] +
               plane[2] * camera[2]);
  /* Match BoxAbovePlane: positive is outside, so orient the portal center to
   * the non-positive side of every camera-to-edge clipping plane. */
  if (plane_distance(plane, center) > 0.0f)
    for (int i = 0; i < 4; i++) plane[i] = -plane[i];
  volume->count++;
  return 1;
}

static int portal_child_volume(const PortalDryVolume *parent,
                               const float camera[3],
                               const float quad[4][3],
                               PortalDryVolume *child) {
  *child = *parent;
  float center[3] = {0};
  for (int v = 0; v < 4; v++)
    for (int axis = 0; axis < 3; axis++) center[axis] += quad[v][axis] * 0.25f;
  for (int edge = 0; edge < 4; edge++)
    if (!portal_add_side_plane(child, camera, quad[edge],
                               quad[(edge + 1) & 3], center))
      return 0;
  return 1;
}

#define PORTAL_DRY_MAX_VISITS 256
#define PORTAL_DRY_REVISITS_PER_ROOM 4
#define PORTAL_DOOR_MATCH_D2 1.0f

typedef struct PortalDryContext {
  unsigned char reached[1024];
  unsigned char on_path[1024];
  unsigned char visits[1024];
  int respect_closed_doors;
  unsigned total_visits;
  unsigned blocked_closed;
  unsigned matched_open;
  unsigned unmatched;
  unsigned visit_limit;
} PortalDryContext;

/* Return 1 only for a confidently matched, fully closed physical door. States
 * 1 and 2 are the engine's OPEN1/OPEN2 constants. State 3 is transitional, so
 * it and every unknown value fail open. A target open state also fails open. */
static int portal_edge_closed_door(const float a[3], const float b[3],
                                   unsigned long *door_id,
                                   unsigned char *state,
                                   unsigned char *target,
                                   float *match_d2) {
  void *nearest = NULL;
  float best = PORTAL_DOOR_MATCH_D2;
  float mx = (a[0] + b[0]) * 0.5f;
  float my = (a[1] + b[1]) * 0.5f;
  float mz = (a[2] + b[2]) * 0.5f;
  for (unsigned i = 0; i < g_door_census_count; i++) {
    void *door = g_door_census[i];
    float pos[3];
    memcpy(pos, (char *)door + 0x94, sizeof pos);
    float dx = pos[0] - mx, dy = pos[1] - my, dz = pos[2] - mz;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= best) continue;
    best = d2;
    nearest = door;
  }
  if (!nearest) return -1;
  memcpy(door_id, (char *)nearest + 4, sizeof *door_id);
  memcpy(state, (char *)nearest + 0x31c, sizeof *state);
  memcpy(target, (char *)nearest + 0x31d, sizeof *target);
  *match_d2 = best;
  if (*state == 1 || *state == 2 || *target == 1 || *target == 2)
    return 0;
  return *state == 0 && *target == 0;
}

static void portal_dry_visit(int room_index, int depth,
                             const float camera[3],
                             const PortalDryVolume *volume,
                             PortalDryContext *ctx) {
  if (room_index < 0 || room_index >= g_sws_room_count ||
      depth > PORTAL_DRY_MAX_DEPTH || ctx->on_path[room_index] ||
      ctx->total_visits >= PORTAL_DRY_MAX_VISITS ||
      ctx->visits[room_index] >= PORTAL_DRY_REVISITS_PER_ROOM) {
    if (ctx->total_visits >= PORTAL_DRY_MAX_VISITS ||
        (room_index >= 0 && room_index < g_sws_room_count &&
         ctx->visits[room_index] >= PORTAL_DRY_REVISITS_PER_ROOM))
      ctx->visit_limit++;
    return;
  }
  ctx->reached[room_index] = 1;
  ctx->visits[room_index]++;
  ctx->total_visits++;
  ctx->on_path[room_index] = 1;

  void *room = (char *)g_sws_rooms + room_index * 0x4c;
  void *mesh = NULL, *adjacency = NULL;
  int edge_count = 0;
  memcpy(&mesh, (char *)room + 0x3c, sizeof mesh);
  if (mesh) {
    memcpy(&adjacency, (char *)mesh + 0x8c, sizeof adjacency);
    memcpy(&edge_count, (char *)mesh + 0x90, sizeof edge_count);
  }
  if (!mesh || !adjacency || edge_count < 0 || edge_count > 65536) {
    ctx->on_path[room_index] = 0;
    return;
  }

  for (int edge = 0; edge < edge_count; edge++) {
    int adjacent = -1;
    memcpy(&adjacent, (char *)adjacency + edge * 8 + 4, sizeof adjacent);
    if (adjacent < 0 || adjacent >= g_sws_room_count ||
        ctx->on_path[adjacent]) continue;
    float a[3] = {0}, b[3] = {0};
    if (!RoomMeshGetEdgeVertices(mesh, edge, a, b)) continue;
    float quad[4][3] = {
      {a[0], a[1], a[2]}, {b[0], b[1], b[2]},
      {b[0], b[1], b[2] + 3.0f}, {a[0], a[1], a[2] + 3.0f}
    };
    if (!portal_quad_intersects(volume, quad)) continue;

    unsigned long door_id = 0;
    unsigned char state = 0, target = 0;
    float match_d2 = 0.0f;
    int closed = portal_edge_closed_door(a, b, &door_id, &state,
                                         &target, &match_d2);
    if (closed < 0) {
      ctx->unmatched++;
    } else if (closed) {
      if (ctx->respect_closed_doors) {
        ctx->blocked_closed++;
        continue;
      }
    } else {
      ctx->matched_open++;
    }

    PortalDryVolume child;
    if (!portal_child_volume(volume, camera, quad, &child)) continue;
    portal_dry_visit(adjacent, depth + 1, camera, &child, ctx);
  }
  ctx->on_path[room_index] = 0;
}

static int portal_compute_diff(uintptr_t current, const void **active,
                               unsigned active_count,
                               unsigned char candidates_out[1024],
                               int emit_log) {
  memset(candidates_out, 0, 1024);
  if (!current || !g_sws_rooms || g_sws_room_count <= 0 ||
      g_sws_room_count > 1024 || !g_current_camera || !*g_current_camera)
    return 0;
  int current_index = -1;
  if (!find_sws_room((char *)current + 0x78, &current_index)) return 0;

  void *camera = *g_current_camera;
  float camera_pos[3];
  float *planes = NULL;
  int plane_count = 0;
  memcpy(camera_pos, (char *)camera + 0xa8, sizeof camera_pos);
  memcpy(&planes, (char *)camera + 0x230, sizeof planes);
  memcpy(&plane_count, (char *)camera + 0x234, sizeof plane_count);
  if (!planes || plane_count <= 0 || plane_count > 16 ||
      plane_count > PORTAL_DRY_MAX_PLANES - 4 * PORTAL_DRY_MAX_DEPTH ||
      !(camera_pos[0] > -1e6f && camera_pos[0] < 1e6f &&
        camera_pos[1] > -1e6f && camera_pos[1] < 1e6f &&
        camera_pos[2] > -1e6f && camera_pos[2] < 1e6f))
    return 0;

  PortalDryVolume root = {0};
  root.count = plane_count;
  memcpy(root.planes, planes, (unsigned)plane_count * sizeof root.planes[0]);
  PortalDryContext geometry = {0};
  PortalDryContext door_aware = {0};
  door_aware.respect_closed_doors = 1;
  portal_dry_visit(current_index, 0, camera_pos, &root, &geometry);
  portal_dry_visit(current_index, 0, camera_pos, &root, &door_aware);

  /* Door-frame and threshold meshes can belong to the room across the opening.
   * Preserve every walkmesh room directly adjacent to the current room. If the
   * current room's adjacency cannot be read, the entire filter fails open. */
  unsigned char direct_adjacent[1024] = {0};
  void *current_sws = (char *)g_sws_rooms + current_index * 0x4c;
  void *current_mesh = NULL, *current_adjacency = NULL;
  int current_edge_count = 0;
  memcpy(&current_mesh, (char *)current_sws + 0x3c, sizeof current_mesh);
  if (current_mesh) {
    memcpy(&current_adjacency, (char *)current_mesh + 0x8c,
           sizeof current_adjacency);
    memcpy(&current_edge_count, (char *)current_mesh + 0x90,
           sizeof current_edge_count);
  }
  if (!current_mesh || !current_adjacency || current_edge_count < 0 ||
      current_edge_count > 65536)
    return 0;
  for (int edge = 0; edge < current_edge_count; edge++) {
    int adjacent = -1;
    memcpy(&adjacent, (char *)current_adjacency + edge * 8 + 4,
           sizeof adjacent);
    if (adjacent >= 0 && adjacent < g_sws_room_count)
      direct_adjacent[adjacent] = 1;
  }

  char geometry_rooms[768], door_rooms[768], candidates[768];
  int gr = 0, dr = 0, cr = 0;
  unsigned geometry_count = 0, door_count = 0, candidate_count = 0;
  geometry_rooms[0] = door_rooms[0] = candidates[0] = 0;
  for (int i = 0; i < g_sws_room_count; i++) {
    const char *name = (char *)g_sws_rooms + i * 0x4c + 0x20;
    if (geometry.reached[i]) {
      geometry_count++;
      if (gr < (int)sizeof geometry_rooms - 24)
        gr += snprintf(geometry_rooms + gr, sizeof geometry_rooms - gr,
                       "%.16s ", name);
    }
    if (door_aware.reached[i]) {
      door_count++;
      if (dr < (int)sizeof door_rooms - 24)
        dr += snprintf(door_rooms + dr, sizeof door_rooms - dr,
                       "%.16s ", name);
    }
  }

  /* A differential candidate must already be in the engine's active result,
   * be reachable with identical portal geometry, and disappear only when
   * confidently matched closed doors are respected. Current, forced, unknown,
   * and unmapped rooms are retained by construction. */
  for (unsigned i = 0; i < active_count; i++) {
    const void *aur = active[i];
    int sws_index = -1;
    if (!aur || aur == (const void *)current ||
        *(const unsigned char *)((const char *)aur + 0xb8) ||
        !find_sws_room((const char *)aur + 0x78, &sws_index) ||
        direct_adjacent[sws_index] || !geometry.reached[sws_index] ||
        door_aware.reached[sws_index])
      continue;
    candidates_out[sws_index] = 1;
    candidate_count++;
    if (cr < (int)sizeof candidates - 72)
      cr += snprintf(candidates + cr, sizeof candidates - cr,
                     "%.63s ", (const char *)aur + 0x78);
  }

  if (emit_log)
    log_printf("[portal-diff] current=%.63s geometry=%u door=%u candidates=%u blocked=%u geom-visits=%u door-visits=%u geom-limited=%u door-limited=%u candidate-rooms=%s geometry-rooms=%s door-rooms=%s",
               (char *)current + 0x78, geometry_count, door_count,
               candidate_count, door_aware.blocked_closed,
               geometry.total_visits, door_aware.total_visits,
               geometry.visit_limit, door_aware.visit_limit,
               candidates, geometry_rooms, door_rooms);
  /* Any traversal cap makes the differential uncertain. Fail open rather than
   * allowing a partial set to drive filtering. */
  if (geometry.visit_limit || door_aware.visit_limit) {
    memset(candidates_out, 0, 1024);
    return 0;
  }
  return 1;
}

/* Passive doorway/view-volume correlation. CollectActiveRooms rejects a room
 * when BoxAbovePlane returns positive for one camera plane. For each shared
 * walkmesh edge, report whether its floor segment or a conservative
 * three-unit-high doorway rectangle is wholly outside one of those planes. */
static void room_adjacency_telemetry(uintptr_t current) {
  if (!current || !g_sws_rooms || !RoomMeshGetEdgeVertices) return;

  void *camera = g_current_camera ? *g_current_camera : NULL;
  float camera_pos[3] = {0};
  float *planes = NULL;
  int plane_count = 0;
  if (camera) {
    memcpy(camera_pos, (char *)camera + 0xa8, sizeof camera_pos);
    memcpy(&planes, (char *)camera + 0x230, sizeof planes);
    memcpy(&plane_count, (char *)camera + 0x234, sizeof plane_count);
  }
  int camera_valid = camera && planes && plane_count > 0 && plane_count <= 16 &&
      camera_pos[0] > -1e6f && camera_pos[0] < 1e6f &&
      camera_pos[1] > -1e6f && camera_pos[1] < 1e6f &&
      camera_pos[2] > -1e6f && camera_pos[2] < 1e6f;

  int room_index = -1;
  void *sws_room = find_sws_room((char *)current + 0x78, &room_index);
  if (!sws_room) {
    log_printf("[room-adj] current=%.63s no CSWSRoom match count=%d",
               (char *)current + 0x78, g_sws_room_count);
    return;
  }

  void *mesh = NULL;
  memcpy(&mesh, (char *)sws_room + 0x3c, sizeof mesh);
  int edge_count = 0;
  void *adjacency = NULL;
  if (mesh) {
    memcpy(&adjacency, (char *)mesh + 0x8c, sizeof adjacency);
    memcpy(&edge_count, (char *)mesh + 0x90, sizeof edge_count);
  }
  if (!mesh || !adjacency || edge_count < 0 || edge_count > 65536) {
    log_printf("[room-adj] current=%.63s index=%d mesh=%p adjacency=%p edges=%d invalid",
               (char *)current + 0x78, room_index, mesh, adjacency,
               edge_count);
    return;
  }

  unsigned adjacent_count = 0;
  for (int e = 0; e < edge_count; e++) {
    int adjacent = -1;
    memcpy(&adjacent, (char *)adjacency + e * 8 + 4, sizeof adjacent);
    if (adjacent >= 0 && adjacent < g_sws_room_count) adjacent_count++;
  }
  log_printf("[room-adj] current=%.63s index=%d edges=%d adjacent=%u camera=%p valid=%d pos=(%.2f,%.2f,%.2f) planes=%d",
             (char *)current + 0x78, room_index, edge_count,
             adjacent_count, camera, camera_valid,
             camera_pos[0], camera_pos[1], camera_pos[2], plane_count);

  unsigned emitted = 0;
  for (int e = 0; e < edge_count && emitted < 64; e++) {
    int adjacent = -1, local_topology = -1;
    memcpy(&local_topology, (char *)adjacency + e * 8,
           sizeof local_topology);
    memcpy(&adjacent, (char *)adjacency + e * 8 + 4, sizeof adjacent);
    if (adjacent < 0 || adjacent >= g_sws_room_count) continue;
    void *peer = (char *)g_sws_rooms + adjacent * 0x4c;
    float a[3] = {0}, b[3] = {0};
    int valid = RoomMeshGetEdgeVertices(mesh, e, a, b);
    void *nearest_door = NULL;
    unsigned long nearest_id = 0;
    unsigned char door_state = 0, door_target = 0, door_linked = 0;
    float nearest_d2 = 1e30f, door_pos[3] = {0};
    if (valid) {
      float mx = (a[0] + b[0]) * 0.5f;
      float my = (a[1] + b[1]) * 0.5f;
      float mz = (a[2] + b[2]) * 0.5f;
      for (unsigned di = 0; di < g_door_census_count; di++) {
        void *door = g_door_census[di];
        float pos[3];
        memcpy(pos, (char *)door + 0x94, sizeof pos);
        float dx = pos[0] - mx, dy = pos[1] - my, dz = pos[2] - mz;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= nearest_d2) continue;
        nearest_d2 = d2;
        nearest_door = door;
        memcpy(door_pos, pos, sizeof door_pos);
        memcpy(&nearest_id, (char *)door + 4, sizeof nearest_id);
        memcpy(&door_state, (char *)door + 0x31c, sizeof door_state);
        memcpy(&door_target, (char *)door + 0x31d, sizeof door_target);
        memcpy(&door_linked, (char *)door + 0x3d4, sizeof door_linked);
      }
    }
    int floor_out_plane = -1, portal_out_plane = -1;
    float worst_floor = -1e30f, worst_portal = -1e30f;
    if (valid && camera_valid) {
      float at[3] = {a[0], a[1], a[2] + 3.0f};
      float bt[3] = {b[0], b[1], b[2] + 3.0f};
      for (int pi = 0; pi < plane_count; pi++) {
        const float *plane = planes + pi * 4;
        float da = plane_distance(plane, a);
        float db = plane_distance(plane, b);
        float dat = plane_distance(plane, at);
        float dbt = plane_distance(plane, bt);
        float floor_min = da < db ? da : db;
        float portal_min = floor_min;
        if (dat < portal_min) portal_min = dat;
        if (dbt < portal_min) portal_min = dbt;
        if (floor_min > worst_floor) worst_floor = floor_min;
        if (portal_min > worst_portal) worst_portal = portal_min;
        if (floor_min > 0.0f && floor_out_plane < 0) floor_out_plane = pi;
        if (portal_min > 0.0f && portal_out_plane < 0)
          portal_out_plane = pi;
      }
    }
    log_printf("[room-adj-edge] from=%.63s[%d] edge=%d to=%.16s[%d] local=%d valid=%d floor-out=%d portal-out=%d floor-margin=%.3f portal-margin=%.3f a=(%.2f,%.2f,%.2f) b=(%.2f,%.2f,%.2f) door=%p id=%08lx d2=%.3f dpos=(%.2f,%.2f,%.2f) state=%u target=%u linked=%u",
               (char *)current + 0x78, room_index, e,
               (char *)peer + 0x20, adjacent, local_topology, valid,
               floor_out_plane, portal_out_plane, worst_floor, worst_portal,
               a[0], a[1], a[2], b[0], b[1], b[2], nearest_door,
               nearest_id, nearest_d2, door_pos[0], door_pos[1], door_pos[2],
               (unsigned)door_state, (unsigned)door_target,
               (unsigned)door_linked);
    emitted++;
  }
}

/* Emulator-only proof that filtering the transient list returned by
 * CollectActiveRooms suppresses room rendering without changing canonical VIS
 * links. Fail open for invalid lists, missing current rooms, forced rooms, and
 * any room other than the explicitly configured A/B pair. */
static unsigned filter_active_rooms_ab(void *scene, void *room_list) {
  if (!PERF_ACTIVE_ROOM_FILTER_FROM[0] || !PERF_ACTIVE_ROOM_FILTER_ROOM[0])
    return 0;

  uintptr_t current = 0;
  memcpy(&current, (char *)scene + 0xe0, sizeof current);
  if (!current || strcmp((char *)current + 0x78,
                         PERF_ACTIVE_ROOM_FILTER_FROM) != 0)
    return 0;

  const void **snapshot = NULL;
  unsigned count = 0;
  if (!room_list_snapshot(room_list, &snapshot, &count) || !snapshot)
    return 0;

  void **items = (void **)snapshot;
  unsigned write_index = 0, removed = 0;
  for (unsigned read_index = 0; read_index < count; read_index++) {
    void *room = items[read_index];
    int reject = room && room != (void *)current &&
        !*(unsigned char *)((char *)room + 0xb8) &&
        strcmp((char *)room + 0x78, PERF_ACTIVE_ROOM_FILTER_ROOM) == 0;
    if (reject) {
      removed++;
      continue;
    }
    items[write_index++] = room;
  }
  if (removed) {
    for (unsigned i = write_index; i < count; i++) items[i] = NULL;
    memcpy((char *)room_list + 4, &write_index, sizeof write_index);
  }
  return removed;
}

static void portal_filter_reset(const char *reason) {
  for (int i = 0; i < g_sws_room_count && i < 1024; i++) {
    if (g_portal_filter_applied[i])
      log_printf("[portal-filter] restore room=%.16s reason=%s",
                 (char *)g_sws_rooms + i * 0x4c + 0x20, reason);
  }
  memset(g_portal_candidate_since, 0, sizeof g_portal_candidate_since);
  memset(g_portal_filter_applied, 0, sizeof g_portal_filter_applied);
  g_portal_hidden_gob_count = 0;
}

/* Emulator-only dynamic validation. The canonical VIS graph is untouched.
 * Candidates must remain continuously door-caused for the configured delay.
 * Any uncertainty, current-room change, or candidate disappearance restores
 * the room immediately by leaving it in this frame's transient result. */
static int portal_ptr_index(const void *const *items, unsigned count,
                            const void *item) {
  for (unsigned i = 0; i < count; i++)
    if (items[i] == item) return (int)i;
  return -1;
}

/* Build a conservative Gob rejection set without removing any CAurRoom. A Gob
 * is eligible only when it belongs to an applied door-occluded room and does
 * not occur in any retained active room. Shared objects, uncertain ownership,
 * overflow, and invalid room lists all fail open. */
static void portal_rebuild_hidden_gobs(const void **active,
                                       unsigned active_count,
                                       uintptr_t current) {
  g_portal_hidden_gob_count = 0;
#if PERF_DYNAMIC_PORTAL_GOB_FILTER
  const void *candidate_gobs[PORTAL_HIDDEN_GOBS_MAX];
  unsigned candidate_count = 0;
  int overflow = 0;

  for (unsigned pass = 0; pass < 2 && !overflow; pass++) {
    for (unsigned ai = 0; ai < active_count && !overflow; ai++) {
      const void *aur = active[ai];
      int sws_index = -1;
      if (!aur || !find_sws_room((const char *)aur + 0x78, &sws_index) ||
          sws_index < 0 || sws_index >= g_sws_room_count)
        continue;
      int hidden_owner = aur != (const void *)current &&
          !*(const unsigned char *)((const char *)aur + 0xb8) &&
          g_portal_filter_applied[sws_index];
      if ((pass == 0) != hidden_owner) continue;

      uintptr_t node = 0;
      const void **gobs = NULL;
      unsigned gob_count = 0;
      memcpy(&node, (const char *)aur + 0x58, sizeof node);
      if (!node || !room_list_snapshot((const char *)node + 0x74,
                                       &gobs, &gob_count)) {
        overflow = 1;
        break;
      }
      for (unsigned gi = 0; gi < gob_count; gi++) {
        const void *gob = gobs[gi];
        if (!gob) continue;
        int at = portal_ptr_index(candidate_gobs, candidate_count, gob);
        if (pass == 0) {
          if (at >= 0) continue;
          if (candidate_count >= PORTAL_HIDDEN_GOBS_MAX) {
            overflow = 1;
            break;
          }
          candidate_gobs[candidate_count++] = gob;
        } else if (at >= 0) {
          candidate_gobs[at] = NULL; /* shared with a retained active room */
        }
      }
    }
  }

  if (overflow) return;
  for (unsigned i = 0; i < candidate_count; i++)
    if (candidate_gobs[i])
      g_portal_hidden_gobs[g_portal_hidden_gob_count++] = candidate_gobs[i];
#else
  (void)active;
  (void)active_count;
  (void)current;
#endif
}

/* Deliberately aggressive distance-only benchmark. Distance is measured from
 * the live camera position (the closest stable proxy available here for the
 * player) to each room's world-space AABB. The current room is always retained;
 * all portal, door, adjacency, and force-retain semantics are ignored. */
static unsigned filter_active_rooms_distance(void *scene, void *room_list) {
#if PERF_DISTANCE_ROOM_FILTER
  uintptr_t current = 0;
  const void **snapshot = NULL;
  unsigned count = 0;
  void *camera = g_current_camera ? *g_current_camera : NULL;
  float pos[3];
  memcpy(&current, (char *)scene + 0xe0, sizeof current);
  if (!current || !camera ||
      !room_list_snapshot(room_list, &snapshot, &count) || !snapshot)
    return 0;
  memcpy(pos, (char *)camera + 0xa8, sizeof pos);
  if (!(pos[0] > -1e6f && pos[0] < 1e6f &&
        pos[1] > -1e6f && pos[1] < 1e6f &&
        pos[2] > -1e6f && pos[2] < 1e6f))
    return 0;

  const float radius2 = PERF_DISTANCE_ROOM_RADIUS_M *
                        PERF_DISTANCE_ROOM_RADIUS_M;
  void **items = (void **)snapshot;
  unsigned write_index = 0, removed = 0;
  for (unsigned read_index = 0; read_index < count; read_index++) {
    void *room = items[read_index];
    int reject = 0;
    if (room && room != (void *)current) {
      float bmin[3], bmax[3];
      memcpy(bmin, (char *)room + 0x1c, sizeof bmin);
      memcpy(bmax, (char *)room + 0x28, sizeof bmax);
      int valid = 1;
      float d2 = 0.0f;
      for (unsigned axis = 0; axis < 3; axis++) {
        if (!(bmin[axis] > -1e6f && bmin[axis] < 1e6f &&
              bmax[axis] > -1e6f && bmax[axis] < 1e6f &&
              bmin[axis] <= bmax[axis])) {
          valid = 0;
          break;
        }
        float d = pos[axis] < bmin[axis] ? bmin[axis] - pos[axis] :
                  pos[axis] > bmax[axis] ? pos[axis] - bmax[axis] : 0.0f;
        d2 += d * d;
      }
      reject = valid && d2 > radius2;
    }
    if (reject) {
      removed++;
      continue;
    }
    items[write_index++] = room;
  }
  if (removed) {
    for (unsigned i = write_index; i < count; i++) items[i] = NULL;
    memcpy((char *)room_list + 4, &write_index, sizeof write_index);
  }
  return removed;
#else
  (void)scene;
  (void)room_list;
  return 0;
#endif
}

static unsigned filter_active_rooms_dynamic(void *scene, void *room_list,
                                            int emit_diff_log) {
#if PERF_DYNAMIC_PORTAL_FILTER || PERF_DYNAMIC_PORTAL_GOB_FILTER
  uintptr_t current = 0;
  memcpy(&current, (char *)scene + 0xe0, sizeof current);
  const void **snapshot = NULL;
  unsigned count = 0;
  if (!current || !room_list_snapshot(room_list, &snapshot, &count) ||
      !snapshot || !g_sws_rooms || g_sws_room_count <= 0 ||
      g_sws_room_count > 1024) {
    portal_filter_reset("uncertain");
    g_portal_filter_current = 0;
    return 0;
  }
  if (g_portal_filter_current != current) {
    portal_filter_reset("current-room");
    g_portal_filter_current = current;
  }

  unsigned char candidates[1024];
  if (!portal_compute_diff(current, snapshot, count, candidates,
                           emit_diff_log)) {
    portal_filter_reset("invalid-diff");
    return 0;
  }

  uint64_t now = sceKernelGetProcessTimeWide();
  for (int i = 0; i < g_sws_room_count; i++) {
    if (!candidates[i]) {
      if (g_portal_filter_applied[i])
        log_printf("[portal-filter] restore room=%.16s reason=%s",
                   (char *)g_sws_rooms + i * 0x4c + 0x20,
                   "candidate-cleared");
      g_portal_candidate_since[i] = 0;
      g_portal_filter_applied[i] = 0;
      continue;
    }
    if (!g_portal_candidate_since[i]) {
      g_portal_candidate_since[i] = now;
      continue;
    }
    if (!g_portal_filter_applied[i] &&
        now - g_portal_candidate_since[i] >=
            PERF_DYNAMIC_PORTAL_DELAY_US) {
      g_portal_filter_applied[i] = 1;
      log_printf("[portal-filter] apply room=%.16s stable-us=%u",
                 (char *)g_sws_rooms + i * 0x4c + 0x20,
                 (unsigned)(now - g_portal_candidate_since[i]));
    }
  }

  portal_rebuild_hidden_gobs(snapshot, count, current);
  if (emit_diff_log && g_portal_hidden_gob_count)
    log_printf("[portal-gob] current=%.63s hidden-exclusive=%u active-rooms=%u",
               (char *)current + 0x78, g_portal_hidden_gob_count, count);

#if PERF_DYNAMIC_PORTAL_FILTER
  void **items = (void **)snapshot;
  unsigned write_index = 0, removed = 0;
  for (unsigned read_index = 0; read_index < count; read_index++) {
    void *room = items[read_index];
    int sws_index = -1;
    int reject = room && room != (void *)current &&
        !*(unsigned char *)((char *)room + 0xb8) &&
        find_sws_room((char *)room + 0x78, &sws_index) &&
        sws_index >= 0 && sws_index < g_sws_room_count &&
        g_portal_filter_applied[sws_index] && candidates[sws_index];
    if (reject) {
      removed++;
      continue;
    }
    items[write_index++] = room;
  }
  if (removed) {
    for (unsigned i = write_index; i < count; i++) items[i] = NULL;
    memcpy((char *)room_list + 4, &write_index, sizeof write_index);
  }
  return removed;
#else
  return 0;
#endif
#else
  (void)scene;
  (void)room_list;
  (void)emit_diff_log;
  return 0;
#endif
}

/* Census of the engine's post-frustum room list after the optional scoped A/B
 * refinement above. Canonical room VIS lists remain untouched. */
static void CollectActiveRooms_probe(void *scene, void *room_list) {
  CollectActiveRooms_orig(scene, room_list);
  int sample_due = (++g_active_room_samples % 120u) == 0;
  unsigned filtered = filter_active_rooms_ab(scene, room_list);
  filtered += filter_active_rooms_distance(scene, room_list);
  filtered += filter_active_rooms_dynamic(scene, room_list, sample_due);
#if PERF_ROOM_VIS_TELEMETRY
  if (!sample_due) return;

  const void **active = NULL;
  unsigned active_count = 0;
  if (!room_list_snapshot(room_list, &active, &active_count)) {
    log_printf("[active-rooms] invalid output list scene=%p", scene);
    return;
  }

  uintptr_t current = 0;
  int16_t ignore = 0;
  int total_rooms = 0;
  memcpy(&current, (char *)scene + 0xe0, sizeof current);
  memcpy(&ignore, (char *)scene + 0xe4, sizeof ignore);
  memcpy(&total_rooms, (char *)scene + 0xd8, sizeof total_rooms);

  const void **native = NULL;
  unsigned native_count = 0;
  int native_valid = current &&
      room_list_snapshot((void *)(current + 0x5c), &native, &native_count);

  char kept[768], rejected[768];
  int ku = 0, ru = 0;
  kept[0] = rejected[0] = 0;
  for (unsigned i = 0; i < active_count && ku < (int)sizeof kept - 72; i++) {
    uintptr_t room = (uintptr_t)active[i];
    if (!room) continue;
    ku += snprintf(kept + ku, sizeof kept - ku, "%.63s%s ",
                   (char *)room + 0x78,
                   *(unsigned char *)(room + 0xb8) ? "*" : "");
  }
  if (native_valid) {
    for (unsigned i = 0; i < native_count && ru < (int)sizeof rejected - 72; i++) {
      uintptr_t room = (uintptr_t)native[i];
      if (room && !room_ptr_in_list(active, active_count, (void *)room))
        ru += snprintf(rejected + ru, sizeof rejected - ru, "%.63s ",
                       (char *)room + 0x78);
    }
  }

  log_printf("[active-rooms] current=%.63s native=%s%u active=%u filtered=%u total=%d ignore=%d kept=%s rejected=%s",
             current ? (char *)current + 0x78 : "NULL",
             native_valid ? "" : "?", native_valid ? native_count : 0,
             active_count, filtered, total_rooms, (int)ignore, kept, rejected);
  room_adjacency_telemetry(current);

  /* One compact detail line per current/native room makes oversized or invalid
   * bounds and the +0xb8 force-retain flag visible without reproducing the
   * engine's camera-plane math in loader code. */
  for (unsigned i = 0; current && i <= (native_valid ? native_count : 0); i++) {
    uintptr_t room = i ? (uintptr_t)native[i - 1] : current;
    if (!room) continue;
    float lo[3], hi[3];
    memcpy(lo, (void *)(room + 0x1c), sizeof lo);
    memcpy(hi, (void *)(room + 0x28), sizeof hi);
    int gob_valid = 0;
    unsigned gobs = room_gob_count(room, &gob_valid);
    log_printf("[active-room] name=%.63s active=%d forced=%u gobs=%s%u bbox=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
               (char *)room + 0x78,
               room_ptr_in_list(active, active_count, (void *)room),
               (unsigned)*(unsigned char *)(room + 0xb8),
               gob_valid ? "" : "?", gobs,
               lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  }
#else
  (void)scene;
  (void)room_list;
  (void)filtered;
#endif
}

static void room_vis_edge_ab(void *scene, uintptr_t current) {
  if (!SceneSetVisibility || !PERF_VIS_UNLINK_FROM_ROOM[0] ||
      !PERF_VIS_UNLINK_ROOM[0]) return;

  /* Restrict the A/B mutation to the source room. Restore the verified
   * bidirectional edge as soon as the current room changes, so transitions and
   * every other room retain the original VIS graph. */
  int in_source = current &&
      strcmp((char *)current + 0x78, PERF_VIS_UNLINK_FROM_ROOM) == 0;
  if (!in_source) {
    if (g_perf_vis_unlink_active && g_perf_vis_unlink_from &&
        g_perf_vis_unlink_to) {
      SceneSetVisibility(scene, g_perf_vis_unlink_from,
                         g_perf_vis_unlink_to, 1);
      log_printf("[roomvis-ab] restored VIS edge %.63s <-> %.63s",
                 (char *)g_perf_vis_unlink_from + 0x78,
                 (char *)g_perf_vis_unlink_to + 0x78);
      g_perf_vis_unlink_active = 0;
    }
    return;
  }
  if (g_perf_vis_unlink_active) return;

  const void **links = NULL;
  unsigned link_count = 0;
  if (!room_list_snapshot((void *)(current + 0x5c), &links, &link_count))
    return;
  for (unsigned i = 0; i < link_count; i++) {
    void *linked = (void *)links[i];
    if (linked && strcmp((char *)linked + 0x78, PERF_VIS_UNLINK_ROOM) == 0) {
      g_perf_vis_unlink_from = (void *)current;
      g_perf_vis_unlink_to = linked;
      SceneSetVisibility(scene, (void *)current, linked, 0);
      g_perf_vis_unlink_active = 1;
      log_printf("[roomvis-ab] removed VIS edge %.63s <-> %.63s",
                 (char *)current + 0x78, (char *)linked + 0x78);
      return;
    }
  }
}

static void room_vis_telemetry(void *scene) {
#if PERF_ROOM_VIS_TELEMETRY
  /* Scene+0xe0 is the current CAurRoom*. CAurRoom+0x5c is its verified VIS
   * List<CAurRoom*>, +0x78 is its fixed 64-byte name, and +0x58 points to
   * nodedata whose +0x74 field is the room's List<Gob*>. These offsets come
   * directly from Scene::SetCurrentRoom, QueryVisibility, CAurRoomGetName,
   * AddObjectToRooms, and their list helpers. */
  uintptr_t current = 0;
  memcpy(&current, (char *)scene + 0xe0, sizeof current);
  room_vis_edge_ab(scene, current);

  if ((++g_room_vis_samples % 120u) != 0) return;
  if (!current) {
    log_printf("[roomvis] current=NULL");
    return;
  }

  const void **links = NULL;
  unsigned link_count = 0;
  if (!room_list_snapshot((void *)(current + 0x5c), &links, &link_count)) {
    log_printf("[roomvis] current=%p name=%.63s invalid VIS list",
               (void *)current, (char *)current + 0x78);
    return;
  }

  char summary[1536];
  int used = snprintf(summary, sizeof summary, "current=%.63s links=%u ",
                      (char *)current + 0x78, link_count);
  for (unsigned i = 0; i <= link_count && used < (int)sizeof summary - 96; i++) {
    uintptr_t room = i ? (uintptr_t)links[i - 1] : current;
    if (!room) continue;
    uintptr_t node = 0;
    memcpy(&node, (void *)(room + 0x58), sizeof node);
    unsigned gob_count = 0;
    const void **gobs = NULL;
    int valid = node &&
        room_list_snapshot((void *)(node + 0x74), &gobs, &gob_count);
    if (valid) {
      const char *room_name = (const char *)room + 0x78;
      for (unsigned g = 0; g < gob_count; g++)
        if (gobs[g]) gl_perf_gob_note_room(gobs[g], room_name);
    }
    used += snprintf(summary + used, sizeof summary - used,
                     "%.63s:%s%u ", (char *)room + 0x78,
                     valid ? "" : "?", valid ? gob_count : 0);
  }
  log_printf("[roomvis] %s", summary);
#else
  (void)scene;
#endif
}

static void SceneDoGobBuckets_probe(void *self) {
  room_vis_telemetry(self);
  gl_perf_stage_enter(GL_STAGE_GOB_BUCKETS);
  SceneDoGobBuckets_orig(self);
  gl_perf_stage_leave(GL_STAGE_GOB_BUCKETS);
}

DEFINE_SCENE_STAGE_WRAPPER(SceneDoHologramBuckets, GL_STAGE_HOLOGRAM_BUCKETS)
DEFINE_SCENE_STAGE_WRAPPER(SceneDoDistortionBuckets, GL_STAGE_DISTORTION_BUCKETS)
DEFINE_SCENE_STAGE_WRAPPER(SceneDoFadeBuckets, GL_STAGE_FADE_BUCKETS)
DEFINE_SCENE_STAGE_WRAPPER(SceneDoEmitterBucket, GL_STAGE_EMITTER_BUCKET)

static void (*SceneRenderShadows_orig)(void *self, int a, int b, int c, int d) = NULL;
static void SceneRenderShadows_probe(void *self, int a, int b, int c, int d) {
  gl_perf_stage_enter(GL_STAGE_SHADOWS);
  SceneRenderShadows_orig(self, a, b, c, d);
  gl_perf_stage_leave(GL_STAGE_SHADOWS);
}

typedef struct {
  const char *symbol;
  uintptr_t replacement;
  void **original;
  const char *tag;
} SceneStageHook;

/* Install only the three hooks required by the dynamic portal filter.  The
 * earlier build enabled PERF_DYNAMIC_PORTAL_FILTER while leaving performance
 * telemetry off, but CollectActiveRooms was installed only by the telemetry
 * hook bundle, so the filter never ran.  Keep this narrow path independent of
 * the expensive scene/callee attribution probes. */
static void install_dynamic_portal_filter(void) {
  CLYTGetRoomCount =
      (void *)so_symbol(&kotor_mod, "_ZN4CLYT12GetRoomCountEv");
  g_current_camera = (void **)so_symbol(&kotor_mod, "CurrentCamera");
  RoomMeshGetEdgeVertices = (void *)so_symbol(
      &kotor_mod, "_ZN18CSWRoomSurfaceMesh15GetEdgeVerticesEiR6VectorS1_");

  SceneStageHook hooks[] = {
    {"_ZN8CSWSArea9LoadRoomsEP4CLYT",
     (uintptr_t)&CSWSAreaLoadRooms_probe,
     (void **)&CSWSAreaLoadRooms_orig, "portalLoadRooms"},
    {"_ZN8CSWSDoor9AddToAreaEP8CSWSAreafffi",
     (uintptr_t)&CSWSDoorAddToArea_probe,
     (void **)&CSWSDoorAddToArea_orig, "portalDoorAddToArea"},
    {"_Z18CollectActiveRoomsP5SceneR4ListIP8CAurRoomE",
     (uintptr_t)&CollectActiveRooms_probe,
     (void **)&CollectActiveRooms_orig, "portalActiveRooms"},
#if PERF_DYNAMIC_PORTAL_GOB_FILTER
    {"_ZN3Gob15VisibilityCheckEv",
     (uintptr_t)&GobVisibilityCheck_probe,
     (void **)&GobVisibilityCheck_orig, "portalGobVisibility"},
#endif
  };

  for (unsigned h = 0; h < sizeof(hooks) / sizeof(hooks[0]); h++) {
    uintptr_t original = so_symbol(&kotor_mod, hooks[h].symbol);
    uintptr_t callable = 0;
    unsigned replaced = 0;
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT &&
          type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + sym->st_name, hooks[h].symbol) != 0)
        continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) continue;
      if (!callable) callable = *slot;
      kuKernelCpuUnrestrictedMemcpy(slot, &hooks[h].replacement,
                                    sizeof hooks[h].replacement);
      replaced++;
    }
    *hooks[h].original = (void *)callable;
    log_printf("[portal-filter] install %s replaced=%u original=%p",
               hooks[h].tag, replaced, (void *)callable);
  }
}

static void install_scene_stage_attribution(void) {
  SceneSetVisibility = (void *)so_symbol(
      &kotor_mod, "_ZN5Scene13SetVisibilityEP8CAurRoomS1_i");
  log_printf("[roomvis-ab] Scene::SetVisibility=%p edge=%s<->%s",
             (void *)SceneSetVisibility, PERF_VIS_UNLINK_FROM_ROOM,
             PERF_VIS_UNLINK_ROOM);

  CLYTGetRoomCount =
      (void *)so_symbol(&kotor_mod, "_ZN4CLYT12GetRoomCountEv");
  g_current_camera = (void **)so_symbol(&kotor_mod, "CurrentCamera");
  RoomMeshGetEdgeVertices = (void *)so_symbol(
      &kotor_mod, "_ZN18CSWRoomSurfaceMesh15GetEdgeVerticesEiR6VectorS1_");
  log_printf("[room-adj] GetRoomCount=%p CurrentCamera=%p GetEdgeVertices=%p",
             (void *)CLYTGetRoomCount, (void *)g_current_camera,
             (void *)RoomMeshGetEdgeVertices);

  SceneStageHook hooks[] = {
    {"_ZN8CSWSArea9LoadRoomsEP4CLYT",
     (uintptr_t)&CSWSAreaLoadRooms_probe,
     (void **)&CSWSAreaLoadRooms_orig, "loadRooms"},
    {"_ZN8CSWSDoor9AddToAreaEP8CSWSAreafffi",
     (uintptr_t)&CSWSDoorAddToArea_probe,
     (void **)&CSWSDoorAddToArea_orig, "doorAddToArea"},
    {"_Z18CollectActiveRoomsP5SceneR4ListIP8CAurRoomE",
     (uintptr_t)&CollectActiveRooms_probe,
     (void **)&CollectActiveRooms_orig, "activeRooms"},
    {"_ZN5Scene16RenderSinglePassEv", (uintptr_t)&SceneRenderSinglePass_probe,
     (void **)&SceneRenderSinglePass_orig, "single"},
    {"_ZN5Scene13RenderShadowsEiiii", (uintptr_t)&SceneRenderShadows_probe,
     (void **)&SceneRenderShadows_orig, "shadow"},
    {"_ZN5Scene21RenderDynamicGeometryEv", (uintptr_t)&SceneRenderDynamic_probe,
     (void **)&SceneRenderDynamic_orig, "dynamic"},
    {"_ZN5Scene20RenderStaticGeometryEv", (uintptr_t)&SceneRenderStatic_probe,
     (void **)&SceneRenderStatic_orig, "static"},
    {"_ZN5Scene16RenderLensFlaresEv", (uintptr_t)&SceneRenderLensFlares_probe,
     (void **)&SceneRenderLensFlares_orig, "flare"},
    {"_ZN5Scene13DoMeshBucketsEv", (uintptr_t)&SceneDoMeshBuckets_probe,
     (void **)&SceneDoMeshBuckets_orig, "mesh"},
    {"_ZN5Scene12DoGobBucketsEv", (uintptr_t)&SceneDoGobBuckets_probe,
     (void **)&SceneDoGobBuckets_orig, "gob"},
    {"_ZN5Scene20DoHologramGobBucketsEv", (uintptr_t)&SceneDoHologramBuckets_probe,
     (void **)&SceneDoHologramBuckets_orig, "hologram"},
    {"_ZN5Scene19DoDistortionBucketsEv", (uintptr_t)&SceneDoDistortionBuckets_probe,
     (void **)&SceneDoDistortionBuckets_orig, "distort"},
    {"_ZN5Scene13DoFadeBucketsEv", (uintptr_t)&SceneDoFadeBuckets_probe,
     (void **)&SceneDoFadeBuckets_orig, "fade"},
    {"_ZN5Scene15DoEmitterBucketEv", (uintptr_t)&SceneDoEmitterBucket_probe,
     (void **)&SceneDoEmitterBucket_orig, "emitter"},
  };

  for (unsigned h = 0; h < sizeof(hooks) / sizeof(hooks[0]); h++) {
    uintptr_t original = so_symbol(&kotor_mod, hooks[h].symbol);
    uintptr_t callable = 0;
    unsigned replaced = 0, mismatch = 0;
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT &&
          type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + sym->st_name, hooks[h].symbol) != 0) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) {
        mismatch++;
        continue;
      }
      if (!callable) callable = *slot;
      kuKernelCpuUnrestrictedMemcpy(slot, &hooks[h].replacement,
                                    sizeof hooks[h].replacement);
      replaced++;
    }
    *hooks[h].original = (void *)callable;
    log_printf("[stage] install %s replaced=%u mismatch=%u original=%p",
               hooks[h].tag, replaced, mismatch, (void *)callable);
  }
}

#define DEFINE_RENDER_CALLEE_WRAPPER(name, callee_id, args, callargs) \
  static void (*name##_orig) args = NULL; \
  static void name##_probe args { \
    gl_perf_callee_enter(callee_id); \
    name##_orig callargs; \
    gl_perf_callee_leave(callee_id); \
  }

static void (*GobRender_orig)(void *self, int recurse) = NULL;
static const char *(*GobGetModelName)(void *self) = NULL;
static unsigned g_perf_model_cull_calls;

static int ascii_prefix_ci(const char *s, const char *prefix) {
  if (!s || !prefix) return 0;
  while (*prefix) {
    char a = *s++, b = *prefix++;
    if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
    if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
    if (a != b) return 0;
  }
  return 1;
}

/* Character families observed in KOTOR II. Party/stunt models use P_, PLC_,
 * PMH/PFH and related prefixes; generic NPC bodies use N_. Room models such as
 * 001EBO* deliberately do not match. This is a visual diagnostic only. */
static int perf_is_character_model(const char *name) {
  return ascii_prefix_ci(name, "P_") || ascii_prefix_ci(name, "PLC_") ||
         ascii_prefix_ci(name, "PMH") || ascii_prefix_ci(name, "PFH") ||
         ascii_prefix_ci(name, "N_");
}

static void GobRender_probe(void *self, int recurse) {
  const char *model_name = GobGetModelName ? GobGetModelName(self) : NULL;
  gl_perf_gob_enter(self, model_name);
  gl_perf_callee_enter(GL_CALLEE_GOB_RENDER);
  if (PERF_CULL_ALL_CHARACTERS && perf_is_character_model(model_name)) {
    unsigned n = ++g_perf_model_cull_calls;
    if (n <= 12)
      log_printf("[model-cull] skipped character %s gob=%p call=%u",
                 model_name, self, n);
  } else {
    GobRender_orig(self, recurse);
  }
  gl_perf_callee_leave(GL_CALLEE_GOB_RENDER);
  gl_perf_gob_leave(self);
}
DEFINE_RENDER_CALLEE_WRAPPER(GobPartDraw, GL_CALLEE_GOB_PART_DRAW,
                             (void *self, void *part, int recurse), (self, part, recurse))
DEFINE_RENDER_CALLEE_WRAPPER(PartTriMeshDraw, GL_CALLEE_TRIMESH_DRAW,
                             (void *self, int recurse), (self, recurse))
DEFINE_RENDER_CALLEE_WRAPPER(RenderFlat, GL_CALLEE_RENDER_FLAT,
                             (void *primitive), (primitive))
DEFINE_RENDER_CALLEE_WRAPPER(RenderLightMapped, GL_CALLEE_RENDER_LIGHTMAPPED,
                             (void *primitive), (primitive))
DEFINE_RENDER_CALLEE_WRAPPER(RenderEnvironmentMapped, GL_CALLEE_RENDER_ENVMAPPED,
                             (void *primitive, int bump, int force), (primitive, bump, force))
DEFINE_RENDER_CALLEE_WRAPPER(RenderEMLM, GL_CALLEE_RENDER_EMLM,
                             (void *primitive), (primitive))
DEFINE_RENDER_CALLEE_WRAPPER(VertexProgramEnable, GL_CALLEE_VERTEX_PROGRAM_ENABLE,
                             (void *self, void *part, int force), (self, part, force))
DEFINE_RENDER_CALLEE_WRAPPER(MaterialBindTexture0, GL_CALLEE_MATERIAL_BIND_TEXTURE0,
                             (void *self), (self))
DEFINE_RENDER_CALLEE_WRAPPER(GobProxyPartDraw, GL_CALLEE_PROXY_PART_DRAW,
                             (void *self, void *part, int recurse), (self, part, recurse))
DEFINE_RENDER_CALLEE_WRAPPER(GLRenderSetInterleavedBuffer, GL_CALLEE_SET_INTERLEAVED_BUFFER,
                             (unsigned type, int stride, unsigned long offset, int count,
                              void *mesh, unsigned long pool),
                             (type, stride, offset, count, mesh, pool))
/* GLRender::DrawElements first calls VBO-pool-manager vtable slot +0x18.
 * Objdump resolves that slot to the index-pool lookup at lib+0x50cbc0: it
 * binds the element buffer, uploads it if dirty, and returns the base offset.
 * The manager is created during renderer startup, so install this inner hook
 * lazily on the first draw rather than during loader initialization. */
static uintptr_t (*PoolLookup_orig)(void *self, unsigned long pool) = NULL;
static uintptr_t **g_pool_lookup_slot = NULL;
static int g_pool_lookup_install_attempted = 0;

static uintptr_t PoolLookup_probe(void *self, unsigned long pool) {
  gl_perf_callee_enter(GL_CALLEE_POOL_LOOKUP);
  uintptr_t rc = PoolLookup_orig(self, pool);
  gl_perf_callee_leave(GL_CALLEE_POOL_LOOKUP);
  return rc;
}

static void install_pool_lookup_attribution(void) {
  if (g_pool_lookup_install_attempted) return;
  g_pool_lookup_install_attempted = 1;
  void *manager = NULL;
  memcpy(&manager, (void *)(kotor_mod.text_base + 0x8f65b8), sizeof manager);
  if (!manager) {
    log_printf("[callee] poolLookup install deferred: manager is NULL");
    g_pool_lookup_install_attempted = 0;
    return;
  }
  uintptr_t *vtable = NULL;
  memcpy(&vtable, manager, sizeof vtable);
  if (!vtable) {
    log_printf("[callee] poolLookup install FAILED: manager=%p vtable=NULL", manager);
    return;
  }
  uintptr_t *slot = vtable + 6; /* +0x18: bind/upload index pool, return base */
  uintptr_t original = 0;
  memcpy(&original, slot, sizeof original);
  uintptr_t expected = kotor_mod.text_base + 0x50cbc1; /* Thumb callable */
  if ((original & ~(uintptr_t)1) != (expected & ~(uintptr_t)1)) {
    log_printf("[callee] poolLookup install FAILED: manager=%p vtable=%p "
               "slot=%p original=%p expected=%p",
               manager, (void *)vtable, (void *)slot, (void *)original,
               (void *)expected);
    return;
  }
  PoolLookup_orig = (void *)original;
  uintptr_t replacement = (uintptr_t)&PoolLookup_probe;
  kuKernelCpuUnrestrictedMemcpy(slot, &replacement, sizeof replacement);
  g_pool_lookup_slot = (uintptr_t **)slot;
  log_printf("[callee] poolLookup installed: manager=%p vtable=%p "
             "slot=%p original=%p", manager, (void *)vtable,
             (void *)slot, (void *)original);
}

static void (*GLRenderDrawElements_orig)(int mode, unsigned count,
                                         unsigned first, unsigned long pool) = NULL;
static void GLRenderDrawElements_probe(int mode, unsigned count,
                                       unsigned first, unsigned long pool) {
  install_pool_lookup_attribution();
  gl_perf_callee_enter(GL_CALLEE_GLRENDER_DRAW_ELEMENTS);
  GLRenderDrawElements_orig(mode, count, first, pool);
  gl_perf_callee_leave(GL_CALLEE_GLRENDER_DRAW_ELEMENTS);
}

static int (*IsPartRenderable_orig)(void *part) = NULL;
static int IsPartRenderable_probe(void *part) {
  gl_perf_callee_enter(GL_CALLEE_IS_PART_RENDERABLE);
  int rc = IsPartRenderable_orig(part);
  gl_perf_callee_leave(GL_CALLEE_IS_PART_RENDERABLE);
  return rc;
}

static int (*PartTriMeshGetRenderPath_orig)(void *self) = NULL;
static int PartTriMeshGetRenderPath_probe(void *self) {
  gl_perf_callee_enter(GL_CALLEE_GET_RENDER_PATH);
  int rc = PartTriMeshGetRenderPath_orig(self);
  gl_perf_callee_leave(GL_CALLEE_GET_RENDER_PATH);
  return rc;
}

/* Accept/reject census (performance investigation, experiment 5). The red-view
 * slowdown is ~5x more distinct gob submissions at a constant per-draw cost,
 * so the question is whether the engine is ACCEPTING too many gobs (permissive
 * VIS/room culling) or traversing more objects that all pass legitimately.
 * Counting outcomes here is free next to the timing probe that already wraps
 * the call; the ratio per window is the answer.
 *
 * Hardware answered: 100% acceptance, ~66 checks/frame (1:1 with Gob::Render)
 * vs ~300 draws -- and the disassembly shows why: gobs that fail the engine's
 * size test enter a grace/LOS path and are kept whenever the raycast confirms
 * line of sight, which down a corridor it always does. Experiment 6 therefore
 * culls at the hook instead: gobs the engine would render but whose angular
 * size proxy is below VIS_CULL_X_ENGINE_T * T are rejected here, and the
 * caller's rc==0 branch skips the whole per-gob submission. The engine's two
 * early-outs (always-visible flag, vtable predicate) are replicated first so
 * flagged gobs (characters/doors/interactables) are never hook-culled. */
static unsigned g_vis_calls = 0, g_vis_accept = 0, g_vis_reject = 0;
static unsigned g_vis_culled = 0, g_vis_cull_skip = 0;
static uintptr_t g_vis_tslot = 0;      /* GOT slot -> &T (libkotor2.so+0x866d7c) */
static uintptr_t g_vis_camslot = 0;    /* GOT slot -> camera chain (+0x8661da) */
static float g_vis_engine_t = 0.0f;    /* engine cutoff threshold, once resolved */
/* Experiment 6b: the float behind the GOT slot is the named engine global
 * `enablevisibilitytest` (relocation addend 0x904974; `countvisibilityculls`
 * lives at 0x904978). This Android build leaves it at 0, which disables the
 * engine's size->grace->LOS cull path entirely -- every gob in a VIS-linked
 * room is submitted, behind walls included. Writing a positive threshold
 * re-enables the engine's own mechanism; we only re-arm it if reset. */
static uintptr_t g_vis_tptr = 0;             /* runtime &enablevisibilitytest */
static volatile uint32_t *g_vis_cullcount = NULL;  /* countvisibilityculls */
static unsigned g_vis_rearms = 0;

/* Resolve the globals once after relocations are applied and enable the
 * engine-side test. The camera object does not exist yet at install time, so
 * the hook-cull camera chain stays validated per call (when enabled). */
static void visibility_engine_init(void) {
  g_vis_tslot = kotor_mod.text_base + 0x866d7c;
  g_vis_camslot = kotor_mod.text_base + 0x8661da;
  uintptr_t tptr = 0;
  memcpy(&tptr, (void *)g_vis_tslot, sizeof tptr);
  int in_mod = (tptr >= kotor_mod.text_base &&
                tptr < kotor_mod.text_base + kotor_mod.text_size) ||
               (tptr >= kotor_mod.data_base &&
                tptr < kotor_mod.data_base + kotor_mod.data_size);
  if (!in_mod) {
    log_printf("[visible] DISABLED: T slot %p holds %p (outside module "
               "text %p+%zx / data %p+%zx)", (void *)g_vis_tslot, (void *)tptr,
               (void *)kotor_mod.text_base, kotor_mod.text_size,
               (void *)kotor_mod.data_base, kotor_mod.data_size);
    g_vis_tslot = 0;
    return;
  }
  g_vis_tptr = tptr;
  g_vis_cullcount = (volatile uint32_t *)(kotor_mod.text_base + 0x904978);
  memcpy(&g_vis_engine_t, (void *)tptr, sizeof g_vis_engine_t);
  log_printf("[visible] enablevisibilitytest=%f at %p, countvisibilityculls at %p",
             (double)g_vis_engine_t, (void *)tptr, (void *)g_vis_cullcount);
#if VIS_ENGINE_ENABLE_TEST
  if (VIS_ENGINE_TEST_T > 0.0f && !(g_vis_engine_t > 0.0f)) {
    float t = VIS_ENGINE_TEST_T;
    memcpy((void *)tptr, &t, sizeof t);
    g_vis_engine_t = t;
    log_printf("[visible] engine visibility test ENABLED: 0 -> %f "
               "(cull when 2r/d below cutoff, after engine grace + LOS)",
               (double)t);
  }
#endif
}

/* Effects-low A/B uses only exported engine configuration globals. This is
 * safer than substituting render functions: the engine remains responsible
 * for selecting a valid path and maintaining matching GL/resource lifetime.
 * All currently selected switches are 32-bit integer globals in .data. */
static void effects_low_init(void) {
  struct EffectSwitch { const char *symbol; int disable; } switches[] = {
    {"enablerenderenv", EFFECTS_LOW_DISABLE_ENVIRONMENT_MAPPING},
    {"enableenvmap", EFFECTS_LOW_DISABLE_ENVIRONMENT_MAPPING},
    {"enableonepassenvmap", EFFECTS_LOW_DISABLE_ENVIRONMENT_MAPPING},
    {"renderemlm", EFFECTS_LOW_DISABLE_EMLM},
    {"enableemitters", EFFECTS_LOW_DISABLE_EMITTERS},
    {"enableshadows", EFFECTS_LOW_DISABLE_SHADOWS},
    {"enablefocusshadowing", EFFECTS_LOW_DISABLE_SHADOWS},
    {"enablesoftshadows", EFFECTS_LOW_DISABLE_SHADOWS},
    /* Keep basic lighting and lightmaps: disabling those can omit structural
     * geometry. Only disable optional bump/specular/bumpy-shiny paths, which
     * have ordinary material fallbacks in GetRenderPath. */
    {"enablebumpmap", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"enablerenderbumpmap", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"enablebumpyshiny", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"usebumpdiffuse", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"usebumpspecular", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"usenewbumpyskin", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"usebumpdecaltexture", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"enablerenderembmlight", EFFECTS_LOW_DISABLE_ADVANCED_LIGHTING},
    {"enablelighting", EFFECTS_LOW_DISABLE_ALL_LIGHTING},
    {"enablelightmap", EFFECTS_LOW_DISABLE_ALL_LIGHTING},
    {"enablegetnearestlights", EFFECTS_LOW_DISABLE_ALL_LIGHTING},
    {"enablegrass", EFFECTS_LOW_DISABLE_GRASS},
    {"enablegrasswind", EFFECTS_LOW_DISABLE_GRASS},
    {"rendertexturedgrass", EFFECTS_LOW_DISABLE_GRASS},
    {"renderlensflares", EFFECTS_LOW_DISABLE_LENS_FLARES},
  };
  for (unsigned i = 0; i < sizeof switches / sizeof switches[0]; i++) {
    if (!switches[i].disable) continue;
    uintptr_t address = so_symbol(&kotor_mod, switches[i].symbol);
    if (!address) {
      log_printf("[effects-low] symbol not found: %s", switches[i].symbol);
      continue;
    }
    int before = 0, zero = 0;
    memcpy(&before, (void *)address, sizeof before);
    kuKernelCpuUnrestrictedMemcpy((void *)address, &zero, sizeof zero);
    log_printf("[effects-low] %s: %d -> 0 at %p", switches[i].symbol,
               before, (void *)address);
  }
#if EFFECTS_LOW_DISABLE_POSTPROCESSING
  /* Preserve framebuffer/pbuffer allocation. This one-byte engine flag skips
   * optional distortion/noise/saturation work without clearing doframebuffer. */
  uintptr_t post = so_symbol(&kotor_mod, "disablepostprocessing");
  if (post) {
    uint8_t before = 0, one = 1;
    memcpy(&before, (void *)post, sizeof before);
    kuKernelCpuUnrestrictedMemcpy((void *)post, &one, sizeof one);
    log_printf("[effects-low] disablepostprocessing enabled at %p", (void *)post);
  } else {
    log_printf("[effects-low] symbol not found: disablepostprocessing");
  }
#endif
}

static int GobVisibilityCheck_probe(void *self) {
#if VIS_ENGINE_ENABLE_TEST
  /* Re-arm the engine threshold if anything reset it (module load, console).
   * One load + compare per call; logged at most a handful of times. */
  if (g_vis_tptr && VIS_ENGINE_TEST_T > 0.0f) {
    float cur;
    memcpy(&cur, (void *)g_vis_tptr, sizeof cur);
    if (!(cur > 0.0f)) {
      float t = VIS_ENGINE_TEST_T;
      memcpy((void *)g_vis_tptr, &t, sizeof t);
      if (g_vis_rearms < 8)
        log_printf("[visible] threshold re-armed (%f -> %f)", (double)cur, (double)t);
      g_vis_rearms++;
    }
  }
#endif
  gl_perf_callee_enter(GL_CALLEE_GOB_VISIBILITY);
  int rc = GobVisibilityCheck_orig(self);
  gl_perf_callee_leave(GL_CALLEE_GOB_VISIBILITY);
  g_vis_calls++;
#if PERF_DYNAMIC_PORTAL_GOB_FILTER
  if (rc && portal_ptr_index(g_portal_hidden_gobs,
                             g_portal_hidden_gob_count, self) >= 0) {
    int force_visible = 0;
    const uint8_t *gob = (const uint8_t *)self;
    uintptr_t obj = 0, vt = 0;
    memcpy(&obj, gob + 0x84, sizeof obj);
    if (obj && (*(const uint8_t *)(obj + 0x50) & 0x40))
      force_visible = 1;
    memcpy(&vt, gob, sizeof vt);
    if (!force_visible && vt) {
      int (*vpred)(void *) = NULL;
      memcpy(&vpred, (void *)(vt + 0x1dc), sizeof vpred);
      if (vpred && vpred(self)) force_visible = 1;
    }
    if (!force_visible) {
      g_vis_culled++;
      return 0;
    }
  }
#endif
#if VIS_CULL_ENABLE
  if (rc && g_vis_engine_t > 0.0f) {
    const uint8_t *gob = (const uint8_t *)self;
    do {
      /* Engine early-out 1: always-visible flag on the object at gob+0x84. */
      uintptr_t obj = 0;
      memcpy(&obj, gob + 0x84, sizeof obj);
      if (obj) {
        uint8_t flags = *(const uint8_t *)(obj + 0x50);
        if (flags & 0x40) break;                    /* flagged: never cull */
      }
      /* Engine early-out 2: the virtual predicate at vtable+0x1dc. int-in-r0
       * across the softfp boundary is ABI-safe; a nonzero result forces
       * visible exactly as it does inside the engine. */
      uintptr_t vt = 0;
      memcpy(&vt, gob, sizeof vt);
      if (vt) {
        int (*vpred)(void *) = NULL;
        memcpy(&vpred, (void *)(vt + 0x1dc), sizeof vpred);
        if (vpred && vpred(self)) break;
      }
      /* Camera chain: camslot -> P -> cam; position at cam+0xa8..0xb0. */
      uintptr_t p = 0, cam = 0;
      memcpy(&p, (void *)g_vis_camslot, sizeof p);
      if (!p) { g_vis_cull_skip++; break; }
      memcpy(&cam, (void *)p, sizeof cam);
      if (!cam) { g_vis_cull_skip++; break; }
      float cx, cy, cz;
      memcpy(&cx, (void *)(cam + 0xa8), sizeof cx);
      memcpy(&cy, (void *)(cam + 0xac), sizeof cy);
      memcpy(&cz, (void *)(cam + 0xb0), sizeof cz);
      if (!(cx > -1e6f && cx < 1e6f && cy > -1e6f && cy < 1e6f &&
            cz > -1e6f && cz < 1e6f)) { g_vis_cull_skip++; break; }
      float px, py, pz, rad;
      memcpy(&px, gob + 0xa4, sizeof px);
      memcpy(&py, gob + 0xa8, sizeof py);
      memcpy(&pz, gob + 0xac, sizeof pz);
      memcpy(&rad, gob + 0x164, sizeof rad);
      if (!(rad > 0.0f)) break;                     /* degenerate: leave it */
      float dx = cx - px, dy = cy - py, dz = cz - pz;
      float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < 1e-6f) break;                        /* at the camera: keep */
      /* (2r/d < K*T)  <=>  (2r)^2 < (K*T)^2 * d^2 -- no sqrt needed. */
      float kt = VIS_CULL_X_ENGINE_T * g_vis_engine_t;
      float lhs = 4.0f * rad * rad;
      if (lhs < kt * kt * d2) {
        g_vis_culled++;
        return 0;                                   /* caller skips this gob */
      }
    } while (0);
  }
#endif
  if (rc) g_vis_accept++; else g_vis_reject++;
  return rc;
}

/* Watchdog-driven so the window length is fixed wall time, not frame count:
 * a window that thins out when the game slows is the log172 mistake. */
static void visibility_census(void) {
  static unsigned p_calls = 0, p_accept = 0, p_reject = 0, p_culled = 0,
                  p_skip = 0, p_engine = 0;
  unsigned c = g_vis_calls - p_calls;
  unsigned a = g_vis_accept - p_accept;
  unsigned r = g_vis_reject - p_reject;
  unsigned cu = g_vis_culled - p_culled;
  unsigned sk = g_vis_cull_skip - p_skip;
  unsigned ec = 0;
  float t = 0.0f;
  if (g_vis_cullcount) {
    uint32_t now = *g_vis_cullcount;
    /* The engine resets this counter during area/state transitions. Treat a
     * decrease as a reset instead of reporting an unsigned-wrap delta. */
    ec = now >= p_engine ? now - p_engine : now;
    p_engine = now;
  }
  if (g_vis_tptr) memcpy(&t, (void *)g_vis_tptr, sizeof t);
  if (c)
    log_printf("[visible] window: %u checks, %u accepted (%u%%), %u rejected, "
               "%u hook-culled, %u cull-skipped | engine culls=%u total=%u T=%f "
               "rearms=%u",
               c, a, c ? a * 100 / c : 0, r, cu, sk,
               ec, g_vis_cullcount ? *g_vis_cullcount : 0, (double)t,
               g_vis_rearms);
  else
    log_printf("[visible] window: no VisibilityCheck calls (hook not firing?)");
  p_calls = g_vis_calls; p_accept = g_vis_accept; p_reject = g_vis_reject;
  p_culled = g_vis_culled; p_skip = g_vis_cull_skip;
}

/* Frame-loop attribution: the stage/callee hooks leave ~18 ms (clear view) to
 * ~25 ms (red view) per frame outside every wrapped function. These two are
 * the engine's own top-level per-frame entry points; their exclusive time
 * splits that remainder between the message pump (input/SDL/FMOD update) and
 * everything else MainLoop does that is not render-callee instrumented
 * (engine update, AI/scripts, GUI layout). If MainLoop shows one call for a
 * whole window it is session-scoped and only messagepump is informative. */
static int (*ClientMainLoop_orig)(void *self) = NULL;
static int ClientMainLoop_probe(void *self) {
  gl_perf_callee_enter(GL_CALLEE_MAINLOOP);
  int rc = ClientMainLoop_orig(self);
  gl_perf_callee_leave(GL_CALLEE_MAINLOOP);
  return rc;
}
static int (*messagepump_orig)(void) = NULL;
static int messagepump_probe(void) {
  gl_perf_callee_enter(GL_CALLEE_MSGPUMP);
  int rc = messagepump_orig();
  gl_perf_callee_leave(GL_CALLEE_MSGPUMP);
  return rc;
}

static void install_render_callee_attribution(void) {
  /* Verified leaf getter: returns gob->[0x84] + 8 or NULL. Resolve by symbol and
   * call it passively so high-draw pointer identities can be mapped to models. */
  GobGetModelName = (void *)so_symbol(&kotor_mod, "_ZN3Gob12GetModelNameEv");
  log_printf("[gobid] GetModelName=%p", (void *)GobGetModelName);

  SceneStageHook hooks[] = {
    {"_ZN3Gob15VisibilityCheckEv", (uintptr_t)&GobVisibilityCheck_probe,
     (void **)&GobVisibilityCheck_orig, "visible"},
    {"_ZN3Gob6RenderEb", (uintptr_t)&GobRender_probe,
     (void **)&GobRender_orig, "gobRender"},
    {"_ZN3Gob8PartDrawEP4Partb", (uintptr_t)&GobPartDraw_probe,
     (void **)&GobPartDraw_orig, "partDraw"},
    {"_ZN11PartTriMesh4DrawEb", (uintptr_t)&PartTriMeshDraw_probe,
     (void **)&PartTriMeshDraw_orig, "triDraw"},
    {"_Z10RenderFlatP19VertexPrimitiveFlat", (uintptr_t)&RenderFlat_probe,
     (void **)&RenderFlat_orig, "flat"},
    {"_Z17RenderLightMappedP19VertexPrimitiveFlat", (uintptr_t)&RenderLightMapped_probe,
     (void **)&RenderLightMapped_orig, "lightmap"},
    {"_Z23RenderEnvironmentMappedP19VertexPrimitiveFlatbb",
     (uintptr_t)&RenderEnvironmentMapped_probe,
     (void **)&RenderEnvironmentMapped_orig, "envmap"},
    {"_Z10RenderEMLMP19VertexPrimitiveFlat", (uintptr_t)&RenderEMLM_probe,
     (void **)&RenderEMLM_orig, "emlm"},
    {"_ZN13VertexProgram6EnableEP4Partb", (uintptr_t)&VertexProgramEnable_probe,
     (void **)&VertexProgramEnable_orig, "vpEnable"},
    {"_ZN8Material12BindTexture0Ev", (uintptr_t)&MaterialBindTexture0_probe,
     (void **)&MaterialBindTexture0_orig, "bindTex0"},
    {"_ZN3Gob13ProxyPartDrawEP4Partb", (uintptr_t)&GobProxyPartDraw_probe,
     (void **)&GobProxyPartDraw_orig, "proxyPart"},
    {"_Z16IsPartRenderableP11PartTriMesh", (uintptr_t)&IsPartRenderable_probe,
     (void **)&IsPartRenderable_orig, "isRenderable"},
    {"_ZN11PartTriMesh13GetRenderPathEv", (uintptr_t)&PartTriMeshGetRenderPath_probe,
     (void **)&PartTriMeshGetRenderPath_orig, "getPath"},
    {"_ZN8GLRender20SetInterleavedBufferEjimiP14MdlNodeTriMeshm",
     (uintptr_t)&GLRenderSetInterleavedBuffer_probe,
     (void **)&GLRenderSetInterleavedBuffer_orig, "setInterleaved"},
    {"_ZN8GLRender12DrawElementsEN6Aurora20AuroraPrimitiveTypesEjjm",
     (uintptr_t)&GLRenderDrawElements_probe,
     (void **)&GLRenderDrawElements_orig, "engineDraw"},
    {"_ZN21CClientExoAppInternal8MainLoopEv", (uintptr_t)&ClientMainLoop_probe,
     (void **)&ClientMainLoop_orig, "mainLoop"},
    {"_Z11messagepumpv", (uintptr_t)&messagepump_probe,
     (void **)&messagepump_orig, "msgpump"},
  };

  for (unsigned h = 0; h < sizeof(hooks) / sizeof(hooks[0]); h++) {
    uintptr_t original = so_symbol(&kotor_mod, hooks[h].symbol);
    uintptr_t callable = 0;
    unsigned replaced = 0, mismatch = 0;
    for (int i = 0; i < kotor_mod.num_reldyn + kotor_mod.num_relplt; i++) {
      Elf32_Rel *rel = i < kotor_mod.num_reldyn ? &kotor_mod.reldyn[i] :
          &kotor_mod.relplt[i - kotor_mod.num_reldyn];
      unsigned type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT &&
          type != R_ARM_JUMP_SLOT) continue;
      Elf32_Sym *sym = &kotor_mod.dynsym[ELF32_R_SYM(rel->r_info)];
      if (strcmp(kotor_mod.dynstr + sym->st_name, hooks[h].symbol) != 0) continue;
      uintptr_t *slot = (uintptr_t *)(kotor_mod.text_base + rel->r_offset);
      if (!original || ((*slot ^ original) & ~(uintptr_t)1)) {
        mismatch++;
        continue;
      }
      if (!callable) callable = *slot;
      kuKernelCpuUnrestrictedMemcpy(slot, &hooks[h].replacement,
                                    sizeof hooks[h].replacement);
      replaced++;
    }
    *hooks[h].original = (void *)callable;
    log_printf("[callee] install %s replaced=%u mismatch=%u original=%p",
               hooks[h].tag, replaced, mismatch, (void *)callable);
  }
}

// The barrier in MainLoop counts outstanding async resource requests, so watch the
// two ends of that queue directly: PreSpawnAsync issues a request, RetreiveAsync
// collects a finished one. If issues >> retrieves, the worker never drains it; if
// they balance, the stall is elsewhere and the counter belongs to something else.
static void *(*PreSpawnAsync_orig)(void *self, char *name) = NULL;
static void *(*RetreiveAsync_orig)(void *self, void *req) = NULL;
static unsigned g_pre_n = 0, g_ret_n = 0;

static void *PreSpawnAsync_probe(void *self, char *name) {
  void *rc = PreSpawnAsync_orig(self, name);
  if (g_pre_n < 64)
    log_printf("[async] PreSpawnAsync(\"%.24s\") -> %p  [issued=%u retrieved=%u]",
               name ? name : "?", rc, g_pre_n + 1, g_ret_n);
  g_pre_n++;
  return rc;
}

static void *RetreiveAsync_probe(void *self, void *req) {
  void *rc = RetreiveAsync_orig(self, req);
  if (g_ret_n < 64)
    log_printf("[async] RetreiveAsync(%p) -> %p  [issued=%u retrieved=%u]",
               req, rc, g_pre_n, g_ret_n + 1);
  g_ret_n++;
  return rc;
}

// ---- load-stall probe -------------------------------------------------------
// Decoded from CServerExoAppInternal::MainLoop (libKOTOR +0x3eb94c). Once per
// frame it does, in effect:
//
//   ls = ((void **)g_pAppManager)[5];        // appManager + 0x14, the load state
//   if (ls[14] != 0)          -> bail; an error code is already latched
//   mode = ls[1];             -> only 1, 2 or 3 do anything at all
//   if      (ls[3] == 0)      -> nothing queued
//   else if (ls[3] != ls[2])  -> CSWSModule::LoadModuleInProgress(ls[2], ls[3])
//   else if (ls[5] != 1)      -> CSWSModule::LoadModuleFinish()
//
// LoadModuleInProgress (+0x3884b8) loads exactly ONE area per call via
// CSWSArea::LoadArea and then stores ls[2]+1 back to ls[2]. So ls[2] is progress,
// ls[3] is the target, and Finish only fires when they meet. A frozen bar means
// ls[2] stopped climbing, which is either "MainLoop never reaches the call" or
// "LoadArea stopped succeeding" -- opposite causes. Log the gate itself plus both
// calls so the next run distinguishes them instead of us guessing again.
//
// LoadArea returning 0 is FAILURE (LoadModuleInProgress then tears the area down
// and returns 4, which makes MainLoop call UnloadModule) -- so an all-zero return
// here would show up as an abort, not a hang.
static void *g_appmgr_ptr = NULL;

static void *(*MainLoop_orig)(void *self) = NULL;
static void *(*LoadInProgress_orig)(void *self, int prog, int target) = NULL;
static void *(*LoadArea_orig)(void *self, int a) = NULL;

static void load_state_dump(const char *tag) {
  if (!g_appmgr_ptr) return;
  void *app = *(void **)g_appmgr_ptr;
  if (!app) { log_printf("[load] %s: g_pAppManager is NULL", tag); return; }
  void **ls = (void **)((void **)app)[5];
  if (!ls) { log_printf("[load] %s: appManager[+0x14] is NULL", tag); return; }
  log_printf("[load] %s: mode=%d progress=%d target=%d f20=%d err=%d",
             tag, (int)(intptr_t)ls[1], (int)(intptr_t)ls[2], (int)(intptr_t)ls[3],
             (int)(intptr_t)ls[5], (int)(intptr_t)ls[14]);
}

static unsigned g_ml_n = 0, g_lip_n = 0, g_la_n = 0;

// log81 verdict: 3360 frames were drawn after LoadModuleStart while MainLoop ran
// fewer than 301 times and LoadModuleInProgress/LoadArea ran ZERO times. So the
// server is not being pumped at all. Tracing the two things that can pump it:
//
//   GameUpdate()  (SDL_main +0x18d116/+0x18d1a0) -> CServerExoApp::MainLoop,
//                 gated only on appManager[+8] != NULL
//   UpdateScreen(float,int,int) (+0x3fe098)      -> same, but gated on b == 1
//
// and EVERY one of the 56 UpdateScreen call sites in the binary passes b=0, so
// that branch is dead code: GameUpdate is the only pump. Yet UpdateScreen is the
// only thing calling SDL_GL_SwapWindow during a load, so something is spinning it
// in a nested loop that never returns to SDL_main. Its return address names that
// loop, which is the one fact still missing.
//
// ABI: UpdateScreen's first parameter is a float and the .so is softfp, so it
// arrives in r0, not s0. Declaring it `float` would make our hardfp build read s0
// and shift b/c by one register.
static void *(*UpdateScreen_orig)(uint32_t a, int b, int c) = NULL;
static void *(*GameUpdate_orig)(void) = NULL;
static unsigned g_us_n = 0, g_gu_n = 0;
static uint64_t g_us_time = 0, g_gu_time = 0;
static uint64_t g_us_active = 0, g_gu_active = 0;
static volatile float *g_ai_update_time = NULL, *g_display_fps = NULL;
static volatile int *g_movie_fps = NULL, *g_render_skip = NULL;
static unsigned g_policy_seq = 0, g_selected_skip = 0;
static float g_selector_ai_ms = 0.0f, g_last_ai_ms = 0.0f;
static int g_new_present_group = 1;

void engine_perf_snapshot(engine_perf_t *out, uint64_t now_us) {
  if (!out) return;
  out->game_calls = g_gu_n;
  out->game_us = g_gu_time + (g_gu_active ? now_us - g_gu_active : 0);
  out->screen_calls = g_us_n;
  out->screen_us = g_us_time + (g_us_active ? now_us - g_us_active : 0);
  out->policy_seq = g_policy_seq;
  out->selected_skip = g_selected_skip;
  out->selector_ai_ms = g_selector_ai_ms;
  out->next_ai_ms = g_last_ai_ms;
  out->display_fps = g_display_fps ? *g_display_fps : -1.0f;
  out->movie_fps = g_movie_fps ? *g_movie_fps : -1;
}

void engine_perf_presented(void) {
  g_new_present_group = 1;
}

static void *UpdateScreen_probe(uint32_t a, int b, int c) {
  if (g_us_n < 16 || (g_us_n % 200) == 0) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[load] UpdateScreen #%u (a=0x%08x b=%d c=%d) from off=0x%06x "
               "thid=0x%08x  [GameUpdate=%u MainLoop=%u]",
               g_us_n, (unsigned)a, b, c,
               (unsigned)(lr - kotor_mod.text_base),
               (unsigned)sceKernelGetThreadId(), g_gu_n, g_ml_n);
  }
  g_us_n++;
  uint64_t start = sceKernelGetProcessTimeWide();
  g_us_active = start;
  void *rc = UpdateScreen_orig(a, b, c);
  g_us_time += sceKernelGetProcessTimeWide() - start;
  g_us_active = 0;
  return rc;
}

static void *GameUpdate_probe(void) {
  if (g_new_present_group) {
    g_selector_ai_ms = g_last_ai_ms;
    g_selected_skip = g_render_skip ? (unsigned)*g_render_skip : 0;
    g_policy_seq++;
    g_new_present_group = 0;
  }
#if DISABLE_ADAPTIVE_RENDER_SKIP
  // SDL_main has already chosen the skip count and is about to run the primary
  // update. Clearing it here makes that update render and lets SDL_main present
  // it, instead of following it with up to ten no-present update iterations.
  if (g_render_skip && *g_render_skip > 0) *g_render_skip = 0;
#endif
  if ((g_gu_n % 200) == 0) {
    void *app = g_appmgr_ptr ? *(void **)g_appmgr_ptr : NULL;
    log_printf("[load] GameUpdate #%u appMgr=%p client=%p server=%p "
               "[UpdateScreen=%u MainLoop=%u]",
               g_gu_n, app,
               app ? ((void **)app)[1] : NULL,
               app ? ((void **)app)[2] : NULL,
               g_us_n, g_ml_n);
  }
  g_gu_n++;
  uint64_t start = sceKernelGetProcessTimeWide();
  g_gu_active = start;
  void *rc = GameUpdate_orig();
  g_gu_time += sceKernelGetProcessTimeWide() - start;
  g_gu_active = 0;
  if (g_ai_update_time) g_last_ai_ms = *g_ai_update_time;
  return rc;
}

static void *MainLoop_probe(void *self) {
  // Dump every one of the first 16 -- log82 showed MainLoop runs exactly TWICE
  // and then never again, so the every-300 throttle hid the interesting call.
  if (g_ml_n < 16 || (g_ml_n % 300) == 0) {
    char t[48];
    snprintf(t, sizeof t, "MainLoop#%u thid=0x%08x", g_ml_n,
             (unsigned)sceKernelGetThreadId());
    load_state_dump(t);
  }
  g_ml_n++;
  return MainLoop_orig(self);
}

// log82: appManager[+8] (the CServerExoApp) is NULL on every GameUpdate, forever,
// so GameUpdate's `cbz r0` skips the server pump and the load can never advance.
// MainLoop ran exactly twice -- around LoadModuleStart -- so the server DID exist
// briefly and was then torn down. CAppManager::DestroyServer has five call sites:
//   +0x1972b2 CClientExoAppInternal::MainLoop        (-> DisplayMainMenu)
//   +0x19db80 CClientExoAppInternal::ShutDownToMainMenu
//   +0x2beaa4 CSWGuiSaveLoad::LoadGame
//   +0x3ec156 CServerExoAppInternal::MainLoop        (server shutdown path)
//   +0x3fda50 GameDeinit
// Logging the return address says which one fired instead of us reverse
// engineering all five. CreateServer is logged too, to bracket the lifetime.
static void *(*CreateServer_orig)(void *self, int a) = NULL;
static void *(*DestroyServer_orig)(void *self) = NULL;

// log86: modules/END_M01AA.rim, currentgame/ and _s.rim all open cleanly now (the
// OBB fallback works -- CHITIN.key and every .bzf are served from it), yet
// LoadModuleStart still exits rc=1. So CRes::Demand() on the module .ifo still
// returns NULL even though the RIM is readable. The .ifo does NOT come through
// plain file I/O -- it is registered with the resource manager by
// CSWSModule::AddModuleResources -> CExoResMan::AddEncapsulatedResourceFile, and
// only then demanded. Tellingly there was NO file I/O at all inside
// LoadModuleStart's 17 ms window, so the RIM may never be opened.
//
// Watch that whole chain:
//   CSWSModule::AddModuleResources(name)  - does it run, for which module
//   CExoEncapsulatedFile::OpenFile()      - is the RIM actually opened
//   CExoResMan::Demand(CRes*)             - log only NULL returns (the failure)
// Hook CExoResMan::Demand, NOT CRes::Demand: the latter is a 6-instruction thunk
// whose 2nd and 3rd instructions are `ldr r0,[pc,#12]` / `add r0,pc`, and the
// trampoline copies bytes without relocating them, so those would read from the
// wrong address. CExoResMan::Demand is the real implementation it tail-calls and
// its first 8 bytes are position-independent.
// CExoString stores its char* at offset 0 (CExoString::CStr is `ldr r0,[r0]`
// with an empty-string fallback), so *(char**)s is the text.
//
// NB: CExoResMan::AddEncapsulatedResourceFile would be the more direct probe but
// is UNHOOKABLE here -- it is an 8-byte thunk whose body is a PC-relative `b.w`
// into a long-branch veneer. build_thumb_trampoline copies bytes verbatim without
// relocating them, so the copied branch would go somewhere else entirely, and the
// 10-byte patch its 2-mod-4 address demands would resume inside AddKeyTable.
// Always check the first N bytes of a hook target for PC-relative instructions.
static void *(*AddModRes_orig)(void *self, const void *name) = NULL;
static void *(*OpenFile_orig)(void *self) = NULL;
static void *(*Demand_orig)(void *self, void *res) = NULL;
static unsigned g_openfile_n = 0, g_demand_null_n = 0;


static void *AddModRes_probe(void *self, const void *name) {
  log_printf("[res] AddModuleResources(\"%.64s\") ENTER", exostr(name));
  void *rc = AddModRes_orig(self, name);
  log_printf("[res] AddModuleResources(\"%.64s\") EXIT rc=%p  [OpenFile calls=%u]",
             exostr(name), rc, g_openfile_n);
  return rc;
}

static void *OpenFile_probe(void *self) {
  void *rc = OpenFile_orig(self);
  if (g_openfile_n < 48 || !rc)
    log_printf("[res] CExoEncapsulatedFile::OpenFile(self=%p) -> %p  [#%u]",
               self, rc, g_openfile_n + 1);
  g_openfile_n++;
  return rc;
}

// log87: Demand()->NULL is ROUTINE (every optional .txi probe misses), so a flat
// cap of 24 was exhausted at t=105s and the one that matters -- inside
// LoadModuleStart at t=337 -- was never logged. Scope it to the load window
// instead: g_in_lms is armed only while LoadModuleStart runs, where NULL is the
// actual failure. Dump 0x40 bytes so the CRes's ResRef is visible in the ASCII
// column (it is past +0x18, which the earlier 0x20 dump could not reach).


static void *Demand_probe(void *self, void *res) {
  void *rc = Demand_orig(self, res);
  if (!rc && res && (g_in_lms || g_demand_null_n < 4)) {
    dump_res(g_in_lms ? "IN-LoadModuleStart Demand -> NULL" : "Demand -> NULL", res);
    g_demand_null_n++;
  }
  return rc;
}

// log89: the copy theory is dead -- modules/END_M01AA.rim opens at 55565 bytes,
// byte-exact with the OBB's own entry, so the RIM is intact. What the log DOES
// show is that currentgame/END_M01AA.rim is opened `wb` and never once read: no
// `rb` open of the copy appears anywhere in the log. The main module archive is
// therefore never indexed.
//
// Why that is fatal is now settled statically. CResHelper<CResIFO,2014>::SetResRef
// (+0x3856b0) first compares the incoming ResRef against the one it already stores
// at this+0x0c and RETURNS IMMEDIATELY if they match -- so "module" is bound
// exactly once, ever. On that one call it asks CExoResMan::GetResObject(ref, 2014);
// when that misses it `new`s a 168-byte CResGFF, stamps it with the CResIFO vtable
// (+0x59f388 -- exactly the vtable in our dump, so the object we log IS this
// placeholder) and registers it via SetResObject. The placeholder's id (+8) stays
// 0xFFFFFFFF, and CExoResMan::Demand (+0x4c86cc: `ldr r1,[r1,#8]; adds r0,r1,#1;
// beq ret0`) rejects it on sight. A later archive add cannot rescue it unless it
// walks the live CRes objects (UpdateKeyTable).
//
// So the question collapses to one: is the main module RIM ever handed to the
// resource manager at all? AddEncapsulatedResourceFile is the unhookable 8-byte
// thunk (`mov r3,r2; movs r2,#3; b.w <veneer>`) -- but it is only a shim for
// CExoResMan::AddKeyTable(name, type=3, m), which IS a real 396-byte body at
// +0x4c9490: 0-mod-4, prologue push/add r7/stmdb = 8 bytes, no PC-relative ops.
// Hook that and every archive registration in the game names itself.
static unsigned long (*AddKeyTable_orig)(void *, const void *, unsigned long,
                                         unsigned long) = NULL;
static unsigned g_akt_n = 0;

static unsigned long AddKeyTable_probe(void *self, const void *name,
                                       unsigned long type, unsigned long m) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  unsigned long rc = AddKeyTable_orig(self, name, type, m);
  g_akt_n++;
  log_printf("[res] AddKeyTable(\"%.64s\" type=%lu m=0x%lx) -> rc=0x%lx  "
             "from off=0x%06x  [#%u]", exostr(name), type, m, rc,
             (unsigned)(lr - kotor_mod.text_base), g_akt_n);
  return rc;
}

// log91: the stat() fix was correct in itself but changed nothing -- and the log
// shows **zero** `[FS] stat` lines, so `CExoBaseInternal::GetDirectoryList` never
// reaches its two stat call sites (+0x4b675e / +0x4b676c) at all. It bails
// earlier, or is never called for CURRENTGAME:. Probe the enumeration chain
// end-to-end rather than guessing which of the 3248 bytes bails:
//
//   CExoKeyTable::BuildNewTable        +0x4c5eac  type 1..4 -> tail-calls one of
//     `-> AddDirectoryContents(int)    +0x4c4adc  (type 2, our case)
//          `-> CExoBase::GetDirectoryList        the actual enumerator
//   CExoResMan::GetKeyEntry            +0x4ca62c  the lookup that then misses
//
// All four are 0-mod-4 with an 8-byte position-independent prologue
// (push/add r7/stmdb), so all pass the 3-way trampoline check.
//
// CExoArrayList<CExoString> keeps its count at +4 (GetDirectoryList itself reads
// `[r9,#4]` and compares against 1 at +0x4b678c). CResRef stores its chars at
// offset 0 -- AsyncLoad memcpys the lowercased name straight into the struct
// before passing it -- so a CResRef can be printed as a bounded char array.
// log92: GetKeyEntry answered the big question -- `global`, `mainmenu` and
// `chargen` (all in rims/) resolve as type 3002 (.rim), while `end_m01aa` (in
// modules/) does not, at either 3002 or 3009. So one directory key table
// populates and the other does not, even though BOTH directories are in the OBB
// (rims/ 13 entries, modules/ 235). AddDirectoryContents("MODULES:") itself runs
// and returns 1, so the failure is inside the enumeration.
//
// NB the earlier GetDirectoryList probe printed garbage: this is a NON-STATIC
// member, so `this` occupies r0 and every argument is shifted one register right
// (r1=out, r2=dir, r3=type). The old signature read the array-list pointer as the
// dir string and `this` as the array list -- hence "(empty)" names and a nonsense
// count. Corrected below.
//
// CExoArrayList keeps its count at +4 (GetDirectoryList reads `[r9,#4]` and
// compares against 1 at +0x4b678c).
static void *(*GetDirList_orig)(void *, void *, const void *, unsigned,
                                int, int, int) = NULL;
static void *(*AddDirContents_orig)(void *, int) = NULL;
static void *(*GetKeyEntry_orig)(void *, const void *, unsigned, void *, void *) = NULL;
static void *(*AddKey_orig)(void *, const void *, unsigned, unsigned, int) = NULL;
static unsigned g_gdl_n = 0, g_adc_n = 0, g_gke_n = 0, g_addkey_n = 0;

static void *GetDirList_probe(void *self, void *out, const void *dir,
                              unsigned type, int a, int b, int c) {
  void *rc = GetDirList_orig(self, out, dir, type, a, b, c);
  int n = out ? ((int *)out)[1] : -1;
  if (g_gdl_n < 64)
    log_printf("[res] GetDirectoryList(\"%.64s\" type=%u %d,%d,%d) -> rc=%p  "
               "count=%d  [#%u]", exostr(dir), type, a, b, c, rc, n, g_gdl_n + 1);
  // log93 pinned the failure to CURRENTGAME: -- count=1 yet not one
  // AddKey(tbl="CURRENTGAME:") in the entire log. Print the names for any short
  // list so we can see WHICH name the loop rejects. Elements are CExoString,
  // stride 8 (AddDirectoryContents walks with `adds r6,#8`), char* at offset 0;
  // the array base is the list's first word.
  // Cap 16, not 8, so RIMS: (12 entries, and known-good -- its keys DO land)
  // dumps too and gives a positive control for what a name is supposed to look
  // like next to the one currentgame entry that gets rejected.
  if (n > 0 && n <= 16) {
    const char *base = (const char *)((void **)out)[0];
    if (base)
      for (int i = 0; i < n; i++)
        log_printf("[res]    entry[%d] = \"%.64s\"", i, exostr(base + i * 8));
  }
  g_gdl_n++;
  return rc;
}

// How many keys each table actually ends up with -- the direct measure of "did
// this directory enumerate". CResRef stores its chars at offset 0.
static void *AddKey_probe(void *self, const void *resref, unsigned type,
                          unsigned id, int a) {
  void *rc = AddKey_orig(self, resref, type, id, a);
  // CURRENTGAME: is the table under test, so never throttle it; everything else
  // is background and gets a cap (log93 emitted 1200 AddKey lines and the run
  // crawled).
  const char *tbl = exostr((const char *)self + 0x20);
  int watched = tbl && strstr(tbl, "CURRENTGAME");
  if (watched || g_addkey_n < 24 ||
      ((type == 3002 || type == 3009) && g_addkey_n < 300)) {
    char n[17];
    memcpy(n, resref, 16);
    n[16] = 0;
    log_printf("[res] AddKey(tbl=\"%.32s\", \"%s\" type=%u id=%u) -> %p  [#%u]",
               exostr((const char *)self + 0x20), n, type, id, rc,
               g_addkey_n + 1);
  }
  g_addkey_n++;
  return rc;
}

static void *AddDirContents_probe(void *self, int a) {
  void *rc = AddDirContents_orig(self, a);
  if (g_adc_n < 64)
    log_printf("[res] AddDirectoryContents(\"%.64s\", %d) -> rc=%p  [#%u]",
               exostr((const char *)self + 0x20), a, rc, g_adc_n + 1);
  g_adc_n++;
  return rc;
}

static void *GetKeyEntry_probe(void *self, const void *resref, unsigned type,
                               void *tbl, void *ent) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  void *rc = GetKeyEntry_orig(self, resref, type, tbl, ent);
  // 3002 = .rim, 3009 = .rsv -- the two AsyncLoad gates. Rare, so log them all.
  if ((type == 3002 || type == 3009) && g_gke_n < 128) {
    char n[17];
    memcpy(n, resref, 16);
    n[16] = 0;
    log_printf("[res] GetKeyEntry(\"%s\" type=%u) -> %p  from off=0x%06x  [#%u]",
               n, type, rc, (unsigned)(lr - kotor_mod.text_base), g_gke_n + 1);
    g_gke_n++;
  }
  return rc;
}

static void *CreateServer_probe(void *self, int a) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  void *rc = CreateServer_orig(self, a);
  void *app = g_appmgr_ptr ? *(void **)g_appmgr_ptr : NULL;
  log_printf("[load] CAppManager::CreateServer(%d) -> %p  from off=0x%06x  "
             "server now=%p", a, rc, (unsigned)(lr - kotor_mod.text_base),
             app ? ((void **)app)[2] : NULL);
  return rc;
}

static void *DestroyServer_probe(void *self) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  void *app = g_appmgr_ptr ? *(void **)g_appmgr_ptr : NULL;
  log_printf("[load] CAppManager::DestroyServer() from off=0x%06x  server was=%p "
             "[GameUpdate=%u MainLoop=%u UpdateScreen=%u]",
             (unsigned)(lr - kotor_mod.text_base),
             app ? ((void **)app)[2] : NULL, g_gu_n, g_ml_n, g_us_n);
  load_state_dump("at DestroyServer");
  return DestroyServer_orig(self);
}

static void *LoadInProgress_probe(void *self, int prog, int target) {
  void *rc = LoadInProgress_orig(self, prog, target);
  if (g_lip_n < 64 || rc)
    log_printf("[load] LoadModuleInProgress(progress=%d, target=%d) -> rc=%p  [#%u]",
               prog, target, rc, g_lip_n + 1);
  g_lip_n++;
  return rc;
}

static void *LoadArea_probe(void *self, int a) {
  void *rc = LoadArea_orig(self, a);
  if (g_la_n < 64 || !rc)
    log_printf("[load] CSWSArea::LoadArea(%d) -> rc=%p  [#%u]", a, rc, g_la_n + 1);
  g_la_n++;
  return rc;
}

/* --- sound: where does the chain stop? --------------------------------------
 *
 * Audio has never made a sound on hardware. Across log101/log102 the only [snd]
 * lines were "decoder ready" and "output up" -- FMOD::System::createSound was
 * never called even once, and no individual sound file was ever opened. So the
 * backend was never the problem; something upstream never asks for a sound.
 *
 * What the disassembly already settles:
 *   - FModAudioSystem::InitSystem DID run (it is what called audio_start).
 *   - FModAudioSystem::CreateSound (+0x73220, port) has NO guard on the system
 *     handle -- it walks its cache map then goes straight to createSound. So it
 *     was never called; the gate is in libKOTOR, above the companion.
 *   - Sound reaches the companion by exactly two routes:
 *       CExoSoundSourceInternal::Demand()          -> CreateSound   (SFX; bails
 *           early if this->m_pRes (+8) is NULL or CRes::Demand() returns 0)
 *       CExoStreamingSoundSourceInternal::InitializeSource() -> CreateStream
 *           (music/VO, via an SDL_RWops -- our RWFromFile chain)
 *   - CExoSound(unsigned char, unsigned char, int, int) is built at the end of
 *     CClientExoAppInternal::InitializeSoundOptions (libKOTOR +0x19bc38). Its
 *     args are NOT what an earlier pass here guessed. Reading +0x19beac:
 *         uxtb r1,r8   <- [Sound Options] "Number 2D Voices"  (default 24)
 *         uxtb r2,r6   <- [Sound Options] "Number 3D Voices"  (default 16)
 *         clz r0,r9 ; lsrs r0,r0,#5 ; str r0,[sp]   <- 4th arg = (r9 == 0)
 *     So arg1/arg2 are VOICE COUNTS with sane nonzero defaults, and the 4th arg
 *     is the real sound-enabled boolean.
 *   - r9 comes straight from [Sound Options] "Sound Init":
 *         read "Sound Init" -> r4   (ReadIniEntry fails => r4 = 0)
 *         immediately WRITE "Sound Init" = 1
 *         r4 != 0  -> r9 = 1 -> 4th arg 0 -> SOUND DISABLED
 *         r4 == 0  -> r9 = 0 -> 4th arg 1 -> sound enabled
 *         ...and at the very end of the function, WRITE "Sound Init" = 0.
 *     That is a crash-guard: the game marks "I am about to init sound", and if
 *     it finds that mark still set on the next boot it assumes sound init killed
 *     the process last time and silently runs mute forever after.
 *   - Inherited from KOTOR I: the INI had a writable destination and the
 *     sound-init guard was observed as clear.
 *
 * The far better candidate, found by scanning every reference in .text: the
 * global g_bDisableSound (libKOTOR .bss +0xb3e030, GOT slot 0x5a38e8). It has
 * exactly 12 referents and exactly ONE writer -- _Z8GameInitv at +0x3fd056:
 *       ReadIniEntry(swkotor2.ini, [Sound Options], "Disable Sound") -> r4
 *       r4 == 0 (key absent) -> leave g_bDisableSound at its .bss 0, and write
 *                               "Disable Sound=0" back to the ini
 *       r4 != 0              -> g_bDisableSound = (value.AsINT() != 0)
 * Every other referent only reads it, and each read is a hard bail:
 *       CExoSoundInternal::Initialize  +0x4db766  ==1 -> return 0, does nothing
 *       CExoSound::CExoSound           +0x4daa66  !=0 -> never builds m_pInternal
 *       CExoSoundSource::CExoSoundSource        !=0 -> m_pInternal = NULL
 *       SDL_main main loop             +0x18d10a  !=0 -> skips UpdateSystem()
 * One flag therefore suppresses the entire subsystem with no error anywhere,
 * which is exactly the observed symptom. So log the flag itself rather than
 * inferring it: g_bDisableSound is an exported OBJECT, we can just read it.
 */
static int *g_pDisableSound = NULL;
static void *(*GameInit_orig)(void) = NULL;

static void *GameInit_probe(void) {
  void *rc = GameInit_orig();
  if (g_pDisableSound)
    log_printf("[snd?] after GameInit: g_bDisableSound = %d  %s", *g_pDisableSound,
               *g_pDisableSound
                   ? "<<< SOUND IS OFF: swkotor2.ini [Sound Options] Disable Sound is nonzero"
                   : "(sound not disabled by the global)");
  return rc;
}
/*
 *
 * Hence probes rather than another guess: one run shows which link breaks. These
 * are pure log lines -- no threads, no mixer, nothing that could touch frame rate.
 * Every probe passes the callee's return value through: a void-declared hook on a
 * value-returning function was itself a crash once.
 */
static void *(*ExoSound_ctor_orig)(void *, unsigned, unsigned, int, int) = NULL;
static void *(*ExoSoundInit_orig)(void *, unsigned, unsigned, int, int) = NULL;
static void *(*SndDemand_orig)(void *) = NULL;
static void *(*StreamInit_orig)(void *) = NULL;
static void *(*FmodCreateSound_orig)(void *, char *, int, void *, unsigned, int, int) = NULL;
static void *(*FmodCreateStream_orig)(void *, char *, void *, int, int, int, int, int) = NULL;
static void *(*FmodPlaySound_orig)(void *, int) = NULL;

static void *ExoSound_ctor_probe(void *self, unsigned a, unsigned b, int c, int d) {
  log_printf("[snd?] CExoSound(n2DVoices=%u n3DVoices=%u, %d, soundEnabled=%d)  <<< "
             "soundEnabled==0 means \"Sound Init\" was left set in swkotor2.ini",
             a & 0xff, b & 0xff, c, d);
  return ExoSound_ctor_orig(self, a, b, c, d);
}
static void *ExoSoundInit_probe(void *self, unsigned a, unsigned b, int c, int d) {
  void *rc = ExoSoundInit_orig(self, a, b, c, d);
  log_printf("[snd?] CExoSoundInternal::Initialize(%u, %u, %d, %d) -> %p",
             a & 0xff, b & 0xff, c, d, rc);
  return rc;
}
// Upstream of Demand: does the game ever ASK for a sound at all? If these two
// stay silent, nothing below them can ever fire and the gate is higher than the
// sound system. CResRef is a fixed char[16], not necessarily NUL-terminated.
static void *(*SndSrcCtor_orig)(void *, const void *) = NULL;
static void *(*SndSrcPlay_orig)(void *) = NULL;

/* The sound pipeline, counted end to end.
 *
 * Every one of these probes printed its first 40 calls and then went silent --
 * which in log170-172 was around t=50 s, i.e. before anything interesting had
 * happened. So when the game stopped playing sounds at t=1030 in log172 the
 * log could say that OUR playSound had gone to zero, and nothing whatever about
 * why: the four layers above it had stopped reporting sixteen minutes earlier.
 *
 * Counting is free, so count always and keep the printing capped. The layers
 * are, top to bottom:
 *
 *   CExoSoundSource::Play   the game deciding to play something
 *   SoundSource::Demand     resolving the asset (NULL m_pRes = cannot proceed)
 *   FMod::CreateSound       building the FMOD Sound   -> our createSound
 *   FMod::PlaySound         asking FMOD for a voice   -> our playSound
 *
 * The gap between any two adjacent rows names the layer that stopped. The one
 * that matters most is the last: if the game's PlaySound count runs ahead of
 * the count that reached our mixer, the game is refusing itself for want of a
 * free voice -- it has 24 2D + 16 3D of them -- and no counter on our side of
 * the boundary can see that. If they track each other, the wedge is higher up. */
static unsigned g_src_ctor = 0, g_src_play = 0, g_src_play_noint = 0;
static unsigned g_demand = 0, g_demand_nores = 0, g_demand_fail = 0;
static unsigned g_streaminit = 0, g_streaminit_fail = 0;
static unsigned g_fmod_create = 0, g_fmod_createstream = 0;
static unsigned g_fmod_play = 0, g_fmod_play_null = 0;

static void sound_pipeline_census(void) {
  log_printf("[snd?] pipeline: %u SoundSource ctor, %u Play (%u no internal), "
             "%u Demand (%u no CRes / %u failed), %u StreamInit (%u failed), "
             "%u CreateSound + %u CreateStream, %u PlaySound (%u returned null) "
             "-> %u reached the mixer  [gap %d]",
             g_src_ctor, g_src_play, g_src_play_noint,
             g_demand, g_demand_nores, g_demand_fail,
             g_streaminit, g_streaminit_fail,
             g_fmod_create, g_fmod_createstream,
             g_fmod_play, g_fmod_play_null, audio_play_count(),
             (int)g_fmod_play - (int)audio_play_count());
}

static void *SndSrcCtor_probe(void *self, const void *resref) {
  static unsigned n = 0;
  if (n < 40) {
    char nm[17] = {0};
    if (resref) memcpy(nm, resref, 16);
    for (int i = 0; i < 16; i++)
      if (nm[i] && (nm[i] < 0x20 || nm[i] > 0x7e)) nm[i] = '.';
    log_printf("[snd?] CExoSoundSource(\"%s\") #%u", nm, n);
  }
  n++; g_src_ctor++;
  return SndSrcCtor_orig(self, resref);
}
static void *SndSrcPlay_probe(void *self) {
  static unsigned n = 0;
  void *internal = self ? *(void **)((char *)self + 4) : NULL;  // m_pInternal
  if (n < 40)
    log_printf("[snd?] CExoSoundSource::Play() #%u m_pInternal=%p%s", n, internal,
               internal ? "" : "  <<< NULL internal, nothing can play");
  n++; g_src_play++; if (!internal) g_src_play_noint++;
  return SndSrcPlay_orig(self);
}

static void *SndDemand_probe(void *self) {
  void *res = self ? *(void **)((char *)self + 8) : NULL;   // m_pRes: NULL == early bail
  void *rc = SndDemand_orig(self);
  static unsigned n = 0;
  if (n < 40)
    log_printf("[snd?] SoundSource::Demand #%u m_pRes=%p -> %p%s", n, res, rc,
               res ? "" : "  <<< no CRes, cannot reach CreateSound");
  n++; g_demand++; if (!res) g_demand_nores++; if (!rc) g_demand_fail++;
  return rc;
}
static void *StreamInit_probe(void *self) {
  void *rc = StreamInit_orig(self);
  static unsigned n = 0;
  if (n < 40) log_printf("[snd?] StreamingSource::InitializeSource #%u -> %p", n, rc);
  n++; g_streaminit++; if (!rc) g_streaminit_fail++;
  return rc;
}
static unsigned g_nclose = 0, g_nrelease = 0;   /* stream/sound teardown counts */

static void *FmodCreateSound_probe(void *self, char *name, int id, void *data,
                                   unsigned size, int e, int f) {
  static unsigned n = 0;
  if (n < 40)
    log_printf("[snd?] FMod::CreateSound #%u \"%s\" id=%d data=%p size=%u (%d,%d)",
               n, name ? name : "?", id, data, size, e, f);
  n++; g_fmod_create++;
  unsigned previous_id = audio_sfx_context_push((unsigned)id);
  void *rc = FmodCreateSound_orig(self, name, id, data, size, e, f);
  audio_sfx_context_pop(previous_id);
  return rc;
}
/* Churn detector.
 *
 * log167: from t=808 the game created the SAME music stream about 7.5 times a
 * second for the rest of the session -- 1255 KB pulled out of the OBB each time,
 * never played, released 23 ms later -- and that is what took Lower City from
 * 38 fps to 1.5. The per-call log above had spent its 40-line budget by t=100,
 * so the one thing the log could not tell us was WHICH asset was storming; the
 * name had to be recovered afterwards by matching byte counts against the OBB.
 *
 * This costs a strcmp per CreateStream and prints nothing during normal play:
 * repeats of the same name only get reported at 16, 64, 256, ... and a run is
 * summarised when it ends. Unbudgeted on purpose -- a storm that starts in hour
 * two must still be named. */
static void createstream_churn(const char *name) {
  static char last[96];
  static unsigned run = 0;
  static unsigned next = 16;

  if (run && !strncmp(last, name, sizeof last - 1)) {
    if (++run >= next) {
      log_printf("[snd] CreateStream CHURN: \"%s\" x%u back to back "
                 "-- the game is re-creating this and not keeping it", last, run);
      next *= 4;
    }
    return;
  }
  if (run >= 16)
    log_printf("[snd] CreateStream churn ended: \"%s\" was created %u times in a row",
               last, run);
  snprintf(last, sizeof last, "%s", name ? name : "?");
  run  = 1;
  next = 16;
}

static void *FmodCreateStream_probe(void *self, char *name, void *rw, int c, int d,
                                    int e, int f, int g) {
  static unsigned n = 0;
  if (n < 40)
    log_printf("[snd?] FMod::CreateStream #%u \"%s\" rw=%p (%d,%d,%d,%d,%d)  "
               "[files open=%d, closes so far=%u]",
               n, name ? name : "?", rw, c, d, e, f, g, io_open_count(), g_nclose);
  n++; g_fmod_createstream++;
  createstream_churn(name ? name : "?");
  return FmodCreateStream_orig(self, name, rw, c, d, e, f, g);
}
/* Does the companion ever give a stream's OBB handle back?
 *
 * log113: open files climb 34 -> 56 as cumulative streams go 3 -> 40 and never
 * fall, then fopen fails and the app wedges. Each CreateStream holds an OBB
 * SDL_RWops. These two probes settle which fix is needed:
 *   CloseStream never fires        -> the game holds streams forever; the lever
 *                                     is stream lifetime (our placeholder length)
 *   CloseStream fires but handles stay -> the companion's OBB path leaks, and
 *                                     streams must not go through it at all
 * Log-only, and each line carries the live handle count so open/close and the
 * handle total can be read off one line. */
static void *(*FmodCloseStream_orig)(void *, unsigned) = NULL;
static void *(*FmodReleaseSound_orig)(void *, int) = NULL;

static void *FmodCloseStream_probe(void *self, unsigned h) {
  void *rc = FmodCloseStream_orig(self, h);
  g_nclose++;
  if (g_nclose < 60 || (g_nclose & 15) == 0)
    log_printf("[snd?] FMod::CloseStream #%u handle=%u  [files open=%d]",
               g_nclose, h, io_open_count());
  return rc;
}
static void *FmodReleaseSound_probe(void *self, int id) {
  void *rc = FmodReleaseSound_orig(self, id);
  g_nrelease++;
  if (g_nrelease < 40 || (g_nrelease & 63) == 0)
    log_printf("[snd?] FMod::ReleaseSound #%u id=%d  [files open=%d]",
               g_nrelease, id, io_open_count());
  return rc;
}

static void *FmodPlaySound_probe(void *self, int id) {
  static unsigned n = 0;
  void *rc = FmodPlaySound_orig(self, id);
  if (n < 40) log_printf("[snd?] FMod::PlaySound #%u id=%d -> %p", n, id, rc);
  n++; g_fmod_play++; if (!rc) g_fmod_play_null++;
  return rc;
}

// hook_named() resolves against libKOTOR; the FModAudioSystem methods live in the
// companion, so the same dance against port_mod.
static void hook_named_port(const char *sym, uintptr_t probe, void **orig,
                            const char *tag) {
  uintptr_t a = so_symbol(&port_mod, sym);
  if (!a) { log_printf("[snd?] %s symbol missing", tag); return; }
  *orig = (void *)build_thumb_trampoline(a, thumb_patch_len(a));
  if (!*orig) { log_printf("[snd?] %s trampoline FAILED", tag); return; }
  hook_thumb(a, probe);
  log_printf("[snd?] %s PROBED: 0x%08x", tag, (unsigned)a);
}

// Dump the persisted ini so the log records the "Sound Init" value the game is
// about to read, independent of whatever the ctor probe reports. Read-only --
// we are still diagnosing, not fixing.
static void dump_ini(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) {
    log_printf("[snd?] ini ABSENT: %s  (=> defaults, sound should be ENABLED)", path);
    return;
  }
  log_printf("[snd?] ini PRESENT: %s size=%lld", path, (long long)st.st_size);
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) { log_printf("[snd?]   open failed: 0x%08x", (unsigned)fd); return; }
  char buf[2049];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0) { log_printf("[snd?]   read failed/empty: %d", n); return; }
  buf[n] = '\0';
  // Line-by-line so the log stays readable and CRLF does not wreck it.
  char *p = buf;
  while (*p) {
    char *e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    char save = *e;
    *e = '\0';
    if (*p) log_printf("[snd?]   | %s", p);
    *e = save;
    while (*e == '\n' || *e == '\r') e++;
    p = e;
  }
}

static void install_sound_probe(void) {
  dump_ini(DATA_PATH "/swkotor2.ini");
  // The one global that can suppress all of sound. GameInit is its only writer,
  // so read it before (should be .bss 0) and again right after GameInit returns.
  g_pDisableSound = (int *)so_symbol(&kotor_mod, "g_bDisableSound");
  log_printf("[snd?] g_bDisableSound @ %p = %d (pre-GameInit)", g_pDisableSound,
             g_pDisableSound ? *g_pDisableSound : -1);
  hook_named("_Z8GameInitv", (uintptr_t)&GameInit_probe,
             (void **)&GameInit_orig, "GameInit");
  hook_named("_ZN9CExoSoundC1Ehhii", (uintptr_t)&ExoSound_ctor_probe,
             (void **)&ExoSound_ctor_orig, "CExoSound::CExoSound");
  hook_named("_ZN17CExoSoundInternal10InitializeEhhii", (uintptr_t)&ExoSoundInit_probe,
             (void **)&ExoSoundInit_orig, "CExoSoundInternal::Initialize");
  hook_named("_ZN15CExoSoundSourceC1ERK7CResRef", (uintptr_t)&SndSrcCtor_probe,
             (void **)&SndSrcCtor_orig, "CExoSoundSource::CExoSoundSource(CResRef)");
  hook_named("_ZN15CExoSoundSource4PlayEv", (uintptr_t)&SndSrcPlay_probe,
             (void **)&SndSrcPlay_orig, "CExoSoundSource::Play");
  hook_named("_ZN23CExoSoundSourceInternal6DemandEv", (uintptr_t)&SndDemand_probe,
             (void **)&SndDemand_orig, "CExoSoundSourceInternal::Demand");
  hook_named("_ZN32CExoStreamingSoundSourceInternal16InitializeSourceEv",
             (uintptr_t)&StreamInit_probe, (void **)&StreamInit_orig,
             "CExoStreamingSoundSourceInternal::InitializeSource");
  hook_named_port("_ZN15FModAudioSystem11CreateSoundEPciPvmii",
                  (uintptr_t)&FmodCreateSound_probe,
                  (void **)&FmodCreateSound_orig, "FModAudioSystem::CreateSound");
  hook_named_port("_ZN15FModAudioSystem12CreateStreamEPcP9SDL_RWopsiiiii",
                  (uintptr_t)&FmodCreateStream_probe,
                  (void **)&FmodCreateStream_orig, "FModAudioSystem::CreateStream");
  hook_named_port("_ZN15FModAudioSystem9PlaySoundEi", (uintptr_t)&FmodPlaySound_probe,
                  (void **)&FmodPlaySound_orig, "FModAudioSystem::PlaySound");
  hook_named_port("_ZN15FModAudioSystem11CloseStreamEm", (uintptr_t)&FmodCloseStream_probe,
                  (void **)&FmodCloseStream_orig, "FModAudioSystem::CloseStream");
  hook_named_port("_ZN15FModAudioSystem12ReleaseSoundEi", (uintptr_t)&FmodReleaseSound_probe,
                  (void **)&FmodReleaseSound_orig, "FModAudioSystem::ReleaseSound");
}

static void install_load_probe(void) {
  g_appmgr_ptr = (void *)so_symbol(&kotor_mod, "g_pAppManager");
  log_printf("[load] g_pAppManager @ %p", g_appmgr_ptr);
  g_ai_update_time = (volatile float *)so_symbol(&kotor_mod, "g_AIUpdateTime");
  g_display_fps = (volatile float *)so_symbol(&kotor_mod, "displayFPS");
  g_movie_fps = (volatile int *)so_symbol(&kotor_mod, "g_nSetMovieFrameRate");
  g_render_skip = (volatile int *)so_symbol(&port_mod, "g_RenderSkip");
  if (g_ai_update_time) g_last_ai_ms = *g_ai_update_time;
  log_printf("[perf] policy globals: AI=%p renderSkip=%p displayFPS=%p movieFPS=%p",
             (void *)g_ai_update_time, (void *)g_render_skip,
             (void *)g_display_fps, (void *)g_movie_fps);
#if DISABLE_ADAPTIVE_RENDER_SKIP
  log_printf("[perf] adaptive render skip override: ON (selected value is logged, then cleared)");
#endif
  // Let the JOYBUTTON log line report what libKOTOR did with the press. All
  // three are plain .bss globals in libKOTOR; a missing one just drops that
  // figure from the line.
  sdl_gamepad_probe_init(so_symbol(&kotor_mod, "pressedGamepadButtons"),
                         so_symbol(&kotor_mod, "pressedGamepadButtonsThisFrame"),
                         so_symbol(&kotor_mod, "gamepadButtonById"));
  hook_named("_ZN21CServerExoAppInternal8MainLoopEv",
             (uintptr_t)&MainLoop_probe, (void **)&MainLoop_orig,
             "CServerExoAppInternal::MainLoop");
  hook_named("_Z10GameUpdatev",
             (uintptr_t)&GameUpdate_probe, (void **)&GameUpdate_orig,
             "GameUpdate");
  hook_named("_Z12UpdateScreenfii",
             (uintptr_t)&UpdateScreen_probe, (void **)&UpdateScreen_orig,
             "UpdateScreen");
  hook_named("_ZN11CAppManager12CreateServerEi",
             (uintptr_t)&CreateServer_probe, (void **)&CreateServer_orig,
             "CAppManager::CreateServer");
  hook_named("_ZN11CAppManager13DestroyServerEv",
             (uintptr_t)&DestroyServer_probe, (void **)&DestroyServer_orig,
             "CAppManager::DestroyServer");
  hook_named("_ZN10CSWSModule18AddModuleResourcesERK10CExoString",
             (uintptr_t)&AddModRes_probe, (void **)&AddModRes_orig,
             "CSWSModule::AddModuleResources");
  hook_named("_ZN20CExoEncapsulatedFile8OpenFileEv",
             (uintptr_t)&OpenFile_probe, (void **)&OpenFile_orig,
             "CExoEncapsulatedFile::OpenFile");
  hook_named("_ZN16CExoBaseInternal16GetDirectoryListEP13CExoArrayListI10CExoStringERKS1_tiii",
             (uintptr_t)&GetDirList_probe, (void **)&GetDirList_orig,
             "CExoBaseInternal::GetDirectoryList");
  hook_named("_ZN12CExoKeyTable6AddKeyERK7CResReftmi",
             (uintptr_t)&AddKey_probe, (void **)&AddKey_orig,
             "CExoKeyTable::AddKey");
  hook_named("_ZN12CExoKeyTable20AddDirectoryContentsEi",
             (uintptr_t)&AddDirContents_probe, (void **)&AddDirContents_orig,
             "CExoKeyTable::AddDirectoryContents");
  hook_named("_ZN10CExoResMan11GetKeyEntryERK7CResReftPP12CExoKeyTablePP14CKeyTableEntry",
             (uintptr_t)&GetKeyEntry_probe, (void **)&GetKeyEntry_orig,
             "CExoResMan::GetKeyEntry");
  hook_named("_ZN10CExoResMan11AddKeyTableERK10CExoStringmm",
             (uintptr_t)&AddKeyTable_probe, (void **)&AddKeyTable_orig,
             "CExoResMan::AddKeyTable");
  hook_named("_ZN10CExoResMan6DemandEP4CRes",
             (uintptr_t)&Demand_probe, (void **)&Demand_orig,
             "CExoResMan::Demand");
  hook_named("_ZN10CSWSModule20LoadModuleInProgressEii",
             (uintptr_t)&LoadInProgress_probe, (void **)&LoadInProgress_orig,
             "CSWSModule::LoadModuleInProgress");
  hook_named("_ZN8CSWSArea8LoadAreaEi",
             (uintptr_t)&LoadArea_probe, (void **)&LoadArea_orig,
             "CSWSArea::LoadArea");
  hook_named("_ZN12IODispatcher13PreSpawnAsyncEPc",
             (uintptr_t)&PreSpawnAsync_probe, (void **)&PreSpawnAsync_orig,
             "IODispatcher::PreSpawnAsync");
  hook_named("_ZN12IODispatcher13RetreiveAsyncEPv",
             (uintptr_t)&RetreiveAsync_probe, (void **)&RetreiveAsync_orig,
             "IODispatcher::RetreiveAsync");
  hook_named("_ZN10CSWSModule15LoadModuleStartERK10CExoStringi",
             (uintptr_t)&LoadModuleStart_probe, (void **)&LoadModuleStart_orig,
             "CSWSModule::LoadModuleStart");
  hook_named("_ZN10CSWSModule16LoadModuleFinishEv",
             (uintptr_t)&LoadModuleFinish_probe, (void **)&LoadModuleFinish_orig,
             "CSWSModule::LoadModuleFinish");
  hook_named("_Z16LoadScreenUpdateiiii",
             (uintptr_t)&LoadScreenUpdate_probe, (void **)&LoadScreenUpdate_orig,
             "LoadScreenUpdate");
}

static void install_gui_probe(void) {
  uintptr_t db = so_symbol(&kotor_mod, "_Z18AurResGetDataBytesmPv");
  if (db) {
    ResDataBytes_orig = (void *(*)(unsigned long, void *))build_thumb_trampoline(db, thumb_patch_len(db));
    if (ResDataBytes_orig) {
      hook_thumb(db, (uintptr_t)&ResDataBytes_probe);
      log_printf("[model] AurResGetDataBytes PROBED: 0x%08x", (unsigned)db);
    }
  } else {
    log_printf("[model] AurResGetDataBytes symbol missing");
  }

  uintptr_t rs = so_symbol(&kotor_mod, "_ZN12IODispatcher8ReadSyncEPc");
  if (rs) {
    ReadSync_orig = (void *(*)(void *, char *))build_thumb_trampoline(rs, thumb_patch_len(rs));
    if (ReadSync_orig) {
      hook_thumb(rs, (uintptr_t)&ReadSync_probe);
      log_printf("[model] IODispatcher::ReadSync(char*) PROBED: 0x%08x", (unsigned)rs);
    }
  } else {
    log_printf("[model] IODispatcher::ReadSync(char*) symbol missing");
  }

  g_models_read = (const volatile int32_t *)so_symbol(&kotor_mod, "g_nModelsRead");
  log_printf("[model] g_nModelsRead @ %p", (void *)g_models_read);

  uintptr_t lm = so_symbol(&kotor_mod, "_ZN12CSWCAnimBase9LoadModelERK7CResRefh");
  if (lm) {
    LoadModel_orig = (void *(*)(void *, const void *, unsigned))
                         build_thumb_trampoline(lm, thumb_patch_len(lm));
    if (LoadModel_orig) {
      hook_thumb(lm, (uintptr_t)&LoadModel_probe);
      log_printf("[model] CSWCAnimBase::LoadModel PROBED: 0x%08x", (unsigned)lm);
    }
  } else {
    log_printf("[model] CSWCAnimBase::LoadModel symbol missing");
  }

  uintptr_t sx = so_symbol(&kotor_mod, "_ZN12CSWGuiObject24ScaleExtentForResolutionEf");
  if (sx) {
    ScaleExt_orig = (void (*)(void *, uint32_t))
                        build_thumb_trampoline(sx, thumb_patch_len(sx));
    if (ScaleExt_orig) {
      hook_thumb(sx, (uintptr_t)&ScaleExt_probe);
      log_printf("[gui] ScaleExtentForResolution PROBED: 0x%08x", (unsigned)sx);
    }
  } else {
    log_printf("[gui] ScaleExtentForResolution symbol missing");
  }

  uintptr_t xl = so_symbol(&kotor_mod, "_ZN12CSWGuiExtent4LoadEP7CResGFFR10CResStruct");
  if (xl) {
    ExtLoad_orig = (int (*)(void *, void *, void *))build_thumb_trampoline(xl, thumb_patch_len(xl));
    if (ExtLoad_orig) {
      hook_thumb(xl, (uintptr_t)&ExtLoad_probe);
      log_printf("[gui] CSWGuiExtent::Load PROBED: 0x%08x", (unsigned)xl);
    }
  } else {
    log_printf("[gui] CSWGuiExtent::Load symbol missing");
  }

  uintptr_t u = so_symbol(&kotor_mod, "_ZN12CAurGUIImage21cm_nGUIBufferSizeUsedE");
  g_gui_buf_used = (const volatile int32_t *)u;
  log_printf("[gui] cm_nGUIBufferSizeUsed @ 0x%08x", (unsigned)u);

  g_scr_w   = (const volatile int32_t *)so_symbol(&kotor_mod, "g_nScreenWidth");
  g_scr_h   = (const volatile int32_t *)so_symbol(&kotor_mod, "g_nScreenHeight");
  g_scr_wp2 = (const volatile int32_t *)so_symbol(&kotor_mod, "_ZN8GLRender19cm_nScreenWidthPow2E");
  g_scr_hp2 = (const volatile int32_t *)so_symbol(&kotor_mod, "_ZN8GLRender20cm_nScreenHeightPow2E");
  log_printf("[gui] screen globals @ w=%p h=%p wp2=%p hp2=%p",
             (void *)g_scr_w, (void *)g_scr_h, (void *)g_scr_wp2, (void *)g_scr_hp2);

  uintptr_t ini = so_symbol(&kotor_mod, "_ZN11CSWGuiImage10InitializeERK12CSWGuiExtentRK17CSWGuiImageParams");
  if (ini) {
    ImgInit_orig = (void (*)(void *, const void *, const void *))
                       build_thumb_trampoline(ini, thumb_patch_len(ini));
    if (ImgInit_orig) {
      hook_thumb(ini, (uintptr_t)&ImgInit_probe);
      log_printf("[gui] CSWGuiImage::Initialize PROBED: 0x%08x", (unsigned)ini);
    }
  } else {
    log_printf("[gui] CSWGuiImage::Initialize symbol missing");
  }

  uintptr_t se = so_symbol(&kotor_mod, "_ZN11CSWGuiImage9SetExtentERK12CSWGuiExtent");
  if (se) {
    size_t se_len = thumb_patch_len(se);
    SetExtent_orig = (void (*)(void *, const void *))build_thumb_trampoline(se, se_len);
    if (SetExtent_orig) {
      hook_thumb(se, (uintptr_t)&SetExtent_probe);
      // patchLen MUST be sampled before hook_thumb -- re-reading it afterwards
      // walks the patched NOP+LDR and reports 10 instead of the 12 actually used
      // (log56 showed exactly that; it was a logging artifact, not a bug).
      log_printf("[gui] CSWGuiImage::SetExtent PROBED: 0x%08x patchLen=%u (text_base=0x%08x)",
                 (unsigned)se, (unsigned)se_len, (unsigned)kotor_mod.text_base);
    }
  } else {
    log_printf("[gui] CSWGuiImage::SetExtent symbol missing");
  }

  uintptr_t d = so_symbol(&kotor_mod, "_ZN11CSWGuiImage4DrawEf");
  if (d) {
    SWImgDraw_orig = (void (*)(void *, uint32_t))build_thumb_trampoline(d, thumb_patch_len(d));
    if (SWImgDraw_orig) {
      hook_thumb(d, (uintptr_t)&SWImgDraw_probe);
      log_printf("[gui] CSWGuiImage::Draw(float) PROBED: 0x%08x", (unsigned)d);
    }
  } else {
    log_printf("[gui] CSWGuiImage::Draw(float) symbol missing");
  }

  uintptr_t f = so_symbol(&kotor_mod, "_ZN12CAurGUIImage11FlushBufferEf");
  if (f) {
    FlushBuf_orig = (void (*)(void *, uint32_t))build_thumb_trampoline(f, thumb_patch_len(f));
    if (FlushBuf_orig) {
      hook_thumb(f, (uintptr_t)&FlushBuf_probe);
      log_printf("[gui] CAurGUIImage::FlushBuffer(float) PROBED: 0x%08x", (unsigned)f);
    }
  } else {
    log_printf("[gui] CAurGUIImage::FlushBuffer(float) symbol missing");
  }
}

static void install_font_probe(void) {
  uintptr_t ws = so_symbol(&kotor_mod, "_ZN21CAurGUIStringInternal11WrapStringsEi");
  if (ws) {
    WrapStrings_orig = (int (*)(void *, int))build_thumb_trampoline(ws, thumb_patch_len(ws));
    if (WrapStrings_orig) {
      hook_thumb(ws, (uintptr_t)&WrapStrings_guard);
      log_printf("[font] WrapStrings GUARDED: ws=0x%08x tramp=%p", (unsigned)ws, (void *)WrapStrings_orig);
    } else {
      hook_thumb(ws, (uintptr_t)&WrapStrings_noop);
      log_printf("[font] WrapStrings trampoline FAILED -- no-op fallback (text off, no crash)");
    }
  } else {
    log_printf("[font] WrapStrings symbol missing -- guard NOT installed");
  }

  uintptr_t dr = so_symbol(&kotor_mod, "_ZN21CAurGUIStringInternal4DrawEf");
  if (dr) {
    Draw_orig = (void (*)(void *, uint32_t))build_thumb_trampoline(dr, thumb_patch_len(dr));
    if (Draw_orig) {
      hook_thumb(dr, (uintptr_t)&Draw_guard);
      log_printf("[font] Draw GUARDED: dr=0x%08x tramp=%p", (unsigned)dr, (void *)Draw_orig);
    } else {
      hook_thumb(dr, (uintptr_t)&Draw_noop);
      log_printf("[font] Draw trampoline FAILED -- no-op fallback (text off, no crash)");
    }
  } else {
    log_printf("[font] Draw symbol missing -- guard NOT installed");
  }
}

// Runs the game's SDL_main (passed as arg) on its own large-stack thread.
// vitaGL MUST be initialised here, on this thread -- GXM binds its render/
// display context to the initialising thread, and the game does ALL its GL from
// this thread. Initialising vitaGL on the main thread instead makes the first
// GXM-touching call (framebuffer/texture setup after the GL cap-query) block
// forever on a cross-thread GPU sync. (Pure glGetIntegerv queries still work,
// which is why init got as far as it did.)
static void *game_main_thread(void *arg) {
  int (*SDL_main)(int, char **) = arg;
  char *game_argv[] = { "KOTOR2", NULL };

  g_game_thid = sceKernelGetThreadId();   // publish for the watchdog
  log_printf(">>> game thread UID = 0x%08x", (unsigned)g_game_thid);

  gl_worker_attach_producer();
  // The GL worker owns core 2; keep the game thread off it.
  if (gl_worker_mode() == 2)
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1);
  log_printf(">>> init vitaGL on game thread");
  vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
  vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, GL_MSAA_MODE);

  // vitaGL ignores the return of sceGxmShaderPatcherCreate (gxm.c:561), so a
  // failed patcher init is silent -- the global just stays NULL and the first
  // sceGxmShaderPatcherRegisterProgram (during glLinkProgram) hands SceGxm a
  // null and faults at FAR=0x24. Both are non-static globals; report them so a
  // failure here is visible at init instead of as a mystery crash later.
  {
    extern SceGxmShaderPatcher *gxm_shader_patcher;
    extern GLboolean is_shark_online;
    log_printf(">>> vitaGL up: gxm_shader_patcher=%p  shark_online=%d",
               (void *)gxm_shader_patcher, (int)is_shark_online);
    if (!gxm_shader_patcher)
      log_printf("!!! gxm_shader_patcher is NULL -- shader patcher failed to "
                 "create; every glLinkProgram will fault inside SceGxm");
  }

  ensure_writable_dirs();

  log_printf(">>> entering SDL_main");
  int rc = SDL_main(1, game_argv);
  log_printf("<<< SDL_main returned %d", rc);
  return NULL;
}

int main(int argc, char *argv[]) {
  log_init();
  log_printf("KOTOR II Vita loader starting (initial bring-up)");

  sceKernelChangeThreadPriority(0, 127);
  sceKernelChangeThreadCpuAffinityMask(0, 0x40000);

  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);

  gl_worker_init();   // before anything initialises vitaGL (GL_WORKER_MODE)

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  if (check_kubridge() < 0)
    fatal_error("kubridge.skprx is not installed.");

  // vitaGL compiles the game's GLSL at runtime via SceShaccCg (libshacccg.suprx),
  // which is NOT present on retail Vitas. Without it the first shader op hangs
  // silently; fail fast with a clear message instead (matches gtasa_vita).
  if (!file_exists("ur0:/data/libshacccg.suprx") &&
      !file_exists("ur0:/data/external/libshacccg.suprx"))
    fatal_error("libshacccg.suprx is not installed (need it in ur0:/data/).");

  // Load the compression libs FIRST: the companion and libKOTOR list them as
  // NEEDED and import mz_zip_reader_*/LzmaUncompress, which then resolve
  // cross-module to these (the real miniz reads the OBB zips; ret0 stubs made
  // the game read a null zip central directory and crash).
  if (load_module(&cxx_mod, CXX_SO, CXX_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", CXX_SO);
  if (load_module(&lzma_mod, LZMA_SO, LZMA_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", LZMA_SO);
  if (load_module(&miniz_mod, MINIZ_SO, MINIZ_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", MINIZ_SO);

  // Then the companion, so libKOTOR's imports of it resolve cross-module.
  if (load_module(&port_mod, ANDROID_PORT_SO, ANDROID_PORT_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", ANDROID_PORT_SO);
  if (load_module(&kotor_mod, SO_PATH, LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", SO_PATH);

  report_unresolved(&cxx_mod, "libc++_shared.so");
  report_unresolved(&port_mod, "libObbVfs.so");
  report_unresolved(&kotor_mod, "libkotor2.so");

  // Now that both module bases are known, arm the CPU-fault handler so any
  // hardware fault (incl. during static ctors below or inside the game) writes
  // its PC/LR to log.txt instead of silently stopping the log.
  crash_init();

  // Arm the new-handler before any of the game's static ctors run, so a heap
  // exhaustion anywhere from here on is reported and survivable rather than an
  // uncaught bad_alloc (log140).
  heap_init();

  // Same point in the sequence, for the same reason: from the first static ctor
  // onwards every allocation at or above BIGALLOC_MIN_BYTES should be landing in
  // the pool rather than carving up newlib's arena (log145).
  bigalloc_init();

  jni_setup();

  patch_saveload_optional_control();
#if SAVE_LIST_TRACE_ENABLE
  install_save_list_trace();
#endif

  // Do not install the generic Thumb trampoline probes in production. Even the
  // lower-level GetDirectoryList/Find* probes run heavily during startup and
  // were observed corrupting call state before MacPlayBinkGL: its filename
  // reached DOS2MacPath as 0xffffffff, followed by a null-program GL cascade.
  // Save investigation must use call-site patches or resolver wrappers instead.
  log_printf("[save-trace] all trampoline probes disabled (crash prevention)");

  // KOTOR II defines the virtual-keyboard platform functions inside libkotor2,
  // so resolver-table overrides cannot replace them. Patch the definitions
  // directly before static initialization and SDL_main can open an edit box.
  // Without this call the save-name panel opens, but receives no text and can
  // never proceed to CSWGuiSaveLoad::WriteGame.
  ime_install_hooks(&kotor_mod);

  // Keep movies out of the initial boot path until KOTOR II reaches a stable UI.
  bink_patch(&kotor_mod);

  uintptr_t disk_space = so_symbol(&kotor_mod, "_Z24ASLPlat_GetDiskSpaceLeftv");
  if (!disk_space)
    fatal_error("KOTOR II disk-space function not found");
  hook_addr(disk_space, (uintptr_t)&disk_space_left_vita);

  so_initialize(&cxx_mod);
  so_initialize(&lzma_mod);
  so_initialize(&miniz_mod);
  so_initialize(&port_mod);
  so_initialize(&kotor_mod);

  // Android calls these before native startup. They populate the OBB filenames
  // used by KOTOR II's own ASL ObbVfs initialization inside SDL_main.
  void (*set_main_obb)(void *, void *, void *) = (void *)so_symbol(
      &kotor_mod, "Java_com_aspyr_base_ASPYR_mainObbFileName");
  void (*set_patch_obb)(void *, void *, void *) = (void *)so_symbol(
      &kotor_mod, "Java_com_aspyr_base_ASPYR_patchObbFileName");
  if (!set_main_obb || !set_patch_obb)
    fatal_error("KOTOR II OBB filename setters not found");
  set_main_obb(jni_get_env(), NULL, (void *)OBB_MAIN_PATH);
  set_patch_obb(jni_get_env(), NULL, (void *)OBB_PATCH_PATH);
  log_printf("[obb] KOTOR II filenames configured: %s | %s",
             OBB_MAIN_PATH, OBB_PATCH_PATH);

  // Font metrics: inject our bundled .txi as a memory-backed resource so
  // CAurFontInfo populates and GUI text renders. The guards below stay installed
  // regardless -- they keep the GUI-string methods null-safe during the window
  // before the font loads, and are the safety net if injection doesn't take.
#if FONT_TXI_MEMORY_INJECT
  install_aurresget_hook();
#endif
  // Keep module loading on the game's original call path. The former audio
  // barrier wrapped LoadModuleStart/Finish with two Thumb trampolines solely to
  // pause long-stream decoding; it is not required for correctness.
#if (PERF_DYNAMIC_PORTAL_FILTER || PERF_DYNAMIC_PORTAL_GOB_FILTER || \
     PERF_DISTANCE_ROOM_FILTER) && !PERFORMANCE_TELEMETRY_ENABLE
  install_dynamic_portal_filter();
#endif
#if PERFORMANCE_TELEMETRY_ENABLE
  install_scene_stage_attribution();
  install_render_callee_attribution();
#endif
  visibility_engine_init();
  ini_defaults_apply();   // before the game reads its options
  effects_low_init();
  // After every installer above that may own Gob::Render/VisibilityCheck
  // (telemetry, portal Gob filter): culling takes both slots or neither.
  occlusion_cull_install();
  ai_list_cache_install();
  ai_list_cache_set(AI_LIST_CACHE_ENABLE);
  cansee_cache_install();
  cansee_cache_set(CANSEE_CACHE_ENABLE);
  bloom_ctl_install();
  bloom_ctl_set_off(BLOOM_DISABLE);
  gl_state_filter_install();
  ai_budget_install();
  dxt_native_install();
#if GL_WORKER_PROFILE
  glw_prof_install();
#endif
#if GUI_RESOURCE_TRACE_ENABLE
  install_gui_resource_trace();
#endif
#if OPTIONS_RESOURCE_DIRECTORY_FIX
  install_options_resource_directory_fix();
#endif
  install_gui_viewport_fix();
  // KOTOR 1 probes contain binary-specific offsets and are intentionally off
  // until equivalent KOTOR II symbols have been validated.

  // NOTE: vitaGL is initialised on the game thread (see game_main_thread), not
  // here -- GXM context must live on the thread that issues GL calls.

  // Phase 1, step 3: hand off to the game's real entry point. SDL renames the
  // game's main() to SDL_main; run it on a dedicated large-stack thread (the
  // Vita main thread's stack is too small for the game). main() then parks in
  // the join so the process stays alive and vitaGL isn't torn down.
  int (*SDL_main)(int, char **) = (void *)so_symbol(&kotor_mod, "SDL_main");
  if (!SDL_main) {
    fatal_error("SDL_main not found in libkotor2.so");
  }

#if PERFORMANCE_TELEMETRY_ENABLE
  // Diagnostic builds sample thread, heap, audio, input, and visibility state.
  pthread_t wd_thread;
  pthread_create(&wd_thread, NULL, watchdog_thread, NULL);
#endif

  log_printf(">>> starting game entry SDL_main on dedicated thread");
  pthread_t game_thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);   // 4 MB game stack
  if (pthread_create(&game_thread, &attr, game_main_thread, (void *)SDL_main) != 0)
    fatal_error("failed to spawn game thread");
  pthread_join(game_thread, NULL);

  log_printf("<<< game thread exited; loader shutting down");
  return 0;
}
