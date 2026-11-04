/* plt_direct.h -- the engine's calls to its own functions, made direct (plt_direct.c). */
#ifndef PLT_DIRECT_H
#define PLT_DIRECT_H

/* Right after libkotor2 is loaded, before any hook touches its code. */
void plt_direct_scan(void);
/* After the last hook is installed, before SDL_main (game thread). */
void plt_direct_install(void);
void plt_direct_on_swap(void);
int plt_direct_active(void);

#endif
