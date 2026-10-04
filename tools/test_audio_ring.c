/* test_audio_ring.c -- host tests for the PCM ring. Not built into the VPK.
 *
 * Build and run:
 *   gcc -std=c99 -Wall -Wextra -Werror -I loader \
 *       -o /tmp/test_audio_ring tools/test_audio_ring.c loader/audio_ring.c \
 *   && /tmp/test_audio_ring
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_ring.h"

/* A synthetic source: frame N holds the sample value N, so any frame read back
 * proves both that the data survived the wrap and that it landed at the right
 * absolute index. */
typedef struct {
  uint64_t next;      /* absolute index of the next frame to produce */
  uint64_t total;     /* frames before the source is exhausted */
  unsigned per_call;  /* cap on frames produced per call, 0 = no cap */
  unsigned ch;
  unsigned calls;
} FakeSrc;

static unsigned fake_fill(void *ctx, int16_t *dst, unsigned frames, int *eos_out) {
  FakeSrc *s = (FakeSrc *)ctx;
  s->calls++;
  if (s->next >= s->total) { *eos_out = 1; return 0; }
  unsigned want = frames;
  if (s->per_call && want > s->per_call) want = s->per_call;
  if ((uint64_t)want > s->total - s->next) want = (unsigned)(s->total - s->next);
  for (unsigned i = 0; i < want; i++)
    for (unsigned c = 0; c < s->ch; c++)
      dst[i * s->ch + c] = (int16_t)((s->next + i) & 0x7fff);
  s->next += want;
  if (s->next >= s->total) *eos_out = 1;
  return want;
}

static void test_init_is_empty(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  assert(r.base == 0 && r.fill == 0 && r.eos == 0);
  float l, rr;
  assert(audio_ring_frame(&r, 0, &l, &rr) == 0);
  printf("ok init_is_empty\n");
}

static void test_feed_fills_and_reads_back(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 8);
  assert(r.base == 0 && r.fill == 8);
  for (unsigned i = 0; i < 8; i++) {
    float l, rr;
    assert(audio_ring_frame(&r, i, &l, &rr) == 1);
    assert(l == (float)i && rr == (float)i);
  }
  float l, rr;
  assert(audio_ring_frame(&r, 8, &l, &rr) == 0);   /* not decoded yet */
  printf("ok feed_fills_and_reads_back\n");
}

static void test_budget_bounds_one_feed(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 10);
  assert(got == 10 && r.fill == 10);
  printf("ok budget_bounds_one_feed\n");
}

static void test_retire_advances_base(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 3);
  assert(r.base == 3 && r.fill == 5);
  float l, rr;
  assert(audio_ring_frame(&r, 2, &l, &rr) == 0);   /* retired */
  assert(audio_ring_frame(&r, 3, &l, &rr) == 1 && l == 3.0f);
  printf("ok retire_advances_base\n");
}

static void test_retire_behind_base_is_noop(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 5);
  audio_ring_retire(&r, 2);                         /* behind base */
  assert(r.base == 5 && r.fill == 3);
  printf("ok retire_behind_base_is_noop\n");
}

static void test_retire_past_window_empties(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 999);
  assert(r.fill == 0 && r.base == 8);
  printf("ok retire_past_window_empties\n");
}

/* The one that matters: data written across the wrap must read back at the
 * right absolute index. */
static void test_wraparound(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);           /* frames 0..7 */
  audio_ring_retire(&r, 6);                         /* keep 6,7 */
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 6);                                 /* frames 8..13 */
  assert(r.base == 6 && r.fill == 8);
  for (uint64_t i = 6; i < 14; i++) {
    float l, rr;
    assert(audio_ring_frame(&r, i, &l, &rr) == 1);
    assert(l == (float)i);
  }
  printf("ok wraparound\n");
}

