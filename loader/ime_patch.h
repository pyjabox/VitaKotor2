/* ime_patch.h -- Vita on-screen keyboard for the game's text fields
 *
 * The chargen name box, the save-game name box and every other CSWGuiEditbox
 * are typed into through the platform's virtual keyboard. On Android that is
 * the system IME; on Vita it is sceImeDialog, wired up in ime_patch.c.
 */

#ifndef __IME_PATCH_H__
#define __IME_PATCH_H__

#include "so_util.h"

// Resolver entries retained for companion-library variants that import the
// ASLPlat functions.
const so_default_dynlib *ime_get_dynlib(void);
extern const int ime_dynlib_size;

// KOTOR II defines the ASLPlat functions internally, so resolver entries cannot
// override them. Patch those definitions directly after libkotor2.so is loaded.
void ime_install_hooks(so_module *kotor_mod);

// Open the same Vita IME from a direct GUI-panel hook. KOTOR II's save-name
// panel focuses its edit box but, when a joystick is connected, does not call
// ASLPlat_ShowVirtualKeyboard itself.
void ime_show_keyboard(const char *current, unsigned int max_len);

// Queue printable ASCII through the same SDL_TEXTINPUT path used by Android.
// Emulator fallback for desktop builds that accept sceImeDialogInit
// but cannot render an on-screen keyboard.
void ime_queue_text(const char *text);

// Non-zero while the IME dialog owns the screen. The swap hook uses this to
// composite the common dialog into the frame.
int ime_dialog_active(void);

// Poll the dialog and, once the user confirms, queue what they typed as
// SDL_TEXTINPUT events. Call once per frame from the swap hook (game thread).
void ime_pump(void);

#endif
