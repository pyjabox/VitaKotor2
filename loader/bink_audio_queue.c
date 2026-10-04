/* bink_audio_queue.c -- see bink_audio_queue.h. No VitaSDK dependencies. */
#include <stddef.h>
#include <string.h>

#include "bink_audio_queue.h"

void bink_audio_queue_init(BinkAudioQueue *queue, unsigned rate,
                           unsigned channels, unsigned capacity) {
  memset(queue, 0, sizeof *queue);
  queue->rate = rate;
  queue->channels = channels;
  queue->capacity = capacity > BINK_AUDIO_QUEUE_MAX ? BINK_AUDIO_QUEUE_MAX : capacity;
}

int bink_audio_queue_enqueue(BinkAudioQueue *queue, const void *pcm,
                             unsigned bytes) {
  if (!queue || !pcm || !bytes || !queue->capacity ||
      (queue->channels != 1 && queue->channels != 2) ||
      bytes % (queue->channels * sizeof(int16_t)) ||
      queue->count >= queue->capacity)
    return 0;

  unsigned rear = (queue->front + queue->count) % queue->capacity;
  queue->buffers[rear].pcm = (const int16_t *)pcm;
  queue->buffers[rear].bytes = bytes;
  queue->buffers[rear].duplicate_left = 0;
  if (queue->channels == 2) {
    const int16_t *samples = (const int16_t *)pcm;
    unsigned count = bytes / sizeof *samples;
    unsigned left_nonzero = 0, right_nonzero = 0;
    for (unsigned i = 0; i + 1 < count; i += 2) {
      left_nonzero |= samples[i] != 0;
      right_nonzero |= samples[i + 1] != 0;
    }
    /* KOTOR II advertises stereo to OpenSL but its embedded Bink provider
     * writes this movie's mono track only into the left lane. Center such
     * blocks by duplicating left during mixing; preserve genuine stereo. */
    queue->buffers[rear].duplicate_left = left_nonzero && !right_nonzero;
  }
  queue->count++;
  queue->enqueued++;
  if (queue->count > queue->high_water) queue->high_water = queue->count;
  return 1;
}

void bink_audio_queue_clear(BinkAudioQueue *queue) {
  if (!queue) return;
  queue->front = 0;
  queue->count = 0;
  queue->pos = 0.0;
}

static unsigned buffer_frames(const BinkAudioQueue *queue,
                              const BinkAudioBuffer *buffer) {
  return buffer->bytes / (queue->channels * sizeof(int16_t));
}

unsigned bink_audio_queue_frames_to_completion(const BinkAudioQueue *queue,
                                                unsigned out_rate) {
  if (!queue || !queue->count || !queue->rate || !out_rate) return 0;
  unsigned frames = buffer_frames(queue, &queue->buffers[queue->front]);
  if (!frames || queue->pos >= (double)frames) return 1;
  double remaining = (double)frames - queue->pos;
  double step = (double)queue->rate / (double)out_rate;
  unsigned output = (unsigned)(remaining / step);
  if ((double)output * step < remaining) output++;
  return output ? output : 1;
}

static void read_frame(const BinkAudioQueue *queue,
                       const BinkAudioBuffer *buffer, unsigned frame,
                       float *left, float *right) {
  const int16_t *pcm = buffer->pcm + (size_t)frame * queue->channels;
  *left = (float)pcm[0];
  *right = queue->channels == 1 || buffer->duplicate_left ?
             *left : (float)pcm[1];
}

static void pop_front(BinkAudioQueue *queue) {
  queue->buffers[queue->front].pcm = NULL;
  queue->buffers[queue->front].bytes = 0;
  queue->buffers[queue->front].duplicate_left = 0;
  queue->front = (queue->front + 1) % queue->capacity;
  queue->count--;
  queue->completed++;
}

unsigned bink_audio_queue_mix(BinkAudioQueue *queue, int32_t *acc,
                              unsigned out_frames, unsigned out_rate,
                              float gain_l, float gain_r) {
  if (!queue || !acc || !out_rate || !queue->rate || !out_frames) return 0;

  unsigned completed_before = (unsigned)queue->completed;
  const double step = (double)queue->rate / (double)out_rate;

  for (unsigned out = 0; out < out_frames; out++) {
    while (queue->count) {
      unsigned frames = buffer_frames(queue, &queue->buffers[queue->front]);
      if (frames && queue->pos < (double)frames) break;
      if (frames) queue->pos -= (double)frames;
      pop_front(queue);
    }

    if (!queue->count) {
      queue->underrun_frames += out_frames - out;
      break;
    }

    BinkAudioBuffer *current = &queue->buffers[queue->front];
    unsigned frames = buffer_frames(queue, current);
    unsigned frame = (unsigned)queue->pos;
    float l0, r0, l1, r1;
    read_frame(queue, current, frame, &l0, &r0);

    if (frame + 1 < frames) {
      read_frame(queue, current, frame + 1, &l1, &r1);
    } else if (queue->count > 1) {
      unsigned next = (queue->front + 1) % queue->capacity;
      read_frame(queue, &queue->buffers[next], 0, &l1, &r1);
    } else {
      l1 = l0;
      r1 = r0;
    }

    float frac = (float)(queue->pos - (double)frame);
    acc[out * 2]     += (int32_t)((l0 + (l1 - l0) * frac) * gain_l);
    acc[out * 2 + 1] += (int32_t)((r0 + (r1 - r0) * frac) * gain_r);
    queue->mixed_frames++;
    queue->pos += step;
  }

  while (queue->count) {
    unsigned frames = buffer_frames(queue, &queue->buffers[queue->front]);
    if (frames && queue->pos < (double)frames) break;
    if (frames) queue->pos -= (double)frames;
    pop_front(queue);
  }
  return (unsigned)queue->completed - completed_before;
}
