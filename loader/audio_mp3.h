/* audio_mp3.h -- decode KOTOR's MP3-in-a-.wav assets to PCM (see audio_mp3.c) */

#ifndef __AUDIO_MP3_H__
#define __AUDIO_MP3_H__

#include <stdint.h>

typedef struct {
  int16_t *pcm;        // interleaved 16-bit, `channels` per frame (malloc'd)
  unsigned nsamples;   // frames (per channel), NOT total int16 count
  unsigned channels;
  unsigned rate;       // Hz, from the MPEG frame header (not the fake RIFF)
  unsigned ms;         // duration, what FMOD::Sound::getLength must report
} AudioPcm;

/* Concurrent sceAudiodec MP3 handles the library is initialised with. This is a
 * HARD POOL -- CreateDecoder fails 0x807F0007 past it -- and it is fixed at the
 * first InitLibrary call, so it must cover every decoder alive at once:
 * AUDIO_MP3_STREAM_MAX long-lived streaming decoders, plus spares for the short
 * synchronous decodes (VO and effects) that create and delete one per call.
 *
 * This went unnoticed for months at totalStreams = 1, because the whole-asset
 * path creates a decoder, decodes, and deletes it before returning -- never two
 * alive. Streaming holds one open for the life of a track, and with a pool of 1
 * the first music track took the only slot and every later decode failed: voices
 * became substituted silence and dialogue raced, because the engine's
 * IsPlaying() then reports nothing playing.
 *
 * STREAM_MAX must stay strictly below DECODER_POOL so a spare always exists.
 * SDK ceiling is SCE_AUDIODEC_MP3_MAX_NSTREAMS (6). */
#define AUDIO_MP3_DECODER_POOL 4
#define AUDIO_MP3_STREAM_MAX   2

// One-time hardware decoder library init. Safe to call repeatedly.
int  audio_mp3_init_library(void);

// Decode a whole in-memory asset. Handles real RIFF/WAVE PCM, the 58-byte fake
// RIFF header, ID3v2 tags, and raw MP3. Returns 1 and fills `out` (caller owns
// out->pcm), else 0.
int  audio_mp3_decode(const void *data, unsigned len, AudioPcm *out);

// Header-only: fill rate/channels/nsamples/ms WITHOUT decoding or allocating
// (out->pcm stays NULL). Duration is estimated from the frame bitrate. Used to
// build a correctly-timed silent placeholder when a stream is too large to hold
// in RAM, so the game's pacing stays right instead of retrying forever.
int  audio_mp3_probe(const void *data, unsigned len, AudioPcm *out);

void audio_pcm_free(AudioPcm *p);

/* ---- incremental decoding -------------------------------------------------
 * For assets too large to hold decoded (music is ~15 MB of PCM). The decoder
 * stays open and frames are pulled on demand, so the cost is the compressed
 * bytes plus a small ring instead of the whole waveform, and starting a track
 * costs no decode stall.
 *
 * The caller owns `data` and MUST keep it alive and unmoved until
 * audio_mp3_stream_close -- sceAudiodec reads the elementary stream in place. */
typedef struct AudioMp3Stream AudioMp3Stream;

/* Open over an in-memory MP3 asset without decoding it. `fmt` (optional)
 * receives rate/channels/nsamples/ms exactly as the whole-asset path would
 * report them, with fmt->pcm NULL -- the game reads getLength for its own
 * pacing, so a stream must not report a different length than a decoded copy of
 * the same asset. Returns NULL if the asset is not decodable. */
AudioMp3Stream *audio_mp3_stream_open(const void *data, unsigned len, AudioPcm *fmt);

/* 1 if streaming this asset would take one of the AUDIO_MP3_DECODER_POOL
 * hardware handles. The ambient beds are IMA ADPCM behind a 470-byte junk-MP3
 * prefix and decode in software, so they must NOT count against the pool --
 * Lower City runs an ADPCM bed and an MP3 track at the same time, and with a
 * shared cap of 2 the second one would be refused. Cheap: a header sniff. */
int  audio_mp3_stream_needs_hw(const void *data, unsigned len);

/* Decode up to `frames` frames into `dst` (interleaved, fmt->channels per
 * frame). Returns frames actually produced; short or 0 at end of stream. */
unsigned audio_mp3_stream_read(AudioMp3Stream *s, int16_t *dst, unsigned frames);

/* True once the stream is exhausted and nothing is left buffered. */
int  audio_mp3_stream_eos(const AudioMp3Stream *s);

/* Restart from the first frame, for looping music. */
void audio_mp3_stream_rewind(AudioMp3Stream *s);

void audio_mp3_stream_close(AudioMp3Stream *s);

#endif
