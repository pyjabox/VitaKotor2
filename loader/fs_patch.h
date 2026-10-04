/* fs_patch.h -- filesystem redirection + Android Asset Manager for KOTOR */

#ifndef __FS_PATCH_H__
#define __FS_PATCH_H__

#include "so_util.h"

// Translate an Android path to its ux0:data/kotor2/ equivalent. Writes the
// result into `out` (size `outsz`) and returns it. Logs every request.
const char *fs_translate(const char *in, char *out, int outsz);

// Resolver entries this layer contributes: the imported posix file/dir ops
// (wrapped with translation + logging), SDL_AndroidGetExternalStoragePath, and
// the AAssetManager_* / AAsset_* NDK asset API (backed by ux0:data/kotor2/assets).
const so_default_dynlib *fs_get_dynlib(void);
extern const int fs_dynlib_size;
uintptr_t fs_lookup_symbol(const char *name);

#endif
