/* opensl_patch.c -- the small OpenSL ES contract used by KOTOR's Bink adapter.
 *
 * This is deliberately not a general OpenSL implementation. The embedded Bink
 * player needs four pointer-to-vtable interfaces and a bounded PCM queue. Movie
 * PCM is mixed into audio_patch.c's existing 48 kHz output, so there is one BGM
 * port and one output thread for the whole application. */
#include <vitasdk.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "bink_audio_queue.h"
#include "audio_patch.h"
#include "bink_patch.h"
#include "opensl_patch.h"
#include "log.h"

#define BINK_SL_MAX_PLAYERS 4

typedef enum {
  BINK_SL_ENGINE,
  BINK_SL_OUTPUT_MIX,
  BINK_SL_PLAYER
} BinkSLObjectKind;

typedef struct {
  const struct SLObjectItf_ *itf;
  BinkSLObjectKind kind;
  unsigned generation;
  int used, realized;
} BinkSLObject;

typedef struct {
  BinkSLObject object;
  const struct SLEngineItf_ *engine_itf;
} BinkSLEngine;

typedef struct {
  BinkSLObject object;
} BinkSLOutputMix;

typedef struct {
  BinkSLObject object;
  const struct SLPlayItf_ *play_itf;
  const struct SLBufferQueueItf_ *queue_itf;
  const struct SLVolumeItf_ *volume_itf;
  BinkAudioQueue queue;
  slBufferQueueCallback callback;
  void *callback_context;
  SLuint32 play_state;
  SLmillibel volume_mb;
  float gain;
  SLboolean stereo_enabled;
  SLpermille stereo_position;
  uint64_t enqueue_bytes;
  unsigned peak_sample, state_calls, volume_calls;
} BinkSLPlayer;

typedef struct {
  BinkSLPlayer *player;
  unsigned generation;
} CallbackJob;

typedef struct {
  uint64_t engines, output_mixes, players, destroyed;
  uint64_t enqueued, completed, callbacks, mixed_frames, underrun_frames;
  uint64_t discarded;
  unsigned queue_high, errors;
} BinkSLStats;

/* Keep OpenSL call tracing bounded: enough to diagnose player construction and
 * first submission without flooding the log during a full movie. */
static unsigned g_call_trace_budget = 64;

#define SL_TRACE(...) do { \
  if (g_call_trace_budget) { \
    g_call_trace_budget--; \
    log_printf(__VA_ARGS__); \
  } \
} while (0)

static SceUID g_mutex = -1;
static SceUID g_callback_mutex = -1;
static BinkSLEngine g_engine;
static BinkSLOutputMix g_output_mix;
static BinkSLPlayer g_players[BINK_SL_MAX_PLAYERS];
static BinkSLStats g_stats;
static unsigned g_generation;
static int g_active_players;

static const struct SLInterfaceID_ g_iid_engine_value = { 1, 0, 0, 0, {0} };
static const struct SLInterfaceID_ g_iid_play_value = { 2, 0, 0, 0, {0} };
static const struct SLInterfaceID_ g_iid_volume_value = { 3, 0, 0, 0, {0} };
static const struct SLInterfaceID_ g_iid_bufferqueue_value = { 4, 0, 0, 0, {0} };

/* The imported symbols are pointer variables. The resolver supplies the address
 * of each variable; the Android code dereferences it to obtain the IID pointer. */
const SLInterfaceID bink_sl_iid_engine = &g_iid_engine_value;
const SLInterfaceID bink_sl_iid_play = &g_iid_play_value;
const SLInterfaceID bink_sl_iid_volume = &g_iid_volume_value;
const SLInterfaceID bink_sl_iid_bufferqueue = &g_iid_bufferqueue_value;

static void ensure_mutexes(void) {
  if (g_mutex < 0) g_mutex = sceKernelCreateMutex("kotor_bink_sl", 0, 0, NULL);
  if (g_callback_mutex < 0)
    g_callback_mutex = sceKernelCreateMutex("kotor_bink_cb", 0, 0, NULL);
}

static void state_lock(void) {
  ensure_mutexes();
  if (g_mutex >= 0) sceKernelLockMutex(g_mutex, 1, NULL);
}

