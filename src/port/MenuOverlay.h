/**
 * @file MenuOverlay.h
 * @brief The port's own menu and start-up hint, drawn over the game.
 *
 * The menu opens with the remote's Back (or Exit) key, a keyboard's Escape,
 * or a controller's Guide button or Back + Start. It shows how to reach the
 * transfer page (address and PIN), the game data and the mods, root and USB
 * state, the remote layout and the project page, and offers Resume, Game
 * settings (Ghostship's own menu), Restart and Exit. While it is open the game is paused: the
 * menu runs inside the game's buffer swap (SdlHooks.cpp) and the game's audio
 * device is paused, so the game resumes exactly where it was.
 *
 * For the first seconds after the game starts, a one-line hint shows the
 * transfer page address and how to open the menu.
 *
 * Everything is drawn with OpenGL ES 2 calls (valid in the game's ES 3
 * context) and the port's 8x8 font, saving and
 * restoring the GL state it touches, as the game's renderer keeps its own.
 */
#ifndef WM64_MENU_OVERLAY_H
#define WM64_MENU_OVERLAY_H

#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace wm64
{

/** What the menu shows, gathered by main.cpp from the storage, USB and transfer state. */
struct OverlayInfo {
    std::string address;              ///< "http://192.168.1.20:47821", empty when offline or the server is down.
    std::string pin;                  ///< The transfer page's PIN.
    std::string gameSource;           ///< e.g. "ready, from Super Mario 64 (USA)".
    std::string modsSource;           ///< e.g. "3 on the TV, USB drive sda1 linked".
    std::string root;                 ///< e.g. "yes (Homebrew Channel)".
    bool restartPending = false;      ///< Files arrived that the game only reads at start-up.
};

namespace MenuOverlay
{

/** Sets the function the menu calls (on the game's thread) when it opens, and every few seconds while open. */
void SetInfoProvider(std::function<OverlayInfo()> provider);

/** Asks for the menu to open at the next buffer swap (thread-safe). */
void RequestOpen();

/** Sets what "Game settings" does: show or hide Ghostship's own menu (called on the game's thread). */
void SetGameMenuOpener(std::function<void()> opener);

/** Shows or hides Ghostship's own menu (the function set above). Call on the game's thread. */
void ToggleGameMenu();

/**
 * Opens the menu with @p text shown at its top, for messages the player must
 * see while the game runs (the TV has no browser or dialog to show them in).
 * The text goes when the menu closes. Thread-safe.
 */
void ShowNotice(const std::string &text);

/** Whether the menu is open (the game is paused). */
bool IsOpen();

/** Records the game's audio device, paused while the menu is open. */
void SetAudioDevice(SDL_AudioDeviceID device);

/**
 * Called by the buffer swap hook before the real swap: draws the start-up
 * hint, and runs the menu (until it closes) if it was asked for.
 * @param swap The real SDL_GL_SwapWindow, which the menu uses for its own frames.
 */
void OnSwap(SDL_Window *window, void (*swap)(SDL_Window *));

/** Whether Restart was chosen (main.cpp then starts the app again once the game has quit). */
bool RestartRequested();

/** Requests a restart from elsewhere (the transfer page): the game quits and the app starts again. */
void RequestRestart();

} // namespace MenuOverlay
} // namespace wm64

#endif // WM64_MENU_OVERLAY_H
