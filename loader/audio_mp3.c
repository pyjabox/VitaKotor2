/* audio_mp3.c -- KOTOR audio decode: MP3 -> PCM via the Vita hardware decoder
 * Historical format measurements below are inherited from VitaKotor (KOTOR I)
 * and are not KOTOR II validation.
 *
 *   - 91% carry a 58-byte fake RIFF header (declares PCM/8-bit/22050 and a
 *     `data` chunk of size 0 -- all lies) with the real MP3 starting at byte 58.
 *   - 9% are raw MP3 from byte 0, no header at all.
 *   - Every file sampled: 48 kbps, MONO. Rates are 32000 Hz (with the header)
 *     or 22050 Hz (raw). Both MPEG Layer III.
 *
 * So we sniff rather than blindly skipping 58 bytes, and we never trust the RIFF
 * fields -- the MPEG frame header is the only authority for rate and channels.
 *
 * THAT SAMPLE MISSED A THIRD CONTAINER, and it is the one the ambience uses.
 * A full census of the OBB (log167 follow-up) finds 51 of the 119 STREAMMUSIC
 * assets -- every environmental bed -- laid out the other way round:
 *
 *     bytes 0..469   a fake 3-frame LAME3.93 MPEG-2 header, padded with 0x55
 *     byte  470      a REAL RIFF/WAVE, almost always IMA ADPCM 4-bit,
 *                    44100 Hz stereo, blockAlign 2048, 2041 frames per block
 *
 * It is the mirror image of the 58-byte case: there a fake RIFF wraps real MP3,
 * here a fake MP3 wraps a real RIFF. (STREAMSOUNDS uses the same 470-byte
 * prefix, which is why the companion opens those files at offset 470 -- but for
 * STREAMMUSIC it hands us the whole file from byte 0 and leaves the sniffing to
 * us.) Getting this wrong was not subtle: find_sync locked onto the fake LAME
 * frame, the bitrate identity turned a 46-second loop into a fictional 338691 ms
 * MP3, the streaming decoder emitted the 3 junk frames (1728 samples, 78 ms) and
 * hit EOS -- so every ambient bed in the game "finished" after 78 ms and the
 * engine restarted it ten times a second, forever. log167 caught 1256 of those
 * phantom finishes.
 *
 * So: look for RIFF at 0 AND at 470, and believe a `data` chunk with a real
 * size. A `data` size of 0 still means the fake header, and still falls through
 * to MP3 -- that rule is what keeps the 58-byte case working.
 *
 * Decoding runs on sceAudiodec (hardware). We do NOT hand-compute frame lengths:
 * sceAudiodecDecode reports how much elementary stream it actually consumed in
 * ctrl.inputEsSize, so we just advance by that. It is both simpler and correct
 * across MPEG1/2/2.5, which differ in frame-size formula and samples-per-frame.
 */

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>

#include "audio_mp3.h"
#include "minimp3.h"
#include "log.h"
#include "bigalloc.h"

#define MP3_MAX_SAMPLES 1152                 /* MPEG1 Layer III granule pair */
#define MP3_MAX_PCM     (MP3_MAX_SAMPLES * 2 * 2)   /* stereo, 16-bit */

static int g_lib_ready = 0;

int audio_mp3_init_library(void) {
  if (g_lib_ready) return 1;
  SceAudiodecInitParam p;
  memset(&p, 0, sizeof p);
  p.mp3.size = sizeof(SceAudiodecInitStreamParam);
  /* A hard pool, not a hint, and fixed for the life of the process: this runs
   * once and a second call returns 0x807F0002 ("already initialised") without
   * changing the value. See the constant's comment in audio_mp3.h for what
   * getting it wrong did to dialogue. */
  p.mp3.totalStreams = AUDIO_MP3_DECODER_POOL;
  int r = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &p);
  // 0x807F0002 = already initialised; treat as success.
  if (r < 0 && (unsigned)r != 0x807F0002u) {
    log_printf("[snd] sceAudiodecInitLibrary(MP3) failed 0x%08X", (unsigned)r);
    return 0;
  }
  g_lib_ready = 1;
  log_printf("[snd] MP3 decoder library ready");
  return 1;
}

