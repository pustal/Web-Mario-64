/**
 * @file SdlHooks.h
 * @brief The game's SDL calls the port sees first (linked with --wrap, see
 *        cmake/GhostshipWebOS.cmake): SDL_PollEvent, SDL_PeepEvents,
 *        SDL_GL_SwapWindow, SDL_OpenAudioDevice, SDL_CreateWindow and the
 *        message boxes.
 *
 *  - Events (Ghostship takes them with SDL_PeepEvents, the setup screen and
 *    the menu with SDL_PollEvent): remote buttons become the keyboard keys the game knows
 *    (RemoteInput.h); the menu keys open the port's menu instead of reaching
 *    the game; game controllers are opened as SDL game controllers so their
 *    Guide, Back and Start buttons can be told apart.
 *  - Buffer swap: the menu and the start-up hint are drawn (MenuOverlay.h).
 *  - Audio device: remembered, so the menu can pause it.
 *  - Window: fullscreen on the TV, whatever the game's settings say.
 *  - Message boxes (SDL_ShowMessageBox, SDL_ShowSimpleMessageBox): the
 *    game's error messages are written to the port's log; on the TV they are
 *    not shown (a desktop dialog is no use there, and a blocking one would
 *    hang the app): the game carries on as if Cancel was chosen.
 */
#ifndef WM64_SDL_HOOKS_H
#define WM64_SDL_HOOKS_H

#include <SDL.h>

namespace wm64
{
namespace SdlHooks
{

/** SDL's own SDL_PollEvent, past the hook (for the menu's own event loop). */
int PollRealEvent(SDL_Event *event);

/**
 * Opens or closes the game controller of a device added or removed event.
 * @return True if @p event was such an event.
 */
bool HandleDeviceEvent(const SDL_Event &event);

/**
 * The port's look at an event before the game gets it: device events are
 * noted, menu keys open (or close) a menu and are swallowed, remote buttons
 * are rewritten into the game's keys.
 * @return True if the (possibly rewritten) @p event goes on to the game.
 */
bool FilterForGame(SDL_Event *event);

} // namespace SdlHooks
} // namespace wm64

#endif // WM64_SDL_HOOKS_H
