/* bootmenu.h -- language and mods, chosen before the game starts
 * (see bootmenu.c). */
#ifndef BOOTMENU_H
#define BOOTMENU_H

/* Process start: watch for L, held at any moment until the menu would open. */
void bootmenu_watch_begin(void);

/* Main thread, after libkotor2 is loaded: the main menu's Google Play button
 * becomes MODS, which restarts the game into this menu. */
void bootmenu_install_button(void);

/* Game thread, vitaGL up, before the loading screen and SDL_main. Opens on a
 * first run (no language chosen yet), when L was held, or after MODS; saves
 * what was chosen. */
void bootmenu_run(void);

#endif
