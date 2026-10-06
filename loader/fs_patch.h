/* fs_patch.h -- filesystem redirection + Android Asset Manager for KOTOR */

#ifndef __FS_PATCH_H__
#define __FS_PATCH_H__

#include "so_util.h"

// Translate an Android path to its ux0:data/kotor2/ equivalent. Writes the
// result into `out` (size `outsz`) and returns it. Logs every request.
const char *fs_translate(const char *in, char *out, int outsz);
// The same without the log line (stat-like probes).
const char *fs_translate_quiet(const char *in, char *out, int outsz);

// FS_MISS_CACHE: 1 when a translated path in a read-only loose-file folder is
// certainly not on the card (its folder listed once); fs_miss_forget drops the
// lists of a written path's folder and parent; fs_miss_answered counts misses
// answered without the card.
int fs_known_missing(const char *translated);
void fs_miss_forget(const char *translated);
unsigned fs_miss_answered(void);

// Resolver entries this layer contributes: the imported posix file/dir ops
// (wrapped with translation + logging), SDL_AndroidGetExternalStoragePath, and
// the AAssetManager_* / AAsset_* NDK asset API (backed by ux0:data/kotor2/assets).
const so_default_dynlib *fs_get_dynlib(void);
extern const int fs_dynlib_size;
uintptr_t fs_lookup_symbol(const char *name);

#endif
