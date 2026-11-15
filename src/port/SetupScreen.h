/**
 * @file SetupScreen.h
 * @brief The screen shown before the game: waiting for the ROM, making the
 *        game's archive from it, or holding after a crash.
 *
 * Without the game's archive (sm64.o2r) it shows the address to open on a
 * phone or computer, the PIN, the free space, USB and root state and the
 * upload progress. As soon as a ROM is in place (uploaded, or found on a USB
 * drive), it makes the archive from it (Extractor.h), with a progress bar,
 * and returns when the archive is ready. If that fails, it says why and
 * waits for another ROM (or OK to try again).
 *
 * When the game ended unexpectedly at the last launch, the same screen
 * explains that the logs of that launch are on the transfer page, and starts
 * the game when the player presses OK, or after 30 seconds, so the page
 * stays reachable meanwhile.
 */
#ifndef WM64_SETUP_SCREEN_H
#define WM64_SETUP_SCREEN_H

namespace wm64
{

class GameFiles;
class Storage;
class TransferServer;
class UsbAccess;

/** How the setup screen ended. */
enum class SetupResult {
    Ready,  ///< The archive is there; start the game.
    Exit,   ///< The player closed the app.
    Failed, ///< The screen could not be created.
};

/** Why the setup screen is shown. */
enum class SetupReason {
    PrepareGame, ///< No archive yet (or it must be made again): wait for a ROM and make it.
    AfterCrash,  ///< The app crashed last time: offer its logs, start on OK or after a countdown.
};

/**
 * Runs the setup screen until the game can start or the player exits.
 * Creates and destroys its own window; SDL video is shut down on return so
 * the game can initialise it from scratch.
 *
 * @param gameFiles Where the ROM is and where the archive is made (TV or USB drive).
 * @param server May be null if the transfer server failed to start.
 */
SetupResult RunSetupScreen(const Storage &storage, UsbAccess &usb, GameFiles &gameFiles, TransferServer *server,
                           SetupReason reason);

} // namespace wm64

#endif // WM64_SETUP_SCREEN_H