static int mp3_frame_header(const unsigned char *d, unsigned len,
                            unsigned *frame_len, unsigned *signature) {
  static const unsigned br1[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
  static const unsigned br2[16] = { 0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0 };
  static const unsigned rates[4][3] = {
    { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 }
  };
  if (len < 4 || d[0] != 0xFF || (d[1] & 0xE0) != 0xE0) return 0;
  unsigned ver = (d[1] >> 3) & 3, layer = (d[1] >> 1) & 3;
  unsigned bri = (d[2] >> 4) & 0xF, sri = (d[2] >> 2) & 3;
  if (ver == 1 || layer != 1 || bri == 0 || bri == 0xF || sri == 3) return 0;
  unsigned rate = rates[ver][sri];
  unsigned kbps = (ver == 3) ? br1[bri] : br2[bri];
  unsigned bytes = ((ver == 3 ? 144000u : 72000u) * kbps) / rate + ((d[2] >> 1) & 1);
  if (bytes < 4 || bytes > len) return 0;
  if (frame_len) *frame_len = bytes;
  if (signature) *signature = (ver << 8) | sri;
  return 1;
}

/* Locate the first MPEG audio sync word, skipping a fake RIFF header and/or an
 * ID3v2 tag. Nontrivial buffers require two consecutive compatible frames: a
 * reused SFX buffer can contain a random sync-like word, and feeding that false
 * positive to sceAudiodec costs 100+ ms before failing. Tiny UI sounds may
 * legitimately contain only one frame. Returns the byte offset, or -1. */
static int find_sync(const unsigned char *d, unsigned len) {
  unsigned start = 0;
  if (len > 10 && !memcmp(d, "ID3", 3)) {
    /* ID3v2 size is 4 syncsafe bytes (7 bits each) after a 10-byte header. */
    start = 10 + (((unsigned)d[6] << 21) | ((unsigned)d[7] << 14) |
                  ((unsigned)d[8] << 7)  |  (unsigned)d[9]);
  } else if (len > 12 && !memcmp(d, "RIFF", 4)) {
    start = 58;                              /* KOTOR's fixed fake header */
  }
  if (start >= len) start = 0;
  /* Scan forward for a plausible frame header and, where possible, its next. */
  for (unsigned i = start; i + 4 <= len && i < start + 8192; i++) {
    unsigned frame_len = 0, sig = 0;
    if (!mp3_frame_header(d + i, len - i, &frame_len, &sig)) continue;
    if (len <= 512) return (int)i;
    unsigned next_sig = 0;
    if (i + frame_len + 4 <= len &&
        mp3_frame_header(d + i + frame_len, len - i - frame_len, NULL, &next_sig) &&
        next_sig == sig)
      return (int)i;
  }
  return -1;
}

/* MPEG version field -> sample rate table index, and the value sceAudiodec wants
 * in SceAudiodecInfoMp3.version (MPEG1 = 3, MPEG2 = 2, MPEG2.5 = 0). */
static unsigned rate_of(unsigned ver, unsigned sri) {
  static const unsigned r[4][3] = {
    { 11025, 12000,  8000 },   /* 0: MPEG 2.5 */
    {     0,     0,     0 },   /* 1: reserved */
    { 22050, 24000, 16000 },   /* 2: MPEG 2   */
    { 44100, 48000, 32000 },   /* 3: MPEG 1   */
  };
  return (ver < 4 && sri < 3) ? r[ver][sri] : 0;
}

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

/* IMA/DVI ADPCM (WAVE format tag 17). Only 4 of the 1928 sounds in sounds.bzf
 * use it, but they are common action effects, so rejecting the tag left audible
 * gaps. Layout confirmed offline: mono, 44100 Hz, blockAlign 1024, 4 bits,
 * 2041 samples per block -- i.e. a 4-byte per-channel block header (initial
 * predictor int16 + step index) followed by 2 samples per byte. */
static const int ima_step[89] = {
      7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
     19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
     50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
   2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
   5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ima_idx[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

static inline int ima_next(int nib, int *pred, int *idx) {
  int step = ima_step[*idx], diff = step >> 3;
  if (nib & 1) diff += step >> 2;
  if (nib & 2) diff += step >> 1;
  if (nib & 4) diff += step;
  if (nib & 8) diff = -diff;
  int p = *pred + diff;
  if (p >  32767) p =  32767;
  if (p < -32768) p = -32768;
  *pred = p;
  *idx += ima_idx[nib & 15];
  if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
  return p;
}

/* Decode ONE IMA ADPCM block into `dst`. Returns frames written.
 *
 * Each block carries its own predictor and step index in its first 4 bytes per
 * channel, so blocks are entirely independent of one another. That is the whole
 * reason streaming this format is easy: any block can be decoded on its own, a
 * rewind is just "go back to block 0", and there is no inter-block state to
 * carry -- unlike MP3, where the bit reservoir spans frames. */
static unsigned adpcm_block(const unsigned char *blk, unsigned blen, unsigned ch,
                            int16_t *dst, unsigned cap_frames) {
  if (blen < 4 * ch || !cap_frames) return 0;
  int pred[2] = {0, 0}, idx[2] = {0, 0};
  unsigned f = 0, pos = 4 * ch;

  for (unsigned c = 0; c < ch; c++) {
    pred[c] = (int16_t)rd16(blk + c * 4);
    idx[c]  = blk[c * 4 + 2];
    if (idx[c] > 88) idx[c] = 88;
  }
  for (unsigned c = 0; c < ch; c++) dst[c] = (int16_t)pred[c];  /* header seeds frame 0 */
  f = 1;

  if (ch == 1) {                        /* nibbles run low-then-high, in order */
    for (; pos < blen && f < cap_frames; pos++) {
      dst[f++] = (int16_t)ima_next(blk[pos] & 15, &pred[0], &idx[0]);
      if (f < cap_frames)
        dst[f++] = (int16_t)ima_next(blk[pos] >> 4, &pred[0], &idx[0]);
    }
    return f;
  }

  /* Stereo is NOT interleaved per sample. The data is 4-byte groups that
   * ALTERNATE channels, each group holding 8 consecutive samples of one
   * channel, so a pair of groups yields 8 interleaved frames.
   *
   * Emitting the nibbles in the order they appear -- which is what this did
   * before the ambient beds started using it -- writes eight left samples into
   * the space where L R L R belongs. Every output frame then takes two adjacent
   * samples of the SAME channel: half the frames, so the track plays an octave
   * high with the stereo image shredded. It went unnoticed because the only
   * ADPCM assets in use were four mono effects, and the mono path above is
   * right. Verified against a reference decoder in tools/. */
  int16_t l[8], r[8];
  while (pos + 8 <= blen && f < cap_frames) {
    for (unsigned j = 0; j < 4; j++) {
      unsigned b = blk[pos + j];
      l[j * 2]     = (int16_t)ima_next(b & 15, &pred[0], &idx[0]);
      l[j * 2 + 1] = (int16_t)ima_next(b >> 4,  &pred[0], &idx[0]);
    }
    for (unsigned j = 0; j < 4; j++) {
      unsigned b = blk[pos + 4 + j];
      r[j * 2]     = (int16_t)ima_next(b & 15, &pred[1], &idx[1]);
      r[j * 2 + 1] = (int16_t)ima_next(b >> 4,  &pred[1], &idx[1]);
    }
    pos += 8;
    for (unsigned j = 0; j < 8 && f < cap_frames; j++, f++) {
      dst[f * 2]     = l[j];
      dst[f * 2 + 1] = r[j];
    }
  }
  return f;
}

static int adpcm_ima_decode(const unsigned char *d, unsigned len, unsigned ch,
                            unsigned rate, unsigned balign, AudioPcm *out) {
  if ((ch != 1 && ch != 2) || !rate) return 0;
  if (balign < 4 * ch) balign = 1024;

  unsigned nblocks = len / balign;
  unsigned tail    = len % balign;
  if (!nblocks && tail < 4 * ch) return 0;

  /* samples per full block: one seeded by the header, then 2 per byte */
  unsigned per_block = 1 + ((balign - 4 * ch) * 2) / ch;
  unsigned frames    = nblocks * per_block;
  if (tail > 4 * ch) frames += 1 + ((tail - 4 * ch) * 2) / ch;
  if (!frames) return 0;

  int16_t *pcm = (int16_t *)big_malloc((size_t)frames * ch * sizeof(int16_t));
  if (!pcm) { log_printf("[snd] ADPCM out of memory (%u frames)", frames); return 0; }

  unsigned w = 0;
  for (unsigned b = 0; b * balign < len && w < frames; b++) {
    const unsigned char *blk = d + b * balign;
    unsigned blen = len - b * balign;
    if (blen > balign) blen = balign;
    w += adpcm_block(blk, blen, ch, pcm + (size_t)w * ch, frames - w);
  }

  out->pcm      = pcm;
  out->nsamples = w;
  out->channels = ch;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)out->nsamples * 1000u / rate);
  return out->nsamples ? 1 : 0;
}

/* Real RIFF/WAVE PCM -- a SECOND, different asset format.
 *
 * Verified offline 2026-07-31 by LZMA-decompressing data/sounds.bzf out of the
 * OBB: the in-memory SFX resources (gui_click, gui_scroll, ...) are genuine
 * 16-bit MONO PCM at 22050 Hz. They are NOT MP3, and feeding them to the MP3
 * decoder produced "no sync" / "decoded 0 samples" for every sound effect.
 *
 * Only streamwaves/streammusic use the MP3-in-a-fake-RIFF form above. The two
 * are told apart by the `data` chunk: the fake header declares size 0, a real
 * one declares the true byte count. So: real data chunk -> PCM, else fall
 * through to the MP3 sniffer.
 */
/* KOTOR's fixed junk-MP3 prefix. The companion itself uses this constant: it
 * opens every STREAMSOUNDS asset at offset 470. For STREAMMUSIC it passes the
 * whole file from 0 and leaves the sniffing to us. */
#define WAV_JUNK_PREFIX 470

/* Where the real RIFF starts, or -1 if there is not one. */
static int wav_riff_at(const unsigned char *d, unsigned len) {
  /* FModAudioSystem owns the first eight bytes of some transient SFX buffers.
   * They can be reused while the standard WAVE signature and chunks at +8 stay
   * intact. RIFF size is not used by the parser, so WAVE is sufficient. */
  if (len >= 44 && !memcmp(d + 8, "WAVE", 4)) return 0;
  if (len >= WAV_JUNK_PREFIX + 44 &&
      !memcmp(d + WAV_JUNK_PREFIX, "RIFF", 4) &&
      !memcmp(d + WAV_JUNK_PREFIX + 8, "WAVE", 4)) return WAV_JUNK_PREFIX;
  return -1;
}

/* Everything the two RIFF forms need, read from the header alone. */
typedef struct {
  unsigned afmt, ch, rate, bits, balign;
  unsigned data_off, data_len;             /* absolute, into the caller's buffer */
  unsigned per_block;                      /* ADPCM frames per block, else 0 */
  unsigned frames;
} WavInfo;

/* Header-only parse. Returns 1 only for a container we can actually decode --
 * a `data` chunk with a real size and a format we implement. A size of 0 is the
 * fake header wrapping MP3, and must fail here so the MP3 sniffer gets it. */
static int wav_header(const unsigned char *d, unsigned len, WavInfo *w) {
  int base = wav_riff_at(d, len);
  if (base < 0) return 0;

  memset(w, 0, sizeof *w);
  unsigned pos = (unsigned)base + 12, have_fmt = 0;
  while (pos + 8 <= len) {
    unsigned csz = rd32(d + pos + 4);
    unsigned body = pos + 8;
    unsigned avail = len - body;
    if (csz > avail) csz = avail;                  /* tolerate a short tail */

    if (!memcmp(d + pos, "fmt ", 4) && csz >= 16) {
      w->afmt = rd16(d + body);      w->ch   = rd16(d + body + 2);
      w->rate = rd32(d + body + 4);  w->balign = rd16(d + body + 12);
      w->bits = rd16(d + body + 14);
      have_fmt = 1;
    } else if (!memcmp(d + pos, "data", 4)) {
      if (!have_fmt || csz == 0) return 0;
      w->data_off = body;
      w->data_len = csz;
      if (w->ch != 1 && w->ch != 2) return 0;
      if (!w->rate) return 0;
      if (w->afmt == 17) {                          /* IMA ADPCM, 4-bit */
        if (w->balign < 4 * w->ch) w->balign = 1024;
        w->per_block = 1 + ((w->balign - 4 * w->ch) * 2) / w->ch;
        unsigned nblocks = csz / w->balign, tail = csz % w->balign;
        w->frames = nblocks * w->per_block;
        if (tail > 4 * w->ch) w->frames += 1 + ((tail - 4 * w->ch) * 2) / w->ch;
      } else if (w->afmt == 1 && (w->bits == 8 || w->bits == 16)) {
        w->frames = csz / (w->ch * (w->bits / 8));
      } else {
        return 0;
      }
      return w->frames ? 1 : 0;
    }
    pos += 8 + csz + (csz & 1);                    /* chunks are word-aligned */
  }
  return 0;
}

static int wav_try_pcm(const unsigned char *d, unsigned len, AudioPcm *out) {
  WavInfo w;
  if (!wav_header(d, len, &w)) return 0;

  const unsigned char *body = d + w.data_off;
  unsigned ch = w.ch, rate = w.rate, bits = w.bits, frames = w.frames;

  if (w.afmt == 17)
    return adpcm_ima_decode(body, w.data_len, ch, rate, w.balign, out);

  int16_t *pcm = (int16_t *)big_malloc((size_t)frames * ch * sizeof(int16_t));
  if (!pcm) return 0;

  if (bits == 16) {
    memcpy(pcm, body, (size_t)frames * ch * sizeof(int16_t));
  } else {
    /* 8-bit WAV is unsigned, centred on 128 */
    for (unsigned i = 0; i < frames * ch; i++)
      pcm[i] = (int16_t)(((int)body[i] - 128) << 8);
  }

  out->pcm      = pcm;
  out->nsamples = frames;
  out->channels = ch;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)frames * 1000u / rate);
  /* Throttled: this fired 2569 times in log110 and the log I/O alone hurt. */
  static unsigned nlog = 0;
  if (nlog++ < 24)
    log_printf("[snd] RIFF PCM %u Hz %u ch %u-bit -> %u frames (%u ms)",
               rate, ch, bits, frames, out->ms);
  return 1;
}