static void state_unlock(void) {
  if (g_mutex >= 0) sceKernelUnlockMutex(g_mutex, 1);
}

static void callback_lock(void) {
  ensure_mutexes();
  if (g_callback_mutex >= 0) sceKernelLockMutex(g_callback_mutex, 1, NULL);
}

static void callback_unlock(void) {
  if (g_callback_mutex >= 0) sceKernelUnlockMutex(g_callback_mutex, 1);
}

static int iid_is(SLInterfaceID actual, const SLInterfaceID *expected) {
  return actual == *expected;
}

#define CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

static BinkSLPlayer *player_from_play(SLPlayItf self) {
  return CONTAINER_OF(self, BinkSLPlayer, play_itf);
}

static BinkSLPlayer *player_from_queue(SLBufferQueueItf self) {
  return CONTAINER_OF(self, BinkSLPlayer, queue_itf);
}

static BinkSLPlayer *player_from_volume(SLVolumeItf self) {
  return CONTAINER_OF(self, BinkSLPlayer, volume_itf);
}

static SLresult object_realize(SLObjectItf self, SLboolean async) {
  if (!self) {
    log_printf("[BINK:SL:CALL] Object.Realize self=NULL async=%u -> %u",
               (unsigned)async, (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }
  state_lock();
  BinkSLObject *object = (BinkSLObject *)self;
  BinkSLObjectKind kind = object->kind;
  unsigned generation = object->generation;
  SLresult result;
  if (!object->used) {
    result = SL_RESULT_PRECONDITIONS_VIOLATED;
  } else {
    object->realized = 1;
    result = SL_RESULT_SUCCESS;
  }
  state_unlock();
  SL_TRACE("[BINK:SL:CALL] Object.Realize self=%p kind=%u gen=%u async=%u -> %u",
           self, (unsigned)kind, generation, (unsigned)async, (unsigned)result);
  return result;
}

static SLresult object_resume(SLObjectItf self, SLboolean async) {
  return object_realize(self, async);
}

static SLresult object_get_state(SLObjectItf self, SLuint32 *state) {
  if (!self || !state) return SL_RESULT_PARAMETER_INVALID;
  BinkSLObject *object = (BinkSLObject *)self;
  *state = object->realized ? SL_OBJECT_STATE_REALIZED : SL_OBJECT_STATE_UNREALIZED;
  return SL_RESULT_SUCCESS;
}

static SLresult object_get_interface(SLObjectItf self, const SLInterfaceID iid,
                                     void *out) {
  if (!self || !iid || !out) {
    log_printf("[BINK:SL:CALL] Object.GetInterface self=%p iid=%p out=%p -> %u",
               self, iid, out, (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }
  *(void **)out = NULL;

  state_lock();
  BinkSLObject *object = (BinkSLObject *)self;
  BinkSLObjectKind kind = object->kind;
  unsigned generation = object->generation;
  const char *iid_name = iid_is(iid, &bink_sl_iid_engine) ? "ENGINE" :
                         iid_is(iid, &bink_sl_iid_play) ? "PLAY" :
                         iid_is(iid, &bink_sl_iid_bufferqueue) ? "BUFFERQUEUE" :
                         iid_is(iid, &bink_sl_iid_volume) ? "VOLUME" : "UNKNOWN";
  SLresult result = SL_RESULT_FEATURE_UNSUPPORTED;
  if (!object->used) {
    result = SL_RESULT_PRECONDITIONS_VIOLATED;
  } else if (object->kind == BINK_SL_ENGINE && iid_is(iid, &bink_sl_iid_engine)) {
    BinkSLEngine *engine = (BinkSLEngine *)object;
    *(SLEngineItf *)out = &engine->engine_itf;
    result = SL_RESULT_SUCCESS;
  } else if (object->kind == BINK_SL_PLAYER) {
    BinkSLPlayer *player = (BinkSLPlayer *)object;
    if (iid_is(iid, &bink_sl_iid_play)) {
      *(SLPlayItf *)out = &player->play_itf;
      result = SL_RESULT_SUCCESS;
    } else if (iid_is(iid, &bink_sl_iid_bufferqueue)) {
      *(SLBufferQueueItf *)out = &player->queue_itf;
      result = SL_RESULT_SUCCESS;
    } else if (iid_is(iid, &bink_sl_iid_volume)) {
      *(SLVolumeItf *)out = &player->volume_itf;
      result = SL_RESULT_SUCCESS;
    }
  }
  if (result != SL_RESULT_SUCCESS) g_stats.errors++;
  void *returned = *(void **)out;
  state_unlock();
  SL_TRACE("[BINK:SL:CALL] Object.GetInterface self=%p kind=%u gen=%u iid=%s/%p out=%p value=%p -> %u",
           self, (unsigned)kind, generation, iid_name, iid, out, returned,
           (unsigned)result);
  return result;
}

static SLresult object_register_callback(SLObjectItf self, slObjectCallback callback,
                                         void *context) {
  (void)self; (void)callback; (void)context;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void object_abort(SLObjectItf self) { (void)self; }

static void object_destroy(SLObjectItf self) {
  if (!self) return;

  /* A completed-buffer callback may re-enter Enqueue. Serializing Destroy with
   * callback invocation guarantees no callback can use Bink context after this
   * function returns. */
  callback_lock();
  state_lock();
  BinkSLObject *object = (BinkSLObject *)self;
  int log_player = 0;
  unsigned generation = 0, rate = 0, channels = 0, peak = 0;
  unsigned state = 0, state_calls = 0, volume_calls = 0, enqueues = 0;
  int volume_mb = 0;
  uint64_t enqueue_bytes = 0;
  if (!object->used) {
    state_unlock();
    callback_unlock();
    return;
  }

  if (object->kind == BINK_SL_PLAYER) {
    BinkSLPlayer *player = (BinkSLPlayer *)object;
    log_player = 1;
    generation = player->object.generation;
    rate = player->queue.rate;
    channels = player->queue.channels;
    peak = player->peak_sample;
    state = player->play_state;
    state_calls = player->state_calls;
    volume_calls = player->volume_calls;
    volume_mb = player->volume_mb;
    enqueues = (unsigned)player->queue.enqueued;
    enqueue_bytes = player->enqueue_bytes;
    if (player->queue.capacity > 2) bink_patch_stop_audio_pump();
    g_stats.discarded += player->queue.count;
    bink_audio_queue_clear(&player->queue);
    player->callback = NULL;
    player->callback_context = NULL;
    player->play_state = SL_PLAYSTATE_STOPPED;
    __atomic_sub_fetch(&g_active_players, 1, __ATOMIC_RELEASE);
    g_stats.destroyed++;
  }
  object->used = 0;
  object->realized = 0;
  state_unlock();
  if (log_player) {
    log_printf("[BINK:SL] player destroyed: gen=%u %uHz %uch state=%u/%u "
               "volume=%d/%u enqueue=%u/%uKB peak=%u",
               generation, rate, channels, state, state_calls, volume_mb,
               volume_calls, enqueues, (unsigned)(enqueue_bytes / 1024u), peak);
  }
  callback_unlock();
}

static SLresult object_set_priority(SLObjectItf self, SLint32 priority,
                                    SLboolean preemptable) {
  (void)self; (void)priority; (void)preemptable;
  return SL_RESULT_SUCCESS;
}

static SLresult object_get_priority(SLObjectItf self, SLint32 *priority,
                                    SLboolean *preemptable) {
  (void)self;
  if (!priority || !preemptable) return SL_RESULT_PARAMETER_INVALID;
  *priority = 0;
  *preemptable = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static SLresult object_set_loss(SLObjectItf self, SLint16 count,
                                SLInterfaceID *ids, SLboolean enabled) {
  (void)self; (void)count; (void)ids; (void)enabled;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLObjectItf_ g_object_vtable = {
  object_realize, object_resume, object_get_state, object_get_interface,
  object_register_callback, object_abort, object_destroy, object_set_priority,
  object_get_priority, object_set_loss
};

static SLresult play_set_state(SLPlayItf self, SLuint32 state) {
  BinkSLPlayer *player = player_from_play(self);
  if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING) {
    log_printf("[BINK:SL:CALL] Play.SetPlayState self=%p state=%u -> %u",
               self, (unsigned)state, (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }
  state_lock();
  if (!player->object.used) {
    state_unlock();
    log_printf("[BINK:SL:CALL] Play.SetPlayState self=%p state=%u -> %u",
               self, (unsigned)state, (unsigned)SL_RESULT_PRECONDITIONS_VIOLATED);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  SLuint32 previous = player->play_state;
  player->play_state = state;
  player->state_calls++;
  unsigned gen = player->object.generation, rate = player->queue.rate;
  state_unlock();
  if (previous != state)
    log_printf("[BINK:SL] player gen=%u %uHz play_state %u -> %u",
               gen, rate, (unsigned)previous, (unsigned)state);
  SL_TRACE("[BINK:SL:CALL] Play.SetPlayState gen=%u state=%u -> %u",
           gen, (unsigned)state, (unsigned)SL_RESULT_SUCCESS);
  return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ g_play_vtable = {
  .SetPlayState = play_set_state
};

static SLresult queue_enqueue(SLBufferQueueItf self, const void *buffer,
                              SLuint32 size) {
  BinkSLPlayer *player = player_from_queue(self);
  state_lock();
  SLresult result;
  if (!player->object.used) {
    result = SL_RESULT_PRECONDITIONS_VIOLATED;
  } else if (!bink_audio_queue_enqueue(&player->queue, buffer, size)) {
    result = player->queue.count >= player->queue.capacity ?
      SL_RESULT_BUFFER_INSUFFICIENT : SL_RESULT_PARAMETER_INVALID;
  } else {
    const int16_t *pcm = (const int16_t *)buffer;
    unsigned samples = size / sizeof *pcm;
    for (unsigned i = 0; i < samples; i++) {
      int sample = pcm[i];
      unsigned magnitude = (unsigned)(sample < 0 ? -sample : sample);
      if (magnitude > player->peak_sample) player->peak_sample = magnitude;
    }
    player->enqueue_bytes += size;
    g_stats.enqueued++;
    if (player->queue.high_water > g_stats.queue_high)
      g_stats.queue_high = player->queue.high_water;
    result = SL_RESULT_SUCCESS;
    /* First real submission per player is the decisive datum: it proves Bink's
     * Ready gate opened for this track. Log it once (rate identifies which). */
    if (player->queue.enqueued == 1)
      log_printf("[BINK:SL] FIRST enqueue on gen=%u %uHz %uch size=%u",
                 player->object.generation, player->queue.rate,
                 player->queue.channels, (unsigned)size);
  }
  if (result != SL_RESULT_SUCCESS) g_stats.errors++;
  state_unlock();
  SL_TRACE("[BINK:SL:CALL] BufferQueue.Enqueue self=%p buffer=%p bytes=%u -> %u",
           self, buffer, (unsigned)size, (unsigned)result);
  return result;
}

static SLresult queue_clear(SLBufferQueueItf self) {
  BinkSLPlayer *player = player_from_queue(self);
  state_lock();
  if (!player->object.used) {
    state_unlock();
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  g_stats.discarded += player->queue.count;
  bink_audio_queue_clear(&player->queue);
  state_unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(SLBufferQueueItf self, SLBufferQueueState *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  BinkSLPlayer *player = player_from_queue(self);
  state_lock();
  state->count = player->queue.count;
  state->playIndex = (SLuint32)player->queue.completed;
  state_unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult queue_register_callback(SLBufferQueueItf self,
                                        slBufferQueueCallback callback,
                                        void *context) {
  BinkSLPlayer *player = player_from_queue(self);
  state_lock();
  SLresult result;
  unsigned generation = player->object.generation;
  if (!player->object.used) {
    result = SL_RESULT_PRECONDITIONS_VIOLATED;
  } else {
    player->callback = callback;
    player->callback_context = context;
    result = SL_RESULT_SUCCESS;
  }
  state_unlock();
  SL_TRACE("[BINK:SL:CALL] BufferQueue.RegisterCallback self=%p gen=%u callback=%p context=%p -> %u",
           self, generation, callback, context, (unsigned)result);
  return result;
}

static const struct SLBufferQueueItf_ g_queue_vtable = {
  queue_enqueue, queue_clear, queue_get_state, queue_register_callback
};

static SLresult volume_set_level(SLVolumeItf self, SLmillibel level) {
  BinkSLPlayer *player = player_from_volume(self);
  if (level > 0) level = 0;
  state_lock();
  player->volume_mb = level;
  player->gain = powf(10.0f, (float)level / 2000.0f);
  player->volume_calls++;
  state_unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult volume_enable_stereo(SLVolumeItf self, SLboolean enabled) {
  BinkSLPlayer *player = player_from_volume(self);
  state_lock();
  player->stereo_enabled = enabled ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  state_unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult volume_set_stereo(SLVolumeItf self, SLpermille position) {
  BinkSLPlayer *player = player_from_volume(self);
  if (position < -1000 || position > 1000) return SL_RESULT_PARAMETER_INVALID;
  state_lock();
  player->stereo_position = position;
  state_unlock();
  return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ g_volume_vtable = {
  .SetVolumeLevel = volume_set_level,
  .EnableStereoPosition = volume_enable_stereo,
  .SetStereoPosition = volume_set_stereo
};

static SLresult engine_create_player(SLEngineItf self, SLObjectItf *out,
                                     SLDataSource *source, SLDataSink *sink,
                                     SLuint32 interface_count,
                                     const SLInterfaceID *interface_ids,
                                     const SLboolean *interface_required) {
  SL_TRACE("[BINK:SL:CALL] Engine.CreateAudioPlayer self=%p out=%p source=%p sink=%p interfaces=%u ids=%p required=%p",
           self, out, source, sink, (unsigned)interface_count,
           interface_ids, interface_required);
  if (!out) {
    log_printf("[BINK:SL:CALL] Engine.CreateAudioPlayer -> %u (out=NULL)",
               (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }
  *out = NULL;
  if (!source || !source->pLocator || !source->pFormat) {
    log_printf("[BINK:SL:CALL] Engine.CreateAudioPlayer -> %u (invalid source)",
               (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }

  SLDataLocator_BufferQueue *locator = (SLDataLocator_BufferQueue *)source->pLocator;
  SLDataFormat_PCM *format = (SLDataFormat_PCM *)source->pFormat;
  SL_TRACE("[BINK:SL:CALL] source locator=%u buffers=%u format=%u rate_mHz=%u channels=%u bits=%u container=%u endian=%u",
           (unsigned)locator->locatorType, (unsigned)locator->numBuffers,
           (unsigned)format->formatType, (unsigned)format->samplesPerSec,
           (unsigned)format->numChannels, (unsigned)format->bitsPerSample,
           (unsigned)format->containerSize, (unsigned)format->endianness);
  for (SLuint32 i = 0; i < interface_count; i++)
    SL_TRACE("[BINK:SL:CALL] requested[%u] iid=%p required=%u",
             (unsigned)i, interface_ids ? interface_ids[i] : NULL,
             (unsigned)(interface_required ? interface_required[i] : 0));
  if ((locator->locatorType != SL_DATALOCATOR_BUFFERQUEUE &&
       locator->locatorType != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) ||
      !locator->numBuffers || locator->numBuffers > BINK_AUDIO_QUEUE_MAX ||
      format->formatType != SL_DATAFORMAT_PCM ||
      (format->numChannels != 1 && format->numChannels != 2) ||
      format->bitsPerSample != 16 || format->containerSize != 16 ||
      format->endianness != SL_BYTEORDER_LITTLEENDIAN ||
      format->samplesPerSec < 1000 || format->samplesPerSec % 1000) {
    state_lock(); g_stats.errors++; state_unlock();
    log_printf("[BINK:SL:CALL] Engine.CreateAudioPlayer -> %u (unsupported format)",
               (unsigned)SL_RESULT_CONTENT_UNSUPPORTED);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }

  state_lock();
  BinkSLPlayer *player = NULL;
  for (unsigned i = 0; i < BINK_SL_MAX_PLAYERS; i++) {
    if (!g_players[i].object.used) { player = &g_players[i]; break; }
  }
  if (!player) {
    g_stats.errors++;
    state_unlock();
    return SL_RESULT_RESOURCE_ERROR;
  }

  memset(player, 0, sizeof *player);
  player->object.itf = &g_object_vtable;
  player->object.kind = BINK_SL_PLAYER;
  player->object.generation = ++g_generation;
  player->object.used = 1;
  player->play_itf = &g_play_vtable;
  player->queue_itf = &g_queue_vtable;
  player->volume_itf = &g_volume_vtable;
  player->play_state = SL_PLAYSTATE_STOPPED;
  player->gain = 1.0f;
  unsigned rate = format->samplesPerSec / 1000u;
  bink_audio_queue_init(&player->queue, rate, format->numChannels,
                        locator->numBuffers);
  g_stats.players++;
  __atomic_add_fetch(&g_active_players, 1, __ATOMIC_RELEASE);
  *out = &player->object.itf;
  state_unlock();

  log_printf("[BINK:SL] player created: %u Hz %u ch queue=%u generation=%u",
             rate, (unsigned)format->numChannels, (unsigned)locator->numBuffers,
             player->object.generation);
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_output_mix(SLEngineItf self, SLObjectItf *out,
                                         SLuint32 interface_count,
                                         const SLInterfaceID *interface_ids,
                                         const SLboolean *interface_required) {
  SL_TRACE("[BINK:SL:CALL] Engine.CreateOutputMix self=%p out=%p interfaces=%u ids=%p required=%p",
           self, out, (unsigned)interface_count, interface_ids,
           interface_required);
  if (!out) {
    log_printf("[BINK:SL:CALL] Engine.CreateOutputMix -> %u",
               (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }
  state_lock();
  memset(&g_output_mix, 0, sizeof g_output_mix);
  g_output_mix.object.itf = &g_object_vtable;
  g_output_mix.object.kind = BINK_SL_OUTPUT_MIX;
  g_output_mix.object.generation = ++g_generation;
  g_output_mix.object.used = 1;
  g_stats.output_mixes++;
  *out = &g_output_mix.object.itf;
  state_unlock();
  log_printf("[BINK:SL:CALL] Engine.CreateOutputMix out_value=%p -> %u",
             *out, (unsigned)SL_RESULT_SUCCESS);
  return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ g_engine_vtable = {
  .CreateAudioPlayer = engine_create_player,
  .CreateOutputMix = engine_create_output_mix
};

SLresult bink_slCreateEngine(SLObjectItf *out, SLuint32 num_options,
                             const SLEngineOption *options,
                             SLuint32 num_interfaces,
                             const SLInterfaceID *interface_ids,
                             const SLboolean *interface_required) {
  SL_TRACE("[BINK:SL:CALL] slCreateEngine out=%p options=%u/%p interfaces=%u ids=%p required=%p",
           out, (unsigned)num_options, options, (unsigned)num_interfaces,
           interface_ids, interface_required);
  if (!out) {
    log_printf("[BINK:SL:CALL] slCreateEngine -> %u", (unsigned)SL_RESULT_PARAMETER_INVALID);
    return SL_RESULT_PARAMETER_INVALID;
  }
  ensure_mutexes();
  if (g_mutex < 0 || g_callback_mutex < 0) {
    log_printf("[BINK:SL:CALL] slCreateEngine mutex=%d callback_mutex=%d -> %u",
               (int)g_mutex, (int)g_callback_mutex, (unsigned)SL_RESULT_RESOURCE_ERROR);
    return SL_RESULT_RESOURCE_ERROR;
  }
  if (!audio_ensure_output()) {
    log_printf("[BINK:SL] shared Vita audio output unavailable");
    return SL_RESULT_RESOURCE_ERROR;
  }

  state_lock();
  memset(&g_engine, 0, sizeof g_engine);
  g_engine.object.itf = &g_object_vtable;
  g_engine.object.kind = BINK_SL_ENGINE;
  g_engine.object.generation = ++g_generation;
  g_engine.object.used = 1;
  g_engine.engine_itf = &g_engine_vtable;
  g_stats.engines++;
  *out = &g_engine.object.itf;
  state_unlock();
  log_printf("[BINK:SL] engine created (existing Vita mixer backend)");
  log_printf("[BINK:SL:CALL] slCreateEngine out_value=%p -> %u",
             *out, (unsigned)SL_RESULT_SUCCESS);
  return SL_RESULT_SUCCESS;
}

void bink_opensl_mix(int32_t *acc, unsigned frames, unsigned out_rate) {
  if (!acc || !__atomic_load_n(&g_active_players, __ATOMIC_ACQUIRE)) return;

  for (unsigned i = 0; i < BINK_SL_MAX_PLAYERS; i++) {
    BinkSLPlayer *player = &g_players[i];
    state_lock();
    unsigned offset = 0;
    while (player->object.used &&
           player->play_state == SL_PLAYSTATE_PLAYING && offset < frames) {
      unsigned chunk = bink_audio_queue_frames_to_completion(&player->queue,
                                                              out_rate);
      if (!chunk || chunk > frames - offset) chunk = frames - offset;

      float gain_l = player->gain, gain_r = player->gain;
      if (player->stereo_enabled) {
        float pan = (float)player->stereo_position / 1000.0f;
        if (pan > 0.0f) gain_l *= 1.0f - pan;
        else            gain_r *= 1.0f + pan;
      }

      uint64_t under_before = player->queue.underrun_frames;
      uint64_t mixed_before = player->queue.mixed_frames;
      unsigned completed = bink_audio_queue_mix(&player->queue, acc + offset * 2,
                                                 chunk, out_rate, gain_l, gain_r);
      g_stats.completed += completed;
      g_stats.underrun_frames += player->queue.underrun_frames - under_before;
      g_stats.mixed_frames += player->queue.mixed_frames - mixed_before;
      offset += chunk;
      if (!completed) continue;

      CallbackJob job = { player, player->object.generation };
      state_unlock();
      callback_lock();
      state_lock();
      int valid = player->object.used &&
                  player->object.generation == job.generation;
      slBufferQueueCallback callback = valid ? player->callback : NULL;
      void *context = valid ? player->callback_context : NULL;
      SLBufferQueueItf queue = valid ? &player->queue_itf : NULL;
      state_unlock();
      if (callback) {
        callback(queue, context);
        state_lock(); g_stats.callbacks++; state_unlock();
#ifdef KOTOR2_BUILD
        bink_patch_on_audio_buffer_complete();
#endif
      }
      callback_unlock();
      state_lock();
    }
    state_unlock();
  }
}

void bink_opensl_log_stats(void) {
  state_lock();
  BinkSLStats stats = g_stats;
  int active = __atomic_load_n(&g_active_players, __ATOMIC_ACQUIRE);
  /* Snapshot per-player state under the lock, then log outside it. */
  struct { unsigned gen, rate, ch, state, count; uint64_t enq, comp, mixed, under; int used; }
    snap[BINK_SL_MAX_PLAYERS];
  for (unsigned i = 0; i < BINK_SL_MAX_PLAYERS; i++) {
    BinkSLPlayer *p = &g_players[i];
    snap[i].used = p->object.used;
    snap[i].gen = p->object.generation;
    snap[i].rate = p->queue.rate;
    snap[i].ch = p->queue.channels;
    snap[i].state = (unsigned)p->play_state;
    snap[i].count = p->queue.count;
    snap[i].enq = p->queue.enqueued;
    snap[i].comp = p->queue.completed;
    snap[i].mixed = p->queue.mixed_frames;
    snap[i].under = p->queue.underrun_frames;
  }
  state_unlock();
  log_printf("[BINK:SL] stats: engines=%u mixes=%u players=%u/%u active=%d "
             "queue=%u high=%u completed=%u callbacks=%u discarded=%u "
             "mixed=%u underrun=%u errors=%u",
             (unsigned)stats.engines, (unsigned)stats.output_mixes,
             (unsigned)stats.players, (unsigned)stats.destroyed, active,
             (unsigned)stats.enqueued, stats.queue_high,
             (unsigned)stats.completed, (unsigned)stats.callbacks,
             (unsigned)stats.discarded, (unsigned)stats.mixed_frames,
             (unsigned)stats.underrun_frames, stats.errors);
  for (unsigned i = 0; i < BINK_SL_MAX_PLAYERS; i++) {
    if (!snap[i].used && !snap[i].enq) continue;
    log_printf("[BINK:SL]   player[%u] gen=%u %uHz %uch state=%u qcount=%u "
               "enq=%u completed=%u mixed=%u underrun=%u%s",
               i, snap[i].gen, snap[i].rate, snap[i].ch, snap[i].state,
               snap[i].count, (unsigned)snap[i].enq, (unsigned)snap[i].comp,
               (unsigned)snap[i].mixed, (unsigned)snap[i].under,
               snap[i].used ? "" : " (freed)");
  }
}
