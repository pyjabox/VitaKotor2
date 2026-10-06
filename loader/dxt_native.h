/* dxt_native.h -- the game's DXT textures uploaded compressed (dxt_native.c). */

#ifndef __DXT_NATIVE_H__
#define __DXT_NATIVE_H__

/* After the engine is loaded and relocated, before the game thread starts. */
void dxt_native_install(void);
/* Each DRAW_FRAME window: upload counts since the last report. */
void dxt_native_report(void);

#endif