/* kbps by bitrate index, Layer III. Index 0 (free) and 15 (bad) are rejected by
 * find_sync, so 0 entries there are never selected. */
static const unsigned br_mpeg1[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
static const unsigned br_mpeg2[16] = { 0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0 };

int audio_mp3_probe(const void *data, unsigned len, AudioPcm *out) {
  memset(out, 0, sizeof *out);
  if (!data || len < 16) return 0;

  const unsigned char *d = (const unsigned char *)data;

  /* A real RIFF answers exactly, from its header -- no estimate and, since the
   * ADPCM beds are 8-25 MB decoded, no decoding either. (This used to call
   * wav_try_pcm and free the result, which meant "how long is this?" cost a full
   * decode of the largest assets in the game, on the game thread, before we had
   * even decided whether to stream it.) */
  WavInfo w;
  if (wav_header(d, len, &w)) {
    out->pcm      = NULL;
    out->nsamples = w.frames;
    out->channels = w.ch;
    out->rate     = w.rate;
    out->ms       = (unsigned)((uint64_t)w.frames * 1000u / w.rate);
    return 1;
  }

  int off = find_sync(d, len);
  if (off < 0) return 0;

  unsigned ver  = (d[off + 1] >> 3) & 3;
  unsigned bri  = (d[off + 2] >> 4) & 0xF;
  unsigned sri  = (d[off + 2] >> 2) & 3;
  unsigned cmod = (d[off + 3] >> 6) & 3;
  unsigned rate = rate_of(ver, sri);
  unsigned kbps = (ver == 3) ? br_mpeg1[bri] : br_mpeg2[bri];
  if (!rate || !kbps) return 0;

  unsigned es = len - (unsigned)off;
  out->pcm      = NULL;                       /* silence: no allocation at all */
  out->channels = (cmod == 3) ? 1 : 2;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)es * 8u / kbps);   /* bytes*8/kbps == ms */
  out->nsamples = (unsigned)((uint64_t)out->ms * rate / 1000u);
  return out->nsamples ? 1 : 0;
}

