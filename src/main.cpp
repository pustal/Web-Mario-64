/**
 * @file main.cpp
 * @brief Entry point of Web Mario 64 (Super Mario 64 for webOS, built on Ghostship).
 *
 * Before the game starts:
 *  1. a child launched to make the game's archive runs that and nothing else
 *     (Extractor.h: this executable started again with --wm64-extract),
 *  2. the data folder is chosen and Ghostship is pointed at it (SHIP_HOME),
 *  3. a save backup received during the last session is restored,
 *  4. the transfer page starts (it keeps running while the game plays),
 *  5. root access is probed, the USB drives' mods are linked in, and so is
 *     the game's archive when it is kept on a drive (GameFiles.h),
 *  6. without the archive (or when it must be made again), the setup screen
 *     waits for the ROM (uploaded, or found on a USB drive, searched on a
 *     thread) and makes the archive from it, on the TV or the ROM's drive,
 * then Ghostship's own main() runs (renamed ghostship_main by
 * cmake/GhostshipWebOS.cmake). When the game quits after a restart request
 * (the menu or the transfer page), the app starts itself again.
 */
#include "port/Extractor.h"
#include "port/GameFiles.h"
#include "port/Log.h"
#include "port/MenuOverlay.h"
#include "port/NetInfo.h"
#include "port/RemoteInput.h"
#include "port/SetupScreen.h"
#include "port/Storage.h"
#include "port/TransferServer.h"
#include "port/UsbAccess.h"

// Ghostship's accessors for libultraship's parts (src/port/ShipCompat.h) and the GUI classes.
#include "port/ShipCompat.h"
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <imgui.h>

#include <SDL.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

/** Ghostship's main() (src/port/Game.cpp). */
int ghostship_main(int argc, char **argv);

namespace
{

using namespace wm64;

/** Gathers what the menu shows (called on the game's thread). */
OverlayInfo GatherOverlayInfo(const Storage &storage, UsbAccess &usb, GameFiles &gameFiles, const TransferServer &server)
{
    OverlayInfo info;
    const std::string address = FindLanAddress();
    if (server.Port() != 0 && !address.empty())
        info.address = "http://" + address + ":" + std::to_string(server.Port());
    info.pin = server.Pin();

    const GameFilesState files = gameFiles.LastState(); // the menu must not wait on the drives
    info.gameSource = files.archive.present ? "ready (" + HumanBytes(files.archive.bytes) + " archive on " + files.archive.Describe() + ")"
                                            : "missing";
    if (!files.rom.present)
        info.gameSource += ", ROM not kept";
    else if (files.rom.drive != files.archive.drive)
        info.gameSource += ", ROM on " + files.rom.Describe();
    const std::string importedFrom = usb.RomImportedFrom();
    if (!importedFrom.empty())
        info.gameSource += ", ROM copied from " + importedFrom;

    const std::size_t tvMods = Storage::ListMods(storage.ModsDir()).size();
    const std::size_t drives = usb.LinkedModDrives().size();
    info.modsSource          = std::to_string(tvMods) + " on the TV";
    if (drives > 0)
        info.modsSource += ", " + std::to_string(drives) + (drives == 1 ? " USB drive" : " USB drives") + " linked";

    const RootStatus root = usb.Root();
    info.root = root.runningAsRoot ? "yes (the app runs as root)" : root.homebrewChannel ? "yes (Homebrew Channel)" : "no";
    info.restartPending = server.Snapshot().restartPending;
    return info;
}

/** libultraship's setting for moving through its menu with a controller (cmake/cvars.cmake). */
#ifdef CVAR_IMGUI_CONTROLLER_NAV
constexpr const char *kControllerNavCVar = CVAR_IMGUI_CONTROLLER_NAV;
#else
constexpr const char *kControllerNavCVar = "gControlNav";
#endif

/** libultraship's menu window (Ghostship's settings), or null before the game set it up. */
std::shared_ptr<Ship::GuiWindow> GameMenu()
{
    const std::shared_ptr<Ship::Window> window = ShipCompat::GetWindow();
    if (!window || !window->GetGui())
        return nullptr;
    return window->GetGui()->GetMenu();
}

/**
 * Shows or hides Ghostship's settings menu, made usable without a mouse: the
 * remote's arrows and OK move through it (keyboard navigation), and so does a
 * controller (the "Menu Controller Navigation" setting, on by default here as
 * on a console; it can still be turned off in the menu). libultraship sets its
 * navigation flags only when it opens the menu itself, so they are set here too.
 * Runs on the game's thread (event filter or the port's menu).
 */
void ToggleGameMenu()
{
    const std::shared_ptr<Ship::GuiWindow> menu = GameMenu();
    if (!menu)
        return;
    CVarRegisterInteger(kControllerNavCVar, 1);
    menu->ToggleVisibility();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (menu->IsVisible() && CVarGetInteger(kControllerNavCVar, 0) != 0)
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
}

} // namespace