static void test_stereo_interleave(void) {
  int16_t buf[8 * 2];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 2);
  FakeSrc s = { 0, 1000, 0, 2, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  float l, rr;
  assert(audio_ring_frame(&r, 4, &l, &rr) == 1);
  assert(l == 4.0f && rr == 4.0f);
  printf("ok stereo_interleave\n");
}

static void test_eos_is_sticky_and_stops_feeding(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 5, 0, 1, 0 };                    /* only 5 frames exist */
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 5 && r.eos == 1 && r.fill == 5);
  unsigned calls = s.calls;
  got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 0);
  assert(s.calls == calls);                         /* did not call again */
  printf("ok eos_is_sticky_and_stops_feeding\n");
}

/* eos must NOT mean "nothing left to play": the window can still be full. This
 * is what stops the last 0.7s being cut off every track. */
static void test_eos_leaves_window_readable(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 5, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  assert(r.eos == 1);
  for (unsigned i = 0; i < 5; i++) {
    float l, rr;
    assert(audio_ring_frame(&r, i, &l, &rr) == 1);
  }
  printf("ok eos_leaves_window_readable\n");
}

/* A source that returns 0 without eos means "nothing right now" -- feed must
 * give up for this call rather than spin. */
static void test_short_source_does_not_spin(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 1000, 4, 1, 0 };                 /* 4 frames per call */
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 64);                                /* still fills, in pieces */
  assert(s.calls == 16);                            /* 64 / 4, no spinning */
  assert(r.fill == 64);
  printf("ok short_source_does_not_spin\n");
}

/* The window only moves forward, so a reader behind `base` is starved for good.
 * This is why a stream serves one reader and restarts go through reset. */
static void test_reader_behind_base_is_starved(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 100000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 500);                       /* reader ran on to 500 */
  /* retire clamps to base+fill, so the window does not leap to 500 -- it
   * crawls, one feed at a time, and a reader left at 0 is behind it for good. */
  assert(r.base == 64 && r.fill == 0);
  audio_ring_feed(&r, fake_fill, &s, 64);
  float l, rr;
  assert(audio_ring_frame(&r, 0, &l, &rr) == 0);    /* a restart at 0 is lost */
  assert(r.base > 0);
  printf("ok reader_behind_base_is_starved\n");
}

/* reset is the recovery: window back to 0, nothing decoded, ready to refill. */
static void test_reset_rewinds_the_window(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 100000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 500);
  audio_ring_feed(&r, fake_fill, &s, 64);
  assert(r.base > 0);

  audio_ring_reset(&r);
  assert(r.base == 0 && r.fill == 0 && r.eos == 0);

  FakeSrc s2 = { 0, 100000, 0, 1, 0 };              /* decoder rewound too */
  unsigned got = audio_ring_feed(&r, fake_fill, &s2, 64);
  assert(got == 64);
  float l, rr;
  assert(audio_ring_frame(&r, 0, &l, &rr) == 1 && l == 0.0f);
  printf("ok reset_rewinds_the_window\n");
}

/* reset must clear a latched eos, or a restarted track refuses to decode. */
static void test_reset_clears_eos(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 5, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  assert(r.eos == 1);
  audio_ring_reset(&r);
  assert(r.eos == 0);
  FakeSrc s2 = { 0, 5, 0, 1, 0 };
  assert(audio_ring_feed(&r, fake_fill, &s2, 64) == 5);
  printf("ok reset_clears_eos\n");
}

int main(void) {
  test_init_is_empty();
  test_feed_fills_and_reads_back();
  test_budget_bounds_one_feed();
  test_retire_advances_base();
  test_retire_behind_base_is_noop();
  test_retire_past_window_empties();
  test_wraparound();
  test_stereo_interleave();
  test_eos_is_sticky_and_stops_feeding();
  test_eos_leaves_window_readable();
  test_short_source_does_not_spin();
  test_reader_behind_base_is_starved();
  test_reset_rewinds_the_window();
  test_reset_clears_eos();
  printf("\nall audio_ring tests passed\n");
  return 0;
}
