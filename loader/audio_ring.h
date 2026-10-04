/* audio_ring.h -- a ring of decoded PCM frames, filled on demand.
 *
 * For assets too large to hold decoded: music is 14-24 MB of PCM against a heap
 * shared with the game, so the whole waveform cannot be kept. The ring holds a
 * window of it instead, refilled from the audio thread as the mixer drains it.
 *
 * Frames are addressed by ABSOLUTE index -- frame 0 is the first frame of the
 * asset, and stays frame 0 for the life of the stream even after it has been
 * retired out of the window. That is what lets a mixer channel keep a single
 * monotonic play position with no rebasing.
 *
 * Deliberately free of any VitaSDK dependency: the decoder arrives as a
 * callback, so this file compiles and is tested on the host. See
 * tools/test_audio_ring.c.
 */
#ifndef AUDIO_RING_H
#define AUDIO_RING_H

#include <stdint.h>

/* Decode up to `frames` frames into `dst` (interleaved, `ch` int16 per frame).
 * Returns frames actually produced; 0 means nothing is available right now.
 * Set *eos_out to 1 when the source is exhausted and will never produce more. */
typedef unsigned (*AudioRingFill)(void *ctx, int16_t *dst, unsigned frames,
                                  int *eos_out);

typedef struct {
  int16_t *buf;      /* cap * ch int16 units; owned by the caller, not by us */
  unsigned cap;      /* capacity in frames */
  unsigned ch;       /* interleaved channels per frame */
  uint64_t base;     /* absolute index of the oldest frame still in the window */
  unsigned fill;     /* how many valid frames follow base */
  int      eos;      /* the fill callback has reported exhaustion */
} AudioRing;

/* `buf` must hold cap_frames * ch int16 units and outlive the ring. */
void audio_ring_init(AudioRing *ring, int16_t *buf, unsigned cap_frames,
                     unsigned ch);

/* Drop every frame before absolute index `consumed`. Safe to call with a value
 * behind base (no-op) or beyond the window (drops everything).
 *
 * IMPORTANT: the window only ever moves FORWARD. A reader positioned behind
 * `base` can never be served again -- see audio_ring_reset. */
void audio_ring_retire(AudioRing *ring, uint64_t consumed);

/* Rewind the window to frame 0 and forget everything decoded.
 *
 * A ring serves exactly ONE sequential reader. That is not a stylistic
 * preference: with one decoder there is only one read position, and retiring by
 * the minimum of two readers freezes the window between them -- measured at
 * 100% starvation for the reader behind and 83% for the reader ahead, neither
 * recovering. So when playback restarts, the decoder is rewound and the window
 * reset together, rather than trying to serve the old and new positions at once. */
void audio_ring_reset(AudioRing *ring);

/* Top the window up, decoding at most `budget` frames this call. Returns frames
 * added. Bounding the budget is what stops one refill from overrunning the
 * audio thread's output deadline. */
unsigned audio_ring_feed(AudioRing *ring, AudioRingFill fn, void *ctx,
                         unsigned budget);

/* Fetch absolute frame `i` as two floats. Returns 0 if `i` is outside the
 * decoded window -- either retired already, or not decoded yet (an underrun).
 * Mono sources are duplicated to both outputs. */
int audio_ring_frame(const AudioRing *ring, uint64_t i, float *l, float *r);

#endif