int audio_mp3_decode(const void *data, unsigned len, AudioPcm *out) {
  memset(out, 0, sizeof *out);
  if (!data || len < 16) return 0;

  const unsigned char *d = (const unsigned char *)data;

  /* Uncompressed PCM needs no hardware decoder, so try it before init. */
  if (wav_try_pcm(d, len, out)) return 1;

  if (!audio_mp3_init_library()) return 0;
  int off = find_sync(d, len);
  if (off < 0) {
    log_printf("[snd] not MPEG audio (no sync in first 8KB)");
    return 0;
  }

  unsigned ver  = (d[off + 1] >> 3) & 3;
  unsigned bri  = (d[off + 2] >> 4) & 0xF;
  unsigned sri  = (d[off + 2] >> 2) & 3;
  unsigned cmod = (d[off + 3] >> 6) & 3;
  unsigned ch   = (cmod == 3) ? 1 : 2;       /* 3 = single channel */
  unsigned rate = rate_of(ver, sri);
  if (!rate) return 0;

  SceAudiodecCtrl ctrl;
  SceAudiodecInfo info;
  memset(&ctrl, 0, sizeof ctrl);
  memset(&info, 0, sizeof info);
  info.mp3.size    = sizeof(SceAudiodecInfoMp3);
  info.mp3.ch      = ch;
  info.mp3.version = ver;
  ctrl.size        = sizeof ctrl;
  ctrl.pInfo       = &info;
  ctrl.maxEsSize   = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
  ctrl.maxPcmSize  = MP3_MAX_PCM;
  ctrl.wordLength  = SCE_AUDIODEC_WORD_LENGTH_16BITS;

  int r = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_MP3);
  if (r < 0) {
    log_printf("[snd] CreateDecoder failed 0x%08X (ch=%u ver=%u rate=%u)",
               (unsigned)r, ch, ver, rate);
    return 0;
  }

  /* Size the buffer from the stream's OWN bitrate, not from the assumption that
   * every frame is the 96-byte Layer III minimum. The slack in that assumption is
   * not small: KOTOR's voice lines are 48 kbps mono, so their frames are 216
   * bytes and the old bound asked for 2.25x what the clip needs. log148 shows
   * what that costs -- a 13.2 s line wanted 1881 KB when the heap could serve
   * 1024 KB, so it failed and the game played 13 seconds of silence over the
   * cutscene. The 828 KB it actually needed would have fit.
   *
   * bytes*8/kbps == ms is the same CBR identity the silence path above already
   * relies on, and log148 confirms it exactly: 79488 bytes at 48 kbps gives the
   * 13248 ms the game itself reported. Slack is 12.5% plus a whole frame, to
   * absorb mild VBR; the hard bound still caps it, and the decode loop below
   * stops at `cap` rather than running past it. */
  unsigned est_frames = (len - (unsigned)off) / 96 + 8;   /* 96B = min L3 frame */
  unsigned cap = est_frames * MP3_MAX_SAMPLES * ch;
  {
    unsigned kbps = (ver == 3) ? br_mpeg1[bri] : br_mpeg2[bri];
    if (kbps) {
      uint64_t ms = (uint64_t)(len - (unsigned)off) * 8u / kbps;
      uint64_t ns = ms * rate / 1000u;
      uint64_t tight = ns * ch + (ns * ch) / 8u + MP3_MAX_SAMPLES * ch;
      if (tight < cap) cap = (unsigned)tight;
    }
  }
  int16_t *pcm = (int16_t *)big_malloc((size_t)cap * sizeof(int16_t));
  int16_t  frame[MP3_MAX_SAMPLES * 2];
  if (!pcm) {
    /* Was silent, and that hurt: two 15 MB music decodes filled the heap, every
     * later stream failed here with no log, and the game retried forever. */
    log_printf("[snd] OUT OF MEMORY: %u KB for %u ch @ %u Hz (es %u KB)",
               (unsigned)(cap * sizeof(int16_t) / 1024), ch, rate, len / 1024);
    sceAudiodecDeleteDecoder(&ctrl);
    return 0;
  }

  unsigned total = 0, pos = (unsigned)off, frames = 0, errs = 0;
  while (pos + 4 <= len && total + MP3_MAX_SAMPLES * ch <= cap) {
    ctrl.pEs         = (SceUInt8 *)(d + pos);
    ctrl.inputEsSize = 0;
    ctrl.pPcm        = frame;
    ctrl.outputPcmSize = 0;
    unsigned avail = len - pos;
    ctrl.maxEsSize = avail < SCE_AUDIODEC_MP3_MAX_ES_SIZE
                       ? avail : SCE_AUDIODEC_MP3_MAX_ES_SIZE;

    r = sceAudiodecDecode(&ctrl);
    if (r < 0 || ctrl.inputEsSize == 0) {
      /* Resync: step one byte and look for the next frame. A handful of bad
       * frames at a stream edge is normal; a wall of them means real trouble. */
      if (++errs > 64) break;
      pos++;
      continue;
    }
    errs = 0;                       /* consecutive, not lifetime */
    unsigned got = ctrl.outputPcmSize / sizeof(int16_t);
    if (got) {
      memcpy(pcm + total, frame, ctrl.outputPcmSize);
      total += got;
    }
    pos += ctrl.inputEsSize;
    frames++;
  }

  sceAudiodecDeleteDecoder(&ctrl);

  /* Stopping because the buffer filled rather than because the stream ended
   * means the bitrate-based estimate above was short -- a VBR file, most
   * likely. The clip is clipped, not corrupted, but it should not be silent
   * about it: this is the one way the tighter estimate can go wrong. */
  if (pos + 4 <= len && total + MP3_MAX_SAMPLES * ch > cap)
    log_printf("[snd] decode hit the buffer estimate: %u KB held %u ms, %u of %u "
               "bytes left undecoded", (unsigned)(cap * sizeof(int16_t) / 1024),
               (unsigned)((uint64_t)(total / ch) * 1000u / rate), len - pos, len);

  if (!total) {
    big_free(pcm);
    log_printf("[snd] decoded 0 samples (frames=%u errs=%u)", frames, errs);
    return 0;
  }

  out->pcm      = pcm;
  out->nsamples = total / ch;
  out->channels = ch;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)out->nsamples * 1000u / rate);
  return 1;
}

