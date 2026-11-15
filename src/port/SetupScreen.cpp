/**
 * @file SetupScreen.cpp
 * @brief Implementation of the setup screen (see SetupScreen.h).
 */
#include "SetupScreen.h"

#include "BitmapFont.h"
#include "Extractor.h"
#include "Log.h"
#include "NetInfo.h"
#include "RemoteInput.h"
#include "SdlHooks.h"
#include "Storage.h"
#include "TransferServer.h"
#include "UsbAccess.h"

#include <SDL.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wm64
{
namespace
{

/** Logical canvas: divides 1080p and 2160p panels by whole numbers so text stays crisp. */
constexpr int kCanvasWidth  = 480;
constexpr int kCanvasHeight = 270;
constexpr int kLine         = 12;
constexpr std::uint32_t kInfoRefreshMs = 3000;
constexpr std::uint32_t kStartDelayMs  = 1200;
/** After a crash, the screen starts the game on its own after this long. */
constexpr std::uint32_t kAfterCrashWaitMs = 30000;
/** The USB drives are checked this often for one plugged in meanwhile. */
constexpr std::uint32_t kUsbRefreshMs = 3000;
constexpr std::uint32_t kFrameDelayMs  = 33;

/** webOS remote keys (SDL-webOS backport scancodes). */
constexpr int kScancodeWebOSBack = 482;
constexpr int kScancodeWebOSExit = 505;

constexpr Color kBackground = { 12, 20, 60, 255 };
constexpr Color kTitle      = { 255, 222, 120, 255 };
constexpr Color kText       = { 240, 240, 230, 255 };
constexpr Color kDim        = { 165, 180, 215, 255 };
constexpr Color kAccent     = { 130, 210, 255, 255 };
constexpr Color kWarning    = { 255, 140, 100, 255 };
constexpr Color kBarBack    = { 35, 50, 110, 255 };
constexpr Color kBarFill    = { 255, 200, 60, 255 };

/** Everything the screen draws, refreshed periodically. */
struct ScreenState {
    std::string address;
    std::uint64_t freeBytes = 0;
    std::vector<std::string> usbLines;
    TransferStatus transfer;
    bool searching = false;        ///< A ROM search on the USB drives is running.
    ExtractionStatus extraction;   ///< The archive being made (or the last attempt).
    std::uint32_t autoStartAt = 0; ///< After a crash: when the game starts without OK (SDL ticks).
    bool starting = false;
};

/** One line per USB drive, e.g. "USB sda1: 12.3 GB free, ROM found, 2 mods". */
std::vector<std::string> DescribeDrives(UsbAccess &usb)
{
    std::vector<std::string> lines;
    for (const UsbDrive &drive : usb.ListDrives()) {
        std::string line = "USB " + drive.label + ": ";
        line += drive.spaceKnown ? HumanBytes(drive.freeBytes) + " free" : std::string("space unknown");
        if (!drive.romFile.empty())
            line += ", ROM found";
        if (!drive.mods.empty())
            line += ", " + std::to_string(drive.mods.size()) + (drive.mods.size() == 1 ? " mod" : " mods");
        lines.push_back(line);
    }
    if (lines.empty())
        lines.push_back("No USB drive found.");
    return lines;
}

/** True for the keys and buttons that leave the screen (and the app). */
bool IsExitRequest(const SDL_Event &event)
{
    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        const int scancode = static_cast<int>(event.key.keysym.scancode);
        return scancode == kScancodeWebOSBack || scancode == kScancodeWebOSExit || scancode == SDL_SCANCODE_AC_BACK
               || scancode == SDL_SCANCODE_ESCAPE;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        const int button = event.cbutton.button;
        return button == SDL_CONTROLLER_BUTTON_B || button == SDL_CONTROLLER_BUTTON_BACK || button == SDL_CONTROLLER_BUTTON_GUIDE;
    }
    return event.type == SDL_QUIT || event.type == SDL_APP_TERMINATING;
}

void DrawScreen(SDL_Renderer *renderer, const TextRenderer &text, const ScreenState &state, const UsbAccess &usb,
                const TransferServer *server, SetupReason reason)
{
    SDL_SetRenderDrawColor(renderer, kBackground.r, kBackground.g, kBackground.b, kBackground.a);
    SDL_RenderClear(renderer);

    const int center = kCanvasWidth / 2;
    int y            = 16;
    text.DrawTextCentered(center, y, "Web Mario 64", 2, kTitle);
    y += 26;

    if (state.starting) {
        text.DrawTextCentered(center, kCanvasHeight / 2,
                              reason == SetupReason::AfterCrash ? "Starting..." : "The game is ready. Starting...", 1, kText);
        SDL_RenderPresent(renderer);
        return;
    }

    if (state.extraction.running) {
        // Making the archive: a minute or two on a TV.
        y = kCanvasHeight / 2 - 30;
        text.DrawTextCentered(center, y, "Preparing the game from your ROM...", 1, kText);
        y += kLine;
        text.DrawTextCentered(center, y, "This happens once and takes a minute or two.", 1, kDim);
        y += kLine * 2;
        constexpr int kBarWidth  = 300;
        constexpr int kBarHeight = 8;
        const int barX           = center - kBarWidth / 2;
        const std::size_t total  = state.extraction.total;
        const int filled = total > 0 ? static_cast<int>(std::min<std::size_t>(state.extraction.done, total) * kBarWidth / total) : 0;
        text.FillRect(barX, y, kBarWidth, kBarHeight, kBarBack);
        text.FillRect(barX, y, filled, kBarHeight, kBarFill);
        y += kBarHeight + 6;
        text.DrawTextCentered(center, y, total > 0 ? std::to_string(state.extraction.done * 100 / total) + " %" : std::string("Reading the ROM..."), 1, kText);
        SDL_RenderPresent(renderer);
        return;
    }

    if (reason == SetupReason::AfterCrash) {
        text.DrawTextCentered(center, y, "The game crashed last time.", 1, kWarning);
        y += kLine;
        text.DrawTextCentered(center, y, "Its logs are on the transfer page, under Trouble?:", 1, kText);
        y += kLine * 2;
        if (server && server->Port() != 0 && !state.address.empty()) {
            text.DrawTextCentered(center, y, "http://" + state.address + ":" + std::to_string(server->Port()), 2, kAccent);
            y += 22;
            text.DrawTextCentered(center, y, "PIN  " + server->Pin(), 2, kTitle);
            y += 30;
        }
        const std::uint32_t now  = SDL_GetTicks();
        const std::uint32_t left = SDL_TICKS_PASSED(now, state.autoStartAt) ? 0 : (state.autoStartAt - now + 999) / 1000;
        text.DrawTextCentered(center, y, "Starting in " + std::to_string(left) + " s.  OK: start now   BACK: exit", 1, kText);
        SDL_RenderPresent(renderer);
        return;
    }

    if (state.extraction.finished && !state.extraction.succeeded) {
        text.DrawTextCentered(center, y, "The game could not be prepared from this ROM:", 1, kWarning);
        y += kLine;
        text.DrawTextCentered(center, y, state.extraction.error, 1, kWarning);
        y += kLine;
        text.DrawTextCentered(center, y, "Upload another copy, or press OK to try again.", 1, kText);
        y += kLine * 2;
    }
    else {
        text.DrawTextCentered(center, y, "A Super Mario 64 ROM is needed to play.", 1, kText);
        y += kLine;
        text.DrawTextCentered(center, y, "Use your own copy: USA or Japan, .z64, .n64 or .v64.", 1, kDim);
        y += kLine * 2;
    }

    if (!server || server->Port() == 0) {
        text.DrawTextCentered(center, y, "The transfer page could not start.", 1, kWarning);
        y += kLine;
        text.DrawTextCentered(center, y, "Restart the app to try again.", 1, kDim);
        y += kLine * 2;
    }
    else if (state.address.empty()) {
        text.DrawTextCentered(center, y, "Connect the TV to your network to continue.", 1, kWarning);
        y += kLine * 3;
    }
    else {
        text.DrawTextCentered(center, y, "On a phone or computer on this network, open:", 1, kText);
        y += kLine + 4;
        text.DrawTextCentered(center, y, "http://" + state.address + ":" + std::to_string(server->Port()), 2, kAccent);
        y += 22;
        text.DrawTextCentered(center, y, "PIN  " + server->Pin(), 2, kTitle);
        y += 26;
    }

    text.DrawTextCentered(center, y, "Or plug in a USB drive with it in a \"Super Mario 64\" folder", 1, kDim);
    y += kLine;
    text.DrawTextCentered(center, y, "or in RetroArch's roms/n64 (with \"mario\" in the file name).", 1, kDim);
    if (state.searching) {
        y += kLine;
        text.DrawTextCentered(center, y, "Looking for the ROM on the USB drives...", 1, kAccent);
    }
    y += kLine + 4;
    text.DrawTextCentered(center, y, "Free space on the TV: " + HumanBytes(state.freeBytes), 1, kDim);
    y += kLine;
    for (const std::string &line : state.usbLines) {
        text.DrawTextCentered(center, y, line, 1, kDim);
        y += kLine;
    }
    const RootStatus root = usb.Root();
    text.DrawTextCentered(center, y, root.Available() ? "Root access: yes" : "Root access: no", 1, kDim);
    y += kLine + 4;

    if (state.transfer.busy && state.transfer.totalBytes > 0) {
        constexpr int kBarWidth  = 300;
        constexpr int kBarHeight = 8;
        const int barX           = center - kBarWidth / 2;
        const int filled = static_cast<int>(state.transfer.doneBytes * kBarWidth / state.transfer.totalBytes);
        text.FillRect(barX, y, kBarWidth, kBarHeight, kBarBack);
        text.FillRect(barX, y, filled, kBarHeight, kBarFill);
        y += kBarHeight + 4;
        text.DrawTextCentered(center, y, state.transfer.busyWhat + ": " + HumanBytes(state.transfer.doneBytes) + " of " + HumanBytes(state.transfer.totalBytes), 1, kText);
    }
    else if (!state.transfer.lastEvent.empty()) {
        text.DrawTextCentered(center, y, state.transfer.lastEvent, 1, kText);
    }

    text.DrawTextCentered(center, kCanvasHeight - 18, "Press BACK to exit", 1, kDim);
    SDL_RenderPresent(renderer);
}

} // namespace

