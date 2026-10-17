/* cpu_offload.h -- text widths cached, animated textures rebuilt on core 1
 * (cpu_offload.c). */
#ifndef CPU_OFFLOAD_H
#define CPU_OFFLOAD_H

/* Takes FTFont::Advance, DrawTextWrapped, CAurTexture::glImage and the
 * water/ring/random texture controllers (offload_mode.txt). */
void cpu_offload_install(void);

/* At the swap, game thread: the A/B and an [offload] report every 10 s. */
void cpu_offload_on_swap(void);

/* -1 not installed, 1 on, 0 off (the A/B's other half). */
int cpu_offload_phase(void);

#endif
