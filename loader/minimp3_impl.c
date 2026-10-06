/* minimp3_impl.c -- the one translation unit that compiles minimp3
 * (loader/minimp3.h: github.com/lieff/minimp3 at ea99364, CC0). Its NEON
 * paths are on: the toolchain defines __ARM_NEON. Used by audio_mp3.c for
 * streams that get no hardware decoder. */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
