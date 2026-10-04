/* Host tests for the zero-copy Bink PCM queue. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bink_audio_queue.h"

static void test_capacity_and_clear(void) {
  BinkAudioQueue q;
  int16_t pcm[4] = { 1, 1, 2, 2 };
  bink_audio_queue_init(&q, 48000, 2, 2);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(!bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(q.count == 2 && q.high_water == 2);
  bink_audio_queue_clear(&q);
  assert(q.count == 0 && q.pos == 0.0);
  puts("ok capacity_and_clear");
}

static void test_exact_stereo_completion(void) {
  BinkAudioQueue q;
  int16_t pcm[] = { 100, -100, 200, -200, 300, -300, 400, -400 };
  int32_t out[8] = {0};
  bink_audio_queue_init(&q, 48000, 2, 4);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(bink_audio_queue_mix(&q, out, 4, 48000, 1.0f, 1.0f) == 1);
  for (unsigned i = 0; i < 8; i++) assert(out[i] == pcm[i]);
  assert(q.count == 0 && q.completed == 1 && q.mixed_frames == 4);
  puts("ok exact_stereo_completion");
}


static void test_left_only_stereo_is_centered(void) {
  BinkAudioQueue q;
  int16_t pcm[] = { 100, 0, -200, 0, 300, 0, -400, 0 };
  int32_t out[8] = {0};
  bink_audio_queue_init(&q, 48000, 2, 2);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(q.buffers[0].duplicate_left == 1);
  assert(bink_audio_queue_mix(&q, out, 4, 48000, 1.0f, 1.0f) == 1);
  for (unsigned i = 0; i < 4; i++) {
    assert(out[i * 2] == pcm[i * 2]);
    assert(out[i * 2 + 1] == pcm[i * 2]);
  }
  puts("ok left_only_stereo_is_centered");
}

static void test_real_stereo_is_preserved(void) {
  BinkAudioQueue q;
  int16_t pcm[] = { 100, 10, -200, -20, 300, 30, -400, -40 };
  int32_t out[8] = {0};
  bink_audio_queue_init(&q, 48000, 2, 2);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(q.buffers[0].duplicate_left == 0);
  assert(bink_audio_queue_mix(&q, out, 4, 48000, 1.0f, 1.0f) == 1);
  for (unsigned i = 0; i < 8; i++) assert(out[i] == pcm[i]);
  puts("ok real_stereo_is_preserved");
}

static void test_buffer_is_held_until_consumed(void) {
  BinkAudioQueue q;
  int16_t pcm[] = { 10, 20, 30, 40 };
  int32_t out[4] = {0};
  bink_audio_queue_init(&q, 48000, 1, 2);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(bink_audio_queue_mix(&q, out, 2, 48000, 1.0f, 1.0f) == 0);
  assert(q.count == 1);
  assert(bink_audio_queue_mix(&q, out, 2, 48000, 1.0f, 1.0f) == 1);
  assert(q.count == 0);
  puts("ok buffer_is_held_until_consumed");
}

static void test_completion_boundary(void) {
  BinkAudioQueue q;
  int16_t pcm[720 * 2] = {0};
  bink_audio_queue_init(&q, 44100, 2, 38);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  assert(bink_audio_queue_frames_to_completion(&q, 48000) == 784);
  int32_t out[784 * 2] = {0};
  assert(bink_audio_queue_mix(&q, out, 783, 48000, 1.0f, 1.0f) == 0);
  assert(bink_audio_queue_frames_to_completion(&q, 48000) == 1);
  assert(bink_audio_queue_mix(&q, out, 1, 48000, 1.0f, 1.0f) == 1);
  puts("ok completion_boundary");
}

static void test_all_slots_wrap_in_order(void) {
  BinkAudioQueue q;
  int16_t pcm[BINK_AUDIO_QUEUE_MAX][2];
  int32_t out[BINK_AUDIO_QUEUE_MAX * 4] = {0};
  bink_audio_queue_init(&q, 48000, 2, BINK_AUDIO_QUEUE_MAX);
  for (unsigned i = 0; i < BINK_AUDIO_QUEUE_MAX; i++) {
    pcm[i][0] = pcm[i][1] = (int16_t)i;
    assert(bink_audio_queue_enqueue(&q, pcm[i], sizeof pcm[i]));
  }
  assert(bink_audio_queue_mix(&q, out, BINK_AUDIO_QUEUE_MAX, 48000,
                              1.0f, 1.0f) == BINK_AUDIO_QUEUE_MAX);
  for (unsigned i = 0; i < BINK_AUDIO_QUEUE_MAX; i++)
    assert(out[i * 2] == (int32_t)i && out[i * 2 + 1] == (int32_t)i);
  assert(q.count == 0 && q.completed == BINK_AUDIO_QUEUE_MAX);
  puts("ok all_slots_wrap_in_order");
}

static void test_cross_buffer_interpolation(void) {
  BinkAudioQueue q;
  int16_t first[] = { 0, 1000 };
  int16_t second[] = { 2000, 3000 };
  int32_t out[10] = {0};
  bink_audio_queue_init(&q, 24000, 1, 2);
  assert(bink_audio_queue_enqueue(&q, first, sizeof first));
  assert(bink_audio_queue_enqueue(&q, second, sizeof second));
  assert(bink_audio_queue_mix(&q, out, 5, 48000, 1.0f, 1.0f) == 1);
  assert(out[0] == 0 && out[1] == 0);
  assert(out[2] == 500 && out[3] == 500);
  assert(out[4] == 1000 && out[5] == 1000);
  assert(out[6] == 1500 && out[7] == 1500);
  assert(out[8] == 2000 && out[9] == 2000);
  assert(q.count == 1);
  puts("ok cross_buffer_interpolation");
}

static void test_44100_resampling_and_underrun(void) {
  BinkAudioQueue q;
  int16_t pcm[20];
  int32_t out[24];
  for (unsigned i = 0; i < 10; i++) pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)i;
  memset(out, 0, sizeof out);
  bink_audio_queue_init(&q, 44100, 2, 2);
  assert(bink_audio_queue_enqueue(&q, pcm, sizeof pcm));
  unsigned done = bink_audio_queue_mix(&q, out, 12, 48000, 1.0f, 1.0f);
  assert(done == 1 && q.count == 0);
  assert(q.mixed_frames == 11 && q.underrun_frames == 1);
  puts("ok 44100_resampling_and_underrun");
}

int main(void) {
  test_capacity_and_clear();
  test_exact_stereo_completion();
  test_left_only_stereo_is_centered();
  test_real_stereo_is_preserved();
  test_buffer_is_held_until_consumed();
  test_completion_boundary();
  test_all_slots_wrap_in_order();
  test_cross_buffer_interpolation();
  test_44100_resampling_and_underrun();
  puts("\nall Bink audio queue tests passed");
  return 0;
}