SetupResult RunSetupScreen(const Storage &storage, UsbAccess &usb, TransferServer *server, SetupReason reason)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
        return SetupResult::Failed;

    // The whole panel on the TV (SdlHooks.cpp adds fullscreen there); a 1280x720 window on the desktop test version.
    SDL_Window *window = SDL_CreateWindow("Web Mario 64", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer) {
        if (window)
            SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
        return SetupResult::Failed;
    }
    SDL_RenderSetLogicalSize(renderer, kCanvasWidth, kCanvasHeight);
    SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i))
            SDL_GameControllerOpen(i);
    }

    TextRenderer text;
    text.Attach(renderer);

    ScreenState state;
    std::uint32_t nextInfoRefresh = 0;
    std::uint32_t startAt         = 0;
    std::uint32_t nextUsbCheck    = 0;
    std::string knownDrives       = usb.MountSignature(); // main.cpp has looked at these already
    SetupResult result            = SetupResult::Exit;
    bool running                  = true;
    state.autoStartAt             = SDL_GetTicks() + kAfterCrashWaitMs;
    Extraction extraction;
    // The ROM the last failed attempt used (its size and time): the same file is not tried again on its own.
    std::string failedRom;
    const auto romKey = [&storage] {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(storage.RomPath(), ec);
        return ec ? std::string() : std::to_string(time.time_since_epoch().count());
    };

    while (running) {
        SDL_Event event;
        while (SdlHooks::PollRealEvent(&event)) { // past the game's hook: Back exits here
            if (event.type == SDL_CONTROLLERDEVICEADDED)
                SDL_GameControllerOpen(event.cdevice.which);
            else if (IsExitRequest(event))
                running = false;
            else if (reason == SetupReason::AfterCrash && !state.starting && RemoteInput::ToMenuAction(event) == MenuAction::Confirm) {
                state.starting = true;
                startAt        = SDL_GetTicks() + kStartDelayMs;
            }
            else if (reason == SetupReason::PrepareGame && state.extraction.finished && !state.extraction.succeeded
                     && RemoteInput::ToMenuAction(event) == MenuAction::Confirm) {
                failedRom.clear(); // try the same ROM again
            }
        }

        const std::uint32_t now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now, nextInfoRefresh)) {
            state.address   = FindLanAddress();
            state.freeBytes = storage.FreeBytes();
            state.usbLines  = DescribeDrives(usb);
            nextInfoRefresh = now + kInfoRefreshMs;
        }
        if (server)
            state.transfer = server->Snapshot();
        state.searching = usb.RomSearchRunning();

        // A drive plugged in meanwhile: look for the ROM on it (an upload places its own).
        // Only when the drives change, as searching RetroArch folders can take a while.
        if (!state.starting && !state.extraction.running && SDL_TICKS_PASSED(now, nextUsbCheck)) {
            const std::string drives = usb.MountSignature();
            if (drives != knownDrives) {
                knownDrives = drives;
                usb.PlaceContent(storage);
                if (!storage.HasRom())
                    usb.StartRomSearch(storage);
            }
            nextUsbCheck = now + kUsbRefreshMs;
        }

        if (reason == SetupReason::PrepareGame && !state.starting) {
            // A ROM is there (and not the one that just failed): make the archive from it.
            if (!state.extraction.running && storage.HasRom() && romKey() != failedRom && !usb.RomSearchRunning()) {
                std::error_code ec;
                std::filesystem::remove(storage.RebuildMarkerPath(), ec);
                extraction.Start(storage);
                failedRom.clear();
            }
            state.extraction = extraction.Poll();
            if (server)
                server->SetExtraction(state.extraction);
            if (state.extraction.finished && !state.extraction.running) {
                if (state.extraction.succeeded) {
                    state.starting = true;
                    startAt        = now + kStartDelayMs;
                }
                else if (failedRom.empty()) {
                    failedRom = romKey();
                }
            }
        }
        const bool autoStart = reason == SetupReason::AfterCrash && SDL_TICKS_PASSED(now, state.autoStartAt);
        if (!state.starting && autoStart) {
            state.starting = true;
            startAt        = now + kStartDelayMs;
        }
        if (state.starting && SDL_TICKS_PASSED(now, startAt)) {
            result  = SetupResult::Ready;
            running = false;
        }

        DrawScreen(renderer, text, state, usb, server, reason);
        SDL_Delay(kFrameDelayMs);
    }

    text.Detach();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    return result;
}

} // namespace wm64
