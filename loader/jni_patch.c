/* jni_patch.c -- fake Java Native Interface for the KOTOR Android build
 * Historical diagnostic comments are inherited from VitaKotor (KOTOR I)
 * unless explicitly marked as KOTOR II validation.
 *
 * A fake JNIEnv/JavaVM whose function-table slots point at logging shims. This
 * build has NO JNI_OnLoad and does NOT use RegisterNatives -- its native
 * surface is a set of statically exported Java callbacks plus SDL_main. The
 * game reaches Java through its own
 * out-of-line _JNIEnv:: wrappers, which funnel into the *MethodV slots of this
 * table; those are the slots we populate with typed shims. Everything else
 * falls through to a traced safe-default so an unexpected call logs instead of
 * dereferencing a null slot.
 *
 * Instrumentation goal (Phase 1, step 1): every JNI call the lib makes is
 * logged by name -- FindClass, Get(Static)MethodID (name+signature),
 * RegisterNatives (each method name+sig), GetStringUTFChars, and every
 * Call*Method variant -- so the first Java-side call before any hang/crash is
 * always visible. No real Java behaviour is emulated.
 *
 * NOTE: CallStaticFloatMethodV returns a float across the .so boundary. Under
 * the hardfp loader vs softfp Android ABI its result lands in the wrong
 * register; that is the global ABI issue tracked for the toolchain phase, not
 * something to patch here.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "main.h"
#include "config.h"
#include "jni_patch.h"
#include "so_util.h"
#include "log.h"

static char fake_vm[0x1000];
static char fake_env[0x1000];

// Install a shim at JNI function-table index i (byte offset i*4).
#define ENV(i, fn) (((uintptr_t *)fake_env)[(i)] = (uintptr_t)(fn))
#define VM(i, fn)  (((uintptr_t *)fake_vm)[(i)] = (uintptr_t)(fn))

// ---- id / class registries (so Call* logs name the method) ----------------
// GetMethodID & friends hand back a synthetic non-null id that encodes an index
// into this table; Call* shims resolve it back to name+sig for the log. Fake
// ids/classes are non-null so callers that check `if (!id) fail` proceed.
#define MAX_REG 512
static struct { const char *name, *sig; } id_reg[MAX_REG];
static int id_count;
static struct { const char *name; } cls_reg[MAX_REG];
static int cls_count;

#define FAKE_ID_BASE  0x00010000u
#define FAKE_CLS_BASE 0x00020000u

static void *reg_id(const char *name, const char *sig) {
  int i = (id_count < MAX_REG) ? id_count++ : 0;
  id_reg[i].name = name;
  id_reg[i].sig = sig;
  return (void *)(uintptr_t)(FAKE_ID_BASE + i);
}
static const char *id_name(void *mid) {
  uintptr_t v = (uintptr_t)mid;
  if (v >= FAKE_ID_BASE && v < FAKE_ID_BASE + MAX_REG)
    return id_reg[v - FAKE_ID_BASE].name;
  return "?";
}
static void *reg_cls(const char *name) {
  int i = (cls_count < MAX_REG) ? cls_count++ : 0;
  cls_reg[i].name = name;
  return (void *)(uintptr_t)(FAKE_CLS_BASE + i);
}
static const char *cls_name(void *clazz) {
  uintptr_t v = (uintptr_t)clazz;
  if (v >= FAKE_CLS_BASE && v < FAKE_CLS_BASE + MAX_REG)
    return cls_reg[v - FAKE_CLS_BASE].name;
  return "?";
}

// ---- generic safe default -------------------------------------------------
// Any table slot we did not explicitly fill lands here: log and return 0.
// (JNI functions return either a 32-bit value or nothing; 0 is safe for both.)
static int jni_unknown(void) {
  LOG_JNI("<unhandled JNI slot> -> return 0");
  return 0;
}

// ---- lookups --------------------------------------------------------------
static void *FindClass(void *env, const char *name) {
  void *c = reg_cls(name);
  LOG_JNI("FindClass(\"%s\") -> 0x%08x", name ? name : "?", (unsigned)(uintptr_t)c);
  return c;
}
static void *GetMethodID(void *env, void *clazz, const char *name, const char *sig) {
  void *id = reg_id(name, sig);
  LOG_JNI("GetMethodID(%s.%s : %s) -> 0x%08x",
          cls_name(clazz), name ? name : "?", sig ? sig : "?", (unsigned)(uintptr_t)id);
  return id;
}
static void *GetStaticMethodID(void *env, void *clazz, const char *name, const char *sig) {
  void *id = reg_id(name, sig);
  LOG_JNI("GetStaticMethodID(%s.%s : %s) -> 0x%08x",
          cls_name(clazz), name ? name : "?", sig ? sig : "?", (unsigned)(uintptr_t)id);
  return id;
}
static void *GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
  void *id = reg_id(name, sig);
  LOG_JNI("GetFieldID(%s.%s : %s)", cls_name(clazz), name ? name : "?", sig ? sig : "?");
  return id;
}
static void *GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
  void *id = reg_id(name, sig);
  LOG_JNI("GetStaticFieldID(%s.%s : %s)", cls_name(clazz), name ? name : "?", sig ? sig : "?");
  return id;
}

// ---- object / instance Call*MethodV ---------------------------------------
static void *NewObjectV(void *env, void *clazz, void *mid, va_list a) {
  LOG_JNI("NewObject(%s / id=%s)", cls_name(clazz), id_name(mid));
  return (void *)FAKE_CLS_BASE;  // non-null fake object
}
static void *CallObjectMethodV(void *env, void *obj, void *mid, va_list a) {
  LOG_JNI("CallObjectMethod(id=%s)", id_name(mid));
  return NULL;
}
static int CallBooleanMethodV(void *env, void *obj, void *mid, va_list a) {
  LOG_JNI("CallBooleanMethod(id=%s) -> false", id_name(mid));
  return 0;
}
static int CallIntMethodV(void *env, void *obj, void *mid, va_list a) {
  LOG_JNI("CallIntMethod(id=%s) -> 0", id_name(mid));
  return 0;
}
static void CallVoidMethodV(void *env, void *obj, void *mid, va_list a) {
  LOG_JNI("CallVoidMethod(id=%s)", id_name(mid));
}

// ---- static Call*MethodV (the set the game's wrappers actually use) --------
static void *CallStaticObjectMethodV(void *env, void *cls, void *mid, va_list a) {
  LOG_JNI("CallStaticObjectMethod(id=%s) -> null", id_name(mid));
  return NULL;
}
static int CallStaticBooleanMethodV(void *env, void *cls, void *mid, va_list a) {
  const char *n = id_name(mid);
  // Gameplay is controller-only. The Vita IME remains available for text entry.
  int r = 0;
  // log98: this defaulted to 0 for GetHighResolution, which puts the engine in
  // its low-res mobile mode: it clamps the render height to `resolutionCap` (480)
  // and scales width to match, giving glViewport(0,0,847,480) and a 847x480 colour
  // target on a 960x544 screen (847 = 960 * 480/544 exactly). The scene FBO is
  // presented scaled so normal gameplay looks full-screen, but anything drawn in
  // screen coordinates from g_nScreenWidth/Height -- the cinematic letterbox and
  // the conversation overlays -- is laid out for 847x480 and lands misplaced on
  // the 960x544 output. That is the "black bars during cutscenes and dialogue".
  // The Vita has the fill rate for native res, so say yes.
  if (!strcmp(n, "GetHighResolution")) r = 1;
  LOG_JNI("CallStaticBooleanMethod(id=%s) -> %d", n, r);
  return r;
}
static int CallStaticIntMethodV(void *env, void *cls, void *mid, va_list a) {
  const char *n = id_name(mid);
  // Real Vita geometry: returning 0 made the game think the surface was 0-sized.
  int r = 0;
  if (!strcmp(n, "GetScreenWidthPixel"))       r = 960;
  else if (!strcmp(n, "GetScreenHeightPixel")) r = 544;
  LOG_JNI("CallStaticIntMethod(id=%s) -> %d", n, r);
  return r;
}
// Returns uint32 (lands in r0) holding the float bit-pattern, so the softfp
// Android caller reads a correct float from r0 -- a hardfp `float` return would
// go in s0 and the caller would read garbage from r0 (the old ABI caveat).
static uint32_t CallStaticFloatMethodV(void *env, void *cls, void *mid, va_list a) {
  const char *n = id_name(mid);
  float f = 0.0f;                       // 5" Vita OLED, 960x544 -> ~220 dpi
  if (!strcmp(n, "GetScreenWidthInch"))       f = 4.35f;
  else if (!strcmp(n, "GetScreenHeightInch")) f = 2.47f;
  union { float f; uint32_t u; } c = { .f = f };
  LOG_JNI("CallStaticFloatMethod(id=%s) -> %d/1000 (softfp r0)", n, (int)(f * 1000));
  return c.u;
}
static void CallStaticVoidMethodV(void *env, void *cls, void *mid, va_list a) {
  LOG_JNI("CallStaticVoidMethod(id=%s)", id_name(mid));
}

// ---- instance field reads -------------------------------------------------
// The game reads DisplayMetrics.xdpi/.ydpi as floats. This slot was unimplemented,
// so both came back 0.0 -- and the GUI scales widget and glyph geometry by DPI, so
// every widget and every string collapsed to zero size (or NaN) while full-screen
// art, which isn't DPI-scaled, kept drawing. log50 showed exactly that: 4 textured
// quads per frame (1024x512 + 512x1024 + 2x 32x32) with the 756x106 menu buttons
// and the 256x256 font atlas never drawn at all.
// 960x544 across a 5" diagonal is ~220 dpi, which agrees with the GetScreen*Inch
// values above (960/4.35 and 544/2.47 both land on ~220) -- keep them consistent
// or the game can derive contradictory geometry from the two sources.
// Returns uint32 for the same softfp reason as CallStaticFloatMethodV.
static uint32_t GetFloatField(void *env, void *obj, void *fid) {
  const char *n = id_name(fid);
  float f = 1.0f;  // never default to 0: that is a divide-by-zero in layout code
  if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi")) f = 220.0f;
  else if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) f = 1.0f;
  else LOG_JNI("GetFloatField(%s) unmapped -> defaulting to 1.0", n);
  union { float f; uint32_t u; } c = { .f = f };
  LOG_JNI("GetFloatField(%s) -> %d/1000 (softfp r0)", n, (int)(f * 1000));
  return c.u;
}

// ---- strings --------------------------------------------------------------
static void *NewStringUTF(void *env, const char *bytes) {
  LOG_JNI("NewStringUTF(\"%s\")", bytes ? bytes : "(null)");
  return (void *)bytes;  // fake jstring == the original char*
}
static const char *GetStringUTFChars(void *env, void *string, int *isCopy) {
  LOG_JNI("GetStringUTFChars(0x%08x)", (unsigned)(uintptr_t)string);
  if (isCopy) *isCopy = 0;
  return (const char *)string;  // our jstrings are char* (from NewStringUTF)
}
static void ReleaseStringUTFChars(void *env, void *string, const char *utf) {
  LOG_JNI("ReleaseStringUTFChars(0x%08x)", (unsigned)(uintptr_t)string);
}
static int GetStringUTFLength(void *env, void *string) {
  int n = string ? (int)strlen((const char *)string) : 0;
  LOG_JNI("GetStringUTFLength -> %d", n);
  return n;
}

// ---- references / exceptions ----------------------------------------------
static void *NewGlobalRef(void *env, void *obj) {
  LOG_JNI("NewGlobalRef(0x%08x)", (unsigned)(uintptr_t)obj);
  return obj ? obj : (void *)FAKE_CLS_BASE;
}
static void DeleteGlobalRef(void *env, void *obj) { LOG_JNI("DeleteGlobalRef"); }
static void DeleteLocalRef(void *env, void *obj)  { LOG_JNI("DeleteLocalRef"); }
static void *NewLocalRef(void *env, void *obj)    { LOG_JNI("NewLocalRef"); return obj; }
static int ExceptionCheck(void *env)     { LOG_JNI("ExceptionCheck -> false"); return 0; }
static void *ExceptionOccurred(void *env){ LOG_JNI("ExceptionOccurred -> null"); return NULL; }
static void ExceptionClear(void *env)    { LOG_JNI("ExceptionClear"); }
static int GetVersion(void *env)         { LOG_JNI("GetVersion -> 1.6"); return 0x00010006; }

// ---- RegisterNatives ------------------------------------------------------
// This build resolves natives by static export name, so this is not expected
// to fire -- but instrument it anyway: if it ever does, dump every registered
// method's name + signature (that is the native-surface map).
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod;
static int RegisterNatives(void *env, void *clazz, const JNINativeMethod *m, int n) {
  LOG_JNI("RegisterNatives(%s, n=%d):", cls_name(clazz), n);
  for (int i = 0; i < n && m; i++)
    LOG_JNI("  [%d] %s %s -> 0x%08x", i,
            m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?",
            (unsigned)(uintptr_t)m[i].fn);
  return 0;
}

// ---- JavaVM ---------------------------------------------------------------
static int GetEnv(void *vm, void **env, int version) {
  LOG_JNI("JavaVM::GetEnv(version=0x%x)", version);
  *env = fake_env;
  return 0;
}
static int AttachCurrentThread(void *vm, void **env, void *args) {
  LOG_JNI("JavaVM::AttachCurrentThread");
  *env = fake_env;
  return 0;
}
static int DetachCurrentThread(void *vm) { LOG_JNI("JavaVM::DetachCurrentThread"); return 0; }

// ---- table construction ---------------------------------------------------
// Indices are JNINativeInterface_ slot numbers (byte offset = index*4).
static void build_env(void) {
  for (unsigned i = 0; i < sizeof(fake_env) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)&jni_unknown;

  ENV(0, fake_env);                      // reserved0 / self
  ENV(4,   GetVersion);
  ENV(6,   FindClass);
  ENV(15,  ExceptionOccurred);
  ENV(17,  ExceptionClear);
  ENV(21,  NewGlobalRef);
  ENV(22,  DeleteGlobalRef);
  ENV(23,  DeleteLocalRef);
  ENV(25,  NewLocalRef);
  ENV(29,  NewObjectV);
  ENV(33,  GetMethodID);
  ENV(35,  CallObjectMethodV);
  ENV(38,  CallBooleanMethodV);
  ENV(50,  CallIntMethodV);
  ENV(62,  CallVoidMethodV);
  ENV(94,  GetFieldID);
  ENV(102, GetFloatField);   // DisplayMetrics.xdpi/.ydpi -- 0.0 killed all GUI geometry
  ENV(113, GetStaticMethodID);
  ENV(115, CallStaticObjectMethodV);
  ENV(118, CallStaticBooleanMethodV);
  ENV(130, CallStaticIntMethodV);
  ENV(136, CallStaticFloatMethodV);
  ENV(142, CallStaticVoidMethodV);
  ENV(144, GetStaticFieldID);
  ENV(167, NewStringUTF);
  ENV(168, GetStringUTFLength);
  ENV(169, GetStringUTFChars);
  ENV(170, ReleaseStringUTFChars);
  ENV(215, RegisterNatives);
  ENV(228, ExceptionCheck);
}

static void build_vm(void) {
  for (unsigned i = 0; i < sizeof(fake_vm) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)&jni_unknown;
  VM(0, fake_vm);            // reserved0 / self
  VM(4, AttachCurrentThread);   // slot 4  (offset 0x10)
  VM(5, DetachCurrentThread);   // slot 5  (offset 0x14)
  VM(6, GetEnv);                // slot 6  (offset 0x18)
}

void *jni_get_vm(void)  { return fake_vm; }
void *jni_get_env(void) { return fake_env; }

void *Android_JNI_GetEnv(void) {
  LOG_JNI("Android_JNI_GetEnv");
  return fake_env;
}

void jni_setup(void) {
  LOG_JNI("jni_setup: building fake VM/env (instrumented)");
  build_env();
  build_vm();
  // No JNI_OnLoad in this build; the env is exercised when SDL_main runs.
}

static const so_default_dynlib jni_dynlib[] = {
  { "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
};
const int jni_dynlib_size = sizeof(jni_dynlib);
const so_default_dynlib *jni_get_dynlib(void) { return jni_dynlib; }
