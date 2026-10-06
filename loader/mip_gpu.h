/* mip_gpu.h -- mip chains of uncompressed images made by vitaGL (mip_gpu.c). */

#ifndef __MIP_GPU_H__
#define __MIP_GPU_H__

/* After the engine is loaded and relocated, before the game thread starts. */
void mip_gpu_install(void);
/* Each DRAW_FRAME window: image counts and timing since the last report. */
void mip_gpu_report(void);

#endif
