/* fs_patch.c -- filesystem redirection + Android Asset Manager for KOTOR II
 * Historical diagnostic comments are inherited from VitaKotor (KOTOR I)
 * unless explicitly marked as KOTOR II validation.
 *
 * Writable paths originate from
 * SDL_AndroidGetExternalStoragePath, game data streams through SDL_RWFromFile
 * (resolved by Vita-native SDL2 in the entry-point phase), and a small set of
 * APK assets (shaders + a font) come through the Android Asset Manager NDK API
 * (AAssetManager_open / AAsset_*), which neither .so provides -- so we do.
 *
 * Everything here funnels paths to ux0:data/kotor2/ and logs each request,
 * including misses, so the exact file the game wants is always visible.
 *
 *   ux0:data/kotor2/            writable root, DLC, and OBB archives
 *   ux0:data/kotor2/assets/     APK assets served through AAssetManager
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "main.h"
#include "fs_patch.h"
#include "so_util.h"
#include "log.h"

#define ASSET_PATH DATA_PATH "/assets"

// Android path prefixes we rewrite onto DATA_PATH. Longest/most-specific first.
static const char *android_prefixes[] = {
  "/storage/emulated/0/Android/obb/com.aspyr.swkotorii",
  "/storage/emulated/0/Android/data/com.aspyr.swkotorii/files",
  "/storage/emulated/0/Android/data/com.aspyr.swkotorii",
  "/data/data/com.aspyr.swkotorii/files",
  "/data/data/com.aspyr.swkotorii",
  "/sdcard/Android/obb/com.aspyr.swkotorii",
  "/sdcard",
};

#define FS_REL_LOG_LIMIT 600
static unsigned g_rel_log_n = 0;

static const char *fs_translate_ex(const char *in, char *out, int outsz, int do_log);

const char *fs_translate(const char *in, char *out, int outsz) {
  return fs_translate_ex(in, out, outsz, 1);
}

// `do_log` exists for stat(): CExoBaseInternal::GetDirectoryList stats every
// candidate name in a directory, so logging each translation would bury the log.
static const char *fs_translate_ex(const char *in, char *out, int outsz, int do_log) {
  if (!in) { out[0] = 0; return out; }

  // Already a Vita path -- leave it alone.
  if (strncmp(in, "ux0:", 4) == 0 || strncmp(in, "app0:", 5) == 0 ||
      strncmp(in, "ur0:", 4) == 0) {
    snprintf(out, outsz, "%s", in);
    return out;
  }

  for (unsigned i = 0; i < sizeof(android_prefixes) / sizeof(*android_prefixes); i++) {
    size_t plen = strlen(android_prefixes[i]);
    if (strncmp(in, android_prefixes[i], plen) == 0) {
      snprintf(out, outsz, "%s%s", DATA_PATH, in + plen);   // keep tail after prefix
      if (do_log) log_printf("[FS] %s -> %s", in, out);
      return out;
    }
  }

  // Unknown absolute path: fall back to DATA_PATH + the whole path, and flag it
  // so an unexpected location shows up in the log instead of silently failing.
  if (in[0] == '/') {
    snprintf(out, outsz, "%s%s", DATA_PATH, in);
    if (do_log) log_printf("[FS] (unmapped abs) %s -> %s", in, out);
    return out;
  }

  // Relative path -- resolve against the writable root.
  snprintf(out, outsz, "%s/%s", DATA_PATH, in);
  // log97: in-game the game reopens the texture packs per texture load --
  // swpc_tex_gui.erf alone was translated 449 times -- and each log line is an
  // open/write/close on the memory card. Budget it; the interesting translations
  // all happen during bring-up.
  if (do_log && g_rel_log_n < FS_REL_LOG_LIMIT) {
    log_printf("[FS] (rel) %s -> %s", in, out);
    if (++g_rel_log_n == FS_REL_LOG_LIMIT)
      log_printf("[FS] (rel) path trace silenced after %d lines (steady state)",
                 FS_REL_LOG_LIMIT);
  }
  return out;
}

// ---- posix file/dir ops (translate + log + forward to newlib/sceIo) --------
static int fs_access(const char *path, int mode) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  int r = access(t, mode);
  if (r != 0) log_printf("[FS] access MISS: %s", t);
  return r;
}
static int fs_chdir(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  int r = chdir(t);
  log_printf("[FS] chdir %s -> %d", t, r);
  return r;
}
static DIR *fs_opendir(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  DIR *d = opendir(t);
  if (!d) log_printf("[FS] opendir MISS: %s", t);
  return d;
}
// `_findfirst` reads Android ARM32/bionic stat fields at +16/+48/+80, while
// VitaSDK fills newlib's incompatible layout. The apparent stat call at
// libkotor2.so+0x5b4af0 does not reach this resolver directly: KOTOR's internal
// stat()/statImpl dispatches through ASL FsApi::Native::stat, whose callback
// returns here from +0x59d77c (LR +0x59d780).
//
// LR alone is not enough to identify `_findfirst`: every Native::stat callback
// has that same return address. Converting every such call corrupts callers that
// supplied a native-sized output buffer and caused the startup crash observed
// before save enumeration. Restrict the ABI conversion to the save tree. This
// covers `_findfirst` candidates such as `./saves/000001 - autosave` without
// changing unrelated startup/resource stat calls.
struct bionic_stat {
  uint64_t st_dev;
  uint32_t __pad0;
  uint32_t __st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint32_t __pad3;
  uint32_t __pad4;
  int64_t  st_size;
  uint32_t st_blksize;
  uint32_t __pad5;
  uint64_t st_blocks;
  int32_t  st_atime_sec;
  uint32_t st_atime_nsec;
  int32_t  st_mtime_sec;
  uint32_t st_mtime_nsec;
  int32_t  st_ctime_sec;
  uint32_t st_ctime_nsec;
  uint64_t st_ino;
};
_Static_assert(sizeof(struct bionic_stat) == 104, "bionic stat size must be 104");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_mode) == 16,
               "bionic st_mode must sit at +16");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_size) == 48,
               "bionic st_size must sit at +48");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_mtime_sec) == 80,
               "bionic st_mtime must sit at +80");

#define BIONIC_S_IFDIR 0040000
#define BIONIC_S_IFREG 0100000

static int fs_stat(const char *path, void *out) {
  char t[512];
  fs_translate_ex(path, t, sizeof(t), 0);

  uintptr_t caller = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  uintptr_t caller_off = kotor_mod.text_base ? caller - kotor_mod.text_base : 0;
  int is_save_path = path && strstr(path, "saves");
  if (!is_save_path || !kotor_mod.text_base || caller_off != 0x59d780u)
    return stat(t, out);

  struct stat native_st;
  int r = stat(t, &native_st);
  if (r == 0 && out) {
    struct bionic_stat *st = out;
    memset(st, 0, sizeof(*st));
    st->st_mode = (S_ISDIR(native_st.st_mode) ? BIONIC_S_IFDIR : BIONIC_S_IFREG) |
                  (native_st.st_mode & 0777);
    st->st_size = native_st.st_size;
    st->st_atime_sec = native_st.st_atime;
    st->st_mtime_sec = native_st.st_mtime;
    st->st_ctime_sec = native_st.st_ctime;
  } else if (is_save_path) {
    // Misses remain useful for diagnosing partially-created save slots. Successful
    // probes are intentionally silent: opening Load Game stats every file in every
    // slot, and synchronous memory-card logging made the real-Vita menu sluggish.
    log_printf("[save-stat] MISS %s -> %d", t, r);
  }
  return r;
}

// ROOT CAUSE of the module-load stall (log95, confirmed by disassembly).
//
// `readdir` was forwarded straight through, handing the .so a **vitasdk**
// `struct dirent` while it was compiled against **bionic's**. The layouts put
// `d_name` in completely different places:
//
//   bionic ARM32:  u64 d_ino @0, s64 d_off @8, u16 d_reclen @16,
//                  u8 d_type @18, char d_name[256] @19
//   vitasdk:       SceIoStat d_stat @0 (88 bytes), char d_name[256] @88
//
// `CExoBaseInternal::GetDirectoryList` is the only readdir caller in libKOTOR
// (+0x4b6322) and it does exactly this:
//
//   4b6346:  r1 = dirent + 19        ; bionic d_name
//   4b634c:  memcpy(new char[272], r1, 256)
//
// Offset 19 in the vitasdk struct is the **high byte of `st_ctime.month`**,
// which is always 0 because a month is <= 12. So every name read from a real
// directory came back as an empty string -- deterministically, every time.
//
// Why that stalled the load: directories that live in the OBB (rims/, modules/,
// override/) are enumerated from the zip index and were never affected, which is
// why 234 modules/ keys landed and everything looked healthy. `currentgame/`
// exists only on the card, so its one real file -- END_M01AA.rim, a byte-exact
// 55565-byte copy -- yielded a single EMPTY name. `AddDirectoryContents` then
// rejected it at its first filter (`GetResTypeFromFile("")` finds no '.' and
// returns 0xFFFF), so `AddKey` was never called, the CURRENTGAME: key table
// stayed empty, `GetKeyEntry("end_m01aa", .rim)` missed inside
// `CExoResMan::AsyncLoad`, `AddKeyTable("CURRENTGAME:END_M01AA", type=4)` never
// ran, the module's CRes id stayed 0xFFFFFFFF, `Demand` returned NULL and
// `LoadModuleStart` returned 1.
//
// It also explains the MODULES: 234 -> 235 bump: the card's directory entries all
// collapse to one empty string via CExoArrayList::AddUnique.
struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};
_Static_assert(__builtin_offsetof(struct bionic_dirent, d_name) == 19,
               "bionic d_name must sit at +19 -- the .so hardcodes that offset");

#define BIONIC_DT_DIR 4
#define BIONIC_DT_REG 8

static struct bionic_dirent g_bdirent;   // readdir's return is caller-borrowed
static unsigned g_readdir_n = 0;

static void *fs_readdir(DIR *d) {
  struct dirent *e = readdir(d);
  if (!e) return NULL;
  memset(&g_bdirent, 0, sizeof g_bdirent);
  g_bdirent.d_ino    = ++g_readdir_n;          // some callers skip ino == 0
  g_bdirent.d_reclen = (uint16_t)sizeof g_bdirent;
  g_bdirent.d_type   = SCE_S_ISDIR(e->d_stat.st_mode) ? BIONIC_DT_DIR : BIONIC_DT_REG;
  snprintf(g_bdirent.d_name, sizeof g_bdirent.d_name, "%s", e->d_name);
  if (g_readdir_n <= 64)
    log_printf("[FS] readdir -> \"%s\" type=%u  [#%u]", g_bdirent.d_name,
               g_bdirent.d_type, g_readdir_n);
  return &g_bdirent;
}
static int fs_closedir(DIR *d)           { return closedir(d); }
static int fs_unlink(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  int r = unlink(t);
  log_printf("[FS] unlink %s -> %d", t, r);
  return r;
}
/* CExoResMan::RemoveFile uses the C remove() API, resolved lazily through
 * Aspyr's ASL filesystem layer. Leaving it unresolved makes NukeDirectory
 * silently retain old currentgame modules; CopyFileA then overwrites only the
 * new prefix and leaves the previous save's tail behind. Preserve the game's
 * original cleanup path by translating and forwarding this missing primitive. */
