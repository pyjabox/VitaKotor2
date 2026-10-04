/* bink_audio_queue.h -- bounded zero-copy PCM queue for Bink movie audio. */
#ifndef BINK_AUDIO_QUEUE_H
#define BINK_AUDIO_QUEUE_H

#include <stdint.h>

#define BINK_AUDIO_QUEUE_MAX 38

typedef struct {
  const int16_t *pcm;
  unsigned bytes;
  unsigned duplicate_left;
} BinkAudioBuffer;

typedef struct {
  BinkAudioBuffer buffers[BINK_AUDIO_QUEUE_MAX];
  unsigned front, count, capacity;
  unsigned rate, channels;
  double pos;
  uint64_t enqueued, completed, mixed_frames, underrun_frames;
  unsigned high_water;
} BinkAudioQueue;

void bink_audio_queue_init(BinkAudioQueue *queue, unsigned rate,
                           unsigned channels, unsigned capacity);
int bink_audio_queue_enqueue(BinkAudioQueue *queue, const void *pcm,
                             unsigned bytes);
void bink_audio_queue_clear(BinkAudioQueue *queue);

/* Output frames that will consume the current source buffer at `out_rate`.
 * Returns 0 when the queue is empty. */
unsigned bink_audio_queue_frames_to_completion(const BinkAudioQueue *queue,
                                                unsigned out_rate);

/* Add `out_frames` stereo frames to `acc`. The caller owns synchronization.
 * Returns the number of source buffers fully consumed during this call. */
unsigned bink_audio_queue_mix(BinkAudioQueue *queue, int32_t *acc,
                              unsigned out_frames, unsigned out_rate,
                              float gain_l, float gain_r);

#endif
