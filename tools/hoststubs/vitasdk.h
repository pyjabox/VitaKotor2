/* vitasdk.h -- host stubs, enough of the SDK surface for audio_mp3.c to compile
 * off target (see tools/test_audio_codec.c). The MP3 path needs real hardware,
 * so its decoder always fails here; the RIFF/ADPCM path is pure C and is what
 * the test covers. NOT used by the Vita build, which finds the real header. */
#ifndef HOST_VITASDK_H
#define HOST_VITASDK_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t SceUInt8;
typedef struct { unsigned size, ch, version; } SceAudiodecInfoMp3;
typedef union  { SceAudiodecInfoMp3 mp3; } SceAudiodecInfo;
typedef struct { unsigned size; SceAudiodecInfo *pInfo; SceUInt8 *pEs; void *pPcm;
                 unsigned inputEsSize, outputPcmSize, maxEsSize, maxPcmSize,
                 wordLength; } SceAudiodecCtrl;
typedef struct { unsigned size, totalStreams; } SceAudiodecInitStreamParam;
typedef union  { SceAudiodecInitStreamParam mp3; } SceAudiodecInitParam;
#define SCE_AUDIODEC_TYPE_MP3 1
#define SCE_AUDIODEC_WORD_LENGTH_16BITS 16
#define SCE_AUDIODEC_MP3_MAX_ES_SIZE 1441
static inline int sceAudiodecInitLibrary(int t, SceAudiodecInitParam *p) { (void)t;(void)p; return 0; }
static inline int sceAudiodecCreateDecoder(SceAudiodecCtrl *c, int t) { (void)c;(void)t; return -1; }
static inline int sceAudiodecDeleteDecoder(SceAudiodecCtrl *c) { (void)c; return 0; }
static inline int sceAudiodecDecode(SceAudiodecCtrl *c) { (void)c; return -1; }
#endif