static int fs_remove(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  int r = remove(t);
  log_printf("[FS] remove %s -> %d", t, r);
  return r;
}
static int fs_mkdir(const char *path, mode_t mode) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  int r = mkdir(t, mode);
  log_printf("[FS] mkdir %s -> %d", t, r);
  return r;
}
static int fs_rmdir(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  int r = rmdir(t);
  log_printf("[FS] rmdir %s -> %d", t, r);
  return r;
}
static int fs_remove_tree(const char *path) {
  DIR *d = opendir(path);
  if (!d) {
    // It may be a file, or it may not exist. Treat a missing path as success so
    // stale transaction backups can be cleaned idempotently.
    if (unlink(path) == 0 || access(path, F_OK) != 0) return 0;
    return -1;
  }

  int result = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[512];
    int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
    if (n < 0 || n >= (int)sizeof(child) || fs_remove_tree(child) != 0) {
      result = -1;
      break;
    }
  }
  closedir(d);
  if (result == 0 && rmdir(path) != 0) result = -1;
  return result;
}

static int fs_is_load_transaction(const char *from, const char *to) {
  const char *future = DATA_PATH "/futuregame";
  const char *progress = DATA_PATH "/gameinprogress";
  return (!strcmp(from, future) || !strcmp(from, DATA_PATH "/futuregame/")) &&
         (!strcmp(to, progress) || !strcmp(to, DATA_PATH "/gameinprogress/"));
}

