/* audio_patch.h -- FMOD/OpenSLES backend over sceAudioOut (see audio_patch.c)
 * Historical diagnostic comments are inherited from VitaKotor (KOTOR I).
 */

#ifndef __AUDIO_PATCH_H__
#define __AUDIO_PATCH_H__

#include "so_util.h"

// Number of entries appended by audio_get_dynlib().
extern const int audio_dynlib_size;

// Returns the FMOD/OpenSLES table so main can splice it into the resolver
// (kept separate to keep dynlib.c readable).
const so_default_dynlib *audio_get_dynlib(void);
uintptr_t audio_lookup_symbol(const char *name);

// Bytes of decoded PCM currently held by the sound cache.
unsigned audio_cache_bytes(void);

// Free every cached decode nothing is currently referencing, and return how
// many bytes that recovered. Called when the heap is exhausted: the cache is
// pure speed, so handing it back beats an allocation failure. Anything a live
// Sound still points at is kept.
unsigned audio_cache_purge(void);

// One [snd] stats census line. Driven by the watchdog's clock rather than by
// createSound volume, because a volume-triggered summary thins out exactly when
// the thing it measures stops -- which is what happened in log172.
void audio_log_stats(void);

// playSound calls that actually reached the mixer. The sound pipeline census in
// main.c prints this beside the game's own PlaySound count: a gap between them
// is the game refusing itself, which no counter on this side can see.
unsigned audio_play_count(void);

typedef struct {
  unsigned feed_count, feed_max_us, underruns;
  uint64_t feed_us;
  /* Phase telemetry (experiment 1): cumulative mixer-mutex hold for the
   * channel mix, cumulative blocking-output wait, and what non-audio threads
   * cumulatively waited to acquire the mutex (glock). Deltas per frame window
   * attribute slow frames to audio-side contention. */
  uint64_t mix_us, out_us, glock_us;
  unsigned glock_n;
} audio_perf_t;

// Cumulative streaming-decoder work for correlation with slow frames.
void audio_perf_snapshot(audio_perf_t *out);

// Suspend only incremental long-stream decoding/playback. The pause call waits
// for an in-flight feed to finish; PCM sound effects and sceAudioOut stay live.
void audio_streaming_pause(int paused);

// Ensure the application's single sceAudioOut thread is ready. The custom Bink
// OpenSL adapter shares this output rather than opening a second BGM port.
int audio_ensure_output(void);

// FModAudioSystem's wrapper knows the stable resource ID, but FMOD::createSound
// receives only the transient buffer. Scope the ID around that nested call so
// decoded SFX can be found without hashing memory the game may already reuse.
unsigned audio_sfx_context_push(unsigned id);
void audio_sfx_context_pop(unsigned previous_id);

#endif
