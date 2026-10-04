/* ime_patch.c -- Vita on-screen keyboard for the game's text fields
 * Historical diagnostic narrative is inherited from VitaKotor (KOTOR I)
 * unless explicitly marked as KOTOR II validation.
 *
 * Why the name box could not be typed into
 * ----------------------------------------
 * Every editable field in the Aurora GUI is a CSWGuiEditbox. Tapping one runs
 * CSWGuiEditbox::HandleLMouseDown, which takes focus (CExoInput::KeyboardModeOn)
 * and then asks the platform for a keyboard:
 *
 *     ASLPlat_ShowVirtualKeyboard(const char *current, unsigned maxLen)
 *
 * In libandroid_port that function is a single instruction -- a tail call to
 * SDL_StartTextInput(), arguments discarded (+0x81a58). On Android that pops the
 * system IME, and each character comes back as an SDL_TEXTINPUT event, which
 * SDL_main forwards one byte at a time:
 *
 *     cmp r0, #0x303              ; SDL_TEXTINPUT
 *     ldrsb.w r0, [sp, #148]      ; event.text.text[0]
 *     blx HandleWMCharMessage     ; -> CSWGuiManager::HandleKeyPress -> editbox
 *
 * Vita SDL2 has no on-screen keyboard: SDL_StartTextInput is inert and no
 * SDL_TEXTINPUT event is ever produced. So the field could never receive a
 * character. Nothing hung -- log143 shows the game rendering happily at 40 fps
 * on the chargen name screen -- there was simply no way to enter a name, and no
 * way past a screen that will not accept an empty one.
 *
 * The fix
 * -------
 * Override both ASLPlat_* imports (our resolver table wins over the
 * cross-module link, see so_resolve) and drive sceImeDialog instead. When the
 * dialog closes we replay the result into the SAME path Android uses: one
 * SDL_TEXTINPUT event per character, pushed into SDL's queue for the game's own
 * event loop to pick up. That keeps every field working -- chargen name, save
 * names, anything else -- rather than special-casing one screen.
 *
 * What the editbox accepts (CSWGuiEditbox::HandleKeyPress, +0x4aa050):
 *   * 8 (backspace) and 127 (delete) erase one character
 *   * 32..126 insert, EXCEPT '/', '\' and '_' which it drops
 *   * 13/10 fire the "accepted" event to the parent panel -- we do NOT send
 *     these; the player still presses the panel's own button, as on Android
 * Anything below 32 is discarded, so only ASCII is worth queueing.
 */

#include <vitasdk.h>
#include <SDL2/SDL.h>
#include <string.h>

#include "ime_patch.h"
#include "input_patch.h"
#include "log.h"

// The dialog is seeded with the field's current text and we replay the result
// in full, so the field must first be cleared -- one backspace per character we
// seeded. 64 is far above any field the game offers (the chargen name asks for
// 18) and bounds both the seed and the replay queue.
#define IME_TEXT_MAX 64

typedef enum { IME_IDLE = 0, IME_OPEN, IME_DRAIN } ime_state;