static int fs_rename(const char *a, const char *b) {
  char ta[512], tb[512];
  fs_translate(a, ta, sizeof(ta));
  fs_translate(b, tb, sizeof(tb));
  int r = rename(ta, tb);

  // Android/Windows semantics used by KOTOR replace the previous
  // gameinprogress directory. Vita/newlib refuses to rename a directory over an
  // existing non-empty directory, which left a second load using stale files.
  // Keep the old tree recoverable until the new one is in place.
  if (r != 0 && fs_is_load_transaction(ta, tb)) {
    // fs_is_load_transaction() guarantees this exact destination, so construct
    // the backup from the bounded constant instead of appending to arbitrary
    // caller input.
    char backup[] = DATA_PATH "/gameinprogress.vita-old";
    fs_remove_tree(backup);

    int had_old = access(tb, F_OK) == 0;
    if (had_old && rename(tb, backup) != 0) {
      log_printf("[FS] load transaction could not preserve %s", tb);
    } else {
      r = rename(ta, tb);
      if (r == 0) {
        if (had_old && fs_remove_tree(backup) != 0)
          log_printf("[FS] load transaction left backup %s", backup);
        log_printf("[FS] load transaction replaced gameinprogress");
      } else if (had_old) {
        // Best-effort rollback: never discard the last usable game state merely
        // because installation of futuregame failed.
        if (rename(backup, tb) != 0)
          log_printf("[FS] load transaction ROLLBACK FAILED %s", backup);
      }
    }
  }

  log_printf("[FS] rename %s -> %s = %d", ta, tb, r);
  return r;
}