void audio_pcm_free(AudioPcm *p) {
  if (p && p->pcm) { big_free(p->pcm); p->pcm = NULL; }
}

/* ---- incremental decoding -------------------------------------------------
 * audio_mp3_decode above turns a whole asset into one PCM block. That is right
 * for the short clips the game fires constantly, and the cache above it makes
 * repeats free. It is wrong for music: a track is ~15 MB decoded against a heap
 * shared with the game, which is why oversized streams used to be replaced with
 * silence and the score was never audible.
 *
 * Streaming keeps the decoder open and pulls frames on demand instead. The
 * elementary stream must stay alive and unmoved for the lifetime of the handle;
 * the caller owns it. */
#define SW_LOOKAHEAD (16u * 1024u)

struct AudioMp3Stream {
  SceAudiodecCtrl ctrl;
  SceAudiodecInfo info;
  const unsigned char *d;
  unsigned len, start, pos;
  /* Bytes of d present so far: a stream can be opened while the rest of its
   * file is still being read in (audio_patch.c). Written by the loading
   * thread after the bytes, read here before them. */
  volatile unsigned loaded;
  unsigned ch, rate;
  int      created, eos, errs;
  /* No hardware decoder (all taken, or none would start): minimp3 on this
   * thread instead, with its own state. */
  mp3dec_t *sw;
  /* One decode call emits a whole granule pair, usually more than the caller
   * asked for; the remainder waits here rather than being decoded twice. */
  int16_t  carry[MP3_MAX_SAMPLES * 2];
  unsigned carry_n, carry_off;             /* in int16 units */