static ime_state s_state = IME_IDLE;
static int s_drain_frames = 0;
static int  s_configured = 0;        // sceCommonDialogSetConfigParam done once
static int  s_seed_len   = 0;        // characters we pre-filled -> backspaces
static SceWChar16 s_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static SceWChar16 s_initial[IME_TEXT_MAX + 1];
static SceWChar16 s_result[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

// KOTOR II's SDL loop normally forwards SDL_TEXTINPUT to this function. Resolve
// it directly so we can observe whether the GUI actually accepts each character,
// rather than merely proving that SDL dequeued the event.
static int (*s_handle_wm_char)(unsigned short ch) = NULL;
static unsigned s_direct_char_n = 0;

int ime_dialog_active(void) { return s_state != IME_IDLE; }

static void utf16_from_ascii(SceWChar16 *dst, const char *src, int cap) {
  int i = 0;
  if (src)
    for (; src[i] && i < cap; i++)
      dst[i] = (SceWChar16)(unsigned char)src[i];
  dst[i] = 0;
}

// One SDL_TEXTINPUT event carrying a single character. SDL_main reads only
// text[0] and sign-extends it, so anything >= 0x80 would arrive as 0xFFxx --
// callers must filter to ASCII before calling this.
static void push_char(char c) {
  if (s_handle_wm_char) {
    int accepted = s_handle_wm_char((unsigned short)(unsigned char)c);
    if (s_direct_char_n < 128)
      log_printf("[ime] HandleWMChar ch=0x%02x ('%c') -> %d [#%u]",
                 (unsigned char)c, c >= 32 ? c : '.', accepted, ++s_direct_char_n);
    return;
  }

  SDL_Event e;
  memset(&e, 0, sizeof(e));
  e.text.type = SDL_TEXTINPUT;
  e.text.timestamp = SDL_GetTicks();
  e.text.windowID = 0;
  e.text.text[0] = c;
  int accepted = SDL_PushEvent(&e);
  if (s_direct_char_n < 128)
    log_printf("[ime] SDL_PushEvent ch=0x%02x ('%c') -> %d [#%u]",
               (unsigned char)c, c >= 32 ? c : '.', accepted, ++s_direct_char_n);
}

void ime_queue_text(const char *text) {
  int n = 0;
  if (text) {
    for (; text[n] && n < IME_TEXT_MAX; n++) {
      unsigned char c = (unsigned char)text[n];
      if (c >= 32 && c < 127)
        push_char((char)c);
    }
  }
  log_printf("[ime] fallback queued \"%s\" (%d chars)", text ? text : "", n);
}

/* Common dialogs need their config set once before the first one opens, or
 * sceImeDialogInit fails. Done lazily on first use so this costs nothing on the
 * boot path. Language and enter-button come from the console's own settings so
 * the dialog matches the rest of the system; if AppUtil is unavailable we fall
 * back to English / circle rather than leaving the init-time sentinels in
 * place, which sceImeDialogInit rejects. */
static void ime_configure(void) {
  if (s_configured)
    return;
  s_configured = 1;

  int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
  int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
  SceAppUtilInitParam init;
  SceAppUtilBootParam boot;
  memset(&init, 0, sizeof(init));
  memset(&boot, 0, sizeof(boot));
  sceAppUtilInit(&init, &boot);          // harmless if SDL already did it
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);

  SceCommonDialogConfigParam cfg;
  sceCommonDialogConfigParamInit(&cfg);
  cfg.language = (SceSystemParamLang)lang;
  cfg.enterButtonAssign = (SceSystemParamEnterButtonAssign)enter;
  int r = sceCommonDialogSetConfigParam(&cfg);
  log_printf("[ime] common dialog config: lang=%d enter=%d -> 0x%08x",
             lang, enter, (unsigned)r);
}

// ---- the two entry points libKOTOR imports from libandroid_port ----

void ime_show_keyboard(const char *current, unsigned int max_len) {
  if (s_state != IME_IDLE) {           // a second tap while the dialog is up
    log_printf("[ime] ShowVirtualKeyboard ignored -- dialog already open");
    return;
  }
  ime_configure();

  // maxLen comes straight from the control. The chargen name box asks for 18;
  // CSWGuiEditbox passes its own signed-short limit, which can be 0 or negative
  // for "no limit". Keep it inside what we can clear again with backspaces.
  int cap = (int)max_len;
  if (cap <= 0 || cap > IME_TEXT_MAX)
    cap = IME_TEXT_MAX;

  utf16_from_ascii(s_initial, current, cap);
  s_seed_len = 0;
  while (s_initial[s_seed_len])
    s_seed_len++;

  utf16_from_ascii(s_title, "Enter name", SCE_IME_DIALOG_MAX_TITLE_LENGTH - 1);
  memset(s_result, 0, sizeof(s_result));

  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  p.supportedLanguages = 0;                                 // whatever is installed
  p.languagesForced = SCE_FALSE;
  p.type = SCE_IME_TYPE_DEFAULT;
  p.option = 0;
  p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;    // let the player back out
  p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  p.title = s_title;
  p.maxTextLength = (SceUInt32)cap;
  p.initialText = s_initial;
  p.inputTextBuffer = s_result;

  int r = sceImeDialogInit(&p);
  if (r < 0) {
    log_printf("[ime] sceImeDialogInit FAILED 0x%08x (cap=%d, seed=%d chars)",
               (unsigned)r, cap, s_seed_len);
    return;
  }
  s_state = IME_OPEN;
  log_printf("[ime] dialog open: cap=%d seed=\"%s\"", cap, current ? current : "");
}

