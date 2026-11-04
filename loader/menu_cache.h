/* menu_cache.h -- the action menus rebuilt when something changed (menu_cache.c). */
#ifndef MENU_CACHE_H
#define MENU_CACHE_H

/* Takes CSWGuiMainInterface::PopulateMenus's slots; after so_resolve. */
void menu_cache_install(void);
void menu_cache_on_swap(void);

#endif