// ---- SDL Android extension ------------------------------------------------
// Vita SDL2 has no SDL_AndroidGetExternalStoragePath; the game calls it to find
// its writable root. Hand back DATA_PATH so all derived paths land there.
static const char *SDL_AndroidGetExternalStoragePath(void) {
  log_printf("[FS] SDL_AndroidGetExternalStoragePath -> %s", DATA_PATH);
  return DATA_PATH;
}

static const char *SDL_AndroidGetInternalStoragePath(void) {
  log_printf("[FS] SDL_AndroidGetInternalStoragePath -> %s", DATA_PATH);
  return DATA_PATH;
}

// ---- Android Asset Manager (NDK) ------------------------------------------
// Minimal AAsset backed by a real file under ux0:data/kotor2/assets/.
typedef struct {
  FILE *fp;
  long  size;
  long  pos;
} FakeAsset;

static void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", ASSET_PATH, filename ? filename : "");
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    log_printf("[ASSET] open MISS: %s", path);
    return NULL;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  FakeAsset *a = calloc(1, sizeof(FakeAsset));
  a->fp = fp; a->size = sz; a->pos = 0;
  log_printf("[ASSET] open: %s (%ld bytes)", filename ? filename : "?", sz);
  return a;
}
static int AAsset_read(void *asset, void *buf, size_t count) {
  FakeAsset *a = asset;
  if (!a) return -1;
  size_t n = fread(buf, 1, count, a->fp);
  a->pos += (long)n;
  return (int)n;
}
static long AAsset_seek(void *asset, long off, int whence) {
  FakeAsset *a = asset;
  if (!a) return -1;
  if (fseek(a->fp, off, whence) != 0) return -1;
  a->pos = ftell(a->fp);
  return a->pos;
}
static long AAsset_getRemainingLength(void *asset) {
  FakeAsset *a = asset;
  return a ? a->size - a->pos : 0;
}
static void AAsset_close(void *asset) {
  FakeAsset *a = asset;
  if (!a) return;
  if (a->fp) fclose(a->fp);
  free(a);
}

// ---- resolver table -------------------------------------------------------
static const so_default_dynlib fs_dynlib[] = {
  { "access",   (uintptr_t)&fs_access },
  { "chdir",    (uintptr_t)&fs_chdir },
  { "stat",     (uintptr_t)&fs_stat },
  { "opendir",  (uintptr_t)&fs_opendir },
  { "readdir",  (uintptr_t)&fs_readdir },
  { "closedir", (uintptr_t)&fs_closedir },
  { "unlink",   (uintptr_t)&fs_unlink },
  { "remove",   (uintptr_t)&fs_remove },
  { "mkdir",    (uintptr_t)&fs_mkdir },
  { "rmdir",    (uintptr_t)&fs_rmdir },
  { "rename",   (uintptr_t)&fs_rename },
  { "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
  { "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath },
  { "AAssetManager_open",         (uintptr_t)&AAssetManager_open },
  { "AAsset_read",                (uintptr_t)&AAsset_read },
  { "AAsset_seek",                (uintptr_t)&AAsset_seek },
  { "AAsset_getRemainingLength",  (uintptr_t)&AAsset_getRemainingLength },
  { "AAsset_close",               (uintptr_t)&AAsset_close },
};
const int fs_dynlib_size = sizeof(fs_dynlib);
const so_default_dynlib *fs_get_dynlib(void) { return fs_dynlib; }
uintptr_t fs_lookup_symbol(const char *name) {
  for (unsigned i = 0; i < sizeof(fs_dynlib) / sizeof(fs_dynlib[0]); i++)
    if (!strcmp(fs_dynlib[i].symbol, name)) return fs_dynlib[i].func;
  return 0;
}
