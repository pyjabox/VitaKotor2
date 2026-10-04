/* jni_patch.h -- fake JNI environment for the KOTOR Android build */

#ifndef __JNI_PATCH_H__
#define __JNI_PATCH_H__

#include "so_util.h"

// SDL2's JNI helper that the game calls to obtain a JNIEnv*.
void *Android_JNI_GetEnv(void);

// Build the fake VM/env. Every slot the game touches traces to log.txt.
// This build has no JNI_OnLoad; the environment is exercised when the real
// entry point (SDL_main) runs.
void jni_setup(void);

// Accessors for the fake JavaVM* / JNIEnv* (needed when we invoke the entry).
void *jni_get_vm(void);
void *jni_get_env(void);

// Entries this layer contributes to the resolver (Android_JNI_GetEnv, etc.).
const so_default_dynlib *jni_get_dynlib(void);
extern const int jni_dynlib_size;

#endif