  /* RIFF mode: the ambient beds are IMA ADPCM, not MP3 (see the file header).
   * They need no hardware decoder at all -- `created` stays 0 and the
   * sceAudiodec pool is left for the MP3 tracks and the VO. */
  int      is_wav;
  WavInfo  wav;
  unsigned blk;                            /* next block index */
  int16_t *wbuf;                           /* one block of decoded frames */
  unsigned wbuf_n, wbuf_off;               /* in int16 units */
};

/* Would this asset need one of the AUDIO_MP3_DECODER_POOL hardware handles?
 * The caller checks its stream cap against the pool, and a RIFF stream must not
 * count against it -- Lower City runs an ADPCM bed and an MP3 track at once. */
int audio_mp3_stream_needs_hw(const void *data, unsigned len) {
  WavInfo w;
  if (!data || len < 16) return 0;
  return !wav_header((const unsigned char *)data, len, &w);
}

AudioMp3Stream *audio_mp3_stream_open(const void *data, unsigned len, AudioPcm *fmt) {
  return audio_mp3_stream_open_partial(data, len, len, 1, fmt);
}

int audio_mp3_stream_uses_hw(const AudioMp3Stream *s) { return s && s->created; }


void audio_mp3_stream_set_loaded(AudioMp3Stream *s, unsigned loaded, int final) {
  if (!s) return;
  __sync_synchronize();                    /* the bytes before the count */
  if (final) s->len = loaded;              /* the file ended early */
  s->loaded = loaded < s->len ? loaded : s->len;
}

/* Bytes the decoder may read up to: everything once loaded reaches len. */
static unsigned stream_limit(const AudioMp3Stream *s) {
  unsigned lim = s->loaded;
  __sync_synchronize();                    /* the count before the bytes */
  return lim;
}

