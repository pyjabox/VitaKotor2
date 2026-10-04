/* audio_ring.c -- see audio_ring.h for what this is and why it has no VitaSDK
 * dependency. */
#include <stddef.h>
#include "audio_ring.h"

void audio_ring_init(AudioRing *ring, int16_t *buf, unsigned cap_frames,
                     unsigned ch) {
  ring->buf  = buf;
  ring->cap  = cap_frames;
  ring->ch   = ch;
  ring->base = 0;
  ring->fill = 0;
  ring->eos  = 0;
}

void audio_ring_retire(AudioRing *ring, uint64_t consumed) {
  if (consumed <= ring->base) return;
  uint64_t drop = consumed - ring->base;
  if (drop >= (uint64_t)ring->fill) {
    ring->base += ring->fill;
    ring->fill  = 0;
  } else {
    ring->base += drop;
    ring->fill -= (unsigned)drop;
  }
}

void audio_ring_reset(AudioRing *ring) {
  ring->base = 0;
  ring->fill = 0;
  ring->eos  = 0;      /* a latched eos would refuse to decode the restart */
}

unsigned audio_ring_feed(AudioRing *ring, AudioRingFill fn, void *ctx,
                         unsigned budget) {
  unsigned produced = 0;
  while (ring->fill < ring->cap && budget && !ring->eos) {
    /* Frames are written strictly in order, so absolute frame f always lives at
     * f % cap. The only complication is the wrap: write at most as far as the
     * end of the buffer this time round and let the loop come back for the
     * remainder. */
    unsigned w      = (unsigned)((ring->base + ring->fill) % ring->cap);
    unsigned room   = ring->cap - ring->fill;
    unsigned contig = ring->cap - w;
    unsigned chunk  = room < contig ? room : contig;
    if (chunk > budget) chunk = budget;

    int eos = 0;
    unsigned got = fn(ctx, ring->buf + (size_t)w * ring->ch, chunk, &eos);
    if (got > chunk) got = chunk;      /* never trust a callback's own count */

    ring->fill += got;
    produced   += got;
    budget     -= got;

    if (eos) { ring->eos = 1; break; }
    if (!got) break;                   /* nothing available right now */
  }
  return produced;
}

int audio_ring_frame(const AudioRing *ring, uint64_t i, float *l, float *r) {
  if (i < ring->base || i >= ring->base + (uint64_t)ring->fill) return 0;
  const int16_t *p = ring->buf + (size_t)(i % ring->cap) * ring->ch;
  if (ring->ch == 1) { *l = (float)p[0]; *r = (float)p[0]; }
  else               { *l = (float)p[0]; *r = (float)p[1]; }
  return 1;
}
