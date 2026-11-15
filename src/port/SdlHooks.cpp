/**
 * @file SdlHooks.cpp
 * @brief Implementation of the SDL call hooks (see SdlHooks.h).
 *
 * The linker's --wrap=<symbol> sends the game's calls to __wrap_<symbol>;
 * __real_<symbol> is SDL's own function.
 */
#include "SdlHooks.h"

#include "Log.h"
#include "MenuOverlay.h"
#include "RemoteInput.h"

#include <map>

extern "C" {
int __real_SDL_PollEvent(SDL_Event *event);
void __real_SDL_GL_SwapWindow(SDL_Window *window);
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                             SDL_AudioSpec *obtained, int allowed_changes);
int __real_SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType);
SDL_Window *__real_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
int __real_SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid);
int __real_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);
}

namespace wm64
{
namespace SdlHooks
{
namespace
{

/** Game controllers we opened, by joystick instance id. */
std::map<SDL_JoystickID, SDL_GameController *> &Controllers()
{
    static std::map<SDL_JoystickID, SDL_GameController *> controllers;
    return controllers;
}

/** Whether a controller's Back or Start is held, for the Back + Start menu shortcut. */
struct ComboState {
    bool back  = false;
    bool start = false;
};

std::map<SDL_JoystickID, ComboState> &Combos()
{
    static std::map<SDL_JoystickID, ComboState> combos;
    return combos;
}

/**
 * Tracks Back and Start on each controller.
 * @return True when this press completes Back + Start (the menu shortcut).
 */
bool CompletesMenuCombo(const SDL_Event &event)
{
    if (event.type != SDL_CONTROLLERBUTTONDOWN && event.type != SDL_CONTROLLERBUTTONUP)
        return false;
    const bool down    = event.type == SDL_CONTROLLERBUTTONDOWN;
    ComboState &state  = Combos()[event.cbutton.which];
    const bool wasBoth = state.back && state.start;
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
        state.back = down;
    else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START)
        state.start = down;
    return down && !wasBoth && state.back && state.start;
}

} // namespace

int PollRealEvent(SDL_Event *event) { return __real_SDL_PollEvent(event); }

bool HandleDeviceEvent(const SDL_Event &event)
{
    if (event.type == SDL_JOYDEVICEADDED) {
        const int index = event.jdevice.which;
        if (SDL_IsGameController(index)) {
            if (SDL_GameController *controller = SDL_GameControllerOpen(index)) {
                const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
                if (Controllers().count(id) == 0)
                    Controllers()[id] = controller;
                else
                    SDL_GameControllerClose(controller); // already ours: drop the extra reference
            }
        }
        return true;
    }
    if (event.type == SDL_JOYDEVICEREMOVED) {
        const auto found = Controllers().find(event.jdevice.which);
        if (found != Controllers().end()) {
            SDL_GameControllerClose(found->second);
            Controllers().erase(found);
        }
        Combos().erase(event.jdevice.which);
        return true;
    }
    return false;
}

bool FilterForGame(SDL_Event *event)
{
    // The game still sees device events: it opens the joysticks itself.
    HandleDeviceEvent(*event);

    if (RemoteInput::IsMenuOpenRequest(*event) || CompletesMenuCombo(*event)) {
        // Back closes Ghostship's own menu when it is shown; otherwise it opens the port's.
        const bool gameUiOpen = RemoteInput::IsGameUiOpen();
        Log::Printf("menu key: %s", gameUiOpen ? "closing the game's menu" : "opening the menu");
        if (gameUiOpen)
            MenuOverlay::ToggleGameMenu();
        else
            MenuOverlay::RequestOpen();
        return false;
    }
    // The rest of a menu key's presses (repeats, the release) never reach the game either.
    if (RemoteInput::IsMenuKey(*event))
        return false;

    RemoteInput::RewriteForGame(event);
    return true;
}

} // namespace SdlHooks
} // namespace wm64

extern "C" {

int __wrap_SDL_PollEvent(SDL_Event *event)
{
    using namespace wm64;
    if (!event)
        return __real_SDL_PollEvent(nullptr); // only asks whether an event is waiting
    while (__real_SDL_PollEvent(event)) {
        if (SdlHooks::FilterForGame(event))
            return 1;
    }
    return 0;
}

int __wrap_SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType)
{
    using namespace wm64;
    // Ghostship (libultraship's SDL backend) takes its events with SDL_GETEVENT:
    // those pass the port's filter one by one. Peeking and adding are left alone.
    if (action != SDL_GETEVENT || !events || numevents <= 0)
        return __real_SDL_PeepEvents(events, numevents, action, minType, maxType);
    int delivered = 0;
    while (delivered < numevents) {
        SDL_Event *target = &events[delivered];
        const int got     = __real_SDL_PeepEvents(target, 1, SDL_GETEVENT, minType, maxType);
        if (got < 0)
            return delivered > 0 ? delivered : got;
        if (got == 0)
            break;
        if (SdlHooks::FilterForGame(target))
            ++delivered;
    }
    return delivered;
}

void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
    // Start-up progress for the log: the game has drawn its first frame.
    static bool firstFrame = true;
    if (firstFrame) {
        firstFrame = false;
        wm64::Log::Printf("the game drew its first frame");
    }
    wm64::MenuOverlay::OnSwap(window, &__real_SDL_GL_SwapWindow);
    __real_SDL_GL_SwapWindow(window);
}

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                             SDL_AudioSpec *obtained, int allowed_changes)
{
    const SDL_AudioDeviceID id = __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
    wm64::Log::Printf("audio device: %s", id != 0 ? "open" : SDL_GetError());
    if (id != 0 && !iscapture)
        wm64::MenuOverlay::SetAudioDevice(id);
    return id;
}

SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
#if defined(WM64_TARGET_WEBOS)
    // A TV has one screen and no window frame: always the whole panel.
    flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    SDL_Window *window = __real_SDL_CreateWindow(title, x, y, w, h, flags);
    wm64::Log::Printf("window \"%s\" %dx%d (flags 0x%x): %s", title ? title : "", w, h, static_cast<unsigned>(flags),
                     window ? "created" : SDL_GetError());
    return window;
}

int __wrap_SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
    // The game reports errors in message boxes: keep them in the port's log too.
    wm64::Log::Printf("message box \"%s\": %s", data && data->title ? data->title : "", data && data->message ? data->message : "");
#if defined(WM64_TARGET_WEBOS)
    // No dialogs on the TV: the game takes the failure as "Cancel", as on any platform without message boxes.
    (void)buttonid;
    return SDL_SetError("No message boxes on webOS");
#else
    return __real_SDL_ShowMessageBox(data, buttonid);
#endif
}

int __wrap_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
    wm64::Log::Printf("message box \"%s\": %s", title ? title : "", message ? message : "");
#if defined(WM64_TARGET_WEBOS)
    (void)flags;
    (void)window;
    return SDL_SetError("No message boxes on webOS");
#else
    return __real_SDL_ShowSimpleMessageBox(flags, title, message, window);
#endif
}

} // extern "C"
