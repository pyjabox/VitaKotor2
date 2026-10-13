/* bootui.h -- what the loader draws before the game does: the boot menu and
 * the loading screen. Textured quads in screen pixels, top-down, sent to the
 * GL worker; fixed function, no shaders. Game thread only. */
#ifndef BOOTUI_H
#define BOOTUI_H

#include <stdint.h>

#define BOOTUI_SLOTS 32
#define BOOTUI_QUADS 64

typedef struct {
  int slot;
  float x, y, w, h;          /* screen pixels */
  float s0, t0, s1, t1;      /* texture coordinates */
  float r, g, b, a;          /* tint */
  int blend;                 /* alpha-blended over what is below */
} BootQuad;

/* A texture for `slot`, from a malloc'd RGBA image this takes and frees. */
void bootui_upload(int slot, uint8_t *rgba, int w, int h);
/* A 1x1 white texture, for flat colour quads. */
void bootui_solid(int slot);
/* UTF-8 text (ls_text_image) into `slot`; 1 and its size if anything drew. */
int bootui_text(int slot, const char *utf8, int px, int max_w, int line_h, int *w, int *h);

/* One frame: clear to `bg`, draw `q` in order, present. Quads whose slot has
 * no texture are skipped. */
void bootui_frame(const BootQuad *q, int n, const float bg[3]);

/* Every texture, after the last frame that uses them. */
void bootui_free(void);

#endif
