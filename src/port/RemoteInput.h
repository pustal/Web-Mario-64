/**
 * @file RemoteInput.h
 * @brief Maps LG remote control buttons onto the keyboard keys Ghostship
 *        already understands, and recognises the keys that open the menu.
 *
 * libultraship's default keyboard layout for the N64 controller is:
 *   W A S D = stick, X = A, C = B, Z = Z, E = L, R = R, Space = Start,
 *   arrow keys = C buttons, T F G H = D-pad.
 * Rather than teaching the game about the remote, each remote button is
 * rewritten into one of those keys before the game sees the event. Game
 * controllers are untouched: the game reads them itself.
 *
 * Layout (the remote held normally):
 *   arrows = stick (full tilt: Mario runs), OK or Green = A, Red = B,
 *   Yellow = Z, Blue = R, 2 4 6 8 = C up/left/right/down (camera),
 *   0 or Play = Start.
 * While Ghostship's own menu is shown, the remote's keys reach it unchanged
 * (arrows move, OK chooses).
 * Back (and Exit) opens the port's menu (MenuOverlay.h), or closes
 * Ghostship's; on a controller, the Guide button or Back + Start together do.
 */
#ifndef WM64_REMOTE_INPUT_H
#define WM64_REMOTE_INPUT_H

#include <SDL.h>

#include <functional>

namespace wm64
{

/** What a menu should do in response to an input event. */
enum class MenuAction {
    None,
    Up,
    Down,
    Confirm,
    Cancel,
};

namespace RemoteInput
{

/**
 * If @p event is a key event from a mapped remote button, rewrites its
 * scancode and keycode to the keyboard key the game expects.
 *
 * @return True if the event was rewritten.
 */
bool RewriteForGame(SDL_Event *event);

/** True for a press or release of a key that opens the menu (Back, Exit, or Escape on a keyboard). */
bool IsMenuKey(const SDL_Event &event);

/** True when @p event is a press that should open the menu: a menu key, or the controller's Guide button. */
bool IsMenuOpenRequest(const SDL_Event &event);

/** Translates a key or controller press into a menu action (remote, keyboard or pad). */
MenuAction ToMenuAction(const SDL_Event &event);

/** Sets how to tell whether Ghostship's own menu is shown (main.cpp asks libultraship). */
void SetGameUiQuery(std::function<bool()> query);

/** Whether Ghostship's own menu is shown. */
bool IsGameUiOpen();

/** Applies the SDL hints that let a native webOS app receive Back/Exit and hide the pointer. */
void ApplyPlatformHints();

} // namespace RemoteInput
} // namespace wm64

#endif // WM64_REMOTE_INPUT_H