AudioMp3Stream *audio_mp3_stream_open_partial(const void *data, unsigned len, unsigned loaded, int use_hw,
                                              AudioPcm *fmt) {
  if (!data || !len) return NULL;
  if (loaded > len) loaded = len;

  /* RIFF first, and without touching the decoder library: the beds are ADPCM
   * and the whole point of streaming them is that they cost a block buffer
   * rather than 8-25 MB of PCM. */
  {
    WavInfo w;
    if (wav_header((const unsigned char *)data, len, &w)) {
      AudioMp3Stream *s = (AudioMp3Stream *)calloc(1, sizeof *s);
      if (!s) return NULL;
      unsigned per = (w.afmt == 17) ? w.per_block : 1024;   /* PCM: arbitrary chunk */
      s->wbuf = (int16_t *)malloc((size_t)per * w.ch * sizeof(int16_t));
      if (!s->wbuf) { free(s); return NULL; }
      s->is_wav = 1;
      s->wav    = w;
      s->d      = (const unsigned char *)data;
      s->len    = len;
      s->loaded = loaded;
      s->ch     = w.ch;
      s->rate   = w.rate;
      if (fmt) {
        fmt->pcm      = NULL;
        fmt->nsamples = w.frames;
        fmt->channels = w.ch;
        fmt->rate     = w.rate;
        fmt->ms       = (unsigned)((uint64_t)w.frames * 1000u / w.rate);
      }
      log_printf("[snd] stream is RIFF fmt=0x%x %u Hz %u ch -> %u frames (%u ms), "
                 "no hw decoder", w.afmt, w.rate, w.ch, w.frames,
                 (unsigned)((uint64_t)w.frames * 1000u / w.rate));
      return s;
    }
  }

  if (use_hw && !audio_mp3_init_library()) use_hw = 0;   /* idempotent; sizes the pool */

  /* Reuse the header walk so duration/rate/channels match what the non-stream
   * path would have reported. */
  AudioPcm probe;
  if (!audio_mp3_probe(data, len, &probe)) return NULL;

  const unsigned char *d = (const unsigned char *)data;
  int off = find_sync(d, len);
  if (off < 0) return NULL;

  unsigned ver  = (d[off + 1] >> 3) & 3;
  unsigned sri  = (d[off + 2] >> 2) & 3;
  unsigned cmod = (d[off + 3] >> 6) & 3;
  unsigned ch   = (cmod == 3) ? 1 : 2;
  unsigned rate = rate_of(ver, sri);
  if (!rate) return NULL;

  AudioMp3Stream *s = (AudioMp3Stream *)calloc(1, sizeof *s);
  if (!s) return NULL;

  s->d = d; s->len = len; s->loaded = loaded; s->start = (unsigned)off; s->pos = (unsigned)off;
  s->ch = ch; s->rate = rate;

  s->info.mp3.size    = sizeof(SceAudiodecInfoMp3);
  s->info.mp3.ch      = ch;
  s->info.mp3.version = ver;
  s->ctrl.size        = sizeof s->ctrl;
  s->ctrl.pInfo       = &s->info;          /* must point at storage we keep */
  s->ctrl.maxEsSize   = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
  s->ctrl.maxPcmSize  = MP3_MAX_PCM;
  s->ctrl.wordLength  = SCE_AUDIODEC_WORD_LENGTH_16BITS;

  int r = use_hw ? sceAudiodecCreateDecoder(&s->ctrl, SCE_AUDIODEC_TYPE_MP3) : -1;
  if (r >= 0) {
    s->created = 1;
  } else {
    if (use_hw)
      log_printf("[music] stream CreateDecoder failed 0x%08X (ch=%u ver=%u rate=%u) -- software decoder",
                 (unsigned)r, ch, ver, rate);
    s->sw = (mp3dec_t *)malloc(sizeof *s->sw);
    if (!s->sw) { free(s); return NULL; }
    mp3dec_init(s->sw);
  }

  if (fmt) {
    fmt->pcm      = NULL;                  /* streaming: no flat buffer */
    fmt->nsamples = probe.nsamples;
    fmt->channels = ch;
    fmt->rate     = rate;
    fmt->ms       = probe.ms;
  }
  return s;
}

/* Is the next block (or the end of the data) within the bytes loaded? */
static int wav_block_ready(const AudioMp3Stream *s) {
  unsigned lim = stream_limit(s);
  if (lim >= s->len) return 1;
  const WavInfo *w = &s->wav;
  unsigned bsz = (w->afmt == 17) ? w->balign : (1024 * w->ch * (w->bits / 8));
  unsigned end = w->data_off + (s->blk + 1) * bsz;
  if (end > w->data_off + w->data_len) end = w->data_off + w->data_len;
  return end <= lim;
}

/* One block (ADPCM) or one chunk (plain PCM) into s->wbuf. 0 = nothing left. */
static unsigned wav_fill_block(AudioMp3Stream *s) {
  const WavInfo *w = &s->wav;
  unsigned per = (w->afmt == 17) ? w->per_block : 1024;
  unsigned bsz = (w->afmt == 17) ? w->balign : (1024 * w->ch * (w->bits / 8));
  unsigned off = s->blk * bsz;
  if (off >= w->data_len) return 0;
  unsigned blen = w->data_len - off;
  if (blen > bsz) blen = bsz;
  const unsigned char *p = s->d + w->data_off + off;
  s->blk++;

  if (w->afmt == 17) return adpcm_block(p, blen, w->ch, s->wbuf, per);

  unsigned n = blen / (w->ch * (w->bits / 8));
  if (w->bits == 16) memcpy(s->wbuf, p, (size_t)n * w->ch * sizeof(int16_t));
  else for (unsigned i = 0; i < n * w->ch; i++)
         s->wbuf[i] = (int16_t)(((int)p[i] - 128) << 8);
  return n;
}