int main(int argc, char **argv)
{
    // 1. The archive-making child (it exits from inside).
    int childExit = 0;
    if (RunExtractionChild(argc, argv, &childExit))
        return childExit;

    RemoteInput::ApplyPlatformHints();

    // 2. The data folder; Ghostship keeps its files (archive, saves, mods, settings) in SHIP_HOME.
    Storage storage = Storage::Resolve();
    storage.CreateFolders();
    setenv("SHIP_HOME", storage.ShipDir().c_str(), 1);

    // The last launch's log is kept (the transfer page offers it). The crash
    // handler's marker says whether that launch crashed; it is reported once.
    const bool crashedLastTime = Storage::FileSize(storage.CrashMarkerPath()) > 0;
    std::error_code renameError;
    std::filesystem::remove(storage.CrashMarkerPath(), renameError);
    std::filesystem::rename(storage.PortLogPath(false), storage.PortLogPath(true), renameError);
    Log::Open(storage.PortLogPath(false));
    Log::SetCrashMarkerPath(storage.CrashMarkerPath());
    Log::InstallCrashHandler();
    Log::Printf("Web Mario 64 %s; data folder: %s%s", WM64_VERSION, storage.Root().c_str(), storage.Persistent() ? "" : " (temporary)");
    if (crashedLastTime)
        Log::Printf("the last launch crashed");

    // 3. Saves restored from the transfer page while the game was running.
    storage.ApplyPendingBackup();

    // 4. The transfer page, first, so it can be reached (and its logs read) even
    //    if something below takes long.
    UsbAccess usb;
    GameFiles gameFiles(storage, usb);
    TransferServer server(storage, usb, gameFiles, LoadOrCreatePin(storage));
    if (server.Start())
        Log::Printf("transfer page on port %u, PIN %s", static_cast<unsigned>(server.Port()), server.Pin().c_str());
    else
        Log::Printf("the transfer page could not start");
    server.SetCrashedLastTime(crashedLastTime);
    MenuOverlay::SetInfoProvider([&storage, &usb, &gameFiles, &server] { return GatherOverlayInfo(storage, usb, gameFiles, server); });
    MenuOverlay::SetGameMenuOpener(&ToggleGameMenu);
    RemoteInput::SetGameUiQuery([] {
        const std::shared_ptr<Ship::GuiWindow> menu = GameMenu();
        return menu && menu->IsVisible();
    });

    // 5. Root access and the USB drives.
    usb.ProbeRoot();
    usb.PlaceContent(storage);
    gameFiles.Place();
    Log::Printf("USB drives checked");

    // 6. The game's archive: made from the ROM when it is missing or must be made again.
    const bool rebuild = std::filesystem::exists(storage.RebuildMarkerPath()) && gameFiles.HasRom();
    if (rebuild || !storage.HasArchive()) {
        if (rebuild)
            Log::Printf("the archive is made again from the ROM");
        if (!gameFiles.HasRom())
            usb.StartRomSearch(storage);
        server.SetSetupMode(true);
        const SetupResult setup = RunSetupScreen(storage, usb, gameFiles, server.Port() != 0 ? &server : nullptr, SetupReason::PrepareGame);
        if (setup != SetupResult::Ready) {
            server.Stop();
            return setup == SetupResult::Exit ? 0 : 1;
        }
    }
    // The game crashed last time: hold on a screen (OK, or a countdown, starts the game), so its logs can be fetched first.
    else if (crashedLastTime) {
        server.SetSetupMode(true);
        const SetupResult setup = RunSetupScreen(storage, usb, gameFiles, server.Port() != 0 ? &server : nullptr, SetupReason::AfterCrash);
        if (setup == SetupResult::Exit) {
            server.Stop();
            return 0;
        }
    }
    server.SetSetupMode(false);

    // Ghostship finds ghostship.o2r, config.yml and assets/ next to its executable, and
    // its data in SHIP_HOME. webOS passes the launch parameters as an argument; the game
    // does not get them.
    std::vector<char *> gameArgs = { argv[0], nullptr };
    if (chdir(storage.ShipDir().c_str()) != 0)
        Log::Printf("could not enter %s", storage.ShipDir().c_str());
    Log::Printf("starting the game");
    const int result = ghostship_main(1, gameArgs.data());
    Log::Printf("the game ended (%d)", result);

    server.Stop();
    if (MenuOverlay::RestartRequested()) {
        Log::Printf("restarting");
        std::fflush(stdout);
        execv("/proc/self/exe", argv);
        Log::Printf("could not restart");
    }
    return result;
}