static int ASLPlat_IsVirtualKeyboardVisible_hook(void) {
  return ime_dialog_active();
}

void ime_install_hooks(so_module *kotor_mod) {
  uintptr_t show = so_symbol(kotor_mod, "_Z27ASLPlat_ShowVirtualKeyboardPKcj");
  uintptr_t visible = so_symbol(kotor_mod, "_Z32ASLPlat_IsVirtualKeyboardVisiblev");
  s_handle_wm_char = (int (*)(unsigned short))so_symbol(
      kotor_mod, "_Z19HandleWMCharMessaget");
  log_printf("[ime] KOTOR II direct character dispatcher=%p", (void *)s_handle_wm_char);
  if (!show || !visible) {
    log_printf("[ime] KOTOR II virtual-keyboard symbols missing: show=%p visible=%p",
               (void *)show, (void *)visible);
    return;
  }
  hook_addr(show, (uintptr_t)&ime_show_keyboard);
  hook_addr(visible, (uintptr_t)&ASLPlat_IsVirtualKeyboardVisible_hook);
  log_printf("[ime] direct KOTOR II virtual-keyboard hooks installed");
}

// ---- per-frame poll ----

void ime_pump(void) {
  // KOTOR II checks IsVirtualKeyboardVisible while dispatching queued text. If
  // visibility drops in the same swap that queues SDL_TEXTINPUT, it disables
  // keyboard mode/focus before those events are handled. Keep visibility true
  // for two subsequent game frames, then release it.
  if (s_state == IME_DRAIN) {
    if (--s_drain_frames <= 0) {
      s_state = IME_IDLE;
      log_printf("[ime] queued text drain complete; keyboard visibility released");
    }
    return;
  }
  if (s_state != IME_OPEN)
    return;

  SceCommonDialogStatus st = sceImeDialogGetStatus();
  if (st != SCE_COMMON_DIALOG_STATUS_FINISHED)
    return;

  SceImeDialogResult res;
  memset(&res, 0, sizeof(res));
  sceImeDialogGetResult(&res);
  sceImeDialogTerm();

  if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) {
    s_state = IME_IDLE;
    log_printf("[ime] dialog cancelled (button=%d)", (int)res.button);
    return;
  }

  // Clear what we seeded, then replay the confirmed text. The editbox drops
  // '/', '\' and '_' itself, so we do not second-guess it -- we only filter to
  // the printable ASCII range it can represent at all.
  for (int i = 0; i < s_seed_len; i++)
    push_char('\b');

  char out[IME_TEXT_MAX + 1];
  int n = 0, dropped = 0;
  for (int i = 0; s_result[i] && n < IME_TEXT_MAX; i++) {
    SceWChar16 w = s_result[i];
    if (w >= 32 && w < 127) {
      out[n++] = (char)w;
      push_char((char)w);
    } else {
      dropped++;
    }
  }
  out[n] = 0;
  s_state = IME_DRAIN;
  s_drain_frames = 2;
  log_printf("[ime] accepted \"%s\" (%d chars queued, %d backspaces, %d non-ascii dropped); keeping focus for %d frames",
             out, n, s_seed_len, dropped, s_drain_frames);
}

// ---- resolver table ----

static const so_default_dynlib ime_dynlib[] = {
  { "_Z27ASLPlat_ShowVirtualKeyboardPKcj", (uintptr_t)&ime_show_keyboard },
  { "_Z32ASLPlat_IsVirtualKeyboardVisiblev", (uintptr_t)&ASLPlat_IsVirtualKeyboardVisible_hook },
};
const int ime_dynlib_size = sizeof(ime_dynlib);
const so_default_dynlib *ime_get_dynlib(void) { return ime_dynlib; }