unsigned audio_mp3_stream_read(AudioMp3Stream *s, int16_t *dst, unsigned frames) {
  if (!s || !dst || !frames) return 0;

  if (s->is_wav) {
    unsigned want = frames * s->ch, got = 0;
    while (got < want) {
      if (s->wbuf_off < s->wbuf_n) {
        unsigned take = s->wbuf_n - s->wbuf_off;
        if (take > want - got) take = want - got;
        memcpy(dst + got, s->wbuf + s->wbuf_off, take * sizeof(int16_t));
        s->wbuf_off += take;
        got += take;
        continue;
      }
      if (s->eos) break;
      if (!wav_block_ready(s)) break;      /* still being read in: wait, not EOS */
      unsigned n = wav_fill_block(s);
      if (!n) { s->eos = 1; break; }
      s->wbuf_n   = n * s->ch;
      s->wbuf_off = 0;
    }
    return got / s->ch;
  }

  if (!s->created && !s->sw) return 0;
  unsigned want = frames * s->ch;          /* int16 units */
  unsigned got = 0;

  if (s->sw) {
    while (got < want) {
      if (s->carry_off < s->carry_n) {
        unsigned take = s->carry_n - s->carry_off;
        if (take > want - got) take = want - got;
        memcpy(dst + got, s->carry + s->carry_off, take * sizeof(int16_t));
        s->carry_off += take;
        got += take;
        continue;
      }
      if (s->eos) break;
      /* Until it has locked on, minimp3 confirms a sync over several frames,
       * so while the file is still being read in it decodes only with
       * SW_LOOKAHEAD bytes ahead; at the loaded edge it waits, not EOS. */
      unsigned lim = stream_limit(s), end = lim < s->len ? lim : s->len;
      unsigned avail = s->pos < end ? end - s->pos : 0;
      if (lim < s->len && avail < SW_LOOKAHEAD) break;
      if (avail < 4) { s->eos = 1; break; }
      mp3dec_frame_info_t fi;
      int n = mp3dec_decode_frame(s->sw, s->d + s->pos, (int)avail, s->carry, &fi);
      if (fi.frame_bytes <= 0) {               /* nothing more it can find */
        if (lim < s->len) break;
        s->eos = 1;
        break;
      }
      s->pos += (unsigned)fi.frame_bytes;
      if (n <= 0) continue;                    /* skipped junk, or a frame without PCM yet */
      if ((unsigned)fi.channels == s->ch) {
        s->carry_n = (unsigned)n * s->ch;
      } else if (fi.channels == 1 && s->ch == 2) {   /* the header disagreed: match it */
        for (int i = n - 1; i >= 0; i--) s->carry[2 * i] = s->carry[2 * i + 1] = s->carry[i];
        s->carry_n = (unsigned)n * 2;
      } else {
        for (int i = 0; i < n; i++) s->carry[i] = (int16_t)((s->carry[2 * i] + s->carry[2 * i + 1]) / 2);
        s->carry_n = (unsigned)n;
      }
      s->carry_off = 0;
    }
    return got / s->ch;
  }

  while (got < want) {
    if (s->carry_off < s->carry_n) {       /* drain leftovers first */
      unsigned take = s->carry_n - s->carry_off;
      if (take > want - got) take = want - got;
      memcpy(dst + got, s->carry + s->carry_off, take * sizeof(int16_t));
      s->carry_off += take;
      got += take;
      continue;
    }
    if (s->eos) break;

    /* While the file is still being read in, decode only with a whole
     * frame's worth present: a cut frame would look like a bad one, and the
     * resync below would step over real data. */
    unsigned lim = stream_limit(s);
    if (lim < s->len && (s->pos >= lim || lim - s->pos < SCE_AUDIODEC_MP3_MAX_ES_SIZE)) break;
    s->ctrl.pEs           = (SceUInt8 *)(s->d + s->pos);
    s->ctrl.inputEsSize   = 0;
    s->ctrl.pPcm          = s->carry;
    s->ctrl.outputPcmSize = 0;
    unsigned avail = (s->pos < s->len) ? s->len - s->pos : 0;
    if (avail < 4) { s->eos = 1; break; }
    s->ctrl.maxEsSize = avail < SCE_AUDIODEC_MP3_MAX_ES_SIZE
                          ? avail : SCE_AUDIODEC_MP3_MAX_ES_SIZE;

    int r = sceAudiodecDecode(&s->ctrl);
    if (r < 0 || s->ctrl.inputEsSize == 0) {
      /* Same resync rule as the whole-asset path: a few bad frames at an edge
       * are normal, a wall of them means the stream is finished or broken.
       * CONSECUTIVE is the point -- this used to be a lifetime count that was
       * never cleared on a good frame, so a long track could accumulate 65
       * scattered resync bytes over several minutes and then declare EOS in the
       * middle of itself. A 1149-byte ID3v2 tag between the fake RIFF and the
       * first frame would have done it on its own. */
      if (++s->errs > 64) { s->eos = 1; break; }
      s->pos++;
      continue;
    }
    s->errs      = 0;      /* consecutive, not lifetime -- see below */
    s->carry_n   = s->ctrl.outputPcmSize / sizeof(int16_t);
    s->carry_off = 0;
    s->pos      += s->ctrl.inputEsSize;
    if (!s->carry_n && s->pos >= s->len) { s->eos = 1; break; }
  }
  return got / s->ch;
}

int audio_mp3_stream_eos(const AudioMp3Stream *s) {
  if (!s) return 1;
  if (s->is_wav) return s->eos && s->wbuf_off >= s->wbuf_n;
  return s->eos && s->carry_off >= s->carry_n;
}

void audio_mp3_stream_rewind(AudioMp3Stream *s) {
  if (!s) return;
  /* ADPCM blocks are self-contained, so rewinding is just "block 0 again" --
   * no predictor state to reset and no reservoir to refill. */
  s->blk = 0;
  s->wbuf_n = s->wbuf_off = 0;
  s->pos = s->start;
  s->carry_n = s->carry_off = 0;
  s->eos  = 0;
  s->errs = 0;
  if (s->sw) mp3dec_init(s->sw);
}

void audio_mp3_stream_close(AudioMp3Stream *s) {
  if (!s) return;
  if (s->created) sceAudiodecDeleteDecoder(&s->ctrl);
  free(s->sw);
  free(s->wbuf);
  free(s);
}
